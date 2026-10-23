#pragma once

// VST3 transport for Brom preset transactions. The processor endpoint owns the
// one brom_preset_tx; the controller endpoint holds copied published state and
// pending request receipts. Copied messages connect them, so neither side holds
// a pointer into the other. pump(), receive() and every connection call run off
// the audio thread; only audioEdit() and finishTimeline() run on it, and neither
// allocates, locks, or notifies.

#include "preset_protocol.hpp"

#include "../preset/catalog_service.h"
#include "../preset/transaction.h"

#include "pluginterfaces/base/smartpointer.h"
#include "pluginterfaces/vst/ivstmessage.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Steinberg::PokustonVst3Support {

using RequestId = uint64_t;       // zero is invalid; never repeats within a connection
using ConnectionEpoch = uint64_t; // changes on every successful new connection

// A process-unique, never-zero epoch for a new connection. Epochs are counted, never
// derived from object addresses, so two instances can never share one.
ConnectionEpoch allocateConnectionEpoch ();

// complete=false means the processor accepted the request. A failure may
// complete immediately. A load or restore completes only once its result is
// published (committed, or pending while the processor is inactive); a request
// that a newer one replaced completes as BROM_PRESET_SUPERSEDED.
struct PresetReceipt {
    RequestId request = 0;
    brom_preset_result result = BROM_PRESET_OK;
    uint64_t generation = 0;
    bool complete = false;
};

// apply is infallible for validated input. A commit applies Working when
// audible_side is 0 and Reference otherwise; apply precedes reset, then note
// clear, then publication.
struct PresetProcessorHooks {
    void* user = nullptr;
    void (*apply) (void*, const brom_preset_snapshot&) = nullptr;
    void (*hardReset) (void*) = nullptr;
    void (*clearNotes) (void*) = nullptr;
};

// Allocates a host message; adapters build this from IHostApplication.
using PresetMessageFactory = std::function<IPtr<Vst::IMessage> ()>;

// Prepares the first min(count, 128) slots of a mapping off the audio thread. A slot
// whose source cannot be read stays in place as BROM_PRESET_NOT_FOUND.
brom_preset_result buildPresetProgramTable (const brom_preset_host_mapping& mapping,
                                            brom_preset_prepare_identity_fn prepare, void* user,
                                            uint64_t mappingGeneration, PresetProgramTable& out);
// The same slots, prepared again: refreshed availability and content with exactly
// the same count and numbering.
brom_preset_result rebuildPresetProgramTable (const PresetProgramTable& previous,
                                              brom_preset_prepare_identity_fn prepare, void* user,
                                              uint64_t mappingGeneration, PresetProgramTable& out);

inline constexpr std::size_t kPresetPendingRequests = 32;
// pump() keeps one terminal bit per slot in a uint64_t.
static_assert (kPresetPendingRequests <= 64, "pump() scans the slots into a 64-bit mask");
// The audio thread reads these without a lock: the echo slots' doubles here and the
// uint64_t counters of transaction.c (_Atomic uint64_t, the same width and type).
static_assert (std::atomic<double>::is_always_lock_free, "audio-thread atomics must not take a lock");
static_assert (std::atomic<uint64_t>::is_always_lock_free, "audio-thread atomics must not take a lock");
inline constexpr std::size_t kPresetOutboxLimit = 128;
inline constexpr uint32_t kPresetDefaultQueueCapacity = 32;

// Shared connection plumbing: a bounded outbox whose entries stay queued until
// notify succeeds, so a failed delivery is retried from pump() with the same
// bytes and the same message sequence.
class PresetEndpoint {
public:
    PresetEndpoint (const PresetEndpoint&) = delete;
    PresetEndpoint& operator= (const PresetEndpoint&) = delete;
    virtual ~PresetEndpoint () = default;

    void setMessageFactory (PresetMessageFactory factory);

protected:
    PresetEndpoint (const brom_preset_schema& schema, const char* productKey);

