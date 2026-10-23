#include "session.h"

#include <math.h>
#include <string.h>

#define SESSION_HEADER_BYTES BROM_PRESET_SESSION_HEADER_BYTES
#define SESSION_VERSION 1u

typedef struct writer {
    uint8_t *bytes;
    size_t capacity;
    size_t position;
    int failed;
} writer;

typedef struct reader {
    const uint8_t *bytes;
    size_t size;
    size_t position;
    int failed;
} reader;

static void put_bytes(writer *out, const void *source, size_t count) {
    if (out->failed || count > out->capacity - out->position) { out->failed = 1; return; }
    memcpy(out->bytes + out->position, source, count);
    out->position += count;
}

static void put_u8(writer *out, uint8_t value) { put_bytes(out, &value, 1); }

static void put_u16(writer *out, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    put_bytes(out, bytes, sizeof(bytes));
}

static void put_u32(writer *out, uint32_t value) {
    uint8_t bytes[4] = {(uint8_t)value, (uint8_t)(value >> 8),
                        (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    put_bytes(out, bytes, sizeof(bytes));
}

static void put_u64(writer *out, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned index = 0; index < 8; ++index) bytes[index] = (uint8_t)(value >> (8u * index));
    put_bytes(out, bytes, sizeof(bytes));
}

static uint8_t get_u8(reader *in) {
    if (in->position >= in->size) { in->failed = 1; return 0; }
    return in->bytes[in->position++];
}

static uint16_t get_u16(reader *in) {
    uint16_t value = 0;
    for (unsigned index = 0; index < 2; ++index) value |= (uint16_t)get_u8(in) << (8u * index);
    return value;
}

static uint32_t get_u32(reader *in) {
    uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) value |= (uint32_t)get_u8(in) << (8u * index);
    return value;
}

static uint64_t get_u64(reader *in) {
    uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) value |= (uint64_t)get_u8(in) << (8u * index);
    return value;
}

static void get_bytes(reader *in, void *destination, size_t count) {
    if (in->failed || count > in->size - in->position) { in->failed = 1; return; }
    memcpy(destination, in->bytes + in->position, count);
    in->position += count;
}

static uint32_t checksum(const uint8_t *bytes, size_t size) {
    uint32_t value = 2166136261u;
    for (size_t index = 0; index < size; ++index) value = (value ^ bytes[index]) * 16777619u;
    return value;
}

static int schema_valid(const brom_preset_schema *schema) {
    if (!schema || schema->count == 0 || schema->count > BROM_PRESET_MAX_PARAMS) return 0;
    for (uint32_t index = 0; index < schema->count; ++index) {
        const brom_preset_parameter *parameter = &schema->parameters[index];
        if (!isfinite(parameter->minimum) || !isfinite(parameter->maximum) ||
            !isfinite(parameter->default_value) || parameter->minimum > parameter->maximum ||
            parameter->default_value < parameter->minimum || parameter->default_value > parameter->maximum ||
            !memchr(parameter->key, 0, sizeof(parameter->key)) || !parameter->key[0]) return 0;
        for (uint32_t prior = 0; prior < index; ++prior)
            if (schema->parameters[prior].id == parameter->id) return 0;
    }
    return 1;
}

static int value_valid(const brom_preset_parameter *parameter, double value) {
    if (!isfinite(value) || value < parameter->minimum || value > parameter->maximum) return 0;
    if (parameter->steps > 1u) {
        if (parameter->minimum == parameter->maximum) return 0;
        const double step = (parameter->maximum - parameter->minimum) / (double)(parameter->steps - 1u);
        const double position = (value - parameter->minimum) / step;
        if (!isfinite(position) || fabs(position - nearbyint(position)) > 1e-9) return 0;
    }
    return 1;
}

static int snapshot_valid(const brom_preset_schema *schema,
                          const brom_preset_snapshot *snapshot) {
    if (!snapshot || snapshot->count != (int32_t)schema->count) return 0;
    for (uint32_t index = 0; index < schema->count; ++index)
        if (!value_valid(&schema->parameters[index], snapshot->values[index])) return 0;
    return 1;
}

static int snapshots_equal(const brom_preset_schema *schema,
                           const brom_preset_snapshot *left,
                           const brom_preset_snapshot *right) {
    for (uint32_t index = 0; index < schema->count; ++index) {
        const double a = left->values[index], b = right->values[index];
        if (schema->parameters[index].steps > 1u) {
            if (a != b) return 0;
        } else if (fabs(a - b) > fmax(1.0, fabs(b)) * 1e-9) return 0;
    }
    return 1;
}

