#include "transaction.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef enum event_kind { EVENT_LOAD, EVENT_RESTORE, EVENT_COMPARE, EVENT_EDIT } event_kind;

typedef struct tx_event {
    event_kind kind;
    uint64_t generation;
    uint32_t id;
    double value;
    brom_preset_snapshot snapshot;
    brom_preset_identity identity;
    brom_preset_view view;
} tx_event;

typedef struct tx_cell {
    _Atomic uint64_t turn;
    tx_event event;
} tx_cell;

typedef struct view_slot {
    _Atomic uint32_t readers;
    brom_preset_view view;
} view_slot;

struct brom_preset_tx {
    brom_preset_schema schema;
    tx_cell *queue;
    uint32_t capacity;
    uint32_t mask;
    _Atomic uint64_t enqueue_pos;
    _Atomic uint64_t dequeue_pos;
    _Atomic uint64_t accepted_generation;
    _Atomic bool active;
    view_slot published[3];
    _Atomic uint32_t published_index;
    brom_preset_view audio_view;
    tx_event *deferred;
    uint32_t deferred_count;
    bool inactive_pending;
};

static uint32_t next_power_of_two(uint32_t value) {
    uint32_t result = 2;
    while (result < value && result <= UINT32_MAX / 2) result <<= 1;
    return result;
}

