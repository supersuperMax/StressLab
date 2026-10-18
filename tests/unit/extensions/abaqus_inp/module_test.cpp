// The module fem.format.abaqus-inp as the application has it (task 3.2): its library in the copy of
// extensions/ the build makes, loaded by the Extension Manager, run through the C ABI on a real
// document. Opening a file activates it (onOpenFile:*.inp); what it reports reaches the listeners
// of problems; a project with an imported model saves the same bytes twice (CLAUDE.md, rule 6).

#include "fem/core/bindings.h"
#include "fem/core/command/command_bus.h"
#include "fem/core/command/problem.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/extension/error.h"
#include "fem/core/extension/manager.h"
#include "fem/core/extension/open_file.h"
#include "fem/core/form/form.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/model/document.h"
#include "fem/core/model/mesh.h"
#include "fem/storage/project.h"

#include "inp_test_support.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using fem::command::Problem;
using fem::command::Value;
using fem::extension::ExtensionErrc;
using fem::extension::ExtensionError;
using fem::extension::ExtensionSource;
using fem::extension::ExtensionState;
using fem::inp::testing::data;
using fem::inp::testing::utf8;
using fem::model::ObjectId;

constexpr const char* id = "fem.format.abaqus-inp";
const fs::path staged = fs::path(FEM_STAGED_EXTENSIONS_DIR);

std::string read(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::map<std::string, std::string> files_below(const fs::path& folder) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(folder)) {
        if (entry.is_regular_file()) {
            const std::u8string name = fs::relative(entry.path(), folder).generic_u8string();
            files.emplace(std::string(name.begin(), name.end()), read(entry.path()));
        }
    }
    return files;
}

class InpModule : public ::testing::Test {
protected:
    InpModule() {
        (void)manager.install(staged / "fem.model.structural", ExtensionSource::builtin);
        (void)manager.install(staged / id, ExtensionSource::builtin);
        (void)manager.enable(id);  // And fem.model.structural, which it requires.
    }

    /// Imports a fixture with the command, as a script does.
    ObjectId import(const fs::path& file, std::string encoding = "auto") {
        return ObjectId(
            static_cast<std::uint64_t>(bus.execute("inp.import", {{"path", Value(data() / file)},
                                                                  {"encoding", Value(encoding)}})
                                           .value.as_integer()));
    }

    [[nodiscard]] ObjectId child(ObjectId parent, std::string_view type,
                                 std::string_view name) const {
        return document.find_child(parent, type, name);
    }

    fem::model::Document document;
    fem::command::CommandBus bus{document};
    fem::context::ContextKeys keys;
    fem::extension::ExtensionManager manager{document, bus, keys};
    std::vector<Problem> problems;
    fem::Subscription listening =
        bus.on_problem([this](const Problem& problem) { problems.push_back(problem); });
};

TEST_F(InpModule, OpeningAFileActivatesTheModuleAndFillsTheModel) {
    std::vector<std::string> journal;
    const fem::Subscription recording =
        bus.on_journal([&](std::string_view entry) { journal.emplace_back(entry); });
    EXPECT_EQ(manager.get(id).state, ExtensionState::enabled);

    const Value result = fem::extension::open_model_file(manager, bus, data() / "flat.inp").value;
    EXPECT_EQ(manager.get(id).state, ExtensionState::active);
    const ObjectId input(static_cast<std::uint64_t>(result.as_integer()));
    EXPECT_EQ(document.get(input).type(), "abaqus_input");
    EXPECT_EQ(document.get(input).owner(), id);
    // The journal has the command of the module: replaying it reads the file again.
    ASSERT_EQ(journal.size(), 1U);
    EXPECT_TRUE(journal[0].starts_with("stresslab.cmd(\"inp.import\", path=")) << journal[0];
    EXPECT_NE(journal[0].find("flat.inp"), std::string::npos) << journal[0];

    const ObjectId mesh = child({}, "mesh", "flat");
    ASSERT_TRUE(mesh.is_valid());
    const auto data = document.data<fem::model::Mesh>(mesh);
    EXPECT_EQ(data->node_count(), 12U);
    ASSERT_EQ(data->blocks().size(), 1U);
    EXPECT_EQ(data->blocks()[0].type(), fem::model::ElementType::hex8);
    const auto fix = document.data<fem::model::NodeSet>(child(mesh, "node_set", "FIX"));
    EXPECT_EQ(std::vector<std::int32_t>(fix->nodes().begin(), fix->nodes().end()),
              (std::vector<std::int32_t>{0, 3, 6, 9}));
    const auto top = document.data<fem::model::Surface>(child(mesh, "surface", "TOP"));
    EXPECT_EQ(top->sides()[0], (fem::model::ElementSide{1, 5}));

    const ObjectId steel = child({}, "material", "Steel");
    ASSERT_TRUE(steel.is_valid());
    EXPECT_EQ(document.get(steel).owner(), "fem.model.structural");
    const ObjectId section = child({}, "solid_section", "EALL");
    EXPECT_TRUE(document.get(section).depends_on(steel));
    EXPECT_TRUE(document.get(section).depends_on(child(mesh, "element_set", "EALL")));
    const ObjectId step = child({}, "static_step", "Load");
    ASSERT_TRUE(step.is_valid());
    EXPECT_TRUE(child(step, "concentrated_load", "TIP").is_valid());
    EXPECT_TRUE(child(step, "abaqus_keyword", "*CONTACT CONTROLS (flat.inp:54)").is_valid());

    // One step of undo for the whole file.
    ASSERT_TRUE(bus.undo());
    EXPECT_EQ(document.size(), 0U);
    ASSERT_TRUE(bus.redo());
    EXPECT_TRUE(child({}, "mesh", "flat").is_valid());
}

