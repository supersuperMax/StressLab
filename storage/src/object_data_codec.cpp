#include "fem/storage/object_data_codec.h"

#include "fem/core/model/array_data.h"
#include "fem/core/model/element_type.h"
#include "fem/core/model/error.h"
#include "fem/core/model/mesh.h"
#include "fem/storage/error.h"

#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace fem::storage {

namespace {

constexpr std::array<char, 8> magic = {'S', 'L', 'D', 'A', 'T', 'A', '\0', '\0'};

/// `bytes` (task 2.5): one array of bytes, a file of a pinned result, not the data of an object.
/// `arrays` (task 3.1): the named arrays of an object of a module (`model::ArrayData`).
enum class Kind : std::uint32_t {
    mesh = 1,
    node_set = 2,
    element_set = 3,
    surface = 4,
    bytes = 5,
    arrays = 6,
};

/// The type of the items of an array of `Kind::arrays` as the table writes it.
enum class ItemType : std::uint32_t { int32 = 1, int64 = 2, float64 = 3 };

ItemType item_type(model::ScalarType type) noexcept {
    switch (type) {
        case model::ScalarType::int32:
            return ItemType::int32;
        case model::ScalarType::int64:
            return ItemType::int64;
        case model::ScalarType::float64:
            break;
    }
    return ItemType::float64;
}

std::size_t item_size(ItemType type) noexcept {
    return type == ItemType::int32 ? 4 : 8;
}

/// How much of a stream `put_byte_data` reads at a time: many pieces, few calls.
constexpr std::size_t stream_buffer_bytes = 4 * 1024 * 1024;

/// How an array is kept in the table of contents.
enum class Storage : std::uint32_t { inline_items = 0, pieces = 1 };

constexpr std::size_t digest_size = 32;
constexpr std::string_view ref_prefix = "sha256:";

[[noreturn]] void throw_corrupt(const std::string& message) {
    throw ProjectError(ProjectErrc::corrupt_blob, "bulk data block: " + message);
}

/// Unsigned integer of the same size as T, for writing T byte by byte.
template <class T>
using Bits = std::conditional_t<sizeof(T) == 8, std::uint64_t, std::uint32_t>;

/// The items of `values` as little-endian bytes: the memory of `values` itself on a little-endian
/// computer, otherwise a copy in `copy`.
template <class T>
std::span<const std::byte> little_endian(std::span<const T> values, Bytes& copy) {
    static_assert(sizeof(T) == 4 || sizeof(T) == 8);
    if constexpr (std::endian::native == std::endian::little) {
        return std::as_bytes(values);
    } else {
        copy.resize(values.size() * sizeof(T));
        std::size_t at = 0;
        for (const T value : values) {
            const auto bits = std::bit_cast<Bits<T>>(value);
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                copy[at++] = static_cast<std::byte>(bits >> (8 * i));
            }
        }
        return copy;
    }
}

/// Appends the items of little-endian `bytes` (a whole number of items) to `values`.
template <class T>
void append_items(std::vector<T>& values, std::span<const std::byte> bytes) {
    const std::size_t first = values.size();
    values.resize(first + bytes.size() / sizeof(T));
    if constexpr (std::endian::native == std::endian::little) {
        if (!bytes.empty()) {
            std::memcpy(values.data() + first, bytes.data(), bytes.size());
        }
    } else {
        for (std::size_t item = 0; item < bytes.size() / sizeof(T); ++item) {
            Bits<T> bits = 0;
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                bits |=
                    static_cast<Bits<T>>(std::to_integer<std::uint8_t>(bytes[item * sizeof(T) + i]))
                    << (8 * i);
            }
            values[first + item] = std::bit_cast<T>(bits);
        }
    }
}

int hex_value(char c) {
    return c <= '9' ? c - '0' : c - 'a' + 10;
}

/// The 32 bytes of the digest of a reference.
std::array<std::byte, digest_size> digest_of(std::string_view ref) {
    std::array<std::byte, digest_size> digest{};
    const std::string_view hex = ref.substr(ref_prefix.size());
    for (std::size_t i = 0; i < digest_size; ++i) {
        digest[i] = static_cast<std::byte>(hex_value(hex[2 * i]) * 16 + hex_value(hex[2 * i + 1]));
    }
    return digest;
}

