#!/usr/bin/env bash
set -euo pipefail

EXPECTED_ARCHIVE_SHA256="f81d50d17bd337c059016e57e77e21be3827b20027ab82dc2b69242d65062477"
EXPECTED_DATA_SHA256="200dc6a8f5b1e0de8d76d531001bcb886f7c2cc74859a30f3d324f6f5bc782e3"

usage() {
    echo "Usage: $0 /path/to/thWWW_1.0.1.zip /path/to/sd-root" >&2
    exit 2
}

[[ $# -eq 2 ]] || usage
ARCHIVE="$(realpath "$1")"
SD_ROOT="$(realpath -m "$2")"

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
SOURCE_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
PROJECT_ROOT="${THWWW_SWITCH_ROOT:-$SOURCE_DIR}"
NRO="${THWWW_NRO:-$PROJECT_ROOT/release/thwww.nro}"
TARGET="$SD_ROOT/switch/thwww"

[[ -f "$ARCHIVE" ]] || { echo "error: archive not found: $ARCHIVE" >&2; exit 1; }
[[ -f "$NRO" ]] || { echo "error: NRO not found: $NRO (run build-thwww-switch.sh first)" >&2; exit 1; }
command -v unzip >/dev/null || { echo "error: unzip is required" >&2; exit 1; }

actual_archive_sha="$(sha256sum "$ARCHIVE" | awk '{print $1}')"
if [[ "$actual_archive_sha" != "$EXPECTED_ARCHIVE_SHA256" ]]; then
    echo "error: archive hash does not match the official thWWW 1.0.1 release" >&2
    echo " expected: $EXPECTED_ARCHIVE_SHA256" >&2
    echo " actual:   $actual_archive_sha" >&2
    exit 1
fi

mkdir -p -- "$TARGET" "$TARGET/save"
# The free official archive is supplied by the user. The Windows executable is
# unnecessary on Switch and is deliberately excluded from the SD installation.
unzip -q -o "$ARCHIVE" -x 'thWWW.exe' -d "$TARGET"
install -m 0644 "$NRO" "$TARGET/thwww.nro"
mkdir -p -- "$TARGET/save"

actual_data_sha="$(sha256sum "$TARGET/data.win" | awk '{print $1}')"
if [[ "$actual_data_sha" != "$EXPECTED_DATA_SHA256" ]]; then
    echo "error: extracted data.win failed its integrity check" >&2
    exit 1
fi

for required in data.win options.ini font music text thwww.nro; do
    [[ -e "$TARGET/$required" ]] || { echo "error: missing installed item: $required" >&2; exit 1; }
done

printf 'Installed thWWW-switch to: %s\n' "$TARGET"
printf 'NRO SHA-256: %s\n' "$(sha256sum "$TARGET/thwww.nro" | awk '{print $1}')"
printf 'Game data SHA-256: %s\n' "$actual_data_sha"
printf '\nLaunch through hbmenu title takeover/full-memory mode.\n'
