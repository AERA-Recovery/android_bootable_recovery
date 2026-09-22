/*
 * OrangeFox NAS / network storage manager
 */

#include "NasManager.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
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
static const std::string Data_Cache_Dir = "/data/media/0/AERA/NASCache";
static const std::string Nas_Cache_Mode_Off = "off";
static const std::string Nas_Cache_Mode_Data = "data";
static const std::string Data_Cache_Max_Size = "25G";
static const std::string Rclone_RC_Addr = "127.0.0.1:5572";

static const uint64_t KiB = 1024ULL;
static const uint64_t MiB = 1024ULL * KiB;
static const uint64_t MB = 1000ULL * 1000ULL;

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

static uint64_t GetFreeBytesForPath(const std::string& path) {
	struct statvfs st;

	if (statvfs(path.c_str(), &st) != 0)
		return 0;

	return static_cast<uint64_t>(st.f_bavail) * static_cast<uint64_t>(st.f_frsize);
}

static uint64_t GetDirectoryBytes(const std::string& path) {
	DIR* dir = opendir(path.c_str());
	if (!dir)
		return 0;

	uint64_t total = 0;
	struct dirent* entry;

	while ((entry = readdir(dir)) != nullptr) {
		std::string name = entry->d_name;

		if (name == "." || name == "..")
			continue;

		std::string full_path = path + "/" + name;

		struct stat st;
		if (lstat(full_path.c_str(), &st) != 0)
			continue;

		if (S_ISDIR(st.st_mode))
			total += GetDirectoryBytes(full_path);
		else
			total += static_cast<uint64_t>(st.st_size);
	}

	closedir(dir);
	return total;
}

static bool RemoveTreeContents(const std::string& path) {
	DIR* dir = opendir(path.c_str());
	if (!dir)
		return errno == ENOENT;

	struct dirent* entry;

	while ((entry = readdir(dir)) != nullptr) {
		std::string name = entry->d_name;

		if (name == "." || name == "..")
			continue;

		std::string full_path = path + "/" + name;

		struct stat st;
		if (lstat(full_path.c_str(), &st) != 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			RemoveTreeContents(full_path);
			rmdir(full_path.c_str());
		} else {
			unlink(full_path.c_str());
		}
	}

	closedir(dir);
	return true;
}

static std::string NormalizeCacheMode(const std::string& raw_mode) {
	std::string mode = TrimStringLocal(raw_mode);

	std::transform(mode.begin(), mode.end(), mode.begin(),
		[](unsigned char c) { return std::tolower(c); });

	if (mode == "data" || mode == "cache" || mode == "writes" || mode == "1" || mode == "true" || mode == "yes")
		return Nas_Cache_Mode_Data;

	return Nas_Cache_Mode_Off;
}

static std::string GetConfiguredCacheMode() {
	std::string mode;
	DataManager::GetValue(TW_NAS_CACHE_MODE, mode);

	mode = NormalizeCacheMode(mode);
	DataManager::SetValue(TW_NAS_CACHE_MODE, mode);
	return mode;
}

struct RcloneRCStats {
	uint64_t bytes;
	double speed;
	int transfers;
	int active_transfers;
	int checks;
	int errors;
	bool fatal_error;
	bool retry_error;

	RcloneRCStats() : bytes(0), speed(0), transfers(0), active_transfers(0), checks(0), errors(0), fatal_error(false), retry_error(false) {}
};

static bool RcloneRCPost(const std::string& path, std::string& response) {
	response.clear();

	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return false;

	struct timeval timeout;
	timeout.tv_sec = 2;
	timeout.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(5572);

	if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
		close(fd);
		return false;
	}

	if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
		close(fd);
		return false;
	}

	std::stringstream request;
	request << "POST " << path << " HTTP/1.0\r\n"
		<< "Host: " << Rclone_RC_Addr << "\r\n"
		<< "Content-Type: application/json\r\n"
		<< "Content-Length: 2\r\n"
		<< "Connection: close\r\n"
		<< "\r\n"
		<< "{}";

	std::string request_str = request.str();
	const char* data = request_str.data();
	size_t remaining = request_str.size();

	while (remaining > 0) {
		ssize_t written = send(fd, data, remaining, 0);
		if (written <= 0) {
			close(fd);
			return false;
		}

		data += written;
		remaining -= static_cast<size_t>(written);
	}

	char buf[1024];
	ssize_t n;

	while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
		response.append(buf, n);

	close(fd);

	if (response.find(" 200 ") == std::string::npos && response.find(" 200 OK") == std::string::npos)
		return false;

	size_t body = response.find("\r\n\r\n");
	if (body != std::string::npos)
		response.erase(0, body + 4);

	return !response.empty();
}

