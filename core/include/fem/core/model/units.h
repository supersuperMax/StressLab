#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fem::model {

// Units of the four base quantities a model is expressed in. Every other unit (force, stress,
// density, energy...) is derived from them, so a model is consistent by construction.
// The identifiers returned by `symbol()` are stable: they are written to project files.

enum class LengthUnit : std::uint8_t { meter, centimeter, millimeter, micrometer, inch, foot };
enum class MassUnit : std::uint8_t {
    kilogram,
    gram,
    tonne,
    pound,
    slinch,  ///< lbf*s^2/in, the mass unit of the inch-pound-force-second system.
    slug,    ///< lbf*s^2/ft, the mass unit of the foot-pound-force-second system.
};
enum class TimeUnit : std::uint8_t { second, millisecond, microsecond, minute, hour };
enum class TemperatureUnit : std::uint8_t { kelvin, celsius, fahrenheit, rankine };

/// Unit system of a model. Stored explicitly in every document, so exporters can warn when the
/// target expects something else.
struct UnitSystem {
    LengthUnit length = LengthUnit::meter;
    MassUnit mass = MassUnit::kilogram;
    TimeUnit time = TimeUnit::second;
    TemperatureUnit temperature = TemperatureUnit::kelvin;

    friend bool operator==(const UnitSystem&, const UnitSystem&) = default;

    /// m, kg, s, K: force in N, stress in Pa.
    static constexpr UnitSystem si() noexcept { return {}; }
    /// mm, t, s, degC: force in N, stress in MPa, density in t/mm^3.
    static constexpr UnitSystem mm_t_s() noexcept {
        return {LengthUnit::millimeter, MassUnit::tonne, TimeUnit::second,
                TemperatureUnit::celsius};
    }
    /// mm, kg, ms, degC: force in kN, stress in GPa (common in explicit dynamics).
    static constexpr UnitSystem mm_kg_ms() noexcept {
        return {LengthUnit::millimeter, MassUnit::kilogram, TimeUnit::millisecond,
                TemperatureUnit::celsius};
    }
    /// in, slinch, s, degF: force in lbf, stress in psi.
    static constexpr UnitSystem in_slinch_s() noexcept {
        return {LengthUnit::inch, MassUnit::slinch, TimeUnit::second, TemperatureUnit::fahrenheit};
    }
};

/// Physical dimension as exponents of the base quantities: force is {1, 1, -2, 0}.
struct Dimension {
    int length = 0;
    int mass = 0;
    int time = 0;
    int temperature = 0;

    friend bool operator==(const Dimension&, const Dimension&) = default;
};

namespace dimensions {
inline constexpr Dimension dimensionless{};
inline constexpr Dimension length{1, 0, 0, 0};
inline constexpr Dimension mass{0, 1, 0, 0};
inline constexpr Dimension time{0, 0, 1, 0};
inline constexpr Dimension temperature{0, 0, 0, 1};
inline constexpr Dimension force{1, 1, -2, 0};
inline constexpr Dimension pressure{-1, 1, -2, 0};
inline constexpr Dimension density{-3, 1, 0, 0};
inline constexpr Dimension energy{2, 1, -2, 0};
}  // namespace dimensions

/// Size of one unit in SI base units (m, kg, s, K).
/// Temperature factors convert differences; absolute temperatures also need an offset.
[[nodiscard]] double si_factor(LengthUnit unit) noexcept;
[[nodiscard]] double si_factor(MassUnit unit) noexcept;
[[nodiscard]] double si_factor(TimeUnit unit) noexcept;
[[nodiscard]] double si_factor(TemperatureUnit unit) noexcept;

/// Multiplier that converts a value of the given dimension from `units` to SI.
/// For example, a stress in the mm-t-s system times `si_factor(units, dimensions::pressure)`
/// (1e6) is the same stress in Pa.
[[nodiscard]] double si_factor(const UnitSystem& units, Dimension dimension) noexcept;

/// Stable identifiers: "m", "cm", "mm", "um", "in", "ft"; "kg", "g", "t", "lb", "slinch", "slug";
/// "s", "ms", "us", "min", "h"; "K", "degC", "degF", "degR".
[[nodiscard]] std::string_view symbol(LengthUnit unit) noexcept;
[[nodiscard]] std::string_view symbol(MassUnit unit) noexcept;
[[nodiscard]] std::string_view symbol(TimeUnit unit) noexcept;
[[nodiscard]] std::string_view symbol(TemperatureUnit unit) noexcept;

[[nodiscard]] std::optional<LengthUnit> parse_length_unit(std::string_view text) noexcept;
[[nodiscard]] std::optional<MassUnit> parse_mass_unit(std::string_view text) noexcept;
[[nodiscard]] std::optional<TimeUnit> parse_time_unit(std::string_view text) noexcept;
[[nodiscard]] std::optional<TemperatureUnit> parse_temperature_unit(std::string_view text) noexcept;

/// "mm-t-s-degC". For logs and error messages.
[[nodiscard]] std::string to_string(const UnitSystem& units);

}  // namespace fem::model
