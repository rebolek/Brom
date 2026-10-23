#include "preset_bridge.hpp"

#include "../preset/session.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <utility>

namespace Steinberg::PokustonVst3Support {

namespace {

// Processor-originated requests (host state restore) live in the same slot table
// as controller requests; the top bit keeps their IDs out of the controller's range.
constexpr RequestId kLocalRequestBit = 1ull << 63;

bool isLocalRequest (RequestId request) { return (request & kLocalRequestBit) != 0; }

brom_preset_result resultForOutcome (const brom_preset_outcome_record& record) {
    switch (record.outcome) {
    case BROM_PRESET_OUTCOME_APPLIED:
    case BROM_PRESET_OUTCOME_ALREADY_APPLIED: return BROM_PRESET_OK;
    case BROM_PRESET_OUTCOME_STALE: return BROM_PRESET_STALE;
    case BROM_PRESET_OUTCOME_SUPERSEDED: return BROM_PRESET_SUPERSEDED;
    case BROM_PRESET_OUTCOME_DROPPED:
        return record.reason == BROM_PRESET_OK ? BROM_PRESET_INVALID : record.reason;
    case BROM_PRESET_OUTCOME_DEFERRED: break;
    }
    return BROM_PRESET_OK;
}

} // namespace

// ---- program tables ----------------------------------------------------------

brom_preset_result buildPresetProgramTable (const brom_preset_host_mapping& mapping,
                                            brom_preset_prepare_identity_fn prepare, void* user,
                                            uint64_t mappingGeneration, PresetProgramTable& out) {
    if (!prepare || mappingGeneration == 0) return BROM_PRESET_INVALID;
    PresetProgramTable table;
    table.mappingGeneration = mappingGeneration;
    const uint32_t count = brom_preset_catalog_service_program_slot_count (&mapping);
    for (uint32_t index = 0; index < count; ++index) {
        brom_preset_catalog_entry entry {};
        const auto read = brom_preset_host_mapping_read (&mapping, index, &entry);
        if (read != BROM_PRESET_OK) return read;
        PresetProgramSlot slot;
        slot.identity = entry.identity;
        brom_preset_snapshot snapshot {};
        if (prepare (user, &slot.identity, &snapshot) == BROM_PRESET_OK) {
            slot.availability = BROM_PRESET_OK;
            slot.snapshot = snapshot;
        }
        table.slots.push_back (slot);
    }
    out = std::move (table);
    return BROM_PRESET_OK;
}

brom_preset_result rebuildPresetProgramTable (const PresetProgramTable& previous,
                                              brom_preset_prepare_identity_fn prepare, void* user,
                                              uint64_t mappingGeneration, PresetProgramTable& out) {
    if (!prepare || mappingGeneration == 0) return BROM_PRESET_INVALID;
    PresetProgramTable table;
    table.mappingGeneration = mappingGeneration;
    for (const auto& old : previous.slots) {
        PresetProgramSlot slot;
        slot.identity = old.identity;
        brom_preset_snapshot snapshot {};
        if (prepare (user, &slot.identity, &snapshot) == BROM_PRESET_OK) {
            slot.availability = BROM_PRESET_OK;
            slot.snapshot = snapshot;
        }
        table.slots.push_back (slot);
    }
    out = std::move (table);
    return BROM_PRESET_OK;
}

ConnectionEpoch allocateConnectionEpoch () {
    static std::atomic<ConnectionEpoch> next {1};
    return next.fetch_add (1, std::memory_order_relaxed);
}

// ---- shared plumbing ---------------------------------------------------------

PresetEndpoint::CallGuard::CallGuard (PresetEndpoint& endpoint) : endpoint_ (endpoint), ok_ (true) {
    endpoint_.calls_.fetch_add (1);
    if (endpoint_.closing_.load ()) {
        endpoint_.calls_.fetch_sub (1);
        ok_ = false;
    }
}

PresetEndpoint::CallGuard::~CallGuard () {
    if (ok_) endpoint_.calls_.fetch_sub (1);
}

void PresetEndpoint::shutdown () {
    closing_.store (true);
    while (calls_.load () > 0) std::this_thread::yield ();
    std::lock_guard<std::mutex> lock (mutex_);
    outgoing_.clear ();
    peer_ = nullptr;
}

PresetEndpoint::PresetEndpoint (const brom_preset_schema& schema, const char* productKey)
: schema_ (schema), productKey_ (productKey ? productKey : "") {}

void PresetEndpoint::setMessageFactory (PresetMessageFactory factory) {
    std::lock_guard<std::mutex> lock (mutex_);
    factory_ = std::move (factory);
}

PresetWireContext PresetEndpoint::contextLocked () const {
    PresetWireContext context;
    context.schema = &schema_;
    context.productKey = productKey_.c_str ();
    context.epoch = epoch_;
    return context;
}

brom_preset_result PresetEndpoint::enqueueLocked (PresetWireMessage& message) {
    message.epoch = epoch_;
    message.sequence = sequence_ + 1;
    std::vector<uint8_t> bytes;
    const auto encoded = encodePresetMessage (contextLocked (), message, bytes);
    if (encoded != BROM_PRESET_OK) return encoded;
    if (message.kind == PresetWireKind::View)
        outgoing_.erase (std::remove_if (outgoing_.begin (), outgoing_.end (),
                                         [] (const Outgoing& item) {
                                             return item.kind == PresetWireKind::View;
                                         }),
                         outgoing_.end ());
    if (outgoing_.size () >= kPresetOutboxLimit) return BROM_PRESET_CAPACITY;
    sequence_ = message.sequence;
    Outgoing item;
    item.id = ++nextOutgoingId_;
    item.kind = message.kind;
    item.bytes = std::move (bytes);
    outgoing_.push_back (std::move (item));
    return BROM_PRESET_OK;
}

// Entries are removed only after notify succeeds, so a failed send retries the
// same bytes. notify may re-enter receive() on this endpoint, so no lock is held
// across it.
void PresetEndpoint::flushOutgoing () {
    if (flushing_.test_and_set ()) return;
    for (;;) {
        Outgoing item;
        IPtr<Vst::IConnectionPoint> peer;
        PresetMessageFactory factory;
        {
            std::lock_guard<std::mutex> lock (mutex_);
            if (!peer_ || outgoing_.empty () || !factory_) break;
            item = outgoing_.front ();
            peer = peer_;
            factory = factory_;
        }
        IPtr<Vst::IMessage> message = factory ();
        if (!message || writeEncodedPresetMessage (*message, item.kind, item.bytes) != BROM_PRESET_OK)
            break;
        if (peer->notify (message) != kResultOk) break;
        std::lock_guard<std::mutex> lock (mutex_);
        outgoing_.erase (std::remove_if (outgoing_.begin (), outgoing_.end (),
                                         [&item] (const Outgoing& queued) {
                                             return queued.id == item.id;
                                         }),
                         outgoing_.end ());
    }
    flushing_.clear ();
}

// ---- controller --------------------------------------------------------------

PresetControllerBridge::PresetControllerBridge (const brom_preset_schema& schema,
                                                const char* productKey,
                                                const brom_preset_view& initialView)
: PresetEndpoint (schema, productKey), view_ (initialView) {
    viewSequence_ = initialView.publication_sequence;
    knownGeneration_ = initialView.generation;
}

PresetControllerBridge::Record* PresetControllerBridge::findLocked (RequestId request) {
    for (auto& record : records_)
        if (record.used && record.receipt.request == request) return &record;
    return nullptr;
}

const PresetControllerBridge::Record* PresetControllerBridge::findLocked (RequestId request) const {
    for (const auto& record : records_)
        if (record.used && record.receipt.request == request) return &record;
    return nullptr;
}

PresetControllerBridge::~PresetControllerBridge () { shutdown (); }

brom_preset_result PresetControllerBridge::connect (Vst::IConnectionPoint& peer,
                                                    ConnectionEpoch epoch) {
    std::lock_guard<std::mutex> lock (mutex_);
    peer_ = &peer;
    epoch_ = epoch;
    welcomed_ = false;
    haveFreshView_ = false;
    pendingGenerationRequest_ = 0;
    knownGeneration_ = 0; // generations are monotonic per connection, not across them
    outgoing_.clear ();
    haveProgramTable_ = false;
    programTable_ = PresetProgramTable {};
    for (auto& record : records_) record = Record {};
    PresetWireMessage hello;
    hello.kind = PresetWireKind::Handshake;
    hello.handshake.role = static_cast<uint8_t> (PresetHandshakeRole::ControllerHello);
    // Commands from before this connection carry a sequence at or below this.
    hello.handshake.sequenceFloor = sequence_;
    return enqueueLocked (hello);
}

void PresetControllerBridge::disconnect () {
    std::lock_guard<std::mutex> lock (mutex_);
    if (pinService_) {
        for (auto& token : pins_)
            if (token) {
                brom_preset_catalog_service_browser_close (pinService_, token);
                token = 0;
            }
        pinService_ = nullptr;
    }
    peer_ = nullptr;
    outgoing_.clear ();
    welcomed_ = false;
    haveFreshView_ = false;
    pendingGenerationRequest_ = 0;
    haveProgramTable_ = false;
    programTable_ = PresetProgramTable {};
    for (auto& record : records_) {
        if (!record.used || record.receipt.complete) continue;
        record.receipt.complete = true;
        record.receipt.result = BROM_PRESET_STALE;
    }
}

brom_preset_result PresetControllerBridge::submitCommand (PresetCommand& command,
                                                          bool establishesGeneration,
                                                          RequestId& request) {
    std::lock_guard<std::mutex> lock (mutex_);
    if (!peer_ || !welcomed_ || !haveFreshView_) return BROM_PRESET_BUSY;
    Record* free = nullptr;
    for (auto& record : records_)
        if (!record.used) {
            free = &record;
            break;
        }
    if (!free) return BROM_PRESET_CAPACITY;
    command.request = nextRequest_ + 1;
    if (pendingGenerationRequest_ != 0 && command.kind != PresetCommandKind::Load) {
        command.dependsOn = pendingGenerationRequest_;
        command.generation = 0;
    } else {
        command.dependsOn = 0;
        command.generation = knownGeneration_;
    }
    PresetWireMessage message;
    message.kind = PresetWireKind::Command;
    message.command = command;
    const auto queued = enqueueLocked (message);
    if (queued != BROM_PRESET_OK) return queued;
    ++nextRequest_;
    free->used = true;
    free->accepted = false;
    free->receipt = PresetReceipt {command.request, BROM_PRESET_OK, 0, false};
    if (establishesGeneration) pendingGenerationRequest_ = command.request;
    request = command.request;
    return BROM_PRESET_OK;
}

brom_preset_result PresetControllerBridge::submitLoad (const brom_preset_snapshot& snapshot,
                                                       const brom_preset_identity& identity,
                                                       RequestId& request) {
    PresetCommand command;
    command.kind = PresetCommandKind::Load;
    command.snapshot = snapshot;
    command.identity = identity;
    return submitCommand (command, true, request);
}

brom_preset_result PresetControllerBridge::submitCompare (RequestId& request) {
    PresetCommand command;
    command.kind = PresetCommandKind::Compare;
    return submitCommand (command, false, request);
}

brom_preset_result PresetControllerBridge::submitEdit (uint32_t id, double plainValue,
                                                       RequestId& request) {
    if (!std::isfinite (plainValue)) return BROM_PRESET_INVALID;
    PresetCommand command;
    command.kind = PresetCommandKind::Edit;
    command.parameterId = id;
    command.plainValue = plainValue;
    return submitCommand (command, false, request);
}

brom_preset_result PresetControllerBridge::submitSaveAck (
    const brom_preset_save_completion& completion, RequestId& request) {
    PresetCommand command;
    command.kind = PresetCommandKind::SaveAck;
    command.completion = completion;
    return submitCommand (command, false, request);
}

brom_preset_result PresetControllerBridge::acknowledge (RequestId request) {
    std::lock_guard<std::mutex> lock (mutex_);
    Record* record = findLocked (request);
    if (!record) return BROM_PRESET_NOT_FOUND;
    if (!record->receipt.complete) return BROM_PRESET_BUSY;
    if (!peer_) {
        *record = Record {};
        return BROM_PRESET_OK;
    }
    PresetWireMessage message;
    message.kind = PresetWireKind::Command;
    message.command.kind = PresetCommandKind::Acknowledge;
    message.command.request = nextRequest_ + 1;
    message.command.dependsOn = request;
    const auto queued = enqueueLocked (message);
    if (queued != BROM_PRESET_OK) return queued;
    ++nextRequest_;
    *record = Record {};
    return BROM_PRESET_OK;
}

brom_preset_result PresetControllerBridge::setComponentView (const brom_preset_view& view) {
    uint8_t scratch[BROM_PRESET_SESSION_MAX_BYTES];
    std::size_t written = 0;
    if (brom_preset_session_encode (&schema_, productKey_.c_str (), &view, scratch,
                                    sizeof (scratch), &written) != BROM_PRESET_OK)
        return BROM_PRESET_INVALID;
    std::lock_guard<std::mutex> lock (mutex_);
    if (haveFreshView_) return BROM_PRESET_BUSY;
    view_ = view; // the display only: viewSequence_ stays, so the live view still wins
    return BROM_PRESET_OK;
}

brom_preset_result PresetControllerBridge::submitBrowserLoad (
    brom_preset_catalog_service& service, uint64_t token, uint32_t index,
    brom_preset_prepare_identity_fn prepare, void* user, RequestId& request) {
    brom_preset_snapshot snapshot {};
    brom_preset_identity identity {};
    const auto prepared = brom_preset_catalog_service_browser_prepare (
        &service, token, index, prepare, user, &snapshot, &identity);
    if (prepared != BROM_PRESET_OK) return prepared;
    return submitLoad (snapshot, identity, request);
}

brom_preset_result PresetControllerBridge::openBrowser (brom_preset_catalog_service& service,
                                                        uint64_t& token, uint32_t& count) {
    std::lock_guard<std::mutex> lock (mutex_);
    if (pinService_ && pinService_ != &service) return BROM_PRESET_BUSY;
    uint64_t* slot = nullptr;
    for (auto& pin : pins_)
        if (!pin) {
            slot = &pin;
            break;
        }
    if (!slot) return BROM_PRESET_CAPACITY;
    uint64_t opened = 0;
    uint32_t rows = 0;
    const auto result = brom_preset_catalog_service_browser_open (&service, &opened, &rows);
    if (result != BROM_PRESET_OK) return result;
    *slot = opened;
    pinService_ = &service;
    token = opened;
    count = rows;
    return BROM_PRESET_OK;
}

brom_preset_result PresetControllerBridge::closeBrowser (uint64_t token) {
    std::lock_guard<std::mutex> lock (mutex_);
    if (!pinService_ || token == 0) return BROM_PRESET_NOT_FOUND;
    for (auto& pin : pins_)
        if (pin == token) {
            pin = 0;
            return brom_preset_catalog_service_browser_close (pinService_, token);
        }
    return BROM_PRESET_NOT_FOUND;
}

brom_preset_result PresetControllerBridge::publishProgramTable (const PresetProgramTable& table) {
    std::lock_guard<std::mutex> lock (mutex_);
    PresetProgramTable stamped = table;
    stamped.mappingGeneration = mappingGeneration_ + 1;
    std::vector<PresetCatalogPage> pages;
    const auto paginated = paginateProgramTable (stamped, pages);
    if (paginated != BROM_PRESET_OK) return paginated;
    if (!peer_ || !welcomed_) return BROM_PRESET_BUSY;
    ++mappingGeneration_;
    for (const auto& page : pages) {
        PresetWireMessage message;
        message.kind = PresetWireKind::CatalogPage;
        message.catalog = page;
        const auto queued = enqueueLocked (message);
        if (queued != BROM_PRESET_OK) return queued; // the processor discards a partial set
    }
    programTable_ = std::move (stamped);
    haveProgramTable_ = true;
    return BROM_PRESET_OK;
}

bool PresetControllerBridge::readProgramTable (PresetProgramTable& table) const {
    std::lock_guard<std::mutex> lock (mutex_);
    if (!haveProgramTable_) return false;
    table = programTable_;
    return true;
}

tresult PresetControllerBridge::receive (Vst::IMessage& host) {
    CallGuard guard (*this);
    if (!guard.ok ()) return kResultFalse;
    PresetWireContext context;
    {
        std::lock_guard<std::mutex> lock (mutex_);
        context = contextLocked ();
    }
    PresetWireMessage message;
    // The sender retries a message until the receiver accepts it, so refusing one
    // that can never be accepted would hold up everything queued behind it. Anything
    // undecodable, stale or from an older connection is therefore dropped as handled.
    if (readPresetMessage (host, context, message) != BROM_PRESET_OK) return kResultOk;

    std::lock_guard<std::mutex> lock (mutex_);
    switch (message.kind) {
    case PresetWireKind::Handshake:
        if (message.handshake.role != static_cast<uint8_t> (PresetHandshakeRole::ProcessorWelcome))
            return kResultOk;
        if (epoch_ != 0 && message.epoch != epoch_) return kResultOk;
        // A welcome from an older connection, or one already adopted, is finished.
        if (message.epoch < lastWelcomeEpoch_ ||
            (message.epoch == lastWelcomeEpoch_ && message.sequence <= lastWelcomeSequence_))
            return kResultOk;
        lastWelcomeEpoch_ = message.epoch;
        lastWelcomeSequence_ = message.sequence;
        welcomeFloor_ = message.handshake.sequenceFloor;
        epoch_ = message.epoch;
        welcomed_ = true;
        return kResultOk;
    case PresetWireKind::Reply: {
        if (!welcomed_ || message.epoch != epoch_ || message.sequence <= welcomeFloor_)
            return kResultOk;
        Record* record = findLocked (message.reply.request);
        if (!record || record->receipt.complete) return kResultOk; // duplicate or reclaimed
        record->accepted = true;
        record->receipt.generation = message.reply.generation;
        if (message.reply.complete) {
            record->receipt.complete = true;
            record->receipt.result = message.reply.result;
        }
        if (pendingGenerationRequest_ == message.reply.request) pendingGenerationRequest_ = 0;
        if (message.reply.result == BROM_PRESET_OK && message.reply.generation > knownGeneration_)
            knownGeneration_ = message.reply.generation;
        return kResultOk;
    }
    case PresetWireKind::View:
        if (!welcomed_ || message.epoch != epoch_ || message.sequence <= welcomeFloor_)
            return kResultOk;
        if (haveFreshView_ &&
            (message.view.publication_sequence < viewSequence_ ||
             (message.view.publication_sequence == viewSequence_ &&
              message.viewRevision <= viewRevision_)))
            return kResultOk; // duplicate or stale publication
        view_ = message.view;
        viewSequence_ = message.view.publication_sequence;
        viewRevision_ = message.viewRevision;
        haveFreshView_ = true;
        // A view can predate a load the controller already knows was accepted.
        if (view_.generation > knownGeneration_) knownGeneration_ = view_.generation;
        return kResultOk;
    case PresetWireKind::Command:
    case PresetWireKind::CatalogPage: break;
    }
    return kResultOk;
}

bool PresetControllerBridge::readView (brom_preset_view& view) const {
    std::lock_guard<std::mutex> lock (mutex_);
    view = view_;
    return true;
}

brom_preset_result PresetControllerBridge::readReceipt (RequestId request,
                                                        PresetReceipt& receipt) const {
    std::lock_guard<std::mutex> lock (mutex_);
    const Record* record = findLocked (request);
    if (!record) return BROM_PRESET_NOT_FOUND;
    receipt = record->receipt;
    return BROM_PRESET_OK;
}

bool PresetControllerBridge::ready () const {
    std::lock_guard<std::mutex> lock (mutex_);
    return peer_ && welcomed_ && haveFreshView_;
}

ConnectionEpoch PresetControllerBridge::connectionEpoch () const {
    std::lock_guard<std::mutex> lock (mutex_);
    return welcomed_ ? epoch_ : 0;
}

void PresetControllerBridge::pump () {
    CallGuard guard (*this);
    if (guard.ok ()) flushOutgoing ();
}

// ---- processor ---------------------------------------------------------------

PresetProcessorBridge::PresetProcessorBridge (const brom_preset_schema& schema,
                                              const char* productKey,
                                              const brom_preset_view& initialView,
                                              const PresetProcessorHooks& hooks,
                                              uint32_t queueCapacity)
: PresetEndpoint (schema, productKey), hooks_ (hooks) {
    brom_preset_schema schemaCopy = schema;
    brom_preset_view initial = initialView;
    tx_ = brom_preset_tx_create (&schemaCopy, &initial, queueCapacity);
    if (tx_) outcomes_.resize (brom_preset_boundary_outcome_capacity (tx_));
    publicationSequence_ = initialView.publication_sequence;
}

PresetProcessorBridge::~PresetProcessorBridge () {
    shutdown ();
    delete table_.exchange (nullptr);
    brom_preset_tx_destroy (tx_);
}

void PresetProcessorBridge::clearTableLocked () {
    assembler_.reset ();
    if (const PresetProgramTable* old = table_.exchange (nullptr, std::memory_order_acq_rel))
        retiredTables_.emplace_back (old);
}

// The list is pinned for as long as this connection lives: a refresh may change
// what a slot contains, never how many slots there are or which preset each is.
void PresetProcessorBridge::installTableLocked (PresetProgramTable&& completed) {
    const PresetProgramTable* current = table_.load (std::memory_order_acquire);
    if (current) {
        if (current->slots.size () != completed.slots.size ()) return;
        for (size_t i = 0; i < completed.slots.size (); ++i)
            if (std::strcmp (current->slots[i].identity.key, completed.slots[i].identity.key) != 0)
                return;
    }
    const PresetProgramTable* fresh = new PresetProgramTable (std::move (completed));
    if (const PresetProgramTable* old = table_.exchange (fresh, std::memory_order_acq_rel))
        retiredTables_.emplace_back (old);
}

uint32_t PresetProcessorBridge::programCount () const {
    tableReaders_.fetch_add (1, std::memory_order_acq_rel);
    const PresetProgramTable* table = table_.load (std::memory_order_acquire);
    const uint32_t count = table ? static_cast<uint32_t> (table->slots.size ()) : 0u;
    tableReaders_.fetch_sub (1, std::memory_order_release);
    return count;
}

brom_preset_result PresetProcessorBridge::readProgramSlot (uint32_t slot,
                                                           PresetProgramSlot& out) const {
    tableReaders_.fetch_add (1, std::memory_order_acq_rel);
    const PresetProgramTable* table = table_.load (std::memory_order_acquire);
    brom_preset_result result = BROM_PRESET_NOT_FOUND;
    if (table && slot < table->slots.size ()) {
        out = table->slots[slot];
        result = BROM_PRESET_OK;
    }
    tableReaders_.fetch_sub (1, std::memory_order_release);
    return result;
}

std::size_t PresetProcessorBridge::pendingTableReclamations () const {
    std::lock_guard<std::mutex> lock (mutex_);
    return retiredTables_.size ();
}

brom_preset_result PresetProcessorBridge::audioSelectProgram (double normalized) {
    if (!tx_) return BROM_PRESET_INVALID;
    if (!std::isfinite (normalized) || normalized < 0.0 || normalized > 1.0)
        return BROM_PRESET_INVALID;
    tableReaders_.fetch_add (1, std::memory_order_acq_rel);
    const PresetProgramTable* table = table_.load (std::memory_order_acquire);
    brom_preset_result result = BROM_PRESET_BUSY;
    if (table) {
        uint32_t index = 0;
        result = brom_preset_catalog_service_program_slot_for_count (
            static_cast<uint32_t> (table->slots.size ()), normalized, &index);
        if (result == BROM_PRESET_OK) {
            const PresetProgramSlot& slot = table->slots[index];
            if (slot.availability != BROM_PRESET_OK) {
                result = BROM_PRESET_NOT_FOUND;
            } else {
                result = brom_preset_audio_apply_prepared (tx_, &slot.snapshot, &slot.identity);
                if (result == BROM_PRESET_OK) {
                    if (hooks_.apply) hooks_.apply (hooks_.user, slot.snapshot);
                    if (hooks_.hardReset) hooks_.hardReset (hooks_.user);
                    if (hooks_.clearNotes) hooks_.clearNotes (hooks_.user);
                }
            }
        }
    }
    tableReaders_.fetch_sub (1, std::memory_order_release);
    return result;
}

// A request ID names a slot only within the connection that accepted it; a slot of
// a finished connection is an orphan that nothing can address any more.
PresetProcessorBridge::Slot* PresetProcessorBridge::findSlot (RequestId request) {
    for (auto& slot : slots_)
        if (slot.state.load (std::memory_order_acquire) != Free && !slot.orphan &&
            slot.request.load (std::memory_order_relaxed) == request &&
            (isLocalRequest (request) || slot.epoch == epoch_))
            return &slot;
    return nullptr;
}

const PresetProcessorBridge::Slot* PresetProcessorBridge::findSlot (RequestId request) const {
    for (const auto& slot : slots_)
        if (slot.state.load (std::memory_order_acquire) != Free && !slot.orphan &&
            slot.request.load (std::memory_order_relaxed) == request &&
            (isLocalRequest (request) || slot.epoch == epoch_))
            return &slot;
    return nullptr;
}

// The audio side may still complete an orphan's request, so it is only reclaimed
// by pump() once it is terminal.
void PresetProcessorBridge::orphanSlotsLocked () {
    for (auto& slot : slots_)
        if (slot.state.load (std::memory_order_acquire) != Free &&
            !isLocalRequest (slot.request.load (std::memory_order_relaxed)))
            slot.orphan = true;
}

void PresetProcessorBridge::releaseSlot (Slot& slot) {
    slot.request.store (0, std::memory_order_relaxed);
    slot.state.store (Free, std::memory_order_release);
}

PresetProcessorBridge::Slot* PresetProcessorBridge::reserveSlotLocked (RequestId request,
                                                                       uint64_t commandSequence) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        for (auto& slot : slots_) {
            uint32_t expected = Free;
            if (!slot.state.compare_exchange_strong (expected, Reserved,
                                                     std::memory_order_acq_rel))
                continue;
            slot.request.store (request, std::memory_order_relaxed);
            slot.tag.store (++nextTag_, std::memory_order_relaxed);
            slot.generation.store (0, std::memory_order_relaxed);
            slot.result.store (BROM_PRESET_OK, std::memory_order_relaxed);
            slot.epoch = epoch_;
            slot.commandSequence = commandSequence;
            slot.orphan = false;
            slot.acceptSent = false;
            slot.completeSent = false;
            return &slot;
        }
        // A finished connection's terminal slots, then processor-originated requests
        // (which have no controller to acknowledge them), yield their slots; nothing
        // a live controller owns does.
        bool reclaimed = false;
        for (auto& slot : slots_)
            if (slot.state.load (std::memory_order_acquire) == Terminal && slot.orphan) {
                releaseSlot (slot);
                reclaimed = true;
                break;
            }
        if (!reclaimed)
            for (auto& slot : slots_)
                if (slot.state.load (std::memory_order_acquire) == Terminal &&
                    isLocalRequest (slot.request.load (std::memory_order_relaxed))) {
                    releaseSlot (slot);
                    reclaimed = true;
                    break;
                }
        if (!reclaimed) break;
    }
    return nullptr;
}

