#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

#if __has_include("../firmware/weather_dash/recovery_tool.h")
#include "../firmware/weather_dash/recovery_tool.h"

namespace {

int failures = 0;

struct FakePreferences {
    bool beginResult = true;
    size_t writeResult = 1U;
    bool storedValue = false;
    bool forceReadMismatch = false;
    bool ended = false;

    bool begin(const char*, bool) { return beginResult; }
    size_t putBool(const char*, bool value) {
        if (writeResult == 1U) storedValue = value;
        return writeResult;
    }
    bool getBool(const char*, bool fallback) {
        if (writeResult != 1U) return fallback;
        return forceReadMismatch ? !storedValue : storedValue;
    }
    void end() { ended = true; }
};

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void testConditionsForThirtyMinutesAndRequestsOneFrc() {
    RecoveryToolState tool;
    check(tool.start(1000U, 1004U, 748U), "recovery starts from idle");
    check(tool.active(), "recovery holds the high-power override while conditioning");
    check(tool.phase() == RecoveryPhase::Conditioning,
          "recovery begins in conditioning");
    check(tool.tick(1000U + RECOVERY_CONDITIONING_MS - 1U) ==
              RecoveryAction::None,
          "FRC is not requested before thirty minutes");
    check(tool.tick(1000U + RECOVERY_CONDITIONING_MS) ==
              RecoveryAction::PerformFrc,
          "FRC is requested at thirty minutes");
    check(tool.tick(1000U + RECOVERY_CONDITIONING_MS + 1U) ==
              RecoveryAction::None,
          "FRC is requested only once");
}

void testObservesForThirtyMinutesAfterSuccessfulFrc() {
    RecoveryToolState tool;
    tool.start(0U, 1007U, 750U);
    check(tool.tick(RECOVERY_CONDITIONING_MS) == RecoveryAction::PerformFrc,
          "test setup reaches FRC");
    tool.markFrcSucceeded(RECOVERY_CONDITIONING_MS + 2000U, 0x7EA2U, 752U);

    check(tool.phase() == RecoveryPhase::Observing,
          "successful FRC begins observation");
    check(tool.active(), "high-power override remains active while observing");

    tool.recordObservation(430U);
    tool.recordObservation(440U);
    tool.recordObservation(470U);

    const RecoveryObservation& observation = tool.observation();
    check(observation.validSamples == 3U, "valid samples are counted");
    check(observation.minimumPpm == 430U, "minimum CO2 is retained");
    check(observation.maximumPpm == 470U, "maximum CO2 is retained");
    check(observation.finalPpm == 470U, "final CO2 is retained");
    check(observation.inZoneSamples == 2U, "in-zone samples are counted");
    check(observation.averagePpm() == 446U,
          "average CO2 is calculated from observed samples");
    check(observation.inZonePercent() == 67U,
          "in-zone percentage is rounded to the nearest percent");

    const uint32_t observationStart = RECOVERY_CONDITIONING_MS + 2000U;
    check(tool.tick(observationStart + RECOVERY_OBSERVATION_MS - 1U) ==
              RecoveryAction::None,
          "observation does not finish early");
    for (uint32_t i = observation.validSamples;
         i < RECOVERY_MIN_OBSERVATION_SAMPLES; ++i) {
        tool.recordObservation(425U);
    }
    check(tool.tick(observationStart + RECOVERY_OBSERVATION_MS) ==
              RecoveryAction::Finish,
          "observation finishes after thirty minutes");
    check(tool.phase() == RecoveryPhase::Complete,
          "completed recovery reports complete");
    check(!tool.active(), "completed recovery releases high-power override");
}

void testInsufficientObservationIsInconclusive() {
    RecoveryToolState tool;
    tool.start(0U, 1000U, 750U);
    tool.tick(RECOVERY_CONDITIONING_MS);
    tool.markFrcSucceeded(RECOVERY_CONDITIONING_MS + 2000U, 0x8000U, 748U);
    tool.recordObservation(425U);
    check(tool.tick(RECOVERY_CONDITIONING_MS + 2000U +
                    RECOVERY_OBSERVATION_MS) == RecoveryAction::Finish,
          "short observation reaches a terminal result");
    check(tool.phase() == RecoveryPhase::Inconclusive,
          "insufficient valid samples are reported as inconclusive");
    check(!tool.active(), "inconclusive observation releases high-power override");
}

void testAmbiguousFrcOutcomeBlocksAnotherRun() {
    RecoveryToolState tool;
    tool.start(0U, 1000U, 750U);
    tool.tick(RECOVERY_CONDITIONING_MS);
    tool.markFrcAttempted();
    tool.markFailed("I2C response failed");
    check(tool.frcOutcome() == RecoveryFrcOutcome::Unknown,
          "an attempted FRC with no confirmed response stays unknown");
    check(!tool.start(RECOVERY_CONDITIONING_MS + 5000U, 1000U, 750U),
          "an unknown persistent FRC outcome blocks another run");
}

void testRejectedFrcAllowsASeparateFutureRun() {
    RecoveryToolState tool;
    tool.start(0U, 1000U, 750U);
    tool.tick(RECOVERY_CONDITIONING_MS);
    tool.markFrcAttempted();
    tool.markFrcRejected();
    check(tool.frcOutcome() == RecoveryFrcOutcome::ConfirmedRejected,
          "0xFFFF records a confirmed rejected FRC");
    check(tool.start(RECOVERY_CONDITIONING_MS + 5000U, 1000U, 750U),
          "a confirmed rejected FRC does not create an unknown-outcome lock");
}

void testFrcLatchMustBeWrittenAndReadBack() {
    FakePreferences success;
    check(persistRecoveryFrcLatch(success, true),
          "verified latch persistence succeeds");
    check(success.ended, "successful latch persistence closes NVS");

    FakePreferences beginFailure;
    beginFailure.beginResult = false;
    check(!persistRecoveryFrcLatch(beginFailure, true),
          "FRC latch fails when NVS cannot open");

    FakePreferences writeFailure;
    writeFailure.writeResult = 0U;
    check(!persistRecoveryFrcLatch(writeFailure, true),
          "FRC latch fails when NVS writes no value");
    check(writeFailure.ended, "failed latch write still closes NVS");

    FakePreferences readbackFailure;
    readbackFailure.forceReadMismatch = true;
    check(!persistRecoveryFrcLatch(readbackFailure, true),
          "FRC latch fails when readback does not match");
}

void testPersistentBlockDoesNotHideConfirmedFrcOutcome() {
    check(std::string(recoveryReportedFrcOutcome(
              RecoveryFrcOutcome::ConfirmedApplied, true)) ==
              "confirmed_applied",
          "failed latch clear does not hide a confirmed applied FRC");
    check(std::string(recoveryReportedFrcOutcome(
              RecoveryFrcOutcome::NotAttempted, true)) == "unknown",
          "boot-time persisted latch reports an unknown FRC outcome");
}

void testMillisWraparoundPreservesThirtyMinuteDeadline() {
    RecoveryToolState tool;
    const uint32_t start = std::numeric_limits<uint32_t>::max() - 1000U;
    tool.start(start, 1000U, 750U);
    check(tool.tick(start + RECOVERY_CONDITIONING_MS - 1U) ==
              RecoveryAction::None,
          "wraparound does not request FRC early");
    check(tool.tick(start + RECOVERY_CONDITIONING_MS) ==
              RecoveryAction::PerformFrc,
          "wraparound requests FRC at thirty minutes");
}

void testFailedFrcIsNotRetried() {
    RecoveryToolState tool;
    tool.start(50U, 1000U, 760U);
    check(tool.tick(50U + RECOVERY_CONDITIONING_MS) ==
              RecoveryAction::PerformFrc,
          "test setup requests FRC");
    tool.markFailed("FRC failed");
    check(tool.phase() == RecoveryPhase::Failed, "FRC failure is retained");
    check(!tool.active(), "failure releases high-power override");
    check(tool.tick(std::numeric_limits<uint32_t>::max()) ==
              RecoveryAction::None,
          "failed FRC is never retried");
}

void testCancelPreservesWhetherFrcWasApplied() {
    RecoveryToolState beforeFrc;
    beforeFrc.start(0U, 1010U, 740U);
    beforeFrc.cancel();
    check(beforeFrc.phase() == RecoveryPhase::Cancelled,
          "conditioning can be cancelled");
    check(!beforeFrc.frcApplied(), "pre-FRC cancellation made no calibration");
    check(!beforeFrc.active(), "cancellation releases high-power override");

    RecoveryToolState afterFrc;
    afterFrc.start(0U, 1010U, 740U);
    afterFrc.tick(RECOVERY_CONDITIONING_MS);
    afterFrc.markFrcSucceeded(RECOVERY_CONDITIONING_MS + 2000U, 0x8001U,
                              741U);
    afterFrc.cancel();
    check(afterFrc.phase() == RecoveryPhase::Cancelled,
          "observation can be cancelled");
    check(afterFrc.frcApplied(),
          "post-FRC cancellation reports the persistent calibration");
}

}  // namespace

int main() {
    testConditionsForThirtyMinutesAndRequestsOneFrc();
    testObservesForThirtyMinutesAfterSuccessfulFrc();
    testInsufficientObservationIsInconclusive();
    testAmbiguousFrcOutcomeBlocksAnotherRun();
    testRejectedFrcAllowsASeparateFutureRun();
    testFrcLatchMustBeWrittenAndReadBack();
    testPersistentBlockDoesNotHideConfirmedFrcOutcome();
    testMillisWraparoundPreservesThirtyMinuteDeadline();
    testFailedFrcIsNotRetried();
    testCancelPreservesWhetherFrcWasApplied();

    if (failures != 0) {
        std::cerr << failures << " recovery-tool assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Recovery Tool state tests passed\n";
    return EXIT_SUCCESS;
}

#else

int main() {
    std::cerr << "FAIL: Recovery Tool production state is not implemented\n";
    return EXIT_FAILURE;
}

#endif
