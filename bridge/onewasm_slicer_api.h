#ifndef ONEWASM_SLICER_API_H
#define ONEWASM_SLICER_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ONEWASM_API_VERSION_MAJOR 0
#define ONEWASM_API_VERSION_MINOR 2
#define ONEWASM_API_VERSION_PATCH 0
#define ONEWASM_API_VERSION_STRING "0.2.0"

typedef void* onewasm_session_t;
typedef int32_t onewasm_status_t;

typedef void (*onewasm_progress_callback_t)(
    int32_t percent,
    const char* stage_utf8,
    void* user_data
);

enum {
    ONEWASM_OK = 0,
    ONEWASM_ERR_INVALID_ARGUMENT = -1,
    ONEWASM_ERR_CONFIG = -2,
    ONEWASM_ERR_INPUT_IO = -3,
    ONEWASM_ERR_INPUT_FORMAT = -4,
    ONEWASM_ERR_EMPTY_INPUT = -5,
    ONEWASM_ERR_VALIDATION = -6,
    ONEWASM_ERR_SLICE = -7,
    ONEWASM_ERR_OUTPUT = -8,
    ONEWASM_ERR_INTERNAL = -9,
    ONEWASM_ERR_UNSUPPORTED = -10,
    ONEWASM_ERR_CANCELLED = -11,
    ONEWASM_ERR_NO_DATA = -12
};

enum {
    ONEWASM_PLATE_AUTO_ORIENT = 1,
    ONEWASM_PLATE_ARRANGE = 2
};

#define ONEWASM_OBJECT_TRANSFORM_STRIDE 11

onewasm_session_t onewasm_session_create(void);
void onewasm_session_destroy(onewasm_session_t session);

onewasm_status_t onewasm_init(
    onewasm_session_t session,
    const uint8_t* config_data,
    uint32_t config_len
);

onewasm_status_t onewasm_init_profile(
    onewasm_session_t session,
    const char* format_utf8,
    uint32_t format_len,
    const uint8_t* profile_data,
    uint32_t profile_len
);

onewasm_status_t onewasm_set_progress_callback(
    onewasm_session_t session,
    onewasm_progress_callback_t callback,
    void* user_data
);

onewasm_status_t onewasm_cancel(onewasm_session_t session);

onewasm_status_t onewasm_slice_stl(
    onewasm_session_t session,
    const uint8_t* stl_data,
    uint32_t stl_len,
    uint8_t** out_gcode,
    uint32_t* out_len
);

onewasm_status_t onewasm_slice_stl_multi(
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

onewasm_status_t onewasm_prepare_plate(
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

onewasm_status_t onewasm_obj_to_stl(
    const uint8_t* obj_data,
    uint32_t obj_len,
    uint8_t** out_stl,
    uint32_t* out_len
);

onewasm_status_t onewasm_cad_to_stl(
    const uint8_t* cad_data,
    uint32_t cad_len,
    uint8_t** out_stl,
    uint32_t* out_len
);

onewasm_status_t onewasm_write_3mf(
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

onewasm_status_t onewasm_get_capabilities(
    uint8_t** out_json,
    uint32_t* out_len
);

onewasm_status_t onewasm_get_last_statistics(
    onewasm_session_t session,
    uint8_t** out_json,
    uint32_t* out_len
);

/* Draft 0.3 project surface. */
onewasm_status_t onewasm_project_set_objects(
    onewasm_session_t session,
    const uint8_t* object_blob,
    uint32_t object_blob_len,
    const uint8_t* manifest_json,
    uint32_t manifest_len
);

onewasm_status_t onewasm_project_get_manifest(
    onewasm_session_t session,
    uint8_t** out_json,
    uint32_t* out_len
);

onewasm_status_t onewasm_project_prepare(
    onewasm_session_t session,
    const uint8_t* request_json,
    uint32_t request_len,
    uint8_t** out_manifest_json,
    uint32_t* out_len
);

onewasm_status_t onewasm_project_slice(
    onewasm_session_t session,
    const uint8_t* request_json,
    uint32_t request_len,
    uint8_t** out_result_json,
    uint32_t* out_len
);

onewasm_status_t onewasm_project_get_asset(
    onewasm_session_t session,
    const char* asset_id_utf8,
    uint32_t asset_id_len,
    uint8_t** out_data,
    uint32_t* out_len
);

onewasm_status_t onewasm_project_export(
    onewasm_session_t session,
    const char* format_utf8,
    uint32_t format_len,
    const uint8_t* options_json,
    uint32_t options_len,
    uint8_t** out_result_json,
    uint32_t* out_len
);

const char* onewasm_last_error(onewasm_session_t session);
void onewasm_free(void* ptr);

#ifdef __cplusplus
}
#endif

#endif
