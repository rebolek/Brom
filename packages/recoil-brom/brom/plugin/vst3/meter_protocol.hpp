#pragma once

// Brom's bounded VST3 meter message contract. Host-specific IMessage allocation,
// delivery and threading remain with the plug-in adapter.
#include "pluginterfaces/vst/ivstmessage.h"
#include "packages/recoil-brom/brom/plugin/meter_api.h"

#include <atomic>
#include <cstdint>

namespace Steinberg::PokustonVst3Support {

inline constexpr const char* kMeterSnapshotMessageId = "brom.meter.snapshot.v2";
inline constexpr const char* kMeterResetMessageId = "brom.meter.reset.v2";

bool writeMeterSnapshotMessage (Vst::IMessage& message,
                                uint64_t connectionEpoch,
                                const brom_meter_snapshot& snapshot);
bool readMeterSnapshotMessage (Vst::IMessage& message,
                               uint64_t& connectionEpoch,
                               brom_meter_snapshot& snapshot);
bool writeMeterResetMessage (Vst::IMessage& message, uint64_t connectionEpoch,
                             uint64_t generation);
bool readMeterResetMessage (Vst::IMessage& message, uint64_t& connectionEpoch,
                            uint64_t& generation);

// Tracks the newest accepted connection identity across disconnects. Reset markers
// may establish a newer epoch; snapshots are accepted only within that epoch.
class MeterConnectionAcceptance {
public:
    void disconnect () noexcept { activeEpoch_ = 0; generation_ = 0; }
    bool acceptReset (uint64_t connectionEpoch, uint64_t generation) noexcept;
    bool acceptsSnapshot (uint64_t connectionEpoch) const noexcept;
    uint64_t activeEpoch () const noexcept { return activeEpoch_; }
private:
    uint64_t activeEpoch_ = 0;
    uint64_t generation_ = 0;
    uint64_t newestEpoch_ = 0;
};

// One outstanding generation/sequence pair is enough: timer callbacks always
// publish the latest Brom mailbox value and naturally coalesce intermediate ones.
class MeterSnapshotPublisher {
public:
    bool needsPublish (const brom_meter_snapshot& snapshot) const noexcept;
    void published (const brom_meter_snapshot& snapshot) noexcept;
    void reset () noexcept;

private:
    std::atomic<uint64_t> generation_ {0};
    std::atomic<uint64_t> sequence_ {0};
};

} // namespace Steinberg::PokustonVst3Support