TEST_F(InpModule, WarningsReachTheProblemsInBothLanguages) {
    (void)import("flat.inp");
    const auto contact = std::find_if(problems.begin(), problems.end(), [](const Problem& p) {
        return p.message == "%problem.passthrough%" &&
               std::get<std::string>(p.args.at("keyword")) == "*CONTACT CONTROLS";
    });
    ASSERT_NE(contact, problems.end());
    EXPECT_EQ(contact->owner, id);
    EXPECT_EQ(contact->command, "inp.import");
    EXPECT_EQ(contact->severity, Problem::Severity::warning);

    fem::l10n::Localization localization;
    const fem::Subscription catalogs = fem::bind_catalogs(manager, localization);
    localization.set_language("ru");
    EXPECT_EQ(fem::command::problem_text(*contact, localization),
              utf8(u8"flat.inp:54: *CONTACT CONTROLS сохранено без интерпретации."));
    localization.set_language("en");
    EXPECT_EQ(fem::command::problem_text(*contact, localization),
              "flat.inp:54: *CONTACT CONTROLS kept without interpretation.");
    EXPECT_EQ(problems.back().severity, Problem::Severity::info);
    EXPECT_EQ(problems.back().message, "%problem.units%");
}

TEST_F(InpModule, BrokenFileChangesNothingAndSaysWhere) {
    try {
        (void)import(fs::path("errors") / "unknown_node.inp");
        FAIL();
    } catch (const ExtensionError& error) {
        EXPECT_EQ(error.code(), ExtensionErrc::command_failed);
        EXPECT_NE(
            std::string(error.what())
                .find("unknown_node.inp:8: Element 2 refers to node 99, which is not defined."),
            std::string::npos)
            << error.what();
    }
    EXPECT_EQ(document.size(), 0U);
    EXPECT_FALSE(bus.can_undo());
    // The problem was reported before the command failed: the user sees where.
    ASSERT_FALSE(problems.empty());
    EXPECT_EQ(problems.back().severity, Problem::Severity::error);
    EXPECT_EQ(problems.back().message, "%error.unknownNode%");
    EXPECT_EQ(problems.back().file, "unknown_node.inp");
    EXPECT_EQ(problems.back().line, 8);

    EXPECT_THROW((void)import("flat.inp", "koi8-r"), ExtensionError);
    EXPECT_EQ(problems.back().message, "%error.unknownEncoding%");
}

TEST_F(InpModule, SecondImportTakesFreeNames) {
    (void)import("flat.inp");
    (void)import("flat.inp");
    EXPECT_TRUE(child({}, "material", "Steel-2").is_valid());
    EXPECT_TRUE(child({}, "mesh", "flat-2").is_valid());
    EXPECT_TRUE(child({}, "abaqus_input", "flat.inp-2").is_valid());
}

