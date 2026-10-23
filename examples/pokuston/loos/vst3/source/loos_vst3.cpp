#include "examples/pokuston/loos/loos-editor.h"
#include "examples/pokuston/loos/editor.h"
#include "examples/pokuston/loos/loos.h"
// This translation unit carries the program-rule flavor as a string (see program_map.h).
#define BROM_PROGRAM_EMIT_FLAVOR_TAG
#include "packages/recoil-brom/brom/plugin/preset/program_map.h"
#include "packages/recoil-brom/brom/plugin/vst3/process_timeline.hpp"
#include "packages/recoil-brom/brom/plugin/vst3/state_stream.hpp"

#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/common/pluginview.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "base/source/timer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace Steinberg::PokustonLoos {

static const FUID kProcessorUID (0xC916C5DB, 0xE3934C7C, 0x8B899194, 0xC1E07C03);
static const FUID kControllerUID (0x6CF6E20A, 0x7CD04CE1, 0x81B8E58E, 0xAC734D11);
constexpr int32 kEditorWidth = 760;
// Must equal EDITOR-WIDTH / EDITOR-HEIGHT in loos/editor-size.rcl: the editor
// refuses any other size, so a stale value here is a silent blank window.
constexpr int32 kEditorHeight = 576;
// The program parameter sits one past the schema, so adding a synthesis
// parameter moves it rather than colliding with it.
constexpr Vst::ParamID kProgramID = LOOS_PARAM_COUNT;
constexpr uint32 kVstParameterCount = LOOS_PARAM_COUNT + 1;
constexpr Vst::ProgramListID kProgramListID = kProgramID;

using PresetValues = std::array<double, LOOS_PARAM_COUNT>;
using PresetBank = std::vector<PresetValues>;

static void fromAscii (const char* source, Vst::String128 destination) {
    UString (destination, 128).fromAscii (source ? source : "");
}

static bool loadFactoryBank (PresetBank& bank) {
    const uint32 count = loos_get_factory_preset_count ();
    if (count == 0) return false;
    bank.resize (count);
    for (uint32 index = 0; index < bank.size (); ++index) {
        if (!loos_factory_preset_values (
                index, bank[index].data (), LOOS_PARAM_COUNT)) return false;
    }
    return true;
}

// The program rule is Brom's shared one (program_map.h), so the VST3 build and the AU
// build of this product cannot drift from the bridge. A non-finite host value selects
// the first program, as it always did.
static int32 programIndex (double normalized, std::size_t count) {
    if (count == 0) return 0;
    return static_cast<int32> (
        brom_program_index (static_cast<uint32_t> (count), brom_program_value_clamped (normalized)));
}

static double programNormalized (int32 index, std::size_t count) {
    return brom_program_normalized (static_cast<uint32_t> (count),
                                    static_cast<uint32_t> (std::max<int32> (index, 0)));
}

class Controller;

class EditorView final : public CPluginView, public ITimerCallback {
public:
    explicit EditorView (Controller& controller);
    ~EditorView () SMTG_OVERRIDE;

    void attachedToParent () SMTG_OVERRIDE;
    void removedFromParent () SMTG_OVERRIDE;
    tresult PLUGIN_API isPlatformTypeSupported (FIDString type) SMTG_OVERRIDE;
    void onTimer (Timer* timer) SMTG_OVERRIDE;

private:
    Controller& controller_;
    loos_editor* editor_ {nullptr};
    Timer* timer_ {nullptr};
};

class Controller final : public Vst::EditControllerEx1 {
public:
    Controller () : control_ (loos_create ()), bankReady_ (loadFactoryBank (bank_)) {}
    ~Controller () SMTG_OVERRIDE { loos_destroy (control_); }

    static FUnknown* createInstance (void*) {
        return static_cast<Vst::IEditController*> (new Controller);
    }

