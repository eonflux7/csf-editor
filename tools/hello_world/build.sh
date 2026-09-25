#!/usr/bin/env bash
# Build both archives into a new directory; never deploy implicitly.
# Run from the repository root after ./build.sh.
set -euo pipefail
out=${1:?usage: build.sh OUT_DIR ORIGINAL_CONVOY_PAK ORIGINAL_GLOBALEK_PAK [CORPUS_ROOT]}
convoy=${2:?original Convoy.pak required}
global=${3:?original GlobalEK.pak required}
corpus=${4:-../CSF_unpacks}
[ ! -e "$out" ] || { echo "output directory exists: $out" >&2; exit 1; }
mkdir -p "$out"
out=$(realpath "$out")
tools/hello_world/build_mission.sh "$out/mission" "$convoy" "$out/hello-world.pak" "$corpus" > "$out/mission.log" 2>&1 || {
    cat "$out/mission.log" >&2; exit 1;
}
tools/hello_world/build_texts.sh "$out/texts" "$global" "$out/hello-texts.pak" "$corpus" > "$out/texts.log" 2>&1 || {
    cat "$out/texts.log" >&2; exit 1;
}
# Validate again after each child has removed its temporary files.
./build/Release/csf-mod validate "$out/mission"
./build/Release/csf-mod validate "$out/texts"
python3 - "$out" "$convoy" "$global" "$corpus" <<'PY'
import hashlib
import json
import subprocess
import sys
from pathlib import Path

out, convoy, global_pak, corpus = map(Path, sys.argv[1:])
def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()
manifest = {
    'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
    'working_tree': subprocess.check_output(['git', 'status', '--short'], text=True),
    'blender': subprocess.check_output(['blender', '--version'], text=True).splitlines()[0],
    'corpus': str(corpus.resolve()),
    'inputs': {str(p.resolve()): digest(p) for p in (convoy, global_pak)},
    'outputs': {p.name: digest(p) for p in (out / 'hello-world.pak', out / 'hello-texts.pak')},
}
(out / 'build.json').write_text(json.dumps(manifest, indent=2) + '\n')
# Preserve the generator used even when the checkout has uncommitted changes.
import shutil
shutil.copytree('tools/hello_world', out / 'recipe', ignore=shutil.ignore_patterns('__pycache__'))
PY
printf 'Built mission and texts: %s\nSee docs/guides/hello-world-mission.md for deployment and playtest steps.\n' "$out"