static bool identity_valid(const brom_preset_identity *identity) {
    return identity && memchr(identity->key, 0, sizeof(identity->key)) &&
        memchr(identity->display_name, 0, sizeof(identity->display_name));
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

brom_preset_tx *brom_preset_tx_create(const brom_preset_schema *schema,
                                      const brom_preset_view *initial,
                                      uint32_t queue_capacity) {
    if (!schema || !initial || schema->count == 0 || schema->count > BROM_PRESET_MAX_PARAMS ||
        !snapshot_valid(schema, &initial->working) ||
        !snapshot_valid(schema, &initial->reference) ||
        !snapshot_valid(schema, &initial->baseline) || queue_capacity == 0) return NULL;
    uint32_t capacity = next_power_of_two(queue_capacity);
    if (capacity < queue_capacity) return NULL;
    brom_preset_tx *tx = calloc(1, sizeof(*tx));
    if (!tx) return NULL;
    tx->queue = calloc(capacity, sizeof(*tx->queue));
    tx->deferred = calloc(capacity, sizeof(*tx->deferred));
    if (!tx->queue || !tx->deferred) { free(tx->deferred); free(tx->queue); free(tx); return NULL; }
    tx->schema = *schema;
    tx->capacity = capacity;
    tx->mask = capacity - 1;
    for (uint32_t i = 0; i < capacity; ++i) atomic_init(&tx->queue[i].turn, i);
    atomic_init(&tx->enqueue_pos, 0);
    atomic_init(&tx->dequeue_pos, 0);
    atomic_init(&tx->accepted_generation, initial->generation);
    atomic_init(&tx->active, false);
    tx->audio_view = *initial;
    tx->published[0].view = *initial;
    atomic_init(&tx->published_index, 0);
    for (uint32_t i = 0; i < 3; ++i) atomic_init(&tx->published[i].readers, 0);
    return tx;
}

void brom_preset_tx_destroy(brom_preset_tx *tx) {
    if (!tx) return;
    free(tx->deferred);
    free(tx->queue);
    free(tx);
}

static brom_preset_result submit_full(brom_preset_tx *tx, event_kind kind,
                                      const brom_preset_snapshot *snapshot,
                                      const brom_preset_identity *identity,
                                      const brom_preset_view *view, uint64_t *generation) {
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
    event.generation = position + 1;
    if (kind == EVENT_LOAD) { event.snapshot = *snapshot; event.identity = *identity; }
    else event.view = *view;
    publish_position(tx, position, &event);
    uint64_t accepted = atomic_load_explicit(&tx->accepted_generation, memory_order_relaxed);
    while (accepted < event.generation && !atomic_compare_exchange_weak_explicit(
            &tx->accepted_generation, &accepted, event.generation,
            memory_order_release, memory_order_relaxed)) {}
    *generation = event.generation;
    return BROM_PRESET_OK;
}

brom_preset_result brom_preset_submit_load(brom_preset_tx *tx,
        const brom_preset_snapshot *snapshot, const brom_preset_identity *identity,
        uint64_t *generation) {
    return submit_full(tx, EVENT_LOAD, snapshot, identity, NULL, generation);
}

brom_preset_result brom_preset_submit_restore(brom_preset_tx *tx,
        const brom_preset_view *view, uint64_t *generation) {
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

brom_preset_result brom_preset_audio_edit(brom_preset_tx *tx, uint32_t id,
        double value, bool identified_echo, bool *apply_to_dsp) {
    if (!tx || !apply_to_dsp) return BROM_PRESET_INVALID;
    *apply_to_dsp = false;
    if (identified_echo) return BROM_PRESET_OK;
    if (!value_valid(&tx->schema, id, value)) return BROM_PRESET_INVALID;
    if (!set_value_by_id(&tx->schema, &tx->audio_view.working, id, value)) return BROM_PRESET_INVALID;
    ++tx->audio_view.working_revision;
    tx->audio_view.changed = !values_equal(&tx->schema, &tx->audio_view.working, &tx->audio_view.baseline);
    if (tx->audio_view.audible_side != 0) {
        tx->audio_view.audible_side = 0;
        *apply_to_dsp = true;
        tx->inactive_pending = true;
    } else *apply_to_dsp = true;
    return BROM_PRESET_OK;
}

uint64_t brom_preset_audio_cutoff(brom_preset_tx *tx) {
    return tx ? atomic_load_explicit(&tx->enqueue_pos, memory_order_acquire) : 0;
}

bool brom_preset_audio_boundary(brom_preset_tx *tx, uint64_t cutoff,
        brom_preset_commit *out) {
    if (!tx || !out) return false;
    brom_preset_commit commit = {0};
    commit.view = tx->audio_view;
    bool full = false, changed = false;
    unsigned toggles = 0;
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
                if (commit.view.audible_side != 0) { commit.view.audible_side = 0; full = true; }
            }
        }
    }
    while (dequeue_until(tx, cutoff, &event)) {
        if (event.kind == EVENT_LOAD) {
            brom_preset_snapshot snap = event.snapshot;
            snap.index = event.snapshot.index;
            commit.view.working = commit.view.reference = commit.view.baseline = snap;
            commit.view.working_identity = commit.view.reference_identity = event.identity;
            commit.view.generation = event.generation;
            commit.view.working_revision = 0;
            commit.view.audible_side = 0;
            full = changed = true;
            toggles = 0;
        } else if (event.kind == EVENT_RESTORE) {
            commit.view = event.view;
            commit.view.generation = event.generation;
            full = changed = true;
            toggles = 0;
        } else if (event.generation == commit.view.generation && event.kind == EVENT_COMPARE) {
            ++toggles;
        } else if (full && event.generation == commit.view.generation && event.kind == EVENT_EDIT) {
            if (tx->deferred_count < tx->capacity) tx->deferred[tx->deferred_count++] = event;
        } else if (event.generation == commit.view.generation && event.kind == EVENT_EDIT) {
            if (set_value_by_id(&tx->schema, &commit.view.working, event.id, event.value)) {
                ++commit.view.working_revision;
                commit.view.changed = !values_equal(&tx->schema, &commit.view.working, &commit.view.baseline);
                changed = true;
                if (commit.view.audible_side != 0) {
                    commit.view.audible_side = 0;
                    full = true;
                }
            }
        }
    }
    if (toggles & 1u) {
        commit.view.audible_side = commit.view.audible_side == 0 ? 1u : 0u;
        changed = true;
    }
    if (!changed) return false;
    commit.apply_parameters = full || changed;
    commit.reset_voices = full || ((toggles & 1u) != 0);
    commit.publish_view = true;
    *out = commit;
    return true;
}

void brom_preset_audio_publish(brom_preset_tx *tx, const brom_preset_commit *commit) {
    if (!tx || !commit || !commit->publish_view) return;
    tx->audio_view = commit->view;
    uint32_t current = atomic_load_explicit(&tx->published_index, memory_order_acquire);
    for (uint32_t step = 1; step <= 3; ++step) {
        uint32_t candidate = (current + step) % 3;
        if (candidate == current || atomic_load_explicit(&tx->published[candidate].readers, memory_order_acquire)) continue;
        tx->published[candidate].view = commit->view;
        atomic_store_explicit(&tx->published_index, candidate, memory_order_release);
        tx->inactive_pending = false;
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