    tresult PLUGIN_API initialize (FUnknown* context) SMTG_OVERRIDE {
        const auto result = EditControllerEx1::initialize (context);
        if (result != kResultOk || !control_ || !bankReady_) return kResultFalse;

        addUnit (new Vst::Unit (
            STR16 ("Loos"), Vst::kRootUnitId, Vst::kNoParentUnitId, kProgramListID));
        // Named from the engine's own schema rather than a list kept here.
        // The list that used to be here fell one parameter behind the engine
        // and silently made Shape unautomatable in VST3 and AUv2.
        for (uint32 id = 0; id < LOOS_PARAM_COUNT; ++id) {
            Vst::String128 title {};
            fromAscii (loos_param_name (id), title);
            const auto* units = id == LOOS_PARAM_CUTOFF ? STR16 ("Hz")
                               : id == LOOS_PARAM_LFO_RATE ? STR16 ("Hz")
                               : id == LOOS_PARAM_ENV_AMOUNT ? STR16 ("oct")
                               : (id == LOOS_PARAM_ATTACK || id == LOOS_PARAM_DECAY ||
                                  id == LOOS_PARAM_RELEASE) ? STR16 ("s") : nullptr;
            parameters.addParameter (
                title, units, 0,
                loos_parameter_to_normalized (id, loos_parameter_default (id)),
                Vst::ParameterInfo::kCanAutomate, id);
        }

        auto* programs = new Vst::ProgramList (
            STR16 ("Loos Factory Presets"), kProgramListID, Vst::kRootUnitId);
        for (uint32 index = 0; index < bank_.size (); ++index) {
            char name[128] {};
            if (!loos_factory_preset_name (index, name, sizeof (name))) return kResultFalse;
            Vst::String128 title {};
            fromAscii (name, title);
            programs->addProgram (title);
        }
        addProgramList (programs);
        auto* programParameter = programs->getParameter ();
        programParameter->getInfo ().flags &= ~Vst::ParameterInfo::kCanAutomate;
        parameters.addParameter (programParameter);
        return kResultOk;
    }

    tresult PLUGIN_API setComponentState (IBStream* state) SMTG_OVERRIDE {
        if (!PokustonVst3Support::readState<LOOS_STATE_SIZE> (
                state, control_, loos_state_load)) return kResultFalse;
        for (uint32_t parameter = 0; parameter < LOOS_PARAM_COUNT; ++parameter) {
            const double value = loos_get_parameter (control_, parameter);
            EditController::setParamNormalized (parameter, loos_parameter_to_normalized (parameter, value));
        }
        selectedPreset_ = matchingPreset ();
        if (selectedPreset_ >= 0) {
            EditController::setParamNormalized (
                kProgramID, programNormalized (selectedPreset_, bank_.size ()));
        }
        return kResultOk;
    }

    tresult PLUGIN_API setParamNormalized (Vst::ParamID tag, Vst::ParamValue value) SMTG_OVERRIDE {
        const auto result = EditControllerEx1::setParamNormalized (tag, value);
        if (result != kResultOk) return result;
        if (tag < LOOS_PARAM_COUNT) {
            loos_set_parameter (control_, tag, loos_parameter_from_normalized (tag, value));
            // Turning any control means the patch is no longer the preset it
            // came from, and the caption stops claiming otherwise.
            if (selectedPreset_ >= 0 &&
                !sameValue (loos_get_parameter (control_, tag), bank_[selectedPreset_][tag])) {
                selectedPreset_ = -1;
            }
        } else if (tag == kProgramID && !selectingProgram_) {
            selectProgram (programIndex (value, bank_.size ()), false);
        }
        return result;
    }

    Vst::ParamValue PLUGIN_API normalizedParamToPlain (
        Vst::ParamID tag, Vst::ParamValue normalized) SMTG_OVERRIDE {
        return tag < LOOS_PARAM_COUNT
                   ? loos_parameter_from_normalized (tag, normalized)
                   : EditControllerEx1::normalizedParamToPlain (tag, normalized);
    }

