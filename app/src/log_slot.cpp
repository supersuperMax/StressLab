#include "log_slot.h"

#include "fem/core/product.h"

#include <QString>
#include <algorithm>
#include <cctype>
#include <system_error>

namespace fem::app {

std::optional<LogSlot> take_log_slot(const std::filesystem::path& folder, int max_slots) {
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
        return std::nullopt;
    }
    std::string stem(kProductName);
    std::ranges::transform(stem, stem.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (int slot = 1; slot <= max_slots; ++slot) {
        const std::string name =
            slot == 1 ? stem + ".log" : stem + "-" + std::to_string(slot) + ".log";
        const std::filesystem::path lock_path = folder / (name + ".lock");
        auto lock =
            std::make_unique<QLockFile>(QString::fromStdU16String(lock_path.generic_u16string()));
        if (lock->tryLock(0)) {
            return LogSlot{.file_name = name, .lock = std::move(lock)};
        }
    }
    return std::nullopt;
}

}  // namespace fem::app
