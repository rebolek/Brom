// Generic VST3 shell for a brom-effect! (effect ABI revision 2).
//
// The host half of a VST3 effect, written once. It drives the effect linked
// into the binary through the trampolines brom/plugin/effect.h declares and
// brom/plugin/vst3/effect-shell.rcl defines, and knows nothing about what the
// effect sounds like. The CLAP shell (brom/plugin/clap/entry.rcl) is the
// reference for every behaviour; packages/recoil-brom/docs/effect-abi.md,
// "VST3 and AU", says where VST3 has to differ and why.
//
// Identity is packaging data and arrives as compile definitions from the
// product's VST3 project: BROM_VST3_PROCESSOR_UID and BROM_VST3_CONTROLLER_UID
// (four comma-separated 32-bit words each), POKUSTON_PLUGIN_NAME,
// POKUSTON_PLUGIN_VENDOR and POKUSTON_PLUGIN_VERSION.

#include "packages/recoil-brom/brom/plugin/effect.h"
#include "packages/recoil-brom/brom/plugin/vst3/effect_editor.h"

#include "base/source/timer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/common/pluginview.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <vector>

#if !defined(BROM_VST3_PROCESSOR_UID) || !defined(BROM_VST3_CONTROLLER_UID)
#error "BROM_VST3_PROCESSOR_UID and BROM_VST3_CONTROLLER_UID come from the product's VST3 project"
#endif

namespace Steinberg::BromEffectVst3 {

static const FUID kProcessorUID (BROM_VST3_PROCESSOR_UID);
static const FUID kControllerUID (BROM_VST3_CONTROLLER_UID);

constexpr int32 kCapacity = 512;
static_assert (kCapacity == BROM_AUDIO_CAPACITY, "shell chunk size is the audio block capacity");

// VST3 delivers MIDI controllers only as parameters, through IMidiMapping. Each
// controller number gets a hidden parameter in a fixed id range, so adding an
// effect parameter never moves them. The channel is not represented: every
// channel maps to the same parameter and reaches the effect as channel 0.
constexpr Vst::ParamID kMidiCCBase = 0x40000000;
constexpr int32 kMidiCCCount = 128;
// Program changes (and, for an effect with a factory bank, the bank) are the
// parameter right after the effect's own, as Punkva's VST3 build numbered it.
// With no bank it is hidden and spans the 128 MIDI program numbers.
constexpr uint32 kHiddenProgramCount = 128;
// Parameter queues one block may carry: every effect parameter, the program
// parameter and every controller.
constexpr int32 kMaxQueues = 256 + 1 + kMidiCCCount;

static const char* const kSharedMessageId = "BromEffectShared";
static const char* const kSharedAttribute = "block";

static const brom_effect_info& effectInfo () { return *brom_effect_describe (); }
static uint32 paramCount () { return static_cast<uint32> (effectInfo ().param_count); }
static Vst::ParamID programParamId () { return paramCount (); }
static uint32 programCount () {
    const uint32 bank = brom_effect_factory_count ();
    return bank > 0 ? bank : kHiddenProgramCount;
}
static bool variableTail () {
    return (effectInfo ().flags & BROM_EFFECT_VARIABLE_TAIL) != 0;
}

static void fromAscii (const char* source, Vst::String128 destination) {
    UString (destination, 128).fromAscii (source ? source : "");
}

// The VST3 subcategory for an effect category (effect-abi.md, "Shell
// interface"); anything unlisted is plain "Fx".
static const char* subcategoryFor (const char* category) {
    struct Entry { const char* category; const char* subcategory; };
    static const Entry table[] = {
        {"reverb", Vst::PlugType::kFxReverb},
        {"delay", Vst::PlugType::kFxDelay},
        {"filter", Vst::PlugType::kFxFilter},
        {"equalizer", Vst::PlugType::kFxEQ},
        {"compressor", Vst::PlugType::kFxDynamics},
        {"limiter", Vst::PlugType::kFxDynamics},
        {"distortion", Vst::PlugType::kFxDistortion},
        {"chorus", Vst::PlugType::kFxModulation},
        {"flanger", Vst::PlugType::kFxModulation},
        {"phaser", Vst::PlugType::kFxModulation},
        {"pitch-shifter", Vst::PlugType::kFxPitchShift},
        {"analyzer", Vst::PlugType::kFxAnalyzer},
        {"utility", Vst::PlugType::kFxTools},
    };
    if (category)
        for (const auto& entry : table)
            if (std::strcmp (category, entry.category) == 0) return entry.subcategory;
    return Vst::PlugType::kFx;
}

static int32 roundedIndex (double normalized, uint32 count) {
    if (count <= 1) return 0;
    const int32 last = static_cast<int32> (count) - 1;
    return std::clamp (static_cast<int32> (std::llround (normalized * last)), 0, last);
}

// What the processor publishes and the controller reads: the latency, tail and
// current-program caches and the restart request. Both halves run in one
// process (the shell is not kDistributable); the processor allocates it and
// hands its address to the controller in a message. Reference counted because
// either half may be released first.
struct SharedBlock {
    std::atomic<int32> refs {1};
    std::atomic<uint32> latency {0};
    std::atomic<uint32> tail {0};
    std::atomic<int32> program {-1};
    std::atomic<bool> restartWanted {false};
};

static SharedBlock* retainShared (SharedBlock* block) {
    if (block) block->refs.fetch_add (1, std::memory_order_relaxed);
    return block;
}

static void releaseShared (SharedBlock* block) {
    if (block && block->refs.fetch_sub (1, std::memory_order_acq_rel) == 1) delete block;
}

static bool readAll (IBStream* stream, void* bytes, int32 size) {
    auto* cursor = static_cast<uint8*> (bytes);
    int32 total = 0;
    while (total < size) {
        int32 received = 0;
        if (stream->read (cursor + total, size - total, &received) != kResultOk || received <= 0)
            return false;
        total += received;
    }
    return true;
}

static bool writeAll (IBStream* stream, const void* bytes, int32 size) {
    const auto* cursor = static_cast<const uint8*> (bytes);
    int32 total = 0;
    while (total < size) {
        int32 written = 0;
        if (stream->write (const_cast<uint8*> (cursor) + total, size - total, &written) != kResultOk ||
            written <= 0)
            return false;
        total += written;
    }
    return true;
}

// ── Sample conversion ──────────────────────────────────────────────────────
//
// One host bus chunk to or from a planar block. T is the effect's sample type,
// H the host's. Only the first `frames` samples of a plane are written, as in
// the CLAP helpers. A null plane, or one the host flags silent, reads as zero.

template <typename T, typename H>
static void readPlane (T* plane, H* const* buffers, int32 channel, uint64 silence,
                       int32 offset, int32 frames) {
    const H* source = buffers ? buffers[channel] : nullptr;
    if (!source || (silence & (uint64 (1) << channel)) != 0) {
        std::fill (plane, plane + frames, T (0));
        return;
    }
    for (int32 i = 0; i < frames; ++i) plane[i] = static_cast<T> (source[offset + i]);
}

template <typename T, typename H, typename Block>
static void readBus (Block& block, const Vst::AudioBusBuffers* bus, H* const* buffers,
                     int32 planes, int32 offset, int32 frames) {
    std::fill (block.left, block.left + frames, T (0));
    std::fill (block.right, block.right + frames, T (0));
    if (!bus) return;
    const uint64 silence = bus->silenceFlags;
    if (planes > 0) readPlane<T, H> (block.left, buffers, 0, silence, offset, frames);
    if (planes > 1) readPlane<T, H> (block.right, buffers, 1, silence, offset, frames);
}

template <typename H, typename Block>
static void writeBus (const Block& block, H* const* buffers, int32 offset, int32 frames) {
    for (int32 i = 0; i < frames; ++i) {
        buffers[0][offset + i] = static_cast<H> (block.left[i]);
        buffers[1][offset + i] = static_cast<H> (block.right[i]);
    }
}

template <typename H>
static H* const* busBuffers (const Vst::AudioBusBuffers& bus);
template <>
float* const* busBuffers<float> (const Vst::AudioBusBuffers& bus) { return bus.channelBuffers32; }
template <>
double* const* busBuffers<double> (const Vst::AudioBusBuffers& bus) { return bus.channelBuffers64; }

static bool processChunk (void* core, brom_audio_f32& in, brom_audio_f32& side,
                          brom_audio_f32& out, int32 channels, int32 sideChannels, int32 frames) {
    return brom_effect_process_f32 (core, &in, &side, &out, channels, sideChannels, frames);
}
static bool processChunk (void* core, brom_audio_f64& in, brom_audio_f64& side,
                          brom_audio_f64& out, int32 channels, int32 sideChannels, int32 frames) {
    return brom_effect_process_f64 (core, &in, &side, &out, channels, sideChannels, frames);
}

// ── Processor ──────────────────────────────────────────────────────────────

class Processor final : public Vst::AudioEffect {
public:
    Processor () { setControllerClass (kControllerUID); }
    ~Processor () SMTG_OVERRIDE {
        if (active_) brom_effect_deactivate (core_);
        brom_effect_destroy (core_);
        releaseShared (shared_);
    }

