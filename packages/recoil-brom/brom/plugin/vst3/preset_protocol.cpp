#include "preset_protocol.hpp"

#include "../preset/session.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace Steinberg::PokustonVst3Support {

namespace {

constexpr const char* kPayloadAttribute = "payload";

// ---- little-endian primitives ------------------------------------------------

class Writer {
public:
    void u8 (uint8_t value) { bytes (&value, 1); }
    void u16 (uint16_t value) { integer (value, 2); }
    void u32 (uint32_t value) { integer (value, 4); }
    void u64 (uint64_t value) { integer (value, 8); }
    void f64 (double value) {
        uint64_t bits = 0;
        std::memcpy (&bits, &value, sizeof (bits));
        u64 (bits);
    }
    void bytes (const void* source, std::size_t count) {
        if (result != BROM_PRESET_OK) return;
        if (count > kPresetWireMaxBytes - out.size ()) {
            result = BROM_PRESET_CAPACITY;
            return;
        }
        const auto* first = static_cast<const uint8_t*> (source);
        out.insert (out.end (), first, first + count);
    }
    void fail (brom_preset_result code) {
        if (result == BROM_PRESET_OK) result = code;
    }

    std::vector<uint8_t> out;
    brom_preset_result result = BROM_PRESET_OK;

private:
    void integer (uint64_t value, unsigned width) {
        uint8_t buffer[8];
        for (unsigned i = 0; i < width; ++i) buffer[i] = static_cast<uint8_t> (value >> (8u * i));
        bytes (buffer, width);
    }
};

class Reader {
public:
    Reader (const uint8_t* data, std::size_t size) : data (data), size (size) {}
    uint8_t u8 () {
        if (position >= size) {
            failed = true;
            return 0;
        }
        return data[position++];
    }
    uint16_t u16 () { return static_cast<uint16_t> (integer (2)); }
    uint32_t u32 () { return static_cast<uint32_t> (integer (4)); }
    uint64_t u64 () { return integer (8); }
    double f64 () {
        const uint64_t bits = u64 ();
        double value = 0.0;
        std::memcpy (&value, &bits, sizeof (value));
        return value;
    }
    const uint8_t* take (std::size_t count) {
        if (failed || count > size - position) {
            failed = true;
            return nullptr;
        }
        const uint8_t* first = data + position;
        position += count;
        return first;
    }
    bool finished () const { return !failed && position == size; }

    bool failed = false;

private:
    uint64_t integer (unsigned width) {
        uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i) value |= static_cast<uint64_t> (u8 ()) << (8u * i);
        return value;
    }

    const uint8_t* data;
    std::size_t size;
    std::size_t position = 0;
};

// ---- validation mirrors the session codec so a command is rejected for the
// ---- same reasons a restored session would be ---------------------------------

bool schemaValid (const brom_preset_schema* schema) {
    return schema && schema->count > 0 && schema->count <= BROM_PRESET_MAX_PARAMS;
}

bool productKeyValid (const char* key) {
    if (!key || !*key || std::strlen (key) >= BROM_PRESET_KEY_BYTES) return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*> (key); *p; ++p)
        if (*p < 33u || *p > 126u) return false;
    return true;
}

bool valueValid (const brom_preset_parameter& parameter, double value) {
    if (!std::isfinite (value) || value < parameter.minimum || value > parameter.maximum) return false;
    if (parameter.steps > 1u) {
        if (parameter.minimum == parameter.maximum) return false;
        const double step = (parameter.maximum - parameter.minimum) / double (parameter.steps - 1u);
        const double position = (value - parameter.minimum) / step;
        if (!std::isfinite (position) || std::fabs (position - std::nearbyint (position)) > 1e-9)
            return false;
    }
    return true;
}

bool snapshotValid (const brom_preset_schema& schema, const brom_preset_snapshot& snapshot) {
    if (snapshot.count != static_cast<int32_t> (schema.count)) return false;
    for (uint32_t i = 0; i < schema.count; ++i)
        if (!valueValid (schema.parameters[i], snapshot.values[i])) return false;
    return true;
}

bool terminated (const char* text, std::size_t capacity) {
    return std::memchr (text, 0, capacity) != nullptr;
}

