#include "clap_support.h"
#include "clap_support_api.h"
#include <math.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Exported by entry.rcl; Rebol logic! uses the C int ABI. */
extern int recoil_clap_plugin_load_preset(void *instance,
                                          const char *load_key,
                                          uint32_t location_kind,
                                          const char *location);

enum { RECOIL_CLAP_PRESET_PARAM_CAPACITY = 256 };
enum {
    RECOIL_CLAP_FACTORY_PRESET_CAPACITY = 128,
    RECOIL_CLAP_FACTORY_FEATURE_CAPACITY = 16
};

typedef struct recoil_clap_preset_snapshot recoil_clap_preset_snapshot;
typedef struct recoil_clap_preset_mailbox recoil_clap_preset_mailbox;
typedef struct recoil_clap_effect_cache recoil_clap_effect_cache;

struct recoil_clap_preset_snapshot {
    int32_t count;
    int32_t index;
    double values[RECOIL_CLAP_PRESET_PARAM_CAPACITY];
    struct recoil_clap_preset_snapshot *next;
};

struct recoil_clap_preset_mailbox {
    _Atomic(struct recoil_clap_preset_snapshot *) pending;
    _Atomic(struct recoil_clap_preset_snapshot *) retired;
    _Atomic(int32_t) current_index;
    _Atomic(uint32_t) active_count;
    _Atomic(uint64_t) active_values[RECOIL_CLAP_PRESET_PARAM_CAPACITY];
    _Atomic(uint32_t) completions;
};

static void recoil_clap_preset_retire(
    recoil_clap_preset_mailbox *mailbox,
    recoil_clap_preset_snapshot *snapshot) {
    if (!mailbox || !snapshot)
        return;
    recoil_clap_preset_snapshot *head =
        atomic_load_explicit(&mailbox->retired, memory_order_relaxed);
    do {
        snapshot->next = head;
    } while (!atomic_compare_exchange_weak_explicit(
        &mailbox->retired, &head, snapshot,
        memory_order_release, memory_order_relaxed));
}

recoil_clap_preset_mailbox *recoil_clap_preset_mailbox_create(void) {
    recoil_clap_preset_mailbox *mailbox = calloc(1, sizeof(*mailbox));
    if (!mailbox)
        return NULL;
    atomic_init(&mailbox->pending, NULL);
    atomic_init(&mailbox->retired, NULL);
    atomic_init(&mailbox->current_index, -1);
    atomic_init(&mailbox->active_count, 0);
    atomic_init(&mailbox->completions, 0);
    for (uint32_t index = 0; index < RECOIL_CLAP_PRESET_PARAM_CAPACITY; ++index)
        atomic_init(&mailbox->active_values[index], 0);
    return mailbox;
}

void recoil_clap_preset_mailbox_reclaim(recoil_clap_preset_mailbox *mailbox) {
    if (!mailbox)
        return;
    recoil_clap_preset_snapshot *snapshot =
        atomic_exchange_explicit(&mailbox->retired, NULL, memory_order_acquire);
    while (snapshot) {
        recoil_clap_preset_snapshot *next = snapshot->next;
        free(snapshot);
        snapshot = next;
    }
}

void recoil_clap_preset_mailbox_destroy(recoil_clap_preset_mailbox *mailbox) {
    if (!mailbox)
        return;
    free(atomic_exchange_explicit(&mailbox->pending, NULL, memory_order_acquire));
    recoil_clap_preset_mailbox_reclaim(mailbox);
    free(mailbox);
}

recoil_clap_preset_snapshot *recoil_clap_preset_snapshot_create(
    uint32_t count, int32_t index) {
    if (count > RECOIL_CLAP_PRESET_PARAM_CAPACITY)
        return NULL;
    recoil_clap_preset_snapshot *snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot)
        return NULL;
    snapshot->count = (int32_t)count;
    snapshot->index = index;
    return snapshot;
}

void *recoil_clap_preset_snapshot_values(
    recoil_clap_preset_snapshot *snapshot) {
    return snapshot ? snapshot->values : NULL;
}

void recoil_clap_preset_mailbox_publish(
    recoil_clap_preset_mailbox *mailbox,
    recoil_clap_preset_snapshot *snapshot) {
    if (!mailbox || !snapshot)
        return;
    recoil_clap_preset_snapshot *superseded = atomic_exchange_explicit(
        &mailbox->pending, snapshot, memory_order_acq_rel);
    free(superseded);
}

void recoil_clap_preset_mailbox_cancel(recoil_clap_preset_mailbox *mailbox) {
    if (!mailbox)
        return;
    free(atomic_exchange_explicit(&mailbox->pending, NULL,
                                  memory_order_acquire));
}

recoil_clap_preset_snapshot *recoil_clap_preset_mailbox_take(
    recoil_clap_preset_mailbox *mailbox) {
    return mailbox ? atomic_exchange_explicit(
        &mailbox->pending, NULL, memory_order_acquire) : NULL;
}