static int identity_valid(const brom_preset_identity *identity) {
    return identity && memchr(identity->key, 0, sizeof(identity->key)) &&
           memchr(identity->display_name, 0, sizeof(identity->display_name)) &&
           identity->key[0] && identity->display_name[0];
}

static void put_string(writer *out, const char *text, size_t capacity) {
    if (!memchr(text, 0, capacity)) { out->failed = 1; return; }
    const size_t length = strlen(text);
    if (length > UINT16_MAX) { out->failed = 1; return; }
    put_u16(out, (uint16_t)length);
    put_bytes(out, text, length);
}

static int get_string(reader *in, char *out, size_t capacity) {
    const uint16_t length = get_u16(in);
    if (in->failed || length >= capacity ||
        length > in->size - in->position ||
        memchr(in->bytes + in->position, 0, length)) return 0;
    get_bytes(in, out, length);
    out[length] = 0;
    return !in->failed;
}

static void put_snapshot(writer *out, const brom_preset_snapshot *snapshot) {
    put_u32(out, (uint32_t)snapshot->count);
    put_u32(out, (uint32_t)snapshot->index);
    for (int32_t index = 0; index < snapshot->count; ++index) {
        uint64_t bits = 0;
        memcpy(&bits, &snapshot->values[index], sizeof(bits));
        put_u64(out, bits);
    }
}

static int get_snapshot(reader *in, const brom_preset_schema *schema,
                        brom_preset_snapshot *snapshot) {
    snapshot->count = (int32_t)get_u32(in);
    snapshot->index = (int32_t)get_u32(in);
    if (in->failed || snapshot->count != (int32_t)schema->count) return 0;
    for (int32_t index = 0; index < snapshot->count; ++index) {
        const uint64_t bits = get_u64(in);
        memcpy(&snapshot->values[index], &bits, sizeof(bits));
    }
    return !in->failed && snapshot_valid(schema, snapshot);
}

static void put_identity(writer *out, const brom_preset_identity *identity) {
    put_u32(out, identity->kind);
    put_string(out, identity->key, sizeof(identity->key));
    put_string(out, identity->display_name, sizeof(identity->display_name));
}

static int get_identity(reader *in, brom_preset_identity *identity) {
    identity->kind = get_u32(in);
    identity->reserved = 0;
    if (!get_string(in, identity->key, sizeof(identity->key)) ||
        !get_string(in, identity->display_name, sizeof(identity->display_name))) return 0;
    return identity_valid(identity);
}

static int product_key_valid(const char *key) {
    if (!key || !*key || strlen(key) >= BROM_PRESET_KEY_BYTES) return 0;
    for (const unsigned char *p = (const unsigned char *)key; *p; ++p)
        if (*p < 33u || *p > 126u) return 0;
    return 1;
}

brom_preset_result brom_preset_session_encode(
    const brom_preset_schema *schema, const char *product_key,
    const brom_preset_view *view, uint8_t *out, size_t capacity,
    size_t *written) {
    if (written) *written = 0;
    if (!schema_valid(schema) || !product_key_valid(product_key) || !view || !out || !written ||
        capacity < SESSION_HEADER_BYTES ||
        !snapshot_valid(schema, &view->working) || !snapshot_valid(schema, &view->reference) ||
        !snapshot_valid(schema, &view->baseline) || !identity_valid(&view->working_identity) ||
        !identity_valid(&view->reference_identity) || view->audible_side > 1u ||
        !memchr(view->status, 0, sizeof(view->status)) ||
        view->changed != !snapshots_equal(schema, &view->working, &view->baseline))
        return capacity < SESSION_HEADER_BYTES ? BROM_PRESET_CAPACITY : BROM_PRESET_INVALID;

    const size_t bounded_capacity = capacity > BROM_PRESET_SESSION_MAX_BYTES
        ? BROM_PRESET_SESSION_MAX_BYTES : capacity;

    writer payload = {.bytes = out + SESSION_HEADER_BYTES,
                      .capacity = bounded_capacity - SESSION_HEADER_BYTES};
    put_string(&payload, product_key, BROM_PRESET_KEY_BYTES);
    put_u32(&payload, schema->version);
    put_u32(&payload, schema->count);
    put_u64(&payload, view->generation);
    put_u64(&payload, view->working_revision);
    put_u64(&payload, view->publication_sequence);
    put_snapshot(&payload, &view->working);
    put_snapshot(&payload, &view->reference);
    put_snapshot(&payload, &view->baseline);
    put_identity(&payload, &view->working_identity);
    put_identity(&payload, &view->reference_identity);
    put_u32(&payload, view->audible_side);
    put_u8(&payload, view->changed ? 1u : 0u);
    put_string(&payload, view->status, sizeof(view->status));
    if (payload.failed) return BROM_PRESET_CAPACITY;

    writer header = {.bytes = out, .capacity = SESSION_HEADER_BYTES};
    const uint8_t magic[4] = {'B', 'P', 'S', 'N'};
    put_bytes(&header, magic, sizeof(magic));
    put_u16(&header, SESSION_VERSION);
    put_u16(&header, 0);
    put_u32(&header, (uint32_t)payload.position);
    put_u32(&header, checksum(out + SESSION_HEADER_BYTES, payload.position));
    if (header.failed) return BROM_PRESET_CAPACITY;
    *written = SESSION_HEADER_BYTES + payload.position;
    return BROM_PRESET_OK;
}

