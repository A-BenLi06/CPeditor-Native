// SPDX-License-Identifier: GPL-3.0-or-later
#include "../CommittedTextState.h"
#include <iostream>
#include <new>
#include <stdexcept>

namespace {
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void transactions() {
    CPNWinUIEdit::CommittedTextState state;
    expect(state.available(true) && state.revision() == 0, "Initial committed state");
    state.text_changed();
    expect(state.revision() == 1, "Ordinary edit revision");
    state.begin_composition("before");
    state.text_changed(); state.text_changed();
    expect(!state.available(true) && state.composing() && state.revision() == 1,
           "Preedit exposed as committed text");
    state.end_composition();
    expect(!state.composing() && !state.available(true), "End callback bypassed native drain barrier");
    state.settle(false, "after");
    expect(!state.available(true) && state.revision() == 1, "Locked document settled prematurely");
    // Modified notifications may be delivered after OnEndComposition while the TSF lock is held.
    state.text_changed(); state.text_changed();
    expect(state.revision() == 1, "Deferred composition notifications advanced committed revision");
    state.settle(true, "after");
    expect(state.available(true) && state.revision() == 2, "Final composition not committed once");
    state.settle(true, "after");
    expect(state.revision() == 2, "Duplicate barrier advanced revision");
    state.begin_composition("after"); state.text_changed(); state.end_composition();
    state.settle(true, "after");
    expect(state.revision() == 2 && state.available(true), "Cancelled composition advanced revision");
    state.begin_composition(std::string("a\0b", 3));
    state.begin_composition("nested must not replace the original baseline");
    state.end_composition(); state.settle(true, "partial");
    expect(!state.available(true), "Nested composition exposed a partial snapshot");
    state.end_composition(); state.settle(true, std::string("a\0b", 3));
    expect(state.available(true) && state.revision() == 2, "Nested cancellation/NUL baseline changed");
    state.begin_composition("next"); state.end_composition();
    // A new accepted composition during queue processing remains in the same unsettled transaction.
    state.begin_composition("temporary"); state.end_composition(); state.settle(true, "next");
    expect(state.available(true) && state.revision() == 2, "Reentrant composition replaced the baseline");
    // A TSF TIP may SetText before OnStartComposition within the same granted write session.
    state.begin_native_update("session-original");
    state.text_changed();
    state.begin_composition("preedit already inserted");
    state.text_changed(); state.end_composition(); state.settle(false, "session-original");
    expect(state.revision() == 2 && !state.available(true), "Write-session baseline committed too early");
    state.settle(true, "session-original");
    expect(state.revision() == 2 && state.available(true), "Pre-start SetText cancellation advanced revision");
    state.begin_native_update("old"); state.text_changed(); state.text_changed();
    state.settle(true, "new");
    expect(state.revision() == 3 && state.available(true), "Ordinary native write session net change");
    state.finalize();
    expect(!state.available(true), "Finalized control produced a snapshot");
    CPNWinUIEdit::CommittedTextState unbalanced;
    unbalanced.end_composition(); unbalanced.settle(true, "");
    expect(!unbalanced.available(true), "Unbalanced input callbacks failed open");
    unbalanced.begin_native_update("faulted baseline");
    unbalanced.begin_composition("faulted input still active");
    expect(unbalanced.composing() && !unbalanced.tracking_available() && !unbalanced.available(true),
           "Snapshot fault suppressed or hid native composition");
    unbalanced.text_changed(); unbalanced.end_composition();
    expect(!unbalanced.composing() && unbalanced.revision() == 0 && !unbalanced.available(true),
           "Faulted input escaped snapshot barrier");
}
void exception_boundaries() {
    CPNWinUIEdit::CommittedTextState allocation;
    bool last = true;
    allocation.begin_native_update("before");
    std::size_t capture_calls = 0;
    std::size_t unavailable_calls = 0;
    const auto capture = [&]() -> std::string { ++capture_calls; throw std::bad_alloc{}; };
    const auto notify = [&](bool available) { if (!available) ++unavailable_calls; };
    static_assert(noexcept(allocation.publish_availability(true, last, capture, notify)));
    allocation.publish_availability(true, last, capture, notify);
    expect(!last && !allocation.tracking_available() && unavailable_calls == 1,
           "Snapshot allocation exception escaped or failed open");
    allocation.publish_availability(true, last, capture, notify);
    expect(capture_calls == 1, "Faulted tracking repeated full-document allocation");
    allocation.begin_composition("still accepts native composition");
    expect(allocation.composing() && !allocation.available(true), "Fault blocked or exposed preedit");
    allocation.end_composition();

    CPNWinUIEdit::CommittedTextState consumer;
    bool consumer_last = false;
    std::size_t callbacks = 0;
    consumer.publish_availability(true, consumer_last, [] { return std::string{}; },
        [&](bool) { ++callbacks; throw std::runtime_error("Consumer callback failed"); });
    expect(!consumer_last && !consumer.available(true) && callbacks == 2,
           "Consumer exception escaped input boundary");
    consumer.finalize();
    consumer.publish_availability(true, consumer_last, [] { throw std::bad_alloc{}; return std::string{}; },
        [](bool) { throw std::runtime_error("Teardown callback failed"); });
    expect(!consumer.available(true), "Finalize callback failed open");
}
}
int main() {
    try { transactions(); exception_boundaries(); std::cout << "committed text state tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
