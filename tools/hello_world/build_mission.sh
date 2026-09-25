#!/usr/bin/env bash
# Builds the hello-world mission archive in Convoy's slot (decision D1): the map
# and sector map that build_world.sh built into the project, and a scene and
# scripts written from scratch by scene.py. Only Convoy's environment (.MUNDOVIS) and
# databases are reused; the doberman is imported from Ransom and the
# radio's ghost behavior and supporting crate from Escape.
#
#   tools/hello_world/build_mission.sh PROJECT_DIR ORIGINAL_CONVOY_PAK OUT_PAK [CORPUS_ROOT]
#
# PROJECT_DIR comes from build_world.sh; its mission workspace PROJECT_DIR/mission
# is created (it must not exist). Deploy the archive with
# `csf-mod deploy-pak PROJECT_DIR/mission OUT_PAK <test-install> maps/Convoy.pak --apply`,
# together with the GlobalEK.pak from build_texts.sh (the objective strings).
set -euo pipefail
project=${1:?usage}; original=${2:?usage}; out=${3:?usage}; corpus=${4:-../CSF_unpacks}
project=$(realpath "$project"); ws="$project/mission"
csfmod=./build/Release/csf-mod
scene="$corpus/Convoy/Maps/FR03/Convoy.scn"
[ ! -e "$ws" ] || { echo "workspace exists: $ws" >&2; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
"$csfmod" decompile "$scene" "$tmp/convoy.scn.txt" >/dev/null
python3 tools/hello_world/scene.py "$tmp/convoy.scn.txt" "$project/sources/world/terrain.csfworld" "$tmp/scene"
cat "$tmp/scene/anchors.csfproj" >> "$project/project.csfproj"
"$csfmod" compile "$tmp/scene/Convoy.scn.txt" "$scene" "$tmp/Convoy.scn"
"$csfmod" compile "$tmp/scene/Convoy.csc.txt" "${scene%.scn}.csc" "$tmp/Convoy.csc"

"$csfmod" mission-edit "$ws" "$scene" >/dev/null  # creates the workspace
# `add` records where a file is, so the generated scene and cutscene go into
# the workspace's authored/ (MissionEditor saves its edits there too).
mkdir -p "$ws/authored/Maps/FR03"
for file in Convoy.scn Convoy.csc; do
    cp "$tmp/$file" "$ws/authored/Maps/FR03/$file"
    "$csfmod" add "$ws" "Maps/FR03/$file" "$ws/authored/Maps/FR03/$file"
done
# Ransom's doberman and its walk and run animations; Escape's telephone ghost
# and wooden crate. Give the ghost the radio's visible model and bounding box
# through MissionEditor, preserving its interaction behavior and physics.
ops=(--import-class "$corpus/Ransom" 431 --import-anim "$corpus/Ransom" 2383 --import-anim "$corpus/Ransom" 2384
     --import-class "$corpus/Escape" 484 --import-class "$corpus/Escape" 383
     --actor-look 15 211)
ops+=(--force)
./build/Release/csf-info program "$corpus/Convoy/Maps/FR03/Convoy.gsc" --scripts > "$tmp/donor-scripts.txt"
sed -n 's/^SCRIPT\t.*\tid=\([0-9]*\)\t.*/\1/p' "$tmp/donor-scripts.txt" > "$tmp/donor-ids.txt"
while read -r id; do
    ops+=(--delete-script "$id")
done < "$tmp/donor-ids.txt"
for file in "$tmp"/scene/scripts/*.txt; do ops+=(--add-script "$file"); done
"$csfmod" mission-edit "$ws" "$scene" "${ops[@]}" > "$tmp/mission-edit.log" 2>&1 || {
    cat "$tmp/mission-edit.log" >&2; exit 1;
}
sed '/^applied/d' "$tmp/mission-edit.log"
# The project's generated map files; `csf-mod project-build` keeps them current.
for file in Maps/FR03/FR03.rws Maps/FR03/FR03_col.rws Maps/Secs/Convoy.sec; do
    "$csfmod" add "$ws" "$file" "$project/build/$file"
done
"$csfmod" validate "$ws"
"$csfmod" export-mission "$ws" "$original" "$out" --overwrite