TEST_F(InpModule, EncodingIsAnArgumentOfTheCommand) {
    const ObjectId input = import(fs::path("bytes") / "cp1251.inp", "windows-1251");
    EXPECT_EQ(document.get(input).properties().at("heading"), utf8(u8"Кронштейн №1 — «тест»"));
}

TEST_F(InpModule, ImportedModelRoundTripsThroughAProjectFolder) {
    (void)import("assembly.inp");
    (void)import("flat.inp");
    fem::storage::ModuleVersions modules;
    for (const std::string& owner : document.owners()) {
        modules.emplace(owner, owner == "fem.core" ? "0.1.0" : "1.0.0");
    }
    const fs::path root = fs::temp_directory_path() / fs::path(u8"stresslab тест inp");
    fs::remove_all(root);
    const fs::path folder = root / fs::path(u8"кронштейн.slproj");
    (void)fem::storage::save_project(folder, document, modules);
    const std::map<std::string, std::string> files = files_below(folder);

    // Loaded and saved again: the same files, also with the module disabled.
    for (const bool enabled : {true, false}) {
        SCOPED_TRACE(enabled ? "enabled" : "disabled");
        fem::model::Document loaded;
        fem::command::CommandBus loaded_bus(loaded);
        loaded_bus.replace_document(fem::storage::load_project(folder).contents);
        loaded.set_owner_enabled("fem.model.structural", true);
        loaded.set_owner_enabled(id, enabled);
        EXPECT_EQ(loaded.size(), document.size());
        const fem::storage::SaveStats stats = fem::storage::save_project(folder, loaded, modules);
        EXPECT_EQ(stats.written, 0U);
        EXPECT_EQ(stats.removed, 0U);
        EXPECT_EQ(files_below(folder), files);
    }
    fs::remove_all(root);
}

std::set<std::string> catalog_keys(std::string_view language) {
    const nlohmann::json catalog =
        nlohmann::json::parse(read(staged / id / "l10n" / (std::string(language) + ".json")));
    std::set<std::string> keys;
    for (const auto& [key, value] : catalog.at("strings").items()) {
        keys.insert(key);
    }
    return keys;
}

TEST_F(InpModule, EveryTextIsTranslatedToEnglishAndRussian) {
    const std::set<std::string> english = catalog_keys("en");
    EXPECT_EQ(catalog_keys("ru"), english);

    // Texts of the manifest and the schemas ("%key%"), and problems of the code ("problem.x").
    std::set<std::string> referenced;
    const auto collect = [&](const std::string& text, const std::regex& pattern) {
        for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
            referenced.insert((*it)[1].str());
        }
    };
    const std::regex manifest_text("%([A-Za-z0-9_.-]+)%");
    collect(read(staged / id / "stresslab-extension.toml"), manifest_text);
    for (const fs::directory_entry& entry : fs::directory_iterator(staged / id / "schemas")) {
        collect(read(entry.path()), manifest_text);
    }
    const std::regex problem_key("\"((?:problem|error)\\.[A-Za-z]+)\"");
    for (const fs::directory_entry& entry :
         fs::recursive_directory_iterator(fs::path(FEM_INP_SOURCE_DIR))) {
        if (entry.is_regular_file()) {
            collect(read(entry.path()), problem_key);
        }
    }
    for (const std::string& key : referenced) {
        EXPECT_TRUE(english.contains(key)) << key << " has no text";
    }
    for (const std::string& key : english) {
        EXPECT_TRUE(referenced.contains(key)) << key << " is not used";
    }
}

TEST_F(InpModule, FormsOfItsTypesHaveNoMistakes) {
    const auto& contributes = *manager.registry().find(id);
    ASSERT_EQ(contributes.property_schemas.size(), 3U);
    for (const auto& schema : contributes.property_schemas) {
        SCOPED_TRACE(schema.type);
        std::vector<fem::form::SchemaError> errors;
        const fem::form::FormSpec spec = fem::form::parse(schema.schema, &errors);
        EXPECT_TRUE(errors.empty());
        for (const fem::form::Field* field : fem::form::all_fields(spec)) {
            EXPECT_EQ(field->title.front(), '%') << field->pointer;
        }
    }
    ASSERT_EQ(contributes.file_formats.size(), 1U);
    EXPECT_EQ(contributes.file_formats[0].command, "inp.import");
}

}  // namespace
