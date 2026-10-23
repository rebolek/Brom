#define _POSIX_C_SOURCE 200809L
#include "transaction.h"

#include <math.h>
#include <sched.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef enum event_kind { EVENT_LOAD, EVENT_RESTORE, EVENT_COMPARE, EVENT_EDIT, EVENT_SAVE_ACK } event_kind;

typedef struct tx_event {
    event_kind kind;
    uint64_t generation;
    uint32_t id;
    double value;
    brom_preset_snapshot snapshot;
    brom_preset_identity identity;
    brom_preset_view view;
    brom_preset_save_completion save_completion;
    uint64_t tag;
    /* Decided under submission, before the event is published: an inactive
     * restore is already the visible view, so its queued copy is not stale. */
    bool already_applied;
} tx_event;

typedef struct tx_cell {
    _Atomic uint64_t turn;
    tx_event event;
} tx_cell;

typedef struct view_slot {
    _Atomic uint32_t readers;
    brom_preset_view view;
} view_slot;

typedef struct result_slot {
    _Atomic uint64_t generation;
    _Atomic int result;
} result_slot;

struct brom_preset_tx {
    brom_preset_schema schema;
    tx_cell *queue;
    uint32_t capacity;
    uint32_t mask;
    /* The next generation to mint. Submission and apply_prepared both advance it,
     * so a generation no longer depends on a queue position. */
    _Atomic uint64_t next_generation;
    /* Taken by submitters only, never by the audio thread, so a generation and the
     * queue position it travels with are ordered together. */
    atomic_flag submit_lock;
    _Atomic uint64_t enqueue_pos;
    _Atomic uint64_t dequeue_pos;
    _Atomic uint64_t accepted_generation;
    _Atomic uint64_t cutoff_epoch;
    _Atomic uint32_t save_retry_state;
    _Atomic bool active;
    result_slot *results;
    view_slot published[3];
    _Atomic uint32_t published_index;
    brom_preset_view audio_view;
    brom_preset_view inactive_view;
    tx_event *deferred;
    brom_preset_save_completion save_retry;
    uint64_t save_retry_epoch;
    uint64_t save_retry_tag;
    _Atomic uint64_t inactive_sequence;
    bool inactive_apply_parameters;
    bool inactive_reset_voices;
    uint32_t deferred_count;
    uint64_t completion_to_ack;
    uint64_t latest_save_ticket_id;
    bool auto_return_pending;
    bool audio_edit_dirty;
    /* Audio-owned. A program change replaced the view and is waiting to be
     * published; the boundary then neither re-applies nor resets. */
    bool publish_only;
    /* Tags of deferred edits that a program change made stale, reported by the
     * next boundary. At most one boundary's deferred edits can be waiting. */
    uint64_t *stale_tags;
    uint32_t stale_count;
    _Atomic bool inactive_pending;
};

#ifdef BROM_PRESET_TESTING
static brom_preset_tx_test_hook submit_hook;
static void *submit_hook_user;
static bool try_only_lock;
void brom_preset_tx_test_set_submit_hook(brom_preset_tx_test_hook hook, void *user) {
    submit_hook = hook;
    submit_hook_user = user;
}
void brom_preset_tx_test_set_try_only_lock(bool enabled) { try_only_lock = enabled; }
#endif

static bool lock_submit(brom_preset_tx *tx) {
#ifdef BROM_PRESET_TESTING
    if (try_only_lock)
        return !atomic_flag_test_and_set_explicit(&tx->submit_lock, memory_order_acquire);
#endif
    while (atomic_flag_test_and_set_explicit(&tx->submit_lock, memory_order_acquire))
        sched_yield();
    return true;
}

static void unlock_submit(brom_preset_tx *tx) {
    atomic_flag_clear_explicit(&tx->submit_lock, memory_order_release);
}

static uint32_t next_power_of_two(uint32_t value) {
    uint32_t result = 2;
    while (result < value && result <= UINT32_MAX / 2) result <<= 1;
    return result;
}

static bool identity_valid(const brom_preset_identity *identity) {
    return identity && memchr(identity->key, 0, sizeof(identity->key)) &&
        memchr(identity->display_name, 0, sizeof(identity->display_name)) &&
        identity->key[0] != 0 && identity->display_name[0] != 0;
}

static bool schema_valid(const brom_preset_schema *schema) {
    if (!schema || schema->count == 0 || schema->count > BROM_PRESET_MAX_PARAMS) return false;
    for (uint32_t i = 0; i < schema->count; ++i) {
        const brom_preset_parameter *p = &schema->parameters[i];
        if (!isfinite(p->minimum) || !isfinite(p->maximum) || !isfinite(p->default_value) ||
            p->minimum > p->maximum || p->default_value < p->minimum || p->default_value > p->maximum)
            return false;
        for (uint32_t j = 0; j < i; ++j)
            if (schema->parameters[j].id == p->id) return false;
        if (p->steps > 1 && p->minimum == p->maximum) return false;
    }
    return true;
}

static bool value_valid(const brom_preset_schema *schema, uint32_t id, double value) {
    if (!schema || !isfinite(value)) return false;
    for (uint32_t i = 0; i < schema->count; ++i) {
        const brom_preset_parameter *p = &schema->parameters[i];
        if (p->id != id) continue;
        if (value < p->minimum || value > p->maximum) return false;
        if (p->steps > 1) {
            double step = (p->maximum - p->minimum) / (double)(p->steps - 1);
            double position = (value - p->minimum) / step;
            if (fabs(position - nearbyint(position)) > 1e-9) return false;
        }
        return true;
    }
    return false;
}

