/* SPDX-License-Identifier: Apache-2.0 */
#include "fontsource.hpp"

#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aeraui::fontsource {
bool Download(const std::string& url, size_t limit, std::atomic<bool>& cancel,
              std::string& bytes, std::string& error) {
  bytes.clear();
  const std::string api = "https://api.fontsource.org/v1/fonts";
  const bool metadata = url == api ||
      (url.compare(0, api.size() + 1, api + "/") == 0 &&
       url.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-", api.size() + 1) == std::string::npos);
  if ((!metadata && !FontUrl(url)) || limit == 0 || limit > 32 * 1024 * 1024 || cancel.load()) {
    error = "Download failed"; return false;
  }
  int output[2];
  if (pipe2(output, O_CLOEXEC) != 0) { error = "Download failed"; return false; }
  const char* executable = access("/sbin/busybox", X_OK) == 0 ? "/sbin/busybox" : "/system/bin/busybox";
  const pid_t child = fork();
  if (child == 0) {
    close(output[0]);
    if (dup2(output[1], STDOUT_FILENO) < 0) _exit(127);
    close(output[1]);
    const int null_fd = open("/dev/null", O_RDWR);
    if (null_fd < 0 || dup2(null_fd, STDIN_FILENO) < 0 || dup2(null_fd, STDERR_FILENO) < 0) _exit(127);
    if (null_fd > STDERR_FILENO) close(null_fd);
    const char* arguments[] = {executable, "wget", "-q", "-T", "15", "-t", "1",
        "-U", "AERA-Recovery-FontBrowser/1.0", "-O", "-", url.c_str(), nullptr};
    execv(executable, const_cast<char* const*>(arguments));
    _exit(127);
  }
  close(output[1]);
  if (child < 0) { close(output[0]); error = "Download failed"; return false; }
  fcntl(output[0], F_SETFL, fcntl(output[0], F_GETFL) | O_NONBLOCK);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
  bool eof = false, failed = false;
  while (!eof && !failed && !cancel.load() && std::chrono::steady_clock::now() < deadline) {
    pollfd event{output[0], POLLIN | POLLHUP, 0};
    const int ready = poll(&event, 1, 100);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0 || (event.revents & (POLLERR | POLLNVAL))) { failed = true; break; }
    if (ready <= 0) continue;
    char buffer[32768];
    for (;;) {
      const auto count = read(output[0], buffer, sizeof(buffer));
      if (count > 0) {
        if (bytes.size() + static_cast<size_t>(count) > limit) { failed = true; break; }
        bytes.append(buffer, count);
      } else if (count == 0) { eof = true; break; }
      else if (errno != EINTR) { if (errno != EAGAIN) failed = true; break; }
    }
  }
  close(output[0]);
  if (!eof) kill(child, SIGKILL);
  int status = 0;
  pid_t waited;
  do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  // Recovery may already have reaped wget. EOF, bounds and the caller's JSON /
  // FreeType validation still apply when its process-wide SIGCHLD handler wins.
  const bool exited = (waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0) ||
      (waited < 0 && errno == ECHILD);
  if (!eof || failed || cancel.load() || !exited || bytes.empty()) {
    bytes.clear(); error = "Download failed";
    return false;
  }
  return true;
}
}  // namespace aeraui::fontsource
