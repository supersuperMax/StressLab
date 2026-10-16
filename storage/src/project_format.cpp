#include "fem/storage/project_format.h"

#include "fem/core/model/object.h"
#include "fem/core/model/units.h"
#include "fem/storage/error.h"
#include "fem/storage/json.h"
#include "fem/storage/object_data_codec.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <limits>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace fem::storage {

namespace {

using Json = nlohmann::json;

// --- Paths ---------------------------------------------------------------------------------------

bool is_plain(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '_' || c == '-';
}

void append_escaped(std::string& out, char c) {
    constexpr const char* hex = "0123456789ABCDEF";
    const auto byte = static_cast<unsigned char>(c);
    out.push_back('%');
    out.push_back(hex[byte >> 4]);
    out.push_back(hex[byte & 0xF]);
}

// Names Windows keeps for devices, with or without an extension.
bool is_reserved_name(std::string_view segment) {
    static constexpr std::array<std::string_view, 22> reserved = {
        "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
        "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    std::string stem(segment.substr(0, segment.find('.')));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return std::find(reserved.begin(), reserved.end(), stem) != reserved.end();
}

std::string path_segment(std::string_view name) {
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        // A leading or trailing dot ("..", ".git", "a.") and the first letter of a device name are
        // escaped as well, so the segment is an ordinary name everywhere.
        const bool edge_dot = c == '.' && (i == 0 || i + 1 == name.size());
        if (!is_plain(c) || edge_dot || (i == 0 && is_reserved_name(name))) {
            append_escaped(out, c);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// --- Reading -------------------------------------------------------------------------------------

/// The members of a JSON object of a file, taken one by one, so that unknown ones are found.
class Members {
public:
    Members(Json value, std::string file) : value_(std::move(value)), file_(std::move(file)) {
        if (!value_.is_object()) {
            fail("expected a JSON object");
        }
    }

    [[noreturn]] void fail(const std::string& message) const {
        throw ProjectError(ProjectErrc::invalid_file, message, file_);
    }

    [[nodiscard]] std::optional<Json> take_optional(const std::string& key) {
        const auto it = value_.find(key);
        if (it == value_.end()) {
            return std::nullopt;
        }
        Json result = std::move(*it);
        value_.erase(it);
        return result;
    }

    [[nodiscard]] Json take(const std::string& key) {
        std::optional<Json> result = take_optional(key);
        if (!result) {
            fail("\"" + key + "\" is missing");
        }
        return std::move(*result);
    }

    [[nodiscard]] std::uint64_t take_count(const std::string& key, std::uint64_t minimum) {
        return count(take(key), key, minimum);
    }

    [[nodiscard]] std::uint64_t count(const Json& value, const std::string& what,
                                      std::uint64_t minimum) const {
        if (!value.is_number_integer() ||
            (!value.is_number_unsigned() && value.get<std::int64_t>() < 0)) {
            fail("\"" + what + "\" must be a whole number");
        }
        const auto number = value.get<std::uint64_t>();
        if (number < minimum) {
            fail("\"" + what + "\" must be at least " + std::to_string(minimum));
        }
        return number;
    }

    [[nodiscard]] std::string take_string(const std::string& key) {
        Json value = take(key);
        if (!value.is_string()) {
            fail("\"" + key + "\" must be text");
        }
        return value.get<std::string>();
    }

    void finish() const {
        if (!value_.empty()) {
            fail("unknown member \"" + value_.begin().key() + "\"");
        }
    }

    [[nodiscard]] const std::string& file() const noexcept { return file_; }

private:
    Json value_;
    std::string file_;
};

Json parse_file(const ProjectFiles& files, std::string_view path) {
    const auto it = files.find(path);
    if (it == files.end()) {
        throw ProjectError(ProjectErrc::invalid_file, "the file is missing", std::string(path));
    }
    return parse_json(it->second, it->first);
}

model::UnitSystem read_units(Json value, const std::string& file) {
    Members units(std::move(value), file);
    const auto unit = [&units](const std::string& key, auto parse) {
        const std::string symbol = units.take_string(key);
        const auto parsed = parse(symbol);
        if (!parsed) {
            units.fail("unknown unit \"" + symbol + "\" of " + key);
        }
        return *parsed;
    };
    model::UnitSystem result;
    result.length = unit("length", model::parse_length_unit);
    result.mass = unit("mass", model::parse_mass_unit);
    result.time = unit("time", model::parse_time_unit);
    result.temperature = unit("temperature", model::parse_temperature_unit);
    units.finish();
    return result;
}

void read_project_file(const ProjectFiles& files, LoadedProject& project) {
    if (!files.contains(project_file)) {
        throw ProjectError(ProjectErrc::not_a_project, "the folder has no project.json",
                           std::string(project_file));
    }
    Members members(parse_file(files, project_file), std::string(project_file));
    // The format first: a later one may have other members.
    const Json format = members.take("format");
    if (!format.is_number_integer()) {
        members.fail("\"format\" must be a whole number");
    }
    if (format.get<std::int64_t>() > project_format_version) {
        throw ProjectError(ProjectErrc::unsupported_format,
                           "the project has format " + std::to_string(format.get<std::int64_t>()) +
                               ", this version reads up to " +
                               std::to_string(project_format_version),
                           members.file());
    }
    if (format.get<std::int64_t>() == 1) {
        // Format 1 (task 2.1) kept bulk data uncompressed and whole; nobody but its developers
        // had projects in it, so it is not read (docs/adr/0018-blob-store.md).
        throw ProjectError(ProjectErrc::unsupported_format,
                           "the project has format 1 of an early development version, which "
                           "this version does not read",
                           members.file());
    }
    if (format.get<std::int64_t>() != project_format_version) {
        members.fail("unknown format " + format.dump());
    }

    const Json modules = members.take("modules");
    if (!modules.is_object()) {
        members.fail("\"modules\" must be an object");
    }
    for (auto it = modules.begin(); it != modules.end(); ++it) {
        if (!it.value().is_string()) {
            members.fail("the version of module \"" + it.key() + "\" must be text");
        }
        project.modules.emplace(it.key(), it.value().get<std::string>());
    }
    project.contents.next_id = members.take_count("nextObjectId", 1);
    project.contents.units = read_units(members.take("units"), members.file());
    members.finish();
}

/// id -> block of its data.
std::map<model::ObjectId, std::string> read_blob_refs(const ProjectFiles& files) {
    const std::string file(blob_refs_file);
    const Json value = parse_file(files, blob_refs_file);
    if (!value.is_object()) {
        throw ProjectError(ProjectErrc::invalid_file, "expected a JSON object", file);
    }
    std::map<model::ObjectId, std::string> refs;
    for (auto it = value.begin(); it != value.end(); ++it) {
        const std::string& key = it.key();
        std::uint64_t id = 0;
        const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), id);
        if (error != std::errc{} || end != key.data() + key.size() || id == 0 || key[0] == '0') {
            throw ProjectError(ProjectErrc::invalid_file, "\"" + key + "\" is not an object id",
                               file);
        }
        if (!it.value().is_string() || !is_blob_ref(it.value().get_ref<const std::string&>())) {
            throw ProjectError(
                ProjectErrc::invalid_file,
                "the block of object " + key + " must be \"sha256:\" and 64 hex digits", file);
        }
        refs.emplace(model::ObjectId(id), it.value().get<std::string>());
    }
    return refs;
}

class ObjectReader {
public:
    ObjectReader(const BlobStore& blobs, std::map<model::ObjectId, std::string> refs)
        : blobs_(blobs), refs_(std::move(refs)) {}

    model::StoredObject read(const std::string& path, const std::string& text) {
        Members members(parse_json(text, path), path);
        model::StoredObject stored;
        stored.id = model::ObjectId(members.take_count("id", 1));
        const std::string expected = std::to_string(stored.id.value()) + ".json";
        const std::string_view name = std::string_view(path).substr(path.rfind('/') + 1);
        if (name != expected) {
            members.fail("the file of object " + std::to_string(stored.id.value()) +
                         " must be named " + expected);
        }

        model::NewObject& init = stored.init;
        init.type = members.take_string("type");
        init.owner = members.take_string("owner");
        const std::uint64_t schema = members.take_count("schemaVersion", 1);
        if (schema > std::numeric_limits<std::uint32_t>::max()) {
            members.fail("\"schemaVersion\" is too large");
        }
        init.schema_version = static_cast<std::uint32_t>(schema);
        init.name = members.take_string("name");
        if (const Json parent = members.take("parent"); !parent.is_null()) {
            init.parent = model::ObjectId(members.count(parent, "parent", 1));
        }
        const Json dependencies = members.take("dependencies");
        if (!dependencies.is_array()) {
            members.fail("\"dependencies\" must be a list of object ids");
        }
        for (const Json& target : dependencies) {
            init.dependencies.emplace_back(members.count(target, "dependencies", 1));
        }
        init.properties = members.take("properties");
        if (!init.properties.is_object()) {
            members.fail("\"properties\" must be an object");
        }

        const std::optional<Json> description = members.take_optional("data");
        const auto ref = refs_.find(stored.id);
        if (description.has_value() != (ref != refs_.end())) {
            members.fail(description ? "refs/blobs.json has no block for the data of the object"
                                     : "refs/blobs.json has a block for an object without data");
        }
        if (description) {
            init.data = decode(ref->second);
            if (describe_object_data(*init.data) != *description) {
                members.fail("\"data\" does not describe the block " + ref->second);
            }
            refs_.erase(ref);
        }
        members.finish();
        return stored;
    }

    /// Blocks no object claimed.
    [[nodiscard]] const std::map<model::ObjectId, std::string>& unclaimed() const noexcept {
        return refs_;
    }

private:
    std::shared_ptr<const model::ObjectData> decode(const std::string& ref) {
        // Objects with the same data share it, as they did when saved.
        if (const auto it = decoded_.find(ref); it != decoded_.end()) {
            return it->second;
        }
        std::shared_ptr<const model::ObjectData> data;
        try {
            data = get_object_data(ref, blobs_);
        } catch (const ProjectError& error) {
            throw ProjectError(error.code(), ref + ": " + error.what());
        }
        decoded_.emplace(ref, data);
        return data;
    }

    const BlobStore& blobs_;
    std::map<model::ObjectId, std::string> refs_;
    std::map<std::string, std::shared_ptr<const model::ObjectData>> decoded_;
};

// --- Writing -------------------------------------------------------------------------------------

std::string canonical(const Json& value, const std::string& file) {
    try {
        return to_canonical_json(value);
    } catch (const ProjectError& error) {
        throw ProjectError(error.code(), error.what(), file);
    }
}

}  // namespace

std::string object_file(std::string_view owner, std::string_view type, model::ObjectId id) {
    std::string path(model_folder);
    path += '/';
    if (owner != model::core_owner) {
        path += "ext/" + path_segment(owner) + '/';
    }
    path += path_segment(type) + '/' + std::to_string(id.value()) + ".json";
    return path;
}

ProjectFiles write_project(const model::Document& document, const ModuleVersions& modules,
                           BlobStore& blobs) {
    ProjectFiles files;
    Json refs = Json::object();
    // Objects that share their data (as after loading) store it once.
    std::map<const model::ObjectData*, std::string> stored;
    for (const model::Object& object : document.objects()) {
        const std::string path = object_file(object.owner(), object.type(), object.id());
        Json value = {
            {"id", object.id().value()},
            {"type", object.type()},
            {"owner", object.owner()},
            {"schemaVersion", object.schema_version()},
            {"name", object.name()},
            {"parent", object.parent().is_valid() ? Json(object.parent().value()) : Json()},
            {"properties", object.properties()},
        };
        Json dependencies = Json::array();
        for (const model::ObjectId target : object.dependencies()) {
            dependencies.push_back(target.value());
        }
        value["dependencies"] = std::move(dependencies);
        if (const auto& data = object.data()) {
            try {
                value["data"] = describe_object_data(*data);
                auto [it, added] = stored.try_emplace(data.get());
                if (added) {
                    it->second = put_object_data(*data, blobs);
                }
                refs[std::to_string(object.id().value())] = it->second;
            } catch (const ProjectError& error) {
                if (!error.file().empty()) {
                    throw;
                }
                throw ProjectError(error.code(), error.what(), path);
            }
        }
        files.emplace(path, canonical(value, path));
    }
    files.emplace(blob_refs_file, canonical(refs, std::string(blob_refs_file)));

    Json versions = Json::object();
    for (const std::string& owner : document.owners()) {
        const auto it = modules.find(owner);
        versions[owner] = it != modules.end() ? it->second : std::string();
    }
    const model::UnitSystem& units = document.units();
    const Json project = {
        {"format", project_format_version},
        {"modules", std::move(versions)},
        {"nextObjectId", document.next_id()},
        {"units",
         {{"length", model::symbol(units.length)},
          {"mass", model::symbol(units.mass)},
          {"time", model::symbol(units.time)},
          {"temperature", model::symbol(units.temperature)}}},
    };
    files.emplace(project_file, canonical(project, std::string(project_file)));
    return files;
}

std::vector<std::string> read_blob_roots(const ProjectFiles& files) {
    LoadedProject project;
    read_project_file(files, project);
    std::vector<std::string> roots;
    for (auto& [id, ref] : read_blob_refs(files)) {
        roots.push_back(std::move(ref));
    }
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
}

LoadedProject read_project(const ProjectFiles& files, const BlobStore& blobs) {
    LoadedProject project;
    read_project_file(files, project);
    ObjectReader reader(blobs, read_blob_refs(files));

    const std::string prefix = std::string(model_folder) + '/';
    for (const auto& [path, text] : files) {
        if (!path.starts_with(prefix) || !path.ends_with(".json")) {
            continue;
        }
        model::StoredObject stored = reader.read(path, text);
        if (!project.files.emplace(stored.id, path).second) {
            throw ProjectError(ProjectErrc::invalid_file,
                               "object " + std::to_string(stored.id.value()) + " is also in " +
                                   project.files.at(stored.id),
                               path);
        }
        project.contents.objects.push_back(std::move(stored));
    }
    if (!reader.unclaimed().empty()) {
        throw ProjectError(ProjectErrc::invalid_file,
                           "there is no object " +
                               std::to_string(reader.unclaimed().begin()->first.value()) +
                               " with data",
                           std::string(blob_refs_file));
    }
    return project;
}

}  // namespace fem::storage
