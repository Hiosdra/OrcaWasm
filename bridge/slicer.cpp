/**
 * orca-wasm WASM bridge — clean-room implementation.
 *
 * Exports C-linkage symbols consumed by the JavaScript runtime:
 *   onewasm_session_create()                                      → opaque session handle (0 = alloc failed)
 *   onewasm_session_destroy(session)
 *   onewasm_init(session, config, len)                             → 0 = ok
 *   onewasm_slice_stl(session, stl, stlLen, outPtr, outLen)        → 0 = ok
 *   onewasm_slice_stl_multi(session, all, allLen, offsets, n,
 *                           extruderIds, transforms, out, outLen)  → 0 = ok
 *   onewasm_prepare_plate(session, all, allLen, offsets, n,
 *                         transforms, operation, out, outLen)     → 0 = ok
 *   onewasm_obj_to_stl(obj, objLen, outPtr, outLen)                → 0 = ok
 *   onewasm_cad_to_stl(cad, cadLen, outPtr, outLen)                → 0 = ok (STEP)
 *   onewasm_write_3mf(session, stl, stlLen, outPtr, outLen)        → 0 = ok
 *   onewasm_read_3mf(mf, mfLen, outStl, outStlLen)                 → 0 = ok
 *   onewasm_get_capabilities(outJson, outLen)
 *   onewasm_get_last_statistics(session, outJson, outLen)
 *   onewasm_cancel(session)
 *   onewasm_free(ptr)
 *   onewasm_last_error(session)                                   → null-terminated UTF-8 string
 *
 * onewasm_obj_to_stl / onewasm_cad_to_stl / onewasm_read_3mf are pure format conversions
 * — they never touch slicer config state, so they take no session handle.
 *
 * Error codes for the session-bound operations follow one-wasm-slicer-api 0.2:
 *   -1  invalid / uninitialized state (includes a null/invalid session handle)
 *   -2  JSON parse failure
 *   -3  STL write to MEMFS failed
 *   -4  STL load failed
 *   -5  empty model
 *   -6  print validation failed
 *   -7  slicing error
 *   -8  gcode export failed (or, for onewasm_write_3mf, 3MF export failed)
 *   -9  unexpected C++ exception
 *   -11 active operation was cancelled
 *   -12 no optional result is available
 *
 * onewasm_read_3mf reuses the same -1/-3/-4/-5/-8/-9 meanings (input write /
 * 3MF load / no geometry / STL export / exception), decoded via
 * onewasm_last_error(0) like onewasm_obj_to_stl / onewasm_cad_to_stl.
 */

#include "onewasm_slicer_api.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <stdexcept>
#include <memory>
#include <map>
#include <mutex>
#include <new>
#include <limits>
#include <set>
#include <utility>
#include <vector>
#include <sys/stat.h>

#include <emscripten.h>

// OrcaSlicer core
// (note: an earlier attempt to cap oneTBB via tbb::global_control lived here;
// it deadlocked — see wasm/CMakeLists.txt's PTHREAD_POOL_SIZE comment and
// the multithreaded build notes. The pool is sized to hardware_concurrency
// instead.)
#include "libslic3r/libslic3r.h"
#include "libslic3r/Model.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/Orient.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Format/STEP.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Semver.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/core.hpp>

// nlohmann/json — available as a bundled dep inside OrcaSlicer
#include <nlohmann/json.hpp>

// Boost.Log is unusable in this build: no sink is ever registered (utils.cpp's
// file sink needs set_logging_file(), which only the desktop CLI calls), so
// every record that passes the severity filter — left at `warning` by
// utils.cpp's RunOnInit — goes to Boost.Log's *default* sink. Under this
// single-threaded (BOOST_LOG_NO_THREADS) Emscripten build that path is both
// broken and expensive: it non-deterministically traps with "memory access
// out of bounds" inside core::push_record_move() (first seen as the
// nozzle_info.json incident — see ensure_nozzle_info_json() below — and again
// with the Voron Design Cube, where Arachne's per-edge warning storm during
// Voronoi-diagram repair either traps or burns most of the slice's CPU in
// record formatting, turning a multi-second slice into a multi-minute one).
// The records are unobservable either way, so disable the logging core
// outright instead of dodging individual call sites.
static struct DisableBoostLogOnInit {
    DisableBoostLogOnInit() { boost::log::core::get()->set_logging_enabled(false); }
} g_disable_boost_log;

// ── module state ─────────────────────────────────────────────────────────────
// Read-only template of OrcaSlicer's built-in defaults — constructed once,
// never mutated afterwards, so it's safe to share across every session.
static Slic3r::FullPrintConfig g_defaults;
static std::atomic<std::uint64_t> g_next_session_id{1};

// Per-session engine state, behind an opaque handle instead of process-wide
// statics. Today the JS side still creates exactly one session per worker
// and uses it for that worker's entire lifetime, so behaviour is unchanged —
// this only removes the structural blocker that made it unsafe for a future
// caller (a Node CLI batch-processing many jobs, or a worker pool) to hold
// more than one independent slicer session in the same WASM instance.
struct OrcSession {
    const std::uint64_t id = g_next_session_id.fetch_add(1, std::memory_order_relaxed);
    Slic3r::DynamicPrintConfig config;
    bool initialized = false;
    std::string last_error;
    // Bed centre in mm — computed from bed_size_x / bed_size_y in the config JSON.
    // Defaults to centre of a 256×256 mm bed (same as the historical hardcoded value).
    double bed_cx = 128.0;
    double bed_cy = 128.0;
    // "rectangle" or "circle" — read from bed_shape in the config JSON.
    std::string bed_shape = "rectangle";
    // Opt-in override of the engine's mixed-nozzle-temperature guard, matching
    // desktop OrcaSlicer's "Remove mixed temperature restriction" preference.
    // Off by default (the guard exists to prevent nozzle clogging / damage);
    // when set, onewasm_slice_stl / onewasm_slice_stl_multi call
    // Print::set_check_multi_filaments_compatibility(false) before validate().
    // See issue #164.
    bool remove_mixed_temp_restriction = false;
    // Variable (adaptive) layer height, matching desktop OrcaSlicer's Adaptive
    // tool: when on, onewasm_slice_stl / onewasm_slice_stl_multi compute a per-object layer
    // height profile from the mesh geometry (layer_height_profile_adaptive)
    // before slicing, so detailed regions get thinner layers and flat regions
    // thicker ones. Off by default (a fixed layer height is the engine default
    // and what every preset expects). Not native engine config keys — read
    // here as pseudo-keys (like bed_size_* above) and applied in
    // apply_adaptive_layer_height(). See issue #138.
    bool  adaptive_layer_height = false;
    // Quality/speed factor forwarded verbatim to layer_height_profile_adaptive
    // (0..1, engine's own range; desktop's slider default is 0.5). Lower =
    // finer detail (thinner layers, smaller cusp error); higher = faster
    // (thicker layers).
    float adaptive_layer_height_quality = 0.5f;
    onewasm_progress_callback_t progress_callback = nullptr;
    void* progress_user_data = nullptr;
    // `onewasm_cancel` is the one session operation allowed to run while a
    // slice is active. All other session operations remain host-serialized.
    std::mutex control_mutex;
    std::shared_ptr<struct ActiveSlice> active_slice;
    std::string last_statistics_json;
    bool has_last_statistics = false;
    // Draft 0.3 project state. The host-provided mesh blob is copied into the
    // session; the manifest is neutral and never escapes as Orca types.
    nlohmann::json project_manifest;
    std::vector<std::uint8_t> project_object_blob;
    std::map<std::string, std::string> project_assets;
    // The original native project is retained for a lossless clean export.
    // Once the neutral manifest is mutated, preservation=require must fail
    // instead of quietly dropping Orca-specific ZIP entries.
    std::vector<std::uint8_t> native_project_blob;
    bool native_project_dirty = false;
    int progress_base = 0;
    int progress_span = 100;
    // A project slice may call the legacy single-plate entry point several
    // times. Keep cancellation intent across the small gaps between those
    // calls; the active native operation is still cancelled through
    // ActiveSlice when one exists.
    std::atomic<bool> project_cancel_requested{false};
};

struct ProjectMeshInput {
    std::string id;
    bool has_data_range = false;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

struct ProjectObjectInput {
    std::string id;
    std::string mesh_id;
    std::string label;
    int extruder_id = 0;
};

struct ProjectInstanceInput {
    std::string id;
    std::string object_id;
    std::string plate_id;
    std::array<double, 16> matrix{};
};

struct ProjectManifestInput {
    std::vector<std::string> plate_ids;
    std::vector<ProjectMeshInput> meshes;
    std::vector<ProjectObjectInput> objects;
    std::vector<ProjectInstanceInput> instances;
};

static nlohmann::json empty_project_manifest() {
    return nlohmann::json{
        {"schemaVersion", "0.3"},
        {"plates", nlohmann::json::array()},
        {"meshes", nlohmann::json::array()},
        {"objects", nlohmann::json::array()},
        {"instances", nlohmann::json::array()},
    };
}

static void clear_project_outputs(OrcSession& session) {
    std::lock_guard<std::mutex> lock(session.control_mutex);
    session.project_assets.clear();
    session.last_statistics_json.clear();
    session.has_last_statistics = false;
}

static void reset_project(OrcSession& session) {
    session.project_manifest = empty_project_manifest();
    session.project_object_blob.clear();
    session.native_project_blob.clear();
    session.native_project_dirty = false;
    session.project_cancel_requested.store(false, std::memory_order_release);
    clear_project_outputs(session);
}

// The native PrintBase cancellation flag is atomic, but the pointer to the
// active Print is not. Keep both behind one small operation object so a cancel
// request cannot call into a Print while the Print is being constructed or
// destroyed. The shared_ptr also makes the idle/cancel race safe: a caller may
// retain the operation object while the slicing function is unwinding.
struct ActiveSlice {
    std::atomic<bool> cancel_requested{false};
    std::mutex print_mutex;
    Slic3r::PrintBase* print = nullptr;

    void attach(Slic3r::PrintBase& value) {
        std::lock_guard<std::mutex> lock(print_mutex);
        print = &value;
        if (cancel_requested.load(std::memory_order_acquire))
            print->cancel();
    }

    void detach() {
        std::lock_guard<std::mutex> lock(print_mutex);
        print = nullptr;
    }

    void cancel() {
        cancel_requested.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(print_mutex);
        if (print)
            print->cancel();
    }
};

class ActiveSliceGuard {
public:
    explicit ActiveSliceGuard(OrcSession& session)
        : session_(session), operation_(std::make_shared<ActiveSlice>()) {
        std::lock_guard<std::mutex> lock(session_.control_mutex);
        if (session_.active_slice)
            return;
        session_.active_slice = operation_;
        session_.last_statistics_json.clear();
        session_.has_last_statistics = false;
        registered_ = true;
    }

    ~ActiveSliceGuard() {
        if (!registered_)
            return;
        operation_->detach();
        std::lock_guard<std::mutex> lock(session_.control_mutex);
        if (session_.active_slice == operation_)
            session_.active_slice.reset();
    }

    ActiveSliceGuard(const ActiveSliceGuard&) = delete;
    ActiveSliceGuard& operator=(const ActiveSliceGuard&) = delete;

    explicit operator bool() const { return registered_; }
    bool cancelled() const { return operation_->cancel_requested.load(std::memory_order_acquire); }
    void attach(Slic3r::PrintBase& print) { operation_->attach(print); }
    void detach() { operation_->detach(); }

private:
    OrcSession& session_;
    std::shared_ptr<ActiveSlice> operation_;
    bool registered_ = false;
};

// Declaring this guard immediately after the stack Print makes it destruct
// before Print itself. That ordering is what prevents a concurrent cancel
// from observing a dangling native Print pointer during exception unwinding.
struct ActivePrintGuard {
    ActiveSliceGuard& operation;
    explicit ActivePrintGuard(ActiveSliceGuard& value) : operation(value) {}
    ~ActivePrintGuard() { operation.detach(); }
};

struct ProgressWindow {
    ProgressWindow(OrcSession& session, int base, int span)
        : session(session), old_base(session.progress_base), old_span(session.progress_span) {
        session.progress_base = base;
        session.progress_span = span;
    }

    ~ProgressWindow() {
        session.progress_base = old_base;
        session.progress_span = old_span;
    }

    OrcSession& session;
    int old_base;
    int old_span;
};

// Slicing blocks the worker's event loop. MAIN_THREAD_EM_ASM delivers this
// from both the single-threaded engine and a pthread back to that worker,
// where postMessage reaches the host's main thread immediately.
static void post_slice_progress(int percent, const std::string& stage) {
    MAIN_THREAD_EM_ASM({
        // The build smoke test also runs this bridge in Node, where there is
        // no Worker parent to receive host host integration messages.
        if (typeof self !== 'undefined' && typeof self.postMessage === 'function') {
            self.postMessage({ type: 'SLICE_PROGRESS', percent: $0, stage: UTF8ToString($1) });
        }
    }, percent, stage.c_str());
}

struct ProgressState {
    int last_percent = -1;
    std::string last_stage;
    double last_update_ms = -100.0;
    std::mutex mutex;
};

static void attach_progress_callback(Slic3r::Print& print, OrcSession& session) {
    auto state = std::make_shared<ProgressState>();

    print.set_status_callback([state, &session](const Slic3r::PrintBase::SlicingStatus& status) {
        if (status.percent < 0)
            return;

        const double now = emscripten_get_now();
        int percent = 0;
        bool should_emit = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            percent = std::max(state->last_percent, std::min(status.percent, 100));
            should_emit =
                status.text != state->last_stage
                || (percent == 100 && state->last_percent != 100)
                || (percent != state->last_percent && now - state->last_update_ms >= 100.0);
            if (should_emit) {
                state->last_percent = percent;
                state->last_stage = status.text;
                state->last_update_ms = now;
            }
        }
        if (should_emit) {
            const int mapped_percent = session.progress_base + static_cast<int>(std::lround(
                static_cast<double>(percent) * static_cast<double>(session.progress_span) / 100.0
            ));
            if (session.progress_callback)
                session.progress_callback(mapped_percent, status.text.c_str(), session.progress_user_data);
            post_slice_progress(mapped_percent, status.text);
        }
    });
}

static void emit_project_progress(OrcSession& session, int local_percent,
                                  const char* stage) {
    const int clamped = std::max(0, std::min(local_percent, 100));
    const int mapped_percent = session.progress_base + static_cast<int>(std::lround(
        static_cast<double>(clamped) * static_cast<double>(session.progress_span) / 100.0
    ));
    const char* safe_stage = stage ? stage : "";
    if (session.progress_callback)
        session.progress_callback(mapped_percent, safe_stage, session.progress_user_data);
    post_slice_progress(mapped_percent, safe_stage);
}

static OrcSession* as_session(void* ptr) { return static_cast<OrcSession*>(ptr); }

static void record_error(OrcSession& s, const char* msg) { s.last_error = msg ? msg : "unknown error"; }
static void record_error(OrcSession& s, const std::string& str) { s.last_error = str; }

// onewasm_obj_to_stl / onewasm_cad_to_stl are pure format conversions with no config
// state, so they don't need a session — but the JS caller still wants
// onewasm_last_error() to work for them. Keep a small dedicated error slot
// for these functions; onewasm_last_error(0) (the host's call
// pattern for conversion errors) falls back to it when no session is passed.
static std::string g_conversion_last_error;
static void record_error(const char* msg) { g_conversion_last_error = msg ? msg : "unknown error"; }
static void record_error(const std::string& str) { g_conversion_last_error = str; }

static bool valid_project_id(const nlohmann::json& value, std::string& result,
                             const char* field, std::string& error) {
    if (!value.is_string()) {
        error = std::string(field) + " must be a string";
        return false;
    }
    result = value.get<std::string>();
    if (result.empty() || result.size() > 256) {
        error = std::string(field) + " must contain 1..256 characters";
        return false;
    }
    for (const unsigned char character : result) {
        if (!(std::isalnum(character) || character == '.' || character == '_'
              || character == ':' || character == '-')) {
            error = std::string(field) + " contains an unsupported character";
            return false;
        }
    }
    return true;
}

static bool reject_unknown_keys(const nlohmann::json& value,
                                std::initializer_list<const char*> allowed,
                                const char* field, std::string& error) {
    if (!value.is_object())
        return true;
    for (const auto& [key, ignored] : value.items()) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            error = std::string(field) + " contains unsupported field " + key;
            return false;
        }
    }
    return true;
}

static bool project_uint32(const nlohmann::json& value, std::uint32_t& result,
                           const char* field, std::string& error) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number <= UINT32_MAX) {
            result = static_cast<std::uint32_t>(number);
            return true;
        }
    } else if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        if (number >= 0 && number <= UINT32_MAX) {
            result = static_cast<std::uint32_t>(number);
            return true;
        }
    }
    error = std::string(field) + " must be a non-negative 32-bit integer";
    return false;
}

static bool project_number(const nlohmann::json& value, double& result,
                           const char* field, std::string& error) {
    if (!value.is_number()) {
        error = std::string(field) + " must be a number";
        return false;
    }
    result = value.get<double>();
    if (!std::isfinite(result)) {
        error = std::string(field) + " must be finite";
        return false;
    }
    return true;
}

static bool parse_project_matrix(const nlohmann::json& value,
                                 std::array<double, 16>& result,
                                 const char* field, std::string& error) {
    if (!value.is_array() || value.size() != result.size()) {
        error = std::string(field) + " must contain exactly 16 numbers";
        return false;
    }
    for (std::size_t index = 0; index < result.size(); ++index) {
        if (!project_number(value[index], result[index], field, error))
            return false;
    }
    if (std::abs(result[12]) > 1e-9 || std::abs(result[13]) > 1e-9
        || std::abs(result[14]) > 1e-9 || std::abs(result[15] - 1.) > 1e-9) {
        error = std::string(field) + " must be an affine matrix with [0,0,0,1] as its last row";
        return false;
    }
    const double determinant =
        result[0] * (result[5] * result[10] - result[6] * result[9])
        - result[1] * (result[4] * result[10] - result[6] * result[8])
        + result[2] * (result[4] * result[9] - result[5] * result[8]);
    if (std::abs(determinant) < 1e-12) {
        error = std::string(field) + " must have a non-singular 3D transform";
        return false;
    }
    return true;
}

