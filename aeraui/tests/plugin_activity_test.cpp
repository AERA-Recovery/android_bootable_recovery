/* SPDX-License-Identifier: Apache-2.0 */
#include "plugin_api/activity.hpp"
#include <cassert>
#include <cstdio>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>

using namespace aeraui::plugin_api;
int main() {
  Message message;
  message.request_id = 7;
  message.flags = kActivityActive | kActivityCancellable;
  message.value = 5000;
  snprintf(message.title, sizeof(message.title), "Download");
  std::string error;
  assert(SetActivity("ota", "OTA", "/data/media/0/Download", message, error));
  assert(PluginStorageBusy());
  assert(!HostStorageLease());
  assert(Activities().size() == 1);
  MarkActivityCancelling("ota");
  assert(Activities().front().cancelling);
  message.value = 6500;
  assert(SetActivity("ota", "OTA", "/data/media/0/Download", message, error));
  assert(Activities().front().cancelling);
  assert(!SetActivity("ota", "OTA", "/mnt/nas", message, error));
  message.request_id = 8;
  assert(!SetActivity("ota", "OTA", "/data/media/0/Download", message, error));
  message.request_id = 7;
  message.flags = kActivityComplete;
  assert(SetActivity("ota", "OTA", "/data/media/0/Download", message, error));
  assert(!PluginStorageBusy());
  assert(!Activities().front().cancelling);
  {
    HostStorageLease outer;
    assert(outer);
    HostStorageLease inner;
    assert(inner);
    bool other_acquired = true;
    std::thread contender([&] {
      HostStorageLease lease;
      other_acquired = static_cast<bool>(lease);
    });
    contender.join();
    assert(!other_acquired);
    message.flags = kActivityActive;
    assert(!SetActivity("ota", "OTA", "/mnt/nas", message, error));
    assert(SetActivity("temp", "Test", "/tmp/test", message, error));
    assert(!PluginStorageBusy());
    RemoveActivity("temp");
  }
  assert(SetActivity("ota", "OTA", "/mnt/nas", message, error));
  RemoveActivity("ota");
  assert(!PluginStorageBusy());
  assert(!SetActivity("invalid", "Test", "relative", message, error));
  message.flags = kActivityComplete | kActivityActive;
  assert(!SetActivity("invalid", "Test", "", message, error));
  message.flags = kActivityComplete | kActivityCancellable;
  assert(!SetActivity("invalid", "Test", "", message, error));
  message.flags = kActivityActive;
  message.value = 10001;
  assert(!SetActivity("invalid", "Test", "", message, error));
  message.value = kProgressUnknown;
  for (int i = 0; i < 8; ++i)
    assert(SetActivity(std::to_string(i), "Test", "", message, error));
  assert(!SetActivity("overflow", "Test", "", message, error));
  for (int i = 0; i < 8; ++i) RemoveActivity(std::to_string(i));
  assert(Activities().empty());
  char folder[] = "/tmp/aera-task-file-XXXXXX";
  assert(mkdtemp(folder));
  const std::string path = std::string(folder) + "/update.zip";
  FILE *output = fopen(path.c_str(), "wb"); assert(output);
  assert(fwrite("PK\003\004", 1, 4, output) == 4); fclose(output);
  message.value = 7000; message.flags = kActivityActive;
  assert(SetActivity("ota", "OTA", folder, message, error));
  assert(!SetActivityFile("ota", message.request_id + 1, path, error));
  assert(SetActivityFile("ota", message.request_id, path, error));
  message.flags = kActivityComplete;
  assert(SetActivity("ota", "OTA", folder, message, error));
  assert(Activities().front().result_file == path);
  const std::string link = std::string(folder) + "/link.zip";
  assert(!symlink(path.c_str(), link.c_str()));
  assert(!IsReviewablePackage(link));
  assert(!IsReviewablePackage("/dev/null.zip"));
  RemoveActivity("ota"); unlink(link.c_str()); unlink(path.c_str()); rmdir(folder);
}
