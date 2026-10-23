#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* These must agree with core.rcl, which is the engine's own statement of
   them; core-test.rcl checks the numbers. */
enum {
    PM101_PARAM_COUNT = 213,
    PM101_MAX_VOICES = 4,
    PM101_MAX_BLOCK = 512,
};

#define PM101_STATE_MAGIC 0x31303150u
#define PM101_STATE_VERSION 1u
#define PM101_STATE_SIZE (20u + PM101_PARAM_COUNT * 8u)

enum {
    PM101_ROLE_UNIT = 0,
    PM101_ROLE_WAVE_A = 1,
    PM101_ROLE_WAVE_B = 2,
    PM101_ROLE_WINDOW = 3,
    PM101_ROLE_FILTER = 4,
    PM101_ROLE_RATE = 5,
    PM101_ROLE_LEVEL = 6,
    PM101_ROLE_MODE = 7,
    PM101_ROLE_TOGGLE = 8,
    PM101_ROLE_BITS = 9,
    PM101_ROLE_DIVIDER = 10,
    PM101_ROLE_DETUNE_OCTAVE = 11,
    PM101_ROLE_DETUNE_NOTE = 12,
    PM101_ROLE_DETUNE_CENT = 13,
    PM101_ROLE_VIBRATO_RATE = 14,
};

void *pm101_create(void);
void pm101_destroy(void *instance);
void pm101_activate(void *instance, double sample_rate);
void pm101_reset(void *instance);
int pm101_voice_state(void *instance, int slot, void *out);
void pm101_note_on(void *instance, int voice, double frequency, double velocity);
void pm101_note_off(void *instance, int voice);
void pm101_process_block(void *instance, int frames);
const double *pm101_output(const void *instance);
int pm101_output_capacity(void);

bool pm101_set_parameter(void *instance, uint32_t parameter, double value);
double pm101_get_parameter(const void *instance, uint32_t parameter);
const char *pm101_param_name(uint32_t parameter);
const char *pm101_param_module(uint32_t parameter);
int pm101_parameter_role(uint32_t parameter);
int pm101_parameter_steps(uint32_t parameter);
double pm101_parameter_min(uint32_t parameter);
double pm101_parameter_max(uint32_t parameter);
double pm101_parameter_to_normalized(uint32_t parameter, double value);
double pm101_parameter_from_normalized(uint32_t parameter, double normalized);
double pm101_parameter_default(uint32_t parameter);

size_t pm101_state_size(void);
bool pm101_save_state(const void *core, uint8_t *bytes, size_t capacity);
bool pm101_load_state(void *core, const uint8_t *bytes, size_t size);
bool pm101_state_save(const void *core, uint8_t *bytes, size_t capacity);
bool pm101_state_load(void *core, const uint8_t *bytes, size_t size);

uint32_t pm101_get_factory_preset_count(void);
int pm101_factory_preset_name(uint32_t index, void *out, uint32_t capacity);
int pm101_factory_preset_key(uint32_t index, void *out, uint32_t capacity);
int pm101_factory_preset_values(uint32_t index, void *out, uint32_t capacity);

#ifdef __cplusplus
}
#endif
