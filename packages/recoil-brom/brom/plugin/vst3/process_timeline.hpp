#pragma once

#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace Steinberg::PokustonVst3Support {

namespace detail {

// Drain=false is the long-standing timeline: events at or past the end of the
// buffer, and every event of a zero-frame call, are left unconsumed. Drain=true
// also dispatches those, once and in order, and then calls finishTimeline.
template <std::size_t ParameterCount, bool Drain, typename SetParameter, typename DispatchEvent,
          typename Render, typename FinishTimeline>
tresult processTimelineImpl (Vst::ProcessData& data, int32 capacity,
                             SetParameter&& setParameter,
                             DispatchEvent&& dispatchEvent, Render&& render,
                             FinishTimeline&& finishTimeline) {
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
    // A zero-frame call of the original timeline consumes what is due at sample 0 and
    // returns. The run-to-completion timeline (Drain) instead falls through to the drain
    // below, which consumes everything once and calls finishTimeline.
    if (!Drain && data.numSamples == 0) {
        for (uint32 id = 0; id < ParameterCount; ++id) {
            auto* queue = queues[id];
            while (queue && points[id] < queue->getPointCount ()) {
                int32 offset = 0;
                Vst::ParamValue value = 0.0;
                if (queue->getPoint (points[id], offset, value) != kResultOk) {
                    ++points[id];
                    continue;
                }
                if (offset > 0) break;
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
            if (event.sampleOffset > 0) break;
            dispatchEvent (event);
            ++eventIndex;
        }
        return kResultOk;
    }
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
    if constexpr (Drain) {
        for (uint32 id = 0; id < ParameterCount; ++id) {
            auto* queue = queues[id];
            while (queue && points[id] < queue->getPointCount ()) {
                int32 offset = 0;
                Vst::ParamValue value = 0.0;
                if (queue->getPoint (points[id], offset, value) == kResultOk)
                    setParameter (id, value);
                ++points[id];
            }
        }
        for (; eventIndex < eventCount; ++eventIndex) {
            Vst::Event event {};
            if (data.inputEvents->getEvent (eventIndex, event) == kResultOk) dispatchEvent (event);
        }
        finishTimeline ();
    }
    return kResultOk;
}

} // namespace detail

// Allocation-free, sample-exact host timeline shared by all Pokuston instruments.
// Parameter changes precede note events at an identical sample offset.
template <std::size_t ParameterCount, typename SetParameter, typename DispatchEvent,
          typename Render>
tresult processTimeline (Vst::ProcessData& data, int32 capacity,
                         SetParameter&& setParameter,
                         DispatchEvent&& dispatchEvent, Render&& render) {
    return detail::processTimelineImpl<ParameterCount, false> (
        data, capacity, setParameter, dispatchEvent, render, [] {});
}

// The same timeline, run to completion: every parameter change and event is
// consumed exactly once, including those at the final sample boundary and those
// of a zero-frame call, and finishTimeline runs exactly once afterwards. Samples
// that need rendering require capacity > 0.
template <std::size_t ParameterCount, typename SetParameter, typename DispatchEvent,
          typename Render, typename FinishTimeline>
tresult processTimeline (Vst::ProcessData& data, int32 capacity,
                         SetParameter&& setParameter,
                         DispatchEvent&& dispatchEvent, Render&& render,
                         FinishTimeline&& finishTimeline) {
    if (data.numSamples > 0 && capacity <= 0) return kInvalidArgument;
    return detail::processTimelineImpl<ParameterCount, true> (
        data, capacity, setParameter, dispatchEvent, render, finishTimeline);
}

} // namespace Steinberg::PokustonVst3Support
