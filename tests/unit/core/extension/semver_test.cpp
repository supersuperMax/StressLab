#include "fem/core/extension/semver.h"

#include "fem/core/extension/error.h"

#include "extension_test_support.h"
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using fem::extension::ExtensionErrc;
using fem::extension::ExtensionError;
using fem::extension::Version;
using fem::extension::VersionRange;

TEST(Version, ParsesSemanticVersions) {
    const Version version = Version::parse("1.22.333-rc.1+build.7");
    EXPECT_EQ(version.major(), 1U);
    EXPECT_EQ(version.minor(), 22U);
    EXPECT_EQ(version.patch(), 333U);
    EXPECT_EQ(version.prerelease(), "rc.1");
    EXPECT_EQ(version.build(), "build.7");
    EXPECT_TRUE(version.is_prerelease());

    for (const char* text :
         {"0.0.0", "1.2.3", "10.20.30", "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-0.3.7",
          "1.0.0-x.7.z.92", "1.0.0-x-y-z.--", "1.0.0+20130313144700", "1.0.0-beta+exp.sha.5114f85",
          "1.0.0+0.build.1-rc.10000aaa-kk-0.1", "18446744073709551615.0.0"}) {
        EXPECT_EQ(Version::parse(text).to_string(), text);
    }
}

TEST(Version, RejectsWhatIsNotASemanticVersion) {
    for (const char* text :
         {"", "1", "1.2", "1.2.3.4", "01.2.3", "1.02.3", "1.2.03", "1.2.3-", "1.2.3-01", "1.2.3+",
          "1.2.3-a..b", "1.2.3+a+b", "v1.2.3", " 1.2.3", "1.2.3 ", "1.2.-3",
          "18446744073709551616.0.0", "1.2.3-é", "1.2.3-a_b"}) {
        EXPECT_EXTENSION_ERROR(static_cast<void>(Version::parse(text)),
                               ExtensionErrc::invalid_version);
    }
    try {
        static_cast<void>(Version::parse("01.2.3"));
        FAIL();
    } catch (const ExtensionError& error) {
        EXPECT_EQ(std::string(error.what()),
                  "'01.2.3' is not valid: the number '01' has a "
                  "leading zero");
    }
}

TEST(Version, OrdersBySemanticVersioningPrecedence) {
    // The example of semver.org, §11, and numbers compared as numbers.
    const std::vector<Version> ordered = {
        Version::parse("1.0.0-alpha"),
        Version::parse("1.0.0-alpha.1"),
        Version::parse("1.0.0-alpha.beta"),
        Version::parse("1.0.0-beta"),
        Version::parse("1.0.0-beta.2"),
        Version::parse("1.0.0-beta.11"),
        Version::parse("1.0.0-rc.1"),
        Version::parse("1.0.0"),
        Version::parse("1.0.1"),
        Version::parse("1.9.0"),
        Version::parse("1.10.0"),
        Version::parse("2.0.0"),
    };
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        for (std::size_t j = 0; j < ordered.size(); ++j) {
            EXPECT_EQ(ordered[i] < ordered[j], i < j)
                << ordered[i].to_string() << " vs " << ordered[j].to_string();
            EXPECT_EQ(ordered[i] == ordered[j], i == j);
        }
    }
    EXPECT_EQ(Version::parse("1.0.0+a"), Version::parse("1.0.0+b"));
    EXPECT_EQ(Version(1, 2, 3), Version::parse("1.2.3"));
}

struct RangeCase {
    const char* range;
    const char* version;
    bool contains;
};