static bool JsonNumberLocal(const std::string& json, const std::string& key, double& out) {
	std::string needle = "\"" + key + "\"";
	size_t pos = json.find(needle);
	if (pos == std::string::npos)
		return false;

	pos = json.find(':', pos + needle.size());
	if (pos == std::string::npos)
		return false;

	++pos;
	while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])))
		++pos;

	char* end = nullptr;
	double value = strtod(json.c_str() + pos, &end);
	if (end == json.c_str() + pos)
		return false;

	out = value;
	return true;
}

static bool JsonBoolLocal(const std::string& json, const std::string& key, bool& out) {
	std::string needle = "\"" + key + "\"";
	size_t pos = json.find(needle);
	if (pos == std::string::npos)
		return false;

	pos = json.find(':', pos + needle.size());
	if (pos == std::string::npos)
		return false;

	++pos;
	while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])))
		++pos;

	if (json.compare(pos, 4, "true") == 0) {
		out = true;
		return true;
	}

	if (json.compare(pos, 5, "false") == 0) {
		out = false;
		return true;
	}

	return false;
}

static int JsonObjectCountInArrayLocal(const std::string& json, const std::string& key) {
	std::string needle = "\"" + key + "\"";
	size_t pos = json.find(needle);
	if (pos == std::string::npos)
		return 0;

	pos = json.find('[', pos + needle.size());
	if (pos == std::string::npos)
		return 0;

	size_t end = json.find(']', pos);
	if (end == std::string::npos || end <= pos)
		return 0;

	int count = 0;
	for (size_t item = json.find('{', pos); item != std::string::npos && item < end; item = json.find('{', item + 1))
		++count;

	return count;
}

static bool RcloneRCGetStats(RcloneRCStats& stats) {
	std::string response;
	if (!RcloneRCPost("/core/stats", response))
		return false;

	double value = 0;
	if (JsonNumberLocal(response, "bytes", value) && value > 0)
		stats.bytes = static_cast<uint64_t>(value);
	else
		stats.bytes = 0;

	if (JsonNumberLocal(response, "speed", value) && value > 0)
		stats.speed = value;
	else
		stats.speed = 0;

	if (JsonNumberLocal(response, "transferring", value) && value > 0)
		stats.transfers = static_cast<int>(value);
	else if (JsonNumberLocal(response, "transfers", value) && value > 0)
		stats.transfers = static_cast<int>(value);
	else
		stats.transfers = 0;

	stats.active_transfers = JsonObjectCountInArrayLocal(response, "transferring");

	if (JsonNumberLocal(response, "checking", value) && value > 0)
		stats.checks = static_cast<int>(value);
	else if (JsonNumberLocal(response, "checks", value) && value > 0)
		stats.checks = static_cast<int>(value);
	else
		stats.checks = 0;

	if (JsonNumberLocal(response, "errors", value) && value > 0)
		stats.errors = static_cast<int>(value);
	else
		stats.errors = 0;

	JsonBoolLocal(response, "fatalError", stats.fatal_error);
	JsonBoolLocal(response, "retryError", stats.retry_error);

	return true;
}

static bool RcloneRCResetStatsLocal() {
	std::string response;
	return RcloneRCPost("/core/stats-reset", response);
}

