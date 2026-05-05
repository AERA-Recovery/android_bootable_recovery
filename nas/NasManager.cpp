/*
 * OrangeFox NAS / network storage manager
 */

#include "NasManager.hpp"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../data.hpp"
#include "../partitions.hpp"
#include "../twcommon.h"
#include "../twrp-functions.hpp"
#include "../variables.h"

const std::string NasManager::Mount_Point = TW_NAS_MOUNT_POINT;
const std::string NasManager::Config_Dir = "/tmp/of_nas";
const std::string NasManager::Config_File = "/tmp/of_nas/rclone.conf";
const std::string NasManager::Cache_Dir = "/tmp/of_nas/cache";
const std::string NasManager::Log_File = "/tmp/of_nas/rclone.log";
const std::string NasManager::Rclone_Binary = "/system/bin/rclone";

static const std::string Rclone_Obscure_Config_File = "/tmp/of_nas/rclone_obscure.conf";

std::string NasManager::last_error;

static std::string TrimStringLocal(const std::string& input) {
	std::string out = input;

	while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '\t'))
		out.pop_back();

	while (!out.empty() && (out.front() == '\n' || out.front() == '\r' || out.front() == ' ' || out.front() == '\t'))
		out.erase(out.begin());

	return out;
}

static bool LooksLikeRcloneLogLine(const std::string& line) {
	if (line.find(" ERROR : ") != std::string::npos)
		return true;
	if (line.find(" NOTICE: ") != std::string::npos)
		return true;
	if (line.find(" NOTICE : ") != std::string::npos)
		return true;
	if (line.find(" DEBUG : ") != std::string::npos)
		return true;
	if (line.find(" INFO  : ") != std::string::npos)
		return true;
	if (line.find(" INFO : ") != std::string::npos)
		return true;
	if (line.find(" CRITICAL: ") != std::string::npos)
		return true;
	if (line.find(" CRITICAL : ") != std::string::npos)
		return true;

	// Rclone log lines usually start like:
	// 2026/05/04 20:38:28 ERROR : ...
	if (line.size() > 22 &&
		std::isdigit(line[0]) &&
		std::isdigit(line[1]) &&
		std::isdigit(line[2]) &&
		std::isdigit(line[3]) &&
		line[4] == '/' &&
		line[7] == '/' &&
		line[10] == ' ') {
		return true;
	}

	return false;
}

static std::string LastCleanRcloneOutputLine(const std::string& output) {
	std::stringstream ss(output);
	std::string line;
	std::string last_clean;

	while (std::getline(ss, line)) {
		line = TrimStringLocal(line);
		if (line.empty())
			continue;

		if (LooksLikeRcloneLogLine(line))
			continue;

		last_clean = line;
	}

	return last_clean;
}

void NasManager::SetError(const std::string& error) {
	last_error = error;
	DataManager::SetValue(TW_NAS_LAST_ERROR, error);
	DataManager::SetValue(TW_NAS_STATUS_TEXT, error);
	LOGERR("NAS: %s\n", error.c_str());
}

std::string NasManager::GetLastError() {
	return last_error;
}

std::string NasManager::Trim(const std::string& input) {
	return TrimStringLocal(input);
}

std::string NasManager::GetValue(const std::string& var, const std::string& fallback) {
	std::string value;
	DataManager::GetValue(var, value);
	value = Trim(value);

	if (value.empty())
		return fallback;

	return value;
}

bool NasManager::EnsureDirectory(const std::string& path, mode_t mode) {
	if (path.empty())
		return false;

	if (TWFunc::Path_Exists(path)) {
		chmod(path.c_str(), mode);
		return true;
	}

	if (TWFunc::Recursive_Mkdir(path)) {
		chmod(path.c_str(), mode);
		return true;
	}

	std::stringstream ss;
	ss << "Failed to create directory " << path << ": " << strerror(errno);
	SetError(ss.str());
	return false;
}

