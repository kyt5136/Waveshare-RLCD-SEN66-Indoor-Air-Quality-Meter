#pragma once

#include <stddef.h>
#include <stdint.h>

static constexpr uint32_t RECOVERY_CONDITIONING_MS = 30UL * 60UL * 1000UL;
static constexpr uint32_t RECOVERY_OBSERVATION_MS = 30UL * 60UL * 1000UL;
static constexpr uint32_t RECOVERY_MIN_OBSERVATION_SAMPLES = 1500U;
static constexpr uint16_t RECOVERY_MIN_PPM = 350U;
static constexpr uint16_t RECOVERY_MAX_PPM = 450U;

template <typename PreferencesStore>
bool persistRecoveryFrcLatch(PreferencesStore& store, bool uncertain) {
    if (!store.begin("dash", false)) return false;
    size_t written = store.putBool("rec_frc_unknown", uncertain);
    bool verified = written == sizeof(bool) &&
                    store.getBool("rec_frc_unknown", !uncertain) == uncertain;
    store.end();
    return verified;
}

enum class RecoveryPhase : uint8_t {
    Idle,
    Conditioning,
    FrcExecuting,
    Observing,
    Complete,
    Inconclusive,
    Failed,
    Cancelled,
};

enum class RecoveryFrcOutcome : uint8_t {
    NotAttempted,
    ConfirmedApplied,
    ConfirmedRejected,
    Unknown,
};

enum class RecoveryAction : uint8_t {
    None,
    PerformFrc,
    Finish,
};

struct RecoveryObservation {
    uint32_t validSamples = 0;
    uint32_t inZoneSamples = 0;
    uint64_t sumPpm = 0;
    uint16_t minimumPpm = 0;
    uint16_t maximumPpm = 0;
    uint16_t finalPpm = 0;

    void clear() {
        validSamples = 0;
        inZoneSamples = 0;
        sumPpm = 0;
        minimumPpm = 0;
        maximumPpm = 0;
        finalPpm = 0;
    }

    void record(uint16_t ppm) {
        if (validSamples == 0) {
            minimumPpm = ppm;
            maximumPpm = ppm;
        } else {
            if (ppm < minimumPpm) minimumPpm = ppm;
            if (ppm > maximumPpm) maximumPpm = ppm;
        }
        finalPpm = ppm;
        sumPpm += ppm;
        ++validSamples;
        if (ppm >= RECOVERY_MIN_PPM && ppm <= RECOVERY_MAX_PPM) {
            ++inZoneSamples;
        }
    }

    uint16_t averagePpm() const {
        return validSamples == 0 ? 0U
                                 : static_cast<uint16_t>(sumPpm / validSamples);
    }

    uint8_t inZonePercent() const {
        return validSamples == 0
                   ? 0U
                   : static_cast<uint8_t>(
                         (inZoneSamples * 100U + validSamples / 2U) /
                         validSamples);
    }
};

class RecoveryToolState {
  public:
    bool start(uint32_t nowMs, uint16_t pressureHpa, uint16_t initialCo2Ppm) {
        if (active() || frcOutcome_ == RecoveryFrcOutcome::Unknown) return false;
        phase_ = RecoveryPhase::Conditioning;
        startedMs_ = nowMs;
        observationStartedMs_ = 0;
        pressureHpa_ = pressureHpa;
        initialCo2Ppm_ = initialCo2Ppm;
        preFrcCo2Ppm_ = 0;
        correctionRaw_ = 0;
        frcOutcome_ = RecoveryFrcOutcome::NotAttempted;
        observation_.clear();
        return true;
    }

    RecoveryAction tick(uint32_t nowMs) {
        if (phase_ == RecoveryPhase::Conditioning &&
            elapsed(nowMs, startedMs_) >= RECOVERY_CONDITIONING_MS) {
            phase_ = RecoveryPhase::FrcExecuting;
            return RecoveryAction::PerformFrc;
        }
        if (phase_ == RecoveryPhase::Observing &&
            elapsed(nowMs, observationStartedMs_) >= RECOVERY_OBSERVATION_MS) {
            phase_ = observation_.validSamples >= RECOVERY_MIN_OBSERVATION_SAMPLES
                         ? RecoveryPhase::Complete
                         : RecoveryPhase::Inconclusive;
            return RecoveryAction::Finish;
        }
        return RecoveryAction::None;
    }

