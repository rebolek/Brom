#ifndef BROM_PLUGIN_PRESET_SAVE_H
#define BROM_PLUGIN_PRESET_SAVE_H

#include "api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BROM_PRESET_SAVE_PATH_BYTES 2048u
#define BROM_PRESET_SAVE_TEXT_BYTES 16384u

typedef enum brom_preset_save_confirmation {
    BROM_PRESET_SAVE_NO_CONFIRMATION = 0,
    BROM_PRESET_SAVE_CONFIRM_OVERWRITE = 1
} brom_preset_save_confirmation;

typedef struct brom_preset_save_ticket {
    uint64_t ticket_id;
    uint64_t generation;
    uint64_t working_revision;
    brom_preset_snapshot snapshot;
    brom_preset_identity target;
    brom_preset_identity saved_identity;
    char plugin_key[BROM_PRESET_KEY_BYTES];
    char display_name[BROM_PRESET_NAME_BYTES];
    char destination[BROM_PRESET_SAVE_PATH_BYTES];
    char text[BROM_PRESET_SAVE_TEXT_BYTES];
    uint64_t observed_device;
    uint64_t observed_inode;
    uint64_t observed_size;
    int64_t observed_mtime_ns;
    bool destination_existed;
} brom_preset_save_ticket;

typedef struct brom_preset_save_completion {
    uint64_t ticket_id;
    uint64_t generation;
    uint64_t working_revision;
    brom_preset_snapshot snapshot;
    brom_preset_identity identity;
} brom_preset_save_completion;

#ifdef BROM_PRESET_TESTING
typedef enum brom_preset_save_test_stage {
    BROM_PRESET_SAVE_FAIL_NONE = 0,
    BROM_PRESET_SAVE_FAIL_OPEN = 1,
    BROM_PRESET_SAVE_FAIL_WRITE = 2,
    BROM_PRESET_SAVE_FAIL_FLUSH = 3,
    BROM_PRESET_SAVE_FAIL_CLOSE = 4,
    BROM_PRESET_SAVE_FAIL_INSTALL = 5
} brom_preset_save_test_stage;
void brom_preset_save_test_fail(brom_preset_save_test_stage stage);
#endif

brom_preset_result brom_preset_save_capture(
    const brom_preset_schema *schema, const brom_preset_view *view,
    uint64_t expected_generation, const char *directory, const char *plugin_key,
    const brom_preset_identity *target, const char *display_name,
    brom_preset_save_ticket *out_ticket);
brom_preset_result brom_preset_save_write(
    const brom_preset_save_ticket *ticket,
    brom_preset_save_confirmation confirmation,
    brom_preset_save_completion *out_completion);

#ifdef __cplusplus
}
#endif
#endif
