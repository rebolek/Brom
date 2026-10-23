#include "transaction.h"

#include <math.h>
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
    uint64_t generation_base;
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
    tx_event *deferred;
    brom_preset_save_completion save_retry;
    uint64_t save_retry_epoch;
    uint32_t deferred_count;
    uint64_t completion_to_ack;
    bool auto_return_pending;
    bool audio_edit_dirty;
    bool inactive_pending;
};

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
    if (!tx->queue || !tx->deferred || !tx->results) {
        free(tx->results); free(tx->deferred); free(tx->queue); free(tx); return NULL;
    }
    tx->schema = *schema;
    tx->capacity = capacity;
    tx->mask = capacity - 1;
    tx->generation_base = initial->generation;
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
    atomic_init(&tx->active, false);
    tx->audio_view = *initial;
    tx->published[0].view = *initial;
    atomic_init(&tx->published_index, 0);
    for (uint32_t i = 0; i < 3; ++i) atomic_init(&tx->published[i].readers, 0);
    return tx;
}

void brom_preset_tx_destroy(brom_preset_tx *tx) {
    if (!tx) return;
    free(tx->results);
    free(tx->deferred);
    free(tx->queue);
    free(tx);
}

static brom_preset_result submit_full(brom_preset_tx *tx, event_kind kind,
                                      brom_preset_snapshot *snapshot,
                                      brom_preset_identity *identity,
                                      brom_preset_view *view, uint64_t *generation) {
    if (!tx || !generation) return BROM_PRESET_INVALID;
    if (kind == EVENT_LOAD && (!snapshot_valid(&tx->schema, snapshot) || !identity_valid(identity)))
        return BROM_PRESET_INVALID;
    if (kind == EVENT_RESTORE && (!view || !snapshot_valid(&tx->schema, &view->working) ||
        !snapshot_valid(&tx->schema, &view->reference) || !snapshot_valid(&tx->schema, &view->baseline) ||
        !identity_valid(&view->working_identity) || !identity_valid(&view->reference_identity)))
        return BROM_PRESET_INVALID;
    tx_event event = {.kind = kind};
    uint64_t position;
    if (!reserve_position(tx, &position)) return BROM_PRESET_CAPACITY;
    event.generation = tx->generation_base + position + 1;
    if (kind == EVENT_LOAD) { event.snapshot = *snapshot; event.identity = *identity; }
    else event.view = *view;
    result_slot *result = &tx->results[event.generation % tx->capacity];
    atomic_store_explicit(&result->result, BROM_PRESET_BUSY, memory_order_relaxed);
    atomic_store_explicit(&result->generation, event.generation, memory_order_release);
    publish_position(tx, position, &event);
    uint64_t accepted = atomic_load_explicit(&tx->accepted_generation, memory_order_relaxed);
    while (accepted < event.generation && !atomic_compare_exchange_weak_explicit(
            &tx->accepted_generation, &accepted, event.generation,
            memory_order_release, memory_order_relaxed)) {}
    *generation = event.generation;
    return BROM_PRESET_OK;
}

brom_preset_result brom_preset_submit_load(brom_preset_tx *tx,
        brom_preset_snapshot *snapshot, brom_preset_identity *identity,
        uint64_t *generation) {
    return submit_full(tx, EVENT_LOAD, snapshot, identity, NULL, generation);
}

