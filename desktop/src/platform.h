// Small OS services used by the desktop UI (implemented per platform).
#pragma once

#include <string>
#include <vector>

namespace platform {

// filter: pairs of (description, "*.wav;*.sfz"). Returns UTF-8 paths, empty if cancelled.
std::vector<std::string> openFiles(const std::vector<std::pair<std::string, std::string>>& filter, bool multi);
std::string pickFolder(const std::string& title);
std::string saveFile(const std::vector<std::pair<std::string, std::string>>& filter, const std::string& defaultName,
                     const std::string& defaultExt);
void openInExplorer(const std::string& path);
std::string executableDir();

}  // namespace platform