    static FUnknown* createInstance (void*) {
        return static_cast<Vst::IAudioProcessor*> (new Processor);
    }

    tresult PLUGIN_API initialize (FUnknown* context) SMTG_OVERRIDE {
        const auto result = AudioEffect::initialize (context);
        if (result != kResultOk) return result;
        core_ = brom_effect_create ();
        shared_ = new SharedBlock;
        if (!core_) return kResultFalse;
        const auto& info = effectInfo ();
        // Stereo when the effect takes it, the host may select mono.
        mainChannels_ = (info.input_layouts & 2u) != 0 ? 2 : 1;
        addAudioInput (STR16 ("Main In"),
                       mainChannels_ == 2 ? Vst::SpeakerArr::kStereo : Vst::SpeakerArr::kMono);
        if (info.sidechain_channels != 0)
            addAudioInput (STR16 ("Sidechain"),
                           info.sidechain_channels == 1 ? Vst::SpeakerArr::kMono
                                                        : Vst::SpeakerArr::kStereo,
                           Vst::kAux, 0);
        addAudioOutput (STR16 ("Main Out"), Vst::SpeakerArr::kStereo);
        addEventInput (STR16 ("MIDI In"), 16);
        return kResultOk;
    }

    tresult PLUGIN_API connect (Vst::IConnectionPoint* other) SMTG_OVERRIDE {
        const auto result = AudioEffect::connect (other);
        if (result != kResultTrue || !shared_) return result;
        // The controller learns the shared block's address once, on the UI
        // thread, before any processing.
        if (auto* message = allocateMessage ()) {
            message->setMessageID (kSharedMessageId);
            SharedBlock* block = shared_;
            message->getAttributes ()->setBinary (kSharedAttribute, &block, sizeof (block));
            sendMessage (message);
            message->release ();
        }
        return result;
    }

