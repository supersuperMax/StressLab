#pragma once

#include "fem/core/command/model_commands.h"
#include "fem/core/model/mesh.h"
#include "fem/storage/session.h"

#include "storage_test_support.h"
#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <string>
#include <vector>

// A project in a folder with a session, for the tests of the cache of results and of packages
// (task 2.5).
namespace fem::storage::testing {

inline std::string utf8(std::u8string_view text) {
    return {text.begin(), text.end()};
}

inline void write_bytes(const fs::path& file, std::string_view bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

inline std::string read_bytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// The files below `folder` by their paths with '/', and their bytes.
inline std::map<std::string, std::string> files_below(const fs::path& folder) {
    std::map<std::string, std::string> result;
    for (const auto& entry : fs::recursive_directory_iterator(folder)) {
        if (entry.is_regular_file()) {
            const std::u8string path = entry.path().lexically_relative(folder).generic_u8string();
            result.emplace(std::string(path.begin(), path.end()), read_bytes(entry.path()));
        }
    }
    return result;
}

class ProjectFixture : public ::testing::Test {
protected:
    static constexpr const char* module = "fem.demo";

    explicit ProjectFixture(std::string_view name)
        : dir(name),
          session(doc.bus(),
                  [](std::string_view id) { return id == module ? std::string("1.5.0") : ""; }),
          journal_(doc.bus().on_journal(
              [this](std::string_view entry) { journal.emplace_back(entry); })) {
        doc.document.set_owner_enabled(module, true);
    }

    command::CommandBus& bus() { return doc.bus(); }
    ProjectHistory& history() { return session.history(); }
    ResultCache& results() { return session.results(); }

    model::ObjectId add(const std::string& type, const std::string& name) {
        namespace mc = command::model_commands;
        return model::ObjectId(static_cast<std::uint64_t>(
            bus()
                .execute(mc::create, {{"type", type}, {"owner", module}, {"name", name}})
                .value.as_integer()));
    }

    void set(model::ObjectId id, double thickness) {
        namespace mc = command::model_commands;
        bus().execute(mc::set_property,
                      {{"object", id}, {"key", "thickness"}, {"value", thickness}});
    }

    fs::path folder() const { return dir.path() / fs::path(u8"крыло.slproj"); }

    /// A project saved to `folder()` with "skin" of thickness 2, and its first version.
    model::ObjectId committed_project() {
        const model::ObjectId skin = add("section", "skin");
        set(skin, 2.0);
        session.save_as(folder());
        (void)history().commit("Skin 2 mm");
        return skin;
    }

    /// A folder of files as a solver writes them.
    fs::path solver_output(std::string_view name, const std::map<std::string, std::string>& files) {
        const fs::path out = dir.path() / fs::path(std::u8string(name.begin(), name.end()));
        fs::remove_all(out);
        fs::create_directories(out);
        for (const auto& [path, bytes] : files) {
            write_bytes(out / fs::path(std::u8string(path.begin(), path.end())), bytes);
        }
        return out;
    }

    /// The journal since the last call.
    std::vector<std::string> take_journal() {
        std::vector<std::string> result;
        result.swap(journal);
        return result;
    }

    TempDir dir;
    TestDocument doc;
    ProjectSession session;
    std::vector<std::string> journal;

private:
    Subscription journal_;
};

}  // namespace fem::storage::testing
