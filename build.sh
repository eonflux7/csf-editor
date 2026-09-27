#!/usr/bin/env bash
set -euo pipefail

config=Release
target=""
core_only=0
reconfigure=0

usage() {
    cat <<'EOF'
Usage: ./build.sh [options]

Options:
  -c, --config <Debug|Release>  Configuration to build (default: Release)
  -t, --target <name>           Build only the given target
      --core-only               Build without GLFW, ImGui, or OpenGL
  -r, --reconfigure             Force CMake to configure the build tree again
  -h, --help                    Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--config)
            config="${2:-}"
            shift 2
            ;;
        -t|--target)
            target="${2:-}"
            shift 2
            ;;
        --core-only)
            core_only=1
            shift
            ;;
        -r|--reconfigure)
            reconfigure=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

case "$config" in
    Debug|Release) ;;
    *)
        echo "Invalid configuration: $config (expected Debug or Release)" >&2
        exit 2
        ;;
esac

if [[ $core_only -eq 1 ]]; then
    configure_preset=linux-core
    build_directory=build-core
else
    configure_preset=linux
    build_directory=build
fi
build_preset="${configure_preset}-${config,,}"

cd "$(dirname "$0")"

if [[ $reconfigure -eq 1 || ! -f "$build_directory/CMakeCache.txt" ]]; then
    cmake --preset "$configure_preset"
fi

arguments=(--build --preset "$build_preset" --parallel)
if [[ -n "$target" ]]; then
    arguments+=(--target "$target")
fi
cmake "${arguments[@]}"
