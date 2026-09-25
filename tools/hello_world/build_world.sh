#!/usr/bin/env bash
# Builds the hello-world map (docs/plans/hello-world-mission.md) into OUT_DIR:
# Blender terrain -> .csfworld (+ Convoy's house and two trees) -> FR03.rws and
# FR03_col.rws, replacing Convoy's map files in the Convoy slot (decision D1).
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
blender --background --factory-startup --python tools/blender/make_test_terrain.py -- \
    "$out/hello.csfworld" --size 100 --cells 50 --hill-height 4 --hill-radius 15 \
    --texture FFLR_33A --surface Tierra | grep '^csfworld'
cat >> "$out/hello.csfworld" <<'LINES'
# Convoy's house (the World triangles lit by the EDIFICIO_1* lightmaps), on the ground
piece -5650 1020 -6850 -4120 2500 -5440 -2500 -4 -2500
# two Convoy trees (each two scene instances sharing one placement)
prop 1,2 2500 0 -2000
prop 7,8 2000 0 2500 90
LINES
"$csfmod" world-build "$out/hello.csfworld" "$donor" "$out/FR03.rws" --overwrite
