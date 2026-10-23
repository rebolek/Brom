// Generic AUv2 shell for a brom-effect! (effect ABI revision 2).
//
// The host half of an Audio Unit effect, written once, on Apple's
// AudioUnitSDK (AUBase). It drives the effect linked into the binary through
// the trampolines brom/plugin/effect.h declares and
// brom/plugin/vst3/effect-shell.rcl defines, and knows nothing about what the
// effect sounds like. The CLAP shell (brom/plugin/clap/entry.rcl) is the
// reference for every behaviour and the VST3 shell
// (brom/plugin/vst3/effect_vst3.cpp) its closest sibling;
// packages/recoil-brom/docs/effect-abi.md, "AU", says where AU differs and why.
//
// Identity is packaging data: the component type (aufx, or aumf for an effect
// that declares MIDI_INPUT), subtype and manufacturer live in the bundle's
// Info.plist, which brom-effect-au.cmake writes from the product's AU project.
// The code is the same for both types. The compile definitions are POKUSTON_PLUGIN_NAME and,
// with an editor, BROM_AU_VIEW_FACTORY_CLASS and BROM_AU_VIEW_CLASS (Objective-C
// class names, which share one flat namespace across every plug-in a host
// loads, so they carry the product's codes).

#include "packages/recoil-brom/brom/plugin/effect.h"
#include "packages/recoil-brom/brom/plugin/au/effect_editor.h"

#include <AudioUnitSDK/AUBase.h>
#include <AudioUnitSDK/AUPlugInDispatch.h>
#include <AudioUnitSDK/AUUtility.h>

#include <AudioToolbox/AudioUnitUtilities.h>
#include <CoreFoundation/CoreFoundation.h>

#if defined(BROM_AU_EDITOR)
#import <AudioToolbox/AUCocoaUIView.h>
#import <Cocoa/Cocoa.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wfunction-effects"
#pragma clang diagnostic ignored "-Wperf-constraint-implies-noexcept"

#if defined(BROM_AU_EDITOR) && (!defined(BROM_AU_VIEW_FACTORY_CLASS) || !defined(BROM_AU_VIEW_CLASS))
#error "an AU editor needs BROM_AU_VIEW_FACTORY_CLASS and BROM_AU_VIEW_CLASS from brom_add_effect_au"
#endif

#define BROM_AU_STRINGIFY_(x) #x
#define BROM_AU_STRINGIFY(x) BROM_AU_STRINGIFY_(x)

extern "C" AUSDK_EXPORT void* BromEffectAUFactory (const AudioComponentDescription* description);

