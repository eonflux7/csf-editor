#!/usr/bin/env bash
# Builds the hello-world map (docs/plans/hello-world-mission.md) into OUT_DIR:
# Blender terrain (tools/hello_world/terrain.py, with Convoy's house, trees and
# plants) -> .csfworld -> FR03.rws and FR03_col.rws, replacing Convoy's map
# files in the Convoy slot (decision D1).
#
#   tools/hello_world/build_world.sh OUT_DIR [CORPUS_ROOT]
#
# CORPUS_ROOT defaults to ../CSF_unpacks (read-only); run from the repository
# root after ./build.sh.
set -euo pipefail
out=${1:?usage: build_world.sh OUT_DIR [CORPUS_ROOT]}
corpus=${2:-../CSF_unpacks}
donor="$corpus/Convoy/Maps/FR03/FR03.rws"
csfmod=./build/Release/csf-mod
mkdir -p "$out"
blender --background --factory-startup --python tools/hello_world/terrain.py -- \
    "$out/hello.csfworld" | grep '^csfworld'
"$csfmod" world-build "$out/hello.csfworld" "$donor" "$out/FR03.rws" --overwrite
