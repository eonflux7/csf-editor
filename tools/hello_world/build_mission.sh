#!/usr/bin/env bash
# Builds the hello-world mission archive in Convoy's slot (decision D1):
# the map from build_world.sh plus Convoy's scene cut down by scene_ops.py.
#
#   tools/hello_world/build_mission.sh WORKSPACE ORIGINAL_CONVOY_PAK OUT_PAK [CORPUS_ROOT]
#
# WORKSPACE is created (it must not exist). Deploy the archive with
# `csf-mod deploy-pak WORKSPACE OUT_PAK <test-install> maps/Convoy.pak --apply`.
set -euo pipefail
ws=${1:?usage}; original=${2:?usage}; out=${3:?usage}; corpus=${4:-../CSF_unpacks}
csfmod=./build/Release/csf-mod
scene="$corpus/Convoy/Maps/FR03/Convoy.scn"
[ ! -e "$ws" ] || { echo "workspace exists: $ws" >&2; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
tools/hello_world/build_world.sh "$tmp/world" "$corpus"
"$csfmod" decompile "$scene" "$tmp/convoy.scn.txt" >/dev/null
scripts=$(./build/Release/csf-info program "$corpus/Convoy/Maps/FR03/Convoy.gsc" --scripts |
          sed -n 's/^SCRIPT\t.*\tid=\([0-9]*\)\t.*/\1/p' | paste -sd,)
actors=$(./build/Release/csf-info mission "$scene" --objects |
         sed -n 's/^ACTOR\t.*\tid=\([0-9]*\)\t.*/\1/p' | paste -sd,)
# shellcheck disable=SC2046
"$csfmod" mission-edit "$ws" "$scene" $(python3 tools/hello_world/scene_ops.py "$tmp/convoy.scn.txt" "$scripts" "$actors") |
    grep -v '^applied' || true
"$csfmod" add "$ws" Maps/FR03/FR03.rws "$tmp/world/FR03.rws"
"$csfmod" add "$ws" Maps/FR03/FR03_col.rws "$tmp/world/FR03_col.rws"
"$csfmod" validate "$ws"
"$csfmod" export-mission "$ws" "$original" "$out" --overwrite
