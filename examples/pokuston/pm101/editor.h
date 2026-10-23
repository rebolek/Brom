#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pm101_editor pm101_editor;
typedef void (*pm101_editor_edit_fn)(void *user, uint32_t parameter, double value, int phase);

pm101_editor *pm101_editor_create(void *core, pm101_editor_edit_fn edit, void *user);
void pm101_editor_destroy(pm101_editor *editor);
bool pm101_editor_attach(pm101_editor *editor, void *native_parent, int width, int height);
void pm101_editor_detach(pm101_editor *editor);
void pm101_editor_preset_changed(pm101_editor *editor, int32_t index);
void pm101_editor_tick(pm101_editor *editor);

#ifdef __cplusplus
}
#endif
