#pragma once

// Bounded, versioned preset messages exchanged between a VST3 processor and its
// controller. Payloads are explicit little-endian bytes copied into an
// IAttributeList; no native struct, pointer, padding or size_t crosses the wire.
// Everything here runs off the audio thread.

#include "pluginterfaces/vst/ivstmessage.h"

#include "../preset/api.h"
#include "../preset/catalog_service.h"
#include "../preset/save.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Steinberg::PokustonVst3Support {

inline constexpr uint16_t kPresetWireVersion = 1;
inline constexpr std::size_t kPresetWireMaxBytes = 65536;
inline constexpr const char* kPresetRequestMessageId = "brom.preset.request.v1";
inline constexpr const char* kPresetViewMessageId = "brom.preset.view.v1";
inline constexpr const char* kPresetCatalogMessageId = "brom.preset.catalog.v1";

enum class PresetWireKind : uint8_t { Handshake = 1, Command = 2, Reply = 3, View = 4, CatalogPage = 5 };
// Acknowledge tells the processor the controller consumed a terminal receipt, so
// its slot can be reclaimed.
enum class PresetCommandKind : uint8_t { Load = 1, Compare = 2, Edit = 3, SaveAck = 4, Acknowledge = 5 };
enum class PresetHandshakeRole : uint8_t { ControllerHello = 1, ProcessorWelcome = 2 };
// The last value of each enum: the codec accepts kinds 1..last. Update these when a kind
// is added; knownKind and knownCommandKind follow them.
inline constexpr PresetWireKind kLastPresetWireKind = PresetWireKind::CatalogPage;
inline constexpr PresetCommandKind kLastPresetCommandKind = PresetCommandKind::Acknowledge;

struct PresetHandshake {
    uint8_t role = 0;
    uint64_t sequenceFloor = 0;
};

struct PresetCommand {
    PresetCommandKind kind = PresetCommandKind::Compare;
    uint64_t request = 0;   // zero is invalid
    uint64_t dependsOn = 0; // earlier request this one waits on; zero for none
    uint64_t generation = 0; // generation the sender believes current, used when dependsOn is zero
    brom_preset_snapshot snapshot {};           // Load
    brom_preset_identity identity {};           // Load
    uint32_t parameterId = 0;                   // Edit
    double plainValue = 0.0;                    // Edit
    brom_preset_save_completion completion {};  // SaveAck
};

struct PresetReply {
    uint64_t request = 0;
    brom_preset_result result = BROM_PRESET_OK;
    uint64_t generation = 0;
    bool complete = false;
};

// One slot of a host program list, prepared off the audio thread: its stable
// identity, whether its source was readable, and (when it was) the validated
// snapshot. A slot that could not be read stays in place as BROM_PRESET_NOT_FOUND.
struct PresetProgramSlot {
    brom_preset_identity identity {};
    brom_preset_result availability = BROM_PRESET_NOT_FOUND; // OK or NOT_FOUND
    brom_preset_snapshot snapshot {};                        // meaningful only when OK
};

// An immutable program list: at most kPresetProgramMaxSlots slots, whose count and
// numbering are fixed for as long as an endpoint retains it.
struct PresetProgramTable {
    uint64_t mappingGeneration = 0; // nonzero
    std::vector<PresetProgramSlot> slots;
};

inline constexpr std::size_t kPresetCatalogPageEntries = 16;
inline constexpr std::size_t kPresetProgramMaxSlots = BROM_PRESET_PROGRAM_MAX_SLOTS;

// The largest encoded catalog page, field by field as encodePresetMessage writes it:
// the common header (version, kind, reserved, epoch, sequence, schema id, product key
// length and key), the page header (generation, index, total, entry count), and per
// entry a worst-case identity (kind, two length-prefixed strings), the availability
// byte and a full snapshot (count, index, one double per parameter).
inline constexpr std::size_t kPresetWireHeaderMaxBytes = 2 + 1 + 1 + 8 + 8 + 8 + 1 + (BROM_PRESET_KEY_BYTES - 1);
inline constexpr std::size_t kPresetIdentityMaxBytes =
    4 + (2 + BROM_PRESET_KEY_BYTES - 1) + (2 + BROM_PRESET_NAME_BYTES - 1);