void PresetProcessorBridge::completeSlot (Slot& slot, brom_preset_result result,
                                          uint64_t generation) {
    slot.generation.store (generation, std::memory_order_relaxed);
    slot.result.store (result, std::memory_order_relaxed);
    slot.state.store (Terminal, std::memory_order_release);
}

brom_preset_result PresetProcessorBridge::connect (Vst::IConnectionPoint& peer,
                                                   ConnectionEpoch epoch) {
    if (epoch == 0) return BROM_PRESET_INVALID;
    std::lock_guard<std::mutex> lock (mutex_);
    peer_ = &peer;
    epoch_ = epoch;
    helloSeen_ = false;
    viewDirty_ = true;
    outgoing_.clear ();
    clearTableLocked ();
    orphanSlotsLocked ();
    helloFloor_ = ackFloor_ = 0;
    return BROM_PRESET_OK;
}

void PresetProcessorBridge::disconnect () {
    std::lock_guard<std::mutex> lock (mutex_);
    peer_ = nullptr;
    outgoing_.clear ();
    helloSeen_ = false;
    clearTableLocked ();
    orphanSlotsLocked (); // committed state is untouched; only the receipts end
}

bool PresetProcessorBridge::handshakeReady () const {
    std::lock_guard<std::mutex> lock (mutex_);
    return peer_ && helloSeen_;
}

