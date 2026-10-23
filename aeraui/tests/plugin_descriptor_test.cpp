/* SPDX-License-Identifier: Apache-2.0 */
#include "../features/plugin_api/close_descriptors.hpp"
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <sys/wait.h>

namespace {
int calls = 0;
int Missing(unsigned first, unsigned last, unsigned flags) {
  assert(first == 5 && last == ~0U && flags == 0);
  ++calls; errno = ENOSYS; return -1;
}
int Denied(unsigned, unsigned, unsigned) { ++calls; errno = EPERM; return -1; }
int Success(unsigned first, unsigned last, unsigned flags) {
  assert(first == 5 && last == ~0U && flags == 0);
  ++calls; return 0;
}
void Check(int mode) {
  const pid_t pid = fork(); assert(pid >= 0);
  if (pid == 0) {
    const int source = open("/dev/null", O_RDWR); assert(source >= 0);
    for (int fd : {3, 4, 99, 999}) assert(dup2(source, fd) == fd);
    using namespace aeraui::plugin_api;
    if (mode == 0) {
      assert(CloseUnrelatedDescriptors(Success) && calls == 1);
      assert(fcntl(99, F_GETFD) >= 0);
    } else if (mode == 1) {
      assert(CloseUnrelatedDescriptors(Missing) && calls == 1);
      assert(fcntl(99, F_GETFD) == -1 && errno == EBADF);
      assert(fcntl(999, F_GETFD) == -1 && errno == EBADF);
    } else if (mode == 2) {
      assert(!CloseUnrelatedDescriptors(Denied) && calls == 1);
      assert(fcntl(99, F_GETFD) >= 0);
    } else {
      assert(CloseUnrelatedDescriptors());
      assert(fcntl(99, F_GETFD) == -1 && errno == EBADF);
      assert(fcntl(999, F_GETFD) == -1 && errno == EBADF);
    }
    assert(fcntl(3, F_GETFD) >= 0 && fcntl(4, F_GETFD) >= 0);
    _exit(0);
  }
  int status = 0; assert(waitpid(pid, &status, 0) == pid);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
}
int main() {
  for (int mode = 0; mode < 4; ++mode) Check(mode);
  puts("Plugin descriptor cleanup: fast path, ENOSYS fallback, failure and channel retention passed");
}
