#include "updateinstaller.hpp"

#include <Windows.h>
#include <fstream>
#include <shellapi.h>

namespace {

std::wstring getCurrentModulePath()
{
	std::wstring buffer(MAX_PATH, L'\0');
	DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	if (size == 0)
		return {};

	while (size == buffer.size())
	{
		buffer.resize(buffer.size() * 2, L'\0');
		size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (size == 0)
			return {};
	}

	buffer.resize(size);
	return buffer;
}

std::wstring escapePowerShellSingleQuoted(const std::wstring& value)
{
	std::wstring escaped;
	escaped.reserve(value.size() + 2);
	escaped.push_back(L'\'');
	for (const wchar_t ch : value)
	{
		if (ch == L'\'')
			escaped += L"''";
		else
			escaped.push_back(ch);
	}
	escaped.push_back(L'\'');
	return escaped;
}

bool launchExecutableInstaller(const std::filesystem::path& downloadedUpdateFilePath)
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

bool launchPortableZipUpdater(const std::filesystem::path& downloadedUpdateFilePath)
{
	const std::wstring modulePath = getCurrentModulePath();
	if (modulePath.empty())
		return false;

	const std::filesystem::path targetDirectory = std::filesystem::path(modulePath).parent_path();
	const std::filesystem::path scriptPath = downloadedUpdateFilePath.parent_path() / L"candylauncher_apply_update.ps1";

	const std::wstring zipLiteral = escapePowerShellSingleQuoted(downloadedUpdateFilePath.wstring());
	const std::wstring targetLiteral = escapePowerShellSingleQuoted(targetDirectory.wstring());
	const std::wstring appLiteral = escapePowerShellSingleQuoted(modulePath);

	std::wofstream scriptFile(scriptPath, std::ios::binary | std::ios::trunc);
	if (!scriptFile)
		return false;

	scriptFile << L"$ErrorActionPreference = 'Stop'\n";
	scriptFile << L"$zipPath = " << zipLiteral << L"\n";
	scriptFile << L"$targetDir = " << targetLiteral << L"\n";
	scriptFile << L"$appPath = " << appLiteral << L"\n";
	scriptFile << L"$extractDir = Join-Path ([System.IO.Path]::GetDirectoryName($zipPath)) ([System.IO.Path]::GetFileNameWithoutExtension($zipPath) + '_extract')\n";
	scriptFile << L"Start-Sleep -Seconds 2\n";
	scriptFile << L"for ($i = 0; $i -lt 120; $i++) {\n";
	scriptFile << L"    try {\n";
	scriptFile << L"        $stream = [System.IO.File]::Open($appPath, 'Open', 'ReadWrite', 'None')\n";
	scriptFile << L"        $stream.Close()\n";
	scriptFile << L"        break\n";
	scriptFile << L"    } catch {\n";
	scriptFile << L"        Start-Sleep -Milliseconds 500\n";
	scriptFile << L"    }\n";
	scriptFile << L"}\n";
	scriptFile << L"if (Test-Path -LiteralPath $extractDir) { Remove-Item -LiteralPath $extractDir -Recurse -Force }\n";
	scriptFile << L"Expand-Archive -LiteralPath $zipPath -DestinationPath $extractDir -Force\n";
	scriptFile << L"$sourceDir = $extractDir\n";
	scriptFile << L"$entries = @(Get-ChildItem -LiteralPath $extractDir)\n";
	scriptFile << L"if ($entries.Count -eq 1 -and $entries[0].PSIsContainer -and (Test-Path -LiteralPath (Join-Path $entries[0].FullName 'CandyLauncher.exe'))) {\n";
	scriptFile << L"    $sourceDir = $entries[0].FullName\n";
	scriptFile << L"}\n";
	scriptFile << L"$robocopy = Start-Process -FilePath 'robocopy.exe' -ArgumentList @($sourceDir, $targetDir, '/E', '/R:2', '/W:1', '/NFL', '/NDL', '/NJH', '/NJS', '/NP') -Wait -PassThru -WindowStyle Hidden\n";
	scriptFile << L"if ($robocopy.ExitCode -ge 8) { throw ('Robocopy failed with exit code ' + $robocopy.ExitCode) }\n";
	scriptFile << L"Start-Process -FilePath $appPath -WorkingDirectory $targetDir\n";
	scriptFile.close();
	if (!scriptFile)
		return false;

	const std::wstring parameters = L"-NoProfile -ExecutionPolicy Bypass -File \"" + scriptPath.wstring() + L"\"";
	const HINSTANCE result = ShellExecuteW(
		nullptr,
		L"open",
		L"powershell.exe",
		parameters.c_str(),
		downloadedUpdateFilePath.parent_path().c_str(),
		SW_HIDE);

	return reinterpret_cast<INT_PTR>(result) > 32;
}

} // namespace

bool UpdateInstaller::install(const std::filesystem::path& downloadedUpdateFilePath)
{
	const std::wstring extension = downloadedUpdateFilePath.extension().wstring();
	if (_wcsicmp(extension.c_str(), L".zip") == 0)
		return launchPortableZipUpdater(downloadedUpdateFilePath);

	return launchExecutableInstaller(downloadedUpdateFilePath);
}
