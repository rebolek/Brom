#ifndef BROM_PLUGIN_PRESET_API_H
#define BROM_PLUGIN_PRESET_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BROM_PRESET_MAX_PARAMS 256u
#define BROM_PRESET_KEY_BYTES 128u
#define BROM_PRESET_NAME_BYTES 256u
#define BROM_PRESET_STATUS_BYTES 256u

typedef enum brom_preset_result {
    BROM_PRESET_OK = 0,
    BROM_PRESET_INVALID = 1,
    BROM_PRESET_CAPACITY = 2,
    BROM_PRESET_STALE = 3,
    BROM_PRESET_BUSY = 4,
    BROM_PRESET_IO_ERROR = 5,
    BROM_PRESET_NOT_FOUND = 6,
    BROM_PRESET_SUPERSEDED = 7
} brom_preset_result;

typedef struct brom_preset_parameter {
    uint32_t id;
    uint32_t steps;
    double minimum;
    double maximum;
    double default_value;
    char key[BROM_PRESET_KEY_BYTES];
} brom_preset_parameter;

typedef struct brom_preset_schema {
    uint32_t count;
    uint32_t version;
    brom_preset_parameter parameters[BROM_PRESET_MAX_PARAMS];
} brom_preset_schema;

typedef struct brom_preset_snapshot {
    int32_t count;
    int32_t index;
    double values[BROM_PRESET_MAX_PARAMS];
} brom_preset_snapshot;

typedef struct brom_preset_identity {
    char key[BROM_PRESET_KEY_BYTES];
    char display_name[BROM_PRESET_NAME_BYTES];
    uint32_t kind;
    uint32_t reserved;
} brom_preset_identity;

typedef struct brom_preset_view {
    uint64_t generation;
    uint64_t working_revision;
    uint64_t publication_sequence;
    brom_preset_snapshot working;
    brom_preset_snapshot reference;
    brom_preset_snapshot baseline;
    brom_preset_identity working_identity;
    brom_preset_identity reference_identity;
    uint32_t audible_side;
    bool changed;
    char status[BROM_PRESET_STATUS_BYTES];
} brom_preset_view;

typedef struct brom_preset_commit {
    brom_preset_view view;
    bool apply_parameters;
    bool reset_voices;
    bool publish_view;
} brom_preset_commit;

typedef struct brom_preset_tx brom_preset_tx;

int brom_preset_schema_read(void *engine, void *out_schema);
int brom_preset_prepare_text(void *engine, void *bytes, size_t size,
                             void *out_snapshot, void *out_identity);
size_t brom_preset_format_snapshot(void *engine, void *snapshot,
                                   char *name, void *out, size_t capacity);
int brom_preset_decode_legacy(void *engine, void *bytes, size_t size,
                              void *out_snapshot);
void brom_preset_apply_validated(void *engine, void *core,
                                 void *snapshot);

#ifdef __cplusplus
}
#endif
#endif