std::string ref_of(std::span<const std::byte> digest) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string ref(ref_prefix);
    for (const std::byte value : digest) {
        const auto bits = std::to_integer<std::uint8_t>(value);
        ref += digits[bits >> 4];
        ref += digits[bits & 0xf];
    }
    return ref;
}

class Writer {
public:
    template <class T>
    void scalar(T value) {
        static_assert(sizeof(T) == 4 || sizeof(T) == 8);
        const auto bits = std::bit_cast<Bits<T>>(value);
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            bytes_.push_back(static_cast<std::byte>(bits >> (8 * i)));
        }
    }

    void raw(std::span<const std::byte> value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void text(std::string_view value) {
        scalar(static_cast<std::uint32_t>(value.size()));
        raw(std::as_bytes(std::span(value)));
    }

    [[nodiscard]] Bytes take() { return std::move(bytes_); }

private:
    Bytes bytes_;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    template <class T>
    [[nodiscard]] T scalar() {
        static_assert(sizeof(T) == 4 || sizeof(T) == 8);
        Bits<T> bits = 0;
        const std::span<const std::byte> item = take(sizeof(T));
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            bits |= static_cast<Bits<T>>(std::to_integer<std::uint8_t>(item[i])) << (8 * i);
        }
        return std::bit_cast<T>(bits);
    }

    /// A count of items of at least `item_size` bytes each that follow; refused if they do not fit
    /// the block, so a damaged count cannot make a huge allocation.
    [[nodiscard]] std::size_t count(std::size_t item_size) {
        return fitting(scalar<std::uint64_t>(), item_size);
    }

    /// `count`, refused unless that many items of `item_size` bytes fit the rest of the block.
    [[nodiscard]] std::size_t fitting(std::uint64_t count, std::size_t item_size) const {
        if (count > remaining() / item_size) {
            throw_corrupt("an array is longer than the block");
        }
        return static_cast<std::size_t>(count);
    }

    [[nodiscard]] std::span<const std::byte> take(std::size_t size) {
        if (size > remaining()) {
            throw_corrupt("the block is truncated");
        }
        const std::span<const std::byte> result = bytes_.subspan(position_, size);
        position_ += size;
        return result;
    }

    [[nodiscard]] std::string text() {
        const std::span<const std::byte> value = take(scalar<std::uint32_t>());
        return {reinterpret_cast<const char*>(value.data()), value.size()};
    }

    [[nodiscard]] bool starts_with_magic() {
        if (remaining() < magic.size() ||
            std::memcmp(bytes_.data(), magic.data(), magic.size()) != 0) {
            return false;
        }
        position_ += magic.size();
        return true;
    }

    void finish() const {
        if (remaining() != 0) {
            throw_corrupt("bytes left after the data");
        }
    }

private:
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - position_; }

    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
};

// --- Writing -------------------------------------------------------------------------------------

class TableWriter {
public:
    TableWriter(Kind kind, BlobStore& blobs, const ChunkLimits& limits)
        : blobs_(blobs), limits_(limits) {
        out_.raw(std::as_bytes(std::span(magic)));
        out_.scalar(object_data_codec_version);
        out_.scalar(static_cast<std::uint32_t>(kind));
    }

    template <class T>
    void array(std::span<const T> values) {
        Bytes copy;
        const std::span<const std::byte> bytes = little_endian(values, copy);
        out_.scalar(static_cast<std::uint64_t>(values.size()));
        if (bytes.size() <= inline_array_bytes) {
            out_.scalar(static_cast<std::uint32_t>(Storage::inline_items));
            out_.raw(bytes);
            return;
        }
        out_.scalar(static_cast<std::uint32_t>(Storage::pieces));
        const std::vector<std::size_t> ends = chunk_ends(bytes, sizeof(T), limits_);
        out_.scalar(static_cast<std::uint64_t>(ends.size()));
        std::size_t start = 0;
        for (const std::size_t end : ends) {
            const std::span<const std::byte> piece = bytes.subspan(start, end - start);
            const std::string ref = blobs_.put(shuffle_bytes(piece, sizeof(T)));
            out_.raw(digest_of(ref));
            out_.scalar(static_cast<std::uint64_t>(piece.size() / sizeof(T)));
            start = end;
        }
    }

