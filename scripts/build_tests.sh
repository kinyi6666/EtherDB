#!/usr/bin/env bash
# ============================================================
#  EtherDB test build script (Linux / g++)
#
#  Builds the standalone integration / performance tests in tests/ that
#  link against the prebuilt client SDK (src/bin/sdk). No server sources
#  are required.
#
#  Build the SDK first if src/bin/sdk/lib/libetherdb_client.a is missing:
#      scripts/build_sdk.sh
#
#  Output:
#      tests/bin_linux/<name>
#
#  Usage:
#    scripts/build_tests.sh                  build the default set
#    scripts/build_tests.sh full_test        build only named test(s)
#    scripts/build_tests.sh --clean          wipe tests/bin_linux first
#    scripts/build_tests.sh --list           show the default set
#
#  Notes:
#    - Sources contain UTF-8 (CJK / box-drawing). g++ defaults to UTF-8
#      input, so no extra flag is needed; the terminal must be UTF-8 to read
#      the output correctly.
# ============================================================

set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")/.."

OUTDIR="tests/bin_linux"
CLEAN=0
LISTONLY=0
TARGETS=()

usage() {
    cat <<EOF
EtherDB test build script
  scripts/build_tests.sh [--clean] [--list] [--help] [test names...]
Output goes to ${OUTDIR}/
EOF
}

# ---- parse args ----
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean) CLEAN=1; shift ;;
        --list)  LISTONLY=1; shift ;;
        --help|-h) usage; exit 0 ;;
        --*) echo "[EtherDB] ERROR: unknown option: $1" >&2; usage; exit 1 ;;
        *) TARGETS+=("$1"); shift ;;
    esac
done

# ---- default set (the ones referenced by the perf write-up) ----
if [[ ${#TARGETS[@]} -eq 0 ]]; then
    TARGETS=(perf_insert_test perf_insert_test_string streaming_perf_test query_perf_test full_test)
fi

if [[ $LISTONLY -eq 1 ]]; then
    echo "Default test set:"
    for t in "${TARGETS[@]}"; do echo "  $t"; done
    exit 0
fi

# ---- SDK must exist ----
SDK_LIB=""
for cand in \
    "src/bin/sdk/lib/libetherdb_client.a" \
    "src/bin/sdk/lib/libetherdb_client.so" \
    "src/bin/sdk/lib/etherdb_client.lib"
do
    if [[ -e "$cand" ]]; then SDK_LIB="$cand"; break; fi
done

if [[ -z "$SDK_LIB" ]]; then
    echo "[EtherDB] ERROR: client SDK not found under src/bin/sdk/lib/" >&2
    echo "[EtherDB]        Run:  scripts/build_sdk.sh" >&2
    exit 1
fi

# ---- prepare output dir ----
if [[ $CLEAN -eq 1 && -d "$OUTDIR" ]]; then
    rm -rf "$OUTDIR"
fi
mkdir -p "$OUTDIR"

echo "[EtherDB] ============================================="
echo "[EtherDB] Building tests   (g++ / -g -O2)"
echo "[EtherDB] Output: %s" "$OUTDIR/"
echo "[EtherDB] SDK lib: $SDK_LIB"
echo "[EtherDB] ============================================="

CXXFLAGS=(-std=c++17 -g -O2 -Wall -Wextra)
INCLUDES=(-I src/bin/sdk/include -I src)
LDFLAGS=(-pthread)
LIBS=("$SDK_LIB")

FAILED=()
BUILT=0

for t in "${TARGETS[@]}"; do
    src="tests/${t}.cpp"
    if [[ ! -f "$src" ]]; then
        echo "[EtherDB] SKIP  $t  (tests/${t}.cpp not found)"
        FAILED+=("$t")
        continue
    fi

    echo "[EtherDB] CXX   $t"
    if g++ "${CXXFLAGS[@]}" "${INCLUDES[@]}" \
        "$src" -o "$OUTDIR/$t" \
        "${LDFLAGS[@]}" "${LIBS[@]}"
    then
        BUILT=$((BUILT + 1))
    else
        FAILED+=("$t")
    fi
done

echo
echo "[EtherDB] ============================================="
if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo "[EtherDB] FAILED: ${FAILED[*]}"
    echo "[EtherDB] Built $BUILT target(s), some failed."
    exit 1
fi

echo "[EtherDB] OK - $BUILT test(s) built into $OUTDIR/"
echo "[EtherDB] ============================================="
echo "[EtherDB] To run them, start the server first:"
echo "[EtherDB]   src/bin/Release/etherdb_dserver -p 7040"
echo "[EtherDB] Then, e.g.:"
echo "[EtherDB]   $OUTDIR/full_test 7040"
echo "[EtherDB]   $OUTDIR/perf_insert_test 7040 100 perf_data_1"
echo "[EtherDB]   $OUTDIR/streaming_perf_test 7040 perf_data_1 perftest20"
echo "[EtherDB]   $OUTDIR/query_perf_test 7040"
echo "[EtherDB] Note: query_perf_test / streaming_perf_test default to table \"perf_data_1\","
echo "[EtherDB]       which only exists after: perf_insert_test <port> <rounds> perf_data_1"

exit 0