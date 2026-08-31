#!/bin/sh
set -eu

: "${ARTIFACT:?ARTIFACT is required}"
: "${KP_DIR:?KP_DIR is required}"

TARGET_COMPILE=${TARGET_COMPILE:-aarch64-linux-gnu-}
PYTHON=${PYTHON:-python3}
EXPECTED_KP_COMMIT=0fe6d142266b80e5aa445a7ea1534f88a8f33a35
MAX_ARTIFACT_BYTES=2097152

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
case "$ARTIFACT" in
    /*) ;;
    *) ARTIFACT=$PWD/$ARTIFACT ;;
esac
case "$KP_DIR" in
    /*) ;;
    *) KP_DIR=$PWD/$KP_DIR ;;
esac

NM=${TARGET_COMPILE}nm
READELF=${TARGET_COMPILE}readelf
OBJDUMP=${TARGET_COMPILE}objdump
PATCH=$PROJECT_ROOT/kpm/patches/kpatch-next-d05-safe-kpm-unload.patch

for tool in "$PYTHON" "$NM" "$READELF" "$OBJDUMP" git grep wc sha256sum mktemp awk; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "missing required tool: $tool" >&2
        exit 1
    }
done
test -f "$ARTIFACT" || {
    echo "KPM artifact does not exist: $ARTIFACT" >&2
    exit 1
}
test -d "$KP_DIR/.git" || {
    echo "KP_DIR is not a git checkout: $KP_DIR" >&2
    exit 1
}

actual_commit=$(git -C "$KP_DIR" rev-parse HEAD)
test "$actual_commit" = "$EXPECTED_KP_COMMIT" || {
    echo "KPatch commit mismatch: $actual_commit" >&2
    exit 1
}
git -C "$KP_DIR" apply --check --reverse "$PATCH" >/dev/null || {
    echo "required safe-unload patch is not applied exactly" >&2
    exit 1
}

artifact_bytes=$(wc -c < "$ARTIFACT")
test "$artifact_bytes" -le "$MAX_ARTIFACT_BYTES" || {
    echo "KPM artifact is unexpectedly large: $artifact_bytes bytes" >&2
    exit 1
}

"$PYTHON" "$PROJECT_ROOT/tools/gen_kpm_profiles.py" --check
"$PYTHON" "$PROJECT_ROOT/tools/check_kpm_elf.py" \
    --elf "$ARTIFACT" --kpatch-dir "$KP_DIR" \
    --nm "$NM" --readelf "$READELF"

temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/stackplz-kpm-verify.XXXXXX")
trap 'rm -rf "$temporary_dir"' EXIT HUP INT TERM
"$OBJDUMP" -d "$ARTIFACT" > "$temporary_dir/disassembly.txt"
"$NM" -u "$ARTIFACT" > "$temporary_dir/imports.txt"

for register_family in dbgbvr dbgbcr dbgwvr dbgwcr mdscr_el1; do
    grep -Eiq "$register_family" "$temporary_dir/disassembly.txt" || {
        echo "direct debug-register instruction missing: $register_family" >&2
        exit 1
    }
done

if grep -Eiq 'perf_event_create_kernel_counter|register_(user_)?hw_breakpoint|perf_event_open' "$temporary_dir/imports.txt"; then
    echo "forbidden perf breakpoint registration import found" >&2
    exit 1
fi

artifact_sha256=$(sha256sum "$ARTIFACT" | awk '{print $1}')
printf '%s\n' \
    "KPM_ARTIFACT_VERIFIED=1" \
    "artifact=$ARTIFACT" \
    "bytes=$artifact_bytes" \
    "sha256=$artifact_sha256" \
    "kpatch_commit=$actual_commit" \
    "direct_register_families=dbgbvr,dbgbcr,dbgwvr,dbgwcr,mdscr_el1"
