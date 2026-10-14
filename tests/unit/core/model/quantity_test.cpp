// Physical quantities of property values: units to show them in and conversions through SI.

#include "fem/core/model/quantity.h"

#include "fem/core/l10n/localization.h"
#include "fem/core/model/object.h"
#include "fem/core/model/units.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <string_view>

using fem::model::compose_symbol;
using fem::model::DisplayUnit;
using fem::model::find_quantity;
using fem::model::model_unit;
using fem::model::Quantity;
using fem::model::UnitSystem;

namespace {

const Quantity& quantity(std::string_view id) {
    const Quantity* found = find_quantity(id);
    EXPECT_NE(found, nullptr) << id;
    return *found;
}

std::string_view model_unit_id(std::string_view id, const UnitSystem& units) {
    const DisplayUnit* unit = model_unit(quantity(id), units);
    return unit == nullptr ? std::string_view("-") : unit->id;
}

// Text as UTF-8 bytes. GoogleTest from vcpkg is built as C++17 and cannot print char8_t strings.
std::string utf8(std::u8string_view text) {
    return {text.begin(), text.end()};
}

}  // namespace

TEST(Quantity, CatalogHasUniqueIdsAndTheSiUnitFirst) {
    std::set<std::string_view> ids;
    for (const Quantity& entry : fem::model::quantities()) {
        EXPECT_TRUE(ids.insert(entry.id).second) << entry.id;
        ASSERT_FALSE(entry.units.empty()) << entry.id;
        EXPECT_EQ(entry.units.front().factor, 1.0) << entry.id;
        EXPECT_EQ(entry.units.front().offset, 0.0) << entry.id;
        std::set<std::string_view> units;
        for (const DisplayUnit& unit : entry.units) {
            EXPECT_TRUE(units.insert(unit.id).second) << entry.id << " " << unit.id;
            EXPECT_EQ(entry.find_unit(unit.id), &unit);
        }
    }
    EXPECT_EQ(find_quantity("stiffness"), nullptr);
    EXPECT_EQ(quantity("pressure").find_unit("bananas"), nullptr);
}

TEST(Quantity, EveryUnitHasALabelInEnglishAndRussian) {
    fem::l10n::Localization localization;
    for (const std::string language : {"en", "ru"}) {
        localization.set_language(language);
        for (const Quantity& entry : fem::model::quantities()) {
            EXPECT_TRUE(
                localization.find(fem::model::core_owner, "quantity." + std::string(entry.id)))
                << language << " " << entry.id;
            for (const DisplayUnit& unit : entry.units) {
                EXPECT_TRUE(
                    localization.find(fem::model::core_owner, "unit." + std::string(unit.id)))
                    << language << " " << unit.id;
            }
        }
    }
    localization.set_language("ru");
    EXPECT_EQ(localization.text(fem::model::core_owner, "unit.MPa"), utf8(u8"МПа"));
}

TEST(Quantity, DisplayUnitsConvertThroughSi) {
    const DisplayUnit& mpa = *quantity("pressure").find_unit("MPa");
    EXPECT_DOUBLE_EQ(mpa.to_si(210000.0), 2.1e11);
    EXPECT_DOUBLE_EQ(mpa.from_si(2.1e11), 210000.0);
    const DisplayUnit& psi = *quantity("pressure").find_unit("psi");
    EXPECT_NEAR(psi.to_si(1.0), 6894.757293168361, 1e-9);
    const DisplayUnit& celsius = *quantity("temperature").find_unit("degC");
    EXPECT_DOUBLE_EQ(celsius.to_si(20.0), 293.15);
    const DisplayUnit& fahrenheit = *quantity("temperature").find_unit("degF");
    EXPECT_NEAR(fahrenheit.from_si(273.15), 32.0, 1e-12);
    EXPECT_NEAR(fahrenheit.from_si(373.15), 212.0, 1e-12);
    // A difference of temperatures has no offset.
    EXPECT_DOUBLE_EQ(quantity("temperatureDifference").find_unit("degC")->to_si(20.0), 20.0);
    EXPECT_NEAR(quantity("angle").find_unit("deg")->to_si(180.0), 3.14159265358979, 1e-12);
}

