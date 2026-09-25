#!/usr/bin/env bash
# Builds a GlobalEK.pak whose Texts/Convoy.fli carries the hello-world strings
# (tools/hello_world/texts.py). Mission text is read only from GlobalEK.pak: a
# Texts/*.fli inside the mission archive is ignored (in-game, 2026-09-25).
#
#   tools/hello_world/build_texts.sh WORKSPACE ORIGINAL_GLOBALEK_PAK OUT_PAK [CORPUS_ROOT]
#
# Deploy with `csf-mod deploy-pak WORKSPACE OUT_PAK <test-install> GlobalEK.pak --apply`.
set -euo pipefail
ws=${1:?usage}; original=${2:?usage}; out=${3:?usage}; corpus=${4:-../CSF_unpacks}
csfmod=./build/Release/csf-mod
[ ! -e "$ws" ] || { echo "workspace exists: $ws" >&2; exit 1; }
"$csfmod" init "$ws" "$corpus/GlobalEK" "hello-world texts" >/dev/null
mkdir -p "$ws/extra/Texts"
python3 tools/hello_world/texts.py "$corpus/GlobalEK/Texts/Convoy.fli" "$ws/extra/Texts/Convoy.fli"
"$csfmod" add "$ws" Texts/Convoy.fli "$ws/extra/Texts/Convoy.fli"
"$csfmod" validate "$ws"
"$csfmod" export-mission "$ws" "$original" "$out" --overwrite