    void count(std::size_t value) { out_.scalar(static_cast<std::uint64_t>(value)); }
    void scalar(std::uint32_t value) { out_.scalar(value); }
    void text(std::string_view value) { out_.text(value); }

    /// Stores the table and returns its reference.
    std::string finish() { return blobs_.put(out_.take()); }

private:
    Writer out_;
    BlobStore& blobs_;
    const ChunkLimits& limits_;
};

// --- Reading -------------------------------------------------------------------------------------

struct Piece {
    std::string ref;
    std::size_t items = 0;
};

/// An array as the table of contents gives it: inline items or pieces.
struct ArrayEntry {
    std::size_t item_size = 0;
    std::uint64_t count = 0;
    std::span<const std::byte> items;  ///< Inline items, little-endian.
    std::vector<Piece> pieces;
};

/// A table of contents read but not resolved: its arrays in the order of the encoding.
struct Table {
    Kind kind{};
    std::vector<ArrayEntry> arrays;
    std::vector<model::ElementType> block_types;  ///< For a mesh: the type of each element block.
    /// For named arrays: the name, type and width of each array, in the order of `arrays`.
    struct Named {
        std::string name;
        ItemType type{};
        std::size_t width = 1;
    };
    std::vector<Named> named;
};

ArrayEntry read_array(Reader& in, std::size_t item_size) {
    ArrayEntry entry;
    entry.item_size = item_size;
    entry.count = in.scalar<std::uint64_t>();
    const auto storage = static_cast<Storage>(in.scalar<std::uint32_t>());
    if (storage == Storage::inline_items) {
        entry.items = in.take(in.fitting(entry.count, item_size) * item_size);
        return entry;
    }
    if (storage != Storage::pieces) {
        throw_corrupt("unknown storage of an array " +
                      std::to_string(static_cast<std::uint32_t>(storage)));
    }
    const std::size_t pieces = in.count(digest_size + 8);
    entry.pieces.reserve(pieces);
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < pieces; ++i) {
        Piece piece;
        piece.ref = ref_of(in.take(digest_size));
        const auto items = in.scalar<std::uint64_t>();
        if (items == 0 || items > entry.count - total) {
            throw_corrupt("the pieces of an array do not add up to its size");
        }
        total += items;
        piece.items = static_cast<std::size_t>(items);
        entry.pieces.push_back(std::move(piece));
    }
    if (total != entry.count) {
        throw_corrupt("the pieces of an array do not add up to its size");
    }
    return entry;
}

Table read_table(std::span<const std::byte> bytes) {
    Reader in(bytes);
    if (!in.starts_with_magic()) {
        throw_corrupt("not a block of object data");
    }
    if (const auto version = in.scalar<std::uint32_t>(); version != object_data_codec_version) {
        throw_corrupt("unknown encoding version " + std::to_string(version));
    }
    Table table;
    table.kind = static_cast<Kind>(in.scalar<std::uint32_t>());
    switch (table.kind) {
        case Kind::mesh: {
            table.arrays.push_back(read_array(in, sizeof(model::Label)));
            for (int axis = 0; axis < 3; ++axis) {
                table.arrays.push_back(read_array(in, sizeof(double)));
            }
            // A block takes at least its type name length and two empty arrays.
            const std::size_t blocks = in.count(4 + 2 * (8 + 4));
            for (std::size_t b = 0; b < blocks; ++b) {
                const std::string name = in.text();
                const std::optional<model::ElementType> type = model::parse_element_type(name);
                if (!type) {
                    throw_corrupt("unknown element type '" + name + "'");
                }
                table.block_types.push_back(*type);
                table.arrays.push_back(read_array(in, sizeof(model::Label)));
                table.arrays.push_back(read_array(in, sizeof(model::Index)));
            }
            break;
        }
        case Kind::node_set:
        case Kind::element_set:
        case Kind::surface:
            table.arrays.push_back(read_array(in, sizeof(model::Index)));
            break;
        case Kind::bytes:
            table.arrays.push_back(read_array(in, 1));
            break;
        case Kind::arrays: {
            // An array takes at least its name length, type, width and an empty array.
            const std::size_t arrays = in.count(4 + 4 + 4 + 8 + 4);
            for (std::size_t a = 0; a < arrays; ++a) {
                Table::Named named;
                named.name = in.text();
                named.type = static_cast<ItemType>(in.scalar<std::uint32_t>());
                if (named.type != ItemType::int32 && named.type != ItemType::int64 &&
                    named.type != ItemType::float64) {
                    throw_corrupt("unknown type of the items of the array '" + named.name + "'");
                }
                named.width = in.scalar<std::uint32_t>();
                table.arrays.push_back(read_array(in, item_size(named.type)));
                table.named.push_back(std::move(named));
            }
            break;
        }
        default:
            throw_corrupt("unknown kind of data " +
                          std::to_string(static_cast<std::uint32_t>(table.kind)));
    }
    in.finish();
    return table;
}

