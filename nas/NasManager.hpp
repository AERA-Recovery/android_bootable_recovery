/*
 * OrangeFox NAS / network storage manager
 *
 * This implements a virtual NAS storage target using a userspace FUSE backend.
 * Native CIFS/NFS is not possible on your current kernel because CONFIG_CIFS
 * and CONFIG_NFS_FS are disabled.
 */

#ifndef __OF_NAS_MANAGER_HPP
#define __OF_NAS_MANAGER_HPP

#include <string>
#include <sys/stat.h>
#include <stdint.h>
#include <vector>

class NasManager {
public:
	static const std::string Mount_Point;
	static const std::string Config_Dir;
	static const std::string Config_File;
	static const std::string Cache_Dir;
	static const std::string Log_File;
	static const std::string Rclone_Binary;

	static bool IsFuseAvailable();
	static bool IsMounted();
	static bool Mount();
	static bool Unmount();
	static bool SelectAsStorage();
	static bool RefreshStatus();
	static bool ResetTransferStats();
	static bool WaitForPendingUploads(int max_seconds = 1800, uint64_t expected_upload_bytes = 0);
	static std::string GetLastError();

private:
	static std::string last_error;

	static void SetError(const std::string& error);
	static bool EnsureDirectory(const std::string& path, mode_t mode = 0777);
	static bool WriteFile(const std::string& path, const std::string& data, mode_t mode = 0600);
	static bool ReadFile(const std::string& path, std::string& out);
	static bool RunAndWait(const std::vector<std::string>& args, std::string* output = nullptr);
	static bool RunDetached(const std::vector<std::string>& args);
	static bool BuildRcloneConfig();
	static bool RcloneObscure(const std::string& input, std::string& output);
	static std::string GetValue(const std::string& var, const std::string& fallback);
	static std::string Trim(const std::string& input);
	static std::string GetRemoteSpec();
};

#endif // __OF_NAS_MANAGER_HPP
