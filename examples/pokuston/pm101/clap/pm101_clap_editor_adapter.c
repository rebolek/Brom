#include "examples/pokuston/pm101/clap/pm101_clap_editor_adapter.h"
#include "examples/pokuston/pm101/pm101-editor.h"
#include "examples/pokuston/pm101/editor.h"
#include "examples/pokuston/pm101/pm101.h"

/* Exported by clap_entry.rcl. `instance` is the wrapper's plugin_data. */
extern int recoil_clap_plugin_edit_param(
    void *instance, uint32_t param_id, double value, int phase);
extern uint32_t recoil_clap_factory_preset_count(void);
extern int recoil_clap_factory_preset_name(uint32_t index, void *out,
                                            uint32_t capacity);
extern int32_t recoil_clap_plugin_current_factory_preset(void *instance);
extern int recoil_clap_plugin_load_factory_preset_index(void *instance,
                                                         uint32_t index);

uint32_t pm101_editor_preset_count(void *instance) {
    (void)instance;
    return recoil_clap_factory_preset_count();
}

int32_t pm101_editor_current_preset(void *instance) {
    return recoil_clap_plugin_current_factory_preset(instance);
}

const char *pm101_editor_preset_name(void *instance, uint32_t index) {
    (void)instance;
    static char name[256];
    name[0] = '\0';
    recoil_clap_factory_preset_name(index, name, sizeof(name));
    return name;
}

bool pm101_editor_load_preset(void *instance, uint32_t index) {
    return recoil_clap_plugin_load_factory_preset_index(instance, index);
}

static void pm101_clap_edit(void *instance, uint32_t parameter, double value, int phase) {
    recoil_clap_plugin_edit_param(instance, parameter, value, phase);
}

void *recoil_clap_editor_create(void *instance, void *core) {
    return pm101_editor_create(core, pm101_clap_edit, instance);
}

void recoil_clap_editor_destroy(void *editor) {
    pm101_editor_destroy((pm101_editor *)editor);
}

bool recoil_clap_editor_attach(void *editor, void *native_parent, int width, int height) {
    return pm101_editor_attach((pm101_editor *)editor, native_parent, width, height);
}

void recoil_clap_editor_detach(void *editor) {
    pm101_editor_detach((pm101_editor *)editor);
}

void recoil_clap_editor_tick(void *editor) {
    pm101_editor_tick((pm101_editor *)editor);
}

void recoil_clap_editor_preset_changed(void *editor, int32_t index) {
    pm101_editor_preset_changed((pm101_editor *)editor, index);
}