void PresetProcessorBridge::replyLocked (RequestId request, brom_preset_result result,
                                         uint64_t generation, bool complete) {
    PresetWireMessage reply;
    reply.kind = PresetWireKind::Reply;
    reply.reply.request = request;
    reply.reply.result = result;
    reply.reply.generation = generation;
    reply.reply.complete = complete;
    enqueueLocked (reply);
}

void PresetProcessorBridge::handleCommandLocked (const PresetCommand& command,
                                                 uint64_t sequence) {
    if (command.kind == PresetCommandKind::Acknowledge) {
        Slot* acknowledged = findSlot (command.dependsOn);
        if (acknowledged && acknowledged->state.load (std::memory_order_acquire) == Terminal) {
            // Whatever created it can no longer be run again, even if it arrives late.
            ackFloor_ = std::max (ackFloor_, acknowledged->commandSequence);
            releaseSlot (*acknowledged);
        }
        return;
    }

    // A retried request replays its replies and never enqueues a second command.
    if (Slot* existing = findSlot (command.request)) {
        replyLocked (command.request, BROM_PRESET_OK,
                     existing->generation.load (std::memory_order_relaxed), false);
        existing->acceptSent = true;
        existing->completeSent = false;
        return;
    }

    // A duplicate of a command whose slot was acknowledged and reclaimed, or one from
    // before this connection, must not run again.
    if (sequence <= std::max (helloFloor_, ackFloor_)) {
        replyLocked (command.request, BROM_PRESET_STALE, 0, true);
        return;
    }

    uint64_t generation = command.generation;
    if (command.dependsOn != 0 && command.kind != PresetCommandKind::Load) {
        const Slot* dependency = findSlot (command.dependsOn);
        if (!dependency) {
            replyLocked (command.request, BROM_PRESET_STALE, 0, true);
            return;
        }
        if (dependency->state.load (std::memory_order_acquire) == Terminal &&
            dependency->result.load (std::memory_order_relaxed) != BROM_PRESET_OK) {
            replyLocked (command.request,
                         static_cast<brom_preset_result> (
                             dependency->result.load (std::memory_order_relaxed)),
                         0, true);
            return;
        }
        generation = dependency->generation.load (std::memory_order_relaxed);
    }

    Slot* slot = reserveSlotLocked (command.request, sequence);
    if (!slot) {
        replyLocked (command.request, BROM_PRESET_CAPACITY, generation, true);
        return;
    }
    const uint64_t tag = slot->tag.load (std::memory_order_relaxed);

    uint64_t accepted = generation;
    brom_preset_result result = BROM_PRESET_INVALID;
    bool completedAtSubmit = false;
    brom_preset_outcome_record immediate {};
    switch (command.kind) {
    case PresetCommandKind::Load: {
        brom_preset_snapshot snapshot = command.snapshot;
        brom_preset_identity identity = command.identity;
        result = brom_preset_submit_load_tagged (tx_, &snapshot, &identity, tag, &accepted);
        break;
    }
    case PresetCommandKind::Compare:
        result = brom_preset_submit_compare_tagged (tx_, tag, generation);
        break;
    case PresetCommandKind::Edit:
        result = brom_preset_submit_edit_tagged (tx_, tag, generation, command.parameterId,
                                                 command.plainValue);
        break;
    case PresetCommandKind::SaveAck:
        accepted = command.completion.generation;
        result = brom_preset_save_ack_submit_tagged (tx_, &command.completion, tag,
                                                     &completedAtSubmit, &immediate);
        break;
    case PresetCommandKind::Acknowledge: break;
    }

    if (result != BROM_PRESET_OK) {
        completeSlot (*slot, result, accepted);
        replyLocked (command.request, result, accepted, true);
        slot->acceptSent = slot->completeSent = true;
        return;
    }
    slot->generation.store (accepted, std::memory_order_relaxed);
    uint32_t expected = Reserved;
    slot->state.compare_exchange_strong (expected, Accepted, std::memory_order_acq_rel);
    replyLocked (command.request, BROM_PRESET_OK, accepted, false);
    slot->acceptSent = true;
    if (completedAtSubmit) {
        completeSlot (*slot, resultForOutcome (immediate), accepted);
        // The primitive applied it to the pending view and copied that view's old
        // publication sequence, so the controller needs a freshly stamped copy.
        if (immediate.outcome == BROM_PRESET_OUTCOME_APPLIED) viewDirty_ = true;
    }
}