brom_preset_result brom_preset_session_decode(
    const uint8_t *bytes, size_t size, const brom_preset_schema *schema,
    const char *product_key, brom_preset_view *out_view) {
    if (!bytes || !out_view || !schema_valid(schema) || !product_key_valid(product_key) ||
        size < SESSION_HEADER_BYTES || size > BROM_PRESET_SESSION_MAX_BYTES)
        return size > BROM_PRESET_SESSION_MAX_BYTES ? BROM_PRESET_CAPACITY : BROM_PRESET_INVALID;
    reader header = {.bytes = bytes, .size = SESSION_HEADER_BYTES};
    uint8_t magic[4] = {0};
    get_bytes(&header, magic, sizeof(magic));
    const uint16_t version = get_u16(&header);
    const uint16_t flags = get_u16(&header);
    const uint32_t payload_size = get_u32(&header);
    const uint32_t expected_checksum = get_u32(&header);
    if (header.failed || memcmp(magic, BROM_PRESET_SESSION_MAGIC, 4) != 0 || version != SESSION_VERSION || flags != 0 ||
        payload_size != size - SESSION_HEADER_BYTES ||
        checksum(bytes + SESSION_HEADER_BYTES, payload_size) != expected_checksum)
        return BROM_PRESET_INVALID;

    reader payload = {.bytes = bytes + SESSION_HEADER_BYTES, .size = payload_size};
    char decoded_product[BROM_PRESET_KEY_BYTES] = {0};
    if (!get_string(&payload, decoded_product, sizeof(decoded_product)) ||
        strcmp(decoded_product, product_key) != 0 || get_u32(&payload) != schema->version ||
        get_u32(&payload) != schema->count) return BROM_PRESET_INVALID;
    brom_preset_view view = {0};
    view.generation = get_u64(&payload);
    view.working_revision = get_u64(&payload);
    view.publication_sequence = get_u64(&payload);
    if (!get_snapshot(&payload, schema, &view.working) ||
        !get_snapshot(&payload, schema, &view.reference) ||
        !get_snapshot(&payload, schema, &view.baseline) ||
        !get_identity(&payload, &view.working_identity) ||
        !get_identity(&payload, &view.reference_identity)) return BROM_PRESET_INVALID;
    view.audible_side = get_u32(&payload);
    view.changed = get_u8(&payload) != 0;
    if (payload.failed || view.audible_side > 1u ||
        !get_string(&payload, view.status, sizeof(view.status)) ||
        payload.failed || payload.position != payload.size ||
        view.changed != !snapshots_equal(schema, &view.working, &view.baseline))
        return BROM_PRESET_INVALID;
    *out_view = view;
    return BROM_PRESET_OK;
}

brom_preset_result brom_preset_session_migrate_legacy(
    const brom_preset_schema *schema, const brom_preset_snapshot *snapshot,
    const brom_preset_identity *identity, brom_preset_view *out_view) {
    if (!schema_valid(schema) || !snapshot_valid(schema, snapshot) ||
        !identity_valid(identity) || !out_view) return BROM_PRESET_INVALID;
    brom_preset_view migrated = {0};
    migrated.working = migrated.reference = migrated.baseline = *snapshot;
    migrated.working_identity = migrated.reference_identity = *identity;
    migrated.audible_side = 0;
    migrated.changed = false;
    *out_view = migrated;
    return BROM_PRESET_OK;
}
