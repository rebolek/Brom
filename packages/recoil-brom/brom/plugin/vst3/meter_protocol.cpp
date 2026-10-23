#include "meter_protocol.hpp"

#include <cmath>
#include <cstring>

namespace Steinberg::PokustonVst3Support {

bool writeMeterSnapshotMessage (Vst::IMessage& message,
                                uint64_t connectionEpoch,
                                const brom_meter_snapshot& snapshot) {
    auto* attributes = message.getAttributes ();
    if (!attributes || !std::isfinite (snapshot.excitation) ||
        !std::isfinite (snapshot.output) || snapshot.excitation < 0.0 || snapshot.excitation > 1.0 ||
        snapshot.output < 0.0 || snapshot.output > 1.0 ||
        snapshot.excitation_hold_ns < 0 || snapshot.output_hold_ns < 0 ||
        snapshot.observed_ns < 0 || snapshot.sequence == 0 || snapshot.generation == 0 ||
        connectionEpoch == 0 || connectionEpoch > INT64_MAX ||
        snapshot.sequence > INT64_MAX || snapshot.generation > INT64_MAX) return false;
    message.setMessageID (kMeterSnapshotMessageId);
    return attributes->setFloat ("excitation", snapshot.excitation) == kResultOk &&
        attributes->setFloat ("output", snapshot.output) == kResultOk &&
        attributes->setInt ("excitation-hold-ns", snapshot.excitation_hold_ns) == kResultOk &&
        attributes->setInt ("output-hold-ns", snapshot.output_hold_ns) == kResultOk &&
        attributes->setInt ("observed-ns", snapshot.observed_ns) == kResultOk &&
        attributes->setInt ("sequence", static_cast<int64> (snapshot.sequence)) == kResultOk &&
        attributes->setInt ("generation", static_cast<int64> (snapshot.generation)) == kResultOk &&
        attributes->setInt ("connection-epoch", static_cast<int64> (connectionEpoch)) == kResultOk;
}

bool readMeterSnapshotMessage (Vst::IMessage& message,
                               uint64_t& connectionEpoch,
                               brom_meter_snapshot& snapshot) {
    const char* messageId = message.getMessageID ();
    if (!messageId || std::strcmp (messageId, kMeterSnapshotMessageId) != 0) return false;
    auto* attributes = message.getAttributes ();
    if (!attributes) return false;
    brom_meter_snapshot decoded {};
    int64 sequence = 0, generation = 0, epoch = 0;
    if (attributes->getFloat ("excitation", decoded.excitation) != kResultOk ||
        attributes->getFloat ("output", decoded.output) != kResultOk ||
        attributes->getInt ("excitation-hold-ns", decoded.excitation_hold_ns) != kResultOk ||
        attributes->getInt ("output-hold-ns", decoded.output_hold_ns) != kResultOk ||
        attributes->getInt ("observed-ns", decoded.observed_ns) != kResultOk ||
        attributes->getInt ("sequence", sequence) != kResultOk || sequence <= 0 ||
        attributes->getInt ("generation", generation) != kResultOk || generation <= 0 ||
        attributes->getInt ("connection-epoch", epoch) != kResultOk || epoch <= 0 ||
        !std::isfinite (decoded.excitation) || !std::isfinite (decoded.output) ||
        decoded.excitation < 0.0 || decoded.excitation > 1.0 ||
        decoded.output < 0.0 || decoded.output > 1.0 ||
        decoded.excitation_hold_ns < 0 || decoded.output_hold_ns < 0 ||
        decoded.observed_ns < 0) return false;
    decoded.sequence = static_cast<uint64_t> (sequence);
    decoded.generation = static_cast<uint64_t> (generation);
    connectionEpoch = static_cast<uint64_t> (epoch);
    snapshot = decoded;
    return true;
}

bool writeMeterResetMessage (Vst::IMessage& message, uint64_t connectionEpoch,
                             uint64_t generation) {
    auto* attributes = message.getAttributes ();
    if (!attributes || connectionEpoch == 0 || connectionEpoch > INT64_MAX ||
        generation == 0 || generation > INT64_MAX) return false;
    message.setMessageID (kMeterResetMessageId);
    return attributes->setInt ("generation", static_cast<int64> (generation)) == kResultOk &&
        attributes->setInt ("connection-epoch", static_cast<int64> (connectionEpoch)) == kResultOk;
}

bool readMeterResetMessage (Vst::IMessage& message, uint64_t& connectionEpoch,
                            uint64_t& generation) {
    const char* messageId = message.getMessageID ();
    if (!messageId || std::strcmp (messageId, kMeterResetMessageId) != 0) return false;
    auto* attributes = message.getAttributes ();
    int64 decoded = 0, epoch = 0;
    if (!attributes || attributes->getInt ("generation", decoded) != kResultOk || decoded <= 0 ||
        attributes->getInt ("connection-epoch", epoch) != kResultOk || epoch <= 0)
        return false;
    connectionEpoch = static_cast<uint64_t> (epoch);
    generation = static_cast<uint64_t> (decoded);
    return true;
}

bool MeterConnectionAcceptance::acceptReset (uint64_t epoch, uint64_t generation) noexcept {
    if (epoch == 0 || generation == 0 || epoch < newestEpoch_) return false;
    if (activeEpoch_ == 0 && epoch == newestEpoch_) return false;
    if (epoch == activeEpoch_ && generation <= generation_) return false;
    activeEpoch_ = epoch;
    newestEpoch_ = epoch;
    generation_ = generation;
    return true;
}

bool MeterConnectionAcceptance::acceptsSnapshot (uint64_t epoch) const noexcept {
    return epoch != 0 && epoch == activeEpoch_;
}

bool MeterSnapshotPublisher::needsPublish (
    const brom_meter_snapshot& snapshot) const noexcept {
    return snapshot.generation != generation_.load (std::memory_order_acquire) ||
        snapshot.sequence != sequence_.load (std::memory_order_acquire);
}

void MeterSnapshotPublisher::published (
    const brom_meter_snapshot& snapshot) noexcept {
    sequence_.store (snapshot.sequence, std::memory_order_relaxed);
    generation_.store (snapshot.generation, std::memory_order_release);
}

void MeterSnapshotPublisher::reset () noexcept {
    sequence_.store (0, std::memory_order_relaxed);
    generation_.store (0, std::memory_order_release);
}

} // namespace Steinberg::PokustonVst3Support