bool identityValid (const brom_preset_identity& identity) {
    return terminated (identity.key, sizeof (identity.key)) &&
           terminated (identity.display_name, sizeof (identity.display_name)) &&
           identity.key[0] && identity.display_name[0];
}

bool knownParameter (const brom_preset_schema& schema, uint32_t id) {
    for (uint32_t i = 0; i < schema.count; ++i)
        if (schema.parameters[i].id == id) return true;
    return false;
}

// ---- field codecs ------------------------------------------------------------

void putString (Writer& w, const char* text, std::size_t capacity) {
    if (!terminated (text, capacity)) return w.fail (BROM_PRESET_INVALID);
    const std::size_t length = std::strlen (text);
    w.u16 (static_cast<uint16_t> (length));
    w.bytes (text, length);
}

bool getString (Reader& r, char* out, std::size_t capacity) {
    const uint16_t length = r.u16 ();
    if (r.failed || length >= capacity) return false;
    const uint8_t* text = r.take (length);
    if (!text || std::memchr (text, 0, length)) return false;
    std::memcpy (out, text, length);
    out[length] = 0;
    return true;
}

void putSnapshot (Writer& w, const brom_preset_schema& schema,
                  const brom_preset_snapshot& snapshot) {
    if (!snapshotValid (schema, snapshot)) return w.fail (BROM_PRESET_INVALID);
    w.u32 (static_cast<uint32_t> (snapshot.count));
    w.u32 (static_cast<uint32_t> (snapshot.index));
    for (int32_t i = 0; i < snapshot.count; ++i) w.f64 (snapshot.values[i]);
}

bool getSnapshot (Reader& r, const brom_preset_schema& schema, brom_preset_snapshot& snapshot) {
    snapshot = brom_preset_snapshot {};
    snapshot.count = static_cast<int32_t> (r.u32 ());
    snapshot.index = static_cast<int32_t> (r.u32 ());
    if (r.failed || snapshot.count != static_cast<int32_t> (schema.count)) return false;
    for (int32_t i = 0; i < snapshot.count; ++i) snapshot.values[i] = r.f64 ();
    return !r.failed && snapshotValid (schema, snapshot);
}

void putIdentity (Writer& w, const brom_preset_identity& identity) {
    if (!identityValid (identity)) return w.fail (BROM_PRESET_INVALID);
    w.u32 (identity.kind);
    putString (w, identity.key, sizeof (identity.key));
    putString (w, identity.display_name, sizeof (identity.display_name));
}

bool getIdentity (Reader& r, brom_preset_identity& identity) {
    identity = brom_preset_identity {};
    identity.kind = r.u32 ();
    return getString (r, identity.key, sizeof (identity.key)) &&
           getString (r, identity.display_name, sizeof (identity.display_name)) &&
           identityValid (identity);
}

bool knownKind (uint8_t kind) { return kind >= 1 && kind <= static_cast<uint8_t> (kLastPresetWireKind); }

bool knownCommandKind (uint8_t kind) {
    return kind >= 1 && kind <= static_cast<uint8_t> (kLastPresetCommandKind);
}

// A page is internally consistent: every page but the last is full, and the last
// holds exactly what is left. An empty table is one empty page.
bool pageShapeValid (const PresetCatalogPage& page) {
    if (page.mappingGeneration == 0 || page.totalCount > kPresetProgramMaxSlots ||
        page.entryCount > kPresetCatalogPageEntries)
        return false;
    if (page.totalCount == 0) return page.pageIndex == 0 && page.entryCount == 0;
    const uint64_t first = static_cast<uint64_t> (page.pageIndex) * kPresetCatalogPageEntries;
    if (first >= page.totalCount) return false;
    const uint64_t remaining = page.totalCount - first;
    return page.entryCount == (remaining < kPresetCatalogPageEntries ? remaining
                                                                      : kPresetCatalogPageEntries);
}

bool availabilityValid (brom_preset_result result) {
    return result == BROM_PRESET_OK || result == BROM_PRESET_NOT_FOUND;
}

} // namespace