void recoil_clap_preset_mailbox_commit(
    recoil_clap_preset_mailbox *mailbox,
    recoil_clap_preset_snapshot *snapshot) {
    if (!mailbox || !snapshot)
        return;
    atomic_store_explicit(&mailbox->current_index, -1, memory_order_release);
    for (int32_t index = 0; index < snapshot->count; ++index) {
        uint64_t bits = 0;
        memcpy(&bits, &snapshot->values[index], sizeof(bits));
        atomic_store_explicit(&mailbox->active_values[index], bits,
                              memory_order_relaxed);
    }
    atomic_store_explicit(&mailbox->active_count, (uint32_t)snapshot->count,
                          memory_order_relaxed);
    atomic_store_explicit(&mailbox->current_index, snapshot->index,
                          memory_order_release);
    atomic_fetch_add_explicit(&mailbox->completions, 1, memory_order_release);
    recoil_clap_preset_retire(mailbox, snapshot);
}

void recoil_clap_preset_mailbox_reject(
    recoil_clap_preset_mailbox *mailbox,
    recoil_clap_preset_snapshot *snapshot) {
    recoil_clap_preset_retire(mailbox, snapshot);
}

int32_t recoil_clap_preset_mailbox_current(
    const recoil_clap_preset_mailbox *mailbox) {
    return mailbox ? atomic_load_explicit(&mailbox->current_index,
                                           memory_order_acquire) : -1;
}

void recoil_clap_preset_mailbox_clear(recoil_clap_preset_mailbox *mailbox) {
    if (mailbox)
        atomic_store_explicit(&mailbox->current_index, -1,
                              memory_order_release);
}

bool recoil_clap_preset_mailbox_active_matches(
    const recoil_clap_preset_mailbox *mailbox,
    uint32_t param_id,
    double value) {
    if (!mailbox || atomic_load_explicit(&mailbox->current_index,
                                         memory_order_acquire) == -1)
        return false;
    if (param_id >= atomic_load_explicit(&mailbox->active_count,
                                         memory_order_relaxed))
        return false;
    const uint64_t bits = atomic_load_explicit(
        &mailbox->active_values[param_id], memory_order_relaxed);
    double expected = 0.0;
    memcpy(&expected, &bits, sizeof(expected));
    const double scale = fmax(1.0, fabs(expected));
    return fabs(value - expected) <= scale * 1e-9;
}

uint32_t recoil_clap_preset_mailbox_take_completions(
    recoil_clap_preset_mailbox *mailbox) {
    return mailbox ? atomic_exchange_explicit(
        &mailbox->completions, 0, memory_order_acquire) : 0;
}

/*
 * Remaining helpers are intentional CLAP-side ABI glue:
 * audio buffer marshaling and stream wrappers. Plugin instance ownership,
 * parameter storage, and GUI wrapper logic now live on the Recoil side.
 */

const char *recoil_clap_features_list[] = {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_SYNTHESIZER,
    NULL,
};

/* Tail and current program of an effect, written at block end on the audio
 * thread and read on the main thread. Two independent atomics: wait-free, no
 * allocation after creation. Tail starts at 0 and program at -1 ("none"). */
struct recoil_clap_effect_cache {
    _Atomic(uint32_t) tail;
    _Atomic(int32_t) program;
};

recoil_clap_effect_cache *recoil_clap_effect_cache_create(void) {
    recoil_clap_effect_cache *cache = calloc(1, sizeof(*cache));
    if (!cache)
        return NULL;
    atomic_init(&cache->tail, 0);
    atomic_init(&cache->program, -1);
    return cache;
}

void recoil_clap_effect_cache_destroy(recoil_clap_effect_cache *cache) {
    free(cache);
}

uint32_t recoil_clap_effect_cache_tail(const recoil_clap_effect_cache *cache) {
    return cache ? atomic_load_explicit(&cache->tail, memory_order_relaxed) : 0;
}

void recoil_clap_effect_cache_set_tail(recoil_clap_effect_cache *cache, uint32_t tail) {
    if (cache)
        atomic_store_explicit(&cache->tail, tail, memory_order_relaxed);
}

int32_t recoil_clap_effect_cache_program(const recoil_clap_effect_cache *cache) {
    return cache ? atomic_load_explicit(&cache->program, memory_order_relaxed) : -1;
}

void recoil_clap_effect_cache_set_program(recoil_clap_effect_cache *cache, int32_t program) {
    if (cache)
        atomic_store_explicit(&cache->program, program, memory_order_relaxed);
}

const char *recoil_clap_effect_features_list[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    NULL,
    NULL,
};

/* Slot 1 of the effect feature list follows the effect's category. An
 * unknown category leaves the list at "audio-effect" alone. */
void recoil_clap_effect_features_select(const char *category) {
    static const struct { const char *category; const char *feature; } known[] = {
        {"reverb", CLAP_PLUGIN_FEATURE_REVERB},
        {"delay", CLAP_PLUGIN_FEATURE_DELAY},
        {"filter", CLAP_PLUGIN_FEATURE_FILTER},
        {"equalizer", CLAP_PLUGIN_FEATURE_EQUALIZER},
        {"compressor", CLAP_PLUGIN_FEATURE_COMPRESSOR},
        {"limiter", CLAP_PLUGIN_FEATURE_LIMITER},
        {"distortion", CLAP_PLUGIN_FEATURE_DISTORTION},
        {"chorus", CLAP_PLUGIN_FEATURE_CHORUS},
        {"flanger", CLAP_PLUGIN_FEATURE_FLANGER},
        {"phaser", CLAP_PLUGIN_FEATURE_PHASER},
        {"pitch-shifter", CLAP_PLUGIN_FEATURE_PITCH_SHIFTER},
        {"analyzer", CLAP_PLUGIN_FEATURE_ANALYZER},
        {"utility", CLAP_PLUGIN_FEATURE_UTILITY},
    };
    recoil_clap_effect_features_list[1] = NULL;
    if (!category) return;
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
        if (strcmp(category, known[i].category) == 0) {
            recoil_clap_effect_features_list[1] = known[i].feature;
            return;
        }
    }
}