static bool snapshot_valid(const brom_preset_schema *schema, const brom_preset_snapshot *snapshot) {
    if (!schema || !snapshot || schema->count == 0 || schema->count > BROM_PRESET_MAX_PARAMS ||
        snapshot->count != (int32_t)schema->count) return false;
    for (uint32_t i = 0; i < schema->count; ++i)
        if (!value_valid(schema, schema->parameters[i].id, snapshot->values[i])) return false;
    return true;
}

static bool set_value_by_id(const brom_preset_schema *schema,
                            brom_preset_snapshot *snapshot, uint32_t id, double value) {
    for (uint32_t i = 0; i < schema->count; ++i) {
        if (schema->parameters[i].id != id) continue;
        snapshot->values[i] = value;
        return true;
    }
    return false;
}

static bool values_equal(const brom_preset_schema *schema,
                         const brom_preset_snapshot *left,
                         const brom_preset_snapshot *right) {
    for (uint32_t i = 0; i < schema->count; ++i) {
        const brom_preset_parameter *p = &schema->parameters[i];
        double a = left->values[i], b = right->values[i];
        if (p->steps > 1) {
            if (a != b) return false;
        } else if (fabs(a - b) > fmax(1.0, fabs(b)) * 1e-9) return false;
    }
    return true;
}

static bool reserve_position(brom_preset_tx *tx, uint64_t *position_out) {
    uint64_t pos = atomic_load_explicit(&tx->enqueue_pos, memory_order_relaxed);
    for (;;) {
        tx_cell *cell = &tx->queue[pos & tx->mask];
        uint64_t turn = atomic_load_explicit(&cell->turn, memory_order_acquire);
        intptr_t difference = (intptr_t)(turn - pos);
        if (difference == 0) {
            if (atomic_compare_exchange_weak_explicit(&tx->enqueue_pos, &pos, pos + 1,
                    memory_order_relaxed, memory_order_relaxed)) {
                *position_out = pos;
                return true;
            }
        } else if (difference < 0) return false;
        else pos = atomic_load_explicit(&tx->enqueue_pos, memory_order_relaxed);
    }
}

static void publish_position(brom_preset_tx *tx, uint64_t position, const tx_event *event) {
    tx_cell *cell = &tx->queue[position & tx->mask];
    cell->event = *event;
    atomic_store_explicit(&cell->turn, position + 1, memory_order_release);
}

static bool publish_view_only(brom_preset_tx *tx, const brom_preset_view *view) {
    uint32_t current = atomic_load_explicit(&tx->published_index, memory_order_acquire);
    for (uint32_t step = 1; step <= 3; ++step) {
        uint32_t candidate = (current + step) % 3;
        if (candidate == current ||
            atomic_load_explicit(&tx->published[candidate].readers, memory_order_acquire)) continue;
        tx->published[candidate].view = *view;
        atomic_store_explicit(&tx->published_index, candidate, memory_order_release);
        return true;
    }
    return false;
}

static void store_inactive_view(brom_preset_tx *tx, const brom_preset_view *view,
                               bool apply_parameters, bool reset_voices) {
    atomic_fetch_add_explicit(&tx->inactive_sequence, 1, memory_order_acq_rel);
    tx->inactive_view = *view;
    tx->inactive_apply_parameters = apply_parameters;
    tx->inactive_reset_voices = reset_voices;
    atomic_store_explicit(&tx->inactive_pending, true, memory_order_release);
    atomic_fetch_add_explicit(&tx->inactive_sequence, 1, memory_order_release);
    publish_view_only(tx, view);
}

/* While nobody renders, the freshest view is the pending inactive one: it carries
 * restores and acknowledgments that never passed through the audio view. */
static bool read_pending_inactive(brom_preset_tx *tx, brom_preset_view *out) {
    if (!atomic_load_explicit(&tx->inactive_pending, memory_order_acquire)) return false;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        const uint64_t before = atomic_load_explicit(&tx->inactive_sequence, memory_order_acquire);
        if (before & 1u) continue;
        *out = tx->inactive_view;
        if (before == atomic_load_explicit(&tx->inactive_sequence, memory_order_acquire)) return true;
    }
    return false;
}

static bool dequeue_until(brom_preset_tx *tx, uint64_t cutoff, tx_event *out) {
    uint64_t pos = atomic_load_explicit(&tx->dequeue_pos, memory_order_relaxed);
    if (pos >= cutoff) return false;
    tx_cell *cell = &tx->queue[pos & tx->mask];
    if (atomic_load_explicit(&cell->turn, memory_order_acquire) != pos + 1) return false;
    *out = cell->event;
    atomic_store_explicit(&cell->turn, pos + tx->capacity, memory_order_release);
    atomic_store_explicit(&tx->dequeue_pos, pos + 1, memory_order_relaxed);
    return true;
}

