#include "fem/core/model/units.h"

#include <array>
#include <utility>

namespace fem::model {

namespace {

// International yard and pound agreement (1959): exact values.
constexpr double inch = 0.0254;
constexpr double foot = 0.3048;
constexpr double pound = 0.45359237;
constexpr double standard_gravity = 9.80665;
constexpr double pound_force = pound * standard_gravity;

template <class Unit, std::size_t N>
std::optional<Unit> parse(const std::array<std::pair<Unit, std::string_view>, N>& table,
                          std::string_view text) noexcept {
    for (const auto& [unit, name] : table) {
        if (name == text) {
            return unit;
        }
    }
    return std::nullopt;
}

template <class Unit, std::size_t N>
std::string_view lookup(const std::array<std::pair<Unit, std::string_view>, N>& table,
                        Unit unit) noexcept {
    for (const auto& [candidate, name] : table) {
        if (candidate == unit) {
            return name;
        }
    }
    return {};
}

constexpr std::array<std::pair<LengthUnit, std::string_view>, 6> length_symbols{{
    {LengthUnit::meter, "m"},
    {LengthUnit::centimeter, "cm"},
    {LengthUnit::millimeter, "mm"},
    {LengthUnit::micrometer, "um"},
    {LengthUnit::inch, "in"},
    {LengthUnit::foot, "ft"},
}};

constexpr std::array<std::pair<MassUnit, std::string_view>, 6> mass_symbols{{
    {MassUnit::kilogram, "kg"},
    {MassUnit::gram, "g"},
    {MassUnit::tonne, "t"},
    {MassUnit::pound, "lb"},
    {MassUnit::slinch, "slinch"},
    {MassUnit::slug, "slug"},
}};

constexpr std::array<std::pair<TimeUnit, std::string_view>, 5> time_symbols{{
    {TimeUnit::second, "s"},
    {TimeUnit::millisecond, "ms"},
    {TimeUnit::microsecond, "us"},
    {TimeUnit::minute, "min"},
    {TimeUnit::hour, "h"},
}};

constexpr std::array<std::pair<TemperatureUnit, std::string_view>, 4> temperature_symbols{{
    {TemperatureUnit::kelvin, "K"},
    {TemperatureUnit::celsius, "degC"},
    {TemperatureUnit::fahrenheit, "degF"},
    {TemperatureUnit::rankine, "degR"},
}};

double power(double base, int exponent) noexcept {
    double result = 1.0;
    for (int i = 0; i < exponent; ++i) {
        result *= base;
    }
    for (int i = 0; i > exponent; --i) {
        result /= base;
    }
    return result;
}

}  // namespace

double si_factor(LengthUnit unit) noexcept {
    switch (unit) {
        case LengthUnit::meter:
            return 1.0;
        case LengthUnit::centimeter:
            return 1e-2;
        case LengthUnit::millimeter:
            return 1e-3;
        case LengthUnit::micrometer:
            return 1e-6;
        case LengthUnit::inch:
            return inch;
        case LengthUnit::foot:
            return foot;
    }
    return 1.0;
}

double si_factor(MassUnit unit) noexcept {
    switch (unit) {
        case MassUnit::kilogram:
            return 1.0;
        case MassUnit::gram:
            return 1e-3;
        case MassUnit::tonne:
            return 1e3;
        case MassUnit::pound:
            return pound;
        case MassUnit::slinch:
            return pound_force / inch;
        case MassUnit::slug:
            return pound_force / foot;
    }
    return 1.0;
}

double si_factor(TimeUnit unit) noexcept {
    switch (unit) {
        case TimeUnit::second:
            return 1.0;
        case TimeUnit::millisecond:
            return 1e-3;
        case TimeUnit::microsecond:
            return 1e-6;
        case TimeUnit::minute:
            return 60.0;
        case TimeUnit::hour:
            return 3600.0;
    }
    return 1.0;
}

double si_factor(TemperatureUnit unit) noexcept {
    switch (unit) {
        case TemperatureUnit::kelvin:
        case TemperatureUnit::celsius:
            return 1.0;
        case TemperatureUnit::fahrenheit:
        case TemperatureUnit::rankine:
            return 5.0 / 9.0;
    }
    return 1.0;
}

double si_factor(const UnitSystem& units, Dimension dimension) noexcept {
    return power(si_factor(units.length), dimension.length) *
           power(si_factor(units.mass), dimension.mass) *
           power(si_factor(units.time), dimension.time) *
           power(si_factor(units.temperature), dimension.temperature);
}

std::string_view symbol(LengthUnit unit) noexcept {
    return lookup(length_symbols, unit);
}

std::string_view symbol(MassUnit unit) noexcept {
    return lookup(mass_symbols, unit);
}

std::string_view symbol(TimeUnit unit) noexcept {
    return lookup(time_symbols, unit);
}

std::string_view symbol(TemperatureUnit unit) noexcept {
    return lookup(temperature_symbols, unit);
}

std::optional<LengthUnit> parse_length_unit(std::string_view text) noexcept {
    return parse(length_symbols, text);
}

std::optional<MassUnit> parse_mass_unit(std::string_view text) noexcept {
    return parse(mass_symbols, text);
}

std::optional<TimeUnit> parse_time_unit(std::string_view text) noexcept {
    return parse(time_symbols, text);
}

std::optional<TemperatureUnit> parse_temperature_unit(std::string_view text) noexcept {
    return parse(temperature_symbols, text);
}

std::string to_string(const UnitSystem& units) {
    std::string result(symbol(units.length));
    result += '-';
    result += symbol(units.mass);
    result += '-';
    result += symbol(units.time);
    result += '-';
    result += symbol(units.temperature);
    return result;
}

}  // namespace fem::model