uint64_t presetSchemaId (const brom_preset_schema& schema) {
    uint64_t hash = 1469598103934665603ull; // FNV-1a, 64 bit
    auto mix = [&hash] (const void* data, std::size_t size) {
        const auto* bytes = static_cast<const uint8_t*> (data);
        for (std::size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    };
    auto mix32 = [&mix] (uint32_t value) {
        const uint8_t bytes[4] = {uint8_t (value), uint8_t (value >> 8), uint8_t (value >> 16),
                                  uint8_t (value >> 24)};
        mix (bytes, 4);
    };
    auto mixDouble = [&mix] (double value) {
        uint64_t bits = 0;
        std::memcpy (&bits, &value, sizeof (bits));
        uint8_t bytes[8];
        for (unsigned i = 0; i < 8; ++i) bytes[i] = uint8_t (bits >> (8u * i));
        mix (bytes, 8);
    };
    mix32 (schema.version);
    mix32 (schema.count);
    const uint32_t count = schema.count > BROM_PRESET_MAX_PARAMS ? BROM_PRESET_MAX_PARAMS : schema.count;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& p = schema.parameters[i];
        mix32 (p.id);
        mix32 (p.steps);
        mixDouble (p.minimum);
        mixDouble (p.maximum);
        mixDouble (p.default_value);
        const void* end = std::memchr (p.key, 0, sizeof (p.key));
        mix (p.key, end ? static_cast<std::size_t> (static_cast<const char*> (end) - p.key)
                        : sizeof (p.key));
        mix32 (0xFFu); // key terminator, so adjacent keys cannot run together
    }
    return hash;
}

const char* presetMessageId (PresetWireKind kind) {
    switch (kind) {
    case PresetWireKind::Handshake:
    case PresetWireKind::Command:
    case PresetWireKind::Reply: return kPresetRequestMessageId;
    case PresetWireKind::View: return kPresetViewMessageId;
    case PresetWireKind::CatalogPage: return kPresetCatalogMessageId;
    }
    return nullptr;
}

brom_preset_result paginateProgramTable (const PresetProgramTable& table,
                                         std::vector<PresetCatalogPage>& pages) {
    if (table.mappingGeneration == 0 || table.slots.size () > kPresetProgramMaxSlots)
        return BROM_PRESET_INVALID;
    std::vector<PresetCatalogPage> built;
    const uint32_t total = static_cast<uint32_t> (table.slots.size ());
    const uint32_t pageCount = total == 0 ? 1u
        : (total + kPresetCatalogPageEntries - 1) / static_cast<uint32_t> (kPresetCatalogPageEntries);
    for (uint32_t index = 0; index < pageCount; ++index) {
        PresetCatalogPage page;
        page.mappingGeneration = table.mappingGeneration;
        page.pageIndex = index;
        page.totalCount = total;
        const uint32_t first = index * static_cast<uint32_t> (kPresetCatalogPageEntries);
        page.entryCount = static_cast<uint8_t> (
            total == 0 ? 0u : std::min<uint32_t> (kPresetCatalogPageEntries, total - first));
        for (uint8_t i = 0; i < page.entryCount; ++i) page.entries[i] = table.slots[first + i];
        built.push_back (page);
    }
    pages = std::move (built);
    return BROM_PRESET_OK;
}

void PresetProgramAssembler::reset () {
    active_ = false;
    generation_ = 0;
    total_ = 0;
    nextPage_ = 0;
    slots_.clear ();
}

PresetProgramAssembler::Status PresetProgramAssembler::add (const PresetCatalogPage& page,
                                                            PresetProgramTable& completed) {
    if (!pageShapeValid (page)) {
        reset ();
        return Status::Rejected;
    }
    if (page.pageIndex == 0) {
        // A repeat of the first page of the assembly in progress is a duplicate.
        if (active_ && generation_ == page.mappingGeneration) {
            reset ();
            return Status::Rejected;
        }
        reset ();
        active_ = true;
        generation_ = page.mappingGeneration;
        total_ = page.totalCount;
    } else if (!active_ || generation_ != page.mappingGeneration || total_ != page.totalCount ||
               nextPage_ != page.pageIndex) {
        reset ();
        return Status::Rejected;
    }
    for (uint8_t i = 0; i < page.entryCount; ++i) slots_.push_back (page.entries[i]);
    ++nextPage_;
    if (slots_.size () < total_) return Status::Pending;
    completed.mappingGeneration = generation_;
    completed.slots = std::move (slots_);
    reset ();
    return Status::Complete;
}

