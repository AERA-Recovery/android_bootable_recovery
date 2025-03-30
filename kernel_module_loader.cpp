#include "kernel_module_loader.hpp"
#include "common.h"
#include "variables.h"
#include "data.hpp"

const std::vector<std::string> kernel_modules_requested = TWFunc::split_string(EXPAND(TW_LOAD_VENDOR_MODULES), ' ', true);
const size_t expected_module_count = kernel_modules_requested.size();
size_t requested_modules_loaded = 0;

BOOT_MODE KernelModuleLoader::Get_Boot_Mode() {
	std::string cmdline;
	std::string bootconfig;
	android::base::ReadFileToString("/proc/bootconfig", &bootconfig);
	android::base::ReadFileToString("/proc/cmdline", &cmdline);

	if (cmdline.find("twrpfastboot=1") != std::string::npos && (bootconfig.find("androidboot.force_normal_boot = \"1\"") != std::string::npos ||
		cmdline.find("androidboot.force_normal_boot=1") != std::string::npos))
		return RECOVERY_FASTBOOT_MODE;
	else if (android::base::GetProperty(TW_FASTBOOT_MODE_PROP, "0") == "1")
		return FASTBOOTD_MODE;

	return RECOVERY_IN_BOOT_MODE;
}

bool KernelModuleLoader::Load_Vendor_Modules() {
	// check /lib/modules (ramdisk vendor_boot)
	// check /lib/modules/N.N (ramdisk vendor_boot)
	// check /lib/modules/N.N-gki (ramdisk vendor_boot)
	// check /vendor/lib/modules (ramdisk)
	// check /vendor/lib/modules/1.1 (ramdisk prebuilt modules)
	// check /vendor/lib/modules/N.N (vendor mounted)
	// check /vendor/lib/modules/N.N-gki (vendor mounted)
	// check /vendor_dlkm/lib/modules (vendor_dlkm mounted)
	if (android::base::GetBoolProperty(TW_MODULES_MOUNTED_PROP, false)) return true;

	LOGINFO("Attempting to load modules\n");
	std::string vendor_base_dir(VENDOR_MODULE_DIR);
	std::string base_dir(VENDOR_BOOT_MODULE_DIR);
	std::string vendor_dlkm_base_dir(VENDOR_DLKM_MODULE_DIR);
	std::vector<std::string> module_dirs;
	std::vector<std::string> vendor_module_dirs;

	TWPartition* ven = PartitionManager.Find_Partition_By_Path("/vendor");
	TWPartition* ven_dlkm = PartitionManager.Find_Partition_By_Path("/vendor_dlkm");
	vendor_module_dirs.push_back(VENDOR_MODULE_DIR);
	vendor_module_dirs.push_back(vendor_base_dir + "/1.1");

	module_dirs.push_back(base_dir);

	struct utsname uts;
	if (uname(&uts)) {
		LOGERR("Unable to query kernel for version info\n");
	}

	std::string rls(uts.release);
	std::vector<std::string> release = TWFunc::split_string(rls, '.', true);
	module_dirs.push_back(base_dir + "/" + release[0] + "." + release[1]);
#ifndef TW_LOAD_VENDOR_MODULES_EXCLUDE_GKI
	std::string gki = "/" + release[0] + "." + release[1] + "-gki";
	module_dirs.push_back(base_dir + gki);
	vendor_module_dirs.push_back(vendor_base_dir + gki);
#endif

	TWFunc::RunFoxScript("/system/bin/beforemodules.sh", "");

	Check_Loaded_Kernel_Modules();

	switch(Get_Boot_Mode()) {
		case RECOVERY_FASTBOOT_MODE:
			/* On bootmode: once, there is not always stock kernel
			 * so try only with twrp prebuilt modules.
			 */
			for (auto&& module_dir:vendor_module_dirs) {
				Try_And_Load_Modules(module_dir, false);
				if (requested_modules_loaded >= expected_module_count) goto exit;
			}
			break;

		case FASTBOOTD_MODE:
		case RECOVERY_IN_BOOT_MODE:
#ifdef TW_LOAD_VENDOR_BOOT_MODULES
			for (auto&& module_dir:module_dirs) {
				Try_And_Load_Modules(module_dir, false);
				if (requested_modules_loaded >= expected_module_count) goto exit;
			}
#endif
			/* In both mode vendor_boot or vendor modules are used
			 * Because Ramdisk is flashed in both.
			 */
			break;
	}

	if (ven) {
		LOGINFO("Checking mounted /vendor\n");
		ven->Mount(true);
		for (auto&& module_dir:vendor_module_dirs) {
			Try_And_Load_Modules(module_dir, true);
			if (requested_modules_loaded >= expected_module_count) goto exit;
		}
	}

	if (ven_dlkm) {
		LOGINFO("Checking mounted /vendor_dlkm\n");
		ven_dlkm->Mount(true);
		Try_And_Load_Modules(vendor_dlkm_base_dir, true);
		if (requested_modules_loaded >= expected_module_count) goto exit;
	}

#ifndef OF_SKIP_PREBUILT_MODULES
	if (ven)
		ven->UnMount(false);

	LOGINFO("Checking prebuilt modules\n");
	for (auto&& module_dir:vendor_module_dirs) {
		Try_And_Load_Modules(module_dir, true);
		if (requested_modules_loaded >= expected_module_count) goto exit;
	}
#endif

exit:
	if (ven)
		ven->UnMount(false);
	if (ven_dlkm)
		ven_dlkm->UnMount(false);

	if (requested_modules_loaded == 0)
		DataManager::SetValue("of_request_switch_control_mode" , "1");

	android::base::SetProperty(TW_MODULES_MOUNTED_PROP, "true");

	return true;
}

