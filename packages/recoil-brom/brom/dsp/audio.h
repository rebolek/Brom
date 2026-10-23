#ifndef BROM_DSP_AUDIO_H
#define BROM_DSP_AUDIO_H

#include <stddef.h>

#define BROM_AUDIO_CAPACITY 512

/* Recoil emits callable typedefs before its mirrored struct definitions.
 * Give typed callback parameters the same file-scope tag as that mirror.
 * C++ consumers use the public typedef below, whose name shares the tag's
 * namespace in C++ but not in generated C. */
#if !defined(__cplusplus)
struct brom_audio_f32;
struct brom_audio_f64;
#endif

/* Distinct tag lets generated Recoil mirrors share this translation unit. */
typedef struct brom_audio_f32_block {
    float left[BROM_AUDIO_CAPACITY];
    float right[BROM_AUDIO_CAPACITY];
} brom_audio_f32;

typedef struct brom_audio_f64_block {
    double left[BROM_AUDIO_CAPACITY];
    double right[BROM_AUDIO_CAPACITY];
} brom_audio_f64;

#if defined(__cplusplus)
static_assert(sizeof(brom_audio_f32) == 4096, "Brom planar float block size");
static_assert(offsetof(brom_audio_f32, left) == 0, "Brom left plane offset");
static_assert(offsetof(brom_audio_f32, right) == 2048, "Brom right plane offset");
static_assert(sizeof(brom_audio_f64) == 8192, "Brom planar double block size");
static_assert(offsetof(brom_audio_f64, left) == 0, "Brom f64 left plane offset");
static_assert(offsetof(brom_audio_f64, right) == 4096, "Brom f64 right plane offset");
#else
_Static_assert(sizeof(brom_audio_f32) == 4096, "Brom planar float block size");
_Static_assert(offsetof(brom_audio_f32, left) == 0, "Brom left plane offset");
_Static_assert(offsetof(brom_audio_f32, right) == 2048, "Brom right plane offset");
_Static_assert(sizeof(brom_audio_f64) == 8192, "Brom planar double block size");
_Static_assert(offsetof(brom_audio_f64, left) == 0, "Brom f64 left plane offset");
_Static_assert(offsetof(brom_audio_f64, right) == 4096, "Brom f64 right plane offset");
#endif

#endif