    tresult PLUGIN_API setBusArrangements (Vst::SpeakerArrangement* inputs, int32 numIns,
                                           Vst::SpeakerArrangement* outputs,
                                           int32 numOuts) SMTG_OVERRIDE {
        const auto& info = effectInfo ();
        const int32 expectedIns = info.sidechain_channels != 0 ? 2 : 1;
        if (numIns != expectedIns || numOuts != 1 || !inputs || !outputs) return kResultFalse;
        if (outputs[0] != Vst::SpeakerArr::kStereo) return kResultFalse;
        const int32 main = Vst::SpeakerArr::getChannelCount (inputs[0]);
        const bool mainOk = (main == 1 && inputs[0] == Vst::SpeakerArr::kMono &&
                             (info.input_layouts & 1u) != 0) ||
                            (main == 2 && inputs[0] == Vst::SpeakerArr::kStereo &&
                             (info.input_layouts & 2u) != 0);
        if (!mainOk) return kResultFalse;
        if (expectedIns == 2) {
            const int32 side = Vst::SpeakerArr::getChannelCount (inputs[1]);
            if (side < 1 || side > static_cast<int32> (info.sidechain_channels)) return kResultFalse;
        }
        const auto result = AudioEffect::setBusArrangements (inputs, numIns, outputs, numOuts);
        if (result == kResultTrue) mainChannels_ = main;
        return result;
    }

    tresult PLUGIN_API canProcessSampleSize (int32 size) SMTG_OVERRIDE {
        // An f32 effect declines 64-bit so the host converts; an f64 effect
        // converts 32-bit buffers itself.
        if (size == Vst::kSample32) return kResultTrue;
        if (size == Vst::kSample64 && effectInfo ().sample_format == BROM_SAMPLE_F64)
            return kResultTrue;
        return kResultFalse;
    }

    tresult PLUGIN_API setupProcessing (Vst::ProcessSetup& setup) SMTG_OVERRIDE {
        const auto result = AudioEffect::setupProcessing (setup);
        if (result != kResultOk || setup.sampleRate <= 0.0 || setup.maxSamplesPerBlock <= 0)
            return kResultFalse;
        sampleRate_ = setup.sampleRate;
        maxFrames_ = setup.maxSamplesPerBlock;
        return kResultOk;
    }

    tresult PLUGIN_API setActive (TBool state) SMTG_OVERRIDE {
        if (state) {
            if (active_) return AudioEffect::setActive (state);
            if (sampleRate_ <= 0.0 || maxFrames_ <= 0 ||
                !brom_effect_activate (core_, sampleRate_, maxFrames_))
                return kResultFalse;
            brom_effect_reset (core_);
            active_ = true;
            activateQuery ();
            const auto result = AudioEffect::setActive (state);
            if (result != kResultOk) deactivateCore ();
            return result;
        }
        deactivateCore ();
        return AudioEffect::setActive (state);
    }

    tresult PLUGIN_API setProcessing (TBool state) SMTG_OVERRIDE {
        AudioEffect::setProcessing (state);
        return kResultTrue;
    }

    tresult PLUGIN_API setState (IBStream* state) SMTG_OVERRIDE {
        const int32 size = effectInfo ().state_size;
        if (!state || size <= 0) return kResultFalse;
        std::vector<uint8> bytes (static_cast<size_t> (size));
        if (!readAll (state, bytes.data (), size)) return kResultFalse;
        return brom_effect_state_load (core_, bytes.data (), bytes.size ()) ? kResultOk : kResultFalse;
    }

    tresult PLUGIN_API getState (IBStream* state) SMTG_OVERRIDE {
        const int32 size = effectInfo ().state_size;
        if (!state || size <= 0) return kResultFalse;
        std::vector<uint8> bytes (static_cast<size_t> (size));
        if (!brom_effect_state_save (core_, bytes.data (), bytes.size ())) return kResultFalse;
        return writeAll (state, bytes.data (), size) ? kResultOk : kResultFalse;
    }

    // Both are called on the UI thread and read only the caches the audio
    // thread publishes, never the effect.
    uint32 PLUGIN_API getLatencySamples () SMTG_OVERRIDE {
        return shared_ ? shared_->latency.load (std::memory_order_acquire) : 0;
    }

    uint32 PLUGIN_API getTailSamples () SMTG_OVERRIDE {
        // VST3 cannot tell a host the tail changed, so an effect whose tail may
        // grow while active reports infinite rather than be cut off.
        if (variableTail ()) return Vst::kInfiniteTail;
        return shared_ ? shared_->tail.load (std::memory_order_acquire) : 0;
    }

