#!/usr/bin/env bash
# Builds hello world through the editor's operations (ops.py, `csf-mod
# mission-ops`) and checks every mission file against the scene.py build in
# PROJECT_DIR (tools/hello_world/build.sh output): byte for byte, or with the
# decompiled difference when they differ.
#
#   tools/hello_world/parity.sh PROJECT_DIR [CORPUS_ROOT]
set -euo pipefail
project=${1:?usage: parity.sh PROJECT_DIR [CORPUS_ROOT]}; corpus=${2:-../CSF_unpacks}
csfmod=./build/Release/csf-mod
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
python3 tools/hello_world/ops.py "$project/sources/world/terrain.csfworld" "$corpus" "$tmp/ops" >/dev/null
"$csfmod" mission-ops "$tmp/ws" "$corpus/Convoy/Maps/FR03/Convoy.scn" "$tmp/ops/hello.ops" > "$tmp/ops.log" || {
    grep -v '^applied' "$tmp/ops.log" >&2; exit 1;
}
status=0
for file in $(cd "$project/mission/authored" && find . -type f ! -name '*.json' | sort); do
    ours="$tmp/ws/authored/$file" theirs="$project/mission/authored/$file"
    if [ ! -f "$ours" ]; then echo "missing $file"; status=1; continue; fi
    if cmp -s "$ours" "$theirs"; then echo "same    $file"; continue; fi
    status=1
    echo "DIFF    $file"
    if "$csfmod" decompile "$theirs" "$tmp/theirs.txt" >/dev/null 2>&1 &&
       "$csfmod" decompile "$ours" "$tmp/ours.txt" >/dev/null 2>&1; then
        diff "$tmp/theirs.txt" "$tmp/ours.txt" | head -${PARITY_DIFF_LINES:-40} || true
    fi
    rm -f "$tmp/theirs.txt" "$tmp/ours.txt"
done
for file in $(cd "$tmp/ws/authored" && find . -type f ! -name '*.json' | sort); do
    [ -f "$project/mission/authored/$file" ] || { echo "extra   $file"; status=1; }
done
exit $status
