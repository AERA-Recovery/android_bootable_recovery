/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <string>

namespace aera::mobile {
inline constexpr const char *kSocket = "/dev/socket/aera-cellular";
inline constexpr const char *kStatus = "/tmp/aera-cellular/status.json";
inline constexpr const char *kLease = "/tmp/aera-cellular-operation.lock";
inline constexpr const char *kAutoHandled = "/tmp/aera-cellular-auto-handled";

inline bool MarkAutoHandled(bool exclusive = false) {
  const int fd = open(kAutoHandled, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW |
      (exclusive ? O_EXCL : 0), 0600);
  if (fd < 0) return false;
  close(fd);
  return true;
}

inline bool Request(const std::string &command, std::string *reply, int timeout = 500) {
  const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return false;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  strcpy(address.sun_path, kSocket);
  bool ok = connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
  if (ok) ok = send(fd, command.data(), command.size(), MSG_NOSIGNAL) ==
      static_cast<ssize_t>(command.size());
  if (ok) {
    pollfd p{fd, POLLIN, 0};
    int result;
    do { result = poll(&p, 1, timeout); } while (result < 0 && errno == EINTR);
    char buffer[1024];
    const ssize_t count = result > 0 ? recv(fd, buffer, sizeof(buffer), 0) : -1;
    ok = count > 0;
    if (ok && reply) reply->assign(buffer, count);
  }
  close(fd);
  return ok;
}

// The modem reads firmware lazily. Stop it before any operation can replace
// its backing partitions, and prevent a concurrent Connect until we finish.
class StorageLease {
 public:
  StorageLease() {
    if (depth_++ != 0) { valid_ = inherited_; return; }
    // A boot auto-connect must not restart the modem after a flash/restore.
    if (access("/system/bin/aera-cellular-provider", X_OK) == 0) MarkAutoHandled();
    std::string reply;
    const bool stopped = Request("disconnect", &reply, 65000);
    if (stopped && reply != "OK") { inherited_ = false; return; }
    fd_ = open(kLease, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    valid_ = fd_ >= 0 && flock(fd_, LOCK_EX | LOCK_NB) == 0;
    inherited_ = valid_;
  }
  ~StorageLease() {
    if (fd_ >= 0) close(fd_);
    if (--depth_ == 0) inherited_ = true;
  }
  explicit operator bool() const { return valid_; }
  StorageLease(const StorageLease &) = delete;
  StorageLease &operator=(const StorageLease &) = delete;
 private:
  int fd_ = -1;
  bool valid_ = false;
  inline static thread_local unsigned depth_ = 0;
  inline static thread_local bool inherited_ = true;
};
}