static bool parse_project_manifest(const uint8_t* manifest_data,
                                   std::uint32_t manifest_len,
                                   std::uint32_t object_blob_len,
                                   bool require_data_ranges,
                                   ProjectManifestInput& result,
                                   nlohmann::json& normalized_manifest,
                                   std::string& error) {
    if (!manifest_data || manifest_len == 0) {
        error = "project manifest is empty";
        return false;
    }
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(std::string(
            reinterpret_cast<const char*>(manifest_data), static_cast<std::size_t>(manifest_len)));
    } catch (const std::exception& exception) {
        error = std::string("project manifest is not valid JSON: ") + exception.what();
        return false;
    }
    if (!root.is_object() || !root.contains("schemaVersion")
        || !root.at("schemaVersion").is_string()
        || root.at("schemaVersion").get<std::string>() != "0.3") {
        error = "project manifest schemaVersion must be 0.3";
        return false;
    }
    if (!reject_unknown_keys(root, {"schemaVersion", "plates", "meshes", "objects", "instances"},
                             "project manifest", error))
        return false;
    const char* arrays[] = {"plates", "meshes", "objects", "instances"};
    for (const char* name : arrays) {
        if (!root.contains(name) || !root.at(name).is_array()) {
            error = std::string("project manifest field ") + name + " must be an array";
            return false;
        }
    }

    std::set<std::string> plate_ids;
    for (const auto& plate : root.at("plates")) {
        if (!plate.is_object() || !plate.contains("id")) {
            error = "project plate must be an object with an id";
            return false;
        }
        if (!reject_unknown_keys(plate, {"id", "label", "index"}, "project plate", error))
            return false;
        std::string id;
        if (!valid_project_id(plate.at("id"), id, "plate.id", error))
            return false;
        if (plate.contains("label")
            && (!plate.at("label").is_string() || plate.at("label").get<std::string>().size() > 256)) {
            error = "project plate label must be a string of at most 256 characters";
            return false;
        }
        if (plate.contains("index")) {
            std::uint32_t index = 0;
            if (!project_uint32(plate.at("index"), index, "plate.index", error))
                return false;
        }
        if (!plate_ids.insert(id).second) {
            error = "project manifest contains a duplicate plate id: " + id;
            return false;
        }
        result.plate_ids.push_back(std::move(id));
    }

    std::set<std::string> mesh_ids;
    for (const auto& mesh : root.at("meshes")) {
        if (!mesh.is_object() || !mesh.contains("id") || !mesh.contains("format")) {
            error = "project mesh must contain id and format";
            return false;
        }
        if (!reject_unknown_keys(mesh, {"id", "format", "dataRange"}, "project mesh", error))
            return false;
        ProjectMeshInput parsed;
        if (!valid_project_id(mesh.at("id"), parsed.id, "mesh.id", error))
            return false;
        if (!mesh.at("format").is_string() || mesh.at("format").get<std::string>() != "stl") {
            error = "project mesh format must be stl";
            return false;
        }
        if (!mesh_ids.insert(parsed.id).second) {
            error = "project manifest contains a duplicate mesh id: " + parsed.id;
            return false;
        }
        if (!mesh.contains("dataRange")) {
            if (require_data_ranges) {
                error = "project mesh " + parsed.id + " must contain dataRange for project_set_objects";
                return false;
            }
        } else {
            const auto& data_range = mesh.at("dataRange");
            if (!data_range.is_object()
                || !reject_unknown_keys(data_range, {"offset", "length"}, "mesh.dataRange", error)
                || !data_range.contains("offset") || !data_range.contains("length")) {
                error = "project mesh " + parsed.id + " dataRange must contain offset and length";
                return false;
            }
            if (!project_uint32(data_range.at("offset"), parsed.offset,
                                "mesh.dataRange.offset", error)
                || !project_uint32(data_range.at("length"), parsed.length,
                                   "mesh.dataRange.length", error)) {
                return false;
            }
            if (parsed.length == 0
                || static_cast<std::uint64_t>(parsed.offset) + parsed.length > object_blob_len) {
                error = "project mesh " + parsed.id + " dataRange is outside object_blob";
                return false;
            }
            parsed.has_data_range = true;
        }
        result.meshes.push_back(std::move(parsed));
    }

    std::set<std::string> object_ids;
    for (const auto& object : root.at("objects")) {
        if (!object.is_object() || !object.contains("id") || !object.contains("meshId")) {
            error = "project object must contain id and meshId";
            return false;
        }
        if (!reject_unknown_keys(object, {"id", "meshId", "label", "extruderId"}, "project object", error))
            return false;
        ProjectObjectInput parsed;
        if (!valid_project_id(object.at("id"), parsed.id, "object.id", error)
            || !valid_project_id(object.at("meshId"), parsed.mesh_id, "object.meshId", error)) {
            return false;
        }
        if (!object_ids.insert(parsed.id).second) {
            error = "project manifest contains a duplicate object id: " + parsed.id;
            return false;
        }
        if (std::none_of(result.meshes.begin(), result.meshes.end(), [&parsed](const auto& mesh) {
                return mesh.id == parsed.mesh_id;
            })) {
            error = "project object " + parsed.id + " refers to an unknown mesh: " + parsed.mesh_id;
            return false;
        }
        if (object.contains("label")) {
            if (!object.at("label").is_string() || object.at("label").get<std::string>().size() > 256) {
                error = "project object label must be a string of at most 256 characters";
                return false;
            }
            parsed.label = object.at("label").get<std::string>();
        }
        if (object.contains("extruderId")) {
            std::uint32_t extruder_id = 0;
            if (!project_uint32(object.at("extruderId"), extruder_id, "object.extruderId", error))
                return false;
            if (extruder_id > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
                error = "object.extruderId is too large for the native engine";
                return false;
            }
            parsed.extruder_id = static_cast<int>(extruder_id);
        }
        result.objects.push_back(std::move(parsed));
    }

    std::set<std::string> instance_ids;
    for (const auto& instance : root.at("instances")) {
        if (!instance.is_object() || !instance.contains("id") || !instance.contains("objectId")
            || !instance.contains("plateId") || !instance.contains("transform")) {
            error = "project instance must contain id, objectId, plateId, and transform";
            return false;
        }
        if (!reject_unknown_keys(instance, {"id", "objectId", "plateId", "transform"},
                                 "project instance", error))
            return false;
        ProjectInstanceInput parsed;
        if (!valid_project_id(instance.at("id"), parsed.id, "instance.id", error)
            || !valid_project_id(instance.at("objectId"), parsed.object_id, "instance.objectId", error)
            || !valid_project_id(instance.at("plateId"), parsed.plate_id, "instance.plateId", error)) {
            return false;
        }
        if (!instance_ids.insert(parsed.id).second) {
            error = "project manifest contains a duplicate instance id: " + parsed.id;
            return false;
        }
        if (std::none_of(result.objects.begin(), result.objects.end(), [&parsed](const auto& object) {
                return object.id == parsed.object_id;
            })) {
            error = "project instance " + parsed.id + " refers to an unknown object: " + parsed.object_id;
            return false;
        }
        if (plate_ids.find(parsed.plate_id) == plate_ids.end()) {
            error = "project instance " + parsed.id + " refers to an unknown plate: " + parsed.plate_id;
            return false;
        }
        if (!instance.at("transform").is_object()
            || !reject_unknown_keys(instance.at("transform"), {"matrix"}, "instance.transform", error)
            || !instance.at("transform").contains("matrix")
            || !parse_project_matrix(instance.at("transform").at("matrix"), parsed.matrix,
                                     "instance.transform.matrix", error)) {
            return false;
        }
        result.instances.push_back(std::move(parsed));
    }

    if (result.plate_ids.empty() || result.instances.empty()) {
        error = "project manifest must contain at least one plate and one instance";
        return false;
    }
    normalized_manifest = std::move(root);
    return true;
}

// Unconditionally removes a MEMFS temp file on scope exit (success, early
// return, or C++ exception alike). Without this, a throw from do_export()
// (or anything else between file creation and the manual std::remove() at
// the bottom of the happy path) left the partially-written file behind —
// MEMFS is RAM-backed, so that's a real per-failed-slice memory leak over a
// long session, not just a stray file. std::remove() on a missing path is a
// harmless no-op (ENOENT), so double-removal on the happy path is fine.
struct TempFileGuard {
    std::string path;
    explicit TempFileGuard(std::string p) : path(std::move(p)) {}
    ~TempFileGuard() { std::remove(path.c_str()); }
    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
};

// Same rationale as TempFileGuard, for Model::remove_backup_path_if_exist()
// (the lazily-created MEMFS "Auxiliaries"/backup scratch dir that
// store_bbs_3mf/load_bbs_3mf both touch internally via get_backup_path()).
// Traced the two most likely internal throw sites in bbs_3mf.cpp — a
// painting-feature version mismatch during model XML parsing, and a
// malformed Metadata/*.config JSON — and both are caught internally and
// converted to false/-1 returns rather than propagating, so the risk this
// guards against is low in practice. bbs_3mf.cpp is ~9000 lines and wasn't
// exhaustively audited past those two paths, though, and this costs
// nothing, so guard it the same way as everything else here rather than
// rely on that audit staying true across future engine version bumps.
struct ModelBackupPathGuard {
    Slic3r::Model& model;
    explicit ModelBackupPathGuard(Slic3r::Model& m) : model(m) {}
    ~ModelBackupPathGuard() { model.remove_backup_path_if_exist(); }
    ModelBackupPathGuard(const ModelBackupPathGuard&) = delete;
    ModelBackupPathGuard& operator=(const ModelBackupPathGuard&) = delete;
};

// load_bbs_3mf() transfers ownership of imported plate data and embedded
// presets to its output vectors. Keep both behind one guard so partial loads
// and exceptions release the same allocations as OrcaSlicer's GUI callers.
struct Loaded3mfResourcesGuard {
    Slic3r::PlateDataPtrs& plate_data;
    std::vector<Slic3r::Preset*>& project_presets;

    Loaded3mfResourcesGuard(Slic3r::PlateDataPtrs& plates, std::vector<Slic3r::Preset*>& presets)
        : plate_data(plates), project_presets(presets) {}
    ~Loaded3mfResourcesGuard() {
        Slic3r::release_PlateData_list(plate_data);
        for (Slic3r::Preset* preset : project_presets)
            delete preset;
        project_presets.clear();
    }
    Loaded3mfResourcesGuard(const Loaded3mfResourcesGuard&) = delete;
    Loaded3mfResourcesGuard& operator=(const Loaded3mfResourcesGuard&) = delete;
};

// Reads an entire file into a malloc'd buffer. Returns nullptr on any
// failure (missing file, empty/negative size, OOM, or a short read); sets
// *out_err to a short reason and *out_oom to distinguish the OOM case
// (callers map that to a different error code than the others). Callers
// still pick their own record_error() overload (session-aware vs. the
// conversion-functions' shared slot) and error code, since those differ
// per call site — this only owns the mechanical fopen/fseek/malloc/fread
// sequence that onewasm_write_3mf and onewasm_read_3mf both need to read back the
// file they just asked OrcaSlicer to produce.
static char* read_file_to_buffer(const char* path, long* out_len, const char** out_err, bool* out_oom) {
    *out_oom = false;
    FILE* f = std::fopen(path, "rb");
    if (!f) { *out_err = "produced no output"; return nullptr; }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::rewind(f);
    if (sz <= 0) {
        std::fclose(f);
        *out_err = "produced empty output";
        return nullptr;
    }
    char* buf = static_cast<char*>(std::malloc(static_cast<std::size_t>(sz)));
    if (!buf) {
        std::fclose(f);
        *out_err = "out of memory";
        *out_oom = true;
        return nullptr;
    }
    std::size_t nread = std::fread(buf, 1, static_cast<std::size_t>(sz), f);
    std::fclose(f);
    if (nread != static_cast<std::size_t>(sz)) {
        std::free(buf);
        *out_err = "read incomplete";
        return nullptr;
    }
    *out_len = sz;
    return buf;
}

struct ProjectZipReaderGuard {
    Slic3r::MZ_Archive archive;
    bool opened = false;

    ~ProjectZipReaderGuard() {
        if (opened)
            Slic3r::close_zip_reader(&archive.arch);
    }
};

static bool list_project_zip_entries(
    OrcSession& session,
    const std::vector<std::uint8_t>& archive_data,
    std::vector<std::string>& entries,
    std::string& error
) {
    if (archive_data.empty() || archive_data.size() > UINT32_MAX) {
        error = "native project archive is empty or oversized";
        return false;
    }
    const std::string path = "/tmp/ow-project-entries-" + std::to_string(session.id) + ".3mf";
    TempFileGuard file_guard(path);
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        error = "unable to stage native project archive";
        return false;
    }
    const std::size_t written = std::fwrite(archive_data.data(), 1, archive_data.size(), file);
    std::fclose(file);
    if (written != archive_data.size()) {
        error = "unable to stage complete native project archive";
        return false;
    }

    ProjectZipReaderGuard reader;
    if (!Slic3r::open_zip_reader(&reader.archive.arch, path)) {
        error = "native project archive is not a readable ZIP/3MF archive";
        return false;
    }
    reader.opened = true;
    const mz_uint count = mz_zip_reader_get_num_files(&reader.archive.arch);
    entries.reserve(count);
    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&reader.archive.arch, index, &stat)) {
            error = "unable to inspect a native project ZIP entry";
            return false;
        }
        entries.emplace_back(stat.m_filename);
    }
    return true;
}

// ── helpers ───────────────────────────────────────────────────────────────────
static std::string json_val_to_string(const nlohmann::json& v) {
    if (v.is_string())  return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "1" : "0";
    // Keep the JSON number's full representation. std::to_string(double)
    // prints only six digits after the decimal point, which silently changes
    // otherwise valid profile values such as a fine layer height or flow
    // ratio before OrcaSlicer sees them.
    if (v.is_number())  return v.dump();
    return "";
}

// Read a pseudo-key boolean flag out of the config JSON (the bridge's own
// keys — remove_mixed_temp_restriction, adaptive_layer_height — that never
// reach the engine config). Accepts a JSON bool, a number (non-zero = true),
// or the "1"/"true" strings the JS config layer serializes booleans as;
// anything else (including a missing key) yields `dflt`.
static bool json_flag(const nlohmann::json& j, const char* key, bool dflt = false) {
    if (!j.contains(key)) return dflt;
    const auto& v = j[key];
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number())  return v.get<double>() != 0.0;
    if (v.is_string())  { const std::string s = v.get<std::string>(); return s == "1" || s == "true"; }
    return dflt;
}

/**
 * Serialize a JSON array into the single string OrcaSlicer's deserializer
 * expects for that specific option — the separator is a property of the
 * option's *type*, not a universal comma:
 *
 *   coStrings       ';' plus c-style quoting/escaping (escape_strings_cstyle)
 *   coPointsGroups  '#' between groups, ',' between points inside one group
 *   everything else ','
 *
 * Getting this wrong silently fuses N values into 1 rather than failing, which
 * is exactly how a real multi-nozzle profile used to die: a Bambu Lab H2D
 * stores two per-extruder printable areas and two filament colours, both of
 * which collapsed to a single entry. The engine then had nozzle_diameter of
 * length 2 but length-1 companions, and indexed them by extruder id — blowing
 * up in Brim.cpp's outer_inner_brim_area() and ToolOrdering's flush-matrix
 * lookup (issue #140).
 *
 * The separator is looked up in the engine's own option registry rather than
 * mirrored in a hand-maintained list on the JS side, so it cannot drift out of
 * sync with the engine version this bridge is compiled against. Strings go
 * through escape_strings_cstyle() rather than a plain join because several
 * coStrings options (filament_start_gcode and friends) legitimately contain
 * ';' and newlines, which a raw join would corrupt.
 */
static std::string json_array_to_config_string(const std::string& key, const nlohmann::json& arr) {
    std::vector<std::string> parts;
    parts.reserve(arr.size());
    for (const auto& el : arr) {
        if (el.is_null()) continue;
        parts.push_back(json_val_to_string(el));
    }
    if (parts.empty()) return "";

    const Slic3r::ConfigOptionDef* def = Slic3r::print_config_def.get(key);
    const Slic3r::ConfigOptionType type = def ? def->type : Slic3r::coNone;
    if (type == Slic3r::coStrings)
        return Slic3r::escape_strings_cstyle(parts);

    const char sep = (type == Slic3r::coPointsGroups) ? '#' : ',';
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

// Center a newly uploaded mesh like desktop OrcaSlicer: centre X/Y on the
// selected bed and drop the lowest point onto Z=0. STL files from model sites
// are not required to be bed-aligned; leaving a negative raw Z here silently
// clips the lower part of the model and makes the resulting layer count differ
// from desktop OrcaSlicer. The instance is created after this translation, so
// the volume transform itself must carry the Z correction.
static void center_object_xy_only(Slic3r::ModelObject* obj) {
    const Slic3r::BoundingBoxf3 bbox = obj->raw_mesh_bounding_box();
    Slic3r::Vec3d shift = -bbox.center();
    shift.z() = -bbox.min.z();
    obj->translate(shift);
    obj->origin_translation += shift;
}

// Public transform ABI shared by the single-file and multi-file slice calls:
// [scale xyz, rotation xyz (radians), mirror xyz (+/-1), offset xy (mm)].
// The offset is relative to the selected bed centre; NaN in both offset slots
// means that the arrangement pass owns placement.
static constexpr int OBJECT_TRANSFORM_STRIDE = 11;

struct ObjectTransformInput {
    Slic3r::Vec3d scale  = Slic3r::Vec3d(1., 1., 1.);
    Slic3r::Vec3d rotation = Slic3r::Vec3d(0., 0., 0.);
    Slic3r::Vec3d mirror = Slic3r::Vec3d(1., 1., 1.);
    bool has_offset = false;
    double offset_x = 0.;
    double offset_y = 0.;
};

static bool read_object_transform(const float* table, std::size_t index,
                                  ObjectTransformInput& out, std::string& error) {
    if (!table) return true;
    const float* values = table + index * OBJECT_TRANSFORM_STRIDE;
    for (int i = 0; i < 9; ++i) {
        if (!std::isfinite(values[i])) {
            error = "object transform contains a non-finite scale, rotation or mirror value";
            return false;
        }
    }
    if (values[0] <= 0.f || values[1] <= 0.f || values[2] <= 0.f) {
        error = "object transform scale must be greater than zero";
        return false;
    }
    for (int i = 6; i < 9; ++i) {
        if (values[i] != 1.f && values[i] != -1.f) {
            error = "object transform mirror values must be 1 or -1";
            return false;
        }
    }

    const bool x_nan = std::isnan(values[9]);
    const bool y_nan = std::isnan(values[10]);
    if (x_nan != y_nan) {
        error = "object transform offset must contain two finite values or two NaN values";
        return false;
    }
    if (!x_nan && (!std::isfinite(values[9]) || !std::isfinite(values[10]))) {
        error = "object transform offset must be finite";
        return false;
    }

    out.scale = Slic3r::Vec3d(values[0], values[1], values[2]);
    out.rotation = Slic3r::Vec3d(values[3], values[4], values[5]);
    out.mirror = Slic3r::Vec3d(values[6], values[7], values[8]);
    out.has_offset = !x_nan;
    if (out.has_offset) {
        out.offset_x = values[9];
        out.offset_y = values[10];
    }
    return true;
}

static Slic3r::ModelInstance* add_transformed_instance(
    Slic3r::ModelObject* obj, const OrcSession& session,
    const ObjectTransformInput& transform, bool default_to_bed_center) {
    Slic3r::Vec3d offset = default_to_bed_center
        ? Slic3r::Vec3d(session.bed_cx, session.bed_cy, 0.)
        : Slic3r::Vec3d::Zero();
    if (transform.has_offset)
        offset = Slic3r::Vec3d(session.bed_cx + transform.offset_x,
                               session.bed_cy + transform.offset_y, 0.);
    return obj->add_instance(offset, transform.scale, transform.rotation, transform.mirror);
}

static Slic3r::BoundingBox arrangement_bed(const OrcSession& session) {
    const double half_w = session.bed_shape == "circle"
        ? session.bed_cx / std::sqrt(2.)
        : session.bed_cx;
    const double half_h = session.bed_shape == "circle"
        ? session.bed_cy / std::sqrt(2.)
        : session.bed_cy;
    return Slic3r::BoundingBox(
        Slic3r::Point(
            static_cast<coord_t>((session.bed_cx - half_w) * 1e6),
            static_cast<coord_t>((session.bed_cy - half_h) * 1e6)),
        Slic3r::Point(
            static_cast<coord_t>((session.bed_cx + half_w) * 1e6),
            static_cast<coord_t>((session.bed_cy + half_h) * 1e6)));
}

static Slic3r::ArrangeParams arrangement_params() {
    Slic3r::ArrangeParams params;
    params.min_obj_distance = static_cast<coord_t>(2. * 1e6); // 2 mm gap
#ifdef SLIC3R_WASM_MT
    params.parallel = true;
#else
    params.parallel = false;
#endif
    return params;
}

// Arrange transformed instances while treating finite offsets as pinned
// objects. NaN-offset instances remain movable, so a later Arrange action can
// preserve explicit positions and still pack the rest around them.
static void arrange_transformed_model(Slic3r::Model& model, const OrcSession& session,
                                      const std::vector<bool>* pinned = nullptr) {
    Slic3r::arrangement::ArrangePolygons movable;
    Slic3r::arrangement::ArrangePolygons excludes;
    Slic3r::ModelInstancePtrs instances;

    for (std::size_t i = 0; i < model.objects.size(); ++i) {
        auto* obj = model.objects[i];
        if (obj->instances.empty()) continue;
        auto* instance = obj->instances.front();
        Slic3r::arrangement::ArrangePolygon polygon;
        instance->get_arrange_polygon(&polygon);
        polygon.bed_idx = 0;
        const bool fixed = pinned && i < pinned->size() && (*pinned)[i];
        if (fixed)
            excludes.push_back(std::move(polygon));
        else {
            instances.push_back(instance);
            movable.push_back(std::move(polygon));
        }
    }

    if (movable.empty()) return;
    auto params = arrangement_params();
    const auto bed = arrangement_bed(session);
    Slic3r::arrangement::arrange(movable, excludes, bed, params);
    Slic3r::apply_arrange_polys(movable, instances,
        [&session](Slic3r::arrangement::ArrangePolygon& polygon) {
            polygon.translation = Slic3r::Vec2crd(
                static_cast<coord_t>(session.bed_cx * 1e6),
                static_cast<coord_t>(session.bed_cy * 1e6));
        });
}

static void auto_orient_model(Slic3r::Model& model) {
    for (auto* obj : model.objects) {
        if (obj->instances.empty()) continue;
        Slic3r::orientation::orient(obj->instances.front());
        obj->invalidate_bounding_box();
        obj->ensure_on_bed();
    }
}

static nlohmann::json serialize_object_transform(const Slic3r::ModelInstance& instance,
                                                 const OrcSession& session,
                                                 bool keep_position) {
    const Slic3r::Vec3d scale = instance.get_scaling_factor();
    const Slic3r::Vec3d rotation = instance.get_rotation();
    const Slic3r::Vec3d mirror = instance.get_mirror();
    nlohmann::json value;
    value["scale"] = nlohmann::json::array({scale.x(), scale.y(), scale.z()});
    value["rotation"] = nlohmann::json::array({rotation.x(), rotation.y(), rotation.z()});
    value["mirror"] = nlohmann::json::array({mirror.x(), mirror.y(), mirror.z()});
    if (keep_position) {
        const Slic3r::Vec3d offset = instance.get_offset();
        value["offset"] = nlohmann::json::array({offset.x() - session.bed_cx, offset.y() - session.bed_cy});
    } else {
        value["offset"] = nullptr;
    }
    return value;
}

static int write_transform_json(OrcSession& session, const Slic3r::Model& model,
                                const std::vector<bool>& keep_positions,
                                uint8_t** out_transforms, uint32_t* out_len) {
    nlohmann::json result = nlohmann::json::array();
    for (std::size_t i = 0; i < model.objects.size(); ++i) {
        if (model.objects[i]->instances.empty()) {
            record_error(session, "model object has no instance");
            return -5;
        }
        const bool keep = i < keep_positions.size() && keep_positions[i];
        result.push_back(serialize_object_transform(*model.objects[i]->instances.front(), session, keep));
    }
    const std::string json = result.dump();
    if (json.size() > static_cast<std::size_t>(UINT32_MAX) - 1) {
        record_error(session, "plate transform output is too large");
        return -9;
    }
    char* buffer = static_cast<char*>(std::malloc(json.size() + 1));
    if (!buffer) {
        record_error(session, "out of memory");
        return -9;
    }
    std::memcpy(buffer, json.data(), json.size());
    buffer[json.size()] = '\0';
    *out_transforms = reinterpret_cast<uint8_t*>(buffer);
    *out_len = static_cast<uint32_t>(json.size());
    return 0;
}

static const ProjectMeshInput* find_project_mesh(const ProjectManifestInput& manifest,
                                                 const std::string& id) {
    const auto found = std::find_if(manifest.meshes.begin(), manifest.meshes.end(), [&id](const auto& mesh) {
        return mesh.id == id;
    });
    return found == manifest.meshes.end() ? nullptr : &*found;
}

static const ProjectObjectInput* find_project_object(const ProjectManifestInput& manifest,
                                                     const std::string& id) {
    const auto found = std::find_if(manifest.objects.begin(), manifest.objects.end(), [&id](const auto& object) {
        return object.id == id;
    });
    return found == manifest.objects.end() ? nullptr : &*found;
}

static bool project_matrix_to_legacy_transform(const std::array<double, 16>& values,
                                               const OrcSession& session,
                                               ObjectTransformInput& result,
                                               std::string& error) {
    Slic3r::Transform3d matrix = Slic3r::Transform3d::Identity();
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column)
            matrix.matrix()(row, column) = values[static_cast<std::size_t>(row * 4 + column)];
    }
    Slic3r::Geometry::Transformation transformation(matrix);
    if (transformation.has_skew()) {
        error = "OrcaSlicer arrange does not support a skewed project transform";
        return false;
    }
    const auto offset = transformation.get_offset();
    if (std::abs(offset.z()) > 1e-6) {
        error = "OrcaSlicer arrange requires project transforms to be on the bed plane";
        return false;
    }
    result.scale = transformation.get_scaling_factor();
    result.rotation = transformation.get_rotation();
    result.mirror = transformation.get_mirror();
    result.has_offset = true;
    result.offset_x = offset.x() - session.bed_cx;
    result.offset_y = offset.y() - session.bed_cy;
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(result.scale[axis]) || result.scale[axis] <= 0.) {
            error = "project transform scale must be finite and greater than zero";
            return false;
        }
        if (result.mirror[axis] != 1. && result.mirror[axis] != -1.) {
            error = "project transform mirror values must be 1 or -1";
            return false;
        }
    }
    return true;
}

