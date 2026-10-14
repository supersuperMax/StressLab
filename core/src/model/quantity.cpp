#include "fem/core/model/quantity.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace fem::model {

namespace {

// International yard and pound agreement (1959): exact values.
constexpr double inch = 0.0254;
constexpr double foot = 0.3048;
constexpr double pound = 0.45359237;
constexpr double standard_gravity = 9.80665;
constexpr double pound_force = pound * standard_gravity;
constexpr double kelvin_of_celsius = 273.15;
constexpr double rankine = 5.0 / 9.0;
constexpr double kelvin_of_fahrenheit = 459.67 * rankine;
constexpr double pi = 3.14159265358979323846;

constexpr std::array length_units{
    DisplayUnit{"m", 1.0},   DisplayUnit{"cm", 1e-2}, DisplayUnit{"mm", 1e-3},
    DisplayUnit{"um", 1e-6}, DisplayUnit{"in", inch}, DisplayUnit{"ft", foot},
};
constexpr std::array area_units{
    DisplayUnit{"m2", 1.0},         DisplayUnit{"cm2", 1e-4},       DisplayUnit{"mm2", 1e-6},
    DisplayUnit{"in2", inch* inch}, DisplayUnit{"ft2", foot* foot},
};
constexpr std::array volume_units{
    DisplayUnit{"m3", 1.0},
    DisplayUnit{"l", 1e-3},
    DisplayUnit{"cm3", 1e-6},
    DisplayUnit{"mm3", 1e-9},
    DisplayUnit{"in3", inch* inch* inch},
    DisplayUnit{"ft3", foot* foot* foot},
};
constexpr std::array mass_units{
    DisplayUnit{"kg", 1.0},
    DisplayUnit{"g", 1e-3},
    DisplayUnit{"t", 1e3},
    DisplayUnit{"lb", pound},
    DisplayUnit{"slinch", pound_force / inch},
    DisplayUnit{"slug", pound_force / foot},
};
constexpr std::array time_units{
    DisplayUnit{"s", 1.0},    DisplayUnit{"ms", 1e-3},  DisplayUnit{"us", 1e-6},
    DisplayUnit{"min", 60.0}, DisplayUnit{"h", 3600.0},
};
constexpr std::array temperature_units{
    DisplayUnit{"K", 1.0},
    DisplayUnit{"degC", 1.0, kelvin_of_celsius},
    DisplayUnit{"degF", rankine, kelvin_of_fahrenheit},
    DisplayUnit{"degR", rankine},
};
constexpr std::array temperature_difference_units{
    DisplayUnit{"K", 1.0},
    DisplayUnit{"degC", 1.0},
    DisplayUnit{"degF", rankine},
    DisplayUnit{"degR", rankine},
};
constexpr std::array force_units{
    DisplayUnit{"N", 1.0},   DisplayUnit{"kN", 1e3},          DisplayUnit{"MN", 1e6},
    DisplayUnit{"mN", 1e-3}, DisplayUnit{"lbf", pound_force}, DisplayUnit{"kip", 1e3 * pound_force},
};
constexpr std::array pressure_units{
    DisplayUnit{"Pa", 1.0},
    DisplayUnit{"kPa", 1e3},
    DisplayUnit{"MPa", 1e6},
    DisplayUnit{"GPa", 1e9},
    DisplayUnit{"bar", 1e5},
    DisplayUnit{"psi", pound_force / (inch * inch)},
    DisplayUnit{"ksi", 1e3 * pound_force / (inch * inch)},
};
constexpr std::array density_units{
    DisplayUnit{"kg/m3", 1.0},
    DisplayUnit{"g/cm3", 1e3},
    DisplayUnit{"t/mm3", 1e12},
    DisplayUnit{"kg/mm3", 1e9},
    DisplayUnit{"lb/in3", pound / (inch * inch * inch)},
    DisplayUnit{"slinch/in3", pound_force / (inch * inch * inch * inch)},
};
constexpr std::array energy_units{
    DisplayUnit{"J", 1.0},
    DisplayUnit{"kJ", 1e3},
    DisplayUnit{"mJ", 1e-3},
    DisplayUnit{"in*lbf", inch* pound_force},
    DisplayUnit{"ft*lbf", foot* pound_force},
};
constexpr std::array moment_units{
    DisplayUnit{"N*m", 1.0},
    DisplayUnit{"N*mm", 1e-3},
    DisplayUnit{"kN*m", 1e3},
    DisplayUnit{"lbf*in", inch* pound_force},
    DisplayUnit{"lbf*ft", foot* pound_force},
};
constexpr std::array velocity_units{
    DisplayUnit{"m/s", 1.0},   DisplayUnit{"mm/s", 1e-3}, DisplayUnit{"km/h", 1.0 / 3.6},
    DisplayUnit{"in/s", inch}, DisplayUnit{"ft/s", foot},
};
constexpr std::array acceleration_units{
    DisplayUnit{"m/s2", 1.0},   DisplayUnit{"mm/s2", 1e-3}, DisplayUnit{"g0", standard_gravity},
    DisplayUnit{"in/s2", inch}, DisplayUnit{"ft/s2", foot},
};
constexpr std::array frequency_units{
    DisplayUnit{"Hz", 1.0},
    DisplayUnit{"kHz", 1e3},
    DisplayUnit{"rpm", 1.0 / 60.0},
};
constexpr std::array angle_units{
    DisplayUnit{"rad", 1.0},
    DisplayUnit{"deg", pi / 180.0},
};
constexpr std::array thermal_expansion_units{
    DisplayUnit{"1/K", 1.0},
    DisplayUnit{"1/degC", 1.0},
    DisplayUnit{"1/degF", 1.0 / rankine},
};

constexpr std::array catalog{
    Quantity{"length", dimensions::length, false, length_units},
    Quantity{"area", {2, 0, 0, 0}, false, area_units},
    Quantity{"volume", {3, 0, 0, 0}, false, volume_units},
    Quantity{"mass", dimensions::mass, false, mass_units},
    Quantity{"time", dimensions::time, false, time_units},
    Quantity{"temperature", dimensions::temperature, true, temperature_units},
    Quantity{"temperatureDifference", dimensions::temperature, false, temperature_difference_units},
    Quantity{"force", dimensions::force, false, force_units},
    Quantity{"pressure", dimensions::pressure, false, pressure_units},
    Quantity{"density", dimensions::density, false, density_units},
    Quantity{"energy", dimensions::energy, false, energy_units},
    Quantity{"moment", dimensions::energy, false, moment_units},
    Quantity{"velocity", {1, 0, -1, 0}, false, velocity_units},
    Quantity{"acceleration", {1, 0, -2, 0}, false, acceleration_units},
    Quantity{"frequency", {0, 0, -1, 0}, false, frequency_units},
    Quantity{"angle", dimensions::dimensionless, false, angle_units},
    Quantity{"thermalExpansion", {0, 0, 0, -1}, false, thermal_expansion_units},
};

/// SI value of the zero of an absolute temperature in this unit.
double temperature_offset(TemperatureUnit unit) noexcept {
    switch (unit) {
        case TemperatureUnit::celsius:
            return kelvin_of_celsius;
        case TemperatureUnit::fahrenheit:
            return kelvin_of_fahrenheit;
        case TemperatureUnit::kelvin:
        case TemperatureUnit::rankine:
            break;
    }
    return 0.0;
}

bool same(double a, double b) noexcept {
    return std::abs(a - b) <= 1e-9 * std::max(std::abs(a), std::abs(b));
}

/// "⁻²" for -2.
std::string superscript(int exponent) {
    static constexpr std::array<std::string_view, 10> digits{"⁰", "¹", "²", "³", "⁴",
                                                             "⁵", "⁶", "⁷", "⁸", "⁹"};
    std::string text = exponent < 0 ? "⁻" : "";
    const std::string number = std::to_string(std::abs(exponent));
    for (const char digit : number) {
        text += digits[static_cast<std::size_t>(digit - '0')];
    }
    return text;
}

std::string_view shown_symbol(LengthUnit unit) noexcept {
    return unit == LengthUnit::micrometer ? "µm" : symbol(unit);
}

std::string_view shown_symbol(TemperatureUnit unit) noexcept {
    switch (unit) {
        case TemperatureUnit::celsius:
            return "°C";
        case TemperatureUnit::fahrenheit:
            return "°F";
        case TemperatureUnit::rankine:
            return "°R";
        case TemperatureUnit::kelvin:
            break;
    }
    return "K";
}

}  // namespace

