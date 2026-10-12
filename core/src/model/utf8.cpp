#include "utf8.h"

#include <cstddef>
#include <cstdint>

namespace fem::model::detail {

bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<std::uint8_t>(text[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }

        std::size_t length = 0;
        std::uint32_t code_point = 0;
        std::uint32_t min_code_point = 0;
        if ((lead & 0xE0U) == 0xC0U) {
            length = 2;
            code_point = lead & 0x1FU;
            min_code_point = 0x80;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
            code_point = lead & 0x0FU;
            min_code_point = 0x800;
        } else if ((lead & 0xF8U) == 0xF0U) {
            length = 4;
            code_point = lead & 0x07U;
            min_code_point = 0x10000;
        } else {
            return false;
        }
        if (text.size() - i < length) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto continuation = static_cast<std::uint8_t>(text[i + k]);
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3FU);
        }
        const bool surrogate = code_point >= 0xD800 && code_point <= 0xDFFF;
        if (code_point < min_code_point || code_point > 0x10FFFF || surrogate) {
            return false;
        }
        i += length;
    }
    return true;
}

}  // namespace fem::model::detail
