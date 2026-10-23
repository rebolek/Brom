#ifndef BROM_PLUGIN_PRESET_TRANSACTION_H
#define BROM_PLUGIN_PRESET_TRANSACTION_H

#include "api.h"
#include "save.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What a request became at the boundary that consumed it. Outcomes are additive
 * to brom_preset_result: they are not result values, and brom_preset_commit is
 * unchanged. DEFERRED is not final; the same tag reports again at a later
 * boundary. */
typedef enum brom_preset_event_kind {
    BROM_PRESET_EVENT_LOAD = 1,
    BROM_PRESET_EVENT_RESTORE = 2,
    BROM_PRESET_EVENT_COMPARE = 3,
    BROM_PRESET_EVENT_EDIT = 4,
    BROM_PRESET_EVENT_SAVE_ACK = 5
} brom_preset_event_kind;

typedef enum brom_preset_outcome {
    BROM_PRESET_OUTCOME_APPLIED = 1,
    BROM_PRESET_OUTCOME_STALE = 2,
    BROM_PRESET_OUTCOME_SUPERSEDED = 3,
    /* Discarded without effect for a reason other than staleness (for example a
     * duplicate save ticket); `reason` says which. */
    BROM_PRESET_OUTCOME_DROPPED = 4,
    /* An inactive restore was already visible when it was submitted. */
    BROM_PRESET_OUTCOME_ALREADY_APPLIED = 5,
    BROM_PRESET_OUTCOME_DEFERRED = 6
} brom_preset_outcome;

typedef struct brom_preset_outcome_record {
    uint64_t tag;
    brom_preset_event_kind event;
    brom_preset_outcome outcome;
    uint64_t generation;
    brom_preset_result reason;
} brom_preset_outcome_record;

brom_preset_tx *brom_preset_tx_create(brom_preset_schema *, brom_preset_view *, uint32_t);
void brom_preset_tx_destroy(brom_preset_tx *);
brom_preset_result brom_preset_submit_load(brom_preset_tx *, brom_preset_snapshot *, brom_preset_identity *, uint64_t *);
brom_preset_result brom_preset_submit_restore(brom_preset_tx *, brom_preset_view *, uint64_t *);
brom_preset_result brom_preset_submit_compare(brom_preset_tx *, uint64_t);
brom_preset_result brom_preset_submit_edit(brom_preset_tx *, uint64_t, uint32_t, double);
brom_preset_result brom_preset_save_ack_submit(brom_preset_tx *, const brom_preset_save_completion *);
brom_preset_result brom_preset_read_completion(brom_preset_tx *, uint64_t, brom_preset_result *);
brom_preset_result brom_preset_audio_edit(brom_preset_tx *, uint32_t, double, bool, bool *);
uint64_t brom_preset_audio_cutoff(brom_preset_tx *);
bool brom_preset_audio_boundary(brom_preset_tx *, uint64_t, brom_preset_commit *);
/* Tagged submits carry a nonzero caller tag that comes back in the outcome
 * records. `completed` reports a request already resolved at submit time (an
 * inactive restore or save acknowledgment); a save acknowledgment then also
 * fills `immediate`. */
brom_preset_result brom_preset_submit_load_tagged(brom_preset_tx *, brom_preset_snapshot *, brom_preset_identity *, uint64_t tag, uint64_t *);
brom_preset_result brom_preset_submit_restore_tagged(brom_preset_tx *, brom_preset_view *, uint64_t tag, uint64_t *, bool *completed);
brom_preset_result brom_preset_submit_compare_tagged(brom_preset_tx *, uint64_t tag, uint64_t generation);
brom_preset_result brom_preset_submit_edit_tagged(brom_preset_tx *, uint64_t tag, uint64_t generation, uint32_t, double);
brom_preset_result brom_preset_save_ack_submit_tagged(brom_preset_tx *, const brom_preset_save_completion *, uint64_t tag, bool *completed, brom_preset_outcome_record *immediate);
/* Audio thread, no allocation and no lock. Makes an already validated, prepared
 * snapshot the audio-side view at once, under a freshly minted generation: every
 * request, deferred edit and save completion bound to an older generation then
 * completes STALE. It publishes without re-applying: the next boundary publishes
 * the view without apply or reset unless something else changed. The caller
 * applies the snapshot to the DSP itself. */
brom_preset_result brom_preset_audio_apply_prepared(brom_preset_tx *, const brom_preset_snapshot *, const brom_preset_identity *);
/* The smallest outcome buffer a boundary can never overflow. */
uint32_t brom_preset_boundary_outcome_capacity(const brom_preset_tx *);
/* The boundary with per-request outcomes. It stops consuming before the buffer
 * would overflow, so an undersized buffer delays work instead of losing it.
 * Outcomes are filled even when the boundary returns false. */
bool brom_preset_audio_boundary_ex(brom_preset_tx *, uint64_t cutoff, brom_preset_commit *, brom_preset_outcome_record *outcomes, uint32_t capacity, uint32_t *count);
void brom_preset_audio_publish(brom_preset_tx *, brom_preset_commit *);
void brom_preset_audio_publish_inactive(brom_preset_tx *, brom_preset_commit *);
bool brom_preset_read_view(brom_preset_tx *, brom_preset_view *);
void brom_preset_set_active(brom_preset_tx *, bool);
bool brom_preset_audio_resume(brom_preset_tx *, brom_preset_commit *);

#ifdef BROM_PRESET_TESTING
/* Called inside a submitter's critical section, between reserving its queue
 * position and minting its generation. */
typedef void (*brom_preset_tx_test_hook)(brom_preset_tx *tx, void *user);
void brom_preset_tx_test_set_submit_hook(brom_preset_tx_test_hook hook, void *user);
/* Makes a contended submit return BROM_PRESET_BUSY instead of waiting. */
void brom_preset_tx_test_set_try_only_lock(bool enabled);
#endif

#ifdef __cplusplus
}
#endif
#endif