    // Held for the length of any call that can run on another thread, so destruction
    // can wait for it instead of freeing state under it.
    class CallGuard {
    public:
        explicit CallGuard (PresetEndpoint& endpoint);
        ~CallGuard ();
        bool ok () const { return ok_; }

    private:
        PresetEndpoint& endpoint_;
        bool ok_;
    };
    // Called first by a derived destructor: refuses new calls, waits for the ones in
    // flight (including a pump that is inside notify), and drops queued traffic.
    void shutdown ();

    PresetWireContext contextLocked () const;
    // Stamps epoch and sequence, encodes, and queues. A View replaces any queued
    // View that was not sent yet. Caller holds mutex_.
    brom_preset_result enqueueLocked (PresetWireMessage& message);
    void flushOutgoing ();

    struct Outgoing {
        uint64_t id = 0;
        PresetWireKind kind = PresetWireKind::Reply;
        std::vector<uint8_t> bytes;
    };

    brom_preset_schema schema_ {};
    std::string productKey_;
    ConnectionEpoch epoch_ = 0;
    uint64_t sequence_ = 0;
    mutable std::mutex mutex_;
    IPtr<Vst::IConnectionPoint> peer_;
    PresetMessageFactory factory_;
    std::deque<Outgoing> outgoing_;
    uint64_t nextOutgoingId_ = 0;
    std::atomic_flag flushing_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> closing_ {false};
    std::atomic<uint32_t> calls_ {0};
};

class PresetControllerBridge : public PresetEndpoint {
public:
    PresetControllerBridge (const brom_preset_schema& schema, const char* productKey,
                            const brom_preset_view& initialView);
    ~PresetControllerBridge () override;

    // `epoch` zero adopts the processor's epoch from its welcome.
    brom_preset_result connect (Vst::IConnectionPoint& peer, ConnectionEpoch epoch);
    void disconnect ();

    // Until the handshake completes and a fresh view arrives these return
    // BROM_PRESET_BUSY. A command sent before an earlier load was acknowledged
    // depends on that load's request, so it targets the generation the processor
    // accepts; the controller never guesses the next generation.
    brom_preset_result submitLoad (const brom_preset_snapshot& snapshot,
                                   const brom_preset_identity& identity, RequestId& request);
    brom_preset_result submitCompare (RequestId& request);
    brom_preset_result submitEdit (uint32_t id, double plainValue, RequestId& request);
    brom_preset_result submitSaveAck (const brom_preset_save_completion& completion,
                                      RequestId& request);
    // Releases a terminal receipt (and the processor's matching record).
    brom_preset_result acknowledge (RequestId request);

    // A validated copy of the component's saved view, shown until the processor's
    // own view arrives. It creates no request and sends nothing, and it is refused
    // once a live view is showing, because that one is authoritative.
    brom_preset_result setComponentView (const brom_preset_view& view);

    // Resolves the browser row's pinned identity through the product's preparation
    // callback, then submits the resulting snapshot. Nothing is requested, and no
    // request ID is consumed, unless preparation succeeded.
    brom_preset_result submitBrowserLoad (brom_preset_catalog_service& service, uint64_t token,
                                          uint32_t index,
                                          brom_preset_prepare_identity_fn prepare, void* user,
                                          RequestId& request);
    // Browser pins opened through the controller are released on disconnect, so a
    // closed editor never leaves one behind. The service must outlive the connection.
    brom_preset_result openBrowser (brom_preset_catalog_service& service, uint64_t& token,
                                    uint32_t& count);
    brom_preset_result closeBrowser (uint64_t token);

    // Sends a prepared program list to the processor in bounded pages and retains a
    // copy until disconnect. Each publication carries a fresh mapping generation, so
    // sending a list again after a partial delivery starts a clean assembly.
    brom_preset_result publishProgramTable (const PresetProgramTable& table);
    bool readProgramTable (PresetProgramTable& table) const;

