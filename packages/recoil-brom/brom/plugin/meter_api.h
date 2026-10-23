#pragma once

#include <stdint.h>

/* Fixed C ABI copied snapshot shared by Brom's Recoil service and host adapters. */
typedef struct brom_meter_snapshot {
    double excitation;
    double output;
    int64_t excitation_hold_ns;
    int64_t output_hold_ns;
    int64_t observed_ns;
    uint64_t sequence;
    uint64_t generation;
} brom_meter_snapshot;

#if defined(__cplusplus)
static_assert(sizeof(brom_meter_snapshot) == 56,
              "Brom meter snapshot ABI must remain 56 bytes");
#endif
