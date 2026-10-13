#include "fem/shell/menu_order.h"

#include <charconv>
#include <system_error>
#include <tuple>

namespace fem::shell {

MenuGroup parse_menu_group(std::string_view group) {
    const std::size_t at = group.rfind('@');
    if (at != std::string_view::npos) {
        const std::string_view number = group.substr(at + 1);
        int order = 0;
        const char* const end = number.data() + number.size();
        const auto [stop, error] = std::from_chars(number.data(), end, order);
        if (!number.empty() && error == std::errc() && stop == end) {
            return {.name = std::string(group.substr(0, at)), .order = order};
        }
    }
    return {.name = std::string(group)};
}

bool menu_item_before(const MenuGroup& a, const QString& title_a, const MenuGroup& b,
                      const QString& title_b) {
    // Items without a group go after every group.
    const bool a_last = a.name.empty();
    const bool b_last = b.name.empty();
    const int titles = QString::localeAwareCompare(title_a, title_b);
    return std::tie(a_last, a.name, a.order) < std::tie(b_last, b.name, b.order) ||
           (std::tie(a_last, a.name, a.order) == std::tie(b_last, b.name, b.order) && titles < 0);
}

}  // namespace fem::shell