    tresult receive (Vst::IMessage& message);
    bool readView (brom_preset_view& view) const;
    brom_preset_result readReceipt (RequestId request, PresetReceipt& receipt) const;
    void pump ();

    // Connected, welcomed, and holding a fresh view: commands are accepted.
    bool ready () const;
    ConnectionEpoch connectionEpoch () const;

private:
    struct Record {
        bool used = false;
        PresetReceipt receipt;
        bool accepted = false;
    };

    brom_preset_result submitCommand (PresetCommand& command, bool establishesGeneration,
                                      RequestId& request);
    Record* findLocked (RequestId request);
    const Record* findLocked (RequestId request) const;

    PresetProgramTable programTable_;
    bool haveProgramTable_ = false;
    uint64_t mappingGeneration_ = 0;
    uint64_t welcomeFloor_ = 0;       // messages at or below it belong to an older connection
    // Never reset: a welcome from an older connection (lower epoch) or one already
    // adopted (same epoch, not higher sequence) cannot be adopted late.
    uint64_t lastWelcomeEpoch_ = 0;
    uint64_t lastWelcomeSequence_ = 0;
    brom_preset_catalog_service* pinService_ = nullptr;
    std::array<uint64_t, BROM_PRESET_CATALOG_SERVICE_MAX_BROWSERS> pins_ {};
    brom_preset_view view_ {};
    bool haveFreshView_ = false;
    bool welcomed_ = false;
    uint64_t viewSequence_ = 0;
    uint32_t viewRevision_ = 0;
    uint64_t knownGeneration_ = 0;
    RequestId pendingGenerationRequest_ = 0;
    RequestId nextRequest_ = 0;
    std::array<Record, kPresetPendingRequests> records_ {};
};

class PresetProcessorBridge : public PresetEndpoint {
public:
    PresetProcessorBridge (const brom_preset_schema& schema, const char* productKey,
                           const brom_preset_view& initialView, const PresetProcessorHooks& hooks,
                           uint32_t queueCapacity = kPresetDefaultQueueCapacity);
    ~PresetProcessorBridge () override;

    brom_preset_result connect (Vst::IConnectionPoint& peer, ConnectionEpoch epoch);
    void disconnect ();
    tresult receive (Vst::IMessage& message);
    bool handshakeReady () const;

    // setActive is the ownership boundary between the inactive and the active
    // consumer; the VST3 contract never overlaps it with process(). setActive(true)
    // is the single resume function: it applies the pending view through the hooks,
    // publishes it, and only then lets the audio side consume.
    brom_preset_result setActive (bool active);

    // Audio thread. A sample-timed host program value. It resolves from the copied
    // program list the processor holds, so it needs no controller and no disk. The
    // prepared snapshot reaches the DSP through the hooks at the call, and the
    // next boundary publishes it. BROM_PRESET_BUSY until a list has arrived.
    brom_preset_result audioSelectProgram (double normalized);

    // A host that echoes a refreshed value back as a parameter event is handled by an
    // explicit one-shot rule, never by guessing from values: arm the exact id and
    // value off audio, and the first matching audioEdit is taken as that echo. An
    // unconsumed expectation expires at the next boundary.
    brom_preset_result armEcho (uint32_t id, double plainValue);
    std::size_t pendingEchoes () const;

    uint32_t programCount () const;
    brom_preset_result readProgramSlot (uint32_t slot, PresetProgramSlot& out) const;
    // Replaced tables not yet freed because the audio side may still be reading one.
    std::size_t pendingTableReclamations () const;

    // Audio thread. A sample-timed host parameter change; applyToDsp tells the
    // adapter whether to move the DSP now.
    brom_preset_result audioEdit (uint32_t id, double plainValue, bool identifiedEcho,
                                  bool& applyToDsp);
    // Audio thread. Captures the request cutoff after the whole callback timeline,
    // reduces everything before it, runs the hooks, then publishes.
    void finishTimeline ();