    void markFrcSucceeded(uint32_t nowMs, uint16_t correctionRaw,
                          uint16_t preFrcCo2Ppm) {
        correctionRaw_ = correctionRaw;
        preFrcCo2Ppm_ = preFrcCo2Ppm;
        observationStartedMs_ = nowMs;
        frcOutcome_ = RecoveryFrcOutcome::ConfirmedApplied;
        phase_ = RecoveryPhase::Observing;
        observation_.clear();
    }

    void markFrcAttempted() { frcOutcome_ = RecoveryFrcOutcome::Unknown; }

    void markFrcRejected() {
        frcOutcome_ = RecoveryFrcOutcome::ConfirmedRejected;
        phase_ = RecoveryPhase::Failed;
    }

    void markFailed(const char*) { phase_ = RecoveryPhase::Failed; }

    void cancel() {
        if (active()) phase_ = RecoveryPhase::Cancelled;
    }

    void recordObservation(uint16_t ppm) {
        if (phase_ == RecoveryPhase::Observing) observation_.record(ppm);
    }

    bool active() const {
        return phase_ == RecoveryPhase::Conditioning ||
               phase_ == RecoveryPhase::FrcExecuting ||
               phase_ == RecoveryPhase::Observing;
    }

    bool canCancel() const {
        return phase_ == RecoveryPhase::Conditioning ||
               phase_ == RecoveryPhase::Observing;
    }

    RecoveryPhase phase() const { return phase_; }
    uint32_t startedMs() const { return startedMs_; }
    uint32_t observationStartedMs() const { return observationStartedMs_; }
    uint16_t pressureHpa() const { return pressureHpa_; }
    uint16_t initialCo2Ppm() const { return initialCo2Ppm_; }
    uint16_t preFrcCo2Ppm() const { return preFrcCo2Ppm_; }
    uint16_t correctionRaw() const { return correctionRaw_; }
    bool frcApplied() const {
        return frcOutcome_ == RecoveryFrcOutcome::ConfirmedApplied;
    }
    RecoveryFrcOutcome frcOutcome() const { return frcOutcome_; }
    const RecoveryObservation& observation() const { return observation_; }

    static uint32_t elapsed(uint32_t nowMs, uint32_t sinceMs) {
        return static_cast<uint32_t>(nowMs - sinceMs);
    }

  private:
    RecoveryPhase phase_ = RecoveryPhase::Idle;
    uint32_t startedMs_ = 0;
    uint32_t observationStartedMs_ = 0;
    uint16_t pressureHpa_ = 0;
    uint16_t initialCo2Ppm_ = 0;
    uint16_t preFrcCo2Ppm_ = 0;
    uint16_t correctionRaw_ = 0;
    RecoveryFrcOutcome frcOutcome_ = RecoveryFrcOutcome::NotAttempted;
    RecoveryObservation observation_;
};

inline const char* recoveryPhaseName(RecoveryPhase phase) {
    switch (phase) {
        case RecoveryPhase::Conditioning: return "conditioning";
        case RecoveryPhase::FrcExecuting: return "calibrating";
        case RecoveryPhase::Observing: return "observing";
        case RecoveryPhase::Complete: return "complete";
        case RecoveryPhase::Inconclusive: return "inconclusive";
        case RecoveryPhase::Failed: return "failed";
        case RecoveryPhase::Cancelled: return "cancelled";
        default: return "idle";
    }
}

inline const char* recoveryFrcOutcomeName(RecoveryFrcOutcome outcome) {
    switch (outcome) {
        case RecoveryFrcOutcome::ConfirmedApplied: return "confirmed_applied";
        case RecoveryFrcOutcome::ConfirmedRejected: return "confirmed_rejected";
        case RecoveryFrcOutcome::Unknown: return "unknown";
        default: return "not_attempted";
    }
}

inline const char* recoveryReportedFrcOutcome(RecoveryFrcOutcome outcome,
                                               bool persistentBlock) {
    if (persistentBlock && outcome == RecoveryFrcOutcome::NotAttempted) {
        return "unknown";
    }
    return recoveryFrcOutcomeName(outcome);
}