/// The items of an array, its pieces read from `blobs`.
template <class T>
std::vector<T> resolve(const ArrayEntry& entry, const BlobStore& blobs) {
    std::vector<T> values;
    if (entry.pieces.empty()) {
        append_items(values, entry.items);
        return values;
    }
    // No reserve from the count of the table: it is trusted only as far as the pieces go.
    for (const Piece& piece : entry.pieces) {
        const Bytes bytes = blobs.get(piece.ref);
        if (bytes.size() % sizeof(T) != 0 || bytes.size() / sizeof(T) != piece.items) {
            throw_corrupt("the piece " + piece.ref + " has another size than its table says");
        }
        append_items(values, unshuffle_bytes(bytes, sizeof(T)));
    }
    return values;
}

std::shared_ptr<const model::ObjectData> make_data(const Table& table, const BlobStore& blobs) {
    switch (table.kind) {
        case Kind::mesh: {
            model::NodeArrays nodes;
            nodes.ids = resolve<model::Label>(table.arrays[0], blobs);
            nodes.x = resolve<double>(table.arrays[1], blobs);
            nodes.y = resolve<double>(table.arrays[2], blobs);
            nodes.z = resolve<double>(table.arrays[3], blobs);
            std::vector<model::ElementBlock> blocks;
            blocks.reserve(table.block_types.size());
            for (std::size_t b = 0; b < table.block_types.size(); ++b) {
                blocks.emplace_back(table.block_types[b],
                                    resolve<model::Label>(table.arrays[4 + 2 * b], blobs),
                                    resolve<model::Index>(table.arrays[5 + 2 * b], blobs));
            }
            return std::make_shared<const model::Mesh>(std::move(nodes), std::move(blocks));
        }
        case Kind::node_set:
            return std::make_shared<const model::NodeSet>(
                resolve<model::Index>(table.arrays[0], blobs));
        case Kind::element_set:
            return std::make_shared<const model::ElementSet>(
                resolve<model::Index>(table.arrays[0], blobs));
        case Kind::surface: {
            const std::vector<model::Index> flat = resolve<model::Index>(table.arrays[0], blobs);
            if (flat.size() % 2 != 0) {
                throw_corrupt("a surface needs an element and a side for each of its sides");
            }
            std::vector<model::ElementSide> sides(flat.size() / 2);
            for (std::size_t i = 0; i < sides.size(); ++i) {
                sides[i] = {flat[2 * i], flat[2 * i + 1]};
            }
            return std::make_shared<const model::Surface>(std::move(sides));
        }
        case Kind::arrays: {
            std::vector<model::NamedArray> arrays;
            arrays.reserve(table.named.size());
            for (std::size_t a = 0; a < table.named.size(); ++a) {
                const Table::Named& named = table.named[a];
                const ArrayEntry& entry = table.arrays[a];
                model::NamedArray::Items items;
                switch (named.type) {
                    case ItemType::int32:
                        items = resolve<std::int32_t>(entry, blobs);
                        break;
                    case ItemType::int64:
                        items = resolve<std::int64_t>(entry, blobs);
                        break;
                    case ItemType::float64:
                        items = resolve<double>(entry, blobs);
                        break;
                }
                arrays.emplace_back(named.name, std::move(items), named.width);
            }
            // The order of the table is that of the data: by name, each name once.
            auto data = std::make_shared<const model::ArrayData>(std::move(arrays));
            for (std::size_t a = 0; a < table.named.size(); ++a) {
                if (data->arrays()[a].name() != table.named[a].name) {
                    throw_corrupt("the arrays are not in the order of their names");
                }
            }
            return data;
        }
        case Kind::bytes:
            throw_corrupt("the block holds the bytes of a file, not the data of an object");
    }
    throw_corrupt("unknown kind of data");
}

}  // namespace