    tresult PLUGIN_API process (Vst::ProcessData& data) SMTG_OVERRIDE {
        if (!core_ || !shared_) return kResultFalse;
        if (data.symbolicSampleSize != Vst::kSample32 && data.symbolicSampleSize != Vst::kSample64)
            return kResultFalse;
        if (data.numSamples > 0) {
            // Structural errors fail the block; a missing main input is silence.
            if (data.numOutputs < 1 || !data.outputs) return kResultFalse;
            const auto& out = data.outputs[0];
            const bool wide = data.symbolicSampleSize == Vst::kSample64;
            if (out.numChannels < 2) return kResultFalse;
            if (wide ? (!out.channelBuffers64 || !out.channelBuffers64[0] || !out.channelBuffers64[1])
                     : (!out.channelBuffers32 || !out.channelBuffers32[0] || !out.channelBuffers32[1]))
                return kResultFalse;
            if (data.numInputs > 0 && data.inputs && data.inputs[0].numChannels < mainChannels_)
                return kResultFalse;
        }
        runTimeline (data);
        if (data.numSamples > 0 && data.outputs) data.outputs[0].silenceFlags = 0;
        blockRequery ();
        return kResultOk;
    }

private:
    struct QueueCursor {
        Vst::IParamValueQueue* queue;
        int32 point;
        int32 count;
        int32 pass;
    };

    void deactivateCore () {
        if (!active_) return;
        brom_effect_deactivate (core_);
        active_ = false;
    }

    // Inside setActive (true), after activate and reset: the applied latency,
    // the tail and the current program, with no restart request.
    void activateQuery () {
        const uint32 reported = brom_effect_latency (core_);
        shared_->latency.store (reported & ~BROM_LATENCY_PENDING, std::memory_order_release);
        shared_->tail.store (brom_effect_tail (core_), std::memory_order_release);
        shared_->program.store (brom_effect_current_program (core_), std::memory_order_release);
        shared_->restartWanted.store (false, std::memory_order_release);
        restartLatched_ = false;
    }

    // At the end of every block, including zero-length ones. The new latency
    // is published at once, which is what the AU wrapper reads; a change or a
    // pending value asks the controller for one kLatencyChanged, latched until
    // the next activation.
    void blockRequery () {
        shared_->tail.store (brom_effect_tail (core_), std::memory_order_release);
        shared_->program.store (brom_effect_current_program (core_), std::memory_order_release);
        const uint32 reported = brom_effect_latency (core_);
        const uint32 applied = reported & ~BROM_LATENCY_PENDING;
        const bool pending = (reported & BROM_LATENCY_PENDING) != 0;
        const uint32 previous = shared_->latency.exchange (applied, std::memory_order_acq_rel);
        if (!restartLatched_ && (pending || applied != previous)) {
            restartLatched_ = true;
            shared_->restartWanted.store (true, std::memory_order_release);
        }
    }

    int32 passFor (Vst::ParamID id) const {
        if (id < paramCount ()) return 0;
        if (id >= kMidiCCBase && id < kMidiCCBase + kMidiCCCount) return 1;
        if (id == programParamId ()) return 2;
        return -1;
    }

    void applyPoint (Vst::ParamID id, Vst::ParamValue value) {
        if (id < paramCount ()) {
            brom_effect_param_set (core_, id, brom_effect_param_denormalize (id, value));
        } else if (id >= kMidiCCBase && id < kMidiCCBase + kMidiCCCount) {
            brom_effect_midi (core_, 0xB0u, id - kMidiCCBase,
                              static_cast<uint32> (roundedIndex (value, 128)));
        } else if (id == programParamId ()) {
            brom_effect_program (core_, static_cast<uint32> (roundedIndex (value, programCount ())));
        }
    }

    // Apply every point due at `rendered`, as a time group in the CLAP shell's
    // order: parameters, then MIDI controllers, then program changes.
    void applyDue (std::array<QueueCursor, kMaxQueues>& cursors, int32 used, int32 rendered,
                   int32 samples) {
        for (int32 pass = 0; pass < 3; ++pass) {
            for (int32 q = 0; q < used; ++q) {
                auto& cursor = cursors[q];
                if (cursor.pass != pass) continue;
                while (cursor.point < cursor.count) {
                    int32 offset = 0;
                    Vst::ParamValue value = 0.0;
                    if (cursor.queue->getPoint (cursor.point, offset, value) != kResultOk) {
                        ++cursor.point;
                        continue;
                    }
                    if (std::clamp (offset, 0, samples) > rendered) break;
                    applyPoint (cursor.queue->getParameterId (), value);
                    ++cursor.point;
                }
            }
        }
    }

    void runTimeline (Vst::ProcessData& data) {
        std::array<QueueCursor, kMaxQueues> cursors {};
        int32 used = 0;
        if (auto* changes = data.inputParameterChanges) {
            const int32 count = changes->getParameterCount ();
            for (int32 i = 0; i < count && used < kMaxQueues; ++i) {
                auto* queue = changes->getParameterData (i);
                if (!queue) continue;
                const int32 pass = passFor (queue->getParameterId ());
                if (pass < 0) continue;
                cursors[used++] = {queue, 0, queue->getPointCount (), pass};
            }
        }
        const int32 samples = std::max (data.numSamples, 0);
        int32 rendered = 0;
        while (true) {
            applyDue (cursors, used, rendered, samples);
            if (rendered >= samples) break;
            int32 boundary = samples;
            for (int32 q = 0; q < used; ++q) {
                auto& cursor = cursors[q];
                while (cursor.point < cursor.count) {
                    int32 offset = 0;
                    Vst::ParamValue value = 0.0;
                    if (cursor.queue->getPoint (cursor.point, offset, value) == kResultOk) {
                        boundary = std::min (boundary, std::clamp (offset, rendered, samples));
                        break;
                    }
                    ++cursor.point;
                }
            }
            // Every point at or before `rendered` was just applied, so this
            // cannot stall; the guard only bounds a misbehaving queue.
            if (boundary <= rendered) boundary = samples;
            renderRange (data, rendered, boundary - rendered);
            rendered = boundary;
        }
    }

