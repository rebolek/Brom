#pragma once

#include "pluginterfaces/base/ibstream.h"

#include "../preset/session.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Steinberg::PokustonVst3Support {

template <std::size_t Size, typename Load>
bool readState (IBStream* stream, void* core, Load&& load) {
    if (!stream || !core) return false;
    std::array<uint8_t, Size> bytes {};
    int32 total = 0;
    while (total < static_cast<int32> (bytes.size ())) {
        int32 received = 0;
        if (stream->read (bytes.data () + total,
                          static_cast<int32> (bytes.size ()) - total,
                          &received) != kResultOk || received <= 0) return false;
        total += received;
    }
    return load (core, bytes.data (), bytes.size ());
}

template <std::size_t Size, typename Save>
bool writeState (IBStream* stream, const void* core, Save&& save) {
    if (!stream || !core) return false;
    std::array<uint8_t, Size> bytes {};
    if (!save (core, bytes.data (), bytes.size ())) return false;
    int32 total = 0;
    while (total < static_cast<int32> (bytes.size ())) {
        int32 written = 0;
        if (stream->write (bytes.data () + total,
                           static_cast<int32> (bytes.size ()) - total,
                           &written) != kResultOk || written <= 0) return false;
        total += written;
    }
    return true;
}

// ---- variable-size Brom preset sessions ---------------------------------------
// The session envelope is a 16-byte header (magic, version, flags, payload size,
// checksum) followed by exactly the declared payload. These helpers frame by that
// header, move whole envelopes through streams that return short counts, and
// decode into temporary storage so a refused stream leaves the caller's view alone.

inline constexpr std::size_t kPresetSessionHeaderBytes = BROM_PRESET_SESSION_HEADER_BYTES;
static_assert (kPresetSessionHeaderBytes == 16 && BROM_PRESET_SESSION_PAYLOAD_SIZE_OFFSET + 4 <= kPresetSessionHeaderBytes,
               "the payload size field lies inside the 16-byte session header");

namespace detail {

inline bool moveExactly (IBStream& stream, uint8_t* bytes, std::size_t size, bool reading) {
    std::size_t total = 0;
    while (total < size) {
        const int32 want = static_cast<int32> (std::min<std::size_t> (size - total, 1u << 20));
        int32 moved = 0;
        const tresult result = reading ? stream.read (bytes + total, want, &moved)
                                       : stream.write (bytes + total, want, &moved);
        if (result != kResultOk || moved <= 0) return false; // failure, end of data or no progress
        total += static_cast<std::size_t> (moved);
    }
    return true;
}

} // namespace detail

inline brom_preset_result writePresetSession (IBStream* stream, const brom_preset_schema& schema,
                                              const char* productKey,
                                              const brom_preset_view& view) {
    if (!stream) return BROM_PRESET_INVALID;
    uint8_t bytes[BROM_PRESET_SESSION_MAX_BYTES];
    std::size_t written = 0;
    const brom_preset_result encoded =
        brom_preset_session_encode (&schema, productKey, &view, bytes, sizeof (bytes), &written);
    if (encoded != BROM_PRESET_OK) return encoded;
    return detail::moveExactly (*stream, bytes, written, false) ? BROM_PRESET_OK
                                                                 : BROM_PRESET_IO_ERROR;
}

inline brom_preset_result readPresetSession (IBStream* stream, const brom_preset_schema& schema,
                                             const char* productKey, brom_preset_view& view) {
    if (!stream) return BROM_PRESET_INVALID;
    uint8_t bytes[BROM_PRESET_SESSION_MAX_BYTES];
    if (!detail::moveExactly (*stream, bytes, kPresetSessionHeaderBytes, true))
        return BROM_PRESET_IO_ERROR;
    if (std::memcmp (bytes, BROM_PRESET_SESSION_MAGIC, 4) != 0) return BROM_PRESET_INVALID;
    constexpr std::size_t at = BROM_PRESET_SESSION_PAYLOAD_SIZE_OFFSET;
    const uint32_t payload = static_cast<uint32_t> (bytes[at]) | static_cast<uint32_t> (bytes[at + 1]) << 8 |
                             static_cast<uint32_t> (bytes[at + 2]) << 16 |
                             static_cast<uint32_t> (bytes[at + 3]) << 24;
    // The declared size is checked before anything more is read from the stream.
    if (payload > BROM_PRESET_SESSION_MAX_BYTES - kPresetSessionHeaderBytes)
        return BROM_PRESET_CAPACITY;
    if (!detail::moveExactly (*stream, bytes + kPresetSessionHeaderBytes, payload, true))
        return BROM_PRESET_IO_ERROR;
    brom_preset_view decoded {};
    const brom_preset_result result = brom_preset_session_decode (
        bytes, kPresetSessionHeaderBytes + payload, &schema, productKey, &decoded);
    if (result == BROM_PRESET_OK) view = decoded;
    return result;
}

} // namespace Steinberg::PokustonVst3Support