static std::array<double, 16> legacy_transform_to_project_matrix(
    const ObjectTransformInput& input, const OrcSession& session) {
    Slic3r::Geometry::Transformation transformation;
    transformation.set_scaling_factor(input.scale);
    transformation.set_rotation(input.rotation);
    transformation.set_mirror(input.mirror);
    const double x = input.has_offset ? session.bed_cx + input.offset_x : session.bed_cx;
    const double y = input.has_offset ? session.bed_cy + input.offset_y : session.bed_cy;
    transformation.set_offset(Slic3r::Vec3d(x, y, 0.));

    std::array<double, 16> result{};
    const auto& matrix = transformation.get_matrix().matrix();
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column)
            result[static_cast<std::size_t>(row * 4 + column)] = matrix(row, column);
    }
    return result;
}

static nlohmann::json project_matrix_json(const std::array<double, 16>& matrix) {
    nlohmann::json result = nlohmann::json::array();
    for (const double value : matrix)
        result.push_back(value);
    return result;
}

static bool append_native_model_project(
    OrcSession& session,
    Slic3r::Model& model,
    const Slic3r::PlateDataPtrs& plate_data_list,
    nlohmann::json& manifest,
    std::vector<std::uint8_t>& object_blob,
    std::string& error
) {
    manifest = empty_project_manifest();
    object_blob.clear();

    std::vector<std::string> object_ids(model.objects.size());
    std::vector<bool> usable_objects(model.objects.size(), false);
    for (std::size_t object_index = 0; object_index < model.objects.size(); ++object_index) {
        auto* object = model.objects[object_index];
        if (!object)
            continue;
        Slic3r::TriangleMesh mesh = object->raw_mesh();
        if (mesh.empty())
            continue;

        const std::string path = "/tmp/ow-native-project-"
            + std::to_string(session.id) + "-" + std::to_string(object_index) + ".stl";
        TempFileGuard file_guard(path);
        if (!Slic3r::store_stl(path.c_str(), &mesh, true)) {
            error = "OrcaSlicer could not serialize a native project object";
            return false;
        }
        long size = 0;
        const char* read_error = nullptr;
        bool out_of_memory = false;
        char* bytes = read_file_to_buffer(path.c_str(), &size, &read_error, &out_of_memory);
        if (!bytes) {
            error = std::string{"unable to retain native project geometry: "}
                + (read_error ? read_error : "read failed");
            return false;
        }
        const std::size_t byte_count = static_cast<std::size_t>(size);
        if (object_blob.size() > UINT32_MAX || byte_count > UINT32_MAX - object_blob.size()) {
            std::free(bytes);
            error = "OrcaSlicer native project geometry exceeds the C ABI length limit";
            return false;
        }
        const std::uint32_t offset = static_cast<std::uint32_t>(object_blob.size());
        object_blob.insert(
            object_blob.end(),
            reinterpret_cast<const std::uint8_t*>(bytes),
            reinterpret_cast<const std::uint8_t*>(bytes) + byte_count
        );
        std::free(bytes);

        const std::string object_id = "native-object-" + std::to_string(object_index);
        const std::string mesh_id = "native-mesh-" + std::to_string(object_index);
        object_ids[object_index] = object_id;
        usable_objects[object_index] = true;
        nlohmann::json object_entry = {
            {"id", object_id},
            {"meshId", mesh_id},
        };
        if (!object->name.empty())
            object_entry["label"] = object->name.substr(0, 256);
        manifest["meshes"].push_back({
            {"id", mesh_id},
            {"format", "stl"},
            {"dataRange", {
                {"offset", offset},
                {"length", static_cast<std::uint32_t>(byte_count)},
            }},
        });
        manifest["objects"].push_back(std::move(object_entry));
    }

    std::vector<std::pair<std::string, const Slic3r::PlateData*>> plates;
    for (const auto* plate : plate_data_list) {
        if (!plate)
            continue;
        const int index = plate->plate_index >= 0
            ? plate->plate_index
            : static_cast<int>(plates.size());
        const std::string id = "plate-" + std::to_string(index);
        plates.emplace_back(id, plate);
        manifest["plates"].push_back({
            {"id", id},
            {"label", plate->plate_name.empty()
                ? "Plate " + std::to_string(index + 1)
                : plate->plate_name.substr(0, 256)},
            {"index", index},
        });
    }
    if (plates.empty()) {
        plates.emplace_back("plate-0", nullptr);
        manifest["plates"].push_back({
            {"id", "plate-0"},
            {"label", "OrcaSlicer project"},
            {"index", 0},
        });
    }

    auto project_matrix = [](const Slic3r::ModelInstance& instance) {
        std::array<double, 16> matrix{};
        const auto& native = instance.get_matrix().matrix();
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                matrix[static_cast<std::size_t>(row * 4 + column)] = native(row, column);
        return matrix;
    };
    std::set<std::pair<std::size_t, std::size_t>> emitted;
    std::size_t instance_ordinal = 0;
    auto append_instance = [&](const std::string& plate_id,
        const std::size_t object_index,
                               const std::size_t instance_index) {
        if (object_index >= model.objects.size()
            || !usable_objects[object_index]
            || !model.objects[object_index]
            || instance_index >= model.objects[object_index]->instances.size()
            || !emitted.emplace(object_index, instance_index).second)
            return false;
        const auto* instance = model.objects[object_index]->instances[instance_index];
        if (!instance)
            return false;
        manifest["instances"].push_back({
            {"id", "native-instance-" + std::to_string(instance_ordinal++)},
            {"objectId", object_ids[object_index]},
            {"plateId", plate_id},
            {"transform", {{"matrix", project_matrix(*instance)}}},
        });
        return true;
    };

    for (const auto& [plate_id, plate] : plates) {
        if (!plate)
            continue;
        for (const auto& [native_object_id, instance_info] : plate->obj_inst_map) {
            std::size_t object_index = model.objects.size();
            std::size_t instance_index = static_cast<std::size_t>(std::max(0, instance_info.first));
            for (std::size_t candidate = 0; candidate < model.objects.size(); ++candidate) {
                if (!model.objects[candidate])
                    continue;
                if (model.objects[candidate]->id().id == native_object_id) {
                    object_index = candidate;
                    break;
                }
                for (std::size_t candidate_instance = 0;
                     candidate_instance < model.objects[candidate]->instances.size();
                     ++candidate_instance) {
                    const auto* candidate_instance_ptr =
                        model.objects[candidate]->instances[candidate_instance];
                    if (candidate_instance_ptr
                        && candidate_instance_ptr->loaded_id
                        == instance_info.second) {
                        object_index = candidate;
                        instance_index = candidate_instance;
                        break;
                    }
                }
                if (object_index != model.objects.size())
                    break;
            }
            append_instance(plate_id, object_index, instance_index);
        }
    }

    // Standard 3MF files do not carry Orca's plate metadata. In that case,
    // retain every native instance on the first logical plate instead of
    // returning a manifest that silently loses geometry.
    for (std::size_t object_index = 0; object_index < model.objects.size(); ++object_index) {
        if (!model.objects[object_index])
            continue;
        for (std::size_t instance_index = 0;
             instance_index < model.objects[object_index]->instances.size();
             ++instance_index)
            append_instance(plates.front().first, object_index, instance_index);
    }

    if (manifest["instances"].empty()) {
        manifest = empty_project_manifest();
        object_blob.clear();
    }
    return true;
}

static bool collect_project_plate_inputs(
    OrcSession& session,
    const ProjectManifestInput& manifest,
    const std::string& plate_id,
    std::vector<std::uint8_t>& blob,
    std::vector<std::uint32_t>& offsets,
    std::vector<std::int32_t>& extruders,
    std::vector<float>& transforms,
    std::vector<std::size_t>& manifest_indices,
    bool preserve_offsets,
    std::string& error) {
    std::size_t manifest_index = 0;
    for (const auto& instance : manifest.instances) {
        if (instance.plate_id != plate_id) {
            ++manifest_index;
            continue;
        }
        const auto* object = find_project_object(manifest, instance.object_id);
        const auto* mesh = object ? find_project_mesh(manifest, object->mesh_id) : nullptr;
        if (!object || !mesh) {
            error = "project plate contains an instance with an unresolved mesh";
            return false;
        }
        if (!mesh->has_data_range
            || static_cast<std::uint64_t>(mesh->offset) + mesh->length > session.project_object_blob.size()) {
            error = "selected project uses native geometry that the bridge could not materialize as STL";
            return false;
        }
        if (blob.size() > UINT32_MAX || mesh->length > UINT32_MAX - blob.size()) {
            error = "project plate object blob exceeds the C ABI length limit";
            return false;
        }
        const auto begin = session.project_object_blob.begin() + mesh->offset;
        const auto end = begin + mesh->length;
        const auto start = static_cast<std::uint32_t>(blob.size());
        blob.insert(blob.end(), begin, end);
        offsets.push_back(start);
        offsets.push_back(static_cast<std::uint32_t>(blob.size()));
        extruders.push_back(static_cast<std::int32_t>(object->extruder_id));

        ObjectTransformInput transform;
        if (!project_matrix_to_legacy_transform(instance.matrix, session, transform, error))
            return false;
        transforms.push_back(static_cast<float>(transform.scale.x()));
        transforms.push_back(static_cast<float>(transform.scale.y()));
        transforms.push_back(static_cast<float>(transform.scale.z()));
        transforms.push_back(static_cast<float>(transform.rotation.x()));
        transforms.push_back(static_cast<float>(transform.rotation.y()));
        transforms.push_back(static_cast<float>(transform.rotation.z()));
        transforms.push_back(static_cast<float>(transform.mirror.x()));
        transforms.push_back(static_cast<float>(transform.mirror.y()));
        transforms.push_back(static_cast<float>(transform.mirror.z()));
        if (preserve_offsets) {
            transforms.push_back(static_cast<float>(transform.offset_x));
            transforms.push_back(static_cast<float>(transform.offset_y));
        } else {
            transforms.push_back(std::numeric_limits<float>::quiet_NaN());
            transforms.push_back(std::numeric_limits<float>::quiet_NaN());
        }
        manifest_indices.push_back(manifest_index);
        ++manifest_index;
    }
    if (offsets.empty()) {
        error = "selected project plate contains no objects";
        return false;
    }
    return true;
}

struct ProjectExportOptions {
    std::string preservation;
    bool include_slice_artifacts = false;
};

static bool parse_project_export_options(
    const uint8_t* options_data,
    const uint32_t options_len,
    ProjectExportOptions& result,
    std::string& error
) {
    if (!options_data || options_len == 0) {
        error = "project export options are empty";
        return false;
    }
    nlohmann::json options;
    try {
        options = nlohmann::json::parse(std::string(
            reinterpret_cast<const char*>(options_data), static_cast<std::size_t>(options_len)));
    } catch (const std::exception& exception) {
        error = std::string("project export options are not valid JSON: ") + exception.what();
        return false;
    }
    if (!options.is_object() || !options.contains("schemaVersion")
        || !options.at("schemaVersion").is_string()
        || options.at("schemaVersion").get<std::string>() != "0.3") {
        error = "project export options schemaVersion must be 0.3";
        return false;
    }
    if (!reject_unknown_keys(
            options,
            {"schemaVersion", "preservation", "includeSliceArtifacts"},
            "project export options",
            error
        ))
        return false;
    if (!options.contains("preservation") || !options.at("preservation").is_string()) {
        error = "project export options preservation must be a string";
        return false;
    }
    result.preservation = options.at("preservation").get<std::string>();
    if (result.preservation != "require"
        && result.preservation != "best-effort"
        && result.preservation != "portable") {
        error = "project export options preservation must be require, best-effort, or portable";
        return false;
    }
    if (!options.contains("includeSliceArtifacts")
        || !options.at("includeSliceArtifacts").is_boolean()) {
        error = "project export options includeSliceArtifacts must be a boolean";
        return false;
    }
    result.include_slice_artifacts = options.at("includeSliceArtifacts").get<bool>();
    return true;
}

static bool build_project_export_mesh(
    OrcSession& session,
    const ProjectManifestInput& manifest,
    Slic3r::TriangleMesh& output,
    std::string& error
) {
    std::size_t instance_index = 0;
    for (const auto& instance : manifest.instances) {
        const auto* object = find_project_object(manifest, instance.object_id);
        const auto* mesh = object ? find_project_mesh(manifest, object->mesh_id) : nullptr;
        if (!object || !mesh || !mesh->has_data_range) {
            error = "project export requires host-owned STL data for every instance";
            return false;
        }
        if (static_cast<std::uint64_t>(mesh->offset) + mesh->length > session.project_object_blob.size()) {
            error = "project export mesh dataRange is outside object_blob";
            return false;
        }

        const std::string path = "/tmp/ow-project-export-"
            + std::to_string(session.id) + "-" + std::to_string(instance_index++) + ".stl";
        TempFileGuard input_guard(path);
        FILE* file = std::fopen(path.c_str(), "wb");
        if (!file) {
            error = "unable to stage project export STL";
            return false;
        }
        const std::size_t written = std::fwrite(
            session.project_object_blob.data() + mesh->offset,
            1,
            mesh->length,
            file
        );
        std::fclose(file);
        if (written != mesh->length) {
            error = "unable to stage complete project export STL";
            return false;
        }

        Slic3r::Model loaded;
        if (!Slic3r::load_stl(path.c_str(), &loaded, "project-export")) {
            error = "unable to load project export STL";
            return false;
        }
        Slic3r::Transform3d transform = Slic3r::Transform3d::Identity();
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                transform.matrix()(row, column) = instance.matrix[
                    static_cast<std::size_t>(row * 4 + column)
                ];
        for (const auto* loaded_object : loaded.objects) {
            if (!loaded_object)
                continue;
            Slic3r::TriangleMesh mesh_value = loaded_object->raw_mesh();
            mesh_value.transform(transform, true);
            if (output.empty())
                output = std::move(mesh_value);
            else
                output.merge(mesh_value);
        }
    }
    if (output.empty()) {
        error = "project contains no printable geometry";
        return false;
    }
    return true;
}

// The project adapter is defined before the legacy slice entry points, while
// these native-print helpers are shared with those entry points below.
static void zero_plate_origin(Slic3r::Print& print);
static void set_is_bbl_printer(
    Slic3r::Print& print,
    const Slic3r::DynamicPrintConfig& config
);
static void throw_if_cancelled(const ActiveSliceGuard& operation);
static std::string serialize_slice_statistics(
    const Slic3r::Print& print,
    const Slic3r::GCodeProcessorResult& processor_result
);
static void publish_slice_statistics(OrcSession& session, std::string statistics_json);
static void apply_adaptive_layer_height(Slic3r::Print& print, float quality_factor);
static void clamp_wipe_tower_to_bed(
    Slic3r::DynamicPrintConfig& config,
    const Slic3r::Model& model,
    double bed_x,
    double bed_y
);

// The released 0.2 multi-object entry point intentionally exposes the older
// decomposed transform table. The 0.3 project manifest is different: its
// matrix is the source-of-truth affine transform and may contain a valid
// shear or a non-zero Z translation. Build the native model directly for the
// project path so the common API does not inherit the legacy transform limit.
static onewasm_status_t build_project_plate_model(
    OrcSession& session,
    const ProjectManifestInput& manifest,
    const std::string& plate_id,
    Slic3r::Model& model,
    std::string& error
) {
    std::size_t instance_index = 0;
    for (const auto& instance : manifest.instances) {
        if (instance.plate_id != plate_id)
            continue;

        const auto* object_input = find_project_object(manifest, instance.object_id);
        const auto* mesh_input = object_input
            ? find_project_mesh(manifest, object_input->mesh_id) : nullptr;
        if (!object_input || !mesh_input) {
            error = "project instance contains an unresolved object or mesh";
            return ONEWASM_ERR_VALIDATION;
        }
        if (!mesh_input->has_data_range
            || static_cast<std::uint64_t>(mesh_input->offset) + mesh_input->length
                > session.project_object_blob.size()) {
            error = "selected project uses engine-owned geometry; Orca project slicing needs a host-readable mesh asset";
            return ONEWASM_ERR_UNSUPPORTED;
        }

        const std::string path = "/tmp/ow-project-slice-"
            + std::to_string(session.id) + "-" + std::to_string(instance_index++) + ".stl";
        TempFileGuard input_guard(path);
        FILE* file = std::fopen(path.c_str(), "wb");
        if (!file) {
            error = "unable to stage project slice STL";
            return ONEWASM_ERR_INPUT_IO;
        }
        const std::size_t written = std::fwrite(
            session.project_object_blob.data() + mesh_input->offset,
            1,
            mesh_input->length,
            file
        );
        std::fclose(file);
        if (written != mesh_input->length) {
            error = "unable to stage complete project slice STL";
            return ONEWASM_ERR_INPUT_IO;
        }

        const std::size_t first_object = model.objects.size();
        if (!Slic3r::load_stl(path.c_str(), &model, object_input->id.c_str())) {
            error = "OrcaSlicer could not load project mesh " + mesh_input->id;
            return ONEWASM_ERR_INPUT_FORMAT;
        }
        if (model.objects.size() == first_object) {
            error = "project mesh contains no printable objects: " + mesh_input->id;
            return ONEWASM_ERR_EMPTY_INPUT;
        }

        Slic3r::Transform3d matrix = Slic3r::Transform3d::Identity();
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                matrix.matrix()(row, column) = instance.matrix[
                    static_cast<std::size_t>(row * 4 + column)
                ];
            }
        }
        const Slic3r::Geometry::Transformation transformation(matrix);
        for (std::size_t object_index = first_object;
             object_index < model.objects.size(); ++object_index) {
            auto* object = model.objects[object_index];
            if (!object)
                continue;
            if (!object_input->label.empty())
                object->name = object_input->label;
            auto* native_instance = object->add_instance();
            if (!native_instance) {
                error = "OrcaSlicer could not allocate a project instance";
                return ONEWASM_ERR_OUTPUT;
            }
            native_instance->set_transformation(transformation);
            if (object_input->extruder_id > 0)
                object->config.set("extruder", object_input->extruder_id);
        }
    }

    if (model.objects.empty()) {
        error = "selected project plate contains no objects";
        return ONEWASM_ERR_EMPTY_INPUT;
    }
    return ONEWASM_OK;
}