brom_preset_tx *brom_preset_tx_create(brom_preset_schema *schema,
                                      brom_preset_view *initial,
                                      uint32_t queue_capacity) {
    if (!schema_valid(schema) || !initial ||
        !snapshot_valid(schema, &initial->working) ||
        !snapshot_valid(schema, &initial->reference) ||
        !snapshot_valid(schema, &initial->baseline) || queue_capacity == 0) return NULL;
    uint32_t capacity = next_power_of_two(queue_capacity);
    if (capacity < queue_capacity) return NULL;
    brom_preset_tx *tx = calloc(1, sizeof(*tx));
    if (!tx) return NULL;
    tx->queue = calloc(capacity, sizeof(*tx->queue));
    tx->deferred = calloc(capacity, sizeof(*tx->deferred));
    tx->results = calloc(capacity, sizeof(*tx->results));
    tx->stale_tags = calloc(capacity, sizeof(*tx->stale_tags));
    if (!tx->queue || !tx->deferred || !tx->results || !tx->stale_tags) {
        free(tx->stale_tags); free(tx->results); free(tx->deferred); free(tx->queue); free(tx);
        return NULL;
    }
    tx->schema = *schema;
    tx->capacity = capacity;
    tx->mask = capacity - 1;
    atomic_init(&tx->next_generation, initial->generation + 1u);
    atomic_flag_clear(&tx->submit_lock);
    for (uint32_t i = 0; i < capacity; ++i) atomic_init(&tx->queue[i].turn, i);
    for (uint32_t i = 0; i < capacity; ++i) {
        atomic_init(&tx->results[i].generation, UINT64_MAX);
        atomic_init(&tx->results[i].result, BROM_PRESET_STALE);
    }
    atomic_init(&tx->enqueue_pos, 0);
    atomic_init(&tx->dequeue_pos, 0);
    atomic_init(&tx->accepted_generation, initial->generation);
    atomic_init(&tx->cutoff_epoch, 0);
    atomic_init(&tx->save_retry_state, 0);
    atomic_init(&tx->inactive_sequence, 0);
    atomic_init(&tx->inactive_pending, false);
    atomic_init(&tx->active, false);
    tx->audio_view = *initial;
    tx->published[0].view = *initial;
    atomic_init(&tx->published_index, 0);
    for (uint32_t i = 0; i < 3; ++i) atomic_init(&tx->published[i].readers, 0);
    return tx;
}

void brom_preset_tx_destroy(brom_preset_tx *tx) {
    if (!tx) return;
    free(tx->stale_tags);
    free(tx->results);
    free(tx->deferred);
    free(tx->queue);
    free(tx);
}

static brom_preset_result submit_full(brom_preset_tx *tx, event_kind kind,
                                      brom_preset_snapshot *snapshot,
                                      brom_preset_identity *identity,
                                      brom_preset_view *view, uint64_t tag,
                                      uint64_t *generation, bool *completed) {
    if (completed) *completed = false;
    if (!tx || !generation) return BROM_PRESET_INVALID;
    if (kind == EVENT_LOAD && (!snapshot_valid(&tx->schema, snapshot) || !identity_valid(identity)))
        return BROM_PRESET_INVALID;
    if (kind == EVENT_RESTORE && (!view || !snapshot_valid(&tx->schema, &view->working) ||
        !snapshot_valid(&tx->schema, &view->reference) || !snapshot_valid(&tx->schema, &view->baseline) ||
        !identity_valid(&view->working_identity) || !identity_valid(&view->reference_identity)))
        return BROM_PRESET_INVALID;
    tx_event event = {.kind = kind, .tag = tag};
    uint64_t position;
    /* One critical section pairs the queue position with its generation. */
    if (!lock_submit(tx)) return BROM_PRESET_BUSY;
    if (!reserve_position(tx, &position)) {
        unlock_submit(tx);
        return BROM_PRESET_CAPACITY;
    }
#ifdef BROM_PRESET_TESTING
    if (submit_hook) submit_hook(tx, submit_hook_user);
#endif
    event.generation = atomic_fetch_add_explicit(&tx->next_generation, 1u, memory_order_acq_rel);
    if (kind == EVENT_LOAD) { event.snapshot = *snapshot; event.identity = *identity; }
    else event.view = *view;
    const bool inactive_restore =
        kind == EVENT_RESTORE && !atomic_load_explicit(&tx->active, memory_order_acquire);
    event.already_applied = inactive_restore;
    result_slot *result = &tx->results[event.generation % tx->capacity];
    atomic_store_explicit(&result->result, BROM_PRESET_BUSY, memory_order_relaxed);
    atomic_store_explicit(&result->generation, event.generation, memory_order_release);
    publish_position(tx, position, &event);
    uint64_t accepted = atomic_load_explicit(&tx->accepted_generation, memory_order_relaxed);
    while (accepted < event.generation && !atomic_compare_exchange_weak_explicit(
            &tx->accepted_generation, &accepted, event.generation,
            memory_order_release, memory_order_relaxed)) {}
    unlock_submit(tx);
    if (inactive_restore) {
        event.view.generation = event.generation;
        store_inactive_view(tx, &event.view, true, true);
        if (completed) *completed = true;
    }
    *generation = event.generation;
    return BROM_PRESET_OK;
}

brom_preset_result brom_preset_submit_load(brom_preset_tx *tx,
        brom_preset_snapshot *snapshot, brom_preset_identity *identity,
        uint64_t *generation) {
    return submit_full(tx, EVENT_LOAD, snapshot, identity, NULL, 0, generation, NULL);
}

brom_preset_result brom_preset_submit_load_tagged(brom_preset_tx *tx,
        brom_preset_snapshot *snapshot, brom_preset_identity *identity,
        uint64_t tag, uint64_t *generation) {
    return submit_full(tx, EVENT_LOAD, snapshot, identity, NULL, tag, generation, NULL);
}

