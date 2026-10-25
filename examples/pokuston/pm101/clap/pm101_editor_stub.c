/* PM-101 has no editor yet (has-editor: false), but the shared CLAP support
 * references these six adapter entry points, so they must link. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void *recoil_clap_editor_create(void *instance, void *core) {
    (void)instance;
    (void)core;
    return NULL;
}
void recoil_clap_editor_destroy(void *editor) { (void)editor; }
bool recoil_clap_editor_attach(void *editor, void *native_parent,
                               int width, int height) {
    (void)editor;
    (void)native_parent;
    (void)width;
    (void)height;
    return false;
}
void recoil_clap_editor_detach(void *editor) { (void)editor; }
void recoil_clap_editor_tick(void *editor) { (void)editor; }
void recoil_clap_editor_preset_changed(void *editor, int32_t index) {
    (void)editor;
    (void)index;
}