namespace bromau {

constexpr UInt32 kCapacity = 512;
static_assert (kCapacity == BROM_AUDIO_CAPACITY, "shell chunk size is the audio block capacity");

// The shell's own property: the instance pointer, for the Cocoa view factory,
// which receives only the AudioUnit handle. In the custom range (>= 64000).
constexpr AudioUnitPropertyID kInstanceProperty = 64042;

// MIDI events queued between the host's MusicDeviceMIDIEvent calls and the
// render they belong to. A full queue drops further events for that render.
constexpr UInt32 kMidiQueue = 512;
// Scheduled parameter points one render may carry; extra points are dropped.
constexpr UInt32 kMaxScheduled = 1024;
// Main-thread housekeeping period: the VST3 controller timer's 33 ms.
constexpr double kTimerSeconds = 0.033;

static const brom_effect_info& effectInfo () { return *brom_effect_describe (); }
static UInt32 paramCount () { return static_cast<UInt32> (effectInfo ().param_count); }
static bool variableTail () { return (effectInfo ().flags & BROM_EFFECT_VARIABLE_TAIL) != 0; }
static bool isFloatPCM (const AudioStreamBasicDescription& format) {
    return format.mFormatID == kAudioFormatLinearPCM &&
           (format.mFormatFlags & kAudioFormatFlagIsFloat) != 0 &&
           (format.mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0 &&
           (format.mBitsPerChannel == 32 || format.mBitsPerChannel == 64) &&
           format.mBytesPerFrame == format.mBitsPerChannel / 8 && format.mFramesPerPacket == 1;
}

// ── Sample conversion ──────────────────────────────────────────────────────
//
// One AU element chunk to or from a planar block. T is the effect's sample
// type; `wide` says whether the element carries Float64 or Float32 planes.
// Only the first `frames` samples of a plane are written, as in the CLAP
// helpers. A missing or null plane, or an input the host flagged silent,
// reads as zero.

template <typename T>
static void readPlane (T* plane, const AudioBufferList* list, UInt32 channel, bool wide,
                       bool silent, UInt32 offset, UInt32 frames) {
    const void* data = (list && channel < list->mNumberBuffers) ? list->mBuffers[channel].mData
                                                                : nullptr;
    if (!data || silent) {
        std::fill (plane, plane + frames, T (0));
        return;
    }
    if (wide) {
        const auto* source = static_cast<const double*> (data) + offset;
        for (UInt32 i = 0; i < frames; ++i) plane[i] = static_cast<T> (source[i]);
    } else {
        const auto* source = static_cast<const float*> (data) + offset;
        for (UInt32 i = 0; i < frames; ++i) plane[i] = static_cast<T> (source[i]);
    }
}

template <typename T, typename Block>
static void readElement (Block& block, const AudioBufferList* list, UInt32 planes, bool wide,
                         bool silent, UInt32 offset, UInt32 frames) {
    std::fill (block.left, block.left + frames, T (0));
    std::fill (block.right, block.right + frames, T (0));
    if (planes > 0) readPlane<T> (block.left, list, 0, wide, silent, offset, frames);
    if (planes > 1) readPlane<T> (block.right, list, 1, wide, silent, offset, frames);
}

template <typename T>
static void writePlane (const T* plane, AudioBufferList& list, UInt32 channel, bool wide,
                        UInt32 offset, UInt32 frames) {
    void* data = channel < list.mNumberBuffers ? list.mBuffers[channel].mData : nullptr;
    if (!data) return;
    if (wide) {
        auto* target = static_cast<double*> (data) + offset;
        for (UInt32 i = 0; i < frames; ++i) target[i] = static_cast<double> (plane[i]);
    } else {
        auto* target = static_cast<float*> (data) + offset;
        for (UInt32 i = 0; i < frames; ++i) target[i] = static_cast<float> (plane[i]);
    }
}

static bool processChunk (void* core, brom_audio_f32& in, brom_audio_f32& side,
                          brom_audio_f32& out, int32_t channels, int32_t sideChannels,
                          int32_t frames) {
    return brom_effect_process_f32 (core, &in, &side, &out, channels, sideChannels, frames);
}
static bool processChunk (void* core, brom_audio_f64& in, brom_audio_f64& side,
                          brom_audio_f64& out, int32_t channels, int32_t sideChannels,
                          int32_t frames) {
    return brom_effect_process_f64 (core, &in, &side, &out, channels, sideChannels, frames);
}

static AudioStreamBasicDescription floatFormat (Float64 rate, UInt32 channels) {
    return ausdk::ASBD::CreateCommonFloat32 (rate, channels);
}

static CFStringRef copyString (const char* text) {
    return CFStringCreateWithCString (kCFAllocatorDefault, text ? text : "",
                                      kCFStringEncodingUTF8);
}

static CFStringRef stateKey () { return CFSTR ("brom-state"); }

struct MidiEvent {
    UInt32 offset;
    UInt32 status, data1, data2;
};

struct ScheduledPoint {
    UInt32 offset;
    AudioUnitParameterID id;
    AudioUnitParameterValue value;
    bool writeElement;
};

class BromEffectAU final : public ausdk::AUBase {
public:
    explicit BromEffectAU (AudioComponentInstance instance)
    : AUBase (instance, effectInfo ().sidechain_channels != 0 ? 2 : 1, 1) {}

    ~BromEffectAU () override {
        if (timer_) {
            CFRunLoopTimerInvalidate (timer_);
            CFRelease (timer_);
            timer_ = nullptr;
        }
        closeViews ();
        if (active_) brom_effect_deactivate (core_);
        brom_effect_destroy (core_);
        brom_effect_destroy (uiCore_);
        for (auto& preset : presets_)
            if (preset.presetName) CFRelease (preset.presetName);
    }

    void PostConstructor () override {
        const auto& info = effectInfo ();
        hasSidechain_ = info.sidechain_channels != 0;
        mainChannels_ = (info.input_layouts & 2u) != 0 ? 2 : 1;
        Input (0).SetName (CFSTR ("Main In"));
        Input (0).SetStreamFormat (floatFormat (kAUDefaultSampleRate, mainChannels_));
        if (hasSidechain_) {
            Input (1).SetName (CFSTR ("Sidechain"));
            Input (1).SetStreamFormat (floatFormat (kAUDefaultSampleRate, info.sidechain_channels));
        }
        Output (0).SetName (CFSTR ("Main Out"));
        Output (0).SetStreamFormat (floatFormat (kAUDefaultSampleRate, 2));

        // The audio instance and a private one that mirrors the parameters for
        // text, presets and the editor, as the VST3 controller keeps.
        core_ = brom_effect_create ();
        uiCore_ = brom_effect_create ();
        const UInt32 count = paramCount ();
        lastApplied_.assign (count, 0.0f);
        uiMirror_.assign (count, 0.0f);
        indexed_.assign (count, 0);
        for (UInt32 id = 0; id < count; ++id) {
            brom_effect_param param {};
            indexed_[id] = brom_effect_param_info (id, &param) && indexed (param) ? 1 : 0;
        }
        scheduledIds_.assign (count, 0);
        scheduled_.resize (kMaxScheduled);
        pendingState_.assign (static_cast<size_t> (std::max (info.state_size, 0)), 0);
        Globals ()->UseIndexedParameters (count);
        for (UInt32 id = 0; id < count; ++id) {
            const auto value = core_ ? toHost (id, brom_effect_param_get (core_, id)) : 0.0f;
            Globals ()->SetParameter (id, value);
            lastApplied_[id] = value;
            uiMirror_[id] = value;
        }
        const UInt32 bank = brom_effect_factory_count ();
        presets_.resize (bank);
        for (UInt32 index = 0; index < bank; ++index) {
            char name[256] {};
            brom_effect_factory_name (index, name, sizeof (name));
            presets_[index].presetNumber = static_cast<SInt32> (index);
            presets_[index].presetName = copyString (name);
        }

        CFRunLoopTimerContext context {0, this, nullptr, nullptr, nullptr};
        timer_ = CFRunLoopTimerCreate (kCFAllocatorDefault,
                                       CFAbsoluteTimeGetCurrent () + kTimerSeconds,
                                       kTimerSeconds, 0, 0, &BromEffectAU::timerFired, &context);
        if (timer_) CFRunLoopAddTimer (CFRunLoopGetMain (), timer_, kCFRunLoopCommonModes);
    }

    // ── Lifecycle ──────────────────────────────────────────────────────────

    OSStatus Initialize () override {
        if (!core_ || !uiCore_) return kAudioUnitErr_FailedInitialization;
        const auto& info = effectInfo ();
        const UInt32 main = Input (0).NumberChannels ();
        const bool mainOk = (main == 1 && (info.input_layouts & 1u) != 0) ||
                            (main == 2 && (info.input_layouts & 2u) != 0);
        if (!mainOk || Output (0).NumberChannels () != 2) return kAudioUnitErr_FormatNotSupported;
        if (hasSidechain_) {
            const UInt32 side = Input (1).NumberChannels ();
            if (side < 1 || side > info.sidechain_channels) return kAudioUnitErr_FormatNotSupported;
        }
        const Float64 rate = Output (0).GetStreamFormat ().mSampleRate;
        const UInt32 maxFrames = GetMaxFramesPerSlice ();
        if (rate <= 0.0 || maxFrames == 0) return kAudioUnitErr_FailedInitialization;
        if (!brom_effect_activate (core_, rate, static_cast<int32_t> (maxFrames)))
            return kAudioUnitErr_FailedInitialization;
        brom_effect_reset (core_);
        mainChannels_ = main;
        sideChannels_ = hasSidechain_ ? Input (1).NumberChannels () : 0;
        mainWide_ = Input (0).GetStreamFormat ().mBitsPerChannel == 64;
        sideWide_ = hasSidechain_ && Input (1).GetStreamFormat ().mBitsPerChannel == 64;
        outWide_ = Output (0).GetStreamFormat ().mBitsPerChannel == 64;
        midiRead_.store (midiWrite_.load (std::memory_order_acquire), std::memory_order_release);
        resetPending_.store (false, std::memory_order_release);
        active_ = true;
        activateQuery ();
        return noErr;
    }

    void Cleanup () override {
        if (!active_) return;
        brom_effect_deactivate (core_);
        active_ = false;
    }

    // A reset may arrive on any thread, so an active instance resets at the
    // start of the next render rather than under a running one.
    OSStatus Reset (AudioUnitScope scope, AudioUnitElement element) override {
        if (scope == kAudioUnitScope_Global && active_)
            resetPending_.store (true, std::memory_order_release);
        return AUBase::Reset (scope, element);
    }

    bool StreamFormatWritable (AudioUnitScope, AudioUnitElement) override {
        return !IsInitialized ();
    }

    bool CanScheduleParameters () const AUSDK_RTSAFE override { return true; }

    // Float32 or Float64, non-interleaved, per element: the main input in a
    // layout the effect accepts, the sidechain up to its declared width, and
    // a stereo output. Each element converts independently.
    bool ValidFormat (AudioUnitScope scope, AudioUnitElement element,
                      const AudioStreamBasicDescription& format) override {
        if (!isFloatPCM (format)) return false;
        const auto& info = effectInfo ();
        const UInt32 channels = format.mChannelsPerFrame;
        if (scope == kAudioUnitScope_Output) return element == 0 && channels == 2;
        if (scope == kAudioUnitScope_Input) {
            if (element == 0)
                return (channels == 1 && (info.input_layouts & 1u) != 0) ||
                       (channels == 2 && (info.input_layouts & 2u) != 0);
            if (element == 1 && hasSidechain_)
                return channels >= 1 && channels <= info.sidechain_channels;
            return false;
        }
        if (scope == kAudioUnitScope_Global) return channels == 2;
        return false;
    }

    UInt32 SupportedNumChannels (const AUChannelInfo** outInfo) override {
        const auto& info = effectInfo ();
        channelInfo_[0] = {2, 2};
        channelInfo_[1] = {1, 2};
        UInt32 count = 0;
        const AUChannelInfo* first = nullptr;
        if ((info.input_layouts & 2u) != 0 && (info.input_layouts & 1u) != 0) {
            first = &channelInfo_[0];
            count = 2;
        } else if ((info.input_layouts & 2u) != 0) {
            first = &channelInfo_[0];
            count = 1;
        } else {
            first = &channelInfo_[1];
            count = 1;
        }
        if (outInfo) *outInfo = first;
        return count;
    }

    // ── Latency and tail ───────────────────────────────────────────────────
    //
    // Both read only the caches the render thread publishes, never the effect.

    Float64 GetLatency () AUSDK_RTSAFE override {
        const Float64 rate = sampleRate ();
        return rate > 0.0 ? latency_.load (std::memory_order_acquire) / rate : 0.0;
    }

    Float64 GetTailTime () AUSDK_RTSAFE override {
        const Float64 rate = sampleRate ();
        return rate > 0.0 ? reportedTail () / rate : 0.0;
    }

    bool SupportsTail () AUSDK_RTSAFE override { return true; }

    // ── Parameters ─────────────────────────────────────────────────────────

    OSStatus GetParameterInfo (AudioUnitScope scope, AudioUnitParameterID id,
                               AudioUnitParameterInfo& out) override {
        if (scope != kAudioUnitScope_Global || id >= paramCount ())
            return kAudioUnitErr_InvalidParameter;
        brom_effect_param param {};
        if (!brom_effect_param_info (id, &param)) return kAudioUnitErr_InvalidParameter;
        out.flags = kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable |
                    kAudioUnitParameterFlag_ValuesHaveStrings;
        FillInParameterName (out, copyString (param.name), true);
        // A host slider and automation lane are linear between min and max,
        // so every parameter but an indexed one is its normalized 0..1
        // position and the shell applies the engine's taper, as the VST3 shell
        // does; the host shows the engine's text for it. An indexed parameter
        // keeps its whole steps, which a host lists as a menu.
        if (indexed (param)) {
            out.unit = kAudioUnitParameterUnit_Indexed;
            out.minValue = static_cast<AudioUnitParameterValue> (param.min_value);
            out.maxValue = static_cast<AudioUnitParameterValue> (param.max_value);
        } else {
            out.unit = kAudioUnitParameterUnit_Generic;
            out.minValue = 0.0f;
            out.maxValue = 1.0f;
        }
        out.defaultValue = toHost (id, param.default_value);
        return noErr;
    }

    // An indexed (whole-step) parameter lists one string per step, from the
    // engine's own text.
    OSStatus GetParameterValueStrings (AudioUnitScope scope, AudioUnitParameterID id,
                                       CFArrayRef* outStrings) override {
        brom_effect_param param {};
        if (scope != kAudioUnitScope_Global || id >= paramCount () ||
            !brom_effect_param_info (id, &param) || !indexed (param))
            return kAudioUnitErr_InvalidProperty;
        if (!outStrings) return noErr;
        auto* strings = CFArrayCreateMutable (kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
        for (int32_t step = 0; step < param.steps; ++step) {
            char text[128] {};
            valueText (id, param.min_value + step, text, sizeof (text));
            CFStringRef string = copyString (text);
            CFArrayAppendValue (strings, string);
            CFRelease (string);
        }
        *outStrings = strings;
        return noErr;
    }

    // ── Presets and state ──────────────────────────────────────────────────

    OSStatus GetPresets (CFArrayRef* outData) const override {
        if (presets_.empty ()) return kAudioUnitErr_InvalidProperty;
        if (!outData) return noErr;
        auto* array = CFArrayCreateMutable (kCFAllocatorDefault, 0, nullptr);
        for (const auto& preset : presets_) CFArrayAppendValue (array, &preset);
        *outData = array;
        return noErr;
    }

    OSStatus NewFactoryPresetSet (const AUPreset& preset) override {
        if (preset.presetNumber < 0 || static_cast<size_t> (preset.presetNumber) >= presets_.size ())
            return kAudioUnitErr_InvalidPropertyValue;
        selectProgram (static_cast<UInt32> (preset.presetNumber));
        SetAFactoryPresetAsCurrent (presets_[static_cast<size_t> (preset.presetNumber)]);
        return noErr;
    }

    // The SDK's dictionary (parameter values, preset name) plus the effect's
    // own state bytes under "brom-state", read from the audio instance as the
    // CLAP and VST3 shells do.
    OSStatus SaveState (CFPropertyListRef* outData) override {
        const auto result = AUBase::SaveState (outData);
        const int32_t size = effectInfo ().state_size;
        if (result != noErr || size <= 0) return result;
        std::vector<UInt8> bytes (static_cast<size_t> (size));
        if (!brom_effect_state_save (core_, bytes.data (), bytes.size ())) {
            CFRelease (*outData);
            *outData = nullptr;
            return kAudioUnitErr_CannotDoInCurrentContext;
        }
        CFDataRef data = CFDataCreate (kCFAllocatorDefault, bytes.data (), size);
        CFDictionarySetValue (
            static_cast<CFMutableDictionaryRef> (const_cast<void*> (*outData)), stateKey (), data);
        CFRelease (data);
        return noErr;
    }

    // "brom-state", when present, must be exactly state-size bytes the engine
    // accepts; it is checked on the private instance before anything changes.
    // An active instance takes the bytes at the start of its next render, so
    // the load never runs under a render; an inactive one takes them now.
    OSStatus RestoreState (CFPropertyListRef plist) override {
        if (!plist || CFGetTypeID (plist) != CFDictionaryGetTypeID ())
            return kAudioUnitErr_InvalidPropertyValue;
        const int32_t size = effectInfo ().state_size;
        const auto* data = static_cast<CFDataRef> (
            CFDictionaryGetValue (static_cast<CFDictionaryRef> (plist), stateKey ()));
        if (data && (CFGetTypeID (data) != CFDataGetTypeID () || size <= 0 ||
                     CFDataGetLength (data) != size))
            return kAudioUnitErr_InvalidPropertyValue;
        const auto result = AUBase::RestoreState (plist);
        if (result != noErr) return result;
        const UInt32 count = paramCount ();
        if (data) {
            const UInt8* bytes = CFDataGetBytePtr (data);
            if (!brom_effect_state_load (uiCore_, bytes, static_cast<size_t> (size)))
                return kAudioUnitErr_InvalidPropertyValue;
            for (UInt32 id = 0; id < count; ++id) {
                const auto value = toHost (id, brom_effect_param_get (uiCore_, id));
                Globals ()->SetParameter (id, value);
                uiMirror_[id] = value;
            }
            if (active_) {
                std::memcpy (pendingState_.data (), bytes, static_cast<size_t> (size));
                statePending_.store (true, std::memory_order_release);
            } else {
                brom_effect_state_load (core_, bytes, static_cast<size_t> (size));
                for (UInt32 id = 0; id < count; ++id) lastApplied_[id] = Globals ()->GetParameter (id);
                program_.store (brom_effect_current_program (core_), std::memory_order_release);
            }
        } else if (!active_) {
            // The SDK restored only parameter values: apply them now.
            for (UInt32 id = 0; id < count; ++id) applyElementValue (id);
        }
        notifyAllParameters ();
        return noErr;
    }

    // ── Properties ─────────────────────────────────────────────────────────

    OSStatus GetPropertyInfo (AudioUnitPropertyID id, AudioUnitScope scope,
                              AudioUnitElement element, UInt32& outSize,
                              bool& outWritable) override {
        if (scope == kAudioUnitScope_Global) {
            switch (id) {
            case kAudioUnitProperty_BypassEffect:
                if (!hasBypass ()) return kAudioUnitErr_InvalidProperty;
                outSize = sizeof (UInt32);
                outWritable = true;
                return noErr;
            case kAudioUnitProperty_ParameterStringFromValue:
                outSize = sizeof (AudioUnitParameterStringFromValue);
                outWritable = false;
                return noErr;
            case kAudioUnitProperty_ParameterValueFromString:
                outSize = sizeof (AudioUnitParameterValueFromString);
                outWritable = false;
                return noErr;
            case kInstanceProperty:
                outSize = sizeof (BromEffectAU*);
                outWritable = false;
                return noErr;
#if defined(BROM_AU_EDITOR)
            case kAudioUnitProperty_CocoaUI:
                if (!hasEditor ()) return kAudioUnitErr_InvalidProperty;
                outSize = sizeof (AudioUnitCocoaViewInfo);
                outWritable = false;
                return noErr;
#endif
            default:
                break;
            }
        }
        return AUBase::GetPropertyInfo (id, scope, element, outSize, outWritable);
    }

    OSStatus GetProperty (AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element,
                          void* outData) override {
        if (scope == kAudioUnitScope_Global) {
            switch (id) {
            case kAudioUnitProperty_BypassEffect: {
                if (!hasBypass ()) return kAudioUnitErr_InvalidProperty;
                const UInt32 on = bypassOn () ? 1 : 0;
                std::memcpy (outData, &on, sizeof (on));
                return noErr;
            }
            case kAudioUnitProperty_ParameterStringFromValue: {
                auto* request = static_cast<AudioUnitParameterStringFromValue*> (outData);
                if (request->inParamID >= paramCount ()) return kAudioUnitErr_InvalidParameter;
                const auto value = request->inValue ? *request->inValue
                                                    : Globals ()->GetParameter (request->inParamID);
                char text[128] {};
                valueText (request->inParamID, toEngine (request->inParamID, value), text,
                           sizeof (text));
                request->outString = copyString (text);
                return noErr;
            }
            case kAudioUnitProperty_ParameterValueFromString: {
                auto* request = static_cast<AudioUnitParameterValueFromString*> (outData);
                if (request->inParamID >= paramCount () || !request->inString)
                    return kAudioUnitErr_InvalidParameter;
                char text[128] {};
                CFStringGetCString (request->inString, text, sizeof (text), kCFStringEncodingUTF8);
                double plain = 0.0;
                if (!brom_effect_param_parse (uiCore_, request->inParamID, text, &plain)) {
                    char* end = nullptr;
                    plain = std::strtod (text, &end);
                    if (end == text) return kAudioUnitErr_InvalidPropertyValue;
                }
                request->outValue = toHost (request->inParamID, plain);
                return noErr;
            }
            case kInstanceProperty: {
                BromEffectAU* self = this;
                std::memcpy (outData, &self, sizeof (self));
                return noErr;
            }
#if defined(BROM_AU_EDITOR)
            case kAudioUnitProperty_CocoaUI: {
                if (!hasEditor ()) return kAudioUnitErr_InvalidProperty;
                CFURLRef bundle = copyBundleURL ();
                if (!bundle) return kAudioUnitErr_InvalidProperty;
                AudioUnitCocoaViewInfo info {};
                info.mCocoaAUViewBundleLocation = bundle;
                info.mCocoaAUViewClass[0] =
                    CFSTR (BROM_AU_STRINGIFY (BROM_AU_VIEW_FACTORY_CLASS));
                CFRetain (info.mCocoaAUViewClass[0]);
                std::memcpy (outData, &info, sizeof (info));
                return noErr;
            }
#endif
            default:
                break;
            }
        }
        return AUBase::GetProperty (id, scope, element, outData);
    }

    OSStatus SetProperty (AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element,
                          const void* data, UInt32 size) override {
        if (scope == kAudioUnitScope_Global && id == kAudioUnitProperty_BypassEffect) {
            if (!hasBypass ()) return kAudioUnitErr_InvalidProperty;
            if (size < sizeof (UInt32)) return kAudioUnitErr_InvalidPropertyValue;
            UInt32 on = 0;
            std::memcpy (&on, data, sizeof (on));
            setBypass (on != 0);
            return noErr;
        }
        return AUBase::SetProperty (id, scope, element, data, size);
    }

    // ── MIDI ───────────────────────────────────────────────────────────────

    // MusicDeviceMIDIEvent, called before the render it belongs to. The event
    // is applied at its offset in that render, status byte (channel included)
    // and data passed through unchanged. MIDI 1.0 only: MIDIEventList (UMP,
    // MIDI 2.0) and SysEx keep AUBase's kAudio_UnimplementedError.
    OSStatus MIDIEvent (UInt32 status, UInt32 data1, UInt32 data2,
                        UInt32 offset) AUSDK_RTSAFE override {
        const UInt32 write = midiWrite_.load (std::memory_order_relaxed);
        const UInt32 read = midiRead_.load (std::memory_order_acquire);
        if (write - read >= kMidiQueue) return noErr;
        midiQueue_[write % kMidiQueue] = {offset, status & 0xFFu, data1 & 0x7Fu, data2 & 0x7Fu};
        midiWrite_.store (write + 1, std::memory_order_release);
        return noErr;
    }

    // ── Render ─────────────────────────────────────────────────────────────

    OSStatus Render (AudioUnitRenderActionFlags& ioActionFlags, const AudioTimeStamp& timeStamp,
                     UInt32 frames) AUSDK_RTSAFE override {
        if (!active_ || !core_) return kAudioUnitErr_Uninitialized;
        if (statePending_.load (std::memory_order_acquire)) {
            brom_effect_state_load (core_, pendingState_.data (), pendingState_.size ());
            statePending_.store (false, std::memory_order_release);
        }
        if (resetPending_.exchange (false, std::memory_order_acq_rel)) brom_effect_reset (core_);

        const AudioBufferList* mainList = nullptr;
        const AudioBufferList* sideList = nullptr;
        bool mainSilent = false;
        bool sideSilent = false;
        UInt32 sidePlanes = 0;
        if (frames > 0) {
            // A main input with nothing connected reads as silence; a failed
            // pull is a structural error. A sidechain that is unconnected or
            // fails to pull reads as 0 planes.
            auto& main = Input (0);
            if (main.IsActive ()) {
                AudioUnitRenderActionFlags flags = 0;
                const OSStatus pulled = main.PullInput (flags, timeStamp, 0, frames);
                if (pulled != noErr) return pulled;
                mainList = &main.GetBufferList ();
                mainSilent = (flags & kAudioUnitRenderAction_OutputIsSilence) != 0;
            }
            if (hasSidechain_) {
                auto& side = Input (1);
                if (side.IsActive ()) {
                    AudioUnitRenderActionFlags flags = 0;
                    if (side.PullInput (flags, timeStamp, 1, frames) == noErr) {
                        sideList = &side.GetBufferList ();
                        sideSilent = (flags & kAudioUnitRenderAction_OutputIsSilence) != 0;
                        sidePlanes = sideChannels_;
                    }
                }
            }
        }

        collectEvents (frames);
        UInt32 rendered = 0;
        while (true) {
            applyDue (rendered);
            if (rendered >= frames) break;
            UInt32 boundary = std::min (nextEventOffset (), frames);
            // Every point at or before `rendered` was just applied, so this
            // cannot stall; the guard only bounds a misbehaving list.
            if (boundary <= rendered) boundary = frames;
            if (!renderRange (mainList, mainSilent, sideList, sideSilent, sidePlanes, rendered,
                              boundary - rendered))
                return kAudioUnitErr_CannotDoInCurrentContext;
            rendered = boundary;
        }
        if (frames > 0) ioActionFlags &= ~kAudioUnitRenderAction_OutputIsSilence;
        blockRequery ();
        return noErr;
    }

    // ── What the editor and its view use (main thread) ─────────────────────

    void* editorCore () const { return uiCore_; }

    int32_t currentPreset () {
        if (!active_ && core_) {
            const int32_t current = brom_effect_current_program (core_);
            program_.store (current, std::memory_order_release);
            return current;
        }
        return program_.load (std::memory_order_acquire);
    }

    void editFromEditor (UInt32 id, double value, int phase) {
        if (id >= paramCount ()) return;
        AudioUnitParameter parameter {GetComponentInstance (), id, kAudioUnitScope_Global, 0};
        if (phase == 0) gesture (parameter, kAudioUnitEvent_BeginParameterChangeGesture);
        const auto host = toHost (id, value);
        uiMirror_[id] = host;
        brom_effect_param_set (uiCore_, id, value);
        AUParameterSet (nullptr, nullptr, &parameter, host, 0);
        if (phase == 2) gesture (parameter, kAudioUnitEvent_EndParameterChangeGesture);
    }

    bool loadPresetFromEditor (UInt32 index) {
        if (index >= presets_.size ()) return false;
        selectProgram (index);
        SetAFactoryPresetAsCurrent (presets_[index]);
        PropertyChanged (kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0);
        return true;
    }

    void addView (void* view) { views_.push_back (view); }
    void removeView (void* view) { views_.erase (std::remove (views_.begin (), views_.end (), view), views_.end ()); }

private:
    Float64 sampleRate () { return Output (0).GetStreamFormat ().mSampleRate; }

    UInt32 reportedTail () const {
        // AU has no infinite tail and no tail-change notification hosts act
        // on, so a tail that may grow while active is reported infinite, as
        // in the VST3 shell; the seconds come from UINT32_MAX samples.
        return variableTail () ? BROM_TAIL_INFINITE : tail_.load (std::memory_order_acquire);
    }

    static bool indexed (const brom_effect_param& param) {
        return param.steps > 1 &&
               std::fabs ((param.max_value - param.min_value) - (param.steps - 1)) < 1e-9 &&
               std::fabs (param.min_value - std::round (param.min_value)) < 1e-9;
    }

    // The value a host sees for an engineering value, and back: an indexed
    // parameter's whole step as it is, any other parameter's normalized
    // position through the engine's taper (brom_effect_param_normalize /
    // _denormalize, the VST3 shell's mapping).
    AudioUnitParameterValue toHost (UInt32 id, double plain) const {
        if (id < indexed_.size () && indexed_[id]) return static_cast<AudioUnitParameterValue> (plain);
        return static_cast<AudioUnitParameterValue> (brom_effect_param_normalize (id, plain));
    }

    double toEngine (UInt32 id, AudioUnitParameterValue host) const {
        if (id < indexed_.size () && indexed_[id]) return host;
        return brom_effect_param_denormalize (id, host);
    }

    void valueText (UInt32 id, double value, char* text, size_t capacity) {
        // The engine's own text first; plain number formatting is the
        // fallback, as in the CLAP and VST3 shells.
        if (!brom_effect_param_text (uiCore_, id, value, text, static_cast<uint32_t> (capacity)))
            std::snprintf (text, capacity, "%.2f", value);
    }

    bool hasBypass () const {
        const auto& info = effectInfo ();
        return info.bypass_param_id != BROM_NO_BYPASS && info.bypass_param_id < paramCount ();
    }

    bool bypassOn () {
        brom_effect_param param {};
        const UInt32 id = effectInfo ().bypass_param_id;
        if (!brom_effect_param_info (id, &param)) return false;
        return toEngine (id, Globals ()->GetParameter (id)) >= 0.5 * (param.min_value + param.max_value);
    }

    // kAudioUnitProperty_BypassEffect drives the effect's bypass parameter:
    // the shell never bypasses by itself, so the effect keeps its latency.
    void setBypass (bool on) {
        brom_effect_param param {};
        const UInt32 id = effectInfo ().bypass_param_id;
        if (!brom_effect_param_info (id, &param)) return;
        AudioUnitParameter parameter {GetComponentInstance (), id, kAudioUnitScope_Global, 0};
        AUParameterSet (nullptr, nullptr, &parameter,
                        toHost (id, on ? param.max_value : param.min_value), 0);
        lastBypass_ = on ? 1 : 0;
        PropertyChanged (kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0);
    }

    bool hasEditor () const {
        int32_t width = 0, height = 0;
        return brom_effect_editor_size (&width, &height) && width > 0 && height > 0;
    }

    // The .component directory holding this binary: three levels above
    // Contents/MacOS/<executable>.
    static CFURLRef copyBundleURL () {
        Dl_info where {};
        if (!dladdr (reinterpret_cast<const void*> (&BromEffectAUFactory), &where) ||
            !where.dli_fname)
            return nullptr;
        CFURLRef url = CFURLCreateFromFileSystemRepresentation (
            kCFAllocatorDefault, reinterpret_cast<const UInt8*> (where.dli_fname),
            static_cast<CFIndex> (std::strlen (where.dli_fname)), false);
        for (int level = 0; level < 3 && url; ++level) {
            CFURLRef parent = CFURLCreateCopyDeletingLastPathComponent (kCFAllocatorDefault, url);
            CFRelease (url);
            url = parent;
        }
        return url;
    }

    void gesture (const AudioUnitParameter& parameter, AudioUnitEventType type) {
        AudioUnitEvent event {};
        event.mEventType = type;
        event.mArgument.mParameter = parameter;
        AUEventListenerNotify (nullptr, nullptr, &event);
    }

    void notifyAllParameters () {
        AudioUnitParameter any {GetComponentInstance (), kAUParameterListener_AnyParameter,
                                kAudioUnitScope_Global, 0};
        AUParameterListenerNotify (nullptr, nullptr, &any);
    }

    // Apply an element value to the audio instance directly; only while
    // inactive, when no render can run.
    void applyElementValue (UInt32 id) {
        const auto value = Globals ()->GetParameter (id);
        brom_effect_param_set (core_, id, toEngine (id, value));
        lastApplied_[id] = value;
    }

    // A factory program chosen on the main thread (host preset menu or the
    // editor): the private instance takes it, every parameter shows the
    // preset's value, and the audio instance applies the same program at the
    // start of its next render (now, when inactive). An effect whose program
    // callback refuses still gets the bank's values, as in the VST3 shell.
    void selectProgram (UInt32 index) {
        const UInt32 count = paramCount ();
        std::vector<double> values (count);
        const bool haveValues = count == 0 ||
            brom_effect_factory_values (index, values.data (), count);
        const bool applied = brom_effect_program (uiCore_, index);
        if (haveValues) {
            for (UInt32 id = 0; id < count; ++id) {
                if (!applied) brom_effect_param_set (uiCore_, id, values[id]);
                const auto value = toHost (id, values[id]);
                Globals ()->SetParameter (id, value);
                uiMirror_[id] = value;
            }
        }
        if (active_) {
            pendingProgram_.store (static_cast<int32_t> (index), std::memory_order_release);
        } else {
            if (!brom_effect_program (core_, index) && haveValues)
                for (UInt32 id = 0; id < count; ++id) brom_effect_param_set (core_, id, values[id]);
            for (UInt32 id = 0; id < count; ++id) lastApplied_[id] = Globals ()->GetParameter (id);
            program_.store (brom_effect_current_program (core_), std::memory_order_release);
        }
        notifyAllParameters ();
    }

    // Inside Initialize, after activate and reset: the applied latency, the
    // tail and the current program, with no change notification.
    void activateQuery () {
        const uint32_t reported = brom_effect_latency (core_);
        latency_.store (reported & ~BROM_LATENCY_PENDING, std::memory_order_release);
        tail_.store (brom_effect_tail (core_), std::memory_order_release);
        program_.store (brom_effect_current_program (core_), std::memory_order_release);
        restartWanted_.store (false, std::memory_order_release);
        restartLatched_ = false;
    }

    // At the end of every render, including zero-length ones. The new latency
    // is published at once, because an AU host reads kAudioUnitProperty_Latency
    // when told and does not reinitialize; a change or a pending value asks the
    // main thread for one Latency notification, latched until Initialize.
    void blockRequery () {
        tail_.store (brom_effect_tail (core_), std::memory_order_release);
        program_.store (brom_effect_current_program (core_), std::memory_order_release);
        const uint32_t reported = brom_effect_latency (core_);
        const uint32_t applied = reported & ~BROM_LATENCY_PENDING;
        const bool pending = (reported & BROM_LATENCY_PENDING) != 0;
        const uint32_t previous = latency_.exchange (applied, std::memory_order_acq_rel);
        if (!restartLatched_ && (pending || applied != previous)) {
            restartLatched_ = true;
            restartWanted_.store (true, std::memory_order_release);
        }
    }

    // ── Event timeline ─────────────────────────────────────────────────────
    //
    // A render's events: scheduled parameter points (AudioUnitScheduleParameters,
    // at their buffer offsets), MIDI (at theirs), and at offset 0 every
    // parameter the host set without scheduling plus a preset chosen on the
    // main thread. Within one offset, parameters apply first, then MIDI, then
    // program changes: the CLAP shell's order.

    void collectEvents (UInt32 frames) {
        const UInt32 count = paramCount ();
        std::fill (scheduledIds_.begin (), scheduledIds_.end (), 0);
        scheduledCount_ = 0;
        for (const auto& event : GetParamEventList ()) {
            if (event.scope != kAudioUnitScope_Global || event.element != 0 ||
                event.parameter >= count)
                continue;
            scheduledIds_[event.parameter] = 1;
            if (event.eventType == kParameterEvent_Immediate) {
                addScheduled (std::min (event.eventValues.immediate.bufferOffset, frames),
                              event.parameter, event.eventValues.immediate.value, false);
            } else {
                // A ramp applies its start value at its start and its end
                // value at its end when that falls in this render; the effect
                // smooths in between, if it smooths at all.
                const auto& ramp = event.eventValues.ramp;
                const SInt64 start = std::max<SInt64> (ramp.startBufferOffset, 0);
                const SInt64 end = SInt64 (ramp.startBufferOffset) + SInt64 (ramp.durationInFrames);
                addScheduled (static_cast<UInt32> (std::min<SInt64> (start, frames)),
                              event.parameter, ramp.startValue, true);
                if (end <= SInt64 (frames))
                    addScheduled (static_cast<UInt32> (std::max<SInt64> (end, 0)), event.parameter,
                                  ramp.endValue, true);
            }
        }
        std::stable_sort (scheduled_.begin (), scheduled_.begin () + scheduledCount_,
                          [] (const ScheduledPoint& a, const ScheduledPoint& b) {
                              return a.offset < b.offset;
                          });
        scheduledNext_ = 0;

        midiCount_ = 0;
        const UInt32 write = midiWrite_.load (std::memory_order_acquire);
        UInt32 read = midiRead_.load (std::memory_order_relaxed);
        for (; read != write; ++read) {
            MidiEvent event = midiQueue_[read % kMidiQueue];
            event.offset = std::min (event.offset, frames);
            midi_[midiCount_++] = event;
        }
        midiRead_.store (read, std::memory_order_release);
        std::stable_sort (midi_, midi_ + midiCount_, [] (const MidiEvent& a, const MidiEvent& b) {
            return a.offset < b.offset;
        });
        midiNext_[0] = midiNext_[1] = 0;
        startPending_ = true;
    }

    void addScheduled (UInt32 offset, AudioUnitParameterID id, AudioUnitParameterValue value,
                       bool writeElement) {
        if (scheduledCount_ < kMaxScheduled)
            scheduled_[scheduledCount_++] = {offset, id, value, writeElement};
    }

    static bool isProgram (const MidiEvent& event) { return (event.status & 0xF0u) == 0xC0u; }

    UInt32 nextMidi (int kind) {
        UInt32& next = midiNext_[kind];
        while (next < midiCount_ && isProgram (midi_[next]) != (kind == 1)) ++next;
        return next;
    }

    UInt32 nextEventOffset () {
        UInt32 offset = UINT32_MAX;
        if (scheduledNext_ < scheduledCount_) offset = scheduled_[scheduledNext_].offset;
        for (int kind = 0; kind < 2; ++kind) {
            const UInt32 next = nextMidi (kind);
            if (next < midiCount_) offset = std::min (offset, midi_[next].offset);
        }
        return offset;
    }

    void applyDue (UInt32 rendered) {
        // Parameters: at the start, every unscheduled value the host set
        // since the last render, then the scheduled points due.
        if (startPending_) {
            for (UInt32 id = 0; id < paramCount (); ++id) {
                if (scheduledIds_[id]) continue;
                const auto value = Globals ()->GetParameterRT (id);
                if (value != lastApplied_[id]) {
                    brom_effect_param_set (core_, id, toEngine (id, value));
                    lastApplied_[id] = value;
                }
            }
        }
        while (scheduledNext_ < scheduledCount_ && scheduled_[scheduledNext_].offset <= rendered) {
            const auto& point = scheduled_[scheduledNext_++];
            brom_effect_param_set (core_, point.id, toEngine (point.id, point.value));
            lastApplied_[point.id] = point.value;
            if (point.writeElement) Globals ()->SetParameterRT (point.id, point.value);
        }
        // MIDI other than program changes.
        for (UInt32 next = nextMidi (0); next < midiCount_ && midi_[next].offset <= rendered;
             next = nextMidi (0)) {
            const auto& event = midi_[next];
            brom_effect_midi (core_, event.status, event.data1, event.data2);
            ++midiNext_[0];
        }
        // Program changes: a preset chosen on the main thread, then MIDI.
        if (startPending_) {
            const int32_t program = pendingProgram_.exchange (-1, std::memory_order_acq_rel);
            if (program >= 0) brom_effect_program (core_, static_cast<uint32_t> (program));
            startPending_ = false;
        }
        for (UInt32 next = nextMidi (1); next < midiCount_ && midi_[next].offset <= rendered;
             next = nextMidi (1)) {
            brom_effect_program (core_, midi_[next].data1);
            ++midiNext_[1];
        }
    }

    bool renderRange (const AudioBufferList* mainList, bool mainSilent,
                      const AudioBufferList* sideList, bool sideSilent, UInt32 sidePlanes,
                      UInt32 start, UInt32 frames) {
        auto& output = Output (0).GetBufferList ();
        if (output.mNumberBuffers < 2) return false;
        const bool effect64 = effectInfo ().sample_format == BROM_SAMPLE_F64;
        for (UInt32 offset = start; offset < start + frames; offset += kCapacity) {
            const UInt32 chunk = std::min (kCapacity, start + frames - offset);
            if (effect64)
                renderChunk<double> (mainList, mainSilent, sideList, sideSilent, sidePlanes,
                                     output, offset, chunk, in64_, side64_, out64_);
            else
                renderChunk<float> (mainList, mainSilent, sideList, sideSilent, sidePlanes,
                                    output, offset, chunk, in32_, side32_, out32_);
        }
        return true;
    }

    // Every plane is read into scratch before the effect runs and the output is
    // written after, so an in-place host buffer is safe in every width
    // combination. A refused chunk is written as silence.
    template <typename T, typename Block>
    void renderChunk (const AudioBufferList* mainList, bool mainSilent,
                      const AudioBufferList* sideList, bool sideSilent, UInt32 sidePlanes,
                      AudioBufferList& output, UInt32 offset, UInt32 frames, Block& in, Block& side,
                      Block& out) {
        readElement<T> (in, mainList, mainList ? mainChannels_ : 0, mainWide_, mainSilent, offset,
                        frames);
        readElement<T> (side, sideList, sidePlanes, sideWide_, sideSilent, offset, frames);
        if (!processChunk (core_, in, side, out, static_cast<int32_t> (mainChannels_),
                           static_cast<int32_t> (sidePlanes), static_cast<int32_t> (frames))) {
            std::fill (out.left, out.left + frames, T (0));
            std::fill (out.right, out.right + frames, T (0));
        }
        writePlane<T> (out.left, output, 0, outWide_, offset, frames);
        writePlane<T> (out.right, output, 1, outWide_, offset, frames);
    }

    // ── Main-thread housekeeping ───────────────────────────────────────────
    //
    // Runs every 33 ms whether or not an editor is open, like the VST3
    // controller's timer: it is how a change the render thread saw reaches
    // the host, since property notifications are not render-thread safe.

    static void timerFired (CFRunLoopTimerRef, void* info) {
        static_cast<BromEffectAU*> (info)->housekeeping ();
    }

    void housekeeping () {
        if (restartWanted_.exchange (false, std::memory_order_acq_rel))
            PropertyChanged (kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0);
        const UInt32 tail = reportedTail ();
        if (tail != notifiedTail_) {
            notifiedTail_ = tail;
            PropertyChanged (kAudioUnitProperty_TailTime, kAudioUnitScope_Global, 0);
        }
        // A program the render thread applied (MIDI program change or a preset)
        // becomes the current preset, and the parameters the host shows follow
        // the engine's. CLAP's params.get_value reads the engine on the main
        // thread the same way.
        const int32_t program = program_.load (std::memory_order_acquire);
        if (program != notifiedProgram_) {
            notifiedProgram_ = program;
            if (program >= 0 && static_cast<size_t> (program) < presets_.size ()) {
                SetAFactoryPresetAsCurrent (presets_[static_cast<size_t> (program)]);
                PropertyChanged (kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0);
                if (active_ && pendingProgram_.load (std::memory_order_acquire) < 0) {
                    for (UInt32 id = 0; id < paramCount (); ++id)
                        Globals ()->SetParameter (id, toHost (id, brom_effect_param_get (core_, id)));
                    notifyAllParameters ();
                }
            }
        }
        // The private instance follows the parameter values the host sees.
        for (UInt32 id = 0; id < paramCount (); ++id) {
            const auto value = Globals ()->GetParameter (id);
            if (value != uiMirror_[id]) {
                uiMirror_[id] = value;
                brom_effect_param_set (uiCore_, id, toEngine (id, value));
            }
        }
        if (hasBypass ()) {
            const int on = bypassOn () ? 1 : 0;
            if (on != lastBypass_) {
                lastBypass_ = on;
                PropertyChanged (kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0);
            }
        }
    }

    void closeViews ();

    void* core_ {nullptr};
    void* uiCore_ {nullptr};
    bool hasSidechain_ {false};
    bool active_ {false};
    bool restartLatched_ {false};
    bool startPending_ {false};
    bool mainWide_ {false}, sideWide_ {false}, outWide_ {false};
    UInt32 mainChannels_ {2};
    UInt32 sideChannels_ {0};
    AUChannelInfo channelInfo_[2] {};

    // Host values (normalized, or steps for indexed parameters).
    std::vector<AudioUnitParameterValue> lastApplied_;
    std::vector<AudioUnitParameterValue> uiMirror_;
    // Whether a parameter is indexed (host value = engineering whole step)
    // rather than normalized; fixed at construction.
    std::vector<UInt8> indexed_;
    std::vector<UInt8> scheduledIds_;
    std::vector<ScheduledPoint> scheduled_;
    UInt32 scheduledCount_ {0};
    UInt32 scheduledNext_ {0};

    MidiEvent midiQueue_[kMidiQueue] {};
    std::atomic<UInt32> midiWrite_ {0};
    std::atomic<UInt32> midiRead_ {0};
    MidiEvent midi_[kMidiQueue] {};
    UInt32 midiCount_ {0};
    UInt32 midiNext_[2] {0, 0};

    std::vector<UInt8> pendingState_;
    std::atomic<bool> statePending_ {false};
    std::atomic<bool> resetPending_ {false};
    std::atomic<int32_t> pendingProgram_ {-1};

    // The caches: written by the render thread (and Initialize), read by the
    // host-facing getters and the editor.
    std::atomic<uint32_t> latency_ {0};
    std::atomic<uint32_t> tail_ {0};
    std::atomic<int32_t> program_ {-1};
    std::atomic<bool> restartWanted_ {false};

    UInt32 notifiedTail_ {0};
    int32_t notifiedProgram_ {-1};
    int lastBypass_ {-1};
    std::vector<AUPreset> presets_;
    std::vector<void*> views_;
    CFRunLoopTimerRef timer_ {nullptr};

    // One input, sidechain and output block per format; an effect uses one set.
    brom_audio_f32 in32_ {}, side32_ {}, out32_ {};
    brom_audio_f64 in64_ {}, side64_ {}, out64_ {};
};

} // namespace bromau

// ── Cocoa view (editor hook) ─────────────────────────────────────────────

#if defined(BROM_AU_EDITOR)

// The product editor in an NSView. A main-thread timer pumps it every 33 ms
// while the view is in a window and tells it when the committed program
// changes, as the VST3 shell's view does. The editor is attached the first
// time the view joins a window and detached when the view or the Audio Unit
// goes away, whichever is first.
@interface BROM_AU_VIEW_CLASS : NSView {
    bromau::BromEffectAU* _au;
    void* _editor;
    NSTimer* _timer;
    int32_t _preset;
    BOOL _attached;
}
- (instancetype)initWithAU:(bromau::BromEffectAU*)au width:(int32_t)width height:(int32_t)height;
- (void)auWillClose;
@end

@implementation BROM_AU_VIEW_CLASS

- (instancetype)initWithAU:(bromau::BromEffectAU*)au width:(int32_t)width height:(int32_t)height {
    self = [super initWithFrame:NSMakeRect (0, 0, width, height)];
    if (!self) return nil;
    _au = au;
    _preset = -2;
    _editor = brom_au_editor_create (au, au->editorCore ());
    if (!_editor) {
        [self release];
        return nil;
    }
    au->addView (self);
    return self;
}

- (BOOL)isFlipped {
    return YES;
}

- (void)stopTimer {
    [_timer invalidate];
    [_timer release];
    _timer = nil;
}

- (void)releaseEditor {
    [self stopTimer];
    if (_editor) {
        if (_attached) brom_au_editor_detach (_editor);
        brom_au_editor_destroy (_editor);
        _editor = nullptr;
    }
    _attached = NO;
}

- (void)auWillClose {
    [self releaseEditor];
    _au = nullptr;
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (!self.window) {
        [self stopTimer];
        return;
    }
    if (!_editor || !_au) return;
    if (!_attached) {
        const NSSize size = self.frame.size;
        _attached = brom_au_editor_attach (_editor, self, static_cast<int> (size.width),
                                           static_cast<int> (size.height)) != 0;
        if (!_attached) return;
        brom_au_editor_tick (_editor);
    }
    if (!_timer) {
        _timer = [[NSTimer timerWithTimeInterval:bromau::kTimerSeconds
                                          target:self
                                        selector:@selector (tick:)
                                        userInfo:nil
                                         repeats:YES] retain];
        [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
    }
}

- (void)tick:(NSTimer*)timer {
    if (!_editor || !_au || !_attached) return;
    brom_au_editor_tick (_editor);
    const int32_t current = _au->currentPreset ();
    if (current != _preset) {
        _preset = current;
        brom_au_editor_preset_changed (_editor, current);
    }
}

- (void)dealloc {
    [self releaseEditor];
    if (_au) _au->removeView (self);
    [super dealloc];
}

@end

// The view factory the host instantiates from kAudioUnitProperty_CocoaUI. It
// finds the shell instance through the shell's own property.
@interface BROM_AU_VIEW_FACTORY_CLASS : NSObject <AUCocoaUIBase>
@end

@implementation BROM_AU_VIEW_FACTORY_CLASS

- (unsigned)interfaceVersion {
    return 0;
}

- (NSString*)description {
    return @POKUSTON_PLUGIN_NAME " Editor";
}

- (NSView*)uiViewForAudioUnit:(AudioUnit)unit withSize:(NSSize)size {
    (void)size;
    bromau::BromEffectAU* au = nullptr;
    UInt32 bytes = sizeof (au);
    if (AudioUnitGetProperty (unit, bromau::kInstanceProperty, kAudioUnitScope_Global, 0, &au,
                              &bytes) != noErr ||
        !au)
        return nil;
    int32_t width = 0, height = 0;
    if (!brom_effect_editor_size (&width, &height) || width <= 0 || height <= 0) return nil;
    return [[[BROM_AU_VIEW_CLASS alloc] initWithAU:au width:width height:height] autorelease];
}

@end

void bromau::BromEffectAU::closeViews () {
    const auto views = views_;
    views_.clear ();
    for (void* view : views) [(BROM_AU_VIEW_CLASS*)view auWillClose];
}

using bromau::BromEffectAU;

extern "C" void brom_au_host_edit_param (void* host, uint32_t id, double value, int phase) {
    if (host) static_cast<BromEffectAU*> (host)->editFromEditor (id, value, phase);
}

extern "C" uint32_t brom_au_host_preset_count (void*) { return brom_effect_factory_count (); }

extern "C" int32_t brom_au_host_current_preset (void* host) {
    return host ? static_cast<BromEffectAU*> (host)->currentPreset () : -1;
}

extern "C" int brom_au_host_preset_name (void*, uint32_t index, char* out, uint32_t capacity) {
    if (!out || capacity == 0) return 0;
    out[0] = '\0';
    return brom_effect_factory_name (index, out, capacity);
}

extern "C" int brom_au_host_load_preset (void* host, uint32_t index) {
    return host && static_cast<BromEffectAU*> (host)->loadPresetFromEditor (index) ? 1 : 0;
}

#else

void bromau::BromEffectAU::closeViews () { views_.clear (); }

#endif

// The component's factory function, named in Info.plist. The table is
// validated on every call, before an instance exists: a broken one makes
// instantiation fail, so the host never gets a plug-in, as the CLAP entry's
// init and the VST3 GetPluginFactory refuse. AUMIDIEffectFactory dispatches
// the MusicDevice MIDI selectors for both component types: an aumf is where a
// host sends MIDI, and an aufx that a host sends MIDI to anyway still gets it.
extern "C" AUSDK_EXPORT void* BromEffectAUFactory (const AudioComponentDescription* description) {
    if (!brom_effect_init ()) return nullptr;
    return ausdk::AUMIDIEffectFactory<bromau::BromEffectAU>::Factory (description);
}