/* Effect port conversion. One rule set for every width: zero the chunk's
 * first `frames` samples of both planes, then per plane prefer data32 over
 * data64 (only a misbehaving host sets both), leave a plane with neither
 * pointer zero, and fill a plane flagged in constant_mask from its sample 0.
 * Each port is converted on its own, so mixed widths across ports and planes
 * are fine. The macros below define recoil_clap_read_effect_f32/_f64,
 * recoil_clap_write_effect_f32/_f64 and recoil_clap_clear_effect_f32/_f64. */
#define RECOIL_CLAP_EFFECT_PLANE(block, ch) ((ch) == 0 ? (block)->left : (block)->right)

#define RECOIL_CLAP_DEFINE_READ_PLANE(suffix, sample_t)                        \
    static void recoil_clap_read_plane_##suffix(                              \
        const clap_audio_buffer_t *buffer, uint32_t channel,                   \
        uint32_t offset, uint32_t frames, sample_t *dst) {                     \
        const bool constant = channel < 64 &&                                  \
            (buffer->constant_mask & ((uint64_t)1 << channel)) != 0;           \
        if (buffer->data32 && buffer->data32[channel]) {                       \
            const float *src = buffer->data32[channel];                        \
            if (constant) {                                                    \
                const sample_t value = (sample_t)src[0];                       \
                for (uint32_t i = 0; i < frames; ++i) dst[i] = value;          \
            } else {                                                           \
                for (uint32_t i = 0; i < frames; ++i)                          \
                    dst[i] = (sample_t)src[offset + i];                        \
            }                                                                  \
        } else if (buffer->data64 && buffer->data64[channel]) {                \
            const double *src = buffer->data64[channel];                       \
            if (constant) {                                                    \
                const sample_t value = (sample_t)src[0];                       \
                for (uint32_t i = 0; i < frames; ++i) dst[i] = value;          \
            } else {                                                           \
                for (uint32_t i = 0; i < frames; ++i)                          \
                    dst[i] = (sample_t)src[offset + i];                        \
            }                                                                  \
        }                                                                      \
    }

RECOIL_CLAP_DEFINE_READ_PLANE(f32, float)
RECOIL_CLAP_DEFINE_READ_PLANE(f64, double)

/* Returns the planes read, 0 for a disconnected sidechain, or -1 for a
 * structural error (a main port narrower than the layout, bad arguments). */
#define RECOIL_CLAP_DEFINE_READ_EFFECT(suffix, block_t, sample_t)              \
    int32_t recoil_clap_read_effect_##suffix(                                  \
        const clap_process_t *proc, uint32_t port_index, uint32_t offset,      \
        uint32_t frames, uint32_t channels, bool main_port, block_t *block) {  \
        if (!block || frames > BROM_AUDIO_CAPACITY || channels > 2 ||          \
            (main_port && channels == 0))                                      \
            return -1;                                                         \
        for (uint32_t i = 0; i < frames; ++i) {                                \
            block->left[i] = (sample_t)0;                                      \
            block->right[i] = (sample_t)0;                                     \
        }                                                                      \
        const bool has_port = proc && proc->audio_inputs &&                    \
            proc->audio_inputs_count > port_index;                             \
        if (main_port) {                                                       \
            if (!has_port) return (int32_t)channels;                           \
            const clap_audio_buffer_t *buffer = &proc->audio_inputs[port_index]; \
            if (buffer->channel_count < channels) return -1;                   \
            for (uint32_t ch = 0; ch < channels; ++ch)                         \
                recoil_clap_read_plane_##suffix(buffer, ch, offset, frames,    \
                    RECOIL_CLAP_EFFECT_PLANE(block, ch));                      \
            return (int32_t)channels;                                          \
        }                                                                      \
        if (!has_port) return 0;                                               \
        const clap_audio_buffer_t *buffer = &proc->audio_inputs[port_index];   \
        if (buffer->channel_count == 0 || (!buffer->data32 && !buffer->data64)) \
            return 0;                                                          \
        const uint32_t planes =                                                \
            buffer->channel_count < channels ? buffer->channel_count : channels; \
        for (uint32_t ch = 0; ch < planes; ++ch)                               \
            recoil_clap_read_plane_##suffix(buffer, ch, offset, frames,        \
                RECOIL_CLAP_EFFECT_PLANE(block, ch));                          \
        return (int32_t)planes;                                                \
    }

RECOIL_CLAP_DEFINE_READ_EFFECT(f32, brom_audio_f32, float)
RECOIL_CLAP_DEFINE_READ_EFFECT(f64, brom_audio_f64, double)

/* Writes both output planes, data32 winning per plane, and clears
 * constant_mask. Every plane is checked before any is written, so a
 * structural failure leaves the host buffer untouched. */