    void renderRange (Vst::ProcessData& data, int32 start, int32 frames) {
        const bool wide = data.symbolicSampleSize == Vst::kSample64;
        const bool effect64 = effectInfo ().sample_format == BROM_SAMPLE_F64;
        for (int32 offset = start; offset < start + frames; offset += kCapacity) {
            const int32 chunk = std::min (kCapacity, start + frames - offset);
            if (effect64) {
                wide ? renderChunk<double> (data, offset, chunk, in64_, side64_, out64_)
                     : renderChunk<float> (data, offset, chunk, in64_, side64_, out64_);
            } else {
                wide ? renderChunk<double> (data, offset, chunk, in32_, side32_, out32_)
                     : renderChunk<float> (data, offset, chunk, in32_, side32_, out32_);
            }
        }
    }

    // Every plane is read into scratch before the effect runs and the output is
    // written after, so an in-place host buffer is safe in every width
    // combination. A refused chunk is written as silence.
    template <typename H, typename Block>
    void renderChunk (Vst::ProcessData& data, int32 offset, int32 frames, Block& in, Block& side,
                      Block& out) {
        using T = std::remove_reference_t<decltype (in.left[0])>;
        const Vst::AudioBusBuffers* main =
            data.numInputs > 0 && data.inputs ? &data.inputs[0] : nullptr;
        readBus<T, H> (in, main, main ? busBuffers<H> (*main) : nullptr, mainChannels_, offset,
                       frames);
        const int32 sideChannels = sidechainPlanes<H> (data);
        const Vst::AudioBusBuffers* sideBus = sideChannels > 0 ? &data.inputs[1] : nullptr;
        readBus<T, H> (side, sideBus, sideBus ? busBuffers<H> (*sideBus) : nullptr, sideChannels,
                       offset, frames);
        if (!processChunk (core_, in, side, out, mainChannels_, sideChannels, frames)) {
            std::fill (out.left, out.left + frames, T (0));
            std::fill (out.right, out.right + frames, T (0));
        }
        writeBus<H> (out, busBuffers<H> (data.outputs[0]), offset, frames);
    }

    // The sidechain planes read for this block: the host's channel count
    // clamped to the declared width, or 0 when the aux bus is absent,
    // inactive or carries no buffers.
    template <typename H>
    int32 sidechainPlanes (const Vst::ProcessData& data) {
        const uint32 declared = effectInfo ().sidechain_channels;
        if (declared == 0 || data.numInputs < 2 || !data.inputs) return 0;
        if (audioInputs.size () < 2 || !audioInputs[1] || !audioInputs[1]->isActive ()) return 0;
        const auto& bus = data.inputs[1];
        if (bus.numChannels <= 0 || !busBuffers<H> (bus)) return 0;
        return std::min (bus.numChannels, static_cast<int32> (declared));
    }

