#ifndef BROM_PLUGIN_EFFECT_H
#define BROM_PLUGIN_EFFECT_H

#include <stdint.h>
#include <stddef.h>

#include "../dsp/audio.h"

/* Effect ABI revision 2: constants, the info and parameter records, and the
 * shell interface. The functions are defined by brom/plugin/vst3/effect-shell.rcl,
 * which only a C++ shell's Recoil entry imports; they act on the engine linked
 * into that binary. */
#define BROM_EFFECT_MAGIC 0x42524658u
#define BROM_EFFECT_REVISION 2u

#define BROM_SAMPLE_F32 1u
#define BROM_SAMPLE_F64 2u

#define BROM_NO_BYPASS 0xFFFFFFFFu
#define BROM_TAIL_INFINITE 0xFFFFFFFFu
#define BROM_LATENCY_PENDING 0x80000000u

#define BROM_EFFECT_VARIABLE_TAIL 0x1u

typedef struct brom_effect_info {
    uint32_t sample_format, input_layouts, sidechain_channels, bypass_param_id, flags;
    int32_t param_count, state_size;
    const char *id, *name, *vendor, *version, *category;
} brom_effect_info;

/* One parameter, as brom_effect_param_info describes it. Values are in the
 * parameter's engineering units; steps of 0 or 1 is continuous. */
typedef struct brom_effect_param {
    uint32_t id;
    int32_t steps;
    double min_value, max_value, default_value;
    const char *name, *module;
} brom_effect_param;

#define BROM_EFFECT_ASSERTS(A) \
    A(BROM_EFFECT_MAGIC == (('B' << 24) | ('R' << 16) | ('F' << 8) | 'X'), "effect magic is BRFX") \
    A(BROM_LATENCY_PENDING == (1u << 31), "latency pending is bit 31") \
    A(BROM_TAIL_INFINITE == UINT32_MAX, "infinite tail is all ones") \
    A(BROM_NO_BYPASS == UINT32_MAX, "no bypass is all ones") \
    A(BROM_EFFECT_VARIABLE_TAIL == (1u << 0), "variable tail is bit 0") \
    A(BROM_SAMPLE_F32 == 1, "f32 sample format") \
    A(BROM_SAMPLE_F64 == 2, "f64 sample format") \
    A(BROM_EFFECT_REVISION == 2, "effect revision") \
    A(offsetof(brom_effect_info, sample_format) == 0, "sample_format offset") \
    A(offsetof(brom_effect_info, input_layouts) == 4, "input_layouts offset") \
    A(offsetof(brom_effect_info, sidechain_channels) == 8, "sidechain_channels offset") \
    A(offsetof(brom_effect_info, bypass_param_id) == 12, "bypass_param_id offset") \
    A(offsetof(brom_effect_info, flags) == 16, "flags offset") \
    A(offsetof(brom_effect_info, param_count) == 20, "param_count offset") \
    A(offsetof(brom_effect_info, state_size) == 24, "state_size offset") \
    A(offsetof(brom_effect_info, id) == (sizeof(void *) == 8 ? 32 : 28), "id offset") \
    A(offsetof(brom_effect_info, name) == offsetof(brom_effect_info, id) + sizeof(void *), "name offset") \
    A(offsetof(brom_effect_info, vendor) == offsetof(brom_effect_info, name) + sizeof(void *), "vendor offset") \
    A(offsetof(brom_effect_info, version) == offsetof(brom_effect_info, vendor) + sizeof(void *), "version offset") \
    A(offsetof(brom_effect_info, category) == offsetof(brom_effect_info, version) + sizeof(void *), "category offset") \
    A(sizeof(brom_effect_info) == offsetof(brom_effect_info, category) + sizeof(void *), "effect info size") \
    A(offsetof(brom_effect_param, steps) == 4, "param steps offset") \
    A(offsetof(brom_effect_param, min_value) == 8, "param min_value offset") \
    A(offsetof(brom_effect_param, max_value) == 16, "param max_value offset") \
    A(offsetof(brom_effect_param, default_value) == 24, "param default_value offset") \
    A(offsetof(brom_effect_param, name) == 32, "param name offset") \
    A(offsetof(brom_effect_param, module) == 32 + sizeof(void *), "param module offset") \
    A(sizeof(brom_effect_param) == 32 + 2 * sizeof(void *), "effect param size")

#if defined(__cplusplus)
#define BROM_EFFECT_ASSERT_ONE(cond, msg) static_assert(cond, msg);
#else
#define BROM_EFFECT_ASSERT_ONE(cond, msg) _Static_assert(cond, msg);
#endif
BROM_EFFECT_ASSERTS(BROM_EFFECT_ASSERT_ONE)

/* Recoil's logic! crosses the C ABI as int, so every predicate below returns
 * int: nonzero is true. */
#if defined(__cplusplus)
extern "C" {
#endif

/* Lifecycle. brom_effect_init validates the table (brom-effect-valid?) and
 * fills the info record; a shell calls it once per module load and exposes no
 * plug-in when it returns false. */
int brom_effect_init(void);
const brom_effect_info *brom_effect_describe(void);
void *brom_effect_create(void);
void brom_effect_destroy(void *core);
int brom_effect_activate(void *core, double sample_rate, int32_t max_frames);
void brom_effect_reset(void *core);
void brom_effect_deactivate(void *core);

/* Audio: at most BROM_AUDIO_CAPACITY frames per call. The trampoline for the
 * format the effect does not use returns false. */
int brom_effect_process_f32(void *core, brom_audio_f32 *input, brom_audio_f32 *sidechain,
                            brom_audio_f32 *output, int32_t channels,
                            int32_t sidechain_channels, int32_t frames);
int brom_effect_process_f64(void *core, brom_audio_f64 *input, brom_audio_f64 *sidechain,
                            brom_audio_f64 *output, int32_t channels,
                            int32_t sidechain_channels, int32_t frames);

/* Controls. Latency, tail and current program are wait-free reads. */
int brom_effect_midi(void *core, uint32_t status, uint32_t data1, uint32_t data2);
int brom_effect_program(void *core, uint32_t index);
int32_t brom_effect_current_program(void *core);
uint32_t brom_effect_latency(void *core);
uint32_t brom_effect_tail(void *core);

/* Parameters, in engineering units except where normalized. */
int brom_effect_param_info(uint32_t index, brom_effect_param *out);
double brom_effect_param_get(void *core, uint32_t id);
int brom_effect_param_set(void *core, uint32_t id, double value);
double brom_effect_param_normalize(uint32_t id, double value);
double brom_effect_param_denormalize(uint32_t id, double normalized);
int brom_effect_param_text(void *core, uint32_t id, double value, char *out, uint32_t capacity);
int brom_effect_param_parse(void *core, uint32_t id, const char *text, double *out);

/* State: exactly state_size bytes, either Brom state format on load. */
int brom_effect_state_save(void *core, void *out, size_t capacity);
int brom_effect_state_load(void *core, const void *bytes, size_t size);

/* Factory presets. */
uint32_t brom_effect_factory_count(void);
int brom_effect_factory_name(uint32_t index, char *out, uint32_t capacity);
int brom_effect_factory_values(uint32_t index, double *out, uint32_t count);

/* Editor: nonzero when the engine has one (has-editor), with its fixed size. */
int brom_effect_editor_size(int32_t *width, int32_t *height);

#if defined(__cplusplus)
}
#endif

#endif
