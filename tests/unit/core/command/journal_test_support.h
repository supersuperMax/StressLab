#pragma once

// Sessions whose journals the tests replay: the golden session behind
// tests/data/journal/golden.slpy and a long random one. Used by the parser tests (task 1.3) and
// by the tests that run journals as Python scripts (task 1.8).

#include "fem/core/command/command_bus.h"
#include "fem/core/command/model_commands.h"
#include "fem/core/model/error.h"

#include "command_test_support.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fem::command::testing {

/// A document with its bus, a module that is enabled and its commands, as a session would have.
struct JournalSession {
    JournalSession() {
        doc.set_owner_enabled(test_owner, true);
        // A command of a module with a path argument: creates an object named after the file.
        bus.register_command(
            {.id = "test.load",
             .owner = test_owner,
             .args = {{.name = "file", .type = ArgType::path},
                      {.name = "options", .type = ArgType::dict, .required = false}}},
            [](CommandContext& context) {
                model::NewObject init{.type = "import", .owner = test_owner};
                const std::u8string stem = context.args().path("file").stem().u8string();
                init.name.assign(stem.begin(), stem.end());
                if (const Value* options = context.args().find("options")) {
                    for (const auto& [key, value] : options->as_dict()) {
                        init.properties[key] = value.as_number();
                    }
                }
                return Value(context.editor().create(std::move(init)));
            });
    }

    model::Document doc;
    CommandBus bus{doc};
    JournalRecorder journal{bus};
};

inline model::ObjectId to_id(const CommandResult& result) {
    return model::ObjectId(static_cast<std::uint64_t>(result.value.as_integer()));
}

/// The session behind tests/data/journal/golden.slpy.
inline void run_golden_session(CommandBus& bus) {
    bus.execute(model_commands::set_units,
                {{"length", "mm"}, {"mass", "t"}, {"time", "s"}, {"temperature", "degC"}});
    const model::ObjectId steel = to_id(bus.execute(
        model_commands::create,
        {{"type", "material"},
         {"owner", test_owner},
         {"name", utf8(u8"Сталь «Ст3»")},
         {"properties", Value::Dict{{"E", 210000.0},
                                    {"nu", 0.3},
                                    {"rho", 7.85e-9},
                                    {"alpha", 1.2e-5},
                                    {"table", Value::List{Value::List{20.0, 210000.0},
                                                          Value::List{400.0, 170000.0}}},
                                    {"note", utf8(u8"ГОСТ 380-2005\n\"кипящая\"\tсталь")}}}}));
    const model::ObjectId part = to_id(
        bus.execute(model_commands::create,
                    {{"type", "part"}, {"owner", test_owner}, {"name", utf8(u8"Кронштейн")}}));
    const model::ObjectId load = to_id(
        bus.execute(model_commands::create,
                    {{"type", "load"},
                     {"owner", test_owner},
                     {"name", "P1"},
                     {"parent", part},
                     {"dependencies", Value::List{steel}},
                     {"properties",
                      Value::Dict{{"value", -1500}, {"direction", Value::List{0.0, 0.0, -1.0}}}}}));
    bus.execute(model_commands::rename, {{"object", load}, {"name", utf8(u8"Сила 1")}});
    bus.undo();
    bus.redo();
    model::ObjectId pressure;
    {
        Transaction transaction = bus.begin_transaction(utf8(u8"Нагрузки"));
        pressure = to_id(bus.execute(
            model_commands::create,
            {{"type", "load"}, {"owner", test_owner}, {"name", "p"}, {"parent", part}}));
        bus.execute(model_commands::set_properties,
                    {{"object", pressure},
                     {"properties", Value::Dict{{"value", 0.5}, {"empty", Value::Dict{}}}}});
        bus.execute(model_commands::add_dependency, {{"object", pressure}, {"target", steel}});
        transaction.commit();
    }
    {
        Transaction abandoned = bus.begin_transaction("abandoned");
        bus.execute(model_commands::create,
                    {{"type", "load"}, {"owner", test_owner}, {"name", "lost"}});
    }
    bus.execute("test.load", {{"file", utf8(u8"D:/Проекты/модель 1.dat")},
                              {"options", Value::Dict{{"scale", 2}}}});
    bus.execute(model_commands::remove, {{"object", load}});
    bus.execute(model_commands::remove_dependency, {{"object", pressure}, {"target", steel}});
}

inline std::string golden_path_text() {
    return FEM_TEST_DATA_DIR "/journal/golden.slpy";
}

inline std::filesystem::path golden_path() {
    const std::string text = golden_path_text();
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

/// Deterministic pseudo-random numbers, the same on every platform.
class Sequence {
public:
    std::uint32_t next(std::uint32_t bound) {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state_ >> 33) % bound;
    }

private:
    std::uint64_t state_ = 2026;
};