tresult PresetProcessorBridge::receive (Vst::IMessage& host) {
    if (!tx_) return kResultFalse;
    CallGuard guard (*this);
    if (!guard.ok ()) return kResultFalse;
    PresetWireContext context;
    {
        std::lock_guard<std::mutex> lock (mutex_);
        context = contextLocked ();
    }
    PresetWireMessage message;
    // See the controller's receive: only "not ready yet" is worth a retry.
    if (readPresetMessage (host, context, message) != BROM_PRESET_OK) return kResultOk;

    std::lock_guard<std::mutex> lock (mutex_);
    switch (message.kind) {
    case PresetWireKind::Handshake: {
        if (message.handshake.role != static_cast<uint8_t> (PresetHandshakeRole::ControllerHello))
            return kResultOk;
        // Between disconnect and connect a welcome would have no peer and connect would
        // discard it, so the controller must keep its hello and retry.
        if (epoch_ == 0 || !peer_) return kResultFalse;
        helloSeen_ = true;
        viewDirty_ = true;
        helloFloor_ = message.handshake.sequenceFloor;
        ackFloor_ = 0;
        PresetWireMessage welcome;
        welcome.kind = PresetWireKind::Handshake;
        welcome.handshake.role = static_cast<uint8_t> (PresetHandshakeRole::ProcessorWelcome);
        welcome.handshake.sequenceFloor = sequence_;
        enqueueLocked (welcome);
        return kResultOk;
    }
    case PresetWireKind::Command:
        if (!helloSeen_) return kResultOk; // from before this connection's hello
        handleCommandLocked (message.command, message.sequence);
        if (!active_.load ()) drainInactiveLocked ();
        return kResultOk;
    case PresetWireKind::CatalogPage: {
        if (!helloSeen_) return kResultOk;
        PresetProgramTable completed;
        switch (assembler_.add (message.catalog, completed)) {
        case PresetProgramAssembler::Status::Pending: return kResultOk;
        case PresetProgramAssembler::Status::Complete:
            installTableLocked (std::move (completed));
            return kResultOk;
        case PresetProgramAssembler::Status::Rejected: break;
        }
        return kResultOk;
    }
    case PresetWireKind::Reply:
    case PresetWireKind::View: break;
    }
    return kResultOk;
}