bool NasManager::WriteFile(const std::string& path, const std::string& data, mode_t mode) {
	int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, mode);
	if (fd < 0) {
		std::stringstream ss;
		ss << "Failed to open " << path << " for writing: " << strerror(errno);
		SetError(ss.str());
		return false;
	}

	ssize_t written = write(fd, data.data(), data.size());
	if (written < 0 || static_cast<size_t>(written) != data.size()) {
		std::stringstream ss;
		ss << "Failed to write " << path << ": " << strerror(errno);
		close(fd);
		SetError(ss.str());
		return false;
	}

	fsync(fd);
	close(fd);
	chmod(path.c_str(), mode);
	return true;
}

bool NasManager::ReadFile(const std::string& path, std::string& out) {
	std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
	if (!in.is_open())
		return false;

	std::stringstream ss;
	ss << in.rdbuf();
	out = ss.str();
	return true;
}

bool NasManager::RunAndWait(const std::vector<std::string>& args, std::string* output) {
	if (args.empty()) {
		SetError("RunAndWait called with empty argv");
		return false;
	}

	int pipefd[2] = {-1, -1};

	if (output && pipe(pipefd) != 0) {
		std::stringstream ss;
		ss << "pipe() failed: " << strerror(errno);
		SetError(ss.str());
		return false;
	}

	pid_t pid = fork();
	if (pid < 0) {
		std::stringstream ss;
		ss << "fork() failed: " << strerror(errno);
		SetError(ss.str());

		if (pipefd[0] >= 0)
			close(pipefd[0]);
		if (pipefd[1] >= 0)
			close(pipefd[1]);

		return false;
	}

	if (pid == 0) {
		if (output) {
			close(pipefd[0]);
			dup2(pipefd[1], STDOUT_FILENO);
			dup2(pipefd[1], STDERR_FILENO);
			close(pipefd[1]);
		}

		std::vector<char*> argv;
		argv.reserve(args.size() + 1);

		for (const std::string& arg : args)
			argv.push_back(const_cast<char*>(arg.c_str()));

		argv.push_back(nullptr);

		execv(argv[0], argv.data());
		_exit(127);
	}

	if (output) {
		close(pipefd[1]);

		char buf[512];
		ssize_t n;

		while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
			output->append(buf, n);

		close(pipefd[0]);
	}

	int status = 0;
	if (waitpid(pid, &status, 0) < 0) {
		std::stringstream ss;
		ss << "waitpid() failed: " << strerror(errno);
		SetError(ss.str());
		return false;
	}

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		std::stringstream ss;
		ss << args[0] << " exited with status ";

		if (WIFEXITED(status))
			ss << WEXITSTATUS(status);
		else
			ss << status;

		if (output && !output->empty())
			ss << ": " << Trim(*output);

		SetError(ss.str());
		return false;
	}

	if (output)
		*output = Trim(*output);

	return true;
}

bool NasManager::RcloneObscure(const std::string& input, std::string& output) {
	output.clear();

	if (input.empty())
		return true;

	setenv("PATH", "/system/bin:/sbin:/vendor/bin:/system/xbin", 1);
	setenv("LD_LIBRARY_PATH", "/system/lib64:/vendor/lib64:/sbin", 1);
	setenv("HOME", Config_Dir.c_str(), 1);

	if (!EnsureDirectory(Config_Dir, 0777))
		return false;

	// Prevent rclone obscure from trying to locate a home/config directory and
	// printing warnings into stdout/stderr, which would corrupt "pass =".
	if (!WriteFile(Rclone_Obscure_Config_File, "", 0600))
		return false;

	std::string raw_output;
	std::vector<std::string> args = {
		Rclone_Binary,
		"--config",
		Rclone_Obscure_Config_File,
		"obscure",
		input
	};

	if (!RunAndWait(args, &raw_output))
		return false;

	output = LastCleanRcloneOutputLine(raw_output);
	output = Trim(output);

	if (output.empty()) {
		SetError("rclone obscure returned empty password output.");
		return false;
	}

	return true;
}

bool NasManager::IsFuseAvailable() {
	if (!TWFunc::Path_Exists("/dev/fuse")) {
		SetError("/dev/fuse is missing. Kernel FUSE support or recovery device node setup is missing.");
		return false;
	}

	chmod("/dev/fuse", 0666);
	return true;
}

bool NasManager::IsMounted() {
	std::ifstream mounts("/proc/mounts");
	std::string line;

	while (std::getline(mounts, line)) {
		if (line.find(" " + Mount_Point + " ") != std::string::npos)
			return true;
	}

	return false;
}

