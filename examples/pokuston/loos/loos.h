#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* These must agree with core.rcl, which is the engine's own statement of them.
   They did not: this header said twelve parameters after the thirteenth
   (Shape) was added, so the VST3 and AUv2 bridges -- which size
   processTimeline and readState from these two macros -- automated and saved
   one fewer parameter than CLAP did, silently. */
enum {
    LOOS_PARAM_CUTOFF = 0,
    LOOS_PARAM_RESONANCE = 1,
    LOOS_PARAM_ENV_AMOUNT = 2,
    LOOS_PARAM_ATTACK = 3,
    LOOS_PARAM_DECAY = 4,
    LOOS_PARAM_SUSTAIN = 5,
    LOOS_PARAM_RELEASE = 6,
    LOOS_PARAM_LFO_RATE = 7,
    LOOS_PARAM_LFO_CUTOFF_DEPTH = 8,
    LOOS_PARAM_MORPH = 9,
    LOOS_PARAM_COUNT = 10,
    LOOS_MAX_VOICES = 4,
    LOOS_MAX_BLOCK = 512,
};

#define LOOS_STATE_MAGIC 0x534F4F4Cu
#define LOOS_STATE_VERSION 2u
#define LOOS_STATE_SIZE (20u + LOOS_PARAM_COUNT * 8u)

void *loos_create(void);
void loos_destroy(void *instance);
void loos_activate(void *instance, double sample_rate);
void loos_reset(void *instance);
/* Fills a brom_voice_state for one slot so Brom's shared allocator can rank
   it. Declared as void * because the record belongs to recoil-brom. */
int loos_voice_state(void *instance, int slot, void *out);
void loos_note_on(void *instance, int voice, double frequency, double velocity);
void loos_note_off(void *instance, int voice);
void loos_process_block(void *instance, int frames);
const double *loos_output(const void *instance);
int loos_output_capacity(void);

bool loos_set_parameter(void *instance, uint32_t parameter, double value);
double loos_get_parameter(const void *instance, uint32_t parameter);
const char *loos_param_name(uint32_t parameter);
double loos_parameter_to_normalized(uint32_t parameter, double value);
double loos_parameter_from_normalized(uint32_t parameter, double normalized);
double loos_parameter_default(uint32_t parameter);

size_t loos_state_size(void);
bool loos_state_save(const void *core, uint8_t *bytes, size_t capacity);
bool loos_state_load(void *core, const uint8_t *bytes, size_t size);

uint32_t loos_get_factory_preset_count(void);
int loos_factory_preset_name(uint32_t index, void *out, uint32_t capacity);
int loos_factory_preset_key(uint32_t index, void *out, uint32_t capacity);
int loos_factory_preset_values(uint32_t index, void *out, uint32_t capacity);

#ifdef __cplusplus
}
#endif
