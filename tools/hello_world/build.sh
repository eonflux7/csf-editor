#!/usr/bin/env bash
# Creates the hello-world authoring project in a new directory and builds both
# archives into PROJECT_DIR/dist/<build-id>/; never deploys implicitly.
# Run from the repository root after ./build.sh.
set -euo pipefail
project=${1:?usage: build.sh PROJECT_DIR ORIGINAL_CONVOY_PAK ORIGINAL_GLOBALEK_PAK [CORPUS_ROOT]}
convoy=${2:?original Convoy.pak required}
global=${3:?original GlobalEK.pak required}
corpus=${4:-../CSF_unpacks}
[ ! -e "$project" ] || { echo "project directory exists: $project" >&2; exit 1; }
mkdir -p "$project"
project=$(realpath "$project")
dist="$project/dist/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$dist"
tools/hello_world/build_world.sh "$project" "$corpus" > "$dist/world.log" 2>&1 || {
    cat "$dist/world.log" >&2; exit 1;
}
tools/hello_world/build_mission.sh "$project" "$convoy" "$dist/hello-world.pak" "$corpus" > "$dist/mission.log" 2>&1 || {
    cat "$dist/mission.log" >&2; exit 1;
}
tools/hello_world/build_texts.sh "$project/texts" "$global" "$dist/hello-texts.pak" "$corpus" > "$dist/texts.log" 2>&1 || {
    cat "$dist/texts.log" >&2; exit 1;
}
# Validate again after each child has removed its temporary files.
./build/Release/csf-mod validate "$project/mission"
./build/Release/csf-mod validate "$project/texts"
python3 - "$dist" "$convoy" "$global" "$corpus" <<'PY'
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

dist, convoy, global_pak, corpus = map(Path, sys.argv[1:])
def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()
manifest = {
    'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
    'working_tree': subprocess.check_output(['git', 'status', '--short'], text=True),
    'blender': subprocess.check_output(['blender', '--version'], text=True).splitlines()[0],
    'corpus': str(corpus.resolve()),
    'inputs': {str(p.resolve()): digest(p) for p in (convoy, global_pak)},
    'outputs': {p.name: digest(p) for p in (dist / 'hello-world.pak', dist / 'hello-texts.pak')},
}
(dist / 'build.json').write_text(json.dumps(manifest, indent=2) + '\n')
# Preserve the generator used even when the checkout has uncommitted changes.
shutil.copytree('tools/hello_world', dist / 'recipe', ignore=shutil.ignore_patterns('__pycache__'))
PY
printf 'Built project %s\nArchives: %s\nSee docs/guides/hello-world-mission.md for deployment and playtest steps.\n' \
    "$project" "$dist"