std::string NasManager::GetRemoteSpec() {
	std::string type = GetValue(TW_NAS_TYPE, "sftp");

	std::transform(type.begin(), type.end(), type.begin(),
		[](unsigned char c) { return std::tolower(c); });

	std::string path = GetValue(TW_NAS_PATH, "");

	if (path.empty())
		return "nas:";

	if (type == "sftp") {
		// Preserve absolute SFTP paths.
		// /home/koaan/OrangeFoxNAS becomes:
		// nas:/home/koaan/OrangeFoxNAS
		return "nas:" + path;
	}

	// SMB paths are paths inside the configured share, so strip leading slash.
	while (!path.empty() && path.front() == '/')
		path.erase(path.begin());

	if (path.empty())
		return "nas:";

	return "nas:" + path;
}

bool NasManager::BuildRcloneConfig() {
	setenv("PATH", "/system/bin:/sbin:/vendor/bin:/system/xbin", 1);
	setenv("LD_LIBRARY_PATH", "/system/lib64:/vendor/lib64:/sbin", 1);
	setenv("HOME", Config_Dir.c_str(), 1);
	setenv("RCLONE_CONFIG", Config_File.c_str(), 1);

	std::string type = GetValue(TW_NAS_TYPE, "sftp");

	std::transform(type.begin(), type.end(), type.begin(),
		[](unsigned char c) { return std::tolower(c); });

	std::string host = GetValue(TW_NAS_HOST, "");
	if (host.empty()) {
		SetError("NAS host/IP is empty.");
		return false;
	}

	std::string user = GetValue(TW_NAS_USER, "");
	std::string pass = GetValue(TW_NAS_PASS, "");
	std::string obscure_pass;

	if (!pass.empty() && !RcloneObscure(pass, obscure_pass))
		return false;

	std::stringstream cfg;
	cfg << "[nas]\n";

	if (type == "sftp") {
		cfg << "type = sftp\n";
		cfg << "host = " << host << "\n";

		if (!user.empty())
			cfg << "user = " << user << "\n";

		cfg << "port = " << GetValue(TW_NAS_PORT, "22") << "\n";

		if (!obscure_pass.empty())
			cfg << "pass = " << obscure_pass << "\n";

		cfg << "shell_type = unix\n";
	} else {
		cfg << "type = smb\n";
		cfg << "host = " << host << "\n";
		cfg << "share = " << GetValue(TW_NAS_SHARE, "Backups") << "\n";

		if (!user.empty())
			cfg << "user = " << user << "\n";

		if (!obscure_pass.empty())
			cfg << "pass = " << obscure_pass << "\n";

		cfg << "domain = " << GetValue(TW_NAS_DOMAIN, "WORKGROUP") << "\n";
	}

	return WriteFile(Config_File, cfg.str(), 0600);
}

