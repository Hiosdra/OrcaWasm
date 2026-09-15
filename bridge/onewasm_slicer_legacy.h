#ifndef ONEWASM_SLICER_LEGACY_H
#define ONEWASM_SLICER_LEGACY_H

/*
 * Internal adapter helpers retained by the bridge implementation. These
 * declarations are deliberately separate from the canonical 0.3 header. The
 * symbols are not part of the 0.3 contract; project operations use a few of
 * them internally, and the PoC still uses the geometry-only 3MF converter to
 * populate its model preview.
 */

#include "onewasm_slicer_api.h"

enum {
    ONEWASM_PLATE_AUTO_ORIENT = 1,
    ONEWASM_PLATE_ARRANGE = 2
};

#ifdef __cplusplus
extern "C" {
#endif

static onewasm_status_t legacy_slice_stl(
    onewasm_session_t session,
    const uint8_t* stl_data,
    uint32_t stl_len,
    uint8_t** out_gcode,
    uint32_t* out_len
);

static onewasm_status_t legacy_slice_stl_multi(
    onewasm_session_t session,
    const uint8_t* stl_blob,
    uint32_t stl_blob_len,
    const uint32_t* object_offsets,
    uint32_t object_count,
    const int32_t* extruder_ids,
    const float* object_transforms,
    uint8_t** out_gcode,
    uint32_t* out_len
);

static onewasm_status_t legacy_prepare_plate(
    onewasm_session_t session,
    const uint8_t* stl_blob,
    uint32_t stl_blob_len,
    const uint32_t* object_offsets,
    uint32_t object_count,
    const float* object_transforms,
    int32_t operation,
    uint8_t** out_transforms_json,
    uint32_t* out_len
);

static onewasm_status_t legacy_write_3mf(
    onewasm_session_t session,
    const uint8_t* stl_data,
    uint32_t stl_len,
    uint8_t** out_3mf,
    uint32_t* out_len
);

onewasm_status_t onewasm_read_3mf(
    const uint8_t* mf_data,
    uint32_t mf_len,
    uint8_t** out_stl,
    uint32_t* out_stl_len
);

static onewasm_status_t legacy_get_last_statistics(
    onewasm_session_t session,
    uint8_t** out_json,
    uint32_t* out_len
);

#ifdef __cplusplus
}
#endif

#endif
