#include "fem/core/model/units.h"

#include <gtest/gtest.h>

#include <set>
#include <string_view>

using namespace fem::model;

TEST(Units, SiIsTheDefault) {
    const UnitSystem units;
    EXPECT_EQ(units, UnitSystem::si());
    EXPECT_EQ(to_string(units), "m-kg-s-K");
    EXPECT_DOUBLE_EQ(si_factor(units, dimensions::force), 1.0);
    EXPECT_DOUBLE_EQ(si_factor(units, dimensions::pressure), 1.0);
}

TEST(Units, MillimeterTonneSecondGivesNewtonAndMegapascal) {
    const UnitSystem units = UnitSystem::mm_t_s();
    EXPECT_EQ(to_string(units), "mm-t-s-degC");
    EXPECT_DOUBLE_EQ(si_factor(units, dimensions::length), 1e-3);
    EXPECT_DOUBLE_EQ(si_factor(units, dimensions::force), 1.0);
    EXPECT_DOUBLE_EQ(si_factor(units, dimensions::pressure), 1e6);
    EXPECT_DOUBLE_EQ(si_factor(units, dimensions::density), 1e12);  // t/mm^3 -> kg/m^3
    EXPECT_DOUBLE_EQ(si_factor(units, dimensions::energy), 1e-3);   // N*mm -> J
}

TEST(Units, MillimeterKilogramMillisecondGivesKilonewtonAndGigapascal) {
    const UnitSystem units = UnitSystem::mm_kg_ms();
    EXPECT_DOUBLE_EQ(si_factor(units, dimensions::force), 1e3);
    EXPECT_DOUBLE_EQ(si_factor(units, dimensions::pressure), 1e9);
}

TEST(Units, InchSlinchSecondGivesPoundForceAndPsi) {
    const UnitSystem units = UnitSystem::in_slinch_s();
    constexpr double pound_force = 4.4482216152605;  // N, exact by definition
    EXPECT_DOUBLE_EQ(si_factor(units, dimensions::force), pound_force);
    EXPECT_DOUBLE_EQ(si_factor(units, dimensions::pressure), pound_force / (0.0254 * 0.0254));
    EXPECT_DOUBLE_EQ(si_factor(MassUnit::slug), pound_force / 0.3048);
}

TEST(Units, TemperatureFactorsConvertDifferences) {
    EXPECT_DOUBLE_EQ(si_factor(TemperatureUnit::celsius), 1.0);
    EXPECT_DOUBLE_EQ(si_factor(TemperatureUnit::fahrenheit), 5.0 / 9.0);
    EXPECT_DOUBLE_EQ(si_factor(TemperatureUnit::rankine), 5.0 / 9.0);
    // Thermal expansion coefficient, 1/degF -> 1/K.
    EXPECT_DOUBLE_EQ(si_factor(UnitSystem::in_slinch_s(), Dimension{0, 0, 0, -1}), 9.0 / 5.0);
}

TEST(Units, DimensionlessFactorIsOne) {
    EXPECT_DOUBLE_EQ(si_factor(UnitSystem::in_slinch_s(), dimensions::dimensionless), 1.0);
}

TEST(Units, SymbolsAreUniqueAndParseBack) {
    std::set<std::string_view> symbols;
    for (const LengthUnit unit : {LengthUnit::meter, LengthUnit::centimeter, LengthUnit::millimeter,
                                  LengthUnit::micrometer, LengthUnit::inch, LengthUnit::foot}) {
        EXPECT_TRUE(symbols.insert(symbol(unit)).second);
        EXPECT_EQ(parse_length_unit(symbol(unit)), unit);
    }
    for (const MassUnit unit : {MassUnit::kilogram, MassUnit::gram, MassUnit::tonne,
                                MassUnit::pound, MassUnit::slinch, MassUnit::slug}) {
        EXPECT_TRUE(symbols.insert(symbol(unit)).second);
        EXPECT_EQ(parse_mass_unit(symbol(unit)), unit);
    }
    for (const TimeUnit unit : {TimeUnit::second, TimeUnit::millisecond, TimeUnit::microsecond,
                                TimeUnit::minute, TimeUnit::hour}) {
        EXPECT_TRUE(symbols.insert(symbol(unit)).second);
        EXPECT_EQ(parse_time_unit(symbol(unit)), unit);
    }
    for (const TemperatureUnit unit : {TemperatureUnit::kelvin, TemperatureUnit::celsius,
                                       TemperatureUnit::fahrenheit, TemperatureUnit::rankine}) {
        EXPECT_TRUE(symbols.insert(symbol(unit)).second);
        EXPECT_EQ(parse_temperature_unit(symbol(unit)), unit);
    }
}

TEST(Units, UnknownSymbolsAreRejected) {
    EXPECT_EQ(parse_length_unit("MM"), std::nullopt);
    EXPECT_EQ(parse_length_unit(""), std::nullopt);
    EXPECT_EQ(parse_mass_unit("ton"), std::nullopt);
    EXPECT_EQ(parse_time_unit("sec"), std::nullopt);
    EXPECT_EQ(parse_temperature_unit("C"), std::nullopt);
}
