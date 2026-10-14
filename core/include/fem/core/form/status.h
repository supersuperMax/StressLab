#pragma once

#include "fem/core/extension/registry.h"
#include "fem/core/form/form.h"
#include "fem/core/model/document.h"
#include "fem/core/model/object_id.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// What is wrong with an object of the model, for the ⚠/✕ marks of the model tree
// (docs/interface-design.md §4, §11; docs/adr/0014-tree-and-forms.md).
namespace fem::form {

/// The property form of an object type, read from a `propertySchemas` contribution.
struct PropertyForm {
    std::string owner;
    std::string type;
    std::uint32_t version = 1;
    FormSpec form;
};

/// The forms of the object types of the enabled modules, read once from a registry. Build a new
/// one when modules are enabled or disabled.
class FormIndex {
public:
    FormIndex() = default;
    explicit FormIndex(const extension::ContributionRegistry& registry);

    /// The form of objects of `type` owned by `owner`, or nullptr.
    [[nodiscard]] const PropertyForm* find(std::string_view owner, std::string_view type) const;

private:
    /// By owner, then type.
    std::map<std::string, std::map<std::string, PropertyForm, std::less<>>, std::less<>> forms_;
};

/// The problems of an object: its properties against its form (see
/// `check`), a warning if the object was written with another version of the schema than its form
/// describes, and an error for every object it depends on that has been removed. An object of a
/// type without a form has only the last two.
[[nodiscard]] std::vector<Problem> object_problems(const model::Document& document,
                                                   const FormIndex& forms, model::ObjectId id);

}  // namespace fem::form
