#pragma once
#include "RevisionPolicy.h"
#include <string>

namespace cpn::windows {
// Synchronous TextChanging model update; visual-tree work belongs to the dispatcher.
struct TestTextChange final {
    enum class Result { unchanged, changed, rejected };
    [[nodiscard]] static Result record(std::string& bytes, std::uint64_t& revision,
                                       std::string new_value, bool mutable_capability) {
        if (bytes == new_value) return Result::unchanged;
        if (!mutable_capability || RevisionPolicy::exhausted(revision)) return Result::rejected;
        const auto successor = RevisionPolicy::next(revision);
        bytes.swap(new_value); revision = successor;
        return Result::changed;
    }
};
}
