#pragma once

#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace Steinberg::PokustonVst3Support {

// Allocation-free, sample-exact host timeline shared by all Pokuston instruments.
// Parameter changes precede note events at an identical sample offset.
template <std::size_t ParameterCount, typename SetParameter, typename DispatchEvent,
          typename Render>
tresult processTimeline (Vst::ProcessData& data, int32 capacity,
                         SetParameter&& setParameter,
                         DispatchEvent&& dispatchEvent, Render&& render) {
    std::array<Vst::IParamValueQueue*, ParameterCount> queues {};
    std::array<int32, ParameterCount> points {};
    if (data.inputParameterChanges) {
        for (int32 i = 0; i < data.inputParameterChanges->getParameterCount (); ++i) {
            auto* queue = data.inputParameterChanges->getParameterData (i);
            if (!queue) continue;
            const auto id = queue->getParameterId ();
            if (id < ParameterCount && !queues[id]) queues[id] = queue;
        }
    }

    int32 eventIndex = 0;
    const int32 eventCount = data.inputEvents ? data.inputEvents->getEventCount () : 0;
    int32 rendered = 0;
    while (rendered < data.numSamples) {
        int32 nextBoundary = data.numSamples;
        for (uint32 id = 0; id < ParameterCount; ++id) {
            int32 offset = 0;
            Vst::ParamValue value = 0.0;
            auto* queue = queues[id];
            while (queue && points[id] < queue->getPointCount () &&
                   queue->getPoint (points[id], offset, value) != kResultOk) ++points[id];
            if (queue && points[id] < queue->getPointCount ())
                nextBoundary = std::min (
                    nextBoundary, std::clamp (offset, rendered, data.numSamples));
        }

        Vst::Event nextEvent {};
        while (eventIndex < eventCount &&
               data.inputEvents->getEvent (eventIndex, nextEvent) != kResultOk) ++eventIndex;
        if (eventIndex < eventCount)
            nextBoundary = std::min (
                nextBoundary,
                std::clamp (nextEvent.sampleOffset, rendered, data.numSamples));

        if (nextBoundary > rendered) {
            const int32 frames = std::min (nextBoundary - rendered, capacity);
            render (rendered, frames);
            rendered += frames;
            continue;
        }

        for (uint32 id = 0; id < ParameterCount; ++id) {
            auto* queue = queues[id];
            while (queue && points[id] < queue->getPointCount ()) {
                int32 offset = 0;
                Vst::ParamValue value = 0.0;
                if (queue->getPoint (points[id], offset, value) != kResultOk) {
                    ++points[id];
                    continue;
                }
                if (std::clamp (offset, 0, data.numSamples) > rendered) break;
                setParameter (id, value);
                ++points[id];
            }
        }
        while (eventIndex < eventCount) {
            Vst::Event event {};
            if (data.inputEvents->getEvent (eventIndex, event) != kResultOk) {
                ++eventIndex;
                continue;
            }
            if (std::clamp (event.sampleOffset, 0, data.numSamples) > rendered) break;
            dispatchEvent (event);
            ++eventIndex;
        }
    }
    return kResultOk;
}

} // namespace Steinberg::PokustonVst3Support
