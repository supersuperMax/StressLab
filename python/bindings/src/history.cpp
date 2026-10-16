#include "fem/storage/history.h"

#include "fem/storage/session.h"

#include "common.h"
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>

namespace fem::python {

namespace {

storage::ProjectHistory& history() {
    return Session::instance().active_document()->project().history();
}

/// A version as a dict; `stresslab.history.Version` is made from it.
nb::dict to_python(const storage::VersionInfo& info) {
    nb::dict result;
    result["id"] = info.id;
    nb::list parents;
    for (const std::string& parent : info.parents) {
        parents.append(parent);
    }
    result["parents"] = parents;
    result["message"] = info.message;
    result["summary"] = info.summary;
    result["author"] = info.author.name;
    result["email"] = info.author.email;
    result["time"] = info.time;
    result["time_offset"] = info.time_offset;
    result["kind"] = std::string(storage::to_string(info.kind));
    result["before"] = info.before;
    result["restored"] = info.restored;
    result["branch"] = info.branch;
    return result;
}

nb::list to_python(const std::vector<storage::VersionInfo>& versions) {
    nb::list result;
    for (const storage::VersionInfo& info : versions) {
        result.append(to_python(info));
    }
    return result;
}

nb::dict to_python(const std::map<std::string, std::string>& names) {
    nb::dict result;
    for (const auto& [name, id] : names) {
        result[to_str(name)] = id;
    }
    return result;
}

}  // namespace

void define_history(nb::module_& m) {
    nb::module_ module = m.def_submodule("history");
    module.def(
        "current_branch", [] { return history().current_branch(); },
        "The variant the next version goes to; None while a version is viewed on its own.");
    module.def(
        "current_version", [] { return history().current_version(); },
        "The id of the version the model was opened from or last committed as; None before the "
        "first one.");
    module.def(
        "has_changes", [] { return history().has_changes(); },
        "True if the model differs from its version.");
    module.def(
        "log",
        [](std::optional<std::string> target, bool all, std::size_t limit) {
            return to_python(history().log(
                target ? std::optional<std::string_view>(*target) : std::nullopt, all, limit));
        },
        nb::arg("target") = nb::none(), nb::arg("all") = false, nb::arg("limit") = 0,
        "Versions newest first, as dicts.");
    module.def(
        "info", [](const std::string& target) { return to_python(history().info(target)); },
        nb::arg("target"), "One version as a dict.");
    module.def(
        "journal", [](const std::string& target) { return history().journal(target); },
        nb::arg("target"), "The journal of a version.");
    module.def(
        "resolve", [](const std::string& target) { return history().resolve(target); },
        nb::arg("target"), "The id of a version.");
    module.def(
        "branches", [] { return to_python(history().branches()); },
        "Variants by name with their last versions.");
    module.def(
        "tags", [] { return to_python(history().tags()); }, "Labels by name with their versions.");
    module.def(
        "stashes", [] { return to_python(history().stashes()); },
        "Changes put aside, newest first, as dicts.");

    module.def(
        "commit", [](const std::string& message) { return history().commit(message); },
        nb::arg("message"), "Saves and makes a version. Written to the journal.");
    module.def(
        "snapshot", [](const std::string& reason) { return history().snapshot(reason); },
        nb::arg("reason") = "",
        "An automatic version if the model changed. Written to the journal.");
    module.def(
        "branch",
        [](const std::string& name, std::optional<std::string> at) {
            history().branch(name, at ? std::optional<std::string_view>(*at) : std::nullopt);
        },
        nb::arg("name"), nb::arg("at") = nb::none(), "Makes a variant. Written to the journal.");
    module.def(
        "checkout", [](const std::string& target) { history().checkout(target); },
        nb::arg("target"), "Opens a variant, a label or a version. Written to the journal.");
    module.def(
        "tag",
        [](const std::string& name, std::optional<std::string> at) {
            history().tag(name, at ? std::optional<std::string_view>(*at) : std::nullopt);
        },
        nb::arg("name"), nb::arg("at") = nb::none(), "Makes a label. Written to the journal.");
    module.def(
        "stash", [](const std::string& message) { history().stash(message); },
        nb::arg("message") = "", "Puts the changes aside. Written to the journal.");
    module.def(
        "stash_pop", [](std::size_t index) { history().stash_pop(index); }, nb::arg("index") = 0,
        "Brings back changes put aside. Written to the journal.");
    module.def(
        "stash_drop", [](std::size_t index) { history().stash_drop(index); }, nb::arg("index") = 0,
        "Forgets changes put aside. Written to the journal.");
    module.def(
        "restore_version",
        [](const std::string& target) { return history().restore_version(target); },
        nb::arg("target"), "A new version with the model of an older one. Written to the journal.");
    module.def(
        "set_author",
        [](const std::string& name, const std::string& email) {
            history().set_author({name, email});
        },
        nb::arg("name"), nb::arg("email"), "Who signs new versions.");
    module.def(
        "author",
        [] {
            const storage::Author& author = history().author();
            return nb::make_tuple(author.name, author.email);
        },
        "Who signs new versions: (name, email).");
}

}  // namespace fem::python