#define RECOIL_CLAP_DEFINE_WRITE_EFFECT(suffix, block_t, sample_t)             \
    bool recoil_clap_write_effect_##suffix(                                    \
        const clap_process_t *proc, uint32_t offset, uint32_t frames,          \
        const block_t *block) {                                                \
        if (!proc || !block || frames > BROM_AUDIO_CAPACITY ||                 \
            !proc->audio_outputs || proc->audio_outputs_count == 0)            \
            return false;                                                      \
        clap_audio_buffer_t *buffer = &proc->audio_outputs[0];                 \
        if (buffer->channel_count < 2) return false;                           \
        for (uint32_t ch = 0; ch < 2; ++ch) {                                  \
            if (!(buffer->data32 && buffer->data32[ch]) &&                     \
                !(buffer->data64 && buffer->data64[ch]))                       \
                return false;                                                  \
        }                                                                      \
        for (uint32_t ch = 0; ch < 2; ++ch) {                                  \
            const sample_t *src = RECOIL_CLAP_EFFECT_PLANE(block, ch);         \
            if (buffer->data32 && buffer->data32[ch]) {                        \
                float *dst = buffer->data32[ch] + offset;                      \
                for (uint32_t i = 0; i < frames; ++i) dst[i] = (float)src[i];  \
            } else {                                                           \
                double *dst = buffer->data64[ch] + offset;                     \
                for (uint32_t i = 0; i < frames; ++i) dst[i] = (double)src[i]; \
            }                                                                  \
        }                                                                      \
        buffer->constant_mask = 0;                                             \
        return true;                                                           \
    }

RECOIL_CLAP_DEFINE_WRITE_EFFECT(f32, brom_audio_f32, float)
RECOIL_CLAP_DEFINE_WRITE_EFFECT(f64, brom_audio_f64, double)

/* A refused chunk is written as silence: the effect left its output block
 * untouched, so without this the scratch would replay an earlier chunk. */
#define RECOIL_CLAP_DEFINE_CLEAR_EFFECT(suffix, block_t, sample_t)             \
    void recoil_clap_clear_effect_##suffix(block_t *block, uint32_t frames) {  \
        if (!block) return;                                                    \
        if (frames > BROM_AUDIO_CAPACITY) frames = BROM_AUDIO_CAPACITY;        \
        for (uint32_t i = 0; i < frames; ++i) {                                \
            block->left[i] = (sample_t)0;                                      \
            block->right[i] = (sample_t)0;                                     \
        }                                                                      \
    }

RECOIL_CLAP_DEFINE_CLEAR_EFFECT(f32, brom_audio_f32, float)
RECOIL_CLAP_DEFINE_CLEAR_EFFECT(f64, brom_audio_f64, double)

/* Recoil owns the bank and the instance. This file only translates its flat
 * accessors into the callback-heavy CLAP discovery ABI. */
typedef struct recoil_clap_preset_provider {
    clap_preset_discovery_provider_t provider;
    const clap_preset_discovery_indexer_t *indexer;
    struct recoil_clap_preset_generation *generation;
    uint32_t preset_index;
} recoil_clap_preset_provider;

typedef struct recoil_clap_cached_preset {
    char name[CLAP_NAME_SIZE];
    char key[CLAP_NAME_SIZE];
    char description[CLAP_PATH_SIZE];
    char provider_id[CLAP_PATH_SIZE];
    char location[CLAP_PATH_SIZE];
    bool is_factory;
    uint32_t location_kind;
    clap_preset_discovery_provider_descriptor_t descriptor;
    uint32_t feature_count;
    char features[RECOIL_CLAP_FACTORY_FEATURE_CAPACITY][CLAP_NAME_SIZE];
} recoil_clap_cached_preset;

typedef struct recoil_clap_preset_generation {
    struct recoil_clap_preset_generation *older;
    char plugin_id[CLAP_PATH_SIZE];
    char plugin_name[CLAP_NAME_SIZE];
    char vendor[CLAP_NAME_SIZE];
    uint32_t count;
    recoil_clap_cached_preset presets[RECOIL_CLAP_FACTORY_PRESET_CAPACITY];
} recoil_clap_preset_generation;

static _Atomic(recoil_clap_preset_generation *) recoil_preset_current;
static recoil_clap_preset_generation *recoil_preset_generations;

static recoil_clap_preset_generation *recoil_preset_active(void) {
    return atomic_load_explicit(&recoil_preset_current, memory_order_acquire);
}

static bool recoil_clap_copy_text(char *destination, size_t capacity,
                                  const char *source) {
    if (!destination || !capacity || !source)
        return false;
    const size_t length = strlen(source);
    if (length >= capacity)
        return false;
    memcpy(destination, source, length + 1);
    return true;
}

static bool CLAP_ABI recoil_preset_provider_init(
    const clap_preset_discovery_provider_t *provider) {
    if (!provider || !provider->provider_data)
        return false;
    const recoil_clap_preset_provider *owned = provider->provider_data;
    if (!owned->indexer || !owned->indexer->declare_location || !owned->generation ||
        owned->preset_index >= owned->generation->count)
        return false;
    const recoil_clap_cached_preset *preset =
        &owned->generation->presets[owned->preset_index];
    const clap_preset_discovery_location_t location = {
        .flags = preset->is_factory ? CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT : 0,
        .name = preset->name,
        .kind = preset->location_kind,
        .location = preset->location_kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN
                        ? NULL : preset->location,
    };
    return owned->indexer->declare_location(owned->indexer, &location);
}

static void CLAP_ABI recoil_preset_provider_destroy(
    const clap_preset_discovery_provider_t *provider) {
    if (provider)
        free(provider->provider_data);
}