static onewasm_status_t slice_project_model(
    OrcSession& session,
    Slic3r::Model& model,
    ActiveSliceGuard& operation,
    uint8_t** out_gcode,
    uint32_t* out_len
) {
    try {
        throw_if_cancelled(operation);
        clamp_wipe_tower_to_bed(
            session.config,
            model,
            2.0 * session.bed_cx,
            2.0 * session.bed_cy
        );
        Slic3r::Print print;
        operation.attach(print);
        ActivePrintGuard print_guard(operation);
        print.apply(model, session.config);
        zero_plate_origin(print);
        set_is_bbl_printer(print, session.config);
        if (session.remove_mixed_temp_restriction)
            print.set_check_multi_filaments_compatibility(false);
        if (session.adaptive_layer_height)
            apply_adaptive_layer_height(print, session.adaptive_layer_height_quality);
        attach_progress_callback(print, session);

        Slic3r::StringObjectException warning;
        Slic3r::StringObjectException validation_error = print.validate(&warning);
        if (!validation_error.string.empty()) {
            record_error(session, validation_error.string);
            return ONEWASM_ERR_VALIDATION;
        }

        try {
            throw_if_cancelled(operation);
            print.process();
            throw_if_cancelled(operation);
        } catch (const Slic3r::CanceledException&) {
            record_error(session, "slice cancelled");
            return ONEWASM_ERR_CANCELLED;
        } catch (const Slic3r::SlicingError& exception) {
            record_error(session, exception.what());
            return ONEWASM_ERR_SLICE;
        }

        TempFileGuard output_guard("/tmp/ow_out.gcode");
        Slic3r::GCodeProcessorResult processor_result;
        {
            Slic3r::GCode gcode;
            gcode.do_export(&print, "/tmp/ow_out.gcode", &processor_result, nullptr);
        }
        throw_if_cancelled(operation);
        const std::string statistics_json = serialize_slice_statistics(print, processor_result);

        FILE* file = std::fopen("/tmp/ow_out.gcode", "rb");
        if (!file) {
            record_error(session, "gcode export produced no output");
            return ONEWASM_ERR_OUTPUT;
        }
        if (std::fseek(file, 0, SEEK_END) != 0) {
            std::fclose(file);
            record_error(session, "unable to seek G-code output");
            return ONEWASM_ERR_OUTPUT;
        }
        const long size = std::ftell(file);
        if (size < 0 || static_cast<unsigned long long>(size) > UINT32_MAX) {
            std::fclose(file);
            record_error(session, "G-code output exceeds the C ABI length limit");
            return ONEWASM_ERR_OUTPUT;
        }
        std::rewind(file);
        auto* buffer = static_cast<uint8_t*>(std::malloc(std::max<long>(1, size)));
        if (!buffer) {
            std::fclose(file);
            record_error(session, "out of memory");
            return ONEWASM_ERR_OUTPUT;
        }
        const std::size_t read = std::fread(buffer, 1, static_cast<std::size_t>(size), file);
        std::fclose(file);
        if (read != static_cast<std::size_t>(size)) {
            std::free(buffer);
            record_error(session, "unable to read complete G-code output");
            return ONEWASM_ERR_OUTPUT;
        }
        *out_gcode = buffer;
        *out_len = static_cast<uint32_t>(size);
        publish_slice_statistics(session, statistics_json);
        return ONEWASM_OK;
    } catch (const Slic3r::CanceledException&) {
        record_error(session, "slice cancelled");
        return ONEWASM_ERR_CANCELLED;
    } catch (const std::exception& exception) {
        record_error(session, exception.what());
        return ONEWASM_ERR_INTERNAL;
    }
}

struct ProjectOutputFailureGuard {
    OrcSession& session;
    bool committed{false};

    ~ProjectOutputFailureGuard() {
        if (!committed)
            clear_project_outputs(session);
    }
};

struct ProjectSliceRequest {
    std::vector<std::string> plate_ids;
    bool include_gcode = true;
    bool include_statistics = true;
};

static bool parse_project_slice_request(const uint8_t* request_data,
                                        std::uint32_t request_len,
                                        const ProjectManifestInput& manifest,
                                        ProjectSliceRequest& result,
                                        std::string& error) {
    if (!request_data || request_len == 0) {
        error = "project slice request is empty";
        return false;
    }
    nlohmann::json request;
    try {
        request = nlohmann::json::parse(std::string(
            reinterpret_cast<const char*>(request_data), static_cast<std::size_t>(request_len)));
    } catch (const std::exception& exception) {
        error = std::string("project slice request is not valid JSON: ") + exception.what();
        return false;
    }
    if (!request.is_object() || !request.contains("schemaVersion")
        || !request.at("schemaVersion").is_string()
        || request.at("schemaVersion").get<std::string>() != "0.3") {
        error = "project slice request schemaVersion must be 0.3";
        return false;
    }
    if (!reject_unknown_keys(
            request,
            {"schemaVersion", "plateSelection", "plateIds", "includeGcode", "includeStatistics"},
            "project slice request",
            error))
        return false;
    if (!request.contains("plateSelection") || !request.at("plateSelection").is_string()) {
        error = "plateSelection must be a string";
        return false;
    }
    const std::string selection = request.at("plateSelection").get<std::string>();
    if (selection == "all") {
        if (request.contains("plateIds")) {
            error = "plateIds must be omitted when plateSelection is all";
            return false;
        }
        result.plate_ids = manifest.plate_ids;
    } else if (selection == "selected") {
        if (!request.contains("plateIds") || !request.at("plateIds").is_array()
            || request.at("plateIds").empty()) {
            error = "plateIds must be a non-empty array for selected plate slicing";
            return false;
        }
        std::set<std::string> unique_ids;
        for (const auto& value : request.at("plateIds")) {
            std::string id;
            if (!valid_project_id(value, id, "plateIds[]", error))
                return false;
            if (!unique_ids.insert(id).second) {
                error = "plateIds contains a duplicate id: " + id;
                return false;
            }
            if (std::find(manifest.plate_ids.begin(), manifest.plate_ids.end(), id)
                == manifest.plate_ids.end()) {
                error = "plateIds contains an unknown plate: " + id;
                return false;
            }
            result.plate_ids.push_back(std::move(id));
        }
    } else {
        error = "plateSelection must be all or selected";
        return false;
    }
    if (result.plate_ids.empty()) {
        error = "project contains no selectable plates";
        return false;
    }
    if (request.contains("includeGcode") && !request.at("includeGcode").is_boolean()) {
        error = "includeGcode must be a boolean";
        return false;
    }
    if (request.contains("includeStatistics") && !request.at("includeStatistics").is_boolean()) {
        error = "includeStatistics must be a boolean";
        return false;
    }
    result.include_gcode = request.value("includeGcode", true);
    result.include_statistics = request.value("includeStatistics", true);
    return true;
}

static bool parse_prepared_transform(const nlohmann::json& value,
                                     ObjectTransformInput& result,
                                     std::string& error) {
    if (!value.is_object() || !value.contains("scale") || !value.contains("rotation")
        || !value.contains("mirror") || !value.contains("offset")) {
        error = "OrcaSlicer returned an invalid project transform";
        return false;
    }

    const auto read_vector = [&error](const nlohmann::json& source,
                                      const char* name,
                                      Slic3r::Vec3d& target) {
        if (!source.is_array() || source.size() != 3) {
            error = std::string("prepared transform ") + name
                + " must contain three numbers";
            return false;
        }
        for (std::size_t index = 0; index < 3; ++index) {
            double number = 0.;
            if (!project_number(source[index], number, name, error))
                return false;
            target[static_cast<int>(index)] = number;
        }
        return true;
    };

    if (!read_vector(value.at("scale"), "scale", result.scale)
        || !read_vector(value.at("rotation"), "rotation", result.rotation)
        || !read_vector(value.at("mirror"), "mirror", result.mirror)) {
        return false;
    }
    if (result.scale.x() <= 0. || result.scale.y() <= 0. || result.scale.z() <= 0.) {
        error = "prepared transform scale must be greater than zero";
        return false;
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (result.mirror[axis] != 1. && result.mirror[axis] != -1.) {
            error = "prepared transform mirror values must be 1 or -1";
            return false;
        }
    }

    const auto& offset = value.at("offset");
    if (offset.is_null()) {
        result.has_offset = false;
        result.offset_x = 0.;
        result.offset_y = 0.;
        return true;
    }
    if (!offset.is_array() || offset.size() != 2) {
        error = "prepared transform offset must be null or contain two numbers";
        return false;
    }
    if (!project_number(offset[0], result.offset_x, "offset", error)
        || !project_number(offset[1], result.offset_y, "offset", error)) {
        return false;
    }
    result.has_offset = true;
    return true;
}

static bool set_project_output(const std::string& value,
                               uint8_t** out_data,
                               uint32_t* out_len,
                               std::string& error) {
    if (value.size() > UINT32_MAX) {
        error = "project output exceeds the C ABI length limit";
        return false;
    }
    auto* buffer = static_cast<uint8_t*>(std::malloc(std::max<std::size_t>(1, value.size())));
    if (!buffer) {
        error = "out of memory";
        return false;
    }
    if (!value.empty())
        std::memcpy(buffer, value.data(), value.size());
    *out_data = buffer;
    *out_len = static_cast<uint32_t>(value.size());
    return true;
}

static bool project_plate_has_instances(const ProjectManifestInput& manifest,
                                        const std::string& plate_id) {
    return std::any_of(manifest.instances.begin(), manifest.instances.end(),
                       [&plate_id](const auto& instance) {
                           return instance.plate_id == plate_id;
                       });
}

// Print::m_origin (the plate offset, read via get_plate_origin()) has no
// default member initializer either, and Vec3d is an Eigen type whose default
// constructor leaves its components uninitialized. Only the desktop GUI's
// PartPlate code ever calls set_plate_origin(), so in this headless bridge it
// stays garbage — and PrintInstance::shift_without_plate_offset() (Print.cpp)
// subtracts it from every instance shift:
//     return shift - Point(scaled(plate_offset.x()), scaled(plate_offset.y()));
// Brim.cpp's append_and_translate() then translates the brim/no-brim polygons
// by that value and hands them to Clipper, which rejects anything beyond its
// coordinate range ("Coordinate outside allowed range"). Because the garbage
// depends on whatever happens to be on the stack, this reproduced only for
// some meshes and flipped with unrelated code changes — the synthetic
// icosphere in smoke-test.mjs started failing purely because an unrelated
// bridge edit shifted the binary layout. Zero it explicitly, exactly like
// set_is_bbl_printer() below does for the other uninitialised Print member.
static void zero_plate_origin(Slic3r::Print& print) {
    print.set_plate_origin(Slic3r::Vec3d::Zero());
}

// Print::m_isBBLPrinter (accessed via is_BBL_printer()) has no default
// member initializer and is otherwise only ever assigned by desktop GUI code
// (BackgroundSlicingProcess.cpp, OrcaSlicer.cpp) from PresetBundle's vendor
// info — none of which runs in this headless bridge. Left unset, it reads
// as uninitialized memory, which GCode.cpp uses to pick between the Bambu
// and "compatible" (;TYPE: ...) reserved-tag formats when writing extrusion
// role comments. A mismatch between that and what desktop OrcaSlicer expects
// when re-opening the file (it infers the format from the exported
// printer_model config) makes every extrusion role show as "Undefined" in
// the Line Type breakdown. Mirror the same "Bambu Lab" vendor-name prefix
// check GUI code uses so it's set deterministically from the same
// printer_model the JS layer already sends.
static void set_is_bbl_printer(Slic3r::Print& print, const Slic3r::DynamicPrintConfig& config) {
    print.is_BBL_printer() = boost::starts_with(config.opt_string("printer_model"), "Bambu Lab");
}

static void throw_if_cancelled(const ActiveSliceGuard& operation) {
    if (operation.cancelled())
        throw Slic3r::CanceledException();
}

static double filament_length_from_volume(double volume_mm3, double diameter_mm) {
    if (!std::isfinite(volume_mm3) || volume_mm3 <= 0.0 || !std::isfinite(diameter_mm) || diameter_mm <= 0.0)
        return 0.0;
    const double cross_section_mm2 = std::acos(-1.0) * std::pow(diameter_mm * 0.5, 2.0);
    return volume_mm3 / cross_section_mm2;
}

static nlohmann::json optional_non_negative(double value) {
    return std::isfinite(value) && value >= 0.0 ? nlohmann::json(value) : nlohmann::json(nullptr);
}

// Convert Orca's native GCodeProcessorResult into the versioned common
// statistics document. The processor stores filament volumes in mm^3; the
// common schema exposes both volume in cm^3 and length in mm, using the
// engine's per-filament diameter/density/cost vectors for conversion.
static std::string serialize_slice_statistics(const Slic3r::Print& print,
                                              const Slic3r::GCodeProcessorResult& processor_result) {
    const auto& native = processor_result.print_statistics;
    const auto& config = print.config();
    const auto normal_index = static_cast<std::size_t>(Slic3r::PrintEstimatedStatistics::ETimeMode::Normal);
    const auto silent_index = static_cast<std::size_t>(Slic3r::PrintEstimatedStatistics::ETimeMode::Stealth);

    std::size_t extruder_count = std::max({
        processor_result.filament_diameters.size(),
        processor_result.filament_densities.size(),
        processor_result.filament_costs.size(),
        config.filament_type.values.size(),
        config.filament_diameter.values.size(),
    });
    for (const auto& [id, unused] : native.total_volumes_per_extruder)
        extruder_count = std::max(extruder_count, id + 1);
    for (const auto& [id, unused] : native.wipe_tower_volumes_per_extruder)
        extruder_count = std::max(extruder_count, id + 1);
    for (const auto& [id, unused] : native.flush_per_filament)
        extruder_count = std::max(extruder_count, id + 1);

    std::vector<double> length_mm(extruder_count, 0.0);
    std::vector<double> volume_cm3(extruder_count, 0.0);
    std::vector<double> mass_g(extruder_count, 0.0);
    std::vector<nlohmann::json> cost(extruder_count, nlohmann::json(0.0));
    std::vector<double> wipe_tower_length_mm(extruder_count, 0.0);
    std::vector<double> flush_length_mm(extruder_count, 0.0);
    std::vector<nlohmann::json> filament_types(extruder_count, nlohmann::json(nullptr));

    auto vector_value = [](const auto& values, std::size_t id, double fallback) {
        return id < values.size() && std::isfinite(values[id]) ? values[id] : fallback;
    };
    auto config_diameter = [&config](std::size_t id) {
        return id < config.filament_diameter.values.size() ? config.filament_diameter.values[id] : 0.0;
    };
    auto diameter_for = [&](std::size_t id) {
        return vector_value(processor_result.filament_diameters, id, config_diameter(id));
    };

    for (std::size_t id = 0; id < extruder_count; ++id) {
        if (id < config.filament_type.values.size())
            filament_types[id] = config.filament_type.values[id];
    }

    double total_length_mm = 0.0;
    double total_volume_cm3 = 0.0;
    double total_mass_g = 0.0;
    double total_filament_cost = 0.0;
    for (const auto& [id, volume] : native.total_volumes_per_extruder) {
        const double diameter = diameter_for(id);
        const double length = filament_length_from_volume(volume, diameter);
        const double volume_cm3_value = std::max(0.0, volume * 0.001);
        const double density = vector_value(processor_result.filament_densities, id, 0.0);
        const double mass = volume_cm3_value * std::max(0.0, density);
        const double filament_cost = vector_value(processor_result.filament_costs, id, 0.0);
        const double cost_value = mass * std::max(0.0, filament_cost) * 0.001;
        length_mm[id] = length;
        volume_cm3[id] = volume_cm3_value;
        mass_g[id] = mass;
        cost[id] = cost_value;
        total_length_mm += length;
        total_volume_cm3 += volume_cm3_value;
        total_mass_g += mass;
        total_filament_cost += cost_value;
    }

    auto map_volume_to_length = [&](const std::map<std::size_t, double>& values,
                                    std::vector<double>& destination) {
        for (const auto& [id, volume] : values)
            destination[id] = filament_length_from_volume(volume, diameter_for(id));
    };
    map_volume_to_length(native.wipe_tower_volumes_per_extruder, wipe_tower_length_mm);
    map_volume_to_length(native.flush_per_filament, flush_length_mm);

    nlohmann::json result;
    result["schemaVersion"] = "0.2";
    result["timeSeconds"] = {
        {"normal", normal_index < native.modes.size() ? optional_non_negative(native.modes[normal_index].time) : nlohmann::json(nullptr)},
        {"silent", silent_index < native.modes.size() && native.modes[silent_index].time > 0.0f
            ? optional_non_negative(native.modes[silent_index].time) : nlohmann::json(nullptr)},
        {"firstLayer", optional_non_negative(processor_result.initial_layer_time)},
    };
    result["filament"] = {
        {"lengthMmByExtruder", length_mm},
        {"volumeCm3ByExtruder", volume_cm3},
        {"massGByExtruder", mass_g},
        {"costByExtruder", cost},
        {"totalLengthMm", total_length_mm},
        {"totalVolumeCm3", total_volume_cm3},
        {"totalMassG", total_mass_g},
        {"totalCost", total_filament_cost},
        {"wipeTowerLengthMmByExtruder", wipe_tower_length_mm},
        {"flushLengthMmByExtruder", flush_length_mm},
        {"filamentTypesByExtruder", filament_types},
    };
    result["toolChanges"] = processor_result.print_statistics.total_extruder_changes;
    result["initialExtruderId"] = print.print_statistics().initial_tool;
    result["printingExtruders"] = nlohmann::json::array();
    for (const auto& [id, volume] : native.total_volumes_per_extruder) {
        if (volume > 0.0)
            result["printingExtruders"].push_back(id);
    }
    return result.dump();
}

static void publish_slice_statistics(OrcSession& session, std::string statistics_json) {
    std::lock_guard<std::mutex> lock(session.control_mutex);
    session.last_statistics_json = std::move(statistics_json);
    session.has_last_statistics = true;
}

// Compute a per-object adaptive layer height profile and store it on each
// object, matching what desktop OrcaSlicer's "Adaptive" button does before a
// slice (GLCanvas3D::LayersEditing::adaptive_layer_height_profile). Must run
// after print.apply() — that's what populates each PrintObject's slicing
// parameters (update_slicing_parameters), and layer_height_profile_adaptive
// needs them — but before print.process(), which reads the profile back off
// the model object (PrintObject::update_layer_height_profile, via slice()).
//
// The profile is set on print.objects()' model objects, which are the copies
// Print owns after apply() (Print::apply → m_model.assign_copy), so this is
// the same object process() later reads. Using the PrintObject's own
// slicing_parameters() guarantees the profile's Z samples line up exactly with
// what update_layer_height_profile validates against, so it is not discarded.
// objects_mutable() hands back non-const PrintObject*, so model_object()
// resolves to the non-const overload (PrintObjectBase has both) — no cast
// needed to write the profile, unlike desktop's GLCanvas3D path where the
// object is genuinely held as const ModelObject*.
static void apply_adaptive_layer_height(Slic3r::Print& print, float quality_factor) {
    for (Slic3r::PrintObject* obj : print.objects_mutable()) {
        const Slic3r::SlicingParameters& sp = obj->slicing_parameters();
        if (!sp.valid) continue;
        Slic3r::ModelObject* model_object = obj->model_object();
        std::vector<double> profile = Slic3r::layer_height_profile_adaptive(sp, *model_object, quality_factor);
        model_object->layer_height_profile.set(std::move(profile));
    }
}