bool NasManager::Mount() {
	last_error.clear();
	DataManager::SetValue(TW_NAS_LAST_ERROR, "");

	setenv("PATH", "/system/bin:/sbin:/vendor/bin:/system/xbin", 1);
	setenv("LD_LIBRARY_PATH", "/system/lib64:/vendor/lib64:/sbin", 1);
	setenv("HOME", Config_Dir.c_str(), 1);
	setenv("RCLONE_CONFIG", Config_File.c_str(), 1);

	if (IsMounted()) {
		DataManager::SetValue(TW_NAS_MOUNTED, 1);
		DataManager::SetValue(TW_NAS_STATUS_TEXT, "NAS already mounted");
		return true;
	}

	if (!TWFunc::Path_Exists(Rclone_Binary)) {
		SetError("/system/bin/rclone is missing. Add rclone to recovery system/bin.");
		return false;
	}

	if (!TWFunc::Path_Exists("/system/bin/fusermount3")) {
		SetError("/system/bin/fusermount3 is missing. rclone mount requires fusermount3 in PATH.");
		return false;
	}

	if (!IsFuseAvailable())
		return false;

	if (!EnsureDirectory(Config_Dir, 0777))
		return false;

	if (!EnsureDirectory(Cache_Dir, 0777))
		return false;

	if (!EnsureDirectory(Mount_Point, 0777))
		return false;

	chmod(Rclone_Binary.c_str(), 0755);
	chmod("/system/bin/fusermount3", 0755);
	chmod("/dev/fuse", 0666);

	if (!BuildRcloneConfig())
		return false;

	std::string remote = GetRemoteSpec();

	std::vector<std::string> args = {
		Rclone_Binary,
		"mount",
		remote,
		Mount_Point,
		"--config", Config_File,
		"--uid", "0",
		"--gid", "0",
		"--umask", "000",
		"--vfs-cache-mode", "writes",
		"--cache-dir", Cache_Dir,
		"--log-file", Log_File,
		"--log-level", "DEBUG",
		"--daemon"
	};

	// Important:
	// Do NOT capture stdout/stderr here. With "rclone mount --daemon",
	// capturing through a pipe can hang forever if the daemon keeps the pipe open.
	if (!RunAndWait(args, nullptr)) {
		std::string log;
		if (ReadFile(Log_File, log) && !log.empty())
			SetError(GetLastError() + "\n" + Trim(log));

		return false;
	}

	for (int i = 0; i < 30; ++i) {
		if (IsMounted()) {
			DataManager::SetValue(TW_NAS_MOUNTED, 1);
			DataManager::SetValue(TW_NAS_STATUS_TEXT, "NAS mounted");
			RefreshStatus();
			return true;
		}

		usleep(100000);
	}

	std::string log;
	if (ReadFile(Log_File, log) && !log.empty())
		SetError("NAS mount did not appear in /proc/mounts. rclone log: " + Trim(log));
	else
		SetError("NAS mount did not appear in /proc/mounts.");

	return false;
}

bool NasManager::Unmount() {
	last_error.clear();
	DataManager::SetValue(TW_NAS_LAST_ERROR, "");

	setenv("PATH", "/system/bin:/sbin:/vendor/bin:/system/xbin", 1);
	setenv("LD_LIBRARY_PATH", "/system/lib64:/vendor/lib64:/sbin", 1);
	setenv("HOME", Config_Dir.c_str(), 1);

	if (!IsMounted()) {
		DataManager::SetValue(TW_NAS_MOUNTED, 0);
		DataManager::SetValue(TW_NAS_STATUS_TEXT, "NAS is not mounted");
		return true;
	}

	sync();

	if (umount2(Mount_Point.c_str(), 0) != 0) {
		LOGINFO("NAS: normal unmount failed: %s; trying lazy unmount\n", strerror(errno));

		if (umount2(Mount_Point.c_str(), MNT_DETACH) != 0) {
			std::stringstream ss;
			ss << "Failed to unmount NAS: " << strerror(errno);
			SetError(ss.str());
			return false;
		}
	}

	if (TWFunc::Path_Exists("/system/bin/killall"))
		RunAndWait({"/system/bin/killall", "rclone"}, nullptr);

	if (TWFunc::Path_Exists("/sbin/killall"))
		RunAndWait({"/sbin/killall", "rclone"}, nullptr);

	DataManager::SetValue(TW_NAS_MOUNTED, 0);
	DataManager::SetValue(TW_NAS_STATUS_TEXT, "NAS unmounted");
	return true;
}

bool NasManager::SelectAsStorage() {
	if (!IsMounted()) {
		SetError("NAS is not mounted. Mount it before selecting it as storage.");
		return false;
	}

	DataManager::SetValue("tw_storage_path", Mount_Point);
	DataManager::SetValue(TW_ZIP_LOCATION_VAR, Mount_Point);
	DataManager::SetValue(TW_USE_EXTERNAL_STORAGE, 1);
	DataManager::SetValue(TW_NAS_USE_AS_STORAGE, 1);
	DataManager::SetValue(TW_NAS_STATUS_TEXT, "NAS selected as current storage");

	return true;
}

bool NasManager::RefreshStatus() {
	bool mounted = IsMounted();

	DataManager::SetValue(TW_NAS_MOUNTED, mounted ? 1 : 0);

	if (mounted)
		DataManager::SetValue(TW_NAS_STATUS_TEXT, "NAS mounted at " + Mount_Point);
	else
		DataManager::SetValue(TW_NAS_STATUS_TEXT, "NAS not mounted");

	return mounted;
}
