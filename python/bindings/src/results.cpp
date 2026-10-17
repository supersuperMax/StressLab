#include "fem/storage/result_cache.h"
#include "fem/storage/session.h"

#include "common.h"
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>

namespace fem::python {

namespace {

storage::ResultCache& results() {
    return Session::instance().active_document()->project().results();
}

storage::ResultInputs to_inputs(const std::optional<nb::dict>& inputs) {
    storage::ResultInputs result;
    if (inputs) {
        for (const auto& [name, value] : *inputs) {
            result.emplace(to_utf8(name), to_utf8(value));
        }
    }
    return result;
}

/// A result as a dict; `stresslab.results.Result` is made from it.
nb::dict to_python(const storage::ResultInfo& info) {
    nb::dict result;
    result["key"] = info.key;
    result["version"] = info.version;
    result["solver"] = to_str(info.solver.id);
    result["solver_version"] = to_str(info.solver.version);
    nb::dict inputs;
    for (const auto& [name, value] : info.inputs) {
        inputs[to_str(name)] = to_str(value);
    }
    result["inputs"] = inputs;
    result["created"] = info.created;
    result["used"] = info.used;
    nb::list files;
    for (const storage::ResultFile& file : info.files) {
        nb::dict entry;
        entry["path"] = to_str(file.path);
        entry["size"] = file.size;
        entry["sha256"] = file.sha256;
        files.append(entry);
    }
    result["files"] = files;
    result["bytes"] = info.bytes;
    result["cached"] = info.cached;
    result["pinned"] = info.pinned;
    result["folder"] = info.folder ? to_python_path(*info.folder) : nb::none();
    return result;
}

}  // namespace

void define_results(nb::module_& m) {
    nb::module_ module = m.def_submodule("results");
    module.def(
        "key",
        [](const std::string& solver, const std::string& solver_version,
           std::optional<nb::dict> inputs, std::optional<std::string> version) {
            const storage::SolverInfo info{solver, solver_version};
            return version ? results().key_at(*version, info, to_inputs(inputs))
                           : results().key(info, to_inputs(inputs));
        },
        nb::arg("solver"), nb::arg("solver_version") = "", nb::arg("inputs") = nb::none(),
        nb::arg("version") = nb::none(),
        "The key of the results of the solver for the model now, or for the model of a version.");
    module.def(
        "put",
        [](const std::string& key, nb::handle folder, const std::string& version,
           const std::string& solver, const std::string& solver_version,
           std::optional<nb::dict> inputs) {
            return to_python(results().put(key, version, {solver, solver_version},
                                           to_inputs(inputs), to_path(folder)));
        },
        nb::arg("key"), nb::arg("folder"), nb::arg("version"), nb::arg("solver"),
        nb::arg("solver_version") = "", nb::arg("inputs") = nb::none(),
        "Puts the files of the folder in the cache as the result of the key, computed from the "
        "version; returns it as a dict.");
    module.def(
        "find",
        [](const std::string& key) -> nb::object {
            const std::optional<storage::ResultInfo> info = results().find(key);
            return info ? nb::object(to_python(*info)) : nb::none();
        },
        nb::arg("key"), "The result of the key as a dict, None if there is none.");
    module.def(
        "list",
        [] {
            nb::list result;
            for (const storage::ResultInfo& info : results().list()) {
                result.append(to_python(info));
            }
            return result;
        },
        "Every result, in the cache or pinned, used last first, as dicts.");
    module.def(
        "pin", [](const std::string& key) { results().pin(key); }, nb::arg("key"),
        "Keeps the result in the history. Written to the journal.");
    module.def(
        "unpin", [](const std::string& key) { results().unpin(key); }, nb::arg("key"),
        "The result is not kept in the history any more. Written to the journal.");
    module.def(
        "remove", [](const std::string& key) { return results().remove(key); }, nb::arg("key"),
        "Removes the files of the result from the cache. Written to the journal.");
    module.def(
        "prune_inactive",
        [] {
            const storage::PruneStats stats = results().prune_inactive();
            nb::dict result;
            result["removed"] = stats.removed;
            result["freed_bytes"] = stats.freed_bytes;
            return result;
        },
        "Removes from the cache the results of versions not of the current variant. Written to "
        "the journal.");
    module.def("limit", [] { return results().limit(); }, "The size the cache may take, in bytes.");
    module.def(
        "set_limit", [](std::uint64_t bytes) { results().set_limit(bytes); }, nb::arg("bytes"),
        "Sets the size the cache may take, in bytes; 0 is no limit.");
    module.def(
        "usage", [] { return results().usage(); }, "The size the cache takes now, in bytes.");
}

}  // namespace fem::python
