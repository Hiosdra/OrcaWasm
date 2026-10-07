#ifndef ONESLICER_SLICER_API_H
#define ONESLICER_SLICER_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * API 0.7.0-pre.1, C binding: the low-level binding of one-slicer-api. Hosts
 * use the TypeScript binding; an Emscripten engine implementing this header
 * embeds the reference glue (js/glue) to provide it. See docs/binding-c.md.
 *
 * One exact API version per release; core behavior is required. Every
 * declaration remains in the ABI. Optional operations may be stubs returning
 * ONESLICER_ERR_UNSUPPORTED; symbol presence is not capability support.
 * Every JSON payload carries schemaVersion equal to the API version string;
 * engines reject any other value. Every engine accepts ordinary project
 * manifests with empty modifierVolumes.
 * Nonempty modifierVolumes requires the advertised optional capability.
 */
#define ONESLICER_API_VERSION_MAJOR 0
#define ONESLICER_API_VERSION_MINOR 7
#define ONESLICER_API_VERSION_PATCH 0
#define ONESLICER_API_VERSION_STRING "0.7.0-pre.1"

typedef void* oneslicer_session_t;
typedef int32_t oneslicer_status_t;

/*
 * The stage string is borrowed and valid only for the duration of the
 * callback. The engine invokes the callback on the thread that called the
 * operation; an engine reporting progress from worker threads proxies the
 * call to that thread. The callback must be non-blocking and must not
 * re-enter the session. percent is 0..100, or -1 when no useful percentage is known.
 */
typedef void (*oneslicer_progress_callback_t)(
    int32_t percent,
    const char* stage_utf8,
    void* user_data
);

enum {
    ONESLICER_OK = 0,
    ONESLICER_ERR_INVALID_ARGUMENT = -1,
    ONESLICER_ERR_CONFIG = -2,
    ONESLICER_ERR_INPUT_IO = -3,
    ONESLICER_ERR_INPUT_FORMAT = -4,
    ONESLICER_ERR_EMPTY_INPUT = -5,
    ONESLICER_ERR_VALIDATION = -6,
    ONESLICER_ERR_SLICE = -7,
    ONESLICER_ERR_OUTPUT = -8,
    ONESLICER_ERR_INTERNAL = -9,
    ONESLICER_ERR_UNSUPPORTED = -10,
    ONESLICER_ERR_CANCELLED = -11,
    ONESLICER_ERR_NO_DATA = -12
};

oneslicer_session_t oneslicer_session_create(void);
void oneslicer_session_destroy(oneslicer_session_t session);

oneslicer_status_t oneslicer_init(
    oneslicer_session_t session,
    const uint8_t* config_data,
    uint32_t config_len
);

/* Replace the active configuration and logical project with a full profile. */
oneslicer_status_t oneslicer_init_profile(
    oneslicer_session_t session,
    const char* format_utf8,
    uint32_t format_len,
    const uint8_t* profile_data,
    uint32_t profile_len
);

/*
 * Apply one engine-native profile fragment to the active configuration.
 * The accepted format identifiers are listed in capabilities.configuration.profileApplyFormats.
 * A successful call preserves the logical project and its geometry, while
 * invalidating slice/export result assets and statistics derived from the
 * previous configuration. Calls are host-serialized with other session
 * operations and may not race a slice.
 */
oneslicer_status_t oneslicer_apply_profile(
    oneslicer_session_t session,
    const char* format_utf8,
    uint32_t format_len,
    const uint8_t* profile_data,
    uint32_t profile_len
);

oneslicer_status_t oneslicer_set_progress_callback(
    oneslicer_session_t session,
    oneslicer_progress_callback_t callback,
    void* user_data
);
oneslicer_status_t oneslicer_cancel(oneslicer_session_t session);

oneslicer_status_t oneslicer_project_set_objects(
    oneslicer_session_t session,
    const uint8_t* object_blob,
    uint32_t object_blob_len,
    const uint8_t* manifest_json,
    uint32_t manifest_len
);
oneslicer_status_t oneslicer_project_get_manifest(
    oneslicer_session_t session,
    uint8_t** out_json,
    uint32_t* out_len
);
/* Optional project.preview.stepFaces. Every engine exports this symbol;
 * engines without mapped STEP previews return ONESLICER_ERR_UNSUPPORTED. */
oneslicer_status_t oneslicer_project_get_preview(
    oneslicer_session_t session,
    const char* mesh_id_utf8,
    uint32_t mesh_id_len,
    uint8_t** out_descriptor_json,
    uint32_t* out_descriptor_len,
    uint8_t** out_preview_blob,
    uint32_t* out_preview_blob_len
);
oneslicer_status_t oneslicer_project_prepare(
    oneslicer_session_t session,
    const uint8_t* request_json,
    uint32_t request_len,
    uint8_t** out_manifest_json,
    uint32_t* out_len
);
oneslicer_status_t oneslicer_project_slice(
    oneslicer_session_t session,
    const uint8_t* request_json,
    uint32_t request_len,
    uint8_t** out_result_json,
    uint32_t* out_len
);
oneslicer_status_t oneslicer_project_get_asset(
    oneslicer_session_t session,
    const char* asset_id_utf8,
    uint32_t asset_id_len,
    uint8_t** out_data,
    uint32_t* out_len
);
oneslicer_status_t oneslicer_project_export(
    oneslicer_session_t session,
    const char* format_utf8,
    uint32_t format_len,
    const uint8_t* options_json,
    uint32_t options_len,
    uint8_t** out_result_json,
    uint32_t* out_len
);

oneslicer_status_t oneslicer_obj_to_stl(
    const uint8_t* obj_data,
    uint32_t obj_len,
    uint8_t** out_stl,
    uint32_t* out_len
);
oneslicer_status_t oneslicer_cad_to_stl(
    const uint8_t* cad_data,
    uint32_t cad_len,
    uint8_t** out_stl,
    uint32_t* out_len
);
/* Optional format.threeMfToStl: merged geometry only, no native settings.
 * Every plate's build items are merged into one mesh in the source scene's
 * coordinates, with transforms (including mirroring) applied. */
oneslicer_status_t oneslicer_three_mf_to_stl(
    const uint8_t* mf_data,
    uint32_t mf_len,
    uint8_t** out_stl,
    uint32_t* out_len
);
oneslicer_status_t oneslicer_get_capabilities(uint8_t** out_json, uint32_t* out_len);
const char* oneslicer_last_error(oneslicer_session_t session);
void oneslicer_free(void* ptr);

#ifdef __cplusplus
}
#endif

#endif