    Vst::ParamValue PLUGIN_API plainParamToNormalized (
        Vst::ParamID tag, Vst::ParamValue plain) SMTG_OVERRIDE {
        return tag < LOOS_PARAM_COUNT
                   ? loos_parameter_to_normalized (tag, plain)
                   : EditControllerEx1::plainParamToNormalized (tag, plain);
    }

    IPlugView* PLUGIN_API createView (FIDString name) SMTG_OVERRIDE {
        if (name && std::strcmp (name, Vst::ViewType::kEditor) == 0) return new EditorView (*this);
        return nullptr;
    }

    void editFromCanvas (uint32 parameter, double value, int phase) {
        const auto normalized = loos_parameter_to_normalized (parameter, value);
        if (phase == 0) beginEdit (parameter);
        setParamNormalized (parameter, normalized);
        if (phase == 1) performEdit (parameter, normalized);
        if (phase == 2) endEdit (parameter);
    }

    bool selectFromCanvas (uint32 index) { return selectProgram (index, true); }
    void* control () const { return control_; }
    int32 selectedPreset () const { return selectedPreset_; }
    uint32 presetCount () const { return static_cast<uint32> (bank_.size ()); }

    const char* presetName (uint32 index) {
        presetName_[0] = '\0';
        loos_factory_preset_name (
            index, presetName_.data (), static_cast<uint32_t> (presetName_.size ()));
        return presetName_.data ();
    }

private:
    static bool sameValue (double left, double right) {
        return std::abs (left - right) <= std::max (1.0, std::abs (right)) * 1e-9;
    }

    int32 matchingPreset () const {
        for (int32 preset = 0; preset < static_cast<int32> (bank_.size ()); ++preset) {
            bool match = true;
            for (uint32 id = 0; id < LOOS_PARAM_COUNT && match; ++id)
                match = sameValue (loos_get_parameter (control_, id), bank_[preset][id]);
            if (match) return preset;
        }
        return -1;
    }

    bool selectProgram (int32 index, bool notifyHost) {
        if (!bankReady_ || index < 0 || index >= static_cast<int32> (bank_.size ())) return false;
        selectingProgram_ = true;
        for (uint32 id = 0; id < LOOS_PARAM_COUNT; ++id) {
            loos_set_parameter (control_, id, bank_[index][id]);
            EditController::setParamNormalized (
                id, loos_parameter_to_normalized (id, bank_[index][id]));
        }
        selectedPreset_ = index;
        const double normalized = programNormalized (index, bank_.size ());
        EditController::setParamNormalized (kProgramID, normalized);
        selectingProgram_ = false;
        if (notifyHost) {
            beginEdit (kProgramID);
            performEdit (kProgramID, normalized);
            endEdit (kProgramID);
        }
        if (componentHandler) componentHandler->restartComponent (Vst::kParamValuesChanged);
        return true;
    }

    void* control_ {nullptr};
    PresetBank bank_ {};
    bool bankReady_ {false};
    bool selectingProgram_ {false};
    int32 selectedPreset_ {-1};
    std::array<char, 256> presetName_ {};
};

static void canvas_edit (void* user, uint32_t parameter, double value, int phase) {
    static_cast<Controller*> (user)->editFromCanvas (parameter, value, phase);
}

EditorView::EditorView (Controller& controller)
: CPluginView (new ViewRect {0, 0, kEditorWidth, kEditorHeight}), controller_ (controller) {
    editor_ = loos_editor_create (controller_.control (), canvas_edit, &controller_);
}

EditorView::~EditorView () {
    removedFromParent ();
    loos_editor_destroy (editor_);
}

tresult PLUGIN_API EditorView::isPlatformTypeSupported (FIDString type) {
#if defined(__APPLE__)
    return type && std::strcmp (type, kPlatformTypeNSView) == 0 ? kResultTrue : kResultFalse;
#elif defined(__linux__)
    return type && std::strcmp (type, kPlatformTypeX11EmbedWindowID) == 0 ? kResultTrue : kResultFalse;
#else
    return kResultFalse;
#endif
}

