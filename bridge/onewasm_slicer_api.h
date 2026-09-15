#ifndef ONEWASM_SLICER_API_H
#define ONEWASM_SLICER_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Version 0.3 clean-break contract. The exported symbol names intentionally
 * stay in the onewasm_ namespace, but their meaning is defined by this
 * version of the header.
 */
#define ONEWASM_API_VERSION_MAJOR 0
#define ONEWASM_API_VERSION_MINOR 3
#define ONEWASM_API_VERSION_PATCH 0
#define ONEWASM_API_VERSION_STRING "0.3.0"

typedef void* onewasm_session_t;
typedef int32_t onewasm_status_t;

/*
 * The stage string is borrowed and valid only for the duration of the
 * callback. The callback must be non-blocking and must not re-enter the
 * session. percent is 0..100, or -1 when no useful percentage is known.
 */
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

/*
 * Replace the logical project's mesh objects and placement with a neutral
 * manifest. Mesh data ranges refer to the object_blob, so large meshes do not
 * pass through base64/JSON. Native settings already loaded in the session
 * remain active.
 */
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

onewasm_status_t onewasm_get_capabilities(
    uint8_t** out_json,
    uint32_t* out_len
);

const char* onewasm_last_error(onewasm_session_t session);
void onewasm_free(void* ptr);

#ifdef __cplusplus
}
#endif

#endif