std::string put_object_data(const model::ObjectData& data, BlobStore& blobs,
                            const ChunkLimits& limits) {
    if (const auto* mesh = dynamic_cast<const model::Mesh*>(&data)) {
        TableWriter out(Kind::mesh, blobs, limits);
        out.array(mesh->node_ids());
        out.array(mesh->x());
        out.array(mesh->y());
        out.array(mesh->z());
        out.count(mesh->blocks().size());
        for (const model::ElementBlock& block : mesh->blocks()) {
            out.text(model::to_string(block.type()));
            out.array(block.ids());
            out.array(block.connectivity());
        }
        return out.finish();
    }
    if (const auto* nodes = dynamic_cast<const model::NodeSet*>(&data)) {
        TableWriter out(Kind::node_set, blobs, limits);
        out.array(nodes->nodes());
        return out.finish();
    }
    if (const auto* elements = dynamic_cast<const model::ElementSet*>(&data)) {
        TableWriter out(Kind::element_set, blobs, limits);
        out.array(elements->elements());
        return out.finish();
    }
    if (const auto* surface = dynamic_cast<const model::Surface*>(&data)) {
        std::vector<model::Index> flat;
        flat.reserve(2 * surface->sides().size());
        for (const model::ElementSide& side : surface->sides()) {
            flat.push_back(side.element);
            flat.push_back(side.side);
        }
        TableWriter out(Kind::surface, blobs, limits);
        out.array(std::span<const model::Index>(flat));
        return out.finish();
    }
    if (const auto* named = dynamic_cast<const model::ArrayData*>(&data)) {
        TableWriter out(Kind::arrays, blobs, limits);
        out.count(named->arrays().size());
        for (const model::NamedArray& array : named->arrays()) {
            out.text(array.name());
            out.scalar(static_cast<std::uint32_t>(item_type(array.type())));
            if (array.width() > std::numeric_limits<std::uint32_t>::max()) {
                throw ProjectError(ProjectErrc::invalid_data,
                                   "the rows of the array '" + array.name() + "' are too wide");
            }
            out.scalar(static_cast<std::uint32_t>(array.width()));
            std::visit([&](const auto& items) { out.array(std::span(items)); }, array.items());
        }
        return out.finish();
    }
    throw ProjectError(ProjectErrc::invalid_data, "this kind of object data cannot be saved");
}

std::shared_ptr<const model::ObjectData> get_object_data(std::string_view ref,
                                                         const BlobStore& blobs) {
    const Bytes bytes = blobs.get(ref);
    const Table table = read_table(bytes);
    try {
        return make_data(table, blobs);
    } catch (const model::ModelError& error) {
        throw_corrupt(error.what());
    }
}

std::vector<std::string> object_data_pieces(std::span<const std::byte> table) {
    std::vector<std::string> pieces;
    for (const ArrayEntry& entry : read_table(table).arrays) {
        for (const Piece& piece : entry.pieces) {
            pieces.push_back(piece.ref);
        }
    }
    return pieces;
}

