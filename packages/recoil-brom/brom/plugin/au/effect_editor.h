#ifndef BROM_PLUGIN_AU_EFFECT_EDITOR_H
#define BROM_PLUGIN_AU_EFFECT_EDITOR_H

/* The editor hook of the generic AU effect shell (effect_au.mm).
 *
 * The AU twin of brom/plugin/vst3/effect_editor.h, function for function. A
 * product with an editor supplies the brom_au_editor_* functions, in a source
 * its AU project passes to brom_add_effect_au (EDITOR_SOURCES), which also
 * defines BROM_AU_EDITOR so the shell publishes a Cocoa view factory
 * (kAudioUnitProperty_CocoaUI) and calls them. The shell supplies the
 * brom_au_host_* functions the editor calls back. The same product editor sits
 * behind the CLAP, VST3 and AU shells; only the adapter differs.
 *
 * `host` is opaque to the product and only ever passed back. `core` is the
 * shell's private effect instance, which mirrors every parameter value the
 * host sees (within one 33 ms main-thread tick); an editor reads it on the
 * main thread. Everything here runs on the main (UI) thread. `native_parent`
 * is the NSView the shell's Cocoa view hands the editor. */

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* Product side. create returns null on failure; attach returns nonzero on
 * success. preset_changed is told the committed factory program, or -1. */
void *brom_au_editor_create(void *host, void *core);
void brom_au_editor_destroy(void *editor);
int brom_au_editor_attach(void *editor, void *native_parent, int width, int height);
void brom_au_editor_detach(void *editor);
void brom_au_editor_tick(void *editor);
void brom_au_editor_preset_changed(void *editor, int32_t index);

/* Shell side. edit_param takes engineering units and a gesture phase:
 * 0 begin, 1 perform, 2 end; the shell writes the value and tells the host
 * (AUParameterSet, plus the begin/end gesture events). */
void brom_au_host_edit_param(void *host, uint32_t id, double value, int phase);
uint32_t brom_au_host_preset_count(void *host);
int32_t brom_au_host_current_preset(void *host);
int brom_au_host_preset_name(void *host, uint32_t index, char *out, uint32_t capacity);
int brom_au_host_load_preset(void *host, uint32_t index);

#if defined(__cplusplus)
}
#endif

#endif
