#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
SOURCE_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
PROJECT_ROOT="${THWWW_SWITCH_ROOT:-$SOURCE_DIR}"
BUILD_DIR="${THWWW_BUILD_DIR:-${TMPDIR:-/tmp}/thwww-switch-build}"
RELEASE_DIR="${THWWW_RELEASE_DIR:-$PROJECT_ROOT/release}"

if [[ -z "${DEVKITPRO:-}" && -d /opt/devkitpro ]]; then
    export DEVKITPRO=/opt/devkitpro
fi
: "${DEVKITPRO:?Set DEVKITPRO to a devkitPro installation containing devkitA64 and Switch portlibs}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"

CMAKE_SWITCH="$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-cmake"
if [[ ! -x "$CMAKE_SWITCH" ]]; then
    echo "error: Switch CMake wrapper not found: $CMAKE_SWITCH" >&2
    exit 1
fi
if [[ ! -x "$DEVKITA64/bin/aarch64-none-elf-gcc" ]]; then
    echo "error: devkitA64 compiler not found under: $DEVKITA64" >&2
    exit 1
fi

if git -C "$SOURCE_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    COMMIT_HASH="$(git -C "$SOURCE_DIR" rev-parse --verify HEAD)"
    COMMIT_DATE="$(git -C "$SOURCE_DIR" show -s --format=%cI HEAD)"
else
    COMMIT_HASH="source-archive"
    COMMIT_DATE="unknown"
fi

rm -rf -- "$BUILD_DIR" "$RELEASE_DIR"
mkdir -p -- "$BUILD_DIR" "$RELEASE_DIR/docs"

"$CMAKE_SWITCH" -S "$SOURCE_DIR" -B "$BUILD_DIR" -G Ninja \
    -DPLATFORM=switch \
    -DCMAKE_BUILD_TYPE=Release \
    -DTHWWW_NORMALIZE_BUILD_PATHS=ON \
    -DENABLE_ASAN=OFF \
    -DENABLE_MODERN_GL=ON \
    -DENABLE_LEGACY_GL=OFF \
    -DENABLE_NOOP_RENDERER=OFF \
    -DENABLE_WAD14=OFF \
    -DENABLE_WAD16=OFF \
    -DENABLE_WAD17=ON \
    -DWERROR=ON \
    -DBUTTERSCOTCH_COMMIT_HASH="$COMMIT_HASH" \
    -DBUTTERSCOTCH_COMMIT_DATE="$COMMIT_DATE"

cmake --build "$BUILD_DIR" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"

for artifact in thwww.nro thwww.nacp butterscotch.elf; do
    if [[ ! -f "$BUILD_DIR/$artifact" ]]; then
        echo "error: expected build artifact is missing: $BUILD_DIR/$artifact" >&2
        exit 1
    fi
done

install -m 0644 "$BUILD_DIR/thwww.nro" "$RELEASE_DIR/thwww.nro"
install -m 0644 "$BUILD_DIR/thwww.nacp" "$RELEASE_DIR/thwww.nacp"
install -m 0644 "$BUILD_DIR/butterscotch.elf" "$RELEASE_DIR/thwww.elf"
install -m 0644 "$SOURCE_DIR/README.md" "$RELEASE_DIR/README.md"
install -m 0644 "$SOURCE_DIR/docs/"*.md "$RELEASE_DIR/docs/"
install -m 0644 "$SOURCE_DIR/LICENSE" "$RELEASE_DIR/LICENSE"
printf '%s\n' \
    "Project: thWWW-switch" \
    "Source commit: $COMMIT_HASH" \
    "Upstream runner: https://github.com/ButterscotchRunner/Butterscotch" \
    > "$RELEASE_DIR/SOURCE-COMMIT.txt"

(
    cd "$RELEASE_DIR"
    sha256sum thwww.nro thwww.nacp thwww.elf > SHA256SUMS
)

printf '\nBuilt thWWW-switch from %s\n' "$COMMIT_HASH"
printf 'Artifacts: %s\n' "$RELEASE_DIR"
cat "$RELEASE_DIR/SHA256SUMS"