void KernelModuleLoader::Try_And_Load_Modules(std::string module_dir, bool vendor_is_mounted) {
	LOGINFO("Checking directory: %s\n", module_dir.c_str());
	int modules_loaded = 0;
	std::string dest_module_dir;
	dest_module_dir = "/tmp" + module_dir;
	TWFunc::Recursive_Mkdir(dest_module_dir);
	Copy_Modules_To_Tmpfs(module_dir);
	if (!Write_Module_List(dest_module_dir))
		return;
	if (!vendor_is_mounted && module_dir == "/vendor/lib/modules") {
		module_dir = "/lib/modules";
	}
	LOGINFO("mounting %s on %s\n", dest_module_dir.c_str(), module_dir.c_str());
	if (mount(dest_module_dir.c_str(), module_dir.c_str(), "", MS_BIND, NULL) == 0) {
		Modprobe m({module_dir}, "modules.load.twrp", false);
		m.LoadListedModules(false);
		modules_loaded = m.GetModuleCount();
		umount2(module_dir.c_str(), MNT_DETACH);
		LOGINFO("Modules Loaded (with dependencies): %d\n", modules_loaded);
	}

	Check_Loaded_Kernel_Modules();
}

void KernelModuleLoader::Check_Loaded_Kernel_Modules() {
	std::vector<string> loaded_modules;
	std::string kernel_module_file = "/proc/modules";
	if (TWFunc::read_file(kernel_module_file, loaded_modules) < 0)
		LOGINFO("Failed to get loaded kernel modules\n");
	LOGINFO("Number of loaded modules: %zu\n", loaded_modules.size());
	if (loaded_modules.size() == 0)
		return;
	requested_modules_loaded = 0;
	for (auto&& module_line:loaded_modules) {
		auto module = TWFunc::Split_String(module_line, " ")[0];
		std::string full_module_name = module + ".ko";
		auto found = std::find(kernel_modules_requested.begin(), kernel_modules_requested.end(), full_module_name);
		if (found != kernel_modules_requested.end()) {
			requested_modules_loaded++;
			LOGINFO("Found the requested module loaded: %s\n", (*found).c_str());
		}
	}

	if (requested_modules_loaded >= expected_module_count)
		LOGINFO("All requested modules are loaded\n");
	else
		LOGINFO("Requested modules are already loaded: %zu\n", requested_modules_loaded);
}

bool KernelModuleLoader::Write_Module_List(std::string module_dir) {
	DIR* d;
	struct dirent* de;
	std::vector<std::string> kernel_modules;
	d = opendir(module_dir.c_str());
	if (d != nullptr) {
		while ((de = readdir(d)) != nullptr) {
			std::string kernel_module = de->d_name;
			if (de->d_type == DT_REG) {
				if (android::base::EndsWith(kernel_module, ".ko")) {
					for (auto&& requested:kernel_modules_requested) {
						if (kernel_module == requested) {
							kernel_modules.push_back(kernel_module);
							continue;
						}
					}
					continue;
				}
			}
		}
		std::string module_file = module_dir + "/modules.load.twrp";
		TWFunc::write_to_file(module_file, kernel_modules);
		closedir(d);
	}
	return true;
}

bool KernelModuleLoader::Copy_Modules_To_Tmpfs(std::string module_dir) {
	std::string ramdisk_dir = "/tmp" + module_dir;
	DIR* d;
	struct dirent* de;
	d = opendir(module_dir.c_str());
	if (d != nullptr) {
		while ((de = readdir(d)) != nullptr) {
			std::string kernel_module = de->d_name;
			if (de->d_type == DT_REG) {
				std::string src =  module_dir + "/" + de->d_name;
				std::string dest = ramdisk_dir + "/" + de->d_name;
				if (TWFunc::copy_file(src, dest, 0700, false) != 0) {
					return false;
				}
			}
		}
		closedir(d);
	} else {
		LOGINFO("Unable to open module directory: %s. Skipping\n", module_dir.c_str());
		return false;
	}
	return true;
}
