#include "fem/core/form/status.h"

namespace fem::form {

FormIndex::FormIndex(const extension::ContributionRegistry& registry) {
    for (const extension::PropertySchemaContribution* schema : registry.property_schemas()) {
        forms_[schema->owner].insert_or_assign(schema->type,
                                               PropertyForm{.owner = schema->owner,
                                                            .type = schema->type,
                                                            .version = schema->version,
                                                            .form = parse(schema->schema)});
    }
}

const PropertyForm* FormIndex::find(std::string_view owner, std::string_view type) const {
    const auto by_owner = forms_.find(owner);
    if (by_owner == forms_.end()) {
        return nullptr;
    }
    const auto found = by_owner->second.find(type);
    return found == by_owner->second.end() ? nullptr : &found->second;
}

std::vector<Problem> object_problems(const model::Document& document, const FormIndex& forms,
                                     model::ObjectId id) {
    const model::Object& object = document.get(id);
    std::vector<Problem> problems;
    if (const PropertyForm* form = forms.find(object.owner(), object.type())) {
        problems = check(form->form, object.properties());
        if (object.schema_version() != form->version) {
            problems.push_back(
                {.code = ProblemCode::schema_version,
                 .severity = Severity::warning,
                 .pointer = {},
                 .title = {},
                 .detail = {{"object", object.schema_version()}, {"schema", form->version}}});
        }
    }
    for (const model::ObjectId missing : document.missing_dependencies(id)) {
        problems.push_back({.code = ProblemCode::missing_dependency,
                            .severity = Severity::error,
                            .pointer = {},
                            .title = {},
                            .detail = {{"object", model::to_string(missing)}}});
    }
    return problems;
}

}  // namespace fem::form
