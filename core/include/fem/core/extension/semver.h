#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fem::extension {

/// A version in the form of Semantic Versioning 2.0.0: major.minor.patch, an optional pre-release
/// ("-beta.2") and optional build metadata ("+20261012").
class Version {
public:
    /// 0.0.0
    Version() = default;
    Version(std::uint64_t major, std::uint64_t minor, std::uint64_t patch) noexcept
        : major_(major), minor_(minor), patch_(patch) {}

    /// Parses "1.2.3", "1.2.3-rc.1", "1.2.3+build.5". Numbers have no leading zeros; identifiers of
    /// the pre-release and the build are non-empty and made of ASCII letters, digits and '-'.
    /// Throws ExtensionError(invalid_version) with the reason.
    [[nodiscard]] static Version parse(std::string_view text);

    [[nodiscard]] std::uint64_t major() const noexcept { return major_; }
    [[nodiscard]] std::uint64_t minor() const noexcept { return minor_; }
    [[nodiscard]] std::uint64_t patch() const noexcept { return patch_; }
    /// Pre-release without the '-', e.g. "rc.1"; empty for a release.
    [[nodiscard]] const std::string& prerelease() const noexcept { return prerelease_; }
    /// Build metadata without the '+'; empty if none.
    [[nodiscard]] const std::string& build() const noexcept { return build_; }
    [[nodiscard]] bool is_prerelease() const noexcept { return !prerelease_.empty(); }

    /// The version as `parse` reads it.
    [[nodiscard]] std::string to_string() const;

    /// Precedence of Semantic Versioning: numbers, then a pre-release below its release, then
    /// pre-release identifiers one by one. Build metadata is ignored, also by `==`.
    friend std::strong_ordering operator<=>(const Version& a, const Version& b) noexcept;
    friend bool operator==(const Version& a, const Version& b) noexcept {
        return (a <=> b) == std::strong_ordering::equal;
    }

private:
    friend class VersionRange;

    std::uint64_t major_ = 0;
    std::uint64_t minor_ = 0;
    std::uint64_t patch_ = 0;
    std::string prerelease_;
    std::string build_;
};

/// The versions a module accepts: of the platform (`engine`) or of a module it requires. A subset
/// of npm ranges:
///
///     range      := set ('||' set)*
///     set        := comparator (' '+ comparator)*
///     comparator := '*' | op? partial
///     op         := '^' | '~' | '=' | '>' | '>=' | '<' | '<='
///     partial    := N | N.N | N.N.N[-prerelease]
///
/// A version is in the range if it satisfies every comparator of some set. `^1.2` means
/// >=1.2.0 <2.0.0 (`^0.2` means >=0.2.0 <0.3.0), `~1.2` means >=1.2.0 <1.3.0, a partial version
/// alone means all versions starting with it (`1.2` is >=1.2.0 <1.3.0), `*` means any release.
/// As in npm, a pre-release is only in a set that names a pre-release of the same major.minor.patch
/// (`>=2.0.0-rc.1` accepts 2.0.0-rc.2 but not 2.1.0-rc.1).
class VersionRange {
public:
    /// "*": every release.
    VersionRange();

    /// Throws ExtensionError(invalid_version) with the reason.
    [[nodiscard]] static VersionRange parse(std::string_view text);

    [[nodiscard]] bool contains(const Version& version) const;
    /// The range as written, without surrounding spaces.
    [[nodiscard]] const std::string& to_string() const noexcept { return text_; }

    friend bool operator==(const VersionRange& a, const VersionRange& b) noexcept {
        return a.text_ == b.text_;
    }

private:
    struct Comparator {
        enum class Op : std::uint8_t { less, less_equal, greater, greater_equal, equal };
        Op op;
        Version version;
    };
    using Set = std::vector<Comparator>;

    std::vector<Set> sets_;
    std::string text_;
};

}  // namespace fem::extension
