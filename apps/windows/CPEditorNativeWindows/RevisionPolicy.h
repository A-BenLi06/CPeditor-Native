#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace cpn::windows {
struct RevisionPolicy {
    [[nodiscard]] static constexpr bool exhausted(std::uint64_t revision) noexcept {
        return revision == std::numeric_limits<std::uint64_t>::max();
    }
    [[nodiscard]] static constexpr bool final_revision_pending(std::uint64_t revision) noexcept {
        return revision == std::numeric_limits<std::uint64_t>::max() - 1;
    }
    [[nodiscard]] static constexpr bool tests_mutable(std::uint64_t workspace, std::uint64_t tests) noexcept {
        return !exhausted(workspace) && !exhausted(tests);
    }
    [[nodiscard]] static std::uint64_t next(std::uint64_t revision) {
        if (exhausted(revision)) throw std::overflow_error("The persisted revision has no successor.");
        return revision + 1;
    }
    [[nodiscard]] static std::uint64_t persistence_revision(std::uint64_t revision, bool changed) {
        return changed ? next(revision) : revision;
    }
    [[nodiscard]] static std::uint64_t document(std::uint64_t native, std::uint64_t native_base, std::uint64_t document_base) {
        if (native < native_base || native - native_base > std::numeric_limits<std::uint64_t>::max() - document_base)
            throw std::overflow_error("The native revision exceeds the document's remaining budget.");
        return native - native_base + document_base;
    }
};
}