const DisplayUnit* Quantity::find_unit(std::string_view unit) const noexcept {
    for (const DisplayUnit& candidate : units) {
        if (candidate.id == unit) {
            return &candidate;
        }
    }
    return nullptr;
}

std::span<const Quantity> quantities() noexcept {
    return catalog;
}

const Quantity* find_quantity(std::string_view id) noexcept {
    for (const Quantity& quantity : catalog) {
        if (quantity.id == id) {
            return &quantity;
        }
    }
    return nullptr;
}

double to_si(const Quantity& quantity, const UnitSystem& units, double value) noexcept {
    const double si = value * si_factor(units, quantity.dimension);
    return quantity.absolute ? si + temperature_offset(units.temperature) : si;
}

double from_si(const Quantity& quantity, const UnitSystem& units, double value) noexcept {
    const double shifted =
        quantity.absolute ? value - temperature_offset(units.temperature) : value;
    return shifted / si_factor(units, quantity.dimension);
}

const DisplayUnit* model_unit(const Quantity& quantity, const UnitSystem& units) noexcept {
    const double factor = si_factor(units, quantity.dimension);
    const double offset = quantity.absolute ? temperature_offset(units.temperature) : 0.0;
    for (const DisplayUnit& unit : quantity.units) {
        if (same(unit.factor, factor) && same(unit.offset, offset)) {
            return &unit;
        }
    }
    return nullptr;
}

std::string compose_symbol(Dimension dimension, const UnitSystem& units) {
    const std::array<std::pair<std::string_view, int>, 4> parts{{
        {symbol(units.mass), dimension.mass},
        {shown_symbol(units.length), dimension.length},
        {symbol(units.time), dimension.time},
        {shown_symbol(units.temperature), dimension.temperature},
    }};
    std::string text;
    for (const auto& [unit, exponent] : parts) {
        if (exponent == 0) {
            continue;
        }
        if (!text.empty()) {
            text += "·";
        }
        text += unit;
        if (exponent != 1) {
            text += superscript(exponent);
        }
    }
    return text.empty() ? "1" : text;
}

}  // namespace fem::model
