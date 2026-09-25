#!/usr/bin/env bash
# Packages the hello-world project's mission text (built by `csf-mod
# project-build` into PROJECT_DIR/build/GlobalEK/Texts/Convoy.fli) as a
# GlobalEK.pak. Mission text is read only from GlobalEK.pak: a Texts/*.fli
# inside the mission archive is ignored (in-game, 2026-09-25).
#
#   tools/hello_world/build_texts.sh PROJECT_DIR ORIGINAL_GLOBALEK_PAK OUT_PAK [CORPUS_ROOT]
#
# Creates the workspace PROJECT_DIR/texts. Deploy with
# `csf-mod deploy-pak PROJECT_DIR/texts OUT_PAK <test-install> GlobalEK.pak --apply`.
set -euo pipefail
project=${1:?usage}; original=${2:?usage}; out=${3:?usage}; corpus=${4:-../CSF_unpacks}
csfmod=./build/Release/csf-mod
project=$(realpath "$project"); ws="$project/texts"
[ ! -e "$ws" ] || { echo "workspace exists: $ws" >&2; exit 1; }
"$csfmod" init "$ws" "$corpus/GlobalEK" "hello-world texts" >/dev/null
"$csfmod" add "$ws" Texts/Convoy.fli "$project/build/GlobalEK/Texts/Convoy.fli"
"$csfmod" validate "$ws"
"$csfmod" export-mission "$ws" "$original" "$out" --overwrite
