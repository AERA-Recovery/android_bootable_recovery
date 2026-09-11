/* SPDX-License-Identifier: Apache-2.0 */
#include "launcher.hpp"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace recovery_ui2::plugin_api {
bool Process::Start(const std::string &runtime, int &control_fd,
                    std::string &error) {
  control_fd = -1;
  if (pid_ >= 0 || runtime.size() != 19 ||
      runtime.compare(0, 13, "/tmp/aera-p2-")) {
    error = "The verified API 2 runtime path is invalid.";
    return false;
  }
  struct stat root{};
  if (lstat(runtime.c_str(), &root) != 0 || !S_ISDIR(root.st_mode) ||
      root.st_uid != 0 || (root.st_mode & 0777) != 0700) {
    error = "The API 2 runtime is no longer private.";
    return false;
  }
  int channels[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channels) != 0) {
    error = "Could not create the bounded plugin channel.";
    return false;
  }
  const int child_control = fcntl(channels[1], F_DUPFD_CLOEXEC, 10);
  if (child_control < 0) {
    close(channels[0]);
    close(channels[1]);
    error = "Could not protect the plugin channel.";
    return false;
  }
  const pid_t child = fork();
  if (child == 0) {
    close(3);
    if (dup2(child_control, 4) < 0) _exit(78);
    close(child_control);
    execl("/system/bin/aera-browser-jail", "aera-browser-jail",
          "--plugin-v2", runtime.c_str(), nullptr);
    _exit(78);
  }
  close(child_control);
  close(channels[1]);
  if (child < 0) {
    close(channels[0]);
    error = "Could not start the isolated plugin supervisor.";
    return false;
  }
  pid_ = child;
  control_fd = channels[0];
  error.clear();
  return true;
}

bool Process::Running() {
  if (pid_ < 0) return false;
  int status = 0;
  const pid_t result = waitpid(pid_, &status, WNOHANG);
  if (result == pid_ || (result < 0 && errno == ECHILD)) pid_ = -1;
  return pid_ >= 0;
}

void Process::Stop() {
  if (pid_ < 0) return;
  int status = 0;
  pid_t result = waitpid(pid_, &status, WNOHANG);
  if (result == 0) kill(pid_, SIGTERM);
  for (int attempt = 0; result == 0 && attempt < 25; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    result = waitpid(pid_, &status, WNOHANG);
    if (result < 0 && errno == EINTR) result = 0;
  }
  if (result == 0) {
    kill(pid_, SIGKILL);
    while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
  }
  pid_ = -1;
}
}  // namespace recovery_ui2::plugin_api
