#include "fem/storage/object_data_codec.h"

#include "fem/core/model/element_type.h"
#include "fem/core/model/error.h"
#include "fem/core/model/mesh.h"
#include "fem/storage/error.h"

#include <array>
#include <bit>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fem::storage {

namespace {

constexpr std::array<char, 8> magic = {'S', 'L', 'D', 'A', 'T', 'A', '\0', '\0'};

enum class Kind : std::uint32_t { mesh = 1, node_set = 2, element_set = 3, surface = 4 };

[[noreturn]] void throw_corrupt(const std::string& message) {
    throw ProjectError(ProjectErrc::corrupt_blob, "bulk data block: " + message);
}

/// Unsigned integer of the same size as T, for writing T byte by byte.
template <class T>
using Bits = std::conditional_t<sizeof(T) == 8, std::uint64_t, std::uint32_t>;

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

    template <class T>
    void array(std::span<const T> values) {
        scalar(static_cast<std::uint64_t>(values.size()));
        bytes_.reserve(bytes_.size() + values.size() * sizeof(T));
        for (const T value : values) {
            scalar(value);
        }
    }

    void text(std::string_view value) {
        scalar(static_cast<std::uint32_t>(value.size()));
        for (const char c : value) {
            bytes_.push_back(static_cast<std::byte>(c));
        }
    }

    void raw(std::span<const char> value) {
        for (const char c : value) {
            bytes_.push_back(static_cast<std::byte>(c));
        }
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
        need(sizeof(T));
        Bits<T> bits = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            bits |= static_cast<Bits<T>>(std::to_integer<std::uint8_t>(bytes_[position_ + i]))
                    << (8 * i);
        }
        position_ += sizeof(T);
        return std::bit_cast<T>(bits);
    }

    /// A count of items of `item_size` bytes that follow; refused if they do not fit the block,
    /// so a corrupt count cannot make a huge allocation.
    [[nodiscard]] std::size_t count(std::size_t item_size) {
        const auto count = scalar<std::uint64_t>();
        if (count > remaining() / item_size) {
            throw_corrupt("an array is longer than the block");
        }
        return static_cast<std::size_t>(count);
    }

    template <class T>
    [[nodiscard]] std::vector<T> array() {
        std::vector<T> values(count(sizeof(T)));
        for (T& value : values) {
            value = scalar<T>();
        }
        return values;
    }

    [[nodiscard]] std::string text() {
        const auto size = scalar<std::uint32_t>();
        need(size);
        std::string value(reinterpret_cast<const char*>(bytes_.data() + position_), size);
        position_ += size;
        return value;
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

    void need(std::size_t size) const {
        if (size > remaining()) {
            throw_corrupt("the block is truncated");
        }
    }

    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
};

void begin(Writer& out, Kind kind) {
    out.raw(magic);
    out.scalar(object_data_codec_version);
    out.scalar(static_cast<std::uint32_t>(kind));
}

void write_mesh(Writer& out, const model::Mesh& mesh) {
    begin(out, Kind::mesh);
    out.array(mesh.node_ids());
    out.array(mesh.x());
    out.array(mesh.y());
    out.array(mesh.z());
    out.scalar(static_cast<std::uint64_t>(mesh.blocks().size()));
    for (const model::ElementBlock& block : mesh.blocks()) {
        out.text(model::to_string(block.type()));
        out.array(block.ids());
        out.array(block.connectivity());
    }
}

std::shared_ptr<const model::ObjectData> read_mesh(Reader& in) {
    model::NodeArrays nodes;
    nodes.ids = in.array<model::Label>();
    nodes.x = in.array<double>();
    nodes.y = in.array<double>();
    nodes.z = in.array<double>();
    // A block takes at least its type name length and two counts.
    const std::size_t block_count = in.count(4 + 8 + 8);
    std::vector<model::ElementBlock> blocks;
    blocks.reserve(block_count);
    for (std::size_t b = 0; b < block_count; ++b) {
        const std::string name = in.text();
        const std::optional<model::ElementType> type = model::parse_element_type(name);
        if (!type) {
            throw_corrupt("unknown element type '" + name + "'");
        }
        std::vector<model::Label> ids = in.array<model::Label>();
        std::vector<model::Index> connectivity = in.array<model::Index>();
        blocks.emplace_back(*type, std::move(ids), std::move(connectivity));
    }
    return std::make_shared<const model::Mesh>(std::move(nodes), std::move(blocks));
}

}  // namespace

Bytes encode_object_data(const model::ObjectData& data) {
    Writer out;
    if (const auto* mesh = dynamic_cast<const model::Mesh*>(&data)) {
        write_mesh(out, *mesh);
    } else if (const auto* nodes = dynamic_cast<const model::NodeSet*>(&data)) {
        begin(out, Kind::node_set);
        out.array(nodes->nodes());
    } else if (const auto* elements = dynamic_cast<const model::ElementSet*>(&data)) {
        begin(out, Kind::element_set);
        out.array(elements->elements());
    } else if (const auto* surface = dynamic_cast<const model::Surface*>(&data)) {
        begin(out, Kind::surface);
        out.scalar(static_cast<std::uint64_t>(surface->sides().size()));
        for (const model::ElementSide& side : surface->sides()) {
            out.scalar(side.element);
            out.scalar(side.side);
        }
    } else {
        throw ProjectError(ProjectErrc::invalid_data, "this kind of object data cannot be saved");
    }
    return out.take();
}

std::shared_ptr<const model::ObjectData> decode_object_data(std::span<const std::byte> bytes) {
    Reader in(bytes);
    if (!in.starts_with_magic()) {
        throw_corrupt("not a block of object data");
    }
    if (const auto version = in.scalar<std::uint32_t>(); version != object_data_codec_version) {
        throw_corrupt("unknown encoding version " + std::to_string(version));
    }
    const auto kind = static_cast<Kind>(in.scalar<std::uint32_t>());
    std::shared_ptr<const model::ObjectData> data;
    try {
        switch (kind) {
            case Kind::mesh:
                data = read_mesh(in);
                break;
            case Kind::node_set:
                data = std::make_shared<const model::NodeSet>(in.array<model::Index>());
                break;
            case Kind::element_set:
                data = std::make_shared<const model::ElementSet>(in.array<model::Index>());
                break;
            case Kind::surface: {
                std::vector<model::ElementSide> sides(in.count(2 * sizeof(model::Index)));
                for (model::ElementSide& side : sides) {
                    side.element = in.scalar<model::Index>();
                    side.side = in.scalar<model::Index>();
                }
                data = std::make_shared<const model::Surface>(std::move(sides));
                break;
            }
            default:
                throw_corrupt("unknown kind of data " +
                              std::to_string(static_cast<std::uint32_t>(kind)));
        }
    } catch (const model::ModelError& error) {
        throw_corrupt(error.what());
    }
    in.finish();
    return data;
}

nlohmann::json describe_object_data(const model::ObjectData& data) {
    if (const auto* mesh = dynamic_cast<const model::Mesh*>(&data)) {
        return {
            {"kind", "mesh"}, {"nodes", mesh->node_count()}, {"elements", mesh->element_count()}};
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
