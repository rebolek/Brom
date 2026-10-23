#pragma once

#include "pluginterfaces/base/ibstream.h"

#include <array>
#include <cstddef>
#include <cstdint>

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

} // namespace Steinberg::PokustonVst3Support
