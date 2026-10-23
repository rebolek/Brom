#ifndef BROM_PLUGIN_VST3_EFFECT_EDITOR_H
#define BROM_PLUGIN_VST3_EFFECT_EDITOR_H

/* The editor hook of the generic VST3 effect shell (effect_vst3.cpp).
 *
 * A product with an editor supplies the brom_vst3_editor_* functions, in a
 * source its VST3 project passes to brom_add_effect_vst3 (EDITOR_SOURCES),
 * which also defines BROM_VST3_EDITOR so the shell calls them. The shell
 * supplies the brom_vst3_host_* functions the editor calls back. This mirrors
 * the CLAP shell's recoil_clap_editor_* adapter: the same product editor sits
 * behind either, and only the adapter differs.
 *
 * `host` is opaque to the product and only ever passed back. `core` is the
 * controller's private effect instance, which mirrors every parameter value
 * the host sees; an editor reads it on the main thread. Everything here runs
 * on the main (UI) thread. */

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* Product side. create returns null on failure; attach returns nonzero on
 * success. preset_changed is told the committed factory program, or -1. */
void *brom_vst3_editor_create(void *host, void *core);
void brom_vst3_editor_destroy(void *editor);
int brom_vst3_editor_attach(void *editor, void *native_parent, int width, int height);
void brom_vst3_editor_detach(void *editor);
void brom_vst3_editor_tick(void *editor);
void brom_vst3_editor_preset_changed(void *editor, int32_t index);

/* Shell side. edit_param takes engineering units and a gesture phase:
 * 0 begin, 1 perform, 2 end; the shell writes the value and tells the host. */
void brom_vst3_host_edit_param(void *host, uint32_t id, double value, int phase);
uint32_t brom_vst3_host_preset_count(void *host);
int32_t brom_vst3_host_current_preset(void *host);
int brom_vst3_host_preset_name(void *host, uint32_t index, char *out, uint32_t capacity);
int brom_vst3_host_load_preset(void *host, uint32_t index);

#if defined(__cplusplus)
}
#endif

#endif