// Place the prime (wipe) tower the way desktop OrcaSlicer does. Its
// PartPlateList::set_default_wipe_tower_pos_for_plate — default anchor, size
// estimate, then clamp into the plate — lives in the GUI and never runs
// headless, so without this a slice uses the raw PrintConfig position (15, 220)
// unchanged: fine on a ~256 mm bed, but off the back edge of a smaller one (a
// 210 mm Prusa MK4) and, on a larger one (a 350 mm Voron), a different spot
// than desktop picks. This ports that routine: anchor at the desktop default,
// estimate the footprint from the loaded model's height — which the JS config
// layer cannot, running before any mesh exists and with a height-driven depth —
// and clamp. The result matches what desktop computes from the same config.
// The multi-material config that turns the tower on is built in
// withPrimeTowerAddressing()/multiFilamentPassthrough() in host profile adapter
// (#163); this is the placement half that has to live where the mesh does.
static void clamp_wipe_tower_to_bed(Slic3r::DynamicPrintConfig& config,
                                    const Slic3r::Model& model,
                                    double bed_x, double bed_y) {
    const auto* enable = config.option<Slic3r::ConfigOptionBool>("enable_prime_tower");
    if (!enable || !enable->value) return;

    // With fewer than two filaments there is no tool change to purge, so the
    // engine builds no tower and there is nothing to place.
    const auto* colours = config.option<Slic3r::ConfigOptionStrings>("filament_colour");
    const int filaments = colours ? static_cast<int>(colours->values.size()) : 1;
    if (filaments < 2) return;

    const auto* nozzles = config.option<Slic3r::ConfigOptionFloats>("nozzle_diameter");
    const int nozzle_count = (nozzles && !nozzles->values.empty())
        ? static_cast<int>(nozzles->values.size()) : 1;

    // Tallest object drives the tower's height-based minimum depth.
    double max_height = 0.0;
    for (const auto* obj : model.objects)
        max_height = std::max(max_height, obj->bounding_box_exact().size().z());

    auto opt_float = [&](const char* key, double dflt) -> double {
        const auto* o = config.option(key);
        return o ? o->getFloat() : dflt;
    };

    // Replicate PartPlate::estimate_wipe_tower_size for the default rib wall:
    // the footprint is a square of side `depth`, and prime_tower_width does not
    // enter it. One term is dropped: desktop adds a per-change filament-change
    // volume for a 2-nozzle machine, which would slightly enlarge `depth` there.
    // Omitting it only matters if a dual-nozzle machine slices on a bed shallow
    // enough to clamp, and those ship deep beds (an H2D is 320 mm) where the
    // tower never reaches the edge — so the simpler estimate is safe in practice.
    const double layer_height  = opt_float("layer_height", 0.2);
    const double wipe_volume   = opt_float("prime_volume", 45.0);
    const double extra_spacing = opt_float("prime_tower_infill_gap", 150.0) / 100.0;
    double       rib_width     = opt_float("wipe_tower_rib_width", 8.0);
    const double extra_rib_len = opt_float("wipe_tower_extra_rib_length", 0.0);

    const double volume = wipe_volume * (nozzle_count == 2 ? filaments : (filaments - 1));
    double depth = std::sqrt(volume / layer_height * extra_spacing);
    const double min_depth = Slic3r::WipeTower::get_limit_depth_by_height(static_cast<float>(max_height));
    const double volume_depth = depth;
    depth = std::max(min_depth, depth);
    rib_width = std::min(rib_width, depth / 2.0);
    // `max(depth + extra_rib_len, volume_depth)` always resolves to the first
    // operand here — `depth` is already >= volume_depth from the max() above and
    // extra_rib_len is non-negative. It is kept verbatim (rather than reduced to
    // `depth + extra_rib_len`) to mirror desktop's estimate_wipe_tower_size line
    // for line, so a future re-sync against upstream diffs cleanly.
    depth = rib_width / std::sqrt(2.0) + std::max(depth + extra_rib_len, volume_depth);
    const double size = depth; // rib tower footprint is square

    double brim = opt_float("prime_tower_brim_width", 3.0);
    if (brim < 0) brim = Slic3r::WipeTower::get_auto_brim_by_height(static_cast<float>(max_height));
    const double margin = WIPE_TOWER_MARGIN + brim;

    // Desktop OrcaSlicer's default tower position (set_default_wipe_tower_pos_
    // for_plate): a top-left-ish anchor, or the i3/bed-slinger variant. The raw
    // PrintConfig default (15, 220) is deliberately NOT used as the start — it
    // is only an unplaced fallback, and since the tower is new here (#163) there
    // is no prior placement to preserve. Our profiles never carry
    // printer_structure (it stays psUndefine), so this resolves to the CoreXY
    // anchor for every printer — exactly what desktop computes from the same
    // config, so a bed the tower already fits (a 350 mm Voron) lands it in the
    // same spot desktop would rather than a different-but-valid corner.
    double x = 165.0; // WIPE_TOWER_DEFAULT_X_POS
    double y = 250.0; // WIPE_TOWER_DEFAULT_Y_POS
    const auto* structure = config.option<Slic3r::ConfigOptionEnum<Slic3r::PrinterStructure>>("printer_structure");
    if (structure && structure->value == Slic3r::psI3) {
        x = 0.0;   // I3_WIPE_TOWER_DEFAULT_X_POS
        y = 250.0; // I3_WIPE_TOWER_DEFAULT_Y_POS
    }

    // Clamp into the plate, in plate-local coordinates (the bridge zeroes the
    // plate origin, so the bed is [0,bed_x] x [0,bed_y]). Same if/else-if shape
    // as set_default_wipe_tower_pos_for_plate: prefer clamping down from the far
    // edge, only lift off the near edge when it wasn't already past the far one.
    if (x + margin + size > bed_x) x = bed_x - size - margin;
    else if (x < margin)           x = margin;
    if (y + margin + size > bed_y) y = bed_y - size - margin;
    else if (y < margin)           y = margin;
    // A tower wider than the bed itself can't be satisfied; keep the origin on
    // the bed rather than hand the engine a negative coordinate.
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    config.option<Slic3r::ConfigOptionFloats>("wipe_tower_x", true)->values = { x };
    config.option<Slic3r::ConfigOptionFloats>("wipe_tower_y", true)->values = { y };
}

// ── public API ────────────────────────────────────────────────────────────────
extern "C" {

/** Allocate a new engine session. Returns 0 (null) on allocation failure. */
EMSCRIPTEN_KEEPALIVE
onewasm_session_t onewasm_session_create() {
    try {
        std::unique_ptr<OrcSession> session(new (std::nothrow) OrcSession());
        if (!session)
            return nullptr;
        reset_project(*session);
        return session.release();
    } catch (...) {
        return nullptr;
    }
}

/** Free a session created by onewasm_session_create(). Safe to call with null. */
EMSCRIPTEN_KEEPALIVE
void onewasm_session_destroy(onewasm_session_t session_ptr) {
    delete as_session(session_ptr);
}

/**
 * Initialise the slicer with a JSON config object.
 * All values must be string-encoded exactly as OrcaSlicer stores them
 * (e.g. "0.2", "15%", "1" for true).  Unknown keys are silently ignored.
 */
// Print::get_hrc_by_nozzle_type() (Print.cpp) reads "info/nozzle_info.json"
// relative to Slic3r::resources_dir(), which our bridge never sets (empty
// string) since we ship no /resources tree. The parse always fails there,
// which is handled — but the BOOST_LOG_TRIVIAL(error) call on that failure
// path traps with "memory access out of bounds" inside boost::log's
// single-threaded core on every single slice, even for trivial models.
// Pre-seed the file in MEMFS so the parse succeeds and that log call (and
// whatever makes it crash) is never reached, rather than patching boost::log.
static void ensure_nozzle_info_json() {
    static bool written = false;
    if (written) return;
    ::mkdir("info", 0755); // ignore EEXIST
    if (FILE* f = std::fopen("info/nozzle_info.json", "wb")) {
        static const char kJson[] =
            R"({"nozzle_hrc":{"hardened_steel":55,"stainless_steel":20,"tungsten_carbide":85,"brass":2,"undefine":0}})";
        std::fwrite(kJson, 1, sizeof(kJson) - 1, f);
        std::fclose(f);
    }
    written = true;
}

EMSCRIPTEN_KEEPALIVE
onewasm_status_t onewasm_init(onewasm_session_t session_ptr, const uint8_t* config_data, uint32_t config_len) {
    OrcSession* session = as_session(session_ptr);
    if (!session) return -1;
    session->last_error.clear();
    session->initialized = false;
    session->last_statistics_json.clear();
    session->has_last_statistics = false;
    reset_project(*session);
    ensure_nozzle_info_json();
    if (!config_data || config_len == 0) {
        record_error(*session, "config data is empty");
        return ONEWASM_ERR_CONFIG;
    }
    try {
        const char* json_data = reinterpret_cast<const char*>(config_data);
        auto j = nlohmann::json::parse(json_data, json_data + config_len);
        if (!j.is_object()) { record_error(*session, "config must be a JSON object"); return -2; }

        // Extract bed dimensions (not native OrcaSlicer config keys — used only
        // for model centering below).  The JS layer passes bed_size_x / bed_size_y
        // from the printer preset or from a parsed printable_area.
        {
            auto pick = [&](const char* k, double fallback) -> double {
                if (!j.contains(k)) return fallback;
                const auto& v = j[k];
                if (v.is_number()) return v.get<double>();
                if (v.is_string()) {
                    try { return std::stod(v.get<std::string>()); } catch (...) {}
                }
                return fallback;
            };
            session->bed_cx = pick("bed_size_x", 256.0) / 2.0;
            session->bed_cy = pick("bed_size_y", 256.0) / 2.0;
            session->bed_shape = (j.contains("bed_shape") && j["bed_shape"].is_string())
                ? j["bed_shape"].get<std::string>()
                : "rectangle";
        }

        // Opt-in override of the mixed-nozzle-temperature guard (issue #164),
        // matching desktop's "Remove mixed temperature restriction". Not a
        // native engine config key — read here as a pseudo-key (like bed_size_*
        // above) and applied via Print::set_check_multi_filaments_compatibility
        // before validate() in the slice functions.
        session->remove_mixed_temp_restriction = json_flag(j, "remove_mixed_temp_restriction");

        // Variable (adaptive) layer height (issue #138), matching desktop's
        // Adaptive tool. Pseudo-keys like the two above — not native engine
        // config options — applied to the model's layer_height_profile in
        // apply_adaptive_layer_height() rather than through the config.
        {
            session->adaptive_layer_height = json_flag(j, "adaptive_layer_height");
            session->adaptive_layer_height_quality = 0.5f;
            if (j.contains("adaptive_layer_height_quality")) {
                const auto& v = j["adaptive_layer_height_quality"];
                double q = 0.5;
                if (v.is_number())
                    q = v.get<double>();
                else if (v.is_string()) {
                    try { q = std::stod(v.get<std::string>()); } catch (...) { q = 0.5; }
                }
                // Clamp to the engine's expected 0..1 range; next_layer_height()
                // lerps against it and an out-of-range value would extrapolate
                // past min/max layer height.
                session->adaptive_layer_height_quality = static_cast<float>(std::min(1.0, std::max(0.0, q)));
            }
        }

        // Start from OrcaSlicer's built-in defaults so all required fields exist.
        session->config = Slic3r::DynamicPrintConfig();
        session->config.apply(g_defaults);

        // use_relative_e_distances defaults to true ("Default is checked" —
        // PrintConfig.cpp) but Print::validate() (Print.cpp) hard-fails
        // *every* slice under relative addressing unless layer_gcode
        // contains "G92 E0" — normally supplied by a real printer profile's
        // start/layer G-code, none of which we ship in this headless build.
        // Without this default every slice through the real app failed
        // validation (-6) with "Relative extruder addressing requires
        // resetting the extruder position...". Absolute addressing (0)
        // needs no such G-code and works on effectively all firmwares.
        // Callers can still opt into relative addressing explicitly if they
        // also supply appropriate layer_gcode.
        session->config.set_deserialize_strict("use_relative_e_distances", "0");

        for (auto& [key, val] : j.items()) {
            // Arrays carry multi-value options (per-extruder / per-filament);
            // the separator depends on the option's type — see
            // json_array_to_config_string().
            std::string sv = val.is_array() ? json_array_to_config_string(key, val)
                                            : json_val_to_string(val);
            if (sv.empty()) continue;
            try {
                // set_deserialize_strict builds a ConfigSubstitutionContext with
                // the Disable rule internally; incompatible values throw and are
                // skipped below.
                session->config.set_deserialize_strict(key, sv);
            } catch (...) {
                // silently skip unknown / incompatible keys
            }
        }

        session->initialized = true;
        return ONEWASM_OK;
    } catch (const std::exception& e) {
        record_error(*session, e.what());
        return ONEWASM_ERR_CONFIG;
    }
}

EMSCRIPTEN_KEEPALIVE
onewasm_status_t onewasm_init_profile(
    onewasm_session_t session_ptr,
    const char* format_utf8,
    uint32_t format_len,
    const uint8_t* profile_data,
    uint32_t profile_len
) {
    OrcSession* session = as_session(session_ptr);
    if (!session) return ONEWASM_ERR_INVALID_ARGUMENT;
    session->last_error.clear();
    session->initialized = false;
    session->last_statistics_json.clear();
    session->has_last_statistics = false;
    reset_project(*session);
    if (!format_utf8 || format_len == 0 || !profile_data || profile_len == 0) {
        record_error(*session, "profile format or data is empty");
        return ONEWASM_ERR_INVALID_ARGUMENT;
    }

    const std::string format(format_utf8, format_utf8 + format_len);
    if (format != "project.3mf") {
        record_error(*session, "unsupported OrcaSlicer profile format: " + format);
        return ONEWASM_ERR_UNSUPPORTED;
    }

    const char* tmp_in = "/tmp/ow_profile.3mf";
    try {
        TempFileGuard in_guard(tmp_in);
        FILE* file = std::fopen(tmp_in, "wb");
        if (!file) {
            record_error(*session, "cannot open profile temp file for writing");
            return ONEWASM_ERR_INPUT_IO;
        }
        const std::size_t written = std::fwrite(profile_data, 1, profile_len, file);
        std::fclose(file);
        if (written != profile_len) {
            record_error(*session, "failed to write complete profile data to MEMFS");
            return ONEWASM_ERR_INPUT_IO;
        }

        Slic3r::Model model;
        ModelBackupPathGuard backup_guard(model);
        Slic3r::DynamicPrintConfig loaded_config;
        Slic3r::ConfigSubstitutionContext substitutions(
            Slic3r::ForwardCompatibilitySubstitutionRule::EnableSilent
        );
        Slic3r::PlateDataPtrs plate_data_list;
        std::vector<Slic3r::Preset*> project_presets;
        Loaded3mfResourcesGuard loaded_resources(plate_data_list, project_presets);
        Slic3r::Semver file_version;
        const bool ok = Slic3r::load_bbs_3mf(
            tmp_in, &loaded_config, &substitutions, &model,
            &plate_data_list, &project_presets,
            nullptr, nullptr, &file_version, nullptr,
            Slic3r::LoadStrategy::AddDefaultInstances
                | Slic3r::LoadStrategy::LoadModel
                | Slic3r::LoadStrategy::LoadConfig
        );
        if (!ok) {
            record_error(*session, "OrcaSlicer could not load the project profile");
            return ONEWASM_ERR_INPUT_FORMAT;
        }

        session->config = Slic3r::DynamicPrintConfig();
        session->config.apply(g_defaults);
        session->config.apply(loaded_config);
        session->remove_mixed_temp_restriction = false;
        session->adaptive_layer_height = false;
        session->adaptive_layer_height_quality = 0.5f;
        nlohmann::json native_manifest;
        std::vector<std::uint8_t> native_object_blob;
        std::string project_error;
        if (!append_native_model_project(
                *session,
                model,
                plate_data_list,
                native_manifest,
                native_object_blob,
                project_error
            )) {
            record_error(*session, project_error);
            return ONEWASM_ERR_INPUT_FORMAT;
        }
        session->project_manifest = std::move(native_manifest);
        session->project_object_blob = std::move(native_object_blob);
        session->native_project_blob.assign(profile_data, profile_data + profile_len);
        session->native_project_dirty = false;
        session->initialized = true;
        return ONEWASM_OK;
    } catch (const std::exception& e) {
        record_error(*session, std::string("OrcaSlicer profile load failed: ") + e.what());
        return ONEWASM_ERR_INTERNAL;
    }
}

EMSCRIPTEN_KEEPALIVE
onewasm_status_t onewasm_set_progress_callback(
    onewasm_session_t session_ptr,
    onewasm_progress_callback_t callback,
    void* user_data
) {
    OrcSession* session = as_session(session_ptr);
    if (!session) return ONEWASM_ERR_INVALID_ARGUMENT;
    session->last_error.clear();
    session->progress_callback = callback;
    session->progress_user_data = user_data;
    return ONEWASM_OK;
}

/** Request native cancellation of the active operation without waiting. */
EMSCRIPTEN_KEEPALIVE
onewasm_status_t onewasm_cancel(onewasm_session_t session_ptr) {
    OrcSession* session = as_session(session_ptr);
    if (!session) return ONEWASM_ERR_INVALID_ARGUMENT;

    session->project_cancel_requested.store(true, std::memory_order_release);

    std::shared_ptr<ActiveSlice> operation;
    {
        std::lock_guard<std::mutex> lock(session->control_mutex);
        operation = session->active_slice;
    }
    if (operation)
        operation->cancel();
    return ONEWASM_OK;
}

EMSCRIPTEN_KEEPALIVE
onewasm_status_t onewasm_get_capabilities(uint8_t** out_json, uint32_t* out_len) {
    g_conversion_last_error.clear();
    if (out_json) *out_json = nullptr;
    if (out_len) *out_len = 0;
    if (!out_json || !out_len) {
        record_error("capability output pointers must not be null");
        return ONEWASM_ERR_INVALID_ARGUMENT;
    }

#ifdef SLIC3R_WASM_MT
    constexpr const char* threading_model = "pthreads";
    constexpr const char* requires_sab = "true";
#else
    constexpr const char* threading_model = "single-threaded";
    constexpr const char* requires_sab = "false";
#endif
    const std::string json = std::string(R"({
  "api":{"name":"one-wasm-slicer-api","version":"0.2.0"},
  "engine":{"family":"OrcaSlicer","version":"2.4.2"},
  "runtime":{"threadingModel":")") + threading_model + R"(","supportedHosts":["web","worker","node"],"requiresSharedArrayBuffer":)" + requires_sab + R"(,"requiresCrossOriginIsolated":)" + requires_sab + R"(,"cancellationMode":"cooperative"},
  "configuration":{"initFormats":["orca.native-json"],"fullProfileFormats":["project.3mf"]},
  "features":{"core.session":"supported","core.configuration":"supported","config.fullProfile":"supported","slice.stl.single":"supported","slice.stl.multi":"supported","slice.transforms":"supported","plate.autoOrient":"supported","plate.arrange":"supported","format.objToStl":"supported","format.stepToStl":"supported","format.3mf.read":"supported","format.3mf.write":"supported","runtime.capabilities":"supported","runtime.progress":"supported","runtime.cancellation":"supported","runtime.statistics":"supported","runtime.errors":"supported","runtime.memory":"supported"}
})";
    if (json.size() > UINT32_MAX) {
        record_error("capability document is too large");
        return ONEWASM_ERR_OUTPUT;
    }
    auto* buffer = static_cast<uint8_t*>(std::malloc(std::max<std::size_t>(1, json.size())));
    if (!buffer) {
        record_error("out of memory");
        return ONEWASM_ERR_OUTPUT;
    }
    std::memcpy(buffer, json.data(), json.size());
    *out_json = buffer;
    *out_len = static_cast<uint32_t>(json.size());
    return ONEWASM_OK;
}

EMSCRIPTEN_KEEPALIVE
onewasm_status_t onewasm_get_last_statistics(
    onewasm_session_t session_ptr,
    uint8_t** out_json,
    uint32_t* out_len
) {
    OrcSession* session = as_session(session_ptr);
    if (out_json) *out_json = nullptr;
    if (out_len) *out_len = 0;
    if (!session || !out_json || !out_len)
        return ONEWASM_ERR_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lock(session->control_mutex);
    if (!session->has_last_statistics) {
        session->last_error = "no statistics are available; run a successful slice first";
        return ONEWASM_ERR_NO_DATA;
    }
    if (session->last_statistics_json.size() > UINT32_MAX) {
        session->last_error = "statistics document is too large";
        return ONEWASM_ERR_OUTPUT;
    }
    auto* buffer = static_cast<uint8_t*>(std::malloc(std::max<std::size_t>(1, session->last_statistics_json.size())));
    if (!buffer) {
        session->last_error = "out of memory";
        return ONEWASM_ERR_OUTPUT;
    }
    std::memcpy(buffer, session->last_statistics_json.data(), session->last_statistics_json.size());
    *out_json = buffer;
    *out_len = static_cast<uint32_t>(session->last_statistics_json.size());
    return ONEWASM_OK;
}

EMSCRIPTEN_KEEPALIVE
onewasm_status_t onewasm_project_set_objects(
    onewasm_session_t session_ptr,
    const uint8_t* object_blob,
    uint32_t object_blob_len,
    const uint8_t* manifest_json,
    uint32_t manifest_len
) {
    OrcSession* session = as_session(session_ptr);
    if (!session) return ONEWASM_ERR_INVALID_ARGUMENT;
    session->last_error.clear();
    clear_project_outputs(*session);
    if (!session->initialized) {
        record_error(*session, "call onewasm_init or onewasm_init_profile first");
        return ONEWASM_ERR_INVALID_ARGUMENT;
    }
    if (!object_blob || object_blob_len == 0) {
        record_error(*session, "project object blob is empty");
        return ONEWASM_ERR_EMPTY_INPUT;
    }

    ProjectManifestInput parsed;
    nlohmann::json normalized;
    std::string error;
    if (!parse_project_manifest(manifest_json, manifest_len, object_blob_len, true,
                                parsed, normalized, error)) {
        record_error(*session, error);
        return ONEWASM_ERR_VALIDATION;
    }

    try {
        std::vector<std::uint8_t> retained_blob(object_blob, object_blob + object_blob_len);
        session->project_object_blob = std::move(retained_blob);
        session->project_manifest = std::move(normalized);
        session->native_project_blob.clear();
        session->native_project_dirty = false;
        session->project_cancel_requested.store(false, std::memory_order_release);
        return ONEWASM_OK;
    } catch (const std::bad_alloc&) {
        record_error(*session, "unable to retain the project object blob");
        return ONEWASM_ERR_OUTPUT;
    } catch (const std::exception& exception) {
        record_error(*session, std::string("project state update failed: ") + exception.what());
        return ONEWASM_ERR_INTERNAL;
    }
}