void EditorView::attachedToParent () {
    if (!editor_ || timer_) return;
    if (!loos_editor_attach (editor_, systemWindow, kEditorWidth, kEditorHeight)) return;
    loos_editor_tick (editor_);
    timer_ = Timer::create (this, 33);
}

void EditorView::removedFromParent () {
    if (timer_) {
        timer_->stop ();
        timer_->release ();
        timer_ = nullptr;
    }
    loos_editor_detach (editor_);
}

void EditorView::onTimer (Timer* timer) {
    if (timer == timer_) loos_editor_tick (editor_);
}

class Processor final : public Vst::AudioEffect {
public:
    Processor () : core_ (loos_create ()), bankReady_ (loadFactoryBank (bank_)) {
        setControllerClass (kControllerUID);
        clearNotes ();
    }
    ~Processor () SMTG_OVERRIDE { loos_destroy (core_); }

    static FUnknown* createInstance (void*) {
        return static_cast<Vst::IAudioProcessor*> (new Processor);
    }

    tresult PLUGIN_API initialize (FUnknown* context) SMTG_OVERRIDE {
        const auto result = AudioEffect::initialize (context);
        if (result != kResultOk || !core_ || !bankReady_) return kResultFalse;
        addAudioOutput (STR16 ("Stereo Out"), Vst::SpeakerArr::kStereo);
        addEventInput (STR16 ("Note Input"), 1);
        return kResultOk;
    }

    tresult PLUGIN_API setActive (TBool state) SMTG_OVERRIDE {
        const auto result = AudioEffect::setActive (state);
        if (result == kResultOk && state) resetSynth ();
        return result;
    }

    tresult PLUGIN_API setupProcessing (Vst::ProcessSetup& setup) SMTG_OVERRIDE {
        const auto result = AudioEffect::setupProcessing (setup);
        if (result == kResultOk) sample_rate_ = setup.sampleRate;
        return result;
    }

    tresult PLUGIN_API canProcessSampleSize (int32 sample_size) SMTG_OVERRIDE {
        return sample_size == Vst::kSample32 ? kResultTrue : kResultFalse;
    }

    tresult PLUGIN_API setState (IBStream* state) SMTG_OVERRIDE {
        return PokustonVst3Support::readState<LOOS_STATE_SIZE> (
                   state, core_, loos_state_load) ? kResultOk : kResultFalse;
    }

    tresult PLUGIN_API getState (IBStream* state) SMTG_OVERRIDE {
        return PokustonVst3Support::writeState<LOOS_STATE_SIZE> (
                   state, core_, loos_state_save) ? kResultOk : kResultFalse;
    }

    tresult PLUGIN_API process (Vst::ProcessData& data) SMTG_OVERRIDE {
        if (data.symbolicSampleSize != Vst::kSample32 || data.numOutputs < 1 || !data.outputs) return kResultOk;
        return PokustonVst3Support::processTimeline<kVstParameterCount> (
            data, loos_output_capacity (),
            [this] (uint32 id, Vst::ParamValue normalized) {
                if (id < LOOS_PARAM_COUNT) {
                    loos_set_parameter (
                        core_, id, loos_parameter_from_normalized (id, normalized));
                } else if (id == kProgramID) {
                    applyProgram (programIndex (normalized, bank_.size ()));
                }
            },
            [this] (const Vst::Event& event) { dispatchEvent (event); },
            [this, &data] (int32 offset, int32 frames) {
                loos_process_block (core_, frames);
                copyOutput (data.outputs[0], offset, frames);
            });
    }

private:
    void applyProgram (int32 index) {
        if (index < 0 || index >= static_cast<int32> (bank_.size ())) return;
        for (uint32 id = 0; id < LOOS_PARAM_COUNT; ++id)
            loos_set_parameter (core_, id, bank_[index][id]);
        loos_reset (core_);
        clearNotes ();
    }

    void clearNotes () {
        voiceNoteIds_.fill (-1);
        voicePitches_.fill (-1);
    }

    void resetSynth () {
        loos_activate (core_, sample_rate_);
        clearNotes ();
        next_voice_ = 0;
    }