brom_preset_result encodePresetMessage (const PresetWireContext& context,
                                        const PresetWireMessage& message,
                                        std::vector<uint8_t>& bytes) {
    if (!schemaValid (context.schema) || !productKeyValid (context.productKey))
        return BROM_PRESET_INVALID;
    if (!knownKind (static_cast<uint8_t> (message.kind))) return BROM_PRESET_INVALID;
    const brom_preset_schema& schema = *context.schema;

    Writer w;
    w.u16 (kPresetWireVersion);
    w.u8 (static_cast<uint8_t> (message.kind));
    w.u8 (0);
    w.u64 (message.epoch);
    w.u64 (message.sequence);
    w.u64 (presetSchemaId (schema));
    const std::size_t keyLength = std::strlen (context.productKey);
    w.u8 (static_cast<uint8_t> (keyLength));
    w.bytes (context.productKey, keyLength);

    switch (message.kind) {
    case PresetWireKind::Handshake: {
        const auto& h = message.handshake;
        if (h.role != static_cast<uint8_t> (PresetHandshakeRole::ControllerHello) &&
            h.role != static_cast<uint8_t> (PresetHandshakeRole::ProcessorWelcome))
            return BROM_PRESET_INVALID;
        w.u8 (h.role);
        w.u64 (h.sequenceFloor);
        break;
    }
    case PresetWireKind::Command: {
        const auto& c = message.command;
        if (c.request == 0) return BROM_PRESET_INVALID;
        w.u64 (c.request);
        w.u64 (c.dependsOn);
        w.u64 (c.generation);
        if (!knownCommandKind (static_cast<uint8_t> (c.kind))) return BROM_PRESET_INVALID;
        w.u8 (static_cast<uint8_t> (c.kind));
        switch (c.kind) {
        case PresetCommandKind::Load:
            putSnapshot (w, schema, c.snapshot);
            putIdentity (w, c.identity);
            break;
        case PresetCommandKind::Compare:
        case PresetCommandKind::Acknowledge: break;
        case PresetCommandKind::Edit:
            if (!std::isfinite (c.plainValue) || !knownParameter (schema, c.parameterId))
                return BROM_PRESET_INVALID;
            w.u32 (c.parameterId);
            w.f64 (c.plainValue);
            break;
        case PresetCommandKind::SaveAck:
            w.u64 (c.completion.ticket_id);
            w.u64 (c.completion.generation);
            w.u64 (c.completion.working_revision);
            putSnapshot (w, schema, c.completion.snapshot);
            putIdentity (w, c.completion.identity);
            break;
        default: return BROM_PRESET_INVALID;
        }
        break;
    }
    case PresetWireKind::Reply: {
        const auto& r = message.reply;
        if (r.request == 0 || static_cast<unsigned> (r.result) > BROM_PRESET_CONFIRMATION_REQUIRED)
            return BROM_PRESET_INVALID;
        w.u64 (r.request);
        w.u8 (static_cast<uint8_t> (r.result));
        w.u64 (r.generation);
        w.u8 (r.complete ? 1 : 0);
        break;
    }
    case PresetWireKind::View: {
        uint8_t session[BROM_PRESET_SESSION_MAX_BYTES];
        std::size_t written = 0;
        const auto encoded = brom_preset_session_encode (&schema, context.productKey, &message.view,
                                                         session, sizeof (session), &written);
        if (encoded != BROM_PRESET_OK) return encoded == BROM_PRESET_CAPACITY ? encoded : BROM_PRESET_INVALID;
        w.u64 (message.view.generation);
        w.u64 (message.view.working_revision);
        w.u64 (message.view.publication_sequence);
        w.u32 (message.viewRevision);
        w.u32 (static_cast<uint32_t> (written));
        w.bytes (session, written);
        break;
    }
    case PresetWireKind::CatalogPage: {
        const auto& page = message.catalog;
        if (!pageShapeValid (page)) return BROM_PRESET_INVALID;
        w.u64 (page.mappingGeneration);
        w.u32 (page.pageIndex);
        w.u32 (page.totalCount);
        w.u8 (page.entryCount);
        for (uint8_t i = 0; i < page.entryCount; ++i) {
            const auto& entry = page.entries[i];
            if (!availabilityValid (entry.availability)) return BROM_PRESET_INVALID;
            putIdentity (w, entry.identity);
            w.u8 (static_cast<uint8_t> (entry.availability));
            if (entry.availability == BROM_PRESET_OK) putSnapshot (w, schema, entry.snapshot);
        }
        break;
    }
    }
    if (w.result != BROM_PRESET_OK) return w.result;
    bytes = std::move (w.out);
    return BROM_PRESET_OK;
}

