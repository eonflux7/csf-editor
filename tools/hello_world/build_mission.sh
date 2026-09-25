#!/usr/bin/env bash
# Builds the hello-world mission archive in Convoy's slot (decision D1): the map
# from build_world.sh, its sector map (`csf-mod sector-build`) and a scene and scripts
# written from scratch by scene.py. Only Convoy's environment (.MUNDOVIS) and
# databases are reused; the doberman is imported from Ransom and the
# radio's ghost behavior and supporting crate from Escape.
#
#   tools/hello_world/build_mission.sh WORKSPACE ORIGINAL_CONVOY_PAK OUT_PAK [CORPUS_ROOT]
#
# WORKSPACE is created (it must not exist). Deploy the archive with
# `csf-mod deploy-pak WORKSPACE OUT_PAK <test-install> maps/Convoy.pak --apply`,
# together with the GlobalEK.pak from build_texts.sh (the objective strings).
set -euo pipefail
ws=${1:?usage}; original=${2:?usage}; out=${3:?usage}; corpus=${4:-../CSF_unpacks}
csfmod=./build/Release/csf-mod
scene="$corpus/Convoy/Maps/FR03/Convoy.scn"
[ ! -e "$ws" ] || { echo "workspace exists: $ws" >&2; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
tools/hello_world/build_world.sh "$tmp/world" "$corpus"
"$csfmod" sector-build "$tmp/world/hello.csfworld" "$tmp/Convoy.sec"
"$csfmod" decompile "$scene" "$tmp/convoy.scn.txt" >/dev/null
python3 tools/hello_world/scene.py "$tmp/convoy.scn.txt" "$tmp/world/hello.csfworld" "$tmp/scene"
"$csfmod" compile "$tmp/scene/Convoy.scn.txt" "$scene" "$tmp/Convoy.scn"
"$csfmod" compile "$tmp/scene/Convoy.csc.txt" "${scene%.scn}.csc" "$tmp/Convoy.csc"

"$csfmod" mission-edit "$ws" "$scene" >/dev/null  # creates the workspace
"$csfmod" add "$ws" Maps/FR03/Convoy.scn "$tmp/Convoy.scn"
"$csfmod" add "$ws" Maps/FR03/Convoy.csc "$tmp/Convoy.csc"
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
# `add` records source paths; keep these files after the scratch directory is
# removed so the project can be reopened and exported again.
mkdir -p "$ws/extra/Maps/FR03" "$ws/extra/Maps/Secs"
cp "$tmp/world/FR03.rws" "$tmp/world/FR03_col.rws" "$ws/extra/Maps/FR03/"
cp "$tmp/Convoy.sec" "$ws/extra/Maps/Secs/"
"$csfmod" add "$ws" Maps/FR03/FR03.rws "$ws/extra/Maps/FR03/FR03.rws"
"$csfmod" add "$ws" Maps/FR03/FR03_col.rws "$ws/extra/Maps/FR03/FR03_col.rws"
"$csfmod" add "$ws" Maps/Secs/Convoy.sec "$ws/extra/Maps/Secs/Convoy.sec"
"$csfmod" validate "$ws"
"$csfmod" export-mission "$ws" "$original" "$out" --overwrite