    void* core_ {nullptr};
    SharedBlock* shared_ {nullptr};
    double sampleRate_ {0.0};
    int32 maxFrames_ {0};
    int32 mainChannels_ {2};
    bool active_ {false};
    bool restartLatched_ {false};
    // One input, sidechain and output block per format; an effect uses one set.
    brom_audio_f32 in32_ {}, side32_ {}, out32_ {};
    brom_audio_f64 in64_ {}, side64_ {}, out64_ {};
};

// ── Controller ─────────────────────────────────────────────────────────────

class Controller final : public Vst::EditControllerEx1,
                         public Vst::IMidiMapping,
                         public ITimerCallback {
public:
    Controller () = default;
    ~Controller () SMTG_OVERRIDE { teardown (); }

    static FUnknown* createInstance (void*) {
        return static_cast<Vst::IEditController*> (new Controller);
    }

    OBJ_METHODS (Controller, EditControllerEx1)
    DEFINE_INTERFACES
        DEF_INTERFACE (IMidiMapping)
    END_DEFINE_INTERFACES (EditControllerEx1)
    REFCOUNT_METHODS (EditControllerEx1)

    tresult PLUGIN_API initialize (FUnknown* context) SMTG_OVERRIDE {
        const auto result = EditControllerEx1::initialize (context);
        if (result != kResultOk) return result;
        // A private instance mirrors the parameter values, decodes component
        // state and answers program queries; it is never activated.
        core_ = brom_effect_create ();
        if (!core_) return kResultFalse;
        const auto& info = effectInfo ();
        const uint32 bank = brom_effect_factory_count ();
        if (bank > 0) {
            Vst::String128 unitName {};
            fromAscii (info.name, unitName);
            addUnit (new Vst::Unit (unitName, Vst::kRootUnitId, Vst::kNoParentUnitId,
                                    programParamId ()));
        }
        for (uint32 index = 0; index < paramCount (); ++index) {
            brom_effect_param param {};
            if (!brom_effect_param_info (index, &param)) return kResultFalse;
            Vst::String128 title {};
            fromAscii (param.name, title);
            int32 flags = Vst::ParameterInfo::kCanAutomate;
            if (param.id == info.bypass_param_id) flags |= Vst::ParameterInfo::kIsBypass;
            parameters.addParameter (
                title, nullptr, param.steps > 1 ? param.steps - 1 : 0,
                brom_effect_param_normalize (param.id, param.default_value), flags, param.id);
        }
        if (bank > 0) {
            Vst::String128 listName {};
            fromAscii (info.name, listName);
            auto* programs = new Vst::ProgramList (listName, programParamId (), Vst::kRootUnitId);
            for (uint32 index = 0; index < bank; ++index) {
                char name[128] {};
                if (!brom_effect_factory_name (index, name, sizeof (name))) return kResultFalse;
                Vst::String128 title {};
                fromAscii (name, title);
                programs->addProgram (title);
            }
            addProgramList (programs);
            auto* programParameter = programs->getParameter ();
            programParameter->getInfo ().flags &= ~Vst::ParameterInfo::kCanAutomate;
            parameters.addParameter (programParameter);
        } else {
            parameters.addParameter (STR16 ("Program"), nullptr, kHiddenProgramCount - 1, 0.0,
                                     Vst::ParameterInfo::kIsHidden |
                                         Vst::ParameterInfo::kIsProgramChange,
                                     programParamId ());
        }
        for (int32 cc = 0; cc < kMidiCCCount; ++cc) {
            char name[32] {};
            std::snprintf (name, sizeof (name), "MIDI CC %d", static_cast<int> (cc));
            Vst::String128 title {};
            fromAscii (name, title);
            parameters.addParameter (title, nullptr, 127, 0.0, Vst::ParameterInfo::kIsHidden,
                                     kMidiCCBase + static_cast<Vst::ParamID> (cc));
        }
        // Runs whether or not an editor is open: it is how a latency change
        // the audio thread saw reaches the host.
        timer_ = Timer::create (this, 33);
        return kResultOk;
    }

    tresult PLUGIN_API terminate () SMTG_OVERRIDE {
        teardown ();
        return EditControllerEx1::terminate ();
    }

    tresult PLUGIN_API notify (Vst::IMessage* message) SMTG_OVERRIDE {
        if (message && std::strcmp (message->getMessageID (), kSharedMessageId) == 0) {
            const void* bytes = nullptr;
            uint32 size = 0;
            if (message->getAttributes ()->getBinary (kSharedAttribute, bytes, size) == kResultOk &&
                bytes && size == sizeof (SharedBlock*)) {
                SharedBlock* block = nullptr;
                std::memcpy (&block, bytes, sizeof (block));
                if (block != shared_) {
                    releaseShared (shared_);
                    shared_ = retainShared (block);
                }
            }
            return kResultOk;
        }
        return EditControllerEx1::notify (message);
    }

    void onTimer (Timer* timer) SMTG_OVERRIDE {
        if (timer != timer_ || !shared_) return;
        if (shared_->restartWanted.exchange (false, std::memory_order_acq_rel) && componentHandler)
            componentHandler->restartComponent (Vst::kLatencyChanged);
    }

    tresult PLUGIN_API getMidiControllerAssignment (int32 busIndex, int16 channel,
                                                    Vst::CtrlNumber controller,
                                                    Vst::ParamID& id) SMTG_OVERRIDE {
        if (busIndex != 0 || channel < 0 || channel > 15) return kResultFalse;
        if (controller == Vst::kCtrlProgramChange) {
            id = programParamId ();
            return kResultTrue;
        }
        if (controller >= 0 && controller < kMidiCCCount) {
            id = kMidiCCBase + static_cast<Vst::ParamID> (controller);
            return kResultTrue;
        }
        return kResultFalse;
    }

    tresult PLUGIN_API setComponentState (IBStream* state) SMTG_OVERRIDE {
        const int32 size = effectInfo ().state_size;
        if (!state || !core_ || size <= 0) return kResultFalse;
        std::vector<uint8> bytes (static_cast<size_t> (size));
        if (!readAll (state, bytes.data (), size) ||
            !brom_effect_state_load (core_, bytes.data (), bytes.size ()))
            return kResultFalse;
        for (uint32 id = 0; id < paramCount (); ++id)
            EditController::setParamNormalized (
                id, brom_effect_param_normalize (id, brom_effect_param_get (core_, id)));
        const int32 program = brom_effect_current_program (core_);
        if (program >= 0 && brom_effect_factory_count () > 0)
            EditController::setParamNormalized (programParamId (), programNormalized (program));
        return kResultOk;
    }

    tresult PLUGIN_API setParamNormalized (Vst::ParamID id, Vst::ParamValue value) SMTG_OVERRIDE {
        const auto result = EditControllerEx1::setParamNormalized (id, value);
        if (result != kResultOk || !core_) return result;
        if (id < paramCount ()) {
            brom_effect_param_set (core_, id, brom_effect_param_denormalize (id, value));
        } else if (id == programParamId () && !selectingProgram_ &&
                   brom_effect_factory_count () > 0) {
            selectProgram (roundedIndex (value, programCount ()));
        }
        return result;
    }

    Vst::ParamValue PLUGIN_API normalizedParamToPlain (Vst::ParamID id,
                                                       Vst::ParamValue value) SMTG_OVERRIDE {
        return id < paramCount () ? brom_effect_param_denormalize (id, value)
                                  : EditControllerEx1::normalizedParamToPlain (id, value);
    }

    Vst::ParamValue PLUGIN_API plainParamToNormalized (Vst::ParamID id,
                                                       Vst::ParamValue value) SMTG_OVERRIDE {
        return id < paramCount () ? brom_effect_param_normalize (id, value)
                                  : EditControllerEx1::plainParamToNormalized (id, value);
    }

    tresult PLUGIN_API getParamStringByValue (Vst::ParamID id, Vst::ParamValue normalized,
                                              Vst::String128 string) SMTG_OVERRIDE {
        if (id >= paramCount ())
            return EditControllerEx1::getParamStringByValue (id, normalized, string);
        const double plain = brom_effect_param_denormalize (id, normalized);
        char text[128] {};
        // The engine's own text first; plain number formatting is the fallback,
        // as in the CLAP shell.
        if (!brom_effect_param_text (core_, id, plain, text, sizeof (text)))
            std::snprintf (text, sizeof (text), "%.2f", plain);
        fromAscii (text, string);
        return kResultOk;
    }

    tresult PLUGIN_API getParamValueByString (Vst::ParamID id, Vst::TChar* string,
                                              Vst::ParamValue& normalized) SMTG_OVERRIDE {
        if (id >= paramCount ())
            return EditControllerEx1::getParamValueByString (id, string, normalized);
        char text[128] {};
        UString (string, 128).toAscii (text, sizeof (text));
        double plain = 0.0;
        if (!brom_effect_param_parse (core_, id, text, &plain)) {
            char* end = nullptr;
            plain = std::strtod (text, &end);
            if (end == text) return kResultFalse;
        }
        normalized = brom_effect_param_normalize (id, plain);
        return kResultOk;
    }

    // The product's editor when it supplies one (BROM_VST3_EDITOR) and the
    // engine declares has-editor; otherwise the host shows its own UI.
    IPlugView* PLUGIN_API createView (FIDString name) SMTG_OVERRIDE;

    // ── What the editor hook calls back (brom_vst3_host_*) ──
    void editFromEditor (uint32 id, double value, int phase) {
        if (id >= paramCount ()) return;
        const auto normalized = brom_effect_param_normalize (id, value);
        if (phase == 0) beginEdit (id);
        setParamNormalized (id, normalized);
        if (phase == 1) performEdit (id, normalized);
        if (phase == 2) endEdit (id);
    }
    void* editorCore () const { return core_; }
    int32 currentPreset () const { return core_ ? brom_effect_current_program (core_) : -1; }
    bool loadPresetFromEditor (uint32 index) {
        if (index >= brom_effect_factory_count ()) return false;
        selectProgram (static_cast<int32> (index), true);
        return true;
    }

private:
    double programNormalized (int32 index) const {
        const uint32 count = programCount ();
        if (count <= 1) return 0.0;
        return static_cast<double> (std::clamp (index, 0, static_cast<int32> (count) - 1)) /
               static_cast<double> (count - 1);
    }

    // A factory program chosen on the controller side: the private instance
    // takes it, every parameter shows the preset's value, and the host is told
    // they all moved. The processor applies the same program through the
    // program parameter.
    //
    // From the editor, `notifyHost` also performs the program parameter edit,
    // which is how the processor hears of it.
    void selectProgram (int32 index, bool notifyHost = false) {
        if (index < 0 || static_cast<uint32> (index) >= brom_effect_factory_count ()) return;
        selectingProgram_ = true;
        // An effect whose program callback ignores its bank still gets the
        // preset's values written into the private instance.
        const bool applied = brom_effect_program (core_, static_cast<uint32> (index));
        std::vector<double> values (paramCount ());
        if (paramCount () == 0 ||
            brom_effect_factory_values (static_cast<uint32> (index), values.data (),
                                        static_cast<uint32> (values.size ()))) {
            for (uint32 id = 0; id < paramCount (); ++id) {
                if (!applied) brom_effect_param_set (core_, id, values[id]);
                EditController::setParamNormalized (
                    id, brom_effect_param_normalize (id, values[id]));
            }
        }
        const auto normalized = programNormalized (index);
        EditController::setParamNormalized (programParamId (), normalized);
        selectingProgram_ = false;
        if (notifyHost) {
            beginEdit (programParamId ());
            performEdit (programParamId (), normalized);
            endEdit (programParamId ());
        }
        if (componentHandler) componentHandler->restartComponent (Vst::kParamValuesChanged);
    }

    void teardown () {
        if (timer_) {
            timer_->stop ();
            timer_->release ();
            timer_ = nullptr;
        }
        releaseShared (shared_);
        shared_ = nullptr;
        brom_effect_destroy (core_);
        core_ = nullptr;
    }

    void* core_ {nullptr};
    SharedBlock* shared_ {nullptr};
    Timer* timer_ {nullptr};
    bool selectingProgram_ {false};
};

#if defined(BROM_VST3_EDITOR)

// The product editor in a VST3 view. A main-thread timer pumps it every 33 ms
// while attached and tells it when the committed program changes, as
// Punkva's former per-product view did.
class EditorView final : public CPluginView, public ITimerCallback {
public:
    EditorView (Controller& controller, const ViewRect& rect)
    : CPluginView (&rect), controller_ (controller) {
        controller_.addRef ();
        editor_ = brom_vst3_editor_create (&controller_, controller_.editorCore ());
    }
    ~EditorView () SMTG_OVERRIDE {
        removedFromParent ();
        brom_vst3_editor_destroy (editor_);
        controller_.release ();
    }

