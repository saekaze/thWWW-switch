#!/usr/bin/env bash
set -euo pipefail

# Deterministic end-to-end regressions for thWWW's pooled danmaku lifecycle,
# precise player collision, and dense graze workload. They require an
# already-built CLI/noop runner and an extracted, legally obtained thWWW 1.0.1
# directory. No proprietary game data is copied.
#
# Usage: scripts/test-thwww-regressions.sh /path/to/butterscotch /path/to/thWWW

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 /path/to/butterscotch /path/to/thWWW" >&2
    exit 2
fi

runner=$1
game_root=$2
data_win="$game_root/data.win"
[[ -x "$runner" ]] || { echo "Runner is not executable: $runner" >&2; exit 2; }
[[ -f "$data_win" ]] || { echo "Missing official data.win: $data_win" >&2; exit 2; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Scenario 1: restore pooled stage-1 bullets at unchanged coordinates, then
# deliberately cross them. Missing active-state/grid synchronization makes the
# player incorrectly invulnerable and leaves miss at zero.
mkdir -p "$tmp/collision-save"
cat > "$tmp/collision-save/Data.ini" <<'INI'
[option]
language="1"
sfx="7"
bgm="9"
INI

cat > "$tmp/suicide.json" <<'JSON'
{
  "360": {"keysPressed": [90], "keysReleased": []},
  "361": {"keysPressed": [], "keysReleased": [90]},
  "420": {"keysPressed": [90], "keysReleased": []},
  "421": {"keysPressed": [], "keysReleased": [90]},
  "480": {"keysPressed": [90], "keysReleased": []},
  "481": {"keysPressed": [], "keysReleased": [90]},
  "900": {"keysPressed": [38], "keysReleased": []},
  "5000": {"keysPressed": [], "keysReleased": [38]},
  "6000": {"keysPressed": [], "keysReleased": []}
}
JSON

"$runner" "$data_win" \
    --save-folder "$tmp/collision-save" \
    --renderer noop \
    --headless \
    --playback-inputs "$tmp/suicide.json" \
    --seed 12648430 \
    --exit-at-frame 6000 \
    --dump-frame-json 5990 \
    --dump-frame-json-file "$tmp/collision-frame.json" \
    --disable-log-colors > "$tmp/collision-runner.log" 2>&1

python3 - "$tmp/collision-frame.json" <<'PY'
import json
import sys

state = json.load(open(sys.argv[1], encoding="utf-8"))
g = state["globalVariables"]
assert state["room"]["name"] == "room_gp", state["room"]
assert g["stage"] == 1, g["stage"]
# Starting life is 2. Holding at the top of the playfield must intersect the
# precise player hurtbox with ordinary danmaku restored from the inactive pool.
assert g["miss"] >= 1, f"no player miss registered: miss={g['miss']} life={g['life']}"
assert g["life"] < 2, f"life did not decrease: {g['life']}"
print(f"PASS: pooled precise danmaku collision registered (miss={g['miss']}, life={g['life']})")
PY

# Scenario 2: unlock practice selection, start stage 5, and focus-move upward
# through its first dense wave. At the sampled frame there are hundreds of
# active bullets/graze boxes and dozens of successful precise grazes without a
# miss. This exercises the spatial-grid hot path without weakening collision.
mkdir -p "$tmp/graze-save"
cat > "$tmp/graze-save/Data.ini" <<'INI'
[option]
language="1"
sfx="7"
bgm="9"
[data]
stage_unlock="7"
INI

cat > "$tmp/dense-graze.json" <<'JSON'
{
  "360": {"keysPressed": [40], "keysReleased": []},
  "361": {"keysPressed": [], "keysReleased": [40]},
  "380": {"keysPressed": [40], "keysReleased": []},
  "381": {"keysPressed": [], "keysReleased": [40]},
  "420": {"keysPressed": [90], "keysReleased": []},
  "421": {"keysPressed": [], "keysReleased": [90]},
  "450": {"keysPressed": [90], "keysReleased": []},
  "451": {"keysPressed": [], "keysReleased": [90]},
  "480": {"keysPressed": [90], "keysReleased": []},
  "481": {"keysPressed": [], "keysReleased": [90]},
  "510": {"keysPressed": [40], "keysReleased": []},
  "511": {"keysPressed": [], "keysReleased": [40]},
  "530": {"keysPressed": [40], "keysReleased": []},
  "531": {"keysPressed": [], "keysReleased": [40]},
  "550": {"keysPressed": [40], "keysReleased": []},
  "551": {"keysPressed": [], "keysReleased": [40]},
  "570": {"keysPressed": [40], "keysReleased": []},
  "571": {"keysPressed": [], "keysReleased": [40]},
  "600": {"keysPressed": [90], "keysReleased": []},
  "601": {"keysPressed": [], "keysReleased": [90]},
  "650": {"keysPressed": [16, 38], "keysReleased": []},
  "849": {"keysPressed": [], "keysReleased": [16, 38]},
  "851": {"keysPressed": [], "keysReleased": []}
}
JSON

"$runner" "$data_win" \
    --save-folder "$tmp/graze-save" \
    --renderer noop \
    --headless \
    --playback-inputs "$tmp/dense-graze.json" \
    --seed 12648430 \
    --exit-at-frame 851 \
    --dump-frame-json 850 \
    --dump-frame-json-file "$tmp/graze-frame.json" \
    --disable-log-colors > "$tmp/graze-runner.log" 2>&1

python3 - "$tmp/graze-frame.json" <<'PY'
from collections import Counter
import json
import sys

state = json.load(open(sys.argv[1], encoding="utf-8"))
g = state["globalVariables"]
counts = Counter(
    instance["objectName"]
    for instance in state["instances"]
    if instance["active"]
)
danmakus = sum(count for name, count in counts.items() if name.startswith("obj_danmaku"))
graze_boxes = counts["obj_grazebox"]

assert state["room"]["name"] == "room_gp", state["room"]
assert g["stage"] == 5, g["stage"]
assert g["miss"] == 0 and g["life"] == 7, (g["miss"], g["life"])
assert g["graze"] >= 40, f"dense path did not register enough grazes: {g['graze']}"
assert danmakus >= 300, f"dense wave unexpectedly sparse: {danmakus} danmaku"
assert graze_boxes >= 250, f"graze workload unexpectedly sparse: {graze_boxes} boxes"
print(
    "PASS: dense precise-graze workload preserved "
    f"(graze={g['graze']}, danmaku={danmakus}, graze_boxes={graze_boxes}, miss={g['miss']})"
)
PY
