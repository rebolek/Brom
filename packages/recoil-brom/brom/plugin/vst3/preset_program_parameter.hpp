#pragma once

// The host-visible program parameter of a Brom plug-in: a VST3 list parameter whose
// value strings are derived with the same rule the processor selects with, so the
// name a host (or the pinned AUv2 wrapper) shows is the preset that sounds.
//
// The SDK's own StringListParameter decodes with floor(v * N), which names the
// previous preset for the wrapper's rounding-down values (N = 10: 7 and 9) when the
// processor decodes with the AU rule, and with the VST3 rule off the grid. This class
// calls brom_program_index, the flavor-selected rule of program_map.h, so it follows
// the build's flavor. Header-only: macro-dependent inline code is acceptable because a
// binary uses exactly one flavor (see BromProgramMap.cmake).
//
// The list is capped at BROM_PRESET_PROGRAM_MAX_SLOTS entries, the slots a host
// program list exposes. The SDK's constructors never see the strings (they arrive
// through appendString, which raises stepCount from -1), so the cap lives there.

#include "../preset/catalog_service.h"
#include "../preset/program_map.h"

#include "base/source/fdebug.h"
#include "public.sdk/source/vst/vstparameters.h"

namespace Steinberg::PokustonVst3Support {

class PresetProgramParameter final : public Vst::StringListParameter {
public:
    // `flags` are any extra parameter flags; the product passes 0 to keep its AU flags.
    PresetProgramParameter (const Vst::TChar* title, Vst::ParamID id, Vst::UnitID unit, int32 flags = 0)
    : Vst::StringListParameter (title, id, nullptr,
                                flags | Vst::ParameterInfo::kIsProgramChange | Vst::ParameterInfo::kIsList,
                                unit) {}

    // Ignores entries past the cap instead of growing a list the table cannot hold.
    void appendString (const Vst::String128 string) SMTG_OVERRIDE {
        if (strings.size () >= BROM_PRESET_PROGRAM_MAX_SLOTS) {
            SMTG_ASSERT (false && "program list exceeds BROM_PRESET_PROGRAM_MAX_SLOTS");
            return;
        }
        Vst::StringListParameter::appendString (string);
    }

    // Clamps and rejects non-finite values before the rule's preconditions: a host's
    // raw value reaches this from the AU wrapper's getParamStringByValue.
    Vst::ParamValue toPlain (Vst::ParamValue normalized) const SMTG_OVERRIDE {
        if (strings.empty ()) return 0;
        const auto count = static_cast<uint32_t> (strings.size ());
        return static_cast<Vst::ParamValue> (
            brom_program_index (count, brom_program_value_clamped (normalized)));
    }

    // The base class throws std::out_of_range on an empty list.
    void toString (Vst::ParamValue normalized, Vst::String128 string) const SMTG_OVERRIDE {
        if (strings.empty ()) {
            string[0] = 0;
            return;
        }
        Vst::StringListParameter::toString (normalized, string);
    }
};

} // namespace Steinberg::PokustonVst3Support
