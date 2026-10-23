#ifndef BROM_PLUGIN_PRESET_CATALOG_H
#define BROM_PLUGIN_PRESET_CATALOG_H

#include "api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct brom_preset_catalog_factory_entry {
    brom_preset_identity identity;
} brom_preset_catalog_factory_entry;

typedef struct brom_preset_catalog_factory_bank {
    const brom_preset_catalog_factory_entry *entries;
    uint32_t count;
} brom_preset_catalog_factory_bank;

typedef struct brom_preset_catalog brom_preset_catalog;
typedef struct brom_preset_host_mapping brom_preset_host_mapping;

brom_preset_result brom_preset_catalog_refresh(
    const char *root, const brom_preset_catalog_factory_bank *factory,
    brom_preset_catalog **out_generation);
void brom_preset_catalog_retain(brom_preset_catalog *generation);
void brom_preset_catalog_release(brom_preset_catalog *generation);
uint32_t brom_preset_catalog_count(const brom_preset_catalog *generation);
brom_preset_result brom_preset_catalog_read(
    const brom_preset_catalog *generation, uint32_t index,
    brom_preset_catalog_entry *out_entry);

/* A host mapping pins the generation and therefore preserves every old slot. */
brom_preset_host_mapping *brom_preset_host_mapping_create(
    brom_preset_catalog *generation);
void brom_preset_host_mapping_release(brom_preset_host_mapping *mapping);
uint32_t brom_preset_host_mapping_count(const brom_preset_host_mapping *mapping);
brom_preset_result brom_preset_host_mapping_read(
    const brom_preset_host_mapping *mapping, uint32_t slot,
    brom_preset_catalog_entry *out_entry);

#ifdef __cplusplus
}
#endif
#endif
