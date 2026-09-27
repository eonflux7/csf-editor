#!/usr/bin/env bash
# Creates the Country authoring project (docs/plans/country-mission.md) in a
# new directory, entirely through the command-line tools, and builds both
# archives into PROJECT_DIR/dist/<build-id>/; never deploys.
# Run from the repository root after ./build.sh.
#
#   tools/country/build.sh PROJECT_DIR ORIGINAL_CONVOY_PAK ORIGINAL_GLOBALEK_PAK [CORPUS_ROOT] [DOWNLOADS]
#
# DOWNLOADS holds the two CC0 models (fetched from OpenGameArt when absent):
# picketfence_0.blend ("Basic Wooden Fence", WeaponGuy) and pzIIIs.blend
# ("panzerkampfwagen III", konserwa).
set -euo pipefail
project=${1:?usage: build.sh PROJECT_DIR ORIGINAL_CONVOY_PAK ORIGINAL_GLOBALEK_PAK [CORPUS_ROOT] [DOWNLOADS]}
convoy=${2:?original Convoy.pak required}
global=${3:?original GlobalEK.pak required}
corpus=$(realpath "${4:-../CSF_unpacks}")
downloads=${5:-}
csfmod=./build/Release/csf-mod
scene="$corpus/Convoy/Maps/FR03/Convoy.scn"
[ ! -e "$project" ] || { echo "project directory exists: $project" >&2; exit 1; }

# 1. The project in Convoy's slot (an emptied mission, a flat starter terrain).
"$csfmod" project-new "$project" --slot Convoy --name Country --corpus "$corpus" \
    --projects-root "$(dirname "$(realpath -m "$project")")"
project=$(realpath "$project")
src="$project/sources"

# 2. Sources: the two downloaded models, the terrain, placeholder lightmaps,
#    textures of its own (FR01's fields, the tank's painted hull and Panzers' tracks).
mkdir -p "$src/downloads"
for file in picketfence_0.blend pzIIIs.blend; do
    if [ -n "$downloads" ] && [ -f "$downloads/$file" ]; then cp "$downloads/$file" "$src/downloads/"
    else curl -sfL -o "$src/downloads/$file" "https://opengameart.org/sites/default/files/$file"; fi
done
blender --background --factory-startup --python tools/country/assets.py -- "$src/downloads" "$src/assets" | grep '^asset'
blender --background --factory-startup --python tools/country/terrain.py -- \
    "$src/world/terrain.csfworld" "$src/world/terrain.blend" | grep '^csfworld'
python3 tools/country/lightmaps.py "$src/lightmaps"
mkdir -p "$src/textures"
cp "$corpus/Ransom/Maps/FR01/Textures/FFLR_16A.dds" "$corpus/Ransom/Maps/FR01/Textures/FFLR_35A.dds" "$src/textures/"
cp "$corpus/Panzers/Models/Vehi/Textures/pcadena.dds" "$src/textures/PZ3_TRACK.dds"
python3 tools/country/textures.py "$src/textures"

# 3. The project records and the World: FR01's buildings and props and
#    Convoy's logs as pieces, the models, trees and bushes; everything on the
#    ground, then textures listed in the mission.
python3 tools/country/project.py "$project"
"$csfmod" project-build "$project" > "$project/build/world.log"
"$csfmod" project-heights "$project" --resnap | tail -1
"$csfmod" project-build "$project" >> "$project/build/world.log"
"$csfmod" project-lightmaps "$project"

# 4. The mission, as editor operations kept as components.
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
"$csfmod" decompile "$corpus/Convoy/Maps/FR03/Convoy.gsc" "$tmp/convoy.gsc.txt" > /dev/null
"$csfmod" decompile "$corpus/Ransom/Maps/FR01/Ransom.gsc" "$tmp/ransom.gsc.txt" > /dev/null
first_text=$(awk '/^texts /{print $4}' "$project/project.csfproj")
python3 tools/country/ops.py "$corpus" "$tmp/convoy.gsc.txt" "$tmp/ransom.gsc.txt" "$src/mission" "$first_text"
"$csfmod" mission-ops "$project/mission" "$scene" "$src/mission/country.ops" --components \
    --ground "$src/world/terrain.csfworld" > "$project/build/mission.log" || {
    grep -v '^applied\|^wrote' "$project/build/mission.log" >&2; exit 1;
}
"$csfmod" mission-components "$project/mission" "$scene" --ground "$src/world/terrain.csfworld" check
"$csfmod" mission-flow "$scene" --workspace "$project/mission" | grep -E '^(error|warning)' || true
"$csfmod" validate "$project/mission"

# 5. Both archives into dist/<build-id>/.
"$csfmod" project-archives "$project" --original-mission "$convoy" --original-texts "$global"
