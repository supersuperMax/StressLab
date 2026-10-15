#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace fem::storage {

/// SHA-256 (FIPS 180-4), for addressing blocks of bulk data by their contents.
class Sha256 {
public:
    using Digest = std::array<std::uint8_t, 32>;

    Sha256() noexcept;

    void update(std::span<const std::byte> bytes) noexcept;
    /// The digest of everything given so far. The object must not be used afterwards.
    [[nodiscard]] Digest finish() noexcept;

private:
    void compress(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t length_ = 0;  // Bytes given so far.
};

/// The digest of `bytes` as 64 lower-case hex digits.
[[nodiscard]] std::string sha256_hex(std::span<const std::byte> bytes);

}  // namespace fem::storage
