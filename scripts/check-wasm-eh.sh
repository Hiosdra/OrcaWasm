#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${EMSDK:-}" || ! -f "$EMSDK/emsdk_env.sh" ]]; then
    echo "Set EMSDK to an installed Emscripten SDK before running this check." >&2
    exit 2
fi

tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/orca-wasm-eh.XXXXXX")"
cleanup() {
    rm -rf "$tmp_root"
}
trap cleanup EXIT

provided_em_cache="${EM_CACHE:-}"
# Emscripten's default cache may live under a read-only SDK installation.
source "$EMSDK/emsdk_env.sh" >/dev/null
if [[ -n "$provided_em_cache" ]]; then
    export EM_CACHE="$provided_em_cache"
else
    export EM_CACHE="$tmp_root/emscripten-cache"
fi

cat > "$tmp_root/longjmp.c" <<'EOF'
#include <setjmp.h>

int check_longjmp(void) {
    jmp_buf environment;
    int value = setjmp(environment);
    if (value == 0) longjmp(environment, 7);
    return value;
}
EOF

cat > "$tmp_root/exceptions.cpp" <<'EOF'
#include <cstdio>
#include <stdexcept>

#if PROBE_PTHREADS
#include <atomic>
#include <thread>
#endif

extern "C" int check_longjmp(void);

int main() {
    int longjmp_value = check_longjmp();
    int exception_caught = 0;
    try {
        throw std::runtime_error("native WebAssembly exception");
    } catch (const std::runtime_error&) {
        exception_caught = 1;
    }

    if (!exception_caught || longjmp_value != 7) return 2;

#if PROBE_PTHREADS
    std::atomic<int> worker_result{0};
    std::thread worker([&worker_result] { worker_result.store(1, std::memory_order_release); });
    worker.join();
    if (worker_result.load(std::memory_order_acquire) != 1) return 3;
#endif

    std::printf("PASS pointer_bytes=%zu longjmp=%d caught=%d pthreads=%d\n",
                sizeof(void*), longjmp_value, exception_caught, PROBE_PTHREADS);
    return 0;
}
EOF

for variant in st mt wasm64 wasm64-mt; do
    variant_dir="$tmp_root/$variant"
    mkdir -p "$variant_dir"
    thread_flags=()
    pthread_value=0
    memory_flags=()
    link_flags=(
        -fwasm-exceptions
        -sSUPPORT_LONGJMP=wasm
        -sEXIT_RUNTIME=1
        -sENVIRONMENT=node,worker
    )
    if [[ "$variant" == "mt" || "$variant" == "wasm64-mt" ]]; then
        thread_flags=(-pthread)
        pthread_value=1
        link_flags+=(
            -sUSE_PTHREADS=1
            -sPTHREAD_POOL_SIZE=1
            -sPTHREAD_POOL_SIZE_STRICT=2
        )
    fi
    if [[ "$variant" == "wasm64" || "$variant" == "wasm64-mt" ]]; then
        memory_flags=(-sMEMORY64=1)
    fi

    emcc -O2 "${thread_flags[@]}" "${memory_flags[@]}" -sSUPPORT_LONGJMP=wasm \
        -c "$tmp_root/longjmp.c" -o "$variant_dir/longjmp.o"
    em++ -O2 "${thread_flags[@]}" "${memory_flags[@]}" -fwasm-exceptions \
        -DPROBE_PTHREADS="$pthread_value" \
        -c "$tmp_root/exceptions.cpp" -o "$variant_dir/exceptions.o"
    em++ -O2 "${thread_flags[@]}" "${memory_flags[@]}" "${link_flags[@]}" \
        "$variant_dir/longjmp.o" "$variant_dir/exceptions.o" \
        -o "$variant_dir/probe.js"
    node "$variant_dir/probe.js"
    echo "WASM bytes ($variant): $(wc -c < "$variant_dir/probe.wasm" | tr -d ' ')"
done