void PresetProcessorBridge::drainInactiveLocked () {
    if (!tx_) return;
    const uint64_t cutoff = brom_preset_audio_cutoff (tx_);
    brom_preset_commit commit {};
    uint32_t count = 0;
    const bool changed = brom_preset_audio_boundary_ex (
        tx_, cutoff, &commit, outcomes_.data (), static_cast<uint32_t> (outcomes_.size ()), &count);
    if (changed) {
        // No hook runs here: the DSP is not running, and resume applies what is pending.
        commit.view.publication_sequence = ++publicationSequence_;
        brom_preset_audio_publish_inactive (tx_, &commit);
        viewDirty_ = true;
    }
    completeFromOutcomes (count);
}

void PresetProcessorBridge::applyCommit (brom_preset_commit& commit) {
    const brom_preset_snapshot& chosen =
        commit.view.audible_side == 0 ? commit.view.working : commit.view.reference;
    if ((commit.apply_parameters || commit.reset_voices) && hooks_.apply)
        hooks_.apply (hooks_.user, chosen);
    if (commit.reset_voices) {
        if (hooks_.hardReset) hooks_.hardReset (hooks_.user);
        if (hooks_.clearNotes) hooks_.clearNotes (hooks_.user);
    }
    commit.view.publication_sequence = ++publicationSequence_;
    brom_preset_audio_publish (tx_, &commit);
}

