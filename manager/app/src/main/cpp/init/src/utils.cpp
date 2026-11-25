#include <filesystem>

namespace fs = std::filesystem;

bool IsRecoveryMode()
{
	return fs::exists("/system/bin/recovery");
}