#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>

namespace fem::model {

/// Stable identifier of an object inside a Document.
///
/// Ids are issued by the document in increasing order and are never reused, so a reference to a
/// removed object stays unambiguous. The default-constructed id (value 0) means "no object".
class ObjectId {
public:
    constexpr ObjectId() noexcept = default;
    constexpr explicit ObjectId(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool is_valid() const noexcept { return value_ != 0; }
    constexpr explicit operator bool() const noexcept { return is_valid(); }

    friend constexpr auto operator<=>(ObjectId, ObjectId) noexcept = default;

private:
    std::uint64_t value_ = 0;
};

/// "#42" for a valid id, "#none" otherwise. For logs and error messages, not for the UI.
[[nodiscard]] std::string to_string(ObjectId id);
std::ostream& operator<<(std::ostream& out, ObjectId id);

}  // namespace fem::model

template <>
struct std::hash<fem::model::ObjectId> {
    std::size_t operator()(fem::model::ObjectId id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value());
    }
};
