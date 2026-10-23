#define _POSIX_C_SOURCE 200809L
/* This library carries its program-rule flavor as a string a stripped binary keeps. */
#define BROM_PROGRAM_EMIT_FLAVOR_TAG
#include "catalog_service.h"
#include "program_map.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct browser_pin {
    uint64_t token; /* zero means the slot is free */
    brom_preset_catalog *generation;
} browser_pin;

struct brom_preset_catalog_service {
    pthread_mutex_t lock;
    char *root;
    brom_preset_catalog_factory_entry *factory_entries;
    brom_preset_catalog_factory_bank factory;
    brom_preset_catalog *current;
    bool refresh_pending;
    browser_pin pins[BROM_PRESET_CATALOG_SERVICE_MAX_BROWSERS];
};

/* Shared by every service so a token from one is never valid in another. */
static _Atomic uint64_t next_token = 1;

#ifdef BROM_PRESET_TESTING
void brom_preset_catalog_service_test_set_next_token(uint64_t next) {
    atomic_store(&next_token, next);
}
#endif

static brom_preset_result allocate_token(uint64_t *out) {
    uint64_t token = atomic_load(&next_token);
    do {
        if (token == 0 || token == UINT64_MAX) return BROM_PRESET_CAPACITY;
    } while (!atomic_compare_exchange_weak(&next_token, &token, token + 1u));
    *out = token;
    return BROM_PRESET_OK;
}

brom_preset_result brom_preset_catalog_service_create(
    const char *root, const brom_preset_catalog_factory_bank *factory,
    brom_preset_catalog_service **out_service) {
    if (!out_service) return BROM_PRESET_INVALID;
    *out_service = NULL;
    if (!root || !*root || !factory || (factory->count && !factory->entries))
        return BROM_PRESET_INVALID;
    brom_preset_catalog_service *service = calloc(1, sizeof(*service));
    if (!service) return BROM_PRESET_CAPACITY;
    service->root = strdup(root);
    if (factory->count) {
        service->factory_entries = malloc((size_t)factory->count * sizeof(*service->factory_entries));
        if (service->factory_entries)
            memcpy(service->factory_entries, factory->entries,
                   (size_t)factory->count * sizeof(*service->factory_entries));
    }
    service->factory.entries = service->factory_entries;
    service->factory.count = factory->count;
    if (!service->root || (factory->count && !service->factory_entries)) {
        free(service->root);
        free(service->factory_entries);
        free(service);
        return BROM_PRESET_CAPACITY;
    }
    const brom_preset_result refreshed =
        brom_preset_catalog_refresh(service->root, &service->factory, &service->current);
    if (refreshed != BROM_PRESET_OK) {
        free(service->root);
        free(service->factory_entries);
        free(service);
        return refreshed;
    }
    pthread_mutex_init(&service->lock, NULL);
    *out_service = service;
    return BROM_PRESET_OK;
}

void brom_preset_catalog_service_destroy(brom_preset_catalog_service *service) {
    if (!service) return;
    for (unsigned i = 0; i < BROM_PRESET_CATALOG_SERVICE_MAX_BROWSERS; ++i)
        if (service->pins[i].token) brom_preset_catalog_release(service->pins[i].generation);
    brom_preset_catalog_release(service->current);
    pthread_mutex_destroy(&service->lock);
    free(service->root);
    free(service->factory_entries);
    free(service);
}

brom_preset_result brom_preset_catalog_service_refresh(brom_preset_catalog_service *service) {
    if (!service) return BROM_PRESET_INVALID;
    brom_preset_catalog *next = NULL;
    /* The configuration is immutable after create, so enumeration needs no lock. */
    const brom_preset_result result =
        brom_preset_catalog_refresh(service->root, &service->factory, &next);
    pthread_mutex_lock(&service->lock);
    if (result != BROM_PRESET_OK) {
        service->refresh_pending = true;
        pthread_mutex_unlock(&service->lock);
        return result;
    }
    brom_preset_catalog *old = service->current;
    service->current = next;
    service->refresh_pending = false;
    pthread_mutex_unlock(&service->lock);
    brom_preset_catalog_release(old); /* pins and mappings keep their own reference */
    return BROM_PRESET_OK;
}

bool brom_preset_catalog_service_refresh_pending(const brom_preset_catalog_service *service) {
    if (!service) return false;
    brom_preset_catalog_service *mutable_service = (brom_preset_catalog_service *)service;
    pthread_mutex_lock(&mutable_service->lock);
    const bool pending = service->refresh_pending;
    pthread_mutex_unlock(&mutable_service->lock);
    return pending;
}

brom_preset_result brom_preset_catalog_service_saved(
    brom_preset_catalog_service *service, const brom_preset_save_completion *completion) {
    (void)completion;
    return brom_preset_catalog_service_refresh(service);
}

brom_preset_result brom_preset_catalog_service_browser_open(
    brom_preset_catalog_service *service, uint64_t *token, uint32_t *count) {
    if (!service || !token || !count) return BROM_PRESET_INVALID;
    pthread_mutex_lock(&service->lock);
    browser_pin *free_pin = NULL;
    for (unsigned i = 0; i < BROM_PRESET_CATALOG_SERVICE_MAX_BROWSERS; ++i)
        if (!service->pins[i].token) { free_pin = &service->pins[i]; break; }
    if (!free_pin) {
        pthread_mutex_unlock(&service->lock);
        return BROM_PRESET_CAPACITY;
    }
    uint64_t issued = 0;
    const brom_preset_result allocated = allocate_token(&issued);
    if (allocated != BROM_PRESET_OK) {
        pthread_mutex_unlock(&service->lock);
        return allocated;
    }
    brom_preset_catalog_retain(service->current);
    free_pin->generation = service->current;
    free_pin->token = issued;
    *token = issued;
    *count = brom_preset_catalog_count(free_pin->generation);
    pthread_mutex_unlock(&service->lock);
    return BROM_PRESET_OK;
}