    void pump ();
    // Runs inside pump() right after the view is read, to make that interleaving
    // testable. Always declared: a member that only some translation units see changes
    // sizeof(PresetProcessorBridge), and an empty std::function costs one null check
    // per pump(), which runs off the audio thread.
    std::function<void ()> testAfterViewRead;
    bool readView (brom_preset_view& view) const;
    // Host state restore (not a controller command).
    brom_preset_result restore (const brom_preset_view& view, RequestId& request);
    brom_preset_result readReceipt (RequestId request, PresetReceipt& receipt) const;

private:
    enum SlotState : uint32_t { Free = 0, Reserved = 1, Accepted = 2, Terminal = 3 };

    // Written by the message thread at accept and by the audio thread at
    // completion; every shared field is atomic, and the state is the handoff.
    struct Slot {
        std::atomic<uint32_t> state {Free};
        std::atomic<uint64_t> request {0};
        std::atomic<uint64_t> tag {0}; // unique per slot use; what the transaction reports back
        std::atomic<uint64_t> generation {0};
        std::atomic<int> result {BROM_PRESET_OK};
        // Message-thread only:
        uint64_t epoch = 0;           // the connection that accepted it
        uint64_t commandSequence = 0; // sequence of the command that created it
        bool orphan = false;          // its connection ended; reclaimed once terminal
        bool acceptSent = false;
        bool completeSent = false;
    };

    struct EchoSlot {
        std::atomic<uint32_t> state {0}; // 0 free, 2 being written, 1 armed
        std::atomic<uint32_t> id {0};
        std::atomic<double> value {0.0};
    };

    Slot* findSlot (RequestId request);
    const Slot* findSlot (RequestId request) const;
    Slot* reserveSlotLocked (RequestId request, uint64_t commandSequence);
    void orphanSlotsLocked ();
    bool consumeEcho (uint32_t id, double value);
    void expireEchoes ();
    void releaseSlot (Slot& slot);
    void completeSlot (Slot& slot, brom_preset_result result, uint64_t generation);
    void handleCommandLocked (const PresetCommand& command, uint64_t sequence);
    void replyLocked (RequestId request, brom_preset_result result, uint64_t generation,
                      bool complete);
    void applyCommit (brom_preset_commit& commit);
    void completeFromOutcomes (uint32_t count);
    void installTableLocked (PresetProgramTable&& completed);
    void clearTableLocked ();
    // The inactive consumer: reduces whatever is queued through the same finite
    // boundary, with no DSP hooks, and publishes it as pending. Caller holds mutex_
    // and the audio side is not running.
    void drainInactiveLocked ();

    brom_preset_tx* tx_ = nullptr;
    PresetProcessorHooks hooks_;
    std::array<Slot, kPresetPendingRequests> slots_ {};
    std::vector<brom_preset_outcome_record> outcomes_;
    std::atomic<uint64_t> publicationSequence_ {0}; // stamped by every publication
    RequestId nextLocalRequest_ = 0;
    bool helloSeen_ = false;
    bool viewDirty_ = false;
    uint32_t nextViewRevision_ = 0;       // re-sends of the last source view
    uint64_t lastSourceViewSequence_ = 0; // what the transaction's own view said then
    bool viewEverSent_ = false;
    std::atomic<bool> active_ {false};
    uint64_t nextTag_ = 0;
    uint64_t helloFloor_ = 0; // commands at or below it are from before this connection
    uint64_t ackFloor_ = 0;   // commands at or below it were acknowledged and reclaimed
    std::array<EchoSlot, 8> echoes_;

    // The program list is immutable once published. The audio side reads it behind
    // a reader count, so a replaced list is only freed by pump() when no reader
    // can still hold it.
    std::atomic<const PresetProgramTable*> table_ {nullptr};
    mutable std::atomic<uint32_t> tableReaders_ {0};
    std::vector<std::unique_ptr<const PresetProgramTable>> retiredTables_;
    PresetProgramAssembler assembler_;
};

} // namespace Steinberg::PokustonVst3Support