static bool CLAP_ABI recoil_preset_provider_get_metadata(
    const clap_preset_discovery_provider_t *provider,
    uint32_t location_kind,
    const char *location,
    const clap_preset_discovery_metadata_receiver_t *receiver) {
    if (!provider || !provider->provider_data || !receiver ||
        !receiver->begin_preset)
        return false;
    const recoil_clap_preset_provider *owned = provider->provider_data;
    if (!owned->generation || owned->preset_index >= owned->generation->count)
        return false;

    const clap_universal_plugin_id_t plugin_id = {
        .abi = "clap",
        .id = owned->generation->plugin_id,
    };

    const recoil_clap_cached_preset *preset =
        &owned->generation->presets[owned->preset_index];
    if (location_kind != preset->location_kind ||
        (preset->location_kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN
             ? location != NULL
             : (!location || strcmp(location, preset->location) != 0)))
        return false;
    if (!preset->name[0] || !preset->key[0]) {
        if (receiver->on_error)
            receiver->on_error(receiver, 0, "Preset metadata is invalid");
        return false;
    }
    if (!receiver->begin_preset(receiver, preset->name, preset->key))
        return false;
    if (receiver->add_plugin_id)
        receiver->add_plugin_id(receiver, &plugin_id);
    if (receiver->set_flags && preset->is_factory)
        receiver->set_flags(receiver,
                            CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT);
    if (receiver->add_creator && owned->generation->vendor[0])
        receiver->add_creator(receiver, owned->generation->vendor);
    if (receiver->set_description && preset->description[0])
        receiver->set_description(receiver, preset->description);
    if (receiver->add_feature) {
        receiver->add_feature(receiver, CLAP_PLUGIN_FEATURE_INSTRUMENT);
        receiver->add_feature(receiver, CLAP_PLUGIN_FEATURE_SYNTHESIZER);
        for (uint32_t item = 0; item < preset->feature_count; ++item)
            receiver->add_feature(receiver, preset->features[item]);
    }
    return true;
}

static const void *CLAP_ABI recoil_preset_provider_get_extension(
    const clap_preset_discovery_provider_t *provider,
    const char *extension_id) {
    (void)provider;
    (void)extension_id;
    return NULL;
}

static uint32_t CLAP_ABI recoil_preset_factory_count(
    const clap_preset_discovery_factory_t *factory) {
    (void)factory;
    recoil_clap_preset_generation *generation = recoil_preset_active();
    return generation ? generation->count : 0;
}

static const clap_preset_discovery_provider_descriptor_t *CLAP_ABI
recoil_preset_factory_get_descriptor(
    const clap_preset_discovery_factory_t *factory, uint32_t index) {
    (void)factory;
    recoil_clap_preset_generation *generation = recoil_preset_active();
    return generation && index < generation->count
               ? &generation->presets[index].descriptor : NULL;
}

static const clap_preset_discovery_provider_t *CLAP_ABI
recoil_preset_factory_create(
    const clap_preset_discovery_factory_t *factory,
    const clap_preset_discovery_indexer_t *indexer,
    const char *provider_id) {
    (void)factory;
    if (!indexer || !provider_id)
        return NULL;
    recoil_clap_preset_generation *generation = recoil_preset_active();
    if (!generation)
        return NULL;
    uint32_t preset_index = 0;
    while (preset_index < generation->count &&
           strcmp(provider_id,
                  generation->presets[preset_index].provider_id) != 0)
        ++preset_index;
    if (preset_index >= generation->count)
        return NULL;
    recoil_clap_preset_provider *owned = calloc(1, sizeof(*owned));
    if (!owned)
        return NULL;
    owned->indexer = indexer;
    owned->generation = generation;
    owned->preset_index = preset_index;
    owned->provider.desc = &generation->presets[preset_index].descriptor;
    owned->provider.provider_data = owned;
    owned->provider.init = recoil_preset_provider_init;
    owned->provider.destroy = recoil_preset_provider_destroy;
    owned->provider.get_metadata = recoil_preset_provider_get_metadata;
    owned->provider.get_extension = recoil_preset_provider_get_extension;
    return &owned->provider;
}

static const clap_preset_discovery_factory_t recoil_preset_factory = {
    .count = recoil_preset_factory_count,
    .get_descriptor = recoil_preset_factory_get_descriptor,
    .create = recoil_preset_factory_create,
};

static bool CLAP_ABI recoil_preset_load_from_location(
    const clap_plugin_t *plugin,
    uint32_t location_kind,
    const char *location,
    const char *load_key) {
    if (!plugin || !plugin->plugin_data || !load_key)
        return false;
    if (location_kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN &&
        location_kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE)
        return false;
    if (location_kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && location)
        return false;
    const bool loaded = recoil_clap_plugin_load_preset(plugin->plugin_data, load_key,
                                                       location_kind, location);
    return loaded;
}

static const clap_plugin_preset_load_t recoil_preset_load = {
    .from_location = recoil_preset_load_from_location,
};