/// Runs a long random session of the core commands, undo, redo and transactions; some commands
/// fail (duplicate names, cycles, removed objects) and some transactions are rolled back.
inline void run_random_session(CommandBus& bus) {
    Sequence random;
    std::vector<model::ObjectId> objects;
    int completed = 0;
    const char* names[] = {"a", "b", "балка", "узел 1", "Сталь «Ст3»", "x\"y", "tab\there", "😀"};
    const char* units[][4] = {
        {"m", "kg", "s", "K"}, {"mm", "t", "s", "degC"}, {"in", "slinch", "s", "degF"}};

    const auto pick = [&]() -> model::ObjectId {
        return objects.empty() ? model::ObjectId{}
                               : objects[random.next(static_cast<std::uint32_t>(objects.size()))];
    };
    const auto random_value = [&](int depth) {
        const auto make = [&](auto& self, int level) -> Value {
            switch (random.next(level > 1 ? 5 : 7)) {
                case 0:
                    return Value(static_cast<int>(random.next(2000)) - 1000);
                case 1:
                    return Value(static_cast<double>(random.next(100000)) * 1e-3);
                case 2:
                    return Value(static_cast<double>(random.next(1000)) * 1.5e-7);
                case 3:
                    return Value(names[random.next(8)]);
                case 4:
                    return Value(random.next(2) == 1);
                case 5: {
                    Value::List list;
                    for (std::uint32_t i = random.next(4); i > 0; --i) {
                        list.push_back(self(self, level + 1));
                    }
                    return Value(std::move(list));
                }
                default: {
                    Value::Dict dict;
                    for (std::uint32_t i = random.next(4); i > 0; --i) {
                        dict[names[random.next(8)]] = self(self, level + 1);
                    }
                    return Value(std::move(dict));
                }
            }
        };
        return make(make, depth);
    };
    const auto properties = [&] {
        Value::Dict dict;
        for (std::uint32_t i = random.next(4); i > 0; --i) {
            dict[std::string("p") + std::to_string(random.next(5))] = random_value(0);
        }
        return Value(std::move(dict));
    };

    const auto run_one = [&] {
        Value::Dict args;
        std::string command;
        switch (random.next(9)) {
            case 0:
            case 1:
            case 2: {
                command = model_commands::create;
                args = {
                    {"type", random.next(2) == 1 ? "item" : "group"},
                    {"owner", test_owner},
                    {"name", std::string(names[random.next(8)]) + std::to_string(random.next(3))},
                    {"properties", properties()}};
                if (const model::ObjectId parent = pick();
                    parent.is_valid() && random.next(2) == 1) {
                    args["parent"] = parent;
                }
                if (const model::ObjectId target = pick();
                    target.is_valid() && random.next(3) == 1) {
                    args["dependencies"] = Value::List{target};
                }
                break;
            }
            case 3:
                command = model_commands::rename;
                args = {
                    {"object", pick()},
                    {"name", std::string(names[random.next(8)]) + std::to_string(random.next(3))}};
                break;
            case 4:
                command = model_commands::set_properties;
                args = {{"object", pick()}, {"properties", properties()}};
                break;
            case 5:
                command = random.next(2) == 1 ? model_commands::add_dependency
                                              : model_commands::remove_dependency;
                args = {{"object", pick()}, {"target", pick()}};
                break;
            case 6:
                command = model_commands::remove;
                args = {{"object", pick()}};
                break;
            case 7: {
                command = model_commands::set_units;
                const auto& system = units[random.next(3)];
                args = {{"length", system[0]},
                        {"mass", system[1]},
                        {"time", system[2]},
                        {"temperature", system[3]}};
                break;
            }
            default:
                if (bus.in_transaction()) {
                    return;
                }
                if (random.next(2) == 1) {
                    bus.undo();
                } else {
                    bus.redo();
                }
                return;
        }
        if (args.contains("object") && args.at("object").as_integer() == 0) {
            return;  // No objects yet.
        }
        try {
            const CommandResult result = bus.execute(command, args);
            ++completed;
            if (command == model_commands::create) {
                objects.push_back(to_id(result));
            }
        } catch (const model::ModelError&) {
            // Duplicate names, cycles, removed objects: refused, not journaled.
        }
    };

    while (completed < 150) {
        if (random.next(8) == 0) {
            Transaction transaction = bus.begin_transaction(names[random.next(8)]);
            for (std::uint32_t i = 2 + random.next(3); i > 0; --i) {
                run_one();
            }
            if (random.next(4) != 0) {
                transaction.commit();
            }
        } else {
            run_one();
        }
    }
}

}  // namespace fem::command::testing
