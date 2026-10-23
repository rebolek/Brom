#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loos_editor loos_editor;
typedef void (*loos_editor_edit_fn)(void *user, uint32_t parameter, double value, int phase);

loos_editor *loos_editor_create(void *core, loos_editor_edit_fn edit, void *user);
void loos_editor_destroy(loos_editor *editor);
bool loos_editor_attach(loos_editor *editor, void *native_parent, int width, int height);
void loos_editor_detach(loos_editor *editor);
void loos_editor_preset_changed(loos_editor *editor, int32_t index);
void loos_editor_tick(loos_editor *editor);

#ifdef __cplusplus
}
#endif