brom_preset_result brom_preset_submit_restore(brom_preset_tx *tx,
        brom_preset_view *view, uint64_t *generation) {
    return submit_full(tx, EVENT_RESTORE, NULL, NULL, view, 0, generation, NULL);
}

brom_preset_result brom_preset_submit_restore_tagged(brom_preset_tx *tx,
        brom_preset_view *view, uint64_t tag, uint64_t *generation, bool *completed) {
    return submit_full(tx, EVENT_RESTORE, NULL, NULL, view, tag, generation, completed);
}

static brom_preset_result submit_simple(brom_preset_tx *tx, tx_event *event,
                                        uint64_t generation) {
    if (!tx) return BROM_PRESET_INVALID;
    if (generation != atomic_load_explicit(&tx->accepted_generation, memory_order_acquire))
        return BROM_PRESET_STALE;
    event->generation = generation;
    uint64_t position;
    if (!reserve_position(tx, &position)) return BROM_PRESET_CAPACITY;
    publish_position(tx, position, event);
    return BROM_PRESET_OK;
}

brom_preset_result brom_preset_submit_compare_tagged(brom_preset_tx *tx, uint64_t tag,
                                                     uint64_t generation) {
    tx_event event = {.kind = EVENT_COMPARE, .tag = tag};
    return submit_simple(tx, &event, generation);
}

brom_preset_result brom_preset_submit_compare(brom_preset_tx *tx, uint64_t generation) {
    return brom_preset_submit_compare_tagged(tx, 0, generation);
}

brom_preset_result brom_preset_submit_edit_tagged(brom_preset_tx *tx, uint64_t tag,
        uint64_t generation, uint32_t id, double value) {
    if (!tx || !value_valid(&tx->schema, id, value)) return BROM_PRESET_INVALID;
    tx_event event = {.kind = EVENT_EDIT, .id = id, .value = value, .tag = tag};
    return submit_simple(tx, &event, generation);
}

brom_preset_result brom_preset_submit_edit(brom_preset_tx *tx, uint64_t generation,
        uint32_t id, double value) {
    return brom_preset_submit_edit_tagged(tx, 0, generation, id, value);
}

static void fill_outcome(brom_preset_outcome_record *record, uint64_t tag,
                         brom_preset_event_kind event, brom_preset_outcome outcome,
                         uint64_t generation, brom_preset_result reason) {
    if (!record) return;
    record->tag = tag;
    record->event = event;
    record->outcome = outcome;
    record->generation = generation;
    record->reason = reason;
}

brom_preset_result brom_preset_save_ack_submit_tagged(
        brom_preset_tx *tx, const brom_preset_save_completion *completion,
        uint64_t tag, bool *completed, brom_preset_outcome_record *immediate) {
    if (completed) *completed = false;
    if (!tx || !completion || !snapshot_valid(&tx->schema, &completion->snapshot) ||
        !identity_valid(&completion->identity)) return BROM_PRESET_INVALID;
    if (!atomic_load_explicit(&tx->active, memory_order_acquire)) {
        const bool had_pending = atomic_load_explicit(&tx->inactive_pending, memory_order_acquire);
        brom_preset_view view = {0};
        if (had_pending) {
            uint64_t before = atomic_load_explicit(&tx->inactive_sequence, memory_order_acquire);
            if (before & 1u) return BROM_PRESET_BUSY;
            view = tx->inactive_view;
            if (before != atomic_load_explicit(&tx->inactive_sequence, memory_order_acquire))
                return BROM_PRESET_BUSY;
        } else if (!brom_preset_read_view(tx, &view)) return BROM_PRESET_BUSY;
        if (completed) *completed = true;
        if (completion->generation != view.generation) {
            fill_outcome(immediate, tag, BROM_PRESET_EVENT_SAVE_ACK, BROM_PRESET_OUTCOME_STALE,
                         completion->generation, BROM_PRESET_STALE);
            return BROM_PRESET_OK;
        }
        if (completion->ticket_id <= tx->latest_save_ticket_id) {
            fill_outcome(immediate, tag, BROM_PRESET_EVENT_SAVE_ACK, BROM_PRESET_OUTCOME_DROPPED,
                         completion->generation, BROM_PRESET_SUPERSEDED);
            return BROM_PRESET_OK;
        }
        view.baseline = completion->snapshot;
        view.working_identity = completion->identity;
        view.changed = !values_equal(&tx->schema, &view.working, &view.baseline);
        tx->latest_save_ticket_id = completion->ticket_id;
        store_inactive_view(tx, &view,
                            had_pending && tx->inactive_apply_parameters,
                            had_pending && tx->inactive_reset_voices);
        fill_outcome(immediate, tag, BROM_PRESET_EVENT_SAVE_ACK, BROM_PRESET_OUTCOME_APPLIED,
                     completion->generation, BROM_PRESET_OK);
        return BROM_PRESET_OK;
    }
    tx_event event = {.kind = EVENT_SAVE_ACK, .generation = completion->generation, .tag = tag};
    event.save_completion = *completion;
    uint64_t position;
    if (!reserve_position(tx, &position)) {
        uint32_t empty = 0;
        if (!atomic_compare_exchange_strong_explicit(&tx->save_retry_state, &empty, 1,
                memory_order_acquire, memory_order_relaxed)) return BROM_PRESET_CAPACITY;
        tx->save_retry = *completion;
        tx->save_retry_tag = tag;
        tx->save_retry_epoch = atomic_load_explicit(&tx->cutoff_epoch, memory_order_acquire) + 1u;
        atomic_store_explicit(&tx->save_retry_state, 2, memory_order_release);
        return BROM_PRESET_OK;
    }
    publish_position(tx, position, &event);
    return BROM_PRESET_OK;
}