TEST(VersionRange, ContainsTheVersionsOfNpmRanges) {
    const std::vector<RangeCase> cases = {
        // Caret: compatible with the leftmost non-zero part.
        {"^1.2.3", "1.2.3", true},
        {"^1.2.3", "1.9.9", true},
        {"^1.2.3", "2.0.0", false},
        {"^1.2.3", "1.2.2", false},
        {"^0.2.3", "0.2.9", true},
        {"^0.2.3", "0.3.0", false},
        {"^0.0.3", "0.0.3", true},
        {"^0.0.3", "0.0.4", false},
        {"^1.2", "1.2.0", true},
        {"^1.2", "2.0.0", false},
        {"^0.1", "0.1.5", true},
        {"^0.1", "0.2.0", false},
        {"^0.0", "0.0.9", true},
        {"^0.0", "0.1.0", false},
        {"^1", "1.9.0", true},
        {"^0", "0.9.9", true},
        {"^0", "1.0.0", false},
        // Tilde: patch updates.
        {"~1.2.3", "1.2.9", true},
        {"~1.2.3", "1.3.0", false},
        {"~1.2", "1.2.0", true},
        {"~1", "1.9.9", true},
        {"~1", "2.0.0", false},
        // A version alone, full or partial.
        {"1.2.3", "1.2.3", true},
        {"=1.2.3", "1.2.4", false},
        {"1.2", "1.2.7", true},
        {"1.2", "1.3.0", false},
        {"1", "1.5.0", true},
        {"1.2.3", "1.2.3+abc", true},
        // Comparisons.
        {">1.2.3", "1.2.4", true},
        {">1.2.3", "1.2.3", false},
        {">1.2", "1.2.9", false},
        {">1.2", "1.3.0", true},
        {">1", "2.0.0", true},
        {">1", "1.9.9", false},
        {">=1.2", "1.2.0", true},
        {">=1.2", "1.1.9", false},
        {">= 1.2", "1.2.0", true},
        {"<1.2", "1.1.9", true},
        {"<1.2", "1.2.0", false},
        {"<=1.2", "1.2.9", true},
        {"<=1.2", "1.3.0", false},
        {"<=1.2.3", "1.2.3", true},
        {"*", "0.0.0", true},
        {"*", "99.1.2", true},
        // Sets and alternatives.
        {">=1.0 <2", "1.5.0", true},
        {">=1.0 <2", "2.0.0", false},
        {"^1.0 || ^3.0", "2.5.0", false},
        {"^1.0 || ^3.0", "3.1.0", true},
        {"^1.0||^3.0", "1.1.0", true},
        // Pre-releases only where a comparator names one of the same version.
        {"^1.0", "1.5.0-rc.1", false},
        {"^1.0", "2.0.0-rc.1", false},
        {"*", "1.0.0-alpha", false},
        {">=2.0.0-rc.1", "2.0.0-rc.2", true},
        {">=2.0.0-rc.1", "2.0.0", true},
        {">=2.0.0-rc.1", "2.1.0-rc.1", false},
        {"1.0.0-rc.1", "1.0.0-rc.1", true},
        {"^1.2.3-beta.2", "1.2.3-beta.4", true},
        {"^1.2.3-beta.2", "1.2.4-beta.1", false},
        {"^1.2.3-beta.2", "1.3.0", true},
    };
    for (const RangeCase& c : cases) {
        EXPECT_EQ(VersionRange::parse(c.range).contains(Version::parse(c.version)), c.contains)
            << c.range << " contains " << c.version;
    }
    EXPECT_TRUE(VersionRange().contains(Version(7, 0, 0)));
    EXPECT_EQ(VersionRange().to_string(), "*");
    EXPECT_EQ(VersionRange::parse("  ^1.2 || ~3  ").to_string(), "^1.2 || ~3");
}

TEST(VersionRange, RejectsWhatIsNotARange) {
    for (const char* text : {"", "  ", "^", ">= ", "~>1.2", "1.2.3.4", "^1.x", ">=a", "1.2 ||",
                             "|| 1.2", "1.2 | 1.3", "^01.2", "1.2-rc", "=>1.0", "1.2,1.3"}) {
        EXPECT_EXTENSION_ERROR(static_cast<void>(VersionRange::parse(text)),
                               ExtensionErrc::invalid_version);
    }
}

}  // namespace
