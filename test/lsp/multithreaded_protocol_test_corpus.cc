#include "doctest.h"
// ^ Violates linting rules, so include first.
#include "absl/strings/match.h"
#include "common/common.h"
#include "common/sort.h"
#include "test/helpers/lsp.h"
#include "test/lsp/ProtocolTest.h"

namespace sorbet::test::lsp {
using namespace std;
using namespace sorbet::realmain::lsp;

namespace {
/*
optional<SorbetTypecheckRunStatus> getTypecheckRunStatus(const LSPMessage &msg) {
    if (msg.isNotification() && msg.method() == LSPMethod::SorbetTypecheckRunInfo) {
        auto &typecheckInfo = get<unique_ptr<SorbetTypecheckRunInfo>>(msg.asNotification().params);
        return typecheckInfo->status;
    }
    return nullopt;
}
*/
/*void sortTimersByStartTime(vector<unique_ptr<CounterImpl::Timing>> &times) {
    fast_sort(times, [](const auto &a, const auto &b) -> bool { return a->start.usec < b->start.usec; });
}*/
/*
void checkDiagnosticTimes(vector<unique_ptr<CounterImpl::Timing>> times, size_t expectedSize,
                          bool assertUniqueStartTimes) {
    CHECK_EQ(times.size(), expectedSize);
    sortTimersByStartTime(times);

    if (assertUniqueStartTimes) {
        // No two diagnostic latency timers should have the same start timestamp.
        for (size_t i = 1; i < times.size(); i++) {
            CHECK_LT(times[i - 1]->start.usec, times[i]->start.usec);
        }
    }
}
*/
// The resolution of MONOTONIC_COARSE seems to be ~1ms, so we use >1ms delay to ensure unique clock values.
// constexpr auto timestampGranularity = chrono::milliseconds(2);

class MultithreadedProtocolTest : public ProtocolTest {
public:
    MultithreadedProtocolTest() : ProtocolTest(/*multithreading*/ true, /*caching*/ false) {}
};

} // namespace

TEST_CASE_FIXTURE(MultithreadedProtocolTest, "MultithreadedWrapperWorks") {
    assertDiagnostics(initializeLSP(), {});

    sendAsync(LSPMessage(make_unique<NotificationMessage>("2.0", LSPMethod::PAUSE, nullopt)));
    sendAsync(*openFile("yolo1.rb", "# typed: true\nclass Foo2\n  def branch\n    2 + \"dog\"\n  end\nend\n"));
    // Pause to differentiate message times.
    sendAsync(*changeFile("yolo1.rb", "# typed: true\nclass Foo1\n  def branch\n    1 + \"bear\"\n  end\nend\n", 3));

    // Pause so that all latency timers for the above operations get reported.

    assertDiagnostics(send(LSPMessage(make_unique<NotificationMessage>("2.0", LSPMethod::RESUME, nullopt))),
                      {{"yolo1.rb", 3, "bear"}});

    /*   auto counters = getCounters();
       CHECK_EQ(counters.getCategoryCounter("lsp.messages.processed", "sorbet.workspaceEdit"), 1);
       CHECK_EQ(counters.getTimings("task_latency", {{"method", "sorbet.workspaceEdit"}}).size(), 1);
       CHECK_EQ(counters.getTimings("task_latency").size(), counters.getHistogramCount("task_latency"));
       CHECK_EQ(counters.getCategoryCounterSum("lsp.messages.canceled"), 0);
       CHECK_EQ(counters.getCategoryCounter("lsp.updates", "slowpath"), 1);
       CHECK_EQ(counters.getCategoryCounterSum("lsp.updates"), 1);
       // 1 per edit
       //
       */
    // checkDiagnosticTimes(counters.getTimings("last_diagnostic_latency"), 2, /* assertUniqueStartTimes */ false);
}

} // namespace sorbet::test::lsp
