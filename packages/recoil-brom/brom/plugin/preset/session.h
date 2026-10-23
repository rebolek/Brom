#ifndef BROM_PLUGIN_PRESET_SESSION_H
#define BROM_PLUGIN_PRESET_SESSION_H

#include "api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BROM_PRESET_SESSION_MAX_BYTES 16384u
/* The envelope: magic, version, flags, payload size, checksum, then the payload. The
 * stream framing in vst3/state_stream.hpp reads the same header. */
#define BROM_PRESET_SESSION_HEADER_BYTES 16u
#define BROM_PRESET_SESSION_MAGIC "BPSN"
#define BROM_PRESET_SESSION_PAYLOAD_SIZE_OFFSET 8u

brom_preset_result brom_preset_session_encode(
    const brom_preset_schema *schema, const char *product_key,
    const brom_preset_view *view, uint8_t *out, size_t capacity,
    size_t *written);
brom_preset_result brom_preset_session_decode(
    const uint8_t *bytes, size_t size, const brom_preset_schema *schema,
    const char *product_key, brom_preset_view *out_view);
brom_preset_result brom_preset_session_migrate_legacy(
    const brom_preset_schema *schema, const brom_preset_snapshot *snapshot,
    const brom_preset_identity *identity, brom_preset_view *out_view);

#ifdef __cplusplus
}
#endif
#endif