inline constexpr std::size_t kPresetSnapshotMaxBytes = 4 + 4 + 8 * BROM_PRESET_MAX_PARAMS;
inline constexpr std::size_t kPresetCatalogPageMaxBytes =
    kPresetWireHeaderMaxBytes + (8 + 4 + 4 + 1) +
    kPresetCatalogPageEntries * (kPresetIdentityMaxBytes + 1 + kPresetSnapshotMaxBytes);
static_assert (kPresetCatalogPageMaxBytes <= kPresetWireMaxBytes,
               "a full catalog page must fit in one wire message");

// One bounded page of a table. An empty table is a single page with no entries.
struct PresetCatalogPage {
    uint64_t mappingGeneration = 0;
    uint32_t pageIndex = 0;
    uint32_t totalCount = 0;
    uint8_t entryCount = 0;
    PresetProgramSlot entries[kPresetCatalogPageEntries] {};
};

// A tagged union by `kind`; only the matching member is meaningful. View
// messages carry the complete validated session view, whose generation,
// working revision and publication sequence travel with it.
struct PresetWireMessage {
    PresetWireKind kind = PresetWireKind::Handshake;
    uint64_t epoch = 0;
    uint64_t sequence = 0;
    PresetHandshake handshake {};
    PresetCommand command {};
    PresetReply reply {};
    brom_preset_view view {};
    // Orders re-sends of one publication: a view's identity is (publication_sequence,
    // viewRevision), so a refreshed copy never needs a sequence the audio side may mint.
    uint32_t viewRevision = 0;
    PresetCatalogPage catalog {};
};

// What both endpoints agreed on at connect time. A message for another product
// or schema is INVALID, and a non-handshake message from another connection
// epoch is STALE. A handshake carries the epoch being established, so its epoch
// is not compared.
struct PresetWireContext {
    const brom_preset_schema* schema = nullptr;
    const char* productKey = nullptr;
    uint64_t epoch = 0;
};

uint64_t presetSchemaId (const brom_preset_schema& schema);

// Splits a table into pages of at most kPresetCatalogPageEntries.
brom_preset_result paginateProgramTable (const PresetProgramTable& table,
                                         std::vector<PresetCatalogPage>& pages);

// Reassembles pages in order. Anything duplicated, reordered, truncated or
// mismatching discards the assembly in progress; only a complete, consistent set
// ever produces a table.
class PresetProgramAssembler {
public:
    enum class Status { Pending, Complete, Rejected };
    Status add (const PresetCatalogPage& page, PresetProgramTable& completed);
    void reset ();

private:
    bool active_ = false;
    uint64_t generation_ = 0;
    uint32_t total_ = 0;
    uint32_t nextPage_ = 0;
    std::vector<PresetProgramSlot> slots_;
};
const char* presetMessageId (PresetWireKind kind);

// On failure the output is left exactly as it was.
brom_preset_result encodePresetMessage (const PresetWireContext& context,
                                        const PresetWireMessage& message,
                                        std::vector<uint8_t>& bytes);
brom_preset_result decodePresetMessage (const void* data, std::size_t size,
                                        const PresetWireContext& context,
                                        PresetWireMessage& message);
// Writes already-encoded bytes into a host message; used to resend a retained
// payload unchanged.
brom_preset_result writeEncodedPresetMessage (Vst::IMessage& host, PresetWireKind kind,
                                              const std::vector<uint8_t>& bytes);
brom_preset_result writePresetMessage (Vst::IMessage& host, const PresetWireContext& context,
                                       const PresetWireMessage& message);
brom_preset_result readPresetMessage (Vst::IMessage& host, const PresetWireContext& context,
                                      PresetWireMessage& message);

} // namespace Steinberg::PokustonVst3Support