    tresult PLUGIN_API isPlatformTypeSupported (FIDString type) SMTG_OVERRIDE {
#if defined(__APPLE__)
        return type && std::strcmp (type, kPlatformTypeNSView) == 0 ? kResultTrue : kResultFalse;
#elif defined(__linux__)
        return type && std::strcmp (type, kPlatformTypeX11EmbedWindowID) == 0 ? kResultTrue
                                                                              : kResultFalse;
#else
        return kResultFalse;
#endif
    }

    tresult PLUGIN_API attached (void* parent, FIDString type) SMTG_OVERRIDE {
        if (!parent || !editor_ || timer_ || isPlatformTypeSupported (type) != kResultTrue)
            return kResultFalse;
        if (!brom_vst3_editor_attach (editor_, parent, rect.getWidth (), rect.getHeight ()))
            return kResultFalse;
        systemWindow = parent;
        brom_vst3_editor_tick (editor_);
        timer_ = Timer::create (this, 33);
        if (!timer_) {
            brom_vst3_editor_detach (editor_);
            systemWindow = nullptr;
            return kResultFalse;
        }
        return kResultOk;
    }

    void removedFromParent () SMTG_OVERRIDE {
        if (timer_) {
            timer_->stop ();
            timer_->release ();
            timer_ = nullptr;
        }
        if (editor_) brom_vst3_editor_detach (editor_);
        systemWindow = nullptr;
    }

