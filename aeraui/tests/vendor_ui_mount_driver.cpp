// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/mount.h>

#define LOGINFO(...) ((void)0)
#define LOGERR(...) ((void)0)

struct TWPartition {
  bool mounted = false;
  bool mount_ok = true;
  bool Can_Be_Mounted = true;
  unsigned int mounts = 0;
  unsigned int unmounts = 0;
  bool Mount(bool) { ++mounts; return mounted = mount_ok; }
  bool UnMount(bool) { ++unmounts; mounted = false; return true; }
  bool Is_Mounted() const { return mounted; }
};

class TWPartitionManager {
 public:
  TWPartition vendor, system, odm, product, data, boot;
  bool has_vendor = true;
  bool datamedia = false;
  bool native_ui_vendor_ = false;
  bool Prepare_Vendor_For_Native_UI(bool fastboot_mode);
  void UnMount_Main_Partitions();
  std::string Get_Android_Root_Path() { return "/system"; }
  TWPartition* Find_Partition_By_Path(const std::string& path) {
    if (path == "/vendor") return has_vendor ? &vendor : nullptr;
    if (path == "/system") return &system;
    if (path == "/odm") return &odm;
    if (path == "/product") return &product;
    if (path == "/data") return &data;
    if (path == "/boot") return &boot;
    return nullptr;
  }
  int UnMount_By_Path(const std::string& path, bool display_error) {
    auto* partition = Find_Partition_By_Path(path);
    return partition ? partition->UnMount(display_error) : false;
  }
  void ApexCleanup() {
    bool Display_Error = false;
    auto* ven = Find_Partition_By_Path("/vendor");
    auto* sys = Find_Partition_By_Path("/system");
    (void)Display_Error; (void)ven; (void)sys;
    // ACTUAL_APEX_CLEANUP
  }
};

TWPartitionManager PartitionManager;
void Process_ResetProps(TWPartition*, TWPartition*) {}

void ModuleCleanupProbe() {
  auto* ven = PartitionManager.Find_Partition_By_Path("/vendor");
  // ACTUAL_MODULE_MOUNT_STATE
  if (ven) ven->Mount(false);
  // ACTUAL_MODULE_CLEANUP
}

namespace TWFunc {
void Recursive_Mkdir(const std::string&) {}
}
class Modprobe {
 public:
  Modprobe(std::initializer_list<std::string>, const char*, bool) {}
  void LoadListedModules(bool) {}
  int GetModuleCount() { return 1; }
};
std::vector<std::string> kernel_modules_requested = {"fixture-module"};
std::string unmounted_bind;
int TestMount(const char*, const char*, const char*, unsigned long, const void*) { return 0; }
int TestUnmount(const char* target, int flags) {
  assert(flags == MNT_DETACH);
  unmounted_bind = target;
  return 0;
}
class KernelModuleLoader {
 public:
  static int Try_And_Load_Modules(std::string module_dir, bool vendor_is_mounted);
  static void Copy_Modules_To_Tmpfs(const std::string&) {}
  static bool Write_Module_List(const std::string&) { return true; }
};

#define mount TestMount
#define umount2 TestUnmount
// ACTUAL_MODULE_BIND
#undef mount
#undef umount2

// ACTUAL_PREPARE
// ACTUAL_MAIN_CLEANUP
// ACTUAL_PROPERTY_PROBE

int main(int argc, char** argv) {
  assert(argc == 2);
  auto& manager = PartitionManager;
  const std::string scenario = argv[1];
  if (scenario == "delayed") {
    assert(manager.Prepare_Vendor_For_Native_UI(false));
    std::mutex mutex;
    std::condition_variable condition;
    bool cleanup_finished = false;
    std::thread renderer([&] {
      std::unique_lock<std::mutex> lock(mutex);
      condition.wait(lock, [&] { return cleanup_finished; });
      assert(manager.vendor.Is_Mounted());
    });
    manager.ApexCleanup();
    manager.UnMount_Main_Partitions();
    {
      std::lock_guard<std::mutex> lock(mutex);
      cleanup_finished = true;
    }
    condition.notify_one();
    renderer.join();
    assert(manager.vendor.unmounts == 0);
    assert(manager.data.unmounts == 1 && manager.product.unmounts == 1);
  } else if (scenario == "unowned") {
    manager.vendor.mounted = true;
    manager.UnMount_Main_Partitions();
    assert(!manager.vendor.mounted && manager.vendor.unmounts == 1);
  } else if (scenario == "unavailable") {
    manager.has_vendor = false;
    assert(manager.Prepare_Vendor_For_Native_UI(false));
    assert(!manager.native_ui_vendor_ && manager.vendor.mounts == 0);
    manager.has_vendor = true;
    manager.vendor.mount_ok = false;
    assert(!manager.Prepare_Vendor_For_Native_UI(false));
    assert(!manager.native_ui_vendor_);
  } else if (scenario == "fastboot") {
    assert(manager.Prepare_Vendor_For_Native_UI(true));
    assert(!manager.native_ui_vendor_ && manager.vendor.mounts == 0);
    assert(manager.Prepare_Vendor_For_Native_UI(false));
    assert(manager.native_ui_vendor_ && manager.vendor.mounts == 1);
  } else if (scenario == "explicit") {
    assert(manager.Prepare_Vendor_For_Native_UI(false));
    assert(manager.UnMount_By_Path("/vendor", false));
    assert(!manager.vendor.mounted && manager.vendor.unmounts == 1);
  } else if (scenario == "probe") {
    manager.vendor.mounted = true;
    Update_Encryption_Props_Before_Format();
    assert(manager.vendor.mounted && manager.vendor.unmounts == 0);
    manager.vendor.mounted = false;
    Update_Encryption_Props_Before_Format();
    assert(!manager.vendor.mounted && manager.vendor.unmounts == 1);
  } else if (scenario == "module-cleanup") {
    assert(manager.Prepare_Vendor_For_Native_UI(false));
    ModuleCleanupProbe();
    assert(manager.vendor.mounted && manager.vendor.unmounts == 0);
    manager.vendor.mounted = false;
    ModuleCleanupProbe();
    assert(!manager.vendor.mounted && manager.vendor.unmounts == 1);
  } else if (scenario == "module-bind") {
    assert(manager.Prepare_Vendor_For_Native_UI(false));
    assert(KernelModuleLoader::Try_And_Load_Modules("/vendor/lib/modules", true) == 1);
    assert(unmounted_bind == "/vendor/lib/modules");
    assert(manager.vendor.mounted && manager.vendor.unmounts == 0);
    assert(KernelModuleLoader::Try_And_Load_Modules("/vendor/lib/modules", false) == 1);
    assert(unmounted_bind == "/lib/modules");
  } else {
    return 2;
  }
}
