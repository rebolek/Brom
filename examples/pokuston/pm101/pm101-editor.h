#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Factory preset navigation supplied by each host-format adapter. The opaque
 * host context is the editor callback user pointer, not the DSP core. */
uint32_t pm101_editor_preset_count(void *host_context);
int32_t pm101_editor_current_preset(void *host_context);
const char *pm101_editor_preset_name(void *host_context, uint32_t index);
bool pm101_editor_load_preset(void *host_context, uint32_t index);

#ifdef __cplusplus
}
#endif
