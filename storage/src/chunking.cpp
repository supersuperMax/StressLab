#include "fem/storage/chunking.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

namespace fem::storage {

namespace {

/// 256 random 64-bit numbers, one per byte value, fixed forever: changing them would move every
/// cut, and the pieces of saved meshes would stop matching new ones.
constexpr std::array<std::uint64_t, 256> make_gear() {
    std::array<std::uint64_t, 256> table{};
    std::uint64_t state = 0x5354524553534c42;  // "STRESSLB"
    for (std::uint64_t& value : table) {
        // splitmix64
        state += 0x9e3779b97f4a7c15;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
        z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
        value = z ^ (z >> 31);
    }
    return table;
}

constexpr std::array<std::uint64_t, 256> gear = make_gear();

/// The top `bits` bits. The top bits of the gear hash depend on the last 64 bytes; the low ones
/// only on the last few, so the masks look at the top.
constexpr std::uint64_t top_bits(int bits) {
    return bits <= 0 ? 0 : ~std::uint64_t{0} << (64 - std::min(bits, 64));
}

void roll(std::uint64_t& hash, std::byte value) {
    hash = (hash << 1) + gear[std::to_integer<std::uint8_t>(value)];
}

}  // namespace

std::vector<std::size_t> chunk_ends(std::span<const std::byte> bytes, std::size_t item_size,
                                    const ChunkLimits& limits) {
    const std::size_t item = std::max<std::size_t>(item_size, 1);
    const auto whole_items = [item](std::size_t size) {
        return std::max(item, size / item * item);
    };
    const std::size_t min = whole_items(limits.min);
    const std::size_t max = std::max(min, whole_items(limits.max));
    const std::size_t average = std::clamp(whole_items(limits.average), min, max);

    // A cut is tried at every item boundary past `min`; with N tries expected before `average`,
    // the chance of a cut is about 1/(2N) per try before it and 2/N after it ("normalized
    // chunking" of FastCDC), so pieces gather around `average`.
    const std::size_t tries = std::max<std::size_t>((average - min) / item, 1);
    const int bits = static_cast<int>(std::bit_width(tries)) - 1;
    const std::uint64_t before_average = top_bits(bits + 1);
    const std::uint64_t after_average = top_bits(bits - 1);

    std::vector<std::size_t> ends;
    std::size_t start = 0;
    const std::size_t size = bytes.size();
    while (start < size) {
        const std::size_t remaining = size - start;
        if (remaining <= min) {
            ends.push_back(size);
            break;
        }
        const std::size_t limit = std::min(remaining, max);
        // The hash of the 64 bytes before the first try: the same bytes give the same hash
        // wherever the piece started.
        std::uint64_t hash = 0;
        for (std::size_t i = start + (min > 64 ? min - 64 : 0); i < start + min; ++i) {
            roll(hash, bytes[i]);
        }
        std::size_t length = limit;
        for (std::size_t offset = min; offset < limit; offset += item) {
            if ((hash & (offset < average ? before_average : after_average)) == 0) {
                length = offset;
                break;
            }
            for (std::size_t k = 0; k < item; ++k) {
                roll(hash, bytes[start + offset + k]);
            }
        }
        start += length;
        ends.push_back(start);
    }
    return ends;
}

Bytes shuffle_bytes(std::span<const std::byte> bytes, std::size_t item_size) {
    const std::size_t count = item_size == 0 ? 0 : bytes.size() / item_size;
    Bytes result(bytes.size());
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t b = 0; b < item_size; ++b) {
            result[b * count + i] = bytes[i * item_size + b];
        }
    }
    return result;
}

Bytes unshuffle_bytes(std::span<const std::byte> bytes, std::size_t item_size) {
    const std::size_t count = item_size == 0 ? 0 : bytes.size() / item_size;
    Bytes result(bytes.size());
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t b = 0; b < item_size; ++b) {
            result[i * item_size + b] = bytes[b * count + i];
        }
    }
    return result;
}

}  // namespace fem::storage
