#include "updateinstaller.hpp"

#include <Windows.h>
#include <shellapi.h>

bool UpdateInstaller::install(const std::filesystem::path& downloadedUpdateFilePath)
{
	const HINSTANCE result = ShellExecuteW(
		nullptr,
		L"open",
		downloadedUpdateFilePath.c_str(),
		nullptr,
		nullptr,
		SW_SHOWNORMAL);

	return reinterpret_cast<INT_PTR>(result) > 32;
}