EMSCRIPTEN_KEEPALIVE
onewasm_status_t onewasm_project_get_manifest(
    onewasm_session_t session_ptr,
    uint8_t** out_json,
    uint32_t* out_len
) {
    OrcSession* session = as_session(session_ptr);
    if (out_json) *out_json = nullptr;
    if (out_len) *out_len = 0;
    if (!session) return ONEWASM_ERR_INVALID_ARGUMENT;
    session->last_error.clear();
    if (!out_json || !out_len) {
        record_error(*session, "manifest output pointers must not be null");
        return ONEWASM_ERR_INVALID_ARGUMENT;
    }

    const nlohmann::json manifest = session->project_manifest.is_null()
        ? empty_project_manifest() : session->project_manifest;
    const std::string output = manifest.dump();
    if (!set_project_output(output, out_json, out_len, session->last_error))
        return ONEWASM_ERR_OUTPUT;
    return ONEWASM_OK;
}

EMSCRIPTEN_KEEPALIVE
onewasm_status_t onewasm_project_prepare(
    onewasm_session_t session_ptr,
    const uint8_t* request_json,
    uint32_t request_len,
    uint8_t** out_manifest_json,
    uint32_t* out_len
) {
    OrcSession* session = as_session(session_ptr);
    if (out_manifest_json) *out_manifest_json = nullptr;
    if (out_len) *out_len = 0;
    if (!session) return ONEWASM_ERR_INVALID_ARGUMENT;
    session->last_error.clear();
    if (!session->initialized) {
        record_error(*session, "call onewasm_init or onewasm_init_profile first");
        return ONEWASM_ERR_INVALID_ARGUMENT;
    }
    if (!out_manifest_json || !out_len || !request_json || request_len == 0) {
        record_error(*session, "project prepare request and output pointers must not be empty");
        return ONEWASM_ERR_INVALID_ARGUMENT;
    }

    clear_project_outputs(*session);
    session->project_cancel_requested.store(false, std::memory_order_release);

    nlohmann::json request;
    try {
        request = nlohmann::json::parse(std::string(
            reinterpret_cast<const char*>(request_json), static_cast<std::size_t>(request_len)));
    } catch (const std::exception& exception) {
        record_error(*session, std::string("project prepare request is not valid JSON: ")
            + exception.what());
        return ONEWASM_ERR_VALIDATION;
    }
    if (!request.is_object() || !request.contains("schemaVersion")
        || !request.at("schemaVersion").is_string()
        || request.at("schemaVersion").get<std::string>() != "0.3") {
        record_error(*session, "project prepare request schemaVersion must be 0.3");
        return ONEWASM_ERR_VALIDATION;
    }
    std::string request_error;
    if (!reject_unknown_keys(request, {"schemaVersion", "operation"},
                             "project prepare request", request_error)) {
        record_error(*session, request_error);
        return ONEWASM_ERR_VALIDATION;
    }
    const std::string operation_name = request.value("operation", "");
    const int operation = operation_name == "auto-orient"
        ? ONEWASM_PLATE_AUTO_ORIENT
        : operation_name == "arrange" ? ONEWASM_PLATE_ARRANGE : 0;
    if (operation == 0) {
        record_error(*session, "project prepare operation must be auto-orient or arrange");
        return ONEWASM_ERR_VALIDATION;
    }

    const std::string manifest_text = session->project_manifest.dump();
    if (manifest_text.size() > UINT32_MAX) {
        record_error(*session, "project manifest exceeds the C ABI length limit");
        return ONEWASM_ERR_OUTPUT;
    }
    ProjectManifestInput manifest;
    nlohmann::json normalized;
    std::string error;
    if (!parse_project_manifest(
            reinterpret_cast<const uint8_t*>(manifest_text.data()),
            static_cast<std::uint32_t>(manifest_text.size()),
            static_cast<std::uint32_t>(session->project_object_blob.size()),
            false,
            manifest, normalized, error)) {
        record_error(*session, error);
        return ONEWASM_ERR_VALIDATION;
    }

    nlohmann::json updated = session->project_manifest;
    try {
        for (const auto& plate_id : manifest.plate_ids) {
            if (!project_plate_has_instances(manifest, plate_id))
                continue;

            std::vector<std::uint8_t> blob;
            std::vector<std::uint32_t> offsets;
            std::vector<std::int32_t> extruders;
            std::vector<float> transforms;
            std::vector<std::size_t> manifest_indices;
            if (!collect_project_plate_inputs(
                    *session, manifest, plate_id, blob, offsets, extruders,
                    transforms, manifest_indices,
                    operation == ONEWASM_PLATE_AUTO_ORIENT, error)) {
                record_error(*session, error);
                return ONEWASM_ERR_VALIDATION;
            }
            if (manifest_indices.size() > UINT32_MAX) {
                record_error(*session, "project plate contains too many objects");
                return ONEWASM_ERR_INPUT_FORMAT;
            }

            uint8_t* prepared_data = nullptr;
            uint32_t prepared_len = 0;
            const auto status = onewasm_prepare_plate(
                session_ptr, blob.data(), static_cast<std::uint32_t>(blob.size()),
                offsets.data(), static_cast<std::uint32_t>(manifest_indices.size()),
                transforms.data(), operation, &prepared_data, &prepared_len);
            if (status != ONEWASM_OK) {
                std::free(prepared_data);
                return status;
            }

            nlohmann::json prepared;
            try {
                prepared = nlohmann::json::parse(std::string(
                    reinterpret_cast<const char*>(prepared_data),
                    static_cast<std::size_t>(prepared_len)));
            } catch (const std::exception& exception) {
                std::free(prepared_data);
                record_error(*session, std::string("OrcaSlicer returned invalid transforms: ")
                    + exception.what());
                return ONEWASM_ERR_INTERNAL;
            }
            std::free(prepared_data);
            if (!prepared.is_array() || prepared.size() != manifest_indices.size()) {
                record_error(*session, "OrcaSlicer returned an incomplete project transform list");
                return ONEWASM_ERR_INTERNAL;
            }

            for (std::size_t index = 0; index < manifest_indices.size(); ++index) {
                ObjectTransformInput transform;
                if (!parse_prepared_transform(prepared[index], transform, error)) {
                    record_error(*session, error);
                    return ONEWASM_ERR_INTERNAL;
                }
                updated["instances"][manifest_indices[index]]["transform"]["matrix"] =
                    project_matrix_json(legacy_transform_to_project_matrix(transform, *session));
            }
        }
    } catch (const std::bad_alloc&) {
        record_error(*session, "unable to retain the prepared project manifest");
        return ONEWASM_ERR_OUTPUT;
    } catch (const std::exception& exception) {
        record_error(*session, std::string("OrcaSlicer project preparation failed: ")
            + exception.what());
        return ONEWASM_ERR_INTERNAL;
    }

    const std::string output = updated.dump();
    if (!set_project_output(output, out_manifest_json, out_len, session->last_error))
        return ONEWASM_ERR_OUTPUT;
    session->project_manifest = std::move(updated);
    if (!session->native_project_blob.empty())
        session->native_project_dirty = true;
    return ONEWASM_OK;
}

EMSCRIPTEN_KEEPALIVE
onewasm_status_t onewasm_project_slice(
    onewasm_session_t session_ptr,
    const uint8_t* request_json,
    uint32_t request_len,
    uint8_t** out_result_json,
    uint32_t* out_len
) {
    OrcSession* session = as_session(session_ptr);
    if (out_result_json) *out_result_json = nullptr;
    if (out_len) *out_len = 0;
    if (!session) return ONEWASM_ERR_INVALID_ARGUMENT;
    session->last_error.clear();
    if (!session->initialized) {
        record_error(*session, "call onewasm_init or onewasm_init_profile first");
        return ONEWASM_ERR_INVALID_ARGUMENT;
    }
    if (!out_result_json || !out_len) {
        record_error(*session, "slice result output pointers must not be null");
        return ONEWASM_ERR_INVALID_ARGUMENT;
    }

    // A new project slice invalidates all assets from the previous operation,
    // including when request validation fails. This keeps get_asset from
    // exposing stale output after a failed call.
    clear_project_outputs(*session);
    session->project_cancel_requested.store(false, std::memory_order_release);

    const std::string manifest_text = session->project_manifest.dump();
    if (manifest_text.size() > UINT32_MAX
        || session->project_object_blob.size() > UINT32_MAX) {
        record_error(*session, "project input exceeds the C ABI length limit");
        return ONEWASM_ERR_INPUT_FORMAT;
    }
    ProjectManifestInput manifest;
    nlohmann::json normalized;
    std::string error;
    if (!parse_project_manifest(
            reinterpret_cast<const uint8_t*>(manifest_text.data()),
            static_cast<std::uint32_t>(manifest_text.size()),
            static_cast<std::uint32_t>(session->project_object_blob.size()),
            false,
            manifest, normalized, error)) {
        record_error(*session, error);
        return ONEWASM_ERR_VALIDATION;
    }

    ProjectSliceRequest request;
    if (!parse_project_slice_request(request_json, request_len, manifest, request, error)) {
        record_error(*session, error);
        return ONEWASM_ERR_VALIDATION;
    }

    ActiveSliceGuard operation(*session);
    if (!operation) {
        record_error(*session, "session already has an active operation");
        return ONEWASM_ERR_INVALID_ARGUMENT;
    }
    ProjectOutputFailureGuard output_guard{*session};
    std::map<std::string, std::string> new_assets;
    nlohmann::json result = {
        {"schemaVersion", "0.3"},
        {"plateResults", nlohmann::json::array()},
        {"warnings", nlohmann::json::array()},
    };

    try {
        for (std::size_t plate_index = 0; plate_index < request.plate_ids.size(); ++plate_index) {
            if (session->project_cancel_requested.load(std::memory_order_acquire)) {
                record_error(*session, "project slice cancelled");
                return ONEWASM_ERR_CANCELLED;
            }
            const int base = static_cast<int>((100 * plate_index) / request.plate_ids.size());
            const int end = static_cast<int>((100 * (plate_index + 1)) / request.plate_ids.size());
            ProgressWindow progress_window(*session, base, std::max(1, end - base));
            emit_project_progress(*session, 0, "Loading project plate");

            nlohmann::json plate_result = {
                {"plateId", request.plate_ids[plate_index]},
                {"assets", nlohmann::json::array()},
                {"statistics", nullptr},
                {"warnings", nlohmann::json::array()},
            };

            if (!project_plate_has_instances(manifest, request.plate_ids[plate_index])) {
                plate_result["warnings"].push_back({
                    {"code", "empty-plate"},
                    {"message", "selected project plate contains no objects"},
                });
                result["plateResults"].push_back(std::move(plate_result));
                emit_project_progress(*session, 100, "Finished empty project plate");
                continue;
            }

            Slic3r::Model model;
            const auto build_status = build_project_plate_model(
                *session,
                manifest,
                request.plate_ids[plate_index],
                model,
                error
            );
            if (build_status != ONEWASM_OK) {
                record_error(*session, error);
                return build_status;
            }

            uint8_t* gcode_data = nullptr;
            uint32_t gcode_len = 0;
            const auto slice_status = slice_project_model(
                *session, model, operation, &gcode_data, &gcode_len
            );
            if (slice_status != ONEWASM_OK) {
                std::free(gcode_data);
                return slice_status;
            }

            if (request.include_gcode) {
                const std::string asset_id = "gcode:" + request.plate_ids[plate_index];
                new_assets.emplace(asset_id, std::string(
                    reinterpret_cast<const char*>(gcode_data), static_cast<std::size_t>(gcode_len)));
                plate_result["assets"].push_back({
                    {"id", asset_id},
                    {"kind", "gcode"},
                    {"mimeType", "text/x-gcode"},
                    {"byteLength", new_assets.at(asset_id).size()},
                });
            }
            std::free(gcode_data);

            if (request.include_statistics) {
                uint8_t* statistics_data = nullptr;
                uint32_t statistics_len = 0;
                const auto statistics_status = onewasm_get_last_statistics(
                    session_ptr, &statistics_data, &statistics_len);
                if (statistics_status != ONEWASM_OK) {
                    std::free(statistics_data);
                    return statistics_status;
                }
                try {
                    nlohmann::json statistics = nlohmann::json::parse(std::string(
                        reinterpret_cast<const char*>(statistics_data),
                        static_cast<std::size_t>(statistics_len)));
                    std::free(statistics_data);
                    if (!statistics.is_object()) {
                        record_error(*session, "OrcaSlicer returned invalid slice statistics");
                        return ONEWASM_ERR_INTERNAL;
                    }
                    statistics["schemaVersion"] = "0.3";
                    plate_result["statistics"] = std::move(statistics);
                } catch (const std::exception& exception) {
                    std::free(statistics_data);
                    record_error(*session, std::string("OrcaSlicer returned invalid slice statistics: ")
                        + exception.what());
                    return ONEWASM_ERR_INTERNAL;
                }
            }
            result["plateResults"].push_back(std::move(plate_result));
        }
        if (session->project_cancel_requested.load(std::memory_order_acquire)) {
            record_error(*session, "project slice cancelled");
            return ONEWASM_ERR_CANCELLED;
        }
        emit_project_progress(*session, 100, "Finished project");
    } catch (const std::bad_alloc&) {
        record_error(*session, "unable to retain project slice results");
        return ONEWASM_ERR_OUTPUT;
    } catch (const std::exception& exception) {
        record_error(*session, std::string("OrcaSlicer project slice failed: ")
            + exception.what());
        return ONEWASM_ERR_INTERNAL;
    }

    {
        std::lock_guard<std::mutex> lock(session->control_mutex);
        session->project_assets = std::move(new_assets);
    }
    const std::string result_text = result.dump();
    if (!set_project_output(result_text, out_result_json, out_len, session->last_error))
        return ONEWASM_ERR_OUTPUT;
    output_guard.committed = true;
    return ONEWASM_OK;
}

EMSCRIPTEN_KEEPALIVE
onewasm_status_t onewasm_project_get_asset(
    onewasm_session_t session_ptr,
    const char* asset_id_utf8,
    uint32_t asset_id_len,
    uint8_t** out_data,
    uint32_t* out_len
) {
    OrcSession* session = as_session(session_ptr);
    if (out_data) *out_data = nullptr;
    if (out_len) *out_len = 0;
    if (!session) return ONEWASM_ERR_INVALID_ARGUMENT;
    session->last_error.clear();
    if (!asset_id_utf8 || asset_id_len == 0 || !out_data || !out_len) {
        record_error(*session, "project asset id and output pointers must not be empty");
        return ONEWASM_ERR_INVALID_ARGUMENT;
    }

    const std::string asset_id(asset_id_utf8, asset_id_utf8 + asset_id_len);
    std::string asset;
    {
        std::lock_guard<std::mutex> lock(session->control_mutex);
        const auto found = session->project_assets.find(asset_id);
        if (found == session->project_assets.end()) {
            record_error(*session, "project asset is not available: " + asset_id);
            return ONEWASM_ERR_NO_DATA;
        }
        asset = found->second;
    }
    if (!set_project_output(asset, out_data, out_len, session->last_error))
        return ONEWASM_ERR_OUTPUT;
    return ONEWASM_OK;
}

EMSCRIPTEN_KEEPALIVE
onewasm_status_t onewasm_project_export(
    onewasm_session_t session_ptr,
    const char* format_utf8,
    uint32_t format_len,
    const uint8_t* options_json,
    uint32_t options_len,
    uint8_t** out_result_json,
    uint32_t* out_len
) {
    OrcSession* session = as_session(session_ptr);
    if (out_result_json) *out_result_json = nullptr;
    if (out_len) *out_len = 0;
    if (!session) return ONEWASM_ERR_INVALID_ARGUMENT;
    session->last_error.clear();
    if (!session->initialized) {
        record_error(*session, "call onewasm_init or onewasm_init_profile first");
        return ONEWASM_ERR_INVALID_ARGUMENT;
    }
    if (!format_utf8 || format_len == 0 || !out_result_json || !out_len) {
        record_error(*session, "project export format and output pointers must not be empty");
        return ONEWASM_ERR_INVALID_ARGUMENT;
    }
    clear_project_outputs(*session);
    const std::string format(format_utf8, format_utf8 + format_len);
    if (format != "project.3mf") {
        record_error(*session, "unsupported OrcaSlicer project export format: " + format);
        return ONEWASM_ERR_UNSUPPORTED;
    }

    ProjectExportOptions options;
    std::string error;
    if (!parse_project_export_options(options_json, options_len, options, error)) {
        record_error(*session, error);
        return ONEWASM_ERR_VALIDATION;
    }
    if (options.include_slice_artifacts) {
        record_error(*session, "OrcaWasm project export does not yet write slice artifacts into native 3MF");
        return ONEWASM_ERR_UNSUPPORTED;
    }

    ProjectOutputFailureGuard output_guard{*session};
    nlohmann::json warnings = nlohmann::json::array();
    nlohmann::json omitted_opaque_entries = nlohmann::json::array();
    std::string package;
    try {
        const bool can_passthrough = !session->native_project_blob.empty()
            && !session->native_project_dirty
            && options.preservation != "portable";
        if (can_passthrough) {
            package.assign(
                reinterpret_cast<const char*>(session->native_project_blob.data()),
                session->native_project_blob.size()
            );
        } else {
            if (!session->native_project_blob.empty()) {
                std::vector<std::string> source_entries;
                if (!list_project_zip_entries(*session, session->native_project_blob, source_entries, error)) {
                    record_error(*session, error);
                    return ONEWASM_ERR_INPUT_FORMAT;
                }
                // These are the entries emitted by the headless
                // onewasm_write_3mf path. Do not classify every Metadata/*
                // file as regenerated: plate artifacts, thumbnails, embedded
                // presets, painting data, and custom per-layer data are not
                // retained by this adapter and must be reported as opaque
                // omissions under the 0.3 policy.
                const std::set<std::string> regenerated_entries{
                    "[Content_Types].xml",
                    "_rels/.rels",
                    "3D/3dmodel.model",
                    "3D/_rels/3dmodel.model.rels",
                    "Metadata/project_settings.config",
                    "Metadata/model_settings.config",
                    "Metadata/slice_info.config",
                };
                for (const std::string& entry : source_entries) {
                    if (regenerated_entries.find(entry) != regenerated_entries.end())
                        continue;
                    omitted_opaque_entries.push_back(entry);
                    warnings.push_back({
                        {"code", "opaque-entry-omitted"},
                        {"message", "native project entry was not regenerated: " + entry},
                    });
                }
                if (options.preservation == "require" && !omitted_opaque_entries.empty()) {
                    record_error(*session, "project export cannot satisfy preservation=require; native opaque entries would be omitted");
                    return ONEWASM_ERR_UNSUPPORTED;
                }
            }

            const std::string manifest_text = session->project_manifest.dump();
            if (manifest_text.size() > UINT32_MAX || session->project_object_blob.size() > UINT32_MAX) {
                record_error(*session, "project export input exceeds the C ABI length limit");
                return ONEWASM_ERR_INPUT_FORMAT;
            }
            ProjectManifestInput manifest;
            nlohmann::json normalized;
            if (!parse_project_manifest(
                    reinterpret_cast<const uint8_t*>(manifest_text.data()),
                    static_cast<uint32_t>(manifest_text.size()),
                    static_cast<uint32_t>(session->project_object_blob.size()),
                    true,
                    manifest,
                    normalized,
                    error
                )) {
                record_error(*session, error);
                return ONEWASM_ERR_VALIDATION;
            }
            if (manifest.plate_ids.size() > 1) {
                if (options.preservation == "require") {
                    record_error(*session, "OrcaWasm portable project export cannot preserve multiple logical plates");
                    return ONEWASM_ERR_UNSUPPORTED;
                }
                warnings.push_back({
                    {"code", "logical-plates-flattened"},
                    {"message", "portable OrcaSlicer 3MF export flattens logical plates into one build"},
                });
            }
            if (options.preservation == "portable") {
                warnings.push_back({
                    {"code", "portable-export"},
                    {"message", "export contains geometry and current OrcaSlicer configuration only"},
                });
            }

            Slic3r::TriangleMesh mesh;
            if (!build_project_export_mesh(*session, manifest, mesh, error)) {
                record_error(*session, error);
                return ONEWASM_ERR_UNSUPPORTED;
            }
            const std::string stl_path = "/tmp/ow-project-export-"
                + std::to_string(session->id) + ".stl";
            TempFileGuard stl_guard(stl_path);
            if (!Slic3r::store_stl(stl_path.c_str(), &mesh, true)) {
                record_error(*session, "OrcaSlicer could not serialize the project export mesh");
                return ONEWASM_ERR_OUTPUT;
            }
            long stl_size = 0;
            const char* read_error = nullptr;
            bool out_of_memory = false;
            char* stl_data = read_file_to_buffer(stl_path.c_str(), &stl_size, &read_error, &out_of_memory);
            if (!stl_data) {
                record_error(*session, std::string{"unable to read project export mesh: "}
                    + (read_error ? read_error : "read failed"));
                return out_of_memory ? ONEWASM_ERR_OUTPUT : ONEWASM_ERR_INPUT_IO;
            }
            uint8_t* exported_data = nullptr;
            uint32_t exported_len = 0;
            const auto export_status = onewasm_write_3mf(
                reinterpret_cast<onewasm_session_t>(session),
                reinterpret_cast<const uint8_t*>(stl_data),
                static_cast<uint32_t>(stl_size),
                &exported_data,
                &exported_len
            );
            std::free(stl_data);
            if (export_status != ONEWASM_OK) {
                std::free(exported_data);
                return export_status;
            }
            package.assign(reinterpret_cast<const char*>(exported_data), exported_len);
            onewasm_free(exported_data);
        }

        if (package.empty() || package.size() > UINT32_MAX) {
            record_error(*session, "project export produced an invalid or oversized package");
            return ONEWASM_ERR_OUTPUT;
        }
        nlohmann::json result = {
            {"schemaVersion", "0.3"},
            {"asset", {
                {"id", "project:export"},
                {"kind", "project"},
                {"mimeType", "model/3mf"},
                {"byteLength", package.size()},
            }},
            {"warnings", std::move(warnings)},
            {"omittedOpaqueEntries", std::move(omitted_opaque_entries)},
        };
        {
            std::lock_guard<std::mutex> lock(session->control_mutex);
            session->project_assets["project:export"] = std::move(package);
        }
        if (!set_project_output(result.dump(), out_result_json, out_len, session->last_error))
            return ONEWASM_ERR_OUTPUT;
        output_guard.committed = true;
        return ONEWASM_OK;
    } catch (const std::bad_alloc&) {
        record_error(*session, "unable to retain the project export package");
        return ONEWASM_ERR_OUTPUT;
    } catch (const std::exception& exception) {
        record_error(*session, std::string{"OrcaSlicer project export failed: "} + exception.what());
        return ONEWASM_ERR_INTERNAL;
    }
}

