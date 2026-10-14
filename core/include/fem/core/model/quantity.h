#pragma once

#include "fem/core/model/units.h"

#include <span>
#include <string>
#include <string_view>

// Physical quantities of property values (docs/adr/0014-tree-and-forms.md): a property with
// `x-quantity` in its schema holds a value in the unit system of the model, and forms show it in a
// unit the user picks from the units of its quantity. Conversions go through SI.
namespace fem::model {

/// A unit a value of a quantity can be shown in. A value `v` in this unit is `v * factor + offset`
/// in SI; the offset is not zero only for absolute temperatures in °C and °F.
struct DisplayUnit {
    /// Stable ASCII id, e.g. "MPa", "kg/m3", "N*m". Its label for users is the string
    /// "unit.<id>" of the catalogs of the core ("МПа" in Russian).
    std::string_view id;
    double factor = 1.0;
    double offset = 0.0;

    [[nodiscard]] double to_si(double value) const noexcept { return value * factor + offset; }
    [[nodiscard]] double from_si(double value) const noexcept { return (value - offset) / factor; }
};

/// A physical quantity: its dimension and the units it is shown in, the SI unit first.
struct Quantity {
    /// Stable id used in schemas: "length", "pressure", "temperature"...
    std::string_view id;
    Dimension dimension;
    /// An absolute temperature: converted with the offset of the temperature unit of the model.
    /// "temperatureDifference" and derived quantities (thermal expansion) convert without it.
    bool absolute = false;
    std::span<const DisplayUnit> units;

    /// The unit with this id, or nullptr.
    [[nodiscard]] const DisplayUnit* find_unit(std::string_view unit) const noexcept;
};

/// Every quantity of the catalog, in a fixed order.
[[nodiscard]] std::span<const Quantity> quantities() noexcept;
/// The quantity with this id, or nullptr.
[[nodiscard]] const Quantity* find_quantity(std::string_view id) noexcept;

/// A value of `quantity` given in the unit system `units`, in SI.
[[nodiscard]] double to_si(const Quantity& quantity, const UnitSystem& units,
                           double value) noexcept;
/// A value of `quantity` in SI, in the unit system `units`.
[[nodiscard]] double from_si(const Quantity& quantity, const UnitSystem& units,
                             double value) noexcept;

/// The unit of the catalog that values of `quantity` have in the unit system `units` (MPa for a
/// pressure in mm-t-s); nullptr if the catalog has none, see `compose_symbol`.
[[nodiscard]] const DisplayUnit* model_unit(const Quantity& quantity,
                                            const UnitSystem& units) noexcept;

/// The unit of a dimension written with the base units of a system, for a unit the catalog does
/// not have: "t·mm⁻¹·s⁻²"; "1" for a dimensionless quantity. Not translated: base unit symbols are
/// the same in every language the application has.
[[nodiscard]] std::string compose_symbol(Dimension dimension, const UnitSystem& units);

}  // namespace fem::model
