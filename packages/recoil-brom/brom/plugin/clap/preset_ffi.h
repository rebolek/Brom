#ifndef BROM_PLUGIN_CLAP_PRESET_FFI_H
#define BROM_PLUGIN_CLAP_PRESET_FFI_H

#include <stdint.h>
#include <stdbool.h>

/* Rebol emits the concrete ABI structs in the CLAP entry translation unit.
 * Keep this shim limited to pointer-based declarations so it does not import
 * duplicate struct definitions from the native preset headers. */
void *brom_preset_tx_create(void *, void *, uint32_t);
void brom_preset_tx_destroy(void *);
int32_t brom_preset_submit_load(void *, void *, void *, void *);
int32_t brom_preset_submit_restore(void *, void *, void *);
int32_t brom_preset_submit_compare(void *, uint64_t);
int32_t brom_preset_submit_edit(void *, uint64_t, uint32_t, double);
int32_t brom_preset_save_ack_submit(void *, const void *);
int32_t brom_preset_audio_edit(void *, uint32_t, double, bool, void *);
uint64_t brom_preset_audio_cutoff(void *);
bool brom_preset_audio_boundary(void *, uint64_t, void *);
void brom_preset_audio_publish(void *, void *);
void brom_preset_audio_publish_inactive(void *, void *);
bool brom_preset_audio_resume(void *, void *);
void brom_preset_set_active(void *, bool);
bool brom_preset_read_view(void *, void *);
int32_t brom_preset_session_encode(void *, const char *, void *, void *, uint64_t, void *);
int32_t brom_preset_session_decode(void *, uint64_t, void *, const char *, void *);
int32_t brom_preset_session_migrate_legacy(void *, void *, void *, void *);

int32_t brom_preset_catalog_refresh(const char *, void *, void **);
void brom_preset_catalog_release(void *);
uint32_t brom_preset_catalog_count(void *);
int32_t brom_preset_catalog_read(void *, uint32_t, void *);

#endif