brom_preset_result decodePresetMessage (const void* data, std::size_t size,
                                        const PresetWireContext& context,
                                        PresetWireMessage& message) {
    if (size > kPresetWireMaxBytes) return BROM_PRESET_CAPACITY;
    if (!data || !schemaValid (context.schema) || !productKeyValid (context.productKey))
        return BROM_PRESET_INVALID;
    const brom_preset_schema& schema = *context.schema;

    Reader r (static_cast<const uint8_t*> (data), size);
    PresetWireMessage decoded {};
    if (r.u16 () != kPresetWireVersion) return BROM_PRESET_INVALID;
    const uint8_t kind = r.u8 ();
    const uint8_t reserved = r.u8 ();
    decoded.epoch = r.u64 ();
    decoded.sequence = r.u64 ();
    const uint64_t schemaId = r.u64 ();
    const uint8_t keyLength = r.u8 ();
    const uint8_t* key = r.take (keyLength);
    if (r.failed || !knownKind (kind) || reserved != 0) return BROM_PRESET_INVALID;
    decoded.kind = static_cast<PresetWireKind> (kind);

    if (keyLength != std::strlen (context.productKey) ||
        std::memcmp (key, context.productKey, keyLength) != 0 ||
        schemaId != presetSchemaId (schema))
        return BROM_PRESET_INVALID;
    if (decoded.kind != PresetWireKind::Handshake && decoded.epoch != context.epoch)
        return BROM_PRESET_STALE;

    switch (decoded.kind) {
    case PresetWireKind::Handshake: {
        decoded.handshake.role = r.u8 ();
        decoded.handshake.sequenceFloor = r.u64 ();
        if (decoded.handshake.role != static_cast<uint8_t> (PresetHandshakeRole::ControllerHello) &&
            decoded.handshake.role != static_cast<uint8_t> (PresetHandshakeRole::ProcessorWelcome))
            return BROM_PRESET_INVALID;
        break;
    }
    case PresetWireKind::Command: {
        auto& c = decoded.command;
        c.request = r.u64 ();
        c.dependsOn = r.u64 ();
        c.generation = r.u64 ();
        const uint8_t commandKind = r.u8 ();
        if (r.failed || c.request == 0) return BROM_PRESET_INVALID;
        switch (commandKind) {
        case static_cast<uint8_t> (PresetCommandKind::Load):
            c.kind = PresetCommandKind::Load;
            if (!getSnapshot (r, schema, c.snapshot) || !getIdentity (r, c.identity))
                return BROM_PRESET_INVALID;
            break;
        case static_cast<uint8_t> (PresetCommandKind::Compare):
            c.kind = PresetCommandKind::Compare;
            break;
        case static_cast<uint8_t> (PresetCommandKind::Acknowledge):
            c.kind = PresetCommandKind::Acknowledge;
            break;
        case static_cast<uint8_t> (PresetCommandKind::Edit):
            c.kind = PresetCommandKind::Edit;
            c.parameterId = r.u32 ();
            c.plainValue = r.f64 ();
            if (r.failed || !std::isfinite (c.plainValue) || !knownParameter (schema, c.parameterId))
                return BROM_PRESET_INVALID;
            break;
        case static_cast<uint8_t> (PresetCommandKind::SaveAck):
            c.kind = PresetCommandKind::SaveAck;
            c.completion.ticket_id = r.u64 ();
            c.completion.generation = r.u64 ();
            c.completion.working_revision = r.u64 ();
            if (!getSnapshot (r, schema, c.completion.snapshot) ||
                !getIdentity (r, c.completion.identity))
                return BROM_PRESET_INVALID;
            break;
        default: return BROM_PRESET_INVALID;
        }
        break;
    }
    case PresetWireKind::Reply: {
        auto& reply = decoded.reply;
        reply.request = r.u64 ();
        const uint8_t result = r.u8 ();
        reply.generation = r.u64 ();
        const uint8_t complete = r.u8 ();
        if (r.failed || reply.request == 0 || result > BROM_PRESET_CONFIRMATION_REQUIRED ||
            complete > 1)
            return BROM_PRESET_INVALID;
        reply.result = static_cast<brom_preset_result> (result);
        reply.complete = complete == 1;
        break;
    }
    case PresetWireKind::View: {
        const uint64_t generation = r.u64 ();
        const uint64_t revision = r.u64 ();
        const uint64_t publication = r.u64 ();
        decoded.viewRevision = r.u32 ();
        const uint32_t length = r.u32 ();
        if (r.failed || length == 0 || length > BROM_PRESET_SESSION_MAX_BYTES)
            return BROM_PRESET_INVALID;
        const uint8_t* session = r.take (length);
        if (!session) return BROM_PRESET_INVALID;
        if (brom_preset_session_decode (session, length, &schema, context.productKey,
                                        &decoded.view) != BROM_PRESET_OK ||
            decoded.view.generation != generation || decoded.view.working_revision != revision)
            return BROM_PRESET_INVALID;
        decoded.view.publication_sequence = publication;
        break;
    }
    case PresetWireKind::CatalogPage: {
        auto& page = decoded.catalog;
        page.mappingGeneration = r.u64 ();
        page.pageIndex = r.u32 ();
        page.totalCount = r.u32 ();
        page.entryCount = r.u8 ();
        if (r.failed || !pageShapeValid (page)) return BROM_PRESET_INVALID;
        for (uint8_t i = 0; i < page.entryCount; ++i) {
            auto& entry = page.entries[i];
            if (!getIdentity (r, entry.identity)) return BROM_PRESET_INVALID;
            const uint8_t availability = r.u8 ();
            if (r.failed || !availabilityValid (static_cast<brom_preset_result> (availability)))
                return BROM_PRESET_INVALID;
            entry.availability = static_cast<brom_preset_result> (availability);
            if (entry.availability == BROM_PRESET_OK && !getSnapshot (r, schema, entry.snapshot))
                return BROM_PRESET_INVALID;
        }
        break;
    }
    }
    if (!r.finished ()) return BROM_PRESET_INVALID;
    message = decoded;
    return BROM_PRESET_OK;
}

