/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace aeraui::artillery {

class Sound {
 public:
  ~Sound() { Close(); }
  Sound() = default;
  Sound(const Sound&) = delete;
  Sound& operator=(const Sound&) = delete;

  bool Prepare() {
    if (muted_) return false;
    if (pid_ > 0) {
      int status = 0;
      if (waitpid(pid_, &status, WNOHANG) != 0) { pid_ = -1; Close(); }
    }
    return fd_ >= 0 || Start();
  }
  void Play(char effect) {
    if (!Prepare()) return;
    if (send(fd_, &effect, 1, MSG_NOSIGNAL | MSG_DONTWAIT) != 1) Close();
  }
  void Stop() { Close(); }
  bool Toggle() { muted_ = !muted_; if (muted_) Close(); return muted_; }

 private:
  bool Start() {
    constexpr char helper[] = "/system/bin/aera-artillery-audio";
    if (access(helper, X_OK)) return false;
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets)) return false;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
    posix_spawn_file_actions_addclose(&actions, sockets[0]);
    posix_spawn_file_actions_addclose(&actions, sockets[1]);
    char* args[] = {const_cast<char*>(helper), nullptr};
    const int result = posix_spawn(&pid_, helper, &actions, nullptr, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(sockets[1]);
    if (result) { close(sockets[0]); pid_ = -1; return false; }
    fd_ = sockets[0];
    return true;
  }
  void Close() {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    if (pid_ <= 0) return;
    kill(pid_, SIGTERM);
    int status = 0;
    for (int i = 0; i < 20; ++i) {
      const pid_t result = waitpid(pid_, &status, WNOHANG);
      if (result == pid_ || (result < 0 && errno != EINTR)) { pid_ = -1; return; }
      usleep(10000);
    }
    kill(pid_, SIGKILL);
    while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
    pid_ = -1;
  }
  pid_t pid_ = -1;
  int fd_ = -1;
  bool muted_ = false;
};

}  // namespace aeraui::artillery
