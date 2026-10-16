#pragma once

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <vector>

namespace fem::storage::detail {

/// Removes the empty folders below `folder` (not `folder` itself), deepest first.
inline void remove_empty_folders(const std::filesystem::path& folder) {
    namespace fs = std::filesystem;
    std::error_code error;
    if (!fs::is_directory(folder, error)) {
        return;
    }
    std::vector<fs::path> folders;
    for (fs::recursive_directory_iterator it(folder, error);
         !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (it->is_directory(error)) {
            folders.push_back(it->path());
        }
    }
    std::sort(folders.begin(), folders.end(), [](const fs::path& a, const fs::path& b) {
        return a.native().size() > b.native().size();
    });
    for (const fs::path& path : folders) {
        if (fs::is_empty(path, error)) {
            fs::remove(path, error);
        }
    }
}

}  // namespace fem::storage::detail