void recoil_clap_preset_discovery_init(const char *id,
                                       const char *name,
                                       const char *vendor,
                                       uint32_t preset_count) {
    if (!id || !name || !vendor)
        return;
    recoil_clap_preset_generation *generation = calloc(1, sizeof(*generation));
    if (!generation)
        return;
    if (!recoil_clap_copy_text(generation->plugin_id,
                               sizeof(generation->plugin_id), id) ||
        !recoil_clap_copy_text(generation->plugin_name,
                               sizeof(generation->plugin_name), name) ||
        !recoil_clap_copy_text(generation->vendor,
                               sizeof(generation->vendor), vendor)) {
        free(generation);
        return;
    }
    generation->count = preset_count <= RECOIL_CLAP_FACTORY_PRESET_CAPACITY
                            ? preset_count : 0;
    generation->older = recoil_preset_generations;
    recoil_preset_generations = generation;
    atomic_store_explicit(&recoil_preset_current, generation, memory_order_release);
    for (uint32_t index = 0; index < generation->count; ++index) {
        recoil_clap_cached_preset *preset = &generation->presets[index];
        preset->is_factory = true;
        preset->location_kind = CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN;
        const int written = snprintf(preset->provider_id,
                                     sizeof(preset->provider_id),
                                     "%s.preset.%u", generation->plugin_id, index);
        if (written < 0 || (size_t)written >= sizeof(preset->provider_id)) {
            generation->count = 0;
            return;
        }
        preset->descriptor.clap_version = CLAP_VERSION;
        preset->descriptor.id = preset->provider_id;
        preset->descriptor.name = generation->plugin_name;
        preset->descriptor.vendor = generation->vendor;
    }
}

bool recoil_clap_preset_discovery_add(uint32_t index,
                                      const char *name,
                                      const char *key,
                                      const char *description) {
    recoil_clap_preset_generation *generation = recoil_preset_active();
    if (!generation || index >= generation->count)
        return false;
    recoil_clap_cached_preset *preset = &generation->presets[index];
    return recoil_clap_copy_text(preset->name, sizeof(preset->name), name) &&
        recoil_clap_copy_text(preset->key, sizeof(preset->key), key) &&
        recoil_clap_copy_text(preset->description,
                              sizeof(preset->description), description);
}

bool recoil_clap_preset_discovery_add_user(uint32_t index,
                                           const char *name,
                                           const char *key,
                                           const char *description,
                                           const char *file_path) {
    recoil_clap_preset_generation *generation = recoil_preset_active();
    if (!generation || index >= generation->count)
        return false;
    recoil_clap_cached_preset *preset = &generation->presets[index];
    if (!recoil_clap_copy_text(preset->name, sizeof(preset->name), name) ||
        !recoil_clap_copy_text(preset->key, sizeof(preset->key), key) ||
        !recoil_clap_copy_text(preset->description,
                               sizeof(preset->description), description) ||
        !recoil_clap_copy_text(preset->location, sizeof(preset->location), file_path))
        return false;
    preset->is_factory = false;
    preset->location_kind = CLAP_PRESET_DISCOVERY_LOCATION_FILE;
    return true;
}

bool recoil_clap_preset_discovery_add_feature(uint32_t index,
                                              const char *feature) {
    recoil_clap_preset_generation *generation = recoil_preset_active();
    if (!generation || index >= generation->count)
        return false;
    recoil_clap_cached_preset *preset = &generation->presets[index];
    if (preset->feature_count >= RECOIL_CLAP_FACTORY_FEATURE_CAPACITY)
        return false;
    char *destination = preset->features[preset->feature_count];
    if (!recoil_clap_copy_text(destination, CLAP_NAME_SIZE, feature))
        return false;
    ++preset->feature_count;
    return true;
}

void *recoil_clap_preset_discovery_factory(void) {
    return (void *)&recoil_preset_factory;
}

void *recoil_clap_preset_load_extension(void) {
    return (void *)&recoil_preset_load;
}

/* Each synth supplies this narrow adapter boundary in its CLAP target. */
extern void *recoil_clap_editor_create(void *instance, void *core);
extern void recoil_clap_editor_destroy(void *editor);
extern bool recoil_clap_editor_attach(void *editor, void *native_parent,
                                      int width, int height);
extern void recoil_clap_editor_detach(void *editor);
extern void recoil_clap_editor_tick(void *editor);
/* Told when the committed factory preset changes, so an editor can write its
   own caption instead of polling for it.  `index` is -1 for a patch that is
   not a factory preset -- a restored state, or one edited away from its
   preset. */
extern void recoil_clap_editor_preset_changed(void *editor, int32_t index);

struct recoil_clap_gui_controller {
    const clap_host_t *host;
    const clap_host_timer_support_t *timer_support;
    void *editor;
    void *parent;
    uint32_t width;
    uint32_t height;
    clap_id timer_id;
    bool attached;
    bool shown;
    bool timer_active;
    bool callback_fallback;
    bool in_callback;
    uint64_t last_callback_tick_ns;
};

int64_t recoil_clap_monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (int64_t)now.tv_sec * INT64_C(1000000000) +
           (int64_t)now.tv_nsec;
}

const char *recoil_clap_gui_platform_api(void) {
#if defined(__APPLE__)
    return CLAP_WINDOW_API_COCOA;
#elif defined(__linux__)
    return CLAP_WINDOW_API_X11;
#else
    return NULL;
#endif
}

bool recoil_clap_gui_is_api_supported(const char *api, bool is_floating) {
    const char *platform_api = recoil_clap_gui_platform_api();
    return !is_floating && api && platform_api && strcmp(api, platform_api) == 0;
}