brom_preset_result brom_preset_save_ack_submit(
        brom_preset_tx *tx, const brom_preset_save_completion *completion) {
    return brom_preset_save_ack_submit_tagged(tx, completion, 0, NULL, NULL);
}

brom_preset_result brom_preset_read_completion(brom_preset_tx *tx, uint64_t generation,
                                                brom_preset_result *out_result) {
    if (!tx || !out_result) return BROM_PRESET_INVALID;
    result_slot *slot = &tx->results[generation % tx->capacity];
    if (atomic_load_explicit(&slot->generation, memory_order_acquire) != generation)
        return BROM_PRESET_STALE;
    *out_result = (brom_preset_result)atomic_load_explicit(&slot->result, memory_order_acquire);
    return BROM_PRESET_OK;
}

brom_preset_result brom_preset_audio_edit(brom_preset_tx *tx, uint32_t id,
        double value, bool identified_echo, bool *apply_to_dsp) {
    if (!tx || !apply_to_dsp) return BROM_PRESET_INVALID;
    *apply_to_dsp = false;
    if (identified_echo) return BROM_PRESET_OK;
    if (!value_valid(&tx->schema, id, value)) return BROM_PRESET_INVALID;
    if (!set_value_by_id(&tx->schema, &tx->audio_view.working, id, value)) return BROM_PRESET_INVALID;
    ++tx->audio_view.working_revision;
    tx->audio_view.changed = !values_equal(&tx->schema, &tx->audio_view.working, &tx->audio_view.baseline);
    tx->audio_edit_dirty = true;
    if (tx->audio_view.audible_side != 0) {
        tx->auto_return_pending = true;
        *apply_to_dsp = false;
    } else *apply_to_dsp = true;
    return BROM_PRESET_OK;
}

brom_preset_result brom_preset_audio_apply_prepared(brom_preset_tx *tx,
        const brom_preset_snapshot *snapshot, const brom_preset_identity *identity) {
    if (!tx || !snapshot || !identity || !snapshot_valid(&tx->schema, snapshot) ||
        !identity_valid(identity)) return BROM_PRESET_INVALID;
    const uint64_t generation =
        atomic_fetch_add_explicit(&tx->next_generation, 1u, memory_order_acq_rel);
    /* Edits waiting behind an earlier load aimed at a view that no longer exists. */
    for (uint32_t i = 0; i < tx->deferred_count; ++i)
        if (tx->deferred[i].tag && tx->stale_count < tx->capacity)
            tx->stale_tags[tx->stale_count++] = tx->deferred[i].tag;
    tx->deferred_count = 0;
    tx->auto_return_pending = false;
    tx->audio_edit_dirty = false;
    brom_preset_view *view = &tx->audio_view;
    view->working = view->reference = view->baseline = *snapshot;
    view->working_identity = view->reference_identity = *identity;
    view->generation = generation;
    view->working_revision = 0;
    view->audible_side = 0;
    view->changed = false;
    uint64_t accepted = atomic_load_explicit(&tx->accepted_generation, memory_order_relaxed);
    while (accepted < generation && !atomic_compare_exchange_weak_explicit(
            &tx->accepted_generation, &accepted, generation,
            memory_order_release, memory_order_relaxed)) {}
    tx->publish_only = true;
    return BROM_PRESET_OK;
}

uint64_t brom_preset_audio_cutoff(brom_preset_tx *tx) {
    if (!tx) return 0;
    atomic_fetch_add_explicit(&tx->cutoff_epoch, 1, memory_order_acq_rel);
    return atomic_load_explicit(&tx->enqueue_pos, memory_order_acquire);
}

typedef struct outcome_sink {
    brom_preset_outcome_record *items;
    uint32_t capacity;
    uint32_t count;
} outcome_sink;

static brom_preset_event_kind public_kind(event_kind kind) {
    switch (kind) {
    case EVENT_LOAD: return BROM_PRESET_EVENT_LOAD;
    case EVENT_RESTORE: return BROM_PRESET_EVENT_RESTORE;
    case EVENT_COMPARE: return BROM_PRESET_EVENT_COMPARE;
    case EVENT_EDIT: return BROM_PRESET_EVENT_EDIT;
    case EVENT_SAVE_ACK: return BROM_PRESET_EVENT_SAVE_ACK;
    }
    return BROM_PRESET_EVENT_LOAD;
}

static bool sink_full(const outcome_sink *sink) {
    return sink->items && sink->count >= sink->capacity;
}

static void sink_add(outcome_sink *sink, const tx_event *event, brom_preset_outcome outcome,
                     brom_preset_result reason, uint64_t generation) {
    if (!sink->items || event->tag == 0 || sink->count >= sink->capacity) return;
    fill_outcome(&sink->items[sink->count++], event->tag, public_kind(event->kind), outcome,
                 generation, reason);
}

