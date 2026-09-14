// SPDX-License-Identifier: Apache-2.0
// Trusted, non-setuid launcher. Invoked by root recovery, not by web content.
#include "jail_policy.hpp"
#include <libminijail.h>
#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <unistd.h>

static constexpr uid_t kBrowserUid = 99090;
static constexpr uid_t kTelegramUid = 99091;
static constexpr uid_t kMediaUid = 99092;
static constexpr uid_t kRecorderUid = 99093;
static constexpr gid_t kMediaRwGid = 1023;
static void Die(const char *message) { perror(message); _exit(78); }
static void Check(int result, const char *what) { if (result < 0) Die(what); }
static volatile sig_atomic_t stopping;
static void Stop(int) { stopping = 1; }
static bool WriteFile(const std::string &path, const char *value) {
  int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  const auto size = strlen(value);
  const bool ok = write(fd, value, size) == static_cast<ssize_t>(size);
  close(fd); return ok;
}
static void PrepareRetroStorage() {
  int aera = open("/sdcard/AERA", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (aera < 0) Die("AERA storage directory");
  if (mkdirat(aera, "RetroArch", 0770) && errno != EEXIST)
    Die("RetroArch storage directory");
  int retro = openat(aera, "RetroArch",
      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  close(aera);
  if (retro < 0) Die("safe RetroArch storage directory");
  Check(fchown(retro, 0, kMediaRwGid), "RetroArch storage ownership");
  Check(fchmod(retro, 0770), "RetroArch storage permissions");
  for (const char *name : {"Saves", "States", "System"}) {
    if (mkdirat(retro, name, 0770) && errno != EEXIST)
      Die("RetroArch data directory");
    int child = openat(retro, name,
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child < 0) Die("safe RetroArch data directory");
    Check(fchown(child, 0, kMediaRwGid), "RetroArch data ownership");
    Check(fchmod(child, 0770), "RetroArch data permissions");
    close(child);
  }
  close(retro);
}
static void PrepareRecorderStorage() {
  int aera = open("/sdcard/AERA", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (aera < 0) Die("AERA storage directory");
  if (mkdirat(aera, "Recordings", 0770) && errno != EEXIST)
    Die("AERA recordings directory");
  int recordings = openat(aera, "Recordings",
      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  close(aera);
  if (recordings < 0) Die("safe AERA recordings directory");
  Check(fchown(recordings, 0, kMediaRwGid), "recordings ownership");
  Check(fchmod(recordings, 0770), "recordings permissions");
  close(recordings);
}
static void PrepareBrowserStorage() {
  int aera = open("/sdcard/AERA", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (aera < 0) Die("AERA storage directory");
  if (mkdirat(aera, "Downloads", 0770) && errno != EEXIST)
    Die("AERA downloads directory");
  int downloads = openat(aera, "Downloads",
      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  close(aera);
  if (downloads < 0) Die("safe AERA downloads directory");
  // WebKit keeps zero supplementary groups. Temporarily make only this
  // directory writable by its dedicated UID; the trusted host restores
  // media_rw ownership after each transfer and when the session closes.
  Check(fchown(downloads, kBrowserUid, kBrowserUid), "downloads ownership");
  Check(fchmod(downloads, 0700), "downloads permissions");
  close(downloads);
}
static void WriteResolverConfig(int root) {
  const int etc = openat(root, "etc", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (etc < 0) Die("browser resolver directory");
  std::string contents;
  const char *keys[] = {"net.wlan0.dns1", "net.wlan0.dns2", "net.dns1", "net.dns2"};
  for (const char *key : keys) {
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get(key, value) <= 0) continue;
    in_addr address4{};
    in6_addr address6{};
    if (inet_pton(AF_INET, value, &address4) != 1 &&
        inet_pton(AF_INET6, value, &address6) != 1) continue;
    const std::string line = std::string("nameserver ") + value + "\n";
    if (contents.find(line) == std::string::npos) contents += line;
  }
  if (contents.empty()) contents = "nameserver 1.1.1.1\nnameserver 8.8.8.8\n";
  const int output = openat(etc, "resolv.conf",
                            O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
  close(etc);
  if (output < 0) Die("browser resolver file");
  size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t written = write(output, contents.data() + offset, contents.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) { close(output); Die("browser resolver write"); }
    offset += static_cast<size_t>(written);
  }
  close(output);
}
// Root supervisor has no web engine and accepts no browser commands. Its only
// job is to bound memory and reclaim this exact process tree, even if WebKit
// crashes or the native UI disappears. No PID/user namespace is required.
static void Supervise(const std::string &root, const char *memory_limit) {
  const std::string name = root.substr(5);
  const std::string tasks = "/dev/cg2_bpf/" + name;
  const std::string memory = "/dev/memcg/" + name;
  Check(mkdir(tasks.c_str(), 0700), "private browser task group");
  if (mkdir(memory.c_str(), 0700)) { rmdir(tasks.c_str()); Die("private browser memory group"); }
  int kill_file = open((tasks + "/cgroup.kill").c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
  int killer = kill_file < 0 ? -1 : fcntl(kill_file, F_DUPFD_CLOEXEC, 5);
  if (kill_file >= 0) close(kill_file);
  if (killer < 0 || !WriteFile(memory + "/memory.limit_in_bytes", memory_limit)) {
    if (killer >= 0) close(killer);
    rmdir(memory.c_str()); rmdir(tasks.c_str()); Die("browser resource limits");
  }
  struct stat swap{};
  if (!stat((memory + "/memory.memsw.limit_in_bytes").c_str(), &swap) &&
      !WriteFile(memory + "/memory.memsw.limit_in_bytes", memory_limit)) {
    close(killer); rmdir(memory.c_str()); rmdir(tasks.c_str()); Die("browser swap limit");
  }
  struct sigaction action{}; action.sa_handler = Stop; sigemptyset(&action.sa_mask);
  for (int signal : {SIGTERM, SIGINT, SIGHUP}) Check(sigaction(signal, &action, nullptr), "supervisor signals");
  const pid_t parent = getppid();
  Check(prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0), "supervisor lifetime");
  if (getppid() != parent) stopping = 1;
  const pid_t child = fork();
  if (child < 0) {
    close(killer); rmdir(memory.c_str()); rmdir(tasks.c_str()); Die("browser fork");
  }
  if (!child) {
    for (int signal : {SIGTERM, SIGINT, SIGHUP}) ::signal(signal, SIG_DFL);
    if (!WriteFile(tasks + "/cgroup.procs", "0") || !WriteFile(memory + "/cgroup.procs", "0"))
      Die("join browser resource groups");
    close(killer);
    return;
  }
  // The supervisor must not keep the UI connection alive after worker exit.
  close(3); close(4);
  int status = 0;
  while (!stopping) {
    const auto result = waitpid(child, &status, WNOHANG);
    if (result == child) break;
    if (result < 0 && errno != EINTR) { stopping = 1; break; }
    usleep(100000);
  }
  // Kernel cgroup.kill covers all descendants and concurrent forks. This
  // descriptor names only the transient group created above, never host tasks.
  if (write(killer, "1", 1) != 1) Die("stop browser process tree");
  close(killer);
  if (stopping) while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  bool removed_memory = false, removed_tasks = false;
  for (int attempt = 0; attempt < 50 && (!removed_memory || !removed_tasks); ++attempt) {
    if (!removed_memory) removed_memory = !rmdir(memory.c_str());
    if (!removed_tasks) removed_tasks = !rmdir(tasks.c_str());
    if (!removed_memory || !removed_tasks) usleep(100000);
  }
  if (!removed_memory || !removed_tasks) fprintf(stderr, "Browser resource group cleanup incomplete\n");
  _exit(!stopping && WIFEXITED(status) ? WEXITSTATUS(status) : 78);
}
int main(int argc, char **argv) {
  if (argc != 3 || (strcmp(argv[1], "--probe") &&
      strcmp(argv[1], "--browser") && strcmp(argv[1], "--retroarch") &&
      strcmp(argv[1], "--telegram") && strcmp(argv[1], "--media") &&
      strcmp(argv[1], "--recorder")) || getuid() || geteuid()) {
    fprintf(stderr, "Usage (root recovery only): aera-browser-jail --probe|--browser|--retroarch|--telegram|--media|--recorder RUNTIME\n");
    return 78;
  }
  const bool retroarch = !strcmp(argv[1], "--retroarch");
  const bool telegram = !strcmp(argv[1], "--telegram");
  const bool media = !strcmp(argv[1], "--media");
  const bool recorder = !strcmp(argv[1], "--recorder");
  const bool browser = !strcmp(argv[1], "--browser");
  const std::string root = argv[2];
  const size_t prefix = recorder ? 14 : media ? 16 :
      (retroarch || telegram) ? 13 : 14;
  const char *expected = retroarch ? "/tmp/aera-ra-" :
      telegram ? "/tmp/aera-tg-" : media ? "/tmp/aera-media-" :
      recorder ? "/tmp/aera-rec-" : "/tmp/aera-web-";
  if (retroarch) PrepareRetroStorage();
  if (recorder) PrepareRecorderStorage();
  if (browser) PrepareBrowserStorage();
  if (root.size() != prefix + 6 || root.compare(0, prefix, expected) ||
      !std::all_of(root.begin() + prefix, root.end(),
                   [](unsigned char c) { return std::isalnum(c); })) return 78;
  int directory = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  struct stat info{}; struct statfs filesystem{};
  if (directory < 0 || fstat(directory, &info) || info.st_uid || (info.st_mode & 0777) != 0700 ||
      fstatfs(directory, &filesystem) ||
      (filesystem.f_type != 0x01021994 && filesystem.f_type != 0x858458f6)) Die("private RAM runtime");
  for (const char *name : {"etc", "proc", "tmp", "dev", "run", "storage", "sdcard", "state", "recordings", "downloads"}) {
    if (mkdirat(directory, name, 0755) && errno != EEXIST) Die("runtime directory");
    struct stat child{};
    if (fstatat(directory, name, &child, AT_SYMLINK_NOFOLLOW) || !S_ISDIR(child.st_mode) || child.st_uid)
      Die("unsafe runtime directory");
  }
  if (retroarch) {
    const int sdcard = openat(directory, "sdcard",
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (sdcard < 0 || (mkdirat(sdcard, "AERA", 0755) && errno != EEXIST))
      Die("RetroArch AERA mount point");
    close(sdcard);
    const int storage = openat(directory, "storage",
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (storage < 0 || (mkdirat(storage, "AERA", 0755) && errno != EEXIST))
      Die("RetroArch compatibility mount point");
    close(storage);
  }
  if (!retroarch && !media && !recorder)
    WriteResolverConfig(directory);
  Check(fchmod(directory, 0755), "runtime root permissions");
  close(directory);
  Supervise(root, telegram ? "536870912" :
      (media || recorder) ? "1073741824" : "1610612736");
  Check(setsid(), "private browser process group");
  const rlim_t file_limit = browser ? 16ULL << 30 :
      recorder ? 2ULL << 30 : 64ULL << 20;
  rlimit files{512U, 512U}, processes{192U, 192U}, core{0, 0},
      size{file_limit, file_limit};
  Check(setrlimit(RLIMIT_NOFILE, &files), "file descriptor limit");
  Check(setrlimit(RLIMIT_NPROC, &processes), "process limit");
  Check(setrlimit(RLIMIT_CORE, &core), "core limit");
  Check(setrlimit(RLIMIT_FSIZE, &size), "file size limit");
  // Close recovery/ADB inherited descriptors. Only stdio, pixels (3), and
  // browser-only control (4) survive. No mount/device/data descriptor leaks.
  Check(syscall(SYS_close_range, 5U, ~0U, 0), "close unrelated descriptors");
  auto *jail = minijail_new();
  if (!jail) Die("minijail_new");
  minijail_namespace_vfs(jail);
  minijail_namespace_uts(jail);
  // Use recovery's already-configured WLAN route. The browser still runs as a
  // dedicated UID inside its chroot with no capabilities and a socket-filtered
  // seccomp policy; it cannot listen, open raw sockets, or alter routes.
  Check(minijail_namespace_set_hostname(jail,
        retroarch ? "aera-retroarch" :
        telegram ? "aera-telegram" : media ? "aera-media" :
        recorder ? "aera-recorder" : "aera-browser"), "private hostname");
  Check(minijail_enter_chroot(jail, root.c_str()), "private filesystem");
  Check(minijail_bind(jail, root.c_str(), "/", 0), "read-only runtime");
  const unsigned long flags = MS_NOSUID | MS_NODEV | MS_NOEXEC;
  Check(minijail_mount_with_data(jail, "proc", "/proc", "proc", flags | MS_RDONLY,
                                "hidepid=2,subset=pid"), "private proc view");
  Check(minijail_mount_with_data(jail, "tmpfs", "/tmp", "tmpfs", flags,
                                "size=512M,mode=1777"), "private temporary storage");
  Check(minijail_mount_with_data(jail, "tmpfs", "/run", "tmpfs", flags,
                                "size=16M,mode=0755,uid=99090,gid=99090"), "private runtime storage");
  Check(minijail_mount_with_data(jail, "tmpfs", "/dev", "tmpfs", MS_NOSUID | MS_NOEXEC,
                                "size=1M,mode=0755"), "private devices");
  for (const char *device : {"/dev/null", "/dev/zero", "/dev/urandom", "/dev/random"})
    Check(minijail_bind(jail, device, device, 1), "safe character device");
  // The browser receives one GPU character device plus the system DMA heap
  // Turnip needs for Vulkan external-memory file descriptors. It receives no
  // display, input, camera, Binder or storage devices; exposing the wider host
  // /dev tree or Android's vendor EGL stack is unnecessary.
  if (!retroarch && !telegram && !media && !recorder) {
    Check(minijail_bind(jail, "/dev/kgsl-3d0", "/dev/kgsl-3d0", 1),
          "browser GPU device");
    Check(minijail_bind(jail, "/dev/dma_heap/system", "/dev/dma_heap/system", 1),
          "browser system DMA heap");
    if (browser)
      Check(minijail_bind(jail, "/sdcard/AERA/Downloads", "/downloads", 1),
            "writable AERA downloads");
  } else if (retroarch && !access("/sdcard/AERA", R_OK | W_OK)) {
    // RetroArch receives only AERA's directory, never the rest of /sdcard.
    // ROMs, saves, states and screenshots can persist under this narrow mount.
    Check(minijail_bind(jail, "/sdcard/AERA", "/sdcard/AERA", 1),
          "AERA emulation content");
    // Keep compatibility with already-published RetroArch configurations that
    // started their browser at /storage/AERA. Both mounts name the same narrow
    // AERA directory; neither exposes the remainder of /sdcard.
    Check(minijail_bind(jail, "/sdcard/AERA", "/storage/AERA", 1),
          "legacy AERA emulation path");
  } else if (telegram) {
    Check(minijail_bind(jail, "/data/recovery/AERA/telegram", "/state", 1),
          "encrypted Telegram state");
    if (!access("/sdcard", R_OK))
      Check(minijail_bind(jail, "/sdcard", "/sdcard", 0),
            "read-only Telegram attachments");
  } else if (media && !access("/sdcard", R_OK)) {
    Check(minijail_bind(jail, "/sdcard", "/sdcard", 0),
          "read-only media library");
  } else if (recorder && !access("/sdcard/AERA/Recordings", R_OK | W_OK)) {
    Check(minijail_bind(jail, "/sdcard/AERA/Recordings", "/recordings", 1),
          "writable AERA recordings");
  }
  Check(minijail_mount_with_data(jail, "tmpfs", "/dev/shm", "tmpfs", flags,
                                "size=128M,mode=1777"), "private shared memory");
  minijail_change_uid(jail, telegram ? kTelegramUid :
      media ? kMediaUid : recorder ? kRecorderUid :
      kBrowserUid);
  minijail_change_gid(jail, telegram ? kTelegramUid :
      media ? kMediaUid : recorder ? kRecorderUid :
      kBrowserUid);
  // Android 16 annotates the list parameter as non-null even when a zero
  // length requests that minijail clear all supplementary groups.
  const bool media_access = retroarch || telegram || media || recorder;
  const gid_t group = media_access ? kMediaRwGid : 0;
  minijail_set_supplementary_gids(jail, media_access ? 1 : 0, &group);
  minijail_use_caps(jail, 0);
  minijail_no_new_privs(jail);
  const bool probe = !strcmp(argv[1], "--probe");
  auto policy = aera_jail::Policy();
  sock_fprog filter{static_cast<unsigned short>(policy.size()), policy.data()};
  minijail_set_seccomp_filters(jail, &filter);
  minijail_use_seccomp_filter(jail);
  // Any failed mount, privilege drop or filter load terminates the helper.
  minijail_enter(jail);
  Check(prctl(PR_SET_DUMPABLE, 0, 0, 0, 0), "non-dumpable process");
  char *const browser_environment[] = {
    const_cast<char *>("PATH=/usr/bin"), const_cast<char *>("HOME=/tmp"),
    const_cast<char *>("TMPDIR=/tmp"), const_cast<char *>("LANG=C.UTF-8"),
    const_cast<char *>("XDG_RUNTIME_DIR=/run"), const_cast<char *>("XDG_CACHE_HOME=/tmp/cache"),
    const_cast<char *>("XDG_DATA_HOME=/tmp/data"), const_cast<char *>("XDG_CONFIG_HOME=/tmp/config"),
    const_cast<char *>("XDG_DATA_DIRS=/usr/share"), const_cast<char *>("GSETTINGS_BACKEND=memory"),
    const_cast<char *>("GIO_MODULE_DIR=/usr/lib/gio/modules"),
    // Recovery has no system D-Bus or desktop network manager. Force GIO's
    // local implementations so proxy/network discovery cannot turn a missing
    // desktop service into a resolver failure before libc sends DNS.
    const_cast<char *>("GIO_USE_PROXY_RESOLVER=dummy"),
    const_cast<char *>("GIO_USE_NETWORK_MONITOR=base"),
    // WebKit coordinated graphics -> Mesa EGL -> Zink -> Turnip -> KGSL.
    // Output remains a WPE SHM frame; the browser never gets the recovery
    // framebuffer or the trusted UI's EGL context.
    const_cast<char *>("VK_DRIVER_FILES=/usr/share/vulkan/icd.d/freedreno_icd.json"),
    const_cast<char *>("MESA_LOADER_DRIVER_OVERRIDE=zink"),
    const_cast<char *>("LIBGL_DRIVERS_PATH=/usr/lib/dri"),
    const_cast<char *>("EGL_PLATFORM=surfaceless"),
    const_cast<char *>("ZINK_DESCRIPTORS=lazy"),
    // Keep WebKit's independently updated video and page layers ordered on
    // Turnip, and finish SHM readback before publishing the composed frame.
    const_cast<char *>("ZINK_DEBUG=sync,flushsync"),
    const_cast<char *>("MESA_SHADER_CACHE_DISABLE=true"),
    // Keep multimedia discovery completely inside the extracted runtime. The
    // registry belongs in the private writable tmpfs; the runtime itself and
    // recovery ramdisk remain read-only.
    const_cast<char *>("GST_PLUGIN_SYSTEM_PATH=/usr/lib/gstreamer-1.0"),
    const_cast<char *>("GST_PLUGIN_SCANNER=/usr/libexec/gstreamer-1.0/gst-plugin-scanner"),
    const_cast<char *>("GST_REGISTRY=/tmp/gstreamer-registry.bin"),
    const_cast<char *>("GST_REGISTRY_REUSE_PLUGIN_SCANNER=no"),
    // The software Cairo tile painter defaults to half the detected cores.
    // Dodge has six fast cores available in recovery; use all six so complex
    // pages do not serialize repaint work behind touch input.
    const_cast<char *>("WEBKIT_CAIRO_PAINTING_THREADS=6"),
    const_cast<char *>("ICU_DATA=/usr/share/icu/76.1"),
    const_cast<char *>("FONTCONFIG_PATH=/etc/fonts"),
    const_cast<char *>("XKB_CONFIG_ROOT=/usr/share/X11/xkb"),
    nullptr};
  char *const retroarch_environment[] = {
    const_cast<char *>("PATH=/usr/bin"), const_cast<char *>("HOME=/tmp"),
    const_cast<char *>("TMPDIR=/tmp"), const_cast<char *>("LANG=C.UTF-8"),
    const_cast<char *>("XDG_RUNTIME_DIR=/run"),
    const_cast<char *>("XDG_CACHE_HOME=/tmp/cache"),
    const_cast<char *>("XDG_DATA_HOME=/tmp/data"),
    const_cast<char *>("XDG_CONFIG_HOME=/tmp/config"), nullptr};
  char *const telegram_environment[] = {
    const_cast<char *>("PATH=/usr/bin"), const_cast<char *>("HOME=/state"),
    const_cast<char *>("TMPDIR=/tmp"), const_cast<char *>("LANG=C.UTF-8"),
    nullptr};
  const char *program = probe ? "/usr/bin/aera-jail-probe" :
      retroarch ? "/usr/bin/retroarch" :
      telegram ? "/usr/bin/aera-telegram" :
      media ? "/usr/bin/aera-media" : recorder ? "/usr/bin/aera-recorder" :
      "/usr/bin/aera-browser-worker";
  char *const browser_args[] = {const_cast<char *>(program),
    const_cast<char *>(probe ? "--inside" : "--isolated-ipc-v1"), nullptr};
  char *const retroarch_args[] = {const_cast<char *>(program),
    const_cast<char *>("--config"), const_cast<char *>("/etc/retroarch.cfg"),
    const_cast<char *>("--menu"), nullptr};
  char *const telegram_args[] = {const_cast<char *>(program),
    const_cast<char *>("--isolated-ipc-v1"), nullptr};
  char *const media_args[] = {const_cast<char *>(program), nullptr};
  char *const recorder_args[] = {const_cast<char *>(program), nullptr};
  execve(program, retroarch ? retroarch_args :
         telegram ? telegram_args : media ? media_args :
         recorder ? recorder_args : browser_args,
         retroarch ? retroarch_environment :
         telegram ? telegram_environment :
         browser_environment);
  Die("exec isolated browser");
}