static browser_pin *find_pin(brom_preset_catalog_service *service, uint64_t token) {
    for (unsigned i = 0; i < BROM_PRESET_CATALOG_SERVICE_MAX_BROWSERS; ++i)
        if (service->pins[i].token == token) return &service->pins[i];
    return NULL;
}

brom_preset_result brom_preset_catalog_service_browser_read(
    brom_preset_catalog_service *service, uint64_t token, uint32_t index,
    brom_preset_catalog_entry *out) {
    if (!service || token == 0 || !out) return BROM_PRESET_INVALID;
    pthread_mutex_lock(&service->lock);
    browser_pin *pin = find_pin(service, token);
    const brom_preset_result result = pin
        ? brom_preset_catalog_read(pin->generation, index, out) : BROM_PRESET_NOT_FOUND;
    pthread_mutex_unlock(&service->lock);
    return result;
}

brom_preset_result brom_preset_catalog_service_browser_close(
    brom_preset_catalog_service *service, uint64_t token) {
    if (!service || token == 0) return BROM_PRESET_INVALID;
    pthread_mutex_lock(&service->lock);
    browser_pin *pin = find_pin(service, token);
    if (!pin) {
        pthread_mutex_unlock(&service->lock);
        return BROM_PRESET_NOT_FOUND;
    }
    brom_preset_catalog *generation = pin->generation;
    pin->token = 0;
    pin->generation = NULL;
    pthread_mutex_unlock(&service->lock);
    brom_preset_catalog_release(generation);
    return BROM_PRESET_OK;
}

brom_preset_result brom_preset_catalog_service_browser_prepare(
    brom_preset_catalog_service *service, uint64_t token, uint32_t index,
    brom_preset_prepare_identity_fn fn, void *user,
    brom_preset_snapshot *out_snapshot, brom_preset_identity *out_identity) {
    if (!service || token == 0 || !fn || !out_snapshot || !out_identity) return BROM_PRESET_INVALID;
    brom_preset_catalog_entry entry;
    const brom_preset_result found =
        brom_preset_catalog_service_browser_read(service, token, index, &entry);
    if (found != BROM_PRESET_OK) return found;
    /* The callback re-reads disk, so it runs without the service lock. */
    brom_preset_snapshot prepared;
    memset(&prepared, 0, sizeof(prepared));
    const brom_preset_result result = fn(user, &entry.identity, &prepared);
    if (result != BROM_PRESET_OK) return result;
    *out_snapshot = prepared;
    *out_identity = entry.identity;
    return BROM_PRESET_OK;
}

brom_preset_result brom_preset_catalog_service_program_open(
    brom_preset_catalog_service *service, brom_preset_host_mapping **out) {
    if (!service || !out) return BROM_PRESET_INVALID;
    *out = NULL;
    pthread_mutex_lock(&service->lock);
    brom_preset_host_mapping *mapping = brom_preset_host_mapping_create(service->current);
    pthread_mutex_unlock(&service->lock);
    if (!mapping) return BROM_PRESET_CAPACITY;
    *out = mapping;
    return BROM_PRESET_OK;
}

uint32_t brom_preset_catalog_service_program_slot_count(const brom_preset_host_mapping *mapping) {
    const uint32_t count = brom_preset_host_mapping_count(mapping);
    return count > BROM_PRESET_PROGRAM_MAX_SLOTS ? BROM_PRESET_PROGRAM_MAX_SLOTS : count;
}

BROM_PROGRAM_STATIC_ASSERT(BROM_PRESET_PROGRAM_MAX_SLOTS <= BROM_PROGRAM_AU_MAX_COUNT,
                           "the AU decode bias is proven for at most 512 slots");

brom_preset_result brom_preset_catalog_service_program_normalize_for_count(
    uint32_t count, uint32_t slot, double *normalized) {
    if (!normalized || count > BROM_PRESET_PROGRAM_MAX_SLOTS) return BROM_PRESET_INVALID;
    if (slot >= count) return BROM_PRESET_NOT_FOUND;
    *normalized = brom_program_normalized(count, slot);
    return BROM_PRESET_OK;
}

brom_preset_result brom_preset_catalog_service_program_slot_for_count(
    uint32_t count, double normalized, uint32_t *slot) {
    if (!slot || count > BROM_PRESET_PROGRAM_MAX_SLOTS) return BROM_PRESET_INVALID;
    if (!brom_program_value_usable(normalized) || normalized < 0.0 || normalized > 1.0)
        return BROM_PRESET_INVALID;
    if (count == 0) return BROM_PRESET_NOT_FOUND;
    *slot = brom_program_index(count, normalized);
    return BROM_PRESET_OK;
}

brom_preset_result brom_preset_catalog_service_program_normalize(
    const brom_preset_host_mapping *mapping, uint32_t slot, double *normalized) {
    if (!mapping || !normalized) return BROM_PRESET_INVALID;
    return brom_preset_catalog_service_program_normalize_for_count(
        brom_preset_catalog_service_program_slot_count(mapping), slot, normalized);
}

brom_preset_result brom_preset_catalog_service_program_slot(
    const brom_preset_host_mapping *mapping, double normalized, uint32_t *slot) {
    if (!mapping || !slot) return BROM_PRESET_INVALID;
    return brom_preset_catalog_service_program_slot_for_count(
        brom_preset_catalog_service_program_slot_count(mapping), normalized, slot);
}
