#!/usr/bin/env bash
# Creates the hello-world authoring project (docs/plans/editor-project-format.md)
# in PROJECT_DIR and builds its map: the Blender terrain (terrain.py) into
# sources/world/, project.csfproj with the house, tree and plant placements
# (project.py), then `csf-mod project-build` into build/: FR03.rws, FR03_col.rws
# and Maps/Secs/Convoy.sec for the Convoy slot (decision D1).
#
#   tools/hello_world/build_world.sh PROJECT_DIR [CORPUS_ROOT]
#
# CORPUS_ROOT defaults to ../CSF_unpacks (read-only); run from the repository
# root after ./build.sh.
set -euo pipefail
project=${1:?usage: build_world.sh PROJECT_DIR [CORPUS_ROOT]}
corpus=${2:-../CSF_unpacks}
mkdir -p "$project/sources/world"
blender --background --factory-startup --python tools/hello_world/terrain.py -- \
    "$project/sources/world/terrain.csfworld" "$project/sources/world/terrain.blend" | grep '^csfworld'
python3 tools/hello_world/project.py "$project" "$corpus"
./build/Release/csf-mod project-build "$project"