/* A newly applied full view overwrites whatever this boundary applied before it. */
static void sink_supersede(outcome_sink *sink) {
    for (uint32_t i = 0; i < sink->count; ++i) {
        brom_preset_outcome_record *record = &sink->items[i];
        if (record->outcome != BROM_PRESET_OUTCOME_APPLIED ||
            record->event == BROM_PRESET_EVENT_SAVE_ACK) continue;
        record->outcome = BROM_PRESET_OUTCOME_SUPERSEDED;
        record->reason = BROM_PRESET_SUPERSEDED;
    }
}

uint32_t brom_preset_boundary_outcome_capacity(const brom_preset_tx *tx) {
    /* Every deferred edit and every queued event reports once, plus the retry slot. */
    /* Plus the deferred edits a program change can leave stale for the next boundary. */
    return tx ? 3u * tx->capacity + 1u : 0;
}

bool brom_preset_audio_boundary_ex(brom_preset_tx *tx, uint64_t cutoff,
        brom_preset_commit *out, brom_preset_outcome_record *outcomes,
        uint32_t outcome_capacity, uint32_t *outcome_count) {
    if (outcome_count) *outcome_count = 0;
    if (!tx || !out || (outcomes && !outcome_count)) return false;
    outcome_sink sink = {.items = outcomes, .capacity = outcomes ? outcome_capacity : 0};
    brom_preset_commit commit = {0};
    /* Rebase on the pending inactive view while inactive, so a second boundary sees
     * what the first published instead of matching requests against a stale generation. */
    if (atomic_load_explicit(&tx->active, memory_order_acquire) ||
        !read_pending_inactive(tx, &commit.view))
        commit.view = tx->audio_view;
    bool full = false, changed = tx->audio_edit_dirty || tx->publish_only;
    bool queued_parameter_change = false;
    unsigned toggles = 0;
    uint64_t latest_full_generation = 0;
    if (tx->auto_return_pending) {
        commit.view.audible_side = 0;
        full = changed = true;
        tx->auto_return_pending = false;
    }
    tx_event event;
    {
        uint32_t reported = 0;
        for (; reported < tx->stale_count && !sink_full(&sink); ++reported) {
            const tx_event stale = {.kind = EVENT_EDIT, .tag = tx->stale_tags[reported]};
            sink_add(&sink, &stale, BROM_PRESET_OUTCOME_STALE, BROM_PRESET_STALE, 0);
        }
        if (reported < tx->stale_count)
            memmove(tx->stale_tags, tx->stale_tags + reported,
                    (tx->stale_count - reported) * sizeof(*tx->stale_tags));
        tx->stale_count -= reported;
    }
    const uint32_t pending = tx->deferred_count;
    uint32_t replayed = 0;
    for (; replayed < pending; ++replayed) {
        if (sink_full(&sink)) break;
        event = tx->deferred[replayed];
        if (event.generation != commit.view.generation || event.kind != EVENT_EDIT) {
            sink_add(&sink, &event, BROM_PRESET_OUTCOME_STALE, BROM_PRESET_STALE, event.generation);
            continue;
        }
        if (set_value_by_id(&tx->schema, &commit.view.working, event.id, event.value)) {
            ++commit.view.working_revision;
            commit.view.changed = !values_equal(&tx->schema, &commit.view.working, &commit.view.baseline);
            changed = true;
            queued_parameter_change = true;
            if (commit.view.audible_side != 0) { commit.view.audible_side = 0; full = true; }
            sink_add(&sink, &event, BROM_PRESET_OUTCOME_APPLIED, BROM_PRESET_OK, event.generation);
        } else sink_add(&sink, &event, BROM_PRESET_OUTCOME_DROPPED, BROM_PRESET_INVALID, event.generation);
    }
    /* A small outcome buffer leaves the unreplayed tail for the next boundary. */
    if (replayed < pending)
        memmove(tx->deferred, tx->deferred + replayed, (pending - replayed) * sizeof(*tx->deferred));
    tx->deferred_count = pending - replayed;
    while (!sink_full(&sink) && dequeue_until(tx, cutoff, &event)) {
        if (event.kind == EVENT_LOAD && event.generation <= commit.view.generation) {
            /* A program change or a newer view already replaced what this load was
             * submitted against. */
            result_slot *slot = &tx->results[event.generation % tx->capacity];
            if (atomic_load_explicit(&slot->generation, memory_order_acquire) == event.generation)
                atomic_store_explicit(&slot->result, BROM_PRESET_STALE, memory_order_release);
            sink_add(&sink, &event, BROM_PRESET_OUTCOME_STALE, BROM_PRESET_STALE, event.generation);
        } else if (event.kind == EVENT_LOAD) {
            if (latest_full_generation) {
                result_slot *prior = &tx->results[latest_full_generation % tx->capacity];
                if (atomic_load_explicit(&prior->generation, memory_order_acquire) == latest_full_generation)
                    atomic_store_explicit(&prior->result, BROM_PRESET_SUPERSEDED, memory_order_release);
            }
            sink_supersede(&sink);
            latest_full_generation = event.generation;
            brom_preset_snapshot snap = event.snapshot;
            snap.index = event.snapshot.index;
            commit.view.working = commit.view.reference = commit.view.baseline = snap;
            commit.view.working_identity = commit.view.reference_identity = event.identity;
            commit.view.generation = event.generation;
            commit.view.working_revision = 0;
            commit.view.audible_side = 0;
            commit.view.changed = false;
            full = changed = true;
            toggles = 0;
            tx->auto_return_pending = false;
            sink_add(&sink, &event, BROM_PRESET_OUTCOME_APPLIED, BROM_PRESET_OK, event.generation);
        } else if (event.kind == EVENT_RESTORE) {
            if (event.generation > commit.view.generation) {
                if (latest_full_generation) {
                    result_slot *prior = &tx->results[latest_full_generation % tx->capacity];
                    if (atomic_load_explicit(&prior->generation, memory_order_acquire) == latest_full_generation)
                        atomic_store_explicit(&prior->result, BROM_PRESET_SUPERSEDED, memory_order_release);
                }
                sink_supersede(&sink);
                latest_full_generation = event.generation;
                commit.view = event.view;
                commit.view.generation = event.generation;
                full = changed = true;
                toggles = 0;
                tx->auto_return_pending = false;
                sink_add(&sink, &event, BROM_PRESET_OUTCOME_APPLIED, BROM_PRESET_OK, event.generation);
            } else if (event.already_applied) {
                sink_add(&sink, &event, BROM_PRESET_OUTCOME_ALREADY_APPLIED, BROM_PRESET_OK, event.generation);
            } else {
                sink_add(&sink, &event, BROM_PRESET_OUTCOME_STALE, BROM_PRESET_STALE, event.generation);
            }
        } else if (event.kind == EVENT_COMPARE) {
            if (event.generation == commit.view.generation) {
                ++toggles;
                sink_add(&sink, &event, BROM_PRESET_OUTCOME_APPLIED, BROM_PRESET_OK, event.generation);
            } else sink_add(&sink, &event, BROM_PRESET_OUTCOME_STALE, BROM_PRESET_STALE, event.generation);
        } else if (event.kind == EVENT_EDIT) {
            if (event.generation != commit.view.generation) {
                sink_add(&sink, &event, BROM_PRESET_OUTCOME_STALE, BROM_PRESET_STALE, event.generation);
            } else if (full) {
                if (tx->deferred_count < tx->capacity) {
                    tx->deferred[tx->deferred_count++] = event;
                    sink_add(&sink, &event, BROM_PRESET_OUTCOME_DEFERRED, BROM_PRESET_OK, event.generation);
                } else sink_add(&sink, &event, BROM_PRESET_OUTCOME_DROPPED, BROM_PRESET_CAPACITY, event.generation);
            } else if (set_value_by_id(&tx->schema, &commit.view.working, event.id, event.value)) {
                ++commit.view.working_revision;
                commit.view.changed = !values_equal(&tx->schema, &commit.view.working, &commit.view.baseline);
                changed = true;
                queued_parameter_change = true;
                if (commit.view.audible_side != 0) {
                    commit.view.audible_side = 0;
                    full = true;
                }
                sink_add(&sink, &event, BROM_PRESET_OUTCOME_APPLIED, BROM_PRESET_OK, event.generation);
            } else sink_add(&sink, &event, BROM_PRESET_OUTCOME_DROPPED, BROM_PRESET_INVALID, event.generation);
        } else if (event.kind == EVENT_SAVE_ACK) {
            if (event.save_completion.generation != commit.view.generation) {
                sink_add(&sink, &event, BROM_PRESET_OUTCOME_STALE, BROM_PRESET_STALE, event.generation);
            } else if (event.save_completion.ticket_id <= tx->latest_save_ticket_id) {
                sink_add(&sink, &event, BROM_PRESET_OUTCOME_DROPPED, BROM_PRESET_SUPERSEDED, event.generation);
            } else {
                commit.view.baseline = event.save_completion.snapshot;
                commit.view.working_identity = event.save_completion.identity;
                tx->latest_save_ticket_id = event.save_completion.ticket_id;
                commit.view.changed = !values_equal(&tx->schema, &commit.view.working,
                                                    &commit.view.baseline);
                changed = true;
                sink_add(&sink, &event, BROM_PRESET_OUTCOME_APPLIED, BROM_PRESET_OK, event.generation);
            }
        }
    }
    const uint32_t retry_state = atomic_load_explicit(&tx->save_retry_state, memory_order_acquire);
    const bool retry_due = retry_state == 2 &&
        atomic_load_explicit(&tx->cutoff_epoch, memory_order_acquire) >= tx->save_retry_epoch;
    if (retry_due && !sink_full(&sink)) {
        brom_preset_save_completion completion = tx->save_retry;
        tx_event retry = {.kind = EVENT_SAVE_ACK, .generation = completion.generation,
                          .tag = tx->save_retry_tag};
        atomic_store_explicit(&tx->save_retry_state, 0, memory_order_release);
        commit.view.status[0] = '\0';
        changed = true;
        if (completion.generation != commit.view.generation) {
            sink_add(&sink, &retry, BROM_PRESET_OUTCOME_STALE, BROM_PRESET_STALE, completion.generation);
        } else if (completion.ticket_id <= tx->latest_save_ticket_id) {
            sink_add(&sink, &retry, BROM_PRESET_OUTCOME_DROPPED, BROM_PRESET_SUPERSEDED, completion.generation);
        } else {
            commit.view.baseline = completion.snapshot;
            commit.view.working_identity = completion.identity;
            tx->latest_save_ticket_id = completion.ticket_id;
            commit.view.changed = !values_equal(&tx->schema, &commit.view.working,
                                                &commit.view.baseline);
            sink_add(&sink, &retry, BROM_PRESET_OUTCOME_APPLIED, BROM_PRESET_OK, completion.generation);
        }
    } else if (retry_state == 2) {
        snprintf(commit.view.status, sizeof(commit.view.status),
                 "Save complete; preset state update queued");
        changed = true;
    }
    if (outcome_count) *outcome_count = sink.count;
    if (toggles & 1u) {
        commit.view.audible_side = commit.view.audible_side == 0 ? 1u : 0u;
        changed = true;
    }
    if (!changed) return false;
    commit.apply_parameters = full || queued_parameter_change;
    commit.reset_voices = full || ((toggles & 1u) != 0);
    commit.publish_view = true;
    tx->completion_to_ack = latest_full_generation;
    *out = commit;
    tx->audio_edit_dirty = false;
    tx->publish_only = false;
    return true;
}

