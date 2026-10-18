/* SPDX-License-Identifier: Apache-2.0 */
// Isolated, disposable PCM client for the existing recovery audio bridge.
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {
volatile sig_atomic_t stopped = 0;
void Stop(int) { stopped = 1; }

bool Send(int fd, const void* bytes, size_t size) {
  auto* cursor = static_cast<const uint8_t*>(bytes);
  while (size && !stopped) {
    const ssize_t count = send(fd, cursor, size, MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    cursor += count;
    size -= static_cast<size_t>(count);
  }
  return size == 0;
}

int Play() {
  // Device bridges already accept the restricted media UID. Do not broaden
  // their protocol or run a sound generator with recovery privileges.
  if (setgroups(0, nullptr) || setgid(99092) || setuid(99092)) return 1;
  prctl(PR_SET_PDEATHSIG, SIGTERM);
  if (getppid() == 1) return 0;
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return 1;
  timeval timeout{0, 200000};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  constexpr char name[] = "aera-browser-audio-v1";
  memcpy(address.sun_path + 1, name, sizeof(name) - 1);
  bool connected = false;
  for (int attempt = 0; attempt < 100 && !stopped; ++attempt) {
    if (!connect(fd, reinterpret_cast<sockaddr*>(&address),
          offsetof(sockaddr_un, sun_path) + sizeof(name))) {
      connected = true;
      break;
    }
    usleep(20000);
  }
  ucred peer{};
  socklen_t peer_size = sizeof(peer);
  if (!connected || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) ||
      peer_size != sizeof(peer) || peer.uid != 0) { close(fd); return 1; }
  constexpr uint32_t hello[] = {0x41525041, 48000, 2, 16};
  if (!Send(fd, hello, sizeof(hello))) { close(fd); return 1; }
  fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
  std::array<int16_t, 1920> pcm{};
  char effect = 0;
  int sample = 0, duration = 0;
  uint32_t noise = 7391;
  timespec deadline{};
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  while (!stopped) {
    char commands[16];
    const ssize_t count = read(STDIN_FILENO, commands, sizeof(commands));
    if (!count) break;
    if (count > 0) {
      effect = commands[count - 1];
      duration = effect == 'w' ? 28800 : effect == 'h' ? 14400 : 9600;
      sample = 0;
    } else if (errno != EAGAIN && errno != EINTR) break;
    for (size_t frame = 0; frame < pcm.size() / 2; ++frame) {
      float value = 0;
      if (sample < duration) {
        const float t = float(sample) / 48000;
        const float envelope = 1 - float(sample) / duration;
        noise = noise * 1664525U + 1013904223U;
        const float random = float((noise >> 16) & 65535) / 32768 - 1;
        if (effect == 'w') {
          const int note = std::min(sample / 4800, 5);
          constexpr float notes[] = {523.25f, 659.25f, 783.99f, 1046.5f, 783.99f, 1046.5f};
          value = std::sin(t * notes[note] * 6.2831853f) * envelope * 0.28f;
        } else {
          const float pitch = effect == 'h' ? 65 : 110;
          value = (random * 0.65f + std::sin(t * pitch * 6.2831853f) * 0.35f) *
              envelope * envelope * 0.46f;
        }
        ++sample;
      }
      pcm[frame * 2] = pcm[frame * 2 + 1] = static_cast<int16_t>(value * 32767);
    }
    if (!Send(fd, pcm.data(), sizeof(pcm))) break;
    deadline.tv_nsec += 20000000;
    if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec > deadline.tv_sec + 1) deadline = now;
  }
  close(fd);
  return 0;
}

void Reap(pid_t pid) {
  if (pid <= 0) return;
  int status = 0;
  if (waitpid(pid, &status, WNOHANG) != 0) return;
  kill(pid, SIGTERM);
  for (int i = 0; i < 15; ++i) {
    if (waitpid(pid, &status, WNOHANG) != 0) return;
    usleep(10000);
  }
  kill(pid, SIGKILL);
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}
}

int main() {
  constexpr char bridge[] = "/system/bin/aera-audio-bridge";
  if (getuid() || access(bridge, X_OK)) return 0;
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, Stop);
  signal(SIGINT, Stop);
  prctl(PR_SET_PDEATHSIG, SIGTERM);
  if (getppid() == 1) return 0;
  const pid_t backend = fork();
  if (!backend) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() == 1) _exit(0);
    execl(bridge, bridge, "--browser-audio", nullptr);
    _exit(1);
  }
  if (backend < 0) return 1;
  const pid_t client = fork();
  if (!client) _exit(Play());
  int status = 0;
  while (client > 0 && !stopped) {
    const pid_t result = waitpid(client, &status, WNOHANG);
    if (result == client || (result < 0 && errno != EINTR)) break;
    usleep(20000);
  }
  Reap(client);
  Reap(backend);
  return 0;
}