static void recoil_clap_gui_stop_scheduling(recoil_clap_gui_controller *controller) {
    if (!controller)
        return;
    controller->callback_fallback = false;
    controller->in_callback = false;
    if (controller->timer_active && controller->timer_support &&
        controller->timer_support->unregister_timer) {
        controller->timer_support->unregister_timer(controller->host,
                                                    controller->timer_id);
    }
    controller->timer_active = false;
    controller->timer_id = CLAP_INVALID_ID;
}

static void recoil_clap_gui_request_fallback(recoil_clap_gui_controller *controller) {
    if (controller && controller->shown && controller->callback_fallback &&
        controller->host && controller->host->request_callback) {
        controller->host->request_callback(controller->host);
    }
}

static bool recoil_clap_gui_attach(recoil_clap_gui_controller *controller) {
    if (!controller || !controller->editor || !controller->parent)
        return false;
    if (controller->attached)
        return true;
    if (!recoil_clap_editor_attach(controller->editor, controller->parent,
                                   (int)controller->width,
                                   (int)controller->height))
        return false;
    controller->attached = true;
    return true;
}

recoil_clap_gui_controller *recoil_clap_gui_create(const clap_host_t *host,
                                                   void *instance,
                                                   void *core,
                                                   uint32_t width,
                                                   uint32_t height) {
    if (!host || !instance || !core || !width || !height ||
        !recoil_clap_gui_platform_api())
        return NULL;

    recoil_clap_gui_controller *controller = calloc(1, sizeof(*controller));
    if (!controller)
        return NULL;
    controller->host = host;
    controller->width = width;
    controller->height = height;
    controller->timer_id = CLAP_INVALID_ID;
    controller->editor = recoil_clap_editor_create(instance, core);
    if (!controller->editor) {
        free(controller);
        return NULL;
    }
    if (host->get_extension) {
        controller->timer_support = host->get_extension(host,
                                                        CLAP_EXT_TIMER_SUPPORT);
    }
    return controller;
}

void recoil_clap_gui_destroy(recoil_clap_gui_controller *controller) {
    if (!controller)
        return;
    recoil_clap_gui_hide(controller);
    if (controller->editor) {
        recoil_clap_editor_destroy(controller->editor);
        controller->editor = NULL;
    }
    free(controller);
}

bool recoil_clap_gui_set_parent(recoil_clap_gui_controller *controller,
                                const clap_window_t *window) {
    if (!controller || !window ||
        !recoil_clap_gui_is_api_supported(window->api, false))
        return false;
#if defined(__APPLE__)
    void *parent = window->cocoa;
#elif defined(__linux__)
    void *parent = (void *)(uintptr_t)window->x11;
#else
    void *parent = NULL;
#endif
    if (!parent)
        return false;
    if (controller->attached && controller->parent == parent)
        return true;

    recoil_clap_gui_stop_scheduling(controller);
    if (controller->attached) {
        recoil_clap_editor_detach(controller->editor);
        controller->attached = false;
    }
    controller->shown = false;
    controller->parent = parent;

    /* CLAP requires set_parent() to create the embedded child.  In
     * particular, Bitwig verifies the X11 parent/child relationship before
     * show() is called. */
    return recoil_clap_gui_attach(controller);
}

bool recoil_clap_gui_show(recoil_clap_gui_controller *controller) {
    if (!controller || !controller->editor || !controller->parent)
        return false;
    if (controller->shown)
        return true;
    if (!recoil_clap_gui_attach(controller))
        return false;

    controller->shown = true;
    recoil_clap_editor_tick(controller->editor);
    if (controller->timer_support && controller->timer_support->register_timer &&
        controller->timer_support->register_timer(controller->host, 33,
                                                  &controller->timer_id)) {
        controller->timer_active = true;
    } else {
        controller->callback_fallback = true;
        recoil_clap_gui_request_fallback(controller);
    }
    return true;
}

bool recoil_clap_gui_hide(recoil_clap_gui_controller *controller) {
    if (!controller)
        return true;
    recoil_clap_gui_stop_scheduling(controller);
    if (controller->attached && controller->editor) {
        recoil_clap_editor_detach(controller->editor);
        controller->attached = false;
    }
    controller->shown = false;
    return true;
}

void recoil_clap_gui_on_timer(recoil_clap_gui_controller *controller,
                              clap_id timer_id) {
    if (!controller || !controller->shown || !controller->timer_active ||
        timer_id != controller->timer_id)
        return;
    recoil_clap_editor_tick(controller->editor);
}

/* The fallback re-arms itself because request_callback() has no timer period.
 * Hosts may dispatch it at event-loop speed, so cap editor polling to 30 Hz;
 * otherwise every callback renders the entire custom panel and can monopolize
 * the host's UI thread.  Some hosts -- Bitwig's macOS plugin host among them --
 * dispatch request_callback() synchronously.  Re-arming from inside the
 * callback then recurses host -> on_main_thread -> request_callback -> host ->
 * ... until the thread stack is exhausted (observed: SIGSEGV on the main-thread
 * guard page).
 *
 * Guard against re-entering: a nested on_main_thread() returns without
 * ticking or re-arming, so a synchronous host's chain unwinds after one
 * exchange instead of growing without bound. */
void recoil_clap_gui_on_main_thread(recoil_clap_gui_controller *controller) {
    if (!controller || !controller->shown || !controller->callback_fallback)
        return;
    if (controller->in_callback)
        return;
    controller->in_callback = true;
    const uint64_t now_ns = recoil_clap_monotonic_ns();
    const uint64_t frame_period_ns = UINT64_C(1000000000) / 30;
    if (!controller->last_callback_tick_ns || !now_ns ||
        now_ns - controller->last_callback_tick_ns >= frame_period_ns) {
        controller->last_callback_tick_ns = now_ns;
        recoil_clap_editor_tick(controller->editor);
    }
    recoil_clap_gui_request_fallback(controller);
    controller->in_callback = false;
}