    void releaseNote (int32 noteId, int32 pitch) {
        for (int32 voice = 0; voice < LOOS_MAX_VOICES; ++voice) {
            const bool matches = noteId >= 0 ? voiceNoteIds_[voice] == noteId
                                             : voicePitches_[voice] == pitch;
            if (!matches) continue;
            loos_note_off (core_, voice);
            voiceNoteIds_[voice] = -1;
            voicePitches_[voice] = -1;
            if (noteId >= 0) return;
        }
    }

    void dispatchEvent (const Vst::Event& event) {
        if (event.type == Vst::Event::kNoteOnEvent) {
            const auto pitch = std::clamp (static_cast<int32> (event.noteOn.pitch), 0, 127);
            if (event.noteOn.velocity <= 0.f) {
                releaseNote (event.noteOn.noteId, pitch);
                return;
            }
            const int32 voice = next_voice_;
            next_voice_ = (next_voice_ + 1) % LOOS_MAX_VOICES;
            voiceNoteIds_[voice] = event.noteOn.noteId;
            voicePitches_[voice] = pitch;
            loos_note_on (core_, voice,
                440.0 * std::exp2 ((static_cast<double> (pitch) - 69.0) / 12.0),
                event.noteOn.velocity);
        } else if (event.type == Vst::Event::kNoteOffEvent) {
            releaseNote (event.noteOff.noteId,
                         std::clamp (static_cast<int32> (event.noteOff.pitch), 0, 127));
        }
    }

    void copyOutput (Vst::AudioBusBuffers& output, int32 offset, int32 frames) const {
        const auto* source = loos_output (core_);
        for (int32 channel = 0; channel < output.numChannels; ++channel) {
            auto* destination = output.channelBuffers32[channel];
            if (!destination) continue;
            for (int32 frame = 0; frame < frames; ++frame) destination[offset + frame] = static_cast<float> (source[frame]);
        }
    }

    void* core_ {nullptr};
    PresetBank bank_ {};
    bool bankReady_ {false};
    double sample_rate_ {44100.0};
    int32 next_voice_ {0};
    std::array<int32, LOOS_MAX_VOICES> voiceNoteIds_ {};
    std::array<int32, LOOS_MAX_VOICES> voicePitches_ {};
};

} // namespace Steinberg::PokustonLoos

extern "C" uint32_t loos_editor_preset_count (void* hostContext) {
    return static_cast<Steinberg::PokustonLoos::Controller*> (hostContext)->presetCount ();
}

extern "C" int32_t loos_editor_current_preset (void* hostContext) {
    return static_cast<Steinberg::PokustonLoos::Controller*> (hostContext)->selectedPreset ();
}

extern "C" const char* loos_editor_preset_name (void* hostContext, uint32_t index) {
    return static_cast<Steinberg::PokustonLoos::Controller*> (hostContext)->presetName (index);
}

extern "C" bool loos_editor_load_preset (void* hostContext, uint32_t index) {
    return static_cast<Steinberg::PokustonLoos::Controller*> (hostContext)->selectFromCanvas (index);
}

using namespace Steinberg;
using namespace Steinberg::Vst;
using Steinberg::PokustonLoos::Controller;
using Steinberg::PokustonLoos::Processor;
using Steinberg::PokustonLoos::kControllerUID;
using Steinberg::PokustonLoos::kProcessorUID;

BEGIN_FACTORY_DEF ("Pokuston", "", "")
    DEF_CLASS2 (INLINE_UID_FROM_FUID (kProcessorUID), PClassInfo::kManyInstances,
                kVstAudioEffectClass, "Loos", kDistributable, "Instrument|Synth",
                "0.1.0", kVstVersionString, Processor::createInstance)
    DEF_CLASS2 (INLINE_UID_FROM_FUID (kControllerUID), PClassInfo::kManyInstances,
                kVstComponentControllerClass, "Loos Controller", 0, "",
                "0.1.0", kVstVersionString, Controller::createInstance)
END_FACTORY