    void onTimer (Timer* timer) SMTG_OVERRIDE {
        if (timer != timer_) return;
        brom_vst3_editor_tick (editor_);
        const int32 current = controller_.currentPreset ();
        if (current != preset_) {
            preset_ = current;
            brom_vst3_editor_preset_changed (editor_, current);
        }
    }

private:
    Controller& controller_;
    void* editor_ {nullptr};
    Timer* timer_ {nullptr};
    int32 preset_ {-2};
};

IPlugView* PLUGIN_API Controller::createView (FIDString name) {
    int32_t width = 0, height = 0;
    if (!name || std::strcmp (name, Vst::ViewType::kEditor) != 0) return nullptr;
    if (!brom_effect_editor_size (&width, &height) || width <= 0 || height <= 0) return nullptr;
    const ViewRect rect {0, 0, width, height};
    return new EditorView (*this, rect);
}

#else

IPlugView* PLUGIN_API Controller::createView (FIDString) { return nullptr; }

#endif

} // namespace Steinberg::BromEffectVst3

#if defined(BROM_VST3_EDITOR)

using Steinberg::BromEffectVst3::Controller;

extern "C" void brom_vst3_host_edit_param (void* host, uint32_t id, double value, int phase) {
    static_cast<Controller*> (host)->editFromEditor (id, value, phase);
}

extern "C" uint32_t brom_vst3_host_preset_count (void*) { return brom_effect_factory_count (); }

extern "C" int32_t brom_vst3_host_current_preset (void* host) {
    return static_cast<Controller*> (host)->currentPreset ();
}

extern "C" int brom_vst3_host_preset_name (void*, uint32_t index, char* out, uint32_t capacity) {
    if (!out || capacity == 0) return 0;
    out[0] = '\0';
    return brom_effect_factory_name (index, out, capacity);
}

extern "C" int brom_vst3_host_load_preset (void* host, uint32_t index) {
    return static_cast<Controller*> (host)->loadPresetFromEditor (index) ? 1 : 0;
}

#endif

using namespace Steinberg;

// Hand-written rather than BEGIN_FACTORY_DEF: the table is validated before
// anything is published, and a broken one exposes no factory at all, as the
// CLAP entry's init refuses to load. The subcategory comes from the category.
SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory () {
    using namespace Steinberg::BromEffectVst3;
    if (gPluginFactory) {
        gPluginFactory->addRef ();
        return gPluginFactory;
    }
    if (!brom_effect_init ()) return nullptr;
    static PFactoryInfo factoryInfo (POKUSTON_PLUGIN_VENDOR, "", "", Vst::kDefaultFactoryFlags);
    gPluginFactory = new CPluginFactory (factoryInfo);
    {
        TUID cid = INLINE_UID_FROM_FUID (kProcessorUID);
        // Not kDistributable: processor and controller share memory.
        PClassInfo2 processorClass (cid, PClassInfo::kManyInstances, kVstAudioEffectClass,
                                    POKUSTON_PLUGIN_NAME, 0,
                                    subcategoryFor (effectInfo ().category), nullptr,
                                    POKUSTON_PLUGIN_VERSION, kVstVersionString);
        gPluginFactory->registerClass (&processorClass, Processor::createInstance);
    }
    {
        TUID cid = INLINE_UID_FROM_FUID (kControllerUID);
        PClassInfo2 controllerClass (cid, PClassInfo::kManyInstances,
                                     kVstComponentControllerClass,
                                     POKUSTON_PLUGIN_NAME " Controller", 0, "", nullptr,
                                     POKUSTON_PLUGIN_VERSION, kVstVersionString);
        gPluginFactory->registerClass (&controllerClass, Controller::createInstance);
    }
    return gPluginFactory;
}