brom_preset_result PresetProcessorBridge::setActive (bool active) {
    if (!tx_) return BROM_PRESET_INVALID;
    std::lock_guard<std::mutex> lock (mutex_);
    if (active) {
        if (!active_.load ()) drainInactiveLocked (); // anything still queued from before
        brom_preset_commit commit {};
        if (brom_preset_audio_resume (tx_, &commit)) applyCommit (commit);
        brom_preset_set_active (tx_, true);
        active_.store (true);
    } else {
        brom_preset_set_active (tx_, false);
        active_.store (false);
        drainInactiveLocked (); // the active consumer's leftovers go to the inactive one
    }
    return BROM_PRESET_OK;
}

brom_preset_result PresetProcessorBridge::audioEdit (uint32_t id, double plainValue,
                                                     bool identifiedEcho, bool& applyToDsp) {
    applyToDsp = false;
    if (!tx_) return BROM_PRESET_INVALID;
    const bool echo = identifiedEcho || consumeEcho (id, plainValue);
    return brom_preset_audio_edit (tx_, id, plainValue, echo, &applyToDsp);
}

brom_preset_result PresetProcessorBridge::armEcho (uint32_t id, double plainValue) {
    if (!std::isfinite (plainValue)) return BROM_PRESET_INVALID;
    bool known = false;
    for (uint32_t i = 0; i < schema_.count; ++i) known = known || schema_.parameters[i].id == id;
    if (!known) return BROM_PRESET_INVALID;
    for (auto& echo : echoes_) {
        uint32_t expected = 0;
        if (!echo.state.compare_exchange_strong (expected, 2u, std::memory_order_acq_rel)) continue;
        echo.id.store (id, std::memory_order_relaxed);
        echo.value.store (plainValue, std::memory_order_relaxed);
        echo.state.store (1u, std::memory_order_release); // visible to the audio side
        return BROM_PRESET_OK;
    }
    return BROM_PRESET_CAPACITY;
}