/* Main thread only.  A commit happens on the audio thread, so entry.rcl asks
   the host for a main-thread callback and forwards from there; this is only
   the hop from controller to editor.  A controller with no editor yet, or one
   whose window is closed, simply does not get told -- reattaching seeds the
   caption from the mailbox, which has committed by then. */
void recoil_clap_gui_preset_changed(recoil_clap_gui_controller *controller,
                                    int32_t index) {
    if (!controller || !controller->editor)
        return;
    recoil_clap_editor_preset_changed(controller->editor, index);
}

void recoil_clap_copy_audio(const clap_process_t *proc,
                            const double *src,
                            uint32_t frames,
                            uint32_t offset,
                            double gain,
                            bool bypass) {
    if (!proc || !proc->audio_outputs || proc->audio_outputs_count == 0 || !src)
        return;

    clap_audio_buffer_t *outputs = proc->audio_outputs;
    clap_audio_buffer_t *out = &outputs[0];
    if (!out || !out->data32)
        return;

    float *out_l = out->data32[0];
    if (!out_l)
        return;

    float *out_r = NULL;
    if (out->channel_count > 1 && out->data32[1])
        out_r = out->data32[1];

    for (uint32_t i = 0; i < frames; i++) {
        float sample = bypass ? 0.0f : (float)(src[i] * gain);
        out_l[offset + i] = sample;
        if (out_r)
            out_r[offset + i] = sample;
    }
}

/* Stereo sibling of recoil_clap_copy_audio: two source buffers instead of one
 * duplicated. Kept separate rather than folded in with a NULL src_r, so the
 * mono path stays exactly the code Kapralova and Loos already render through. */
void recoil_clap_copy_audio_stereo(const clap_process_t *proc,
                                   const double *src_l,
                                   const double *src_r,
                                   uint32_t frames,
                                   uint32_t offset,
                                   double gain,
                                   bool bypass) {
    if (!proc || !proc->audio_outputs || proc->audio_outputs_count == 0)
        return;
    if (!src_l || !src_r)
        return;

    clap_audio_buffer_t *out = &proc->audio_outputs[0];
    if (!out || !out->data32)
        return;

    float *out_l = out->data32[0];
    if (!out_l)
        return;

    float *out_r = NULL;
    if (out->channel_count > 1 && out->data32[1])
        out_r = out->data32[1];

    if (bypass) {
        for (uint32_t i = 0; i < frames; i++) {
            out_l[offset + i] = 0.0f;
            if (out_r) out_r[offset + i] = 0.0f;
        }
    } else if (out_r) {
        for (uint32_t i = 0; i < frames; i++) {
            out_l[offset + i] = (float)(src_l[i] * gain);
            out_r[offset + i] = (float)(src_r[i] * gain);
        }
    } else {
        for (uint32_t i = 0; i < frames; i++) {
            out_l[offset + i] = (float)(((src_l[i] + src_r[i]) * 0.5) * gain);
        }
    }
}

bool recoil_clap_stream_write_all(clap_ostream_t *stream,
                                  const void *src,
                                  uint64_t size) {
    if (!stream || !stream->write || !src)
        return false;
    const uint8_t *cursor = (const uint8_t *)src;
    while (size > 0) {
        int64_t written = stream->write(stream, cursor, size);
        if (written <= 0)
            return false;
        cursor += written;
        size -= (uint64_t)written;
    }
    return true;
}

bool recoil_clap_stream_read_all(clap_istream_t *stream,
                                 void *dst,
                                 uint64_t size) {
    if (!stream || !stream->read || !dst)
        return false;
    uint8_t *cursor = (uint8_t *)dst;
    while (size > 0) {
        int64_t read = stream->read(stream, cursor, size);
        if (read <= 0)
            return false;
        cursor += read;
        size -= (uint64_t)read;
    }
    return true;
}

int64_t recoil_clap_stream_read_to_end(clap_istream_t *stream,
                                       void *dst,
                                       uint64_t capacity) {
    if (!stream || !stream->read || !dst || !capacity || capacity > SIZE_MAX)
        return -1;
    uint8_t *cursor = (uint8_t *)dst;
    uint64_t total = 0;
    while (total < capacity) {
        int64_t count = stream->read(stream, cursor + total, capacity - total);
        if (count < 0)
            return -1;
        if (count == 0)
            return (int64_t)total;
        total += (uint64_t)count;
    }
    uint8_t extra;
    return stream->read(stream, &extra, 1) == 0 ? (int64_t)total : -1;
}

int64_t recoil_clap_read_file(const char *path, void *dst, uint64_t capacity) {
    if (!path || !dst || !capacity || capacity > SIZE_MAX)
        return -1;
    FILE *file = fopen(path, "rb");
    if (!file)
        return -1;
    const size_t count = fread(dst, 1, (size_t)capacity, file);
    const int read_error = ferror(file);
    const int extra = !read_error ? fgetc(file) : EOF;
    const int close_error = fclose(file);
    if (read_error || extra != EOF || close_error != 0)
        return -1;
    return (int64_t)count;
}