brom_preset_result writeEncodedPresetMessage (Vst::IMessage& host, PresetWireKind kind,
                                              const std::vector<uint8_t>& bytes) {
    const char* id = presetMessageId (kind);
    Vst::IAttributeList* attributes = host.getAttributes ();
    if (!id || !attributes || bytes.empty () || bytes.size () > kPresetWireMaxBytes)
        return BROM_PRESET_INVALID;
    host.setMessageID (id);
    if (attributes->setBinary (kPayloadAttribute, bytes.data (),
                               static_cast<uint32> (bytes.size ())) != kResultOk)
        return BROM_PRESET_IO_ERROR;
    return BROM_PRESET_OK;
}

brom_preset_result writePresetMessage (Vst::IMessage& host, const PresetWireContext& context,
                                       const PresetWireMessage& message) {
    std::vector<uint8_t> bytes;
    const auto encoded = encodePresetMessage (context, message, bytes);
    if (encoded != BROM_PRESET_OK) return encoded;
    return writeEncodedPresetMessage (host, message.kind, bytes);
}

brom_preset_result readPresetMessage (Vst::IMessage& host, const PresetWireContext& context,
                                      PresetWireMessage& message) {
    const char* id = host.getMessageID ();
    Vst::IAttributeList* attributes = host.getAttributes ();
    if (!id || !attributes) return BROM_PRESET_INVALID;
    const void* data = nullptr;
    uint32 size = 0;
    if (attributes->getBinary (kPayloadAttribute, data, size) != kResultOk || !data)
        return BROM_PRESET_INVALID;
    PresetWireMessage decoded {};
    const auto result = decodePresetMessage (data, size, context, decoded);
    if (result != BROM_PRESET_OK) return result;
    const char* expected = presetMessageId (decoded.kind);
    if (!expected || std::strcmp (id, expected) != 0) return BROM_PRESET_INVALID;
    message = decoded;
    return BROM_PRESET_OK;
}

} // namespace Steinberg::PokustonVst3Support