std::string put_byte_data(const ByteSource& read, BlobStore& blobs, const ChunkLimits& limits) {
    // The bytes read and not stored yet start at a cut (or at the start), and a cut depends only
    // on the bytes since the cut before it: cutting the stream window by window gives the pieces
    // that cutting it whole would give. Of the pieces of a window all but the last are final; the
    // last waits for more bytes, unless the stream has ended.
    struct StoredPiece {
        std::array<std::byte, digest_size> digest;
        std::uint64_t size;
    };
    std::vector<StoredPiece> pieces;
    std::uint64_t total = 0;
    Bytes window;
    bool ended = false;
    while (!ended) {
        while (!ended && window.size() < stream_buffer_bytes) {
            const std::size_t start = window.size();
            window.resize(stream_buffer_bytes);
            const std::size_t got = read(std::span(window).subspan(start));
            window.resize(start + std::min(got, stream_buffer_bytes - start));
            ended = got == 0;
        }
        if (ended && pieces.empty() && window.size() <= inline_array_bytes) {
            break;
        }
        const std::vector<std::size_t> ends = chunk_ends(window, 1, limits);
        const std::size_t final_pieces = ended ? ends.size() : ends.size() - 1;
        std::size_t begin = 0;
        for (std::size_t i = 0; i < final_pieces; ++i) {
            const std::span<const std::byte> piece =
                std::span<const std::byte>(window).subspan(begin, ends[i] - begin);
            pieces.push_back({digest_of(blobs.put(piece)), piece.size()});
            total += piece.size();
            begin = ends[i];
        }
        window.erase(window.begin(), window.begin() + static_cast<std::ptrdiff_t>(begin));
        if (ended) {
            window.clear();
        }
    }

    Writer out;
    out.raw(std::as_bytes(std::span(magic)));
    out.scalar(object_data_codec_version);
    out.scalar(static_cast<std::uint32_t>(Kind::bytes));
    if (pieces.empty()) {
        out.scalar(static_cast<std::uint64_t>(window.size()));
        out.scalar(static_cast<std::uint32_t>(Storage::inline_items));
        out.raw(window);
    } else {
        out.scalar(total);
        out.scalar(static_cast<std::uint32_t>(Storage::pieces));
        out.scalar(static_cast<std::uint64_t>(pieces.size()));
        for (const StoredPiece& piece : pieces) {
            out.raw(piece.digest);
            out.scalar(piece.size);
        }
    }
    return blobs.put(out.take());
}

std::uint64_t get_byte_data(std::string_view ref, const BlobStore& blobs, const ByteSink& write) {
    const Bytes bytes = blobs.get(ref);
    const Table table = read_table(bytes);
    if (table.kind != Kind::bytes) {
        throw_corrupt("the block " + std::string(ref) + " is the data of an object, not a file");
    }
    const ArrayEntry& entry = table.arrays.front();
    if (entry.pieces.empty()) {
        write(entry.items);
        return entry.items.size();
    }
    std::uint64_t total = 0;
    for (const Piece& piece : entry.pieces) {
        const Bytes piece_bytes = blobs.get(piece.ref);
        if (piece_bytes.size() != piece.items) {
            throw_corrupt("the piece " + piece.ref + " has another size than its table says");
        }
        write(piece_bytes);
        total += piece_bytes.size();
    }
    return total;
}

nlohmann::json describe_object_data(const model::ObjectData& data) {
    if (const auto* mesh = dynamic_cast<const model::Mesh*>(&data)) {
        return {
            {"kind", "mesh"}, {"nodes", mesh->node_count()}, {"elements", mesh->element_count()}};
    }
    if (const auto* named = dynamic_cast<const model::ArrayData*>(&data)) {
        nlohmann::json arrays = nlohmann::json::object();
        for (const model::NamedArray& array : named->arrays()) {
            arrays[array.name()] = {{"type", model::to_string(array.type())},
                                    {"width", array.width()}};
        }
        return {{"kind", "arrays"}, {"size", named->rows()}, {"arrays", std::move(arrays)}};
    }
    std::string_view kind;
    if (dynamic_cast<const model::NodeSet*>(&data) != nullptr) {
        kind = "node_set";
    } else if (dynamic_cast<const model::ElementSet*>(&data) != nullptr) {
        kind = "element_set";
    } else if (dynamic_cast<const model::Surface*>(&data) != nullptr) {
        kind = "surface";
    } else {
        throw ProjectError(ProjectErrc::invalid_data, "this kind of object data cannot be saved");
    }
    return {{"kind", kind}, {"size", static_cast<const model::MeshSet&>(data).size()}};
}

}  // namespace fem::storage