bool brom_preset_audio_boundary(brom_preset_tx *tx, uint64_t cutoff,
        brom_preset_commit *out) {
    return brom_preset_audio_boundary_ex(tx, cutoff, out, NULL, 0, NULL);
}

void brom_preset_audio_publish(brom_preset_tx *tx, brom_preset_commit *commit) {
    if (!tx || !commit || !commit->publish_view) return;
    tx->audio_view = commit->view;
    uint32_t current = atomic_load_explicit(&tx->published_index, memory_order_acquire);
    for (uint32_t step = 1; step <= 3; ++step) {
        uint32_t candidate = (current + step) % 3;
        if (candidate == current || atomic_load_explicit(&tx->published[candidate].readers, memory_order_acquire)) continue;
        tx->published[candidate].view = commit->view;
        atomic_store_explicit(&tx->published_index, candidate, memory_order_release);
        atomic_store_explicit(&tx->inactive_pending, false, memory_order_release);
        tx->inactive_apply_parameters = false;
        tx->inactive_reset_voices = false;
        if (tx->completion_to_ack) {
            result_slot *slot = &tx->results[tx->completion_to_ack % tx->capacity];
            if (atomic_load_explicit(&slot->generation, memory_order_acquire) == tx->completion_to_ack)
                atomic_store_explicit(&slot->result, BROM_PRESET_OK, memory_order_release);
            tx->completion_to_ack = 0;
        }
        return;
    }
}