/**
 * Slice an STL file (raw binary, ASCII or binary format).
 * On success *out_gcode points to a malloc'd, null-terminated G-code string
 * and *out_len contains its byte length (excluding the null terminator).
 * Caller must free the buffer with onewasm_free().
 */
EMSCRIPTEN_KEEPALIVE
onewasm_status_t onewasm_slice_stl(
    onewasm_session_t session_ptr,
    const uint8_t* stl_data,
    uint32_t stl_len,
    uint8_t** out_gcode,
    uint32_t* out_len
) {
    OrcSession* session = as_session(session_ptr);
    if (!session) return -1;
    session->last_error.clear();
    if (out_gcode) *out_gcode = nullptr;
    if (out_len) *out_len = 0;
    if (!session->initialized) { record_error(*session, "call onewasm_init first"); return -1; }
    if (!stl_data || stl_len == 0 || !out_gcode || !out_len)
        return -1;

    ActiveSliceGuard operation(*session);
    if (!operation) {
        record_error(*session, "session already has an active operation");
        return ONEWASM_ERR_INVALID_ARGUMENT;
    }

    // Write raw STL bytes into Emscripten's MEMFS so OrcaSlicer can read it.
    {
        FILE* f = std::fopen("/tmp/ow_in.stl", "wb");
        if (!f) { record_error(*session, "cannot open /tmp/ow_in.stl for writing"); return -3; }
        std::fwrite(stl_data, 1, static_cast<std::size_t>(stl_len), f);
        std::fclose(f);
    }

    try {
        throw_if_cancelled(operation);
        // ── load model ───────────────────────────────────────────────
        Slic3r::Model model;
        const bool stl_ok = Slic3r::load_stl("/tmp/ow_in.stl", &model, "object");
        std::remove("/tmp/ow_in.stl"); // MEMFS is RAM-backed; free it as soon as loaded
        if (!stl_ok) {
            record_error(*session, "STL load failed");
            return -4;
        }
        if (model.objects.empty()) {
            record_error(*session, "model contains no objects");
            return -5;
        }
        throw_if_cancelled(operation);

        // ── place model on bed ───────────────────────────────────────
        // Center the mesh in X/Y, then offset to bed centre. Per-object
        // transforms use the multi-object entry point in the common ABI.
        for (auto* obj : model.objects) {
            center_object_xy_only(obj);
            if (obj->instances.empty()) {
                auto* inst = obj->add_instance();
                // Place at bed centre, derived from bed_size_x / bed_size_y in config.
                inst->set_offset(Slic3r::Vec3d(session->bed_cx, session->bed_cy, 0.0));
            }
        }

        // ── configure & slice ────────────────────────────────────────
        Slic3r::Print print;
        operation.attach(print);
        ActivePrintGuard print_guard(operation);
        print.apply(model, session->config);
        zero_plate_origin(print);
        set_is_bbl_printer(print, session->config);
        // When the user opts in (issue #164), turn off the engine's
        // mixed-nozzle-temperature guard so a single-nozzle AMS plate with
        // filaments whose recommended ranges don't overlap (e.g. PLA + PETG)
        // slices instead of failing validation. The incompatible-temperature
        // case then surfaces through validate()'s `warning` out-param (below)
        // rather than as a fatal error.
        if (session->remove_mixed_temp_restriction)
            print.set_check_multi_filaments_compatibility(false);
        // Variable (adaptive) layer height (issue #138) — compute after apply()
        // (slicing parameters are populated), before validate()/process().
        if (session->adaptive_layer_height)
            apply_adaptive_layer_height(print, session->adaptive_layer_height_quality);
        attach_progress_callback(print, *session);

        {
            // Print::validate() returns a StringObjectException whose
            // `string` member holds the error message ("" when valid). The
            // `warning` out-param catches the non-fatal mixed-temperature
            // notice raised once the guard above is disabled — without a
            // non-null pointer to absorb it, validate() would still return
            // that notice as a fatal error. It is intentionally ignored.
            Slic3r::StringObjectException warning;
            Slic3r::StringObjectException err = print.validate(&warning);
            if (!err.string.empty()) { record_error(*session, err.string); return -6; }
        }

        try {
            throw_if_cancelled(operation);
            print.process();
            throw_if_cancelled(operation);
        } catch (const Slic3r::CanceledException&) {
            record_error(*session, "slice cancelled");
            return ONEWASM_ERR_CANCELLED;
        } catch (const Slic3r::SlicingError& e) {
            record_error(*session, e.what());
            return -7;
        }

        // ── export G-code to MEMFS ───────────────────────────────────
        // Guard covers the do_export() call too: if it throws partway through
        // writing, the partial file is still removed on the way out.
        TempFileGuard out_guard("/tmp/ow_out.gcode");
        Slic3r::GCodeProcessorResult processor_result;
        {
            Slic3r::GCode gcode_gen;
            gcode_gen.do_export(&print, "/tmp/ow_out.gcode", &processor_result, nullptr);
        }
        throw_if_cancelled(operation);
        const std::string statistics_json = serialize_slice_statistics(print, processor_result);

        // ── read result back ─────────────────────────────────────────
        FILE* gf = std::fopen("/tmp/ow_out.gcode", "rb");
        if (!gf) { record_error(*session, "gcode export produced no output"); return -8; }

        std::fseek(gf, 0, SEEK_END);
        long sz = std::ftell(gf);
        std::rewind(gf);

        char* buf = static_cast<char*>(std::malloc(static_cast<std::size_t>(sz) + 1));
        if (!buf) {
            std::fclose(gf);
            record_error(*session, "out of memory");
            return -9;
        }
        std::fread(buf, 1, static_cast<std::size_t>(sz), gf);
        std::fclose(gf);
        buf[sz] = '\0';

        if (sz < 0 || static_cast<unsigned long long>(sz) > UINT32_MAX) {
            std::free(buf);
            record_error(*session, "G-code output exceeds the C ABI length limit");
            return ONEWASM_ERR_OUTPUT;
        }
        *out_gcode = reinterpret_cast<uint8_t*>(buf);
        *out_len   = static_cast<uint32_t>(sz);
        publish_slice_statistics(*session, statistics_json);
        return ONEWASM_OK;

    } catch (const Slic3r::CanceledException&) {
        record_error(*session, "slice cancelled");
        return ONEWASM_ERR_CANCELLED;
    } catch (const std::exception& e) {
        record_error(*session, e.what());
        return -9;
    }
}

/**
 * Prepare the current plate without slicing it.
 *
 * operation 1 = auto-orient each object, operation 2 = arrange the plate.
 * The returned JSON contains one transform object per input STL and is
 * consumed by the host before the next slice. A nullable transform table
 * means identity transforms. In the arrange operation finite input offsets
 * are pinned and NaN offsets remain movable.
 */
EMSCRIPTEN_KEEPALIVE
onewasm_status_t onewasm_prepare_plate(
    onewasm_session_t session_ptr,
    const uint8_t* all_stl, uint32_t all_stl_len,
    const uint32_t* offsets, uint32_t n_files,
    const float* transforms,
    int32_t operation,
    uint8_t** out_transforms, uint32_t* out_len)
{
    OrcSession* session = as_session(session_ptr);
    if (!session) return -1;
    session->last_error.clear();
    if (out_transforms) *out_transforms = nullptr;
    if (out_len) *out_len = 0;
    if (!session->initialized) { record_error(*session, "call onewasm_init first"); return -1; }
    if (!all_stl || all_stl_len == 0 || !offsets || n_files == 0 ||
        !out_transforms || !out_len || (operation != 1 && operation != 2)) {
        record_error(*session, "invalid current-plate arguments");
        return -1;
    }

    const char* base = reinterpret_cast<const char*>(all_stl);
    try {
        Slic3r::Model model;
        for (uint32_t i = 0; i < n_files; ++i) {
            const uint32_t start = offsets[i * 2];
            const uint32_t end = offsets[i * 2 + 1];
            if (end <= start || end > all_stl_len) {
                record_error(*session, "invalid offset table");
                return -1;
            }
            const uint32_t len = end - start;
            const std::string path = "/tmp/ow_prepare_" + std::to_string(i) + ".stl";
            TempFileGuard input_guard(path);
            FILE* file = std::fopen(path.c_str(), "wb");
            if (!file) {
                record_error(*session, "cannot write temp STL");
                return -3;
            }
            const std::size_t written = std::fwrite(base + start, 1, static_cast<std::size_t>(len), file);
            std::fclose(file);
            if (written != static_cast<std::size_t>(len)) {
                record_error(*session, "failed to write complete STL data");
                return -3;
            }
            if (!Slic3r::load_stl(path.c_str(), &model, ("object_" + std::to_string(i)).c_str())) {
                record_error(*session, "STL load failed for file " + std::to_string(i));
                return -4;
            }
        }

        if (model.objects.empty()) {
            record_error(*session, "no objects loaded");
            return -5;
        }
        if (model.objects.size() != static_cast<std::size_t>(n_files)) {
            record_error(*session, "current-plate actions require one printable object per STL file");
            return -1;
        }

        std::vector<bool> pinned(model.objects.size(), false);
        std::vector<bool> keep_positions(model.objects.size(), operation == 2);
        for (std::size_t i = 0; i < model.objects.size(); ++i) {
            ObjectTransformInput transform;
            std::string transform_error;
            if (!read_object_transform(transforms, i, transform, transform_error)) {
                record_error(*session, transform_error);
                return -1;
            }
            pinned[i] = transform.has_offset;
            keep_positions[i] = operation == 2 || transform.has_offset;
            auto* obj = model.objects[i];
            center_object_xy_only(obj);
            add_transformed_instance(obj, *session, transform, operation == 1);
            obj->ensure_on_bed();
        }

        if (operation == 1)
            auto_orient_model(model);
        else
            arrange_transformed_model(model, *session, &pinned);

        return write_transform_json(*session, model, keep_positions, out_transforms, out_len);
    } catch (const std::exception& e) {
        record_error(*session, e.what());
        return -9;
    }
}

/**
 * Convert an OBJ file (raw bytes) to a binary STL.
 * On success *out_stl points to a malloc'd buffer containing the STL and
 * *out_len contains its byte length.  Caller must free with onewasm_free().
 *
 * Error codes:
 *   -3  could not write OBJ to MEMFS
 *   -4  OBJ load failed (invalid / unsupported format)
 *   -5  OBJ contains no geometry
 *   -8  STL export failed
 *   -9  unexpected C++ exception
 */
