#pragma once

#include <filesystem>

namespace UpdateInstaller {

bool install(const std::filesystem::path& downloadedUpdateFilePath);

} // namespace UpdateInstaller
