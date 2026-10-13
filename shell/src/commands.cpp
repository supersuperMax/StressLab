#include "fem/shell/commands.h"

#include "fem/core/log/log.h"
#include "fem/shell/stages.h"
#include "fem/shell/strings.h"

#include <QStringList>
#include <exception>
#include <string>

namespace fem::shell {

QString Commands::title(std::string_view id) const {
    if (const extension::CommandContribution* command =
            workbench_.extensions.registry().find_command(id)) {
        return title(*command);
    }
    return to_qstring(id);
}

QString Commands::title(const extension::CommandContribution& command) const {
    return to_qstring(workbench_.localization.manifest_text(command.owner, command.title));
}

QString Commands::full_title(const extension::CommandContribution& command) const {
    if (command.category.empty()) {
        return title(command);
    }
    return text(
        "command.withCategory",
        {{"category", workbench_.localization.manifest_text(command.owner, command.category)},
         {"title", workbench_.localization.manifest_text(command.owner, command.title)}});
}

Availability Commands::availability(std::string_view id) const {
    const command::CommandSpec* spec = workbench_.bus.find_command(id);
    if (spec == nullptr) {
        return {.available = false, .reason = text("reason.unknownCommand")};
    }
    if (const extension::CommandContribution* command =
            workbench_.extensions.registry().find_command(id);
        command != nullptr && command->enablement) {
        const context::ContextKeys& keys = workbench_.keys;
        const std::vector<context::UnmetCondition> unmet = command->enablement->unmet(
            [&keys](std::string_view key) -> const context::ContextValue& {
                return keys.value(key);
            });
        if (!unmet.empty()) {
            QStringList conditions;
            for (const context::UnmetCondition& condition : unmet) {
                conditions.push_back(describe(condition));
            }
            return {.available = false,
                    .reason = text(
                        "reason.requires",
                        {{"conditions", conditions.join(QStringLiteral(", ")).toStdString()}})};
        }
    }
    QStringList needed;
    for (const command::ArgSpec& arg : spec->args) {
        if (arg.required) {
            needed.push_back(to_qstring(arg.name));
        }
    }
    if (!needed.isEmpty()) {
        return {.available = false,
                .reason = text("reason.arguments",
                               {{"names", needed.join(QStringLiteral(", ")).toStdString()}})};
    }
    return {};
}

QString Commands::describe(const context::UnmetCondition& condition) const {
    using Kind = context::UnmetCondition::Kind;
    const l10n::Localization& localization = workbench_.localization;
    switch (condition.kind) {
        case Kind::key:
            if (const auto found = localization.find(strings_owner, "condition." + condition.key)) {
                return to_qstring(*found);
            }
            break;
        case Kind::not_key:
            if (const auto found =
                    localization.find(strings_owner, "condition.not." + condition.key)) {
                return to_qstring(*found);
            }
            break;
        case Kind::equals:
            if (condition.key == stage_key &&
                condition.literal.kind() == context::ContextValue::Kind::string) {
                return text("condition.stage",
                            {{"stage", stage_title(localization, workbench_.extensions.registry(),
                                                   condition.literal.as_string())
                                           .toStdString()}});
            }
            break;
        default:
            break;
    }
    return to_qstring(condition.text);
}

bool Commands::run(std::string_view id) {
    const auto log = log::logger("shell");
    const Availability availability = this->availability(id);
    if (!availability.available) {
        log->warn("command '{}' is not available: {}", id, availability.reason.toStdString());
        report(text("prompt.commandUnavailable", {{"command", title(id).toStdString()},
                                                  {"reason", availability.reason.toStdString()}}));
        return false;
    }
    try {
        (void)workbench_.bus.execute(id);
        return true;
    } catch (const std::exception& error) {
        log->error("command '{}' failed: {}", id, error.what());
        report(text("prompt.commandFailed", {{"command", title(id).toStdString()},
                                             {"message", std::string(error.what())}}));
        return false;
    }
}

QString Commands::text(std::string_view key, const l10n::Args& args) const {
    return shell::text(workbench_.localization, key, args);
}

void Commands::report(const QString& message) const {
    if (report_) {
        report_(message);
    }
}

}  // namespace fem::shell