EMSCRIPTEN_KEEPALIVE
onewasm_status_t onewasm_obj_to_stl(
    const uint8_t* obj_data,
    uint32_t obj_len,
    uint8_t** out_stl,
    uint32_t* out_len
) {
    g_conversion_last_error.clear();
    if (out_stl) *out_stl = nullptr;
    if (out_len) *out_len = 0;
    if (!obj_data || obj_len == 0 || !out_stl || !out_len) return -1;

    {
        FILE* f = std::fopen("/tmp/ow_in.obj", "wb");
        if (!f) { record_error("cannot open /tmp/ow_in.obj for writing"); return -3; }
        std::size_t written = std::fwrite(obj_data, 1, static_cast<std::size_t>(obj_len), f);
        std::fclose(f);
        if (written != static_cast<std::size_t>(obj_len)) {
            std::remove("/tmp/ow_in.obj");
            record_error("failed to write complete OBJ data to MEMFS");
            return -3;
        }
    }

    int status = -9;
    try {
        Slic3r::Model model;
        Slic3r::ObjInfo obj_info;
        std::string message;
        if (!Slic3r::load_obj("/tmp/ow_in.obj", &model, obj_info, message, "object")) {
            record_error(message.empty() ? "OBJ load failed" : message);
            status = -4;
        } else if (model.objects.empty()) {
            record_error("OBJ contains no geometry");
            status = -5;
        } else {
            // Merge all volumes from all objects into one mesh
            Slic3r::TriangleMesh combined;
            for (auto* obj : model.objects)
                for (auto* vol : obj->volumes)
                    combined.merge(vol->mesh());

            if (combined.facets_count() == 0) {
                record_error("OBJ contains no printable geometry");
                status = -5;
            } else if (!Slic3r::store_stl("/tmp/ow_out.stl", &combined, true)) {
                record_error("STL export failed");
                status = -8;
            } else {
                FILE* sf = std::fopen("/tmp/ow_out.stl", "rb");
                if (!sf) {
                    record_error("STL export produced no output");
                    status = -8;
                } else {
                    std::fseek(sf, 0, SEEK_END);
                    long sz = std::ftell(sf);
                    std::rewind(sf);
                    if (sz <= 0) {
                        std::fclose(sf);
                        record_error("STL export produced empty output");
                        status = -8;
                    } else {
                        char* buf = static_cast<char*>(std::malloc(static_cast<std::size_t>(sz)));
                        if (!buf) {
                            std::fclose(sf);
                            record_error("out of memory");
                            status = -9;
                        } else {
                            std::size_t nread = std::fread(buf, 1, static_cast<std::size_t>(sz), sf);
                            std::fclose(sf);
                            if (nread != static_cast<std::size_t>(sz)) {
                                std::free(buf);
                                record_error("STL read incomplete");
                                status = -8;
                            } else {
                                if (static_cast<unsigned long long>(sz) > UINT32_MAX) {
                                    std::free(buf);
                                    record_error("STL output exceeds the C ABI length limit");
                                    status = -8;
                                } else {
                                *out_stl = reinterpret_cast<uint8_t*>(buf);
                                *out_len = static_cast<uint32_t>(sz);
                                status = 0;
                                }
                            }
                        }
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        record_error(e.what());
        status = -9;
    }

    // Always clean up MEMFS temp files to avoid heap leaks in long-running sessions
    std::remove("/tmp/ow_in.obj");
    std::remove("/tmp/ow_out.stl");
    return status;
}

/**
 * Slice multiple STL files arranged on a single plate.
 *
 * all_stl      concatenation of all STL file bytes
 * offsets      uint32 pairs [start0, end0, start1, end1, …] — one per file
 * n_files      number of files (= offsets length / 2)
 * extruder_ids nullable int32 array of length n_files — 1-based "extruder"
 *              override per object (0 = inherit the config's default),
 *              forwarded to OrcaSlicer's per-object `extruder` config key
 *              (PrintConfig.cpp: coInt, min 0 = inherit; normalize_fdm()
 *              resolves it to the per-region *_filament_id fields). Names a
 *              *filament* slot, not a nozzle: whether two slots share one
 *              nozzle (AMS-style) or drive genuine T0/T1 tool changes is
 *              decided by `filament_map` in the config, which the host integration
 *              builds in withFilamentSlots() (host profile adapter). Real
 *              multi-nozzle profiles work here as of #160 — the crash this
 *              used to be gated against was our own array serialization, see
 *              json_array_to_config_string() above.
 *              Ignored (no-op) when null, so existing single-extruder callers
 *              are unaffected.
 * transforms   nullable float table of 11 values per file: scale xyz,
 *              rotation xyz (radians), mirror xyz, and X/Y offset in mm
 *              relative to bed centre. NaN X/Y delegates placement to arrange.
 *
 * Error codes: same convention as onewasm_slice_stl.
 */
EMSCRIPTEN_KEEPALIVE
onewasm_status_t onewasm_slice_stl_multi(
    onewasm_session_t session_ptr,
    const uint8_t* all_stl, uint32_t all_stl_len,
    const uint32_t* offsets, uint32_t n_files,
    const int32_t* extruder_ids,
    const float* transforms,
    uint8_t** out_gcode, uint32_t* out_len)
{
    OrcSession* session = as_session(session_ptr);
    if (!session) return -1;
    session->last_error.clear();
    if (out_gcode) *out_gcode = nullptr;
    if (out_len) *out_len = 0;
    if (!session->initialized) { record_error(*session, "call onewasm_init first"); return -1; }
    if (!all_stl || all_stl_len == 0 || !offsets || n_files == 0 || !out_gcode || !out_len)
        return -1;

    ActiveSliceGuard operation(*session);
    if (!operation) {
        record_error(*session, "session already has an active operation");
        return ONEWASM_ERR_INVALID_ARGUMENT;
    }

    const char* base = reinterpret_cast<const char*>(all_stl);

    try {
        throw_if_cancelled(operation);
        Slic3r::Model model;

        // ── load each STL segment into the shared model ───────────────────────
        for (uint32_t i = 0; i < n_files; i++) {
            const uint32_t start = offsets[i * 2];
            const uint32_t end   = offsets[i * 2 + 1];
            if (end <= start || end > all_stl_len) {
                record_error(*session, "invalid offset table");
                return -1;
            }
            const uint32_t len = end - start;
            const std::string path = "/tmp/ow_multi_" + std::to_string(i) + ".stl";
            {
                FILE* f = std::fopen(path.c_str(), "wb");
                if (!f) { record_error(*session, "cannot write temp STL"); return -3; }
                std::fwrite(base + start, 1, static_cast<std::size_t>(len), f);
                std::fclose(f);
            }
            const std::string name = "object_" + std::to_string(i);
            const bool ok = Slic3r::load_stl(path.c_str(), &model, name.c_str());
            std::remove(path.c_str());
            if (!ok) {
                record_error(*session, "STL load failed for file " + std::to_string(i));
                return -4;
            }
            throw_if_cancelled(operation);
        }

        if (model.objects.empty()) { record_error(*session, "no objects loaded"); return -5; }

        // The transform table is parallel to input files. A malformed or
        // unusual STL load must not make the transform loop read past that
        // table if one file expands into multiple model objects.
        if (transforms && model.objects.size() != static_cast<std::size_t>(n_files)) {
            record_error(*session, "current-plate transforms require one printable object per STL file");
            return -1;
        }

        // Per-object extruder override requires an exact 1:1 file→object
        // correspondence (true for the common case of one watertight solid
        // per STL). If any file expanded into more than one object, skip the
        // mapping entirely rather than guess a wrong association.
        const bool can_map_extruders =
            extruder_ids != nullptr && model.objects.size() == static_cast<std::size_t>(n_files);

        // ── centre each mesh; give each one an instance; optional extruder override ──
        if (!transforms) {
            // Keep the legacy path untouched when no transform table is sent.
            for (std::size_t i = 0; i < model.objects.size(); i++) {
                auto* obj = model.objects[i];
                center_object_xy_only(obj);
                if (obj->instances.empty())
                    obj->add_instance();
                if (can_map_extruders && extruder_ids[i] > 0) {
                    obj->config.set("extruder", extruder_ids[i]);
                }
            }
        } else {
            std::vector<bool> pinned(model.objects.size(), false);
            for (std::size_t i = 0; i < model.objects.size(); i++) {
                ObjectTransformInput transform;
                std::string transform_error;
                if (!read_object_transform(transforms, i, transform, transform_error)) {
                    record_error(*session, transform_error);
                    return -1;
                }
                pinned[i] = transform.has_offset;
                auto* obj = model.objects[i];
                center_object_xy_only(obj);
                add_transformed_instance(obj, *session, transform, false);
                obj->ensure_on_bed();
                if (can_map_extruders && extruder_ids[i] > 0)
                    obj->config.set("extruder", extruder_ids[i]);
            }
            arrange_transformed_model(model, *session, &pinned);
        }

        // ── auto-arrange on the bed ───────────────────────────────────────
        if (!transforms) {
            // coord_t uses 1 µm resolution: 1 mm = 1,000,000 units.
            // For circular beds the arrangement boundary is the largest axis-aligned
            // square inscribed in the circle (half-side = radius / √2) so objects are
            // never placed in the rectangle corners that fall outside the printable area.
            const double half_w = (session->bed_shape == "circle")
                ? session->bed_cx / std::sqrt(2.0)
                : session->bed_cx;
            const double half_h = (session->bed_shape == "circle")
                ? session->bed_cy / std::sqrt(2.0)
                : session->bed_cy;
            const Slic3r::BoundingBox bed(
                Slic3r::Point(
                    static_cast<coord_t>((session->bed_cx - half_w) * 1e6),
                    static_cast<coord_t>((session->bed_cy - half_h) * 1e6)
                ),
                Slic3r::Point(
                    static_cast<coord_t>((session->bed_cx + half_w) * 1e6),
                    static_cast<coord_t>((session->bed_cy + half_h) * 1e6)
                )
            );

            Slic3r::ArrangeParams params;
            params.min_obj_distance = static_cast<coord_t>(2.0 * 1e6); // 2 mm gap
            // See bridge/CMakeLists.txt's SLIC3R_WASM_MT
            // option — the only threading-aware line in the entire bridge.
            // Everything else runs in parallel automatically via real oneTBB
            // (built from source in CI) once that option is set; the sequential
            // build (default) is unaffected.
#ifdef SLIC3R_WASM_MT
            params.parallel         = true;
#else
            params.parallel         = false; // WASM is single-threaded
#endif

            // Objects that don't fit land at bed centre instead of throwing
            Slic3r::arrange_objects(model, bed, params,
                [session](Slic3r::arrangement::ArrangePolygon& ap) {
                    ap.translation = Slic3r::Vec2crd(
                        static_cast<coord_t>(session->bed_cx * 1e6),
                        static_cast<coord_t>(session->bed_cy * 1e6)
                    );
                });
        }

        // ── configure & slice ─────────────────────────────────────────────
        // Fit the prime tower onto the bed before applying the config — the
        // model is loaded and arranged now, so its height is known (see
        // clamp_wipe_tower_to_bed). bed_cx/cy are half-extents.
        clamp_wipe_tower_to_bed(session->config, model, 2.0 * session->bed_cx, 2.0 * session->bed_cy);
        Slic3r::Print print;
        operation.attach(print);
        ActivePrintGuard print_guard(operation);
        print.apply(model, session->config);
        zero_plate_origin(print);
        set_is_bbl_printer(print, session->config);
        // See onewasm_slice_stl: opt-in override of the mixed-nozzle-temperature guard
        // for single-nozzle multi-material plates (issue #164).
        if (session->remove_mixed_temp_restriction)
            print.set_check_multi_filaments_compatibility(false);
        // Variable (adaptive) layer height (issue #138); see onewasm_slice_stl. With a
        // multi-object plate the engine requires all objects share the same
        // layering when a prime tower is on (Print::validate), so an adaptive
        // multi-material plate with a tower surfaces that as a -6 validation
        // error rather than silently ignoring the setting.
        if (session->adaptive_layer_height)
            apply_adaptive_layer_height(print, session->adaptive_layer_height_quality);
        attach_progress_callback(print, *session);

        {
            // `warning` absorbs the non-fatal mixed-temperature notice when the
            // guard above is off; see onewasm_slice_stl for why the pointer is required.
            Slic3r::StringObjectException warning;
            Slic3r::StringObjectException err = print.validate(&warning);
            if (!err.string.empty()) { record_error(*session, err.string); return -6; }
        }

        try {
            throw_if_cancelled(operation);
            print.process();
            throw_if_cancelled(operation);
        } catch (const Slic3r::CanceledException&) {
            record_error(*session, "slice cancelled");
            return ONEWASM_ERR_CANCELLED;
        } catch (const Slic3r::SlicingError& e) {
            record_error(*session, e.what());
            return -7;
        }

        TempFileGuard out_guard("/tmp/ow_out.gcode");
        Slic3r::GCodeProcessorResult processor_result;
        {
            Slic3r::GCode gcode_gen;
            gcode_gen.do_export(&print, "/tmp/ow_out.gcode", &processor_result, nullptr);
        }
        throw_if_cancelled(operation);
        const std::string statistics_json = serialize_slice_statistics(print, processor_result);

        FILE* gf = std::fopen("/tmp/ow_out.gcode", "rb");
        if (!gf) { record_error(*session, "gcode export produced no output"); return -8; }

        std::fseek(gf, 0, SEEK_END);
        long sz = std::ftell(gf);
        std::rewind(gf);

        char* buf = static_cast<char*>(std::malloc(static_cast<std::size_t>(sz) + 1));
        if (!buf) {
            std::fclose(gf);
            record_error(*session, "out of memory");
            return -9;
        }
        std::fread(buf, 1, static_cast<std::size_t>(sz), gf);
        std::fclose(gf);
        buf[sz] = '\0';

        if (sz < 0 || static_cast<unsigned long long>(sz) > UINT32_MAX) {
            std::free(buf);
            record_error(*session, "G-code output exceeds the C ABI length limit");
            return ONEWASM_ERR_OUTPUT;
        }
        *out_gcode = reinterpret_cast<uint8_t*>(buf);
        *out_len   = static_cast<uint32_t>(sz);
        publish_slice_statistics(*session, statistics_json);
        return ONEWASM_OK;

    } catch (const Slic3r::CanceledException&) {
        record_error(*session, "slice cancelled");
        return ONEWASM_ERR_CANCELLED;
    } catch (const std::exception& e) {
        record_error(*session, e.what());
        return -9;
    }
}

/**
 * Convert a STEP file to binary STL using OrcaSlicer's OCCT reader.
 *
 * Only STEP is supported: OrcaSlicer's load_step() uses STEPCAFControl_Reader,
 * which does not read IGES.  (.iges/.igs are not routed here by the host integration.)
 *
 * Arguments:
 *   cad_data / cad_len  — raw STEP file bytes
 *   out_stl / out_len   — on success: malloc'd binary STL buffer + byte length
 *                         Caller must free with onewasm_free().
 *
 * Error codes (same conventions as onewasm_obj_to_stl):
 *   -1  invalid arguments
 *   -3  could not write STEP data to MEMFS
 *   -4  STEP load failed (bad file / unsupported feature)
 *   -5  file contains no printable geometry
 *   -8  STL export failed
 *   -9  unexpected C++ exception
 */
EMSCRIPTEN_KEEPALIVE
onewasm_status_t onewasm_cad_to_stl(
    const uint8_t* cad_data,
    uint32_t cad_len,
    uint8_t** out_stl,
    uint32_t* out_len
) {
    g_conversion_last_error.clear();
    if (out_stl) *out_stl = nullptr;
    if (out_len) *out_len = 0;
    if (!cad_data || cad_len == 0 || !out_stl || !out_len) return -1;

    const char* tmp_in  = "/tmp/ow_in.step";
    const char* tmp_out = "/tmp/ow_cad_out.stl";

    {
        FILE* f = std::fopen(tmp_in, "wb");
        if (!f) { record_error("cannot open CAD temp file for writing"); return -3; }
        std::size_t written = std::fwrite(cad_data, 1, static_cast<std::size_t>(cad_len), f);
        std::fclose(f);
        if (written != static_cast<std::size_t>(cad_len)) {
            std::remove(tmp_in);
            record_error("failed to write complete CAD data to MEMFS");
            return -3;
        }
    }

    int status = -9;
    try {
        Slic3r::Model model;
        try {
            // OrcaSlicer has no free load_step(); read_from_step is the real
            // entry point (it runs Step::load + Step::mesh under the hood and
            // throws Slic3r::RuntimeError on load/mesh failure).
            model = Slic3r::Model::read_from_step(
                tmp_in,
                Slic3r::LoadStrategy::AddDefaultInstances,
                nullptr,   // ImportStepProgressFn — progress callback
                nullptr,   // StepIsUtf8Fn — encoding probe
                nullptr,   // per-shape mesh callback
                0.003,     // linear deflection  (OrcaSlicer default)
                0.5,       // angular deflection (OrcaSlicer default)
                false);    // split compound
        } catch (const std::exception& e) {
            record_error(std::string("STEP load failed: ") + e.what());
            std::remove(tmp_in);
            std::remove(tmp_out);
            return -4;
        }

        if (model.objects.empty()) {
            record_error("STEP file contains no geometry");
            status = -5;
        } else {
            Slic3r::TriangleMesh combined;
            for (auto* obj : model.objects)
                for (auto* vol : obj->volumes)
                    combined.merge(vol->mesh());

            if (combined.facets_count() == 0) {
                record_error("CAD file contains no printable geometry");
                status = -5;
            } else if (!Slic3r::store_stl(tmp_out, &combined, true)) {
                record_error("STL export of CAD geometry failed");
                status = -8;
            } else {
                FILE* sf = std::fopen(tmp_out, "rb");
                if (!sf) {
                    record_error("STL export produced no output");
                    status = -8;
                } else {
                    std::fseek(sf, 0, SEEK_END);
                    long sz = std::ftell(sf);
                    std::rewind(sf);
                    if (sz <= 0) {
                        std::fclose(sf);
                        record_error("STL export produced empty output");
                        status = -8;
                    } else {
                        char* buf = static_cast<char*>(std::malloc(static_cast<std::size_t>(sz)));
                        if (!buf) {
                            std::fclose(sf);
                            record_error("out of memory");
                            status = -9;
                        } else {
                            std::size_t nread = std::fread(buf, 1, static_cast<std::size_t>(sz), sf);
                            std::fclose(sf);
                            if (nread != static_cast<std::size_t>(sz)) {
                                std::free(buf);
                                record_error("STL read incomplete");
                                status = -8;
                            } else {
                                if (static_cast<unsigned long long>(sz) > UINT32_MAX) {
                                    std::free(buf);
                                    record_error("STL output exceeds the C ABI length limit");
                                    status = -8;
                                } else {
                                *out_stl = reinterpret_cast<uint8_t*>(buf);
                                *out_len = static_cast<uint32_t>(sz);
                                status = 0;
                                }
                            }
                        }
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        record_error(e.what());
        status = -9;
    }

    std::remove(tmp_in);
    std::remove(tmp_out);
    return status;
}

/**
 * Export a single mesh + the session's current config as a .3mf file
 * (geometry + embedded OrcaSlicer settings — no plate/G-code/thumbnail data;
 * see the bridge design notes for why that's out of scope here).
 *
 * On success *out_3mf points to a malloc'd buffer containing the .3mf (a ZIP
 * archive — contains embedded NUL bytes, so callers must use the returned
 * length, never a NUL-terminated string read). Caller must free with
 * onewasm_free().
 *
 * Error codes: same convention as onewasm_slice_stl, with -8 meaning the 3MF export
 * itself (store_bbs_3mf) failed rather than gcode export.
 */
EMSCRIPTEN_KEEPALIVE
onewasm_status_t onewasm_write_3mf(
    onewasm_session_t session_ptr,
    const uint8_t* stl_data,
    uint32_t stl_len,
    uint8_t** out_3mf,
    uint32_t* out_len
) {
    OrcSession* session = as_session(session_ptr);
    if (!session) return -1;
    session->last_error.clear();
    if (out_3mf) *out_3mf = nullptr;
    if (out_len) *out_len = 0;
    if (!session->initialized) { record_error(*session, "call onewasm_init first"); return -1; }
    if (!stl_data || stl_len == 0 || !out_3mf || !out_len)
        return -1;

    try {
        // Guard covers the fwrite below too: if load_stl throws (rather than
        // just returning false), the temp file would otherwise never be
        // removed — a real per-failed-export MEMFS leak since MEMFS is
        // RAM-backed (same rationale as TempFileGuard's other use below).
        TempFileGuard in_guard("/tmp/ow_3mf_in.stl");
        {
            FILE* f = std::fopen("/tmp/ow_3mf_in.stl", "wb");
            if (!f) { record_error(*session, "cannot open /tmp/ow_3mf_in.stl for writing"); return -3; }
            std::size_t written = std::fwrite(stl_data, 1, static_cast<std::size_t>(stl_len), f);
            std::fclose(f);
            if (written != static_cast<std::size_t>(stl_len)) {
                record_error(*session, "failed to write complete STL data to MEMFS");
                return -3;
            }
        }

        Slic3r::Model model;
        const bool stl_ok = Slic3r::load_stl("/tmp/ow_3mf_in.stl", &model, "object");
        if (!stl_ok) {
            record_error(*session, "STL load failed");
            return -4;
        }
        if (model.objects.empty()) {
            record_error(*session, "model contains no objects");
            return -5;
        }

        // Same placement convention as onewasm_slice_stl, so the mesh lands back in
        // the same spot on re-import instead of at the model-space origin.
        for (auto* obj : model.objects) {
            center_object_xy_only(obj);
            if (obj->instances.empty()) {
                auto* inst = obj->add_instance();
                inst->set_offset(Slic3r::Vec3d(session->bed_cx, session->bed_cy, 0.0));
            }
        }

        TempFileGuard out_guard("/tmp/ow_out.3mf");
        {
            // Constructed before store_bbs_3mf() runs, so its destructor
            // (which does the get_backup_path()/"Auxiliaries" dir cleanup
            // that call lazily triggers) still fires even if store_bbs_3mf
            // throws instead of returning false.
            ModelBackupPathGuard backup_guard(model);

            Slic3r::StoreParams store_params;
            store_params.path = "/tmp/ow_out.3mf";
            store_params.model = &model;
            store_params.config = &session->config;
            // plate_data_list / project_presets / thumbnail_* all stay at
            // their StoreParams defaults (empty) — this headless bridge has
            // no PartPlateList, so there's no plate/gcode/thumbnail data to
            // attach. store_bbs_3mf treats all of that as optional and still
            // writes a valid model+config 3mf (verified by reading its
            // implementation: every plate/thumbnail loop is bounded by
            // plate_data_list.size(), which is 0 here).
            bool ok = Slic3r::store_bbs_3mf(store_params);
            if (!ok) {
                record_error(*session, "3MF export failed");
                return -8;
            }
        }

        long sz = 0;
        const char* err = nullptr;
        bool oom = false;
        char* buf = read_file_to_buffer("/tmp/ow_out.3mf", &sz, &err, &oom);
        if (!buf) {
            record_error(*session, std::string("3mf export ") + err);
            return oom ? -9 : -8;
        }

        if (sz < 0 || static_cast<unsigned long long>(sz) > UINT32_MAX) {
            std::free(buf);
            record_error(*session, "3MF output exceeds the C ABI length limit");
            return ONEWASM_ERR_OUTPUT;
        }
        *out_3mf = reinterpret_cast<uint8_t*>(buf);
        *out_len = static_cast<uint32_t>(sz);
        return ONEWASM_OK;

    } catch (const std::exception& e) {
        record_error(*session, e.what());
        return -9;
    }
}

/**
 * Read a .3mf file's printable geometry, using OrcaSlicer's
 * own reader (load_bbs_3mf) rather than the JS-side XML walker
 * (host format adapter) — so this understands whatever OrcaSlicer itself
 * wrote (multi-object assemblies, per-object transforms) exactly the way
 * OrcaSlicer does, instead of re-deriving the 3MF core spec's transform math
 * in JS. A pure format conversion like onewasm_obj_to_stl / onewasm_cad_to_stl — no
 * session/config state involved, so it takes none.
 *
 * On success *out_stl points to a malloc'd binary STL buffer — all objects'
 * meshes, each with its own instance/volume transforms already applied via
 * ModelObject::mesh() (so position/rotation/scale in the file survive),
 * merged into one. Byte length is returned in *out_stl_len. Native project
 * settings are intentionally not converted to a common JSON envelope; pass
 * the original bytes to onewasm_init_profile(session, "project.3mf", ...)
 * when the host wants to load them.
 *
 * Error codes: same convention as onewasm_obj_to_stl.
 *   -3  could not write 3MF bytes to MEMFS
 *   -4  3MF load failed (bad archive / no recognizable model)
 *   -5  3MF contains no printable geometry
 *   -8  STL export of the merged mesh failed
 *   -9  unexpected C++ exception
 */
EMSCRIPTEN_KEEPALIVE
onewasm_status_t onewasm_read_3mf(
    const uint8_t* mf_data,
    uint32_t mf_len,
    uint8_t** out_stl,
    uint32_t* out_stl_len
) {
    g_conversion_last_error.clear();
    if (out_stl) *out_stl = nullptr;
    if (out_stl_len) *out_stl_len = 0;
    if (!mf_data || mf_len == 0 || !out_stl || !out_stl_len)
        return -1;

    const char* tmp_in = "/tmp/ow_3mf_read_in.3mf";
    {
        FILE* f = std::fopen(tmp_in, "wb");
        if (!f) { record_error("cannot open temp file for writing"); return -3; }
        std::size_t written = std::fwrite(mf_data, 1, static_cast<std::size_t>(mf_len), f);
        std::fclose(f);
        if (written != static_cast<std::size_t>(mf_len)) {
            std::remove(tmp_in);
            record_error("failed to write complete 3MF data to MEMFS");
            return -3;
        }
    }

    try {
        TempFileGuard in_guard(tmp_in);

        Slic3r::Model model;
        // Constructed before load_bbs_3mf() runs, so its destructor (the
        // get_backup_path()/"Auxiliaries" dir cleanup that call lazily
        // triggers) still fires even if load_bbs_3mf throws instead of
        // returning false — same rationale as onewasm_write_3mf's backup_guard.
        ModelBackupPathGuard backup_guard(model);
        Slic3r::DynamicPrintConfig config;
        // EnableSilent: substitute unknown/incompatible option values with
        // defaults instead of throwing — mirrors onewasm_init's own "silently
        // skip unknown / incompatible keys" policy for the same reason (a
        // 3MF authored by a different OrcaSlicer/Bambu Studio version may
        // carry option values this pinned engine version doesn't recognize).
        Slic3r::ConfigSubstitutionContext substitutions(Slic3r::ForwardCompatibilitySubstitutionRule::EnableSilent);
        Slic3r::PlateDataPtrs plate_data_list;
        std::vector<Slic3r::Preset*> project_presets;
        Loaded3mfResourcesGuard loaded_resources(plate_data_list, project_presets);
        Slic3r::Semver file_version;

        bool ok = Slic3r::load_bbs_3mf(
            tmp_in, &config, &substitutions, &model,
            &plate_data_list, &project_presets,
            nullptr, nullptr, &file_version, nullptr,
            Slic3r::LoadStrategy::AddDefaultInstances | Slic3r::LoadStrategy::LoadModel | Slic3r::LoadStrategy::LoadConfig);

        if (!ok) {
            record_error("3MF load failed");
            return -4;
        }
        if (model.objects.empty()) {
            record_error("3MF contains no geometry");
            return -5;
        }

        // ModelObject::mesh() bakes in every instance's + volume's transform
        // (position/rotation/scale) — unlike onewasm_obj_to_stl/onewasm_cad_to_stl's
        // raw vol->mesh() merge, which is fine for OBJ/STEP (no separate
        // instance concept there) but would silently drop a 3MF's actual
        // placement if used here.
        Slic3r::TriangleMesh combined;
        for (auto* obj : model.objects) {
            if (!obj) continue;
            combined.merge(obj->mesh());
        }
        if (combined.facets_count() == 0) {
            record_error("3MF contains no printable geometry");
            return -5;
        }

        const char* tmp_out = "/tmp/ow_3mf_read_out.stl";
        TempFileGuard out_guard(tmp_out);
        if (!Slic3r::store_stl(tmp_out, &combined, true)) {
            record_error("STL export of 3MF geometry failed");
            return -8;
        }

        long stl_sz = 0;
        const char* stl_err = nullptr;
        bool stl_oom = false;
        char* stl_buf = read_file_to_buffer(tmp_out, &stl_sz, &stl_err, &stl_oom);
        if (!stl_buf) {
            record_error(std::string("STL export ") + stl_err);
            return stl_oom ? -9 : -8;
        }
        std::unique_ptr<char, decltype(&std::free)> stl_owner(stl_buf, &std::free);
        int stl_len = static_cast<int>(stl_sz);

        if (static_cast<unsigned long long>(stl_len) > UINT32_MAX) {
            record_error("STL output exceeds the C ABI length limit");
            return -8;
        }
        *out_stl = reinterpret_cast<uint8_t*>(stl_owner.release());
        *out_stl_len = static_cast<uint32_t>(stl_len);
        return ONEWASM_OK;

    } catch (const std::exception& e) {
        record_error(e.what());
        return -9;
    }
}

/** Free a buffer returned by onewasm_slice_stl, onewasm_slice_stl_multi, onewasm_obj_to_stl, onewasm_cad_to_stl, onewasm_write_3mf, or onewasm_read_3mf. */
EMSCRIPTEN_KEEPALIVE
void onewasm_free(void* ptr) {
    std::free(ptr);
}

/**
 * Return the last error message as a null-terminated string.
 * The pointer is valid until the next onewasm_* call on the same session (or,
 * for a null session, the next onewasm_obj_to_stl / onewasm_cad_to_stl call).
 *
 * Pass the session used for the failing onewasm_init / onewasm_slice_stl / onewasm_slice_stl_multi
 * call. Pass 0/null after a failing onewasm_obj_to_stl / onewasm_cad_to_stl call
 * (those take no session) — this is also why the parameter used to be
 * documented as "unused" and JS always passed literal 0: that call pattern
 * still works unchanged for conversion errors.
 */
EMSCRIPTEN_KEEPALIVE
const char* onewasm_last_error(onewasm_session_t session_ptr) {
    OrcSession* session = as_session(session_ptr);
    return session ? session->last_error.c_str() : g_conversion_last_error.c_str();
}

} // extern "C"
