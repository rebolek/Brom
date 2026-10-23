#ifndef BROM_PLUGIN_PRESET_CATALOG_SERVICE_H
#define BROM_PLUGIN_PRESET_CATALOG_SERVICE_H

#include "catalog.h"
#include "save.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Owns the current catalog generation, opaque browser pins and the generation a
 * host program list is pinned to. Everything here runs off the audio thread.
 * A browser keeps the rows it opened with, whatever refresh does afterwards, and
 * a program mapping keeps its slots for as long as it is retained. */

#define BROM_PRESET_CATALOG_SERVICE_MAX_BROWSERS 16u
/* A host program list exposes at most this many stable slots. */
#define BROM_PRESET_PROGRAM_MAX_SLOTS 128u

typedef struct brom_preset_catalog_service brom_preset_catalog_service;

/* Resolves one catalog identity into a complete validated snapshot, re-reading its
 * source. It returns BROM_PRESET_NOT_FOUND when the source is gone. */
typedef brom_preset_result (*brom_preset_prepare_identity_fn)(
    void *user, const brom_preset_identity *identity, brom_preset_snapshot *out);

/* Copies its configuration and takes the first generation. */
brom_preset_result brom_preset_catalog_service_create(
    const char *root, const brom_preset_catalog_factory_bank *factory,
    brom_preset_catalog_service **out_service);
/* Releases every pin. */
void brom_preset_catalog_service_destroy(brom_preset_catalog_service *service);

/* Atomically replaces only the current generation. A failed refresh keeps the
 * previous one and leaves refresh_pending set until a later refresh succeeds. */
brom_preset_result brom_preset_catalog_service_refresh(brom_preset_catalog_service *service);
bool brom_preset_catalog_service_refresh_pending(const brom_preset_catalog_service *service);
/* Call after a durable save installed a file. The completion's generation does not
 * matter: a save that is stale for the current preset still changed the catalog. */
brom_preset_result brom_preset_catalog_service_saved(
    brom_preset_catalog_service *service, const brom_preset_save_completion *completion);

/* Tokens are nonzero and unique across the process. The 17th open fails with
 * BROM_PRESET_CAPACITY; nothing is ever evicted. */
brom_preset_result brom_preset_catalog_service_browser_open(
    brom_preset_catalog_service *service, uint64_t *token, uint32_t *count);
brom_preset_result brom_preset_catalog_service_browser_read(
    brom_preset_catalog_service *service, uint64_t token, uint32_t index,
    brom_preset_catalog_entry *out);
brom_preset_result brom_preset_catalog_service_browser_close(
    brom_preset_catalog_service *service, uint64_t token);
/* Resolves the identity the browser opened with at `index` (never a fresh row
 * lookup) and calls fn off the lock. On any failure the outputs are untouched. */
brom_preset_result brom_preset_catalog_service_browser_prepare(
    brom_preset_catalog_service *service, uint64_t token, uint32_t index,
    brom_preset_prepare_identity_fn fn, void *user,
    brom_preset_snapshot *out_snapshot, brom_preset_identity *out_identity);

/* A separately retained mapping of the current generation. Release it with
 * brom_preset_host_mapping_release. */
brom_preset_result brom_preset_catalog_service_program_open(
    brom_preset_catalog_service *service, brom_preset_host_mapping **out);
/* min(entries, BROM_PRESET_PROGRAM_MAX_SLOTS): the slots a host list exposes. Zero
 * means no program parameter. */
uint32_t brom_preset_catalog_service_program_slot_count(const brom_preset_host_mapping *mapping);
/* One slot normalizes to 0.0; otherwise slot / (count - 1). */
brom_preset_result brom_preset_catalog_service_program_normalize(
    const brom_preset_host_mapping *mapping, uint32_t slot, double *normalized);
/* The decode of a finite value in 0..1 ("finite" is a bit test: the SDK builds with
 * -ffast-math). The VST3 rule round(value * (count - 1)) is the default. Defining
 * BROM_PRESET_PROGRAM_AU_WRAPPER when compiling selects the AU rule
 * min(count - 1, floor(value * count + 1/1024)): the VST3 SDK's list decode, tolerant
 * of both i / (count - 1) and the AUv2 wrapper's single-precision i / count. Both
 * rules live in program_map.h; a binary uses exactly one flavor. */
brom_preset_result brom_preset_catalog_service_program_slot(
    const brom_preset_host_mapping *mapping, double normalized, uint32_t *slot);

/* The same rules for a list whose count is already pinned (the processor holds a
 * copied table, not a mapping). `count` is the slot count, at most
 * BROM_PRESET_PROGRAM_MAX_SLOTS. */
brom_preset_result brom_preset_catalog_service_program_normalize_for_count(
    uint32_t count, uint32_t slot, double *normalized);
brom_preset_result brom_preset_catalog_service_program_slot_for_count(
    uint32_t count, double normalized, uint32_t *slot);

#ifdef BROM_PRESET_TESTING
void brom_preset_catalog_service_test_set_next_token(uint64_t next);
#endif

#ifdef __cplusplus
}
#endif
#endif