std::size_t PresetProcessorBridge::pendingEchoes () const {
    std::size_t armed = 0;
    for (const auto& echo : echoes_)
        if (echo.state.load (std::memory_order_acquire) == 1u) ++armed;
    return armed;
}

// The first armed expectation with exactly this id and value is spent; nothing else
// is ever treated as an echo.
bool PresetProcessorBridge::consumeEcho (uint32_t id, double value) {
    for (auto& echo : echoes_) {
        if (echo.state.load (std::memory_order_acquire) != 1u ||
            echo.id.load (std::memory_order_relaxed) != id ||
            echo.value.load (std::memory_order_relaxed) != value)
            continue;
        uint32_t expected = 1u;
        if (echo.state.compare_exchange_strong (expected, 0u, std::memory_order_acq_rel))
            return true;
    }
    return false;
}

void PresetProcessorBridge::expireEchoes () {
    for (auto& echo : echoes_) {
        uint32_t expected = 1u;
        echo.state.compare_exchange_strong (expected, 0u, std::memory_order_acq_rel);
    }
}

void PresetProcessorBridge::completeFromOutcomes (uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const brom_preset_outcome_record& record = outcomes_[i];
        if (record.outcome == BROM_PRESET_OUTCOME_DEFERRED) continue;
        for (auto& slot : slots_) {
            const uint32_t state = slot.state.load (std::memory_order_acquire);
            if ((state != Reserved && state != Accepted) ||
                slot.tag.load (std::memory_order_relaxed) != record.tag)
                continue;
            completeSlot (slot, resultForOutcome (record), record.generation);
            break;
        }
    }
}