TEST(Quantity, ModelValuesConvertWithTheUnitSystemOfTheModel) {
    const UnitSystem mm = UnitSystem::mm_t_s();
    EXPECT_DOUBLE_EQ(fem::model::to_si(quantity("pressure"), mm, 210000.0), 2.1e11);
    EXPECT_DOUBLE_EQ(fem::model::from_si(quantity("density"), mm, 7850.0), 7.85e-9);
    // mm-t-s has temperatures in °C: absolute ones shift, differences and expansion do not.
    EXPECT_DOUBLE_EQ(fem::model::to_si(quantity("temperature"), mm, 20.0), 293.15);
    EXPECT_DOUBLE_EQ(fem::model::from_si(quantity("temperature"), mm, 293.15), 20.0);
    EXPECT_DOUBLE_EQ(fem::model::to_si(quantity("temperatureDifference"), mm, 20.0), 20.0);
    EXPECT_DOUBLE_EQ(fem::model::to_si(quantity("thermalExpansion"), mm, 1.2e-5), 1.2e-5);
    const UnitSystem in = UnitSystem::in_slinch_s();
    EXPECT_NEAR(fem::model::from_si(quantity("temperature"), in, 293.15), 68.0, 1e-12);
    EXPECT_NEAR(fem::model::from_si(quantity("thermalExpansion"), in, 1.8e-5), 1.0e-5, 1e-18);
}

TEST(Quantity, TheUnitOfTheModelIsFoundInTheCatalog) {
    const UnitSystem si = UnitSystem::si();
    const UnitSystem mm = UnitSystem::mm_t_s();
    const UnitSystem in = UnitSystem::in_slinch_s();
    EXPECT_EQ(model_unit_id("pressure", si), "Pa");
    EXPECT_EQ(model_unit_id("pressure", mm), "MPa");
    EXPECT_EQ(model_unit_id("pressure", in), "psi");
    EXPECT_EQ(model_unit_id("force", mm), "N");
    EXPECT_EQ(model_unit_id("force", in), "lbf");
    EXPECT_EQ(model_unit_id("density", si), "kg/m3");
    EXPECT_EQ(model_unit_id("density", mm), "t/mm3");
    EXPECT_EQ(model_unit_id("density", in), "slinch/in3");
    EXPECT_EQ(model_unit_id("energy", mm), "mJ");
    EXPECT_EQ(model_unit_id("moment", mm), "N*mm");
    EXPECT_EQ(model_unit_id("temperature", mm), "degC");
    EXPECT_EQ(model_unit_id("temperature", in), "degF");
    EXPECT_EQ(model_unit_id("temperatureDifference", in), "degF");
    EXPECT_EQ(model_unit_id("thermalExpansion", in), "1/degF");
    EXPECT_EQ(model_unit_id("angle", mm), "rad");
    // mm-kg-ms: stress in GPa, force in kN, velocity in mm/ms = m/s.
    const UnitSystem explicit_dynamics = UnitSystem::mm_kg_ms();
    EXPECT_EQ(model_unit_id("pressure", explicit_dynamics), "GPa");
    EXPECT_EQ(model_unit_id("force", explicit_dynamics), "kN");
    EXPECT_EQ(model_unit_id("density", explicit_dynamics), "kg/mm3");
    EXPECT_EQ(model_unit_id("velocity", explicit_dynamics), "m/s");
    // A system in hours: the catalog has no acceleration in m/h².
    EXPECT_EQ(
        model_unit_id("acceleration", {fem::model::LengthUnit::meter,
                                       fem::model::MassUnit::kilogram, fem::model::TimeUnit::hour}),
        "-");
}

TEST(Quantity, UnitsOutsideTheCatalogAreComposedOfBaseUnits) {
    const UnitSystem mm = UnitSystem::mm_t_s();
    EXPECT_EQ(compose_symbol(fem::model::dimensions::pressure, mm), utf8(u8"t·mm⁻¹·s⁻²"));
    EXPECT_EQ(compose_symbol({0, 0, 0, -1}, mm), utf8(u8"°C⁻¹"));
    EXPECT_EQ(compose_symbol({1, 0, 0, 0}, {fem::model::LengthUnit::micrometer}), utf8(u8"µm"));
    EXPECT_EQ(compose_symbol(fem::model::dimensions::dimensionless, mm), "1");
    EXPECT_EQ(compose_symbol({-12, 0, 0, 0}, mm), utf8(u8"mm⁻¹²"));
}