void brom_preset_audio_publish_inactive(brom_preset_tx *tx, brom_preset_commit *commit) {
    if (!tx || !commit || !commit->publish_view) return;
    /* The audio consumer has seen none of the inactive commits, so what any of them
     * asked for stays asked for until it takes over. */
    const bool carried = atomic_load_explicit(&tx->inactive_pending, memory_order_acquire);
    store_inactive_view(tx, &commit->view,
                        commit->apply_parameters || (carried && tx->inactive_apply_parameters),
                        commit->reset_voices || (carried && tx->inactive_reset_voices));
    if (tx->completion_to_ack) {
        result_slot *slot = &tx->results[tx->completion_to_ack % tx->capacity];
        if (atomic_load_explicit(&slot->generation, memory_order_acquire) == tx->completion_to_ack)
            atomic_store_explicit(&slot->result, BROM_PRESET_OK, memory_order_release);
        tx->completion_to_ack = 0;
    }
}

bool brom_preset_read_view(brom_preset_tx *tx, brom_preset_view *out) {
    if (!tx || !out) return false;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        if (!atomic_load_explicit(&tx->inactive_pending, memory_order_acquire)) break;
        uint64_t before = atomic_load_explicit(&tx->inactive_sequence, memory_order_acquire);
        if (before & 1u) continue;
        brom_preset_view pending = tx->inactive_view;
        uint64_t after = atomic_load_explicit(&tx->inactive_sequence, memory_order_acquire);
        if (before == after && !(after & 1u)) {
            *out = pending;
            return true;
        }
    }
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        uint32_t index = atomic_load_explicit(&tx->published_index, memory_order_acquire);
        view_slot *slot = &tx->published[index];
        atomic_fetch_add_explicit(&slot->readers, 1, memory_order_acquire);
        if (index != atomic_load_explicit(&tx->published_index, memory_order_acquire)) {
            atomic_fetch_sub_explicit(&slot->readers, 1, memory_order_release);
            continue;
        }
        *out = slot->view;
        atomic_fetch_sub_explicit(&slot->readers, 1, memory_order_release);
        return true;
    }
    return false;
}

void brom_preset_set_active(brom_preset_tx *tx, bool active) {
    if (tx) atomic_store_explicit(&tx->active, active, memory_order_release);
}

bool brom_preset_audio_resume(brom_preset_tx *tx, brom_preset_commit *out) {
    if (!tx || !out || !atomic_load_explicit(&tx->inactive_pending, memory_order_acquire)) return false;
    memset(out, 0, sizeof(*out));
    bool apply_parameters = false, reset_voices = false, copied = false;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        uint64_t before = atomic_load_explicit(&tx->inactive_sequence, memory_order_acquire);
        if (before & 1u) continue;
        out->view = tx->inactive_view;
        apply_parameters = tx->inactive_apply_parameters;
        reset_voices = tx->inactive_reset_voices;
        uint64_t after = atomic_load_explicit(&tx->inactive_sequence, memory_order_acquire);
        if (before == after && !(after & 1u)) { copied = true; break; }
    }
    if (!copied) return false;
    out->apply_parameters = apply_parameters;
    out->reset_voices = reset_voices;
    out->publish_view = true;
    tx->completion_to_ack = out->view.generation;
    return true;
}
