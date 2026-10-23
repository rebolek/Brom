#ifndef BROM_PLUGIN_PRESET_TRANSACTION_H
#define BROM_PLUGIN_PRESET_TRANSACTION_H

#include "api.h"

#ifdef __cplusplus
extern "C" {
#endif

brom_preset_tx *brom_preset_tx_create(brom_preset_schema *, brom_preset_view *, uint32_t);
void brom_preset_tx_destroy(brom_preset_tx *);
brom_preset_result brom_preset_submit_load(brom_preset_tx *, brom_preset_snapshot *, brom_preset_identity *, uint64_t *);
brom_preset_result brom_preset_submit_restore(brom_preset_tx *, brom_preset_view *, uint64_t *);
brom_preset_result brom_preset_submit_compare(brom_preset_tx *, uint64_t);
brom_preset_result brom_preset_submit_edit(brom_preset_tx *, uint64_t, uint32_t, double);
brom_preset_result brom_preset_read_completion(brom_preset_tx *, uint64_t, brom_preset_result *);
brom_preset_result brom_preset_audio_edit(brom_preset_tx *, uint32_t, double, bool, bool *);
uint64_t brom_preset_audio_cutoff(brom_preset_tx *);
bool brom_preset_audio_boundary(brom_preset_tx *, uint64_t, brom_preset_commit *);
void brom_preset_audio_publish(brom_preset_tx *, brom_preset_commit *);
bool brom_preset_read_view(brom_preset_tx *, brom_preset_view *);
void brom_preset_set_active(brom_preset_tx *, bool);
bool brom_preset_audio_resume(brom_preset_tx *, brom_preset_commit *);

#ifdef __cplusplus
}
#endif
#endif
