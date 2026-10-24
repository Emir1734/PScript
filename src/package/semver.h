#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pscript::package {

struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;

    static std::optional<SemVer> parse(std::string_view text);
    std::string to_string() const;
    int compare(const SemVer& other) const;
};

enum class VersionConstraintKind {
    Exact,
    Caret,
    Tilde,
};

struct VersionConstraint {
    VersionConstraintKind kind = VersionConstraintKind::Exact;
    SemVer version{};
    std::string raw;

    static std::optional<VersionConstraint> parse(std::string_view text);
    bool matches(const SemVer& candidate) const;
};

bool select_highest_matching(const std::vector<SemVer>& versions, const VersionConstraint& constraint,
                             SemVer* out);

}  // namespace pscript::package