void PresetProcessorBridge::finishTimeline () {
    if (!tx_) return;
    const uint64_t cutoff = brom_preset_audio_cutoff (tx_);
    brom_preset_commit commit {};
    uint32_t count = 0;
    const bool changed = brom_preset_audio_boundary_ex (
        tx_, cutoff, &commit, outcomes_.data (), static_cast<uint32_t> (outcomes_.size ()), &count);
    if (changed) applyCommit (commit);
    completeFromOutcomes (count);
    expireEchoes (); // an expectation lives until the next boundary, no longer
}

void PresetProcessorBridge::pump () {
    CallGuard guard (*this);
    if (!guard.ok ()) return;
    if (tx_) {
        std::lock_guard<std::mutex> lock (mutex_);
        for (auto& slot : slots_)
            if (slot.orphan && slot.state.load (std::memory_order_acquire) == Terminal)
                releaseSlot (slot);
        // A replaced list can be freed once no audio-side reader is inside it: any
        // reader that starts after the swap already sees the new one.
        if (tableReaders_.load (std::memory_order_acquire) == 0) retiredTables_.clear ();
        if (!active_.load ()) drainInactiveLocked ();
        if (peer_ && helloSeen_) {
            // Look at the slots before the view: a command is published before its slot
            // turns terminal, so a view read after the scan already holds every commit
            // whose completion is sent below.
            uint64_t terminal = 0;
            for (std::size_t i = 0; i < slots_.size (); ++i)
                if (slots_[i].state.load (std::memory_order_acquire) == Terminal)
                    terminal |= uint64_t {1} << i;
            brom_preset_view view {};
            if (brom_preset_read_view (tx_, &view)) {
                if (testAfterViewRead) testAfterViewRead ();
                const bool sourceChanged =
                    !viewEverSent_ || view.publication_sequence != lastSourceViewSequence_;
                if (viewDirty_ || sourceChanged) {
                    // The view changed under an unchanged sequence (a synchronous inactive
                    // save acknowledgment): a higher revision of the same publication. The
                    // audio side's own stamps are never involved, so a later real
                    // publication always orders after this copy.
                    nextViewRevision_ = sourceChanged ? 0 : nextViewRevision_ + 1;
                    PresetWireMessage message;
                    message.kind = PresetWireKind::View;
                    message.view = view;
                    message.viewRevision = nextViewRevision_;
                    if (enqueueLocked (message) == BROM_PRESET_OK) {
                        lastSourceViewSequence_ = view.publication_sequence;
                        viewEverSent_ = true;
                        viewDirty_ = false;
                    } else if (!sourceChanged) {
                        --nextViewRevision_;
                    }
                }
            }
            for (std::size_t i = 0; i < slots_.size (); ++i) {
                Slot& slot = slots_[i];
                if (!(terminal & (uint64_t {1} << i)) || slot.completeSent ||
                    isLocalRequest (slot.request.load (std::memory_order_relaxed)))
                    continue;
                replyLocked (slot.request.load (std::memory_order_relaxed),
                             static_cast<brom_preset_result> (
                                 slot.result.load (std::memory_order_relaxed)),
                             slot.generation.load (std::memory_order_relaxed), true);
                slot.completeSent = true;
            }
        }
    }
    flushOutgoing ();
}

bool PresetProcessorBridge::readView (brom_preset_view& view) const {
    return tx_ && brom_preset_read_view (tx_, &view);
}

brom_preset_result PresetProcessorBridge::restore (const brom_preset_view& view,
                                                   RequestId& request) {
    if (!tx_) return BROM_PRESET_INVALID;
    std::lock_guard<std::mutex> lock (mutex_);
    const RequestId id = (++nextLocalRequest_) | kLocalRequestBit;
    Slot* slot = reserveSlotLocked (id, 0);
    if (!slot) return BROM_PRESET_CAPACITY;
    brom_preset_view copy = view;
    copy.publication_sequence = ++publicationSequence_;
    uint64_t generation = 0;
    bool completed = false;
    const auto result = brom_preset_submit_restore_tagged (
        tx_, &copy, slot->tag.load (std::memory_order_relaxed), &generation, &completed);
    if (result != BROM_PRESET_OK) {
        completeSlot (*slot, result, 0);
        slot->completeSent = true;
        request = id;
        return result;
    }
    slot->generation.store (generation, std::memory_order_relaxed);
    if (completed) {
        completeSlot (*slot, BROM_PRESET_OK, generation);
    } else {
        uint32_t expected = Reserved;
        slot->state.compare_exchange_strong (expected, Accepted, std::memory_order_acq_rel);
    }
    viewDirty_ = true;
    request = id;
    if (!active_.load ()) drainInactiveLocked (); // consumes the restore's queued copy
    return BROM_PRESET_OK;
}

brom_preset_result PresetProcessorBridge::readReceipt (RequestId request,
                                                       PresetReceipt& receipt) const {
    std::lock_guard<std::mutex> lock (mutex_);
    const Slot* slot = findSlot (request);
    if (!slot) return BROM_PRESET_NOT_FOUND;
    const uint32_t state = slot->state.load (std::memory_order_acquire);
    receipt.request = request;
    receipt.generation = slot->generation.load (std::memory_order_relaxed);
    receipt.complete = state == Terminal;
    receipt.result = receipt.complete
        ? static_cast<brom_preset_result> (slot->result.load (std::memory_order_relaxed))
        : BROM_PRESET_OK;
    return BROM_PRESET_OK;
}

} // namespace Steinberg::PokustonVst3Support