brom_preset_result brom_preset_submit_restore(brom_preset_tx *tx,
        brom_preset_view *view, uint64_t *generation) {
    return submit_full(tx, EVENT_RESTORE, NULL, NULL, view, generation);
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

brom_preset_result brom_preset_submit_compare(brom_preset_tx *tx, uint64_t generation) {
    tx_event event = {.kind = EVENT_COMPARE};
    return submit_simple(tx, &event, generation);
}

brom_preset_result brom_preset_submit_edit(brom_preset_tx *tx, uint64_t generation,
        uint32_t id, double value) {
    if (!tx || !value_valid(&tx->schema, id, value)) return BROM_PRESET_INVALID;
    tx_event event = {.kind = EVENT_EDIT, .id = id, .value = value};
    return submit_simple(tx, &event, generation);
}

brom_preset_result brom_preset_save_ack_submit(
        brom_preset_tx *tx, const brom_preset_save_completion *completion) {
    if (!tx || !completion || !snapshot_valid(&tx->schema, &completion->snapshot) ||
        !identity_valid(&completion->identity)) return BROM_PRESET_INVALID;
    tx_event event = {.kind = EVENT_SAVE_ACK, .generation = completion->generation};
    event.save_completion = *completion;
    uint64_t position;
    if (!reserve_position(tx, &position)) {
        uint32_t empty = 0;
        if (!atomic_compare_exchange_strong_explicit(&tx->save_retry_state, &empty, 1,
                memory_order_acquire, memory_order_relaxed)) return BROM_PRESET_CAPACITY;
        tx->save_retry = *completion;
        tx->save_retry_epoch = atomic_load_explicit(&tx->cutoff_epoch, memory_order_acquire) + 1u;
        atomic_store_explicit(&tx->save_retry_state, 2, memory_order_release);
        return BROM_PRESET_OK;
    }
    publish_position(tx, position, &event);
    return BROM_PRESET_OK;
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

uint64_t brom_preset_audio_cutoff(brom_preset_tx *tx) {
    if (!tx) return 0;
    atomic_fetch_add_explicit(&tx->cutoff_epoch, 1, memory_order_acq_rel);
    return atomic_load_explicit(&tx->enqueue_pos, memory_order_acquire);
}

bool brom_preset_audio_boundary(brom_preset_tx *tx, uint64_t cutoff,
        brom_preset_commit *out) {
    if (!tx || !out) return false;
    brom_preset_commit commit = {0};
    commit.view = tx->audio_view;
    bool full = false, changed = tx->audio_edit_dirty;
    bool queued_parameter_change = false;
    unsigned toggles = 0;
    uint64_t latest_full_generation = 0;
    if (tx->auto_return_pending) {
        commit.view.audible_side = 0;
        full = changed = true;
        tx->auto_return_pending = false;
    }
    tx_event event;
    uint32_t pending = tx->deferred_count;
    tx->deferred_count = 0;
    for (uint32_t i = 0; i < pending; ++i) {
        event = tx->deferred[i];
        if (event.generation == commit.view.generation && event.kind == EVENT_EDIT) {
            if (set_value_by_id(&tx->schema, &commit.view.working, event.id, event.value)) {
                ++commit.view.working_revision;
                commit.view.changed = !values_equal(&tx->schema, &commit.view.working, &commit.view.baseline);
                changed = true;
                queued_parameter_change = true;
                if (commit.view.audible_side != 0) { commit.view.audible_side = 0; full = true; }
            }
        }
    }
    while (dequeue_until(tx, cutoff, &event)) {
        if (event.kind == EVENT_LOAD) {
            if (latest_full_generation) {
                result_slot *prior = &tx->results[latest_full_generation % tx->capacity];
                if (atomic_load_explicit(&prior->generation, memory_order_acquire) == latest_full_generation)
                    atomic_store_explicit(&prior->result, BROM_PRESET_SUPERSEDED, memory_order_release);
            }
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
        } else if (event.kind == EVENT_RESTORE) {
            if (latest_full_generation) {
                result_slot *prior = &tx->results[latest_full_generation % tx->capacity];
                if (atomic_load_explicit(&prior->generation, memory_order_acquire) == latest_full_generation)
                    atomic_store_explicit(&prior->result, BROM_PRESET_SUPERSEDED, memory_order_release);
            }
            latest_full_generation = event.generation;
            commit.view = event.view;
            commit.view.generation = event.generation;
            full = changed = true;
            toggles = 0;
            tx->auto_return_pending = false;
        } else if (event.generation == commit.view.generation && event.kind == EVENT_COMPARE) {
            ++toggles;
        } else if (full && event.generation == commit.view.generation && event.kind == EVENT_EDIT) {
            if (tx->deferred_count < tx->capacity) tx->deferred[tx->deferred_count++] = event;
        } else if (event.generation == commit.view.generation && event.kind == EVENT_EDIT) {
            if (set_value_by_id(&tx->schema, &commit.view.working, event.id, event.value)) {
                ++commit.view.working_revision;
                commit.view.changed = !values_equal(&tx->schema, &commit.view.working, &commit.view.baseline);
                changed = true;
                queued_parameter_change = true;
                if (commit.view.audible_side != 0) {
                    commit.view.audible_side = 0;
                    full = true;
                }
            }
        } else if (event.kind == EVENT_SAVE_ACK &&
                   event.save_completion.generation == commit.view.generation) {
            commit.view.baseline = event.save_completion.snapshot;
            commit.view.working_identity = event.save_completion.identity;
            commit.view.changed = !values_equal(&tx->schema, &commit.view.working,
                                                &commit.view.baseline);
            changed = true;
        }
    }
    if (atomic_load_explicit(&tx->save_retry_state, memory_order_acquire) == 2 &&
        atomic_load_explicit(&tx->cutoff_epoch, memory_order_acquire) >= tx->save_retry_epoch) {
        brom_preset_save_completion completion = tx->save_retry;
        atomic_store_explicit(&tx->save_retry_state, 0, memory_order_release);
        commit.view.status[0] = '\0';
        changed = true;
        if (completion.generation == commit.view.generation) {
            commit.view.baseline = completion.snapshot;
            commit.view.working_identity = completion.identity;
            commit.view.changed = !values_equal(&tx->schema, &commit.view.working,
                                                &commit.view.baseline);
            changed = true;
        }
    } else if (atomic_load_explicit(&tx->save_retry_state, memory_order_acquire) == 2) {
        snprintf(commit.view.status, sizeof(commit.view.status),
                 "Save complete; preset state update queued");
        changed = true;
    }
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
    return true;
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
        tx->inactive_pending = false;
        if (tx->completion_to_ack) {
            result_slot *slot = &tx->results[tx->completion_to_ack % tx->capacity];
            if (atomic_load_explicit(&slot->generation, memory_order_acquire) == tx->completion_to_ack)
                atomic_store_explicit(&slot->result, BROM_PRESET_OK, memory_order_release);
            tx->completion_to_ack = 0;
        }
        return;
    }
}

bool brom_preset_read_view(brom_preset_tx *tx, brom_preset_view *out) {
    if (!tx || !out) return false;
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
    if (!tx || !out || !tx->inactive_pending) return false;
    memset(out, 0, sizeof(*out));
    out->view = tx->audio_view;
    out->apply_parameters = true;
    out->reset_voices = true;
    out->publish_view = true;
    return true;
}
