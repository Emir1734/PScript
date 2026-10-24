#include "package/semver.h"

#include <charconv>
#include <sstream>

namespace pscript::package {
namespace {

std::optional<int> parse_int_component(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    int value = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    if (value < 0) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

std::optional<SemVer> SemVer::parse(std::string_view text) {
    SemVer out;
    const std::size_t dot1 = text.find('.');
    if (dot1 == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t dot2 = text.find('.', dot1 + 1);
    if (dot2 == std::string_view::npos) {
        return std::nullopt;
    }
    const auto major = parse_int_component(text.substr(0, dot1));
    const auto minor = parse_int_component(text.substr(dot1 + 1, dot2 - dot1 - 1));
    const auto patch = parse_int_component(text.substr(dot2 + 1));
    if (!major || !minor || !patch) {
        return std::nullopt;
    }
    out.major = *major;
    out.minor = *minor;
    out.patch = *patch;
    return out;
}

std::string SemVer::to_string() const {
    return std::to_string(major) + '.' + std::to_string(minor) + '.' + std::to_string(patch);
}

int SemVer::compare(const SemVer& other) const {
    if (major != other.major) {
        return major < other.major ? -1 : 1;
    }
    if (minor != other.minor) {
        return minor < other.minor ? -1 : 1;
    }
    if (patch != other.patch) {
        return patch < other.patch ? -1 : 1;
    }
    return 0;
}

std::optional<VersionConstraint> VersionConstraint::parse(std::string_view text) {
    VersionConstraint out;
    out.raw = std::string(text);
    std::string_view body = text;
    if (body.starts_with('^')) {
        out.kind = VersionConstraintKind::Caret;
        body.remove_prefix(1);
    } else if (body.starts_with('~')) {
        out.kind = VersionConstraintKind::Tilde;
        body.remove_prefix(1);
    } else {
        out.kind = VersionConstraintKind::Exact;
    }
    const auto version = SemVer::parse(body);
    if (!version) {
        return std::nullopt;
    }
    out.version = *version;
    return out;
}

bool VersionConstraint::matches(const SemVer& candidate) const {
    switch (kind) {
        case VersionConstraintKind::Exact:
            return candidate.compare(version) == 0;
        case VersionConstraintKind::Caret: {
            if (candidate.major != version.major) {
                return false;
            }
            if (version.major == 0) {
                return candidate.minor == version.minor && candidate.patch >= version.patch;
            }
            return candidate.compare(version) >= 0;
        }
        case VersionConstraintKind::Tilde:
            return candidate.major == version.major && candidate.minor == version.minor &&
                   candidate.patch >= version.patch;
    }
    return false;
}

bool select_highest_matching(const std::vector<SemVer>& versions, const VersionConstraint& constraint,
                             SemVer* out) {
    bool found = false;
    SemVer best{};
    for (const SemVer& version : versions) {
        if (!constraint.matches(version)) {
            continue;
        }
        if (!found || version.compare(best) > 0) {
            best = version;
            found = true;
        }
    }
    if (!found) {
        return false;
    }
    *out = best;
    return true;
}

}  // namespace pscript::package
