#ifndef BROM_PLUGIN_PRESET_PROGRAM_MAP_H
#define BROM_PLUGIN_PRESET_PROGRAM_MAP_H

#include <math.h>
#include <stdint.h>
#include <string.h>

/* The two ways a host program value maps to a preset slot, as pure functions of the
 * slot count. Dependency-free and header-only (C11 and C++17), so a product that
 * does not link Brom can use the same rule.
 *
 * Encode, for both: index / (count - 1), and 0.0 for a single slot. This is the VST3
 * SDK's ToNormalized(plain, stepCount) with stepCount = count - 1.
 *
 * VST3 rule (the default): floor(value * (count - 1) + 0.5). Every VST3 host sends
 * the encode above.
 *
 * AU rule: min(count - 1, floor(value * count + 1/1024)). The pinned Steinberg AUv2
 * wrapper selects factory preset i of N by sending (float)i / (float)N, which the VST3
 * rule decodes to slot i - 1 for the whole upper half (2i > N). The AU rule is the
 * SDK's own list decode plus a bias that absorbs the wrapper's single-precision
 * rounding, and it still accepts the encode above, so a controller's own edit in the
 * same AU instance keeps working. For one count both encodings of slot i lie in
 * [i/N, (i+1)/N), the bias is above the float error (3.8e-6 index units) and below the
 * VST3 margin (1/(count - 1) >= 1/127), and the argument holds for count <= 1024
 * with float-stored values failing first at 1002; the proven cap is 512.
 *
 * BROM_PRESET_PROGRAM_AU_WRAPPER selects the AU rule for brom_program_index. It is
 * defined by the build for the VST3 target that an AUv2 component embeds, never for a
 * plain VST3 or CLAP build. A binary uses exactly one flavor.
 *
 * Preconditions of the index functions: count >= 1 and a usable value in [0, 1]
 * (see brom_program_value_clamped).
 *
 * The VST3 SDK compiles every Xcode build with -ffast-math, which includes
 * -ffinite-math-only: the compiler then assumes no double is ever NaN or infinite and
 * folds isfinite() AND any bit test on a double to "finite" (measured with Apple
 * clang 17, even through a pointer). A translation unit that includes this header must
 * therefore be compiled with -fno-finite-math-only, placed AFTER -ffast-math (the
 * later flag wins); the brom_program_map CMake targets add it. */
#if defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__
#error "program_map.h needs -fno-finite-math-only after -ffast-math: host program values can be NaN"
#endif

#ifdef __cplusplus
#define BROM_PROGRAM_STATIC_ASSERT(cond, message) static_assert(cond, message)
#else
#define BROM_PROGRAM_STATIC_ASSERT(cond, message) _Static_assert(cond, message)
#endif

#define BROM_PROGRAM_AU_BIAS (1.0 / 1024.0)
/* The bias argument is proven for this many slots (float-stored VST3 values first
 * fail at 1002); callers cap their lists well below it. */
#define BROM_PROGRAM_AU_MAX_COUNT 512u

#if defined(BROM_PRESET_PROGRAM_AU_WRAPPER)
#define BROM_PROGRAM_FLAVOR_NAME "au"
#else
#define BROM_PROGRAM_FLAVOR_NAME "vst3"
#endif

/* A translation unit that defines BROM_PROGRAM_EMIT_FLAVOR_TAG before including this
 * header carries the flavor as a string, which survives stripping:
 *   strings <binary> | grep BROM_PROGRAM_FLAVOR= */
#if defined(BROM_PROGRAM_EMIT_FLAVOR_TAG) && defined(__GNUC__)
static const char brom_program_flavor_tag[] __attribute__((used)) =
    "BROM_PROGRAM_FLAVOR=" BROM_PROGRAM_FLAVOR_NAME;
#endif

/* True for every finite double: the exponent field is not all ones. Reliable only
 * without -ffinite-math-only (enforced above). */
static inline int brom_program_value_usable(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return ((bits >> 52) & 0x7ffu) != 0x7ffu;
}

/* A host value made to satisfy the index preconditions: not usable -> 0.0, otherwise
 * clamped into [0, 1]. */
static inline double brom_program_value_clamped(double value) {
    if (!brom_program_value_usable(value)) return 0.0;
    return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
}

static inline double brom_program_normalized(uint32_t count, uint32_t index) {
    return count <= 1u ? 0.0 : (double)index / (double)(count - 1u);
}

static inline uint32_t brom_program_index_vst3(uint32_t count, double value) {
    if (count <= 1u) return 0u;
    return (uint32_t)floor(value * (double)(count - 1u) + 0.5);
}

static inline uint32_t brom_program_index_au(uint32_t count, double value) {
    if (count <= 1u) return 0u;
    const uint32_t index = (uint32_t)floor(value * (double)count + BROM_PROGRAM_AU_BIAS);
    return index > count - 1u ? count - 1u : index;
}

static inline uint32_t brom_program_index(uint32_t count, double value) {
#if defined(BROM_PRESET_PROGRAM_AU_WRAPPER)
    return brom_program_index_au(count, value);
#else
    return brom_program_index_vst3(count, value);
#endif
}

#endif
