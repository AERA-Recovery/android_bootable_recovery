/* SPDX-License-Identifier: Apache-2.0 */
#include "../../aera_remote/backup_cancel.hpp"
#include "../include/aeraui/screen_timeout.hpp"
#include "../include/aeraui/nas_auto_connect.hpp"
#include <cassert>
#include <poll.h>
#include <sys/wait.h>

namespace aera::pc {
std::atomic<bool> active{true}, cancelled{false};
bool BackupOperationActive() { return active.load(); }
bool BackupCancelRequested() { return active.load() && cancelled.load(); }
}

int main() {
  aeraui::NasAutoConnectPolicy nas;
  assert(!nas.Ready("", "profile", true, false, 0));
  assert(!nas.Ready("Home", "profile", false, false, 0));
  assert(nas.Ready("Home", "profile", true, false, 100));
  assert(!nas.Ready("Home", "profile", true, false, 200));
  assert(nas.Ready("Home", "profile", true, false, 30100));
  assert(nas.Ready("Home", "profile", true, false, 60100));
  assert(!nas.Ready("Home", "profile", true, false, 90100));
  assert(!nas.Ready("Home", "profile", true, true, 90100));
  assert(nas.Ready("Other", "profile", true, false, 90100));
  nas.Suppress();
  assert(!nas.Ready("Other", "profile", true, false, 120100));
  assert(!nas.Ready("", "profile", true, false, 120100));
  assert(nas.Ready("Other", "profile", true, false, 120200));
  assert(aeraui::ValidScreenTimeout(0));
  assert(!aeraui::ValidScreenTimeout(29));
  assert(!aeraui::ValidScreenTimeout(301));
  assert(aeraui::ScreenTimeoutPosition(0) == 10);
  assert(aeraui::ScreenTimeoutSeconds(10) == 0);
  assert(aeraui::ScreenTimeoutSeconds(9) == 300);
  for (int i = 0; i <= 10; ++i)
    assert(aeraui::ScreenTimeoutPosition(aeraui::ScreenTimeoutSeconds(i)) == i);
  aera::backup::Control control;
  assert(!control.Active() && !control.Requested());
  const auto backup = control.Begin(false);
  assert(control.Active() && !control.Requested());
  assert(control.Request(backup, false));
  assert(control.Requested());
  control.Finish(backup);
  assert(!control.Active() && !control.Requested());
  assert(control.Snapshot().requested);
  const auto restore = control.Begin(true);
  assert(!control.Request(backup, true));
  assert(!control.Request(restore, false));
  control.Finish(backup);
  assert(control.Active());
  assert(control.Request(restore, true));
  control.Finish(restore);
  assert(!control.Request(restore, true));
  using namespace aera::pc;
  int channel[2]; assert(pipe(channel) == 0);
  const auto child = fork(); assert(child >= 0);
  if (!child) {
    close(channel[0]); setpgid(0, 0); signal(SIGTERM, SIG_IGN);
    assert(write(channel[1], "R", 1) == 1);
    while (true) pause();
  }
  close(channel[1]); char ready;
  assert(read(channel[0], &ready, 1) == 1);
  BackupChildCancellation watcher(child);
  cancelled.store(true);
  pollfd pipe_event{channel[0], POLLIN | POLLHUP, 0};
  assert(poll(&pipe_event, 1, 5000) > 0);
  assert(read(channel[0], &ready, 1) == 0);
  watcher.Stop(); int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  close(channel[0]);

  active.store(false);
  const auto unrelated = fork(); assert(unrelated >= 0);
  if (!unrelated) { usleep(200000); _exit(0); }
  BackupChildCancellation disabled(unrelated);
  assert(waitpid(unrelated, &status, 0) == unrelated);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