static bool WaitForCacheToDrain(const std::string& path, int max_seconds, bool update_progress, uint64_t expected_upload_bytes) {
	if (path.empty())
		return true;

	uint64_t initial_cache_bytes = GetDirectoryBytes(path);
	bool have_rc_stats = false;

	if (update_progress) {
		DataManager::SetValue("tw_partition", "NAS upload");
		DataManager::SetValue("tw_file_progress", "");
		DataManager::SetProgress(initial_cache_bytes == 0 ? 1.0f : 0.0f);
	}

	for (int i = 0; max_seconds <= 0 || i <= max_seconds; ++i) {
		uint64_t cache_bytes = GetDirectoryBytes(path);
		have_rc_stats = false;
		RcloneRCStats stats;

		if (update_progress && RcloneRCGetStats(stats))
			have_rc_stats = true;

		if (cache_bytes == 0) {
			if (update_progress) {
				DataManager::SetValue("tw_size_progress", "NAS upload complete");
				DataManager::SetProgress(1.0f);
			}

			return true;
		}

		std::stringstream status;
		if (have_rc_stats && expected_upload_bytes > 0) {
			uint64_t remaining_bytes = expected_upload_bytes > stats.bytes ? expected_upload_bytes - stats.bytes : 0;
			status << "Waiting for NAS upload to finish: "
			       << static_cast<unsigned long long>(remaining_bytes / MB)
			       << " MB remaining";
		} else {
			status << "Waiting for NAS cache upload to finish: "
			       << static_cast<unsigned long long>(cache_bytes / MB)
			       << " MB remaining";
		}

		DataManager::SetValue(TW_NAS_STATUS_TEXT, status.str());

		if (update_progress) {
			uint64_t uploaded_bytes = 0;
			uint64_t total_bytes = expected_upload_bytes > 0 ? expected_upload_bytes : initial_cache_bytes;

			if (have_rc_stats && stats.bytes > 0)
				uploaded_bytes = std::min(stats.bytes, total_bytes);
			else if (total_bytes > 0)
				uploaded_bytes = total_bytes > cache_bytes ? total_bytes - cache_bytes : 0;

			int percent = 0;
			if (total_bytes > 0)
				percent = static_cast<int>((std::min(uploaded_bytes, total_bytes) * 100ULL) / total_bytes);

			if (percent > 100)
				percent = 100;

			std::stringstream progress_text;
			progress_text << "NAS upload: "
				      << static_cast<unsigned long long>(uploaded_bytes / MB)
				      << " MB of "
				      << static_cast<unsigned long long>(total_bytes / MB)
				      << " MB ("
				      << percent
				      << "%)";

			if (have_rc_stats && stats.speed > 0)
				progress_text << " @ " << static_cast<unsigned long long>(stats.speed / MB) << " MB/s";

			DataManager::SetValue("tw_size_progress", progress_text.str());
			DataManager::SetProgress(static_cast<float>(percent) / 100.0f);
		}

		LOGINFO("NAS: %s\n", status.str().c_str());

		if (have_rc_stats) {
			if (stats.errors > 0 || stats.fatal_error || stats.retry_error)
				return false;

			if (stats.active_transfers == 0 && stats.checks == 0 &&
				(expected_upload_bytes == 0 || stats.bytes >= expected_upload_bytes)) {
				if (update_progress) {
					DataManager::SetValue("tw_size_progress", "NAS upload complete");
					DataManager::SetProgress(1.0f);
				}

				return true;
			}
		}

		sleep(1);
	}

	return GetDirectoryBytes(path) == 0;
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

bool NasManager::RunDetached(const std::vector<std::string>& args) {
	if (args.empty()) {
		SetError("RunDetached called with empty argv");
		return false;
	}

	pid_t pid = fork();
	if (pid < 0) {
		std::stringstream ss;
		ss << "fork() failed: " << strerror(errno);
		SetError(ss.str());
		return false;
	}

	if (pid == 0) {
		setsid();

		int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}

		std::vector<char*> argv;
		argv.reserve(args.size() + 1);

		for (const std::string& arg : args)
			argv.push_back(const_cast<char*>(arg.c_str()));

		argv.push_back(nullptr);

		execv(argv[0], argv.data());
		_exit(127);
	}

	LOGINFO("NAS: started detached rclone pid %d\n", pid);
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

	if (!EnsureDirectory(Mount_Point, 0777))
		return false;

	std::string cache_mode = GetConfiguredCacheMode();

	LOGINFO("NAS: selected cache mode: %s\n", cache_mode.c_str());

	if (cache_mode == Nas_Cache_Mode_Data) {
		if (!TWFunc::Path_Exists("/data/media/0")) {
			SetError("NAS data cache selected, but /data/media/0 is not available.");
			return false;
		}

		if (!EnsureDirectory(Data_Cache_Dir, 0777))
			return false;

		uint64_t data_free = GetFreeBytesForPath("/data/media/0");
		LOGINFO("NAS: /data/media/0 free bytes before NAS mount: %llu\n",
			static_cast<unsigned long long>(data_free));

		RemoveTreeContents(Data_Cache_Dir);
	} else {
		RemoveTreeContents(Cache_Dir);
		RemoveTreeContents(Data_Cache_Dir);
	}

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
		"--umask", "000"
	};

	if (cache_mode == Nas_Cache_Mode_Data) {
		args.push_back("--vfs-cache-mode");
		args.push_back("writes");
		args.push_back("--cache-dir");
		args.push_back(Data_Cache_Dir);
		args.push_back("--vfs-cache-max-size");
		args.push_back(Data_Cache_Max_Size);
		args.push_back("--vfs-cache-min-free-space");
		args.push_back("1G");
		args.push_back("--vfs-write-back");
		args.push_back("1s");
		args.push_back("--vfs-cache-max-age");
		args.push_back("30m");
		args.push_back("--vfs-cache-poll-interval");
		args.push_back("10s");
	} else {
		args.push_back("--vfs-cache-mode");
		args.push_back("off");
	}

	args.push_back("--dir-cache-time");
	args.push_back("5m");
	args.push_back("--attr-timeout");
	args.push_back("1s");
	args.push_back("--buffer-size");
	args.push_back("64M");
	args.push_back("--log-file");
	args.push_back(Log_File);
	args.push_back("--log-level");
	args.push_back("INFO");
	args.push_back("--rc");
	args.push_back("--rc-no-auth");
	args.push_back("--rc-addr");
	args.push_back(Rclone_RC_Addr);

	if (!RunDetached(args)) {
		std::string log;
		if (ReadFile(Log_File, log) && !log.empty())
			SetError(GetLastError() + "\n" + Trim(log));

		return false;
	}

	for (int i = 0; i < 30; ++i) {
		if (IsMounted()) {
			DataManager::SetValue(TW_NAS_MOUNTED, 1);

			if (cache_mode == Nas_Cache_Mode_Data)
				DataManager::SetValue(TW_NAS_STATUS_TEXT, "NAS mounted with data cache");
			else
				DataManager::SetValue(TW_NAS_STATUS_TEXT, "NAS mounted without cache");

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

	std::string cache_mode = GetConfiguredCacheMode();

	if (!IsMounted()) {
		DataManager::SetValue(TW_NAS_MOUNTED, 0);
		DataManager::SetValue(TW_NAS_STATUS_TEXT, "NAS is not mounted");

		LOGINFO("NAS: cleaning rclone tmp cache: %s\n", Cache_Dir.c_str());
		RemoveTreeContents(Cache_Dir);

		LOGINFO("NAS: cleaning rclone data cache: %s\n", Data_Cache_Dir.c_str());
		RemoveTreeContents(Data_Cache_Dir);

		return true;
	}

	sync();

	if (cache_mode == Nas_Cache_Mode_Data) {
		DataManager::SetValue(TW_NAS_STATUS_TEXT, "Waiting for NAS cache upload to finish");

		if (!WaitForCacheToDrain(Data_Cache_Dir, 1800, true, 0)) {
			SetError("NAS data cache still contains files. Upload may still be running. Not unmounting to avoid data loss.");
			return false;
		}
	}

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

	LOGINFO("NAS: cleaning rclone tmp cache: %s\n", Cache_Dir.c_str());
	RemoveTreeContents(Cache_Dir);

	LOGINFO("NAS: cleaning rclone data cache: %s\n", Data_Cache_Dir.c_str());
	RemoveTreeContents(Data_Cache_Dir);

	DataManager::SetValue(TW_NAS_MOUNTED, 0);
	DataManager::SetValue(TW_NAS_STATUS_TEXT, "NAS unmounted");
	return true;
}

bool NasManager::ResetTransferStats() {
	last_error.clear();
	DataManager::SetValue(TW_NAS_LAST_ERROR, "");

	if (!IsMounted())
		return true;

	if (GetConfiguredCacheMode() != Nas_Cache_Mode_Data)
		return true;

	if (!RcloneRCResetStatsLocal()) {
		LOGINFO("NAS: rclone RC stats reset unavailable; upload progress will fall back if needed\n");
		return false;
	}

	return true;
}

bool NasManager::WaitForPendingUploads(int max_seconds, uint64_t expected_upload_bytes) {
	last_error.clear();
	DataManager::SetValue(TW_NAS_LAST_ERROR, "");

	if (!IsMounted())
		return true;

	std::string cache_mode = GetConfiguredCacheMode();
	if (cache_mode != Nas_Cache_Mode_Data)
		return true;

	DataManager::SetValue(TW_NAS_STATUS_TEXT, "Waiting for NAS cache upload to finish");

	if (!WaitForCacheToDrain(Data_Cache_Dir, max_seconds, true, expected_upload_bytes)) {
		SetError("NAS data cache still contains files. Upload may still be running.");
		return false;
	}

	LOGINFO("NAS: cleaning completed data cache: %s\n", Data_Cache_Dir.c_str());
	RemoveTreeContents(Data_Cache_Dir);

	DataManager::SetValue(TW_NAS_STATUS_TEXT, "NAS cache upload finished");
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

	if (mounted) {
		std::string cache_mode = GetConfiguredCacheMode();

		if (cache_mode == Nas_Cache_Mode_Data)
			DataManager::SetValue(TW_NAS_STATUS_TEXT, "NAS mounted at " + Mount_Point + " using data cache");
		else
			DataManager::SetValue(TW_NAS_STATUS_TEXT, "NAS mounted at " + Mount_Point + " without cache");
	} else {
		DataManager::SetValue(TW_NAS_STATUS_TEXT, "NAS not mounted");
	}

	return mounted;
}
