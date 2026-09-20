#!/usr/bin/env bash
set -euo pipefail

config=Release
core_only=0
no_build=0

usage() {
    cat <<'EOF'
Usage: ./test.sh [options]

Options:
  -c, --config <Debug|Release>  Configuration to test (default: Release)
      --core-only               Test the core targets only
      --no-build                Run tests without rebuilding the test executable
  -h, --help                    Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--config)
            config="${2:-}"
            shift 2
            ;;
        --core-only)
            core_only=1
            shift
            ;;
        --no-build)
            no_build=1
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
test_preset="${configure_preset}-${config,,}"

cd "$(dirname "$0")"

if [[ ! -f "$build_directory/CMakeCache.txt" ]]; then
    cmake --preset "$configure_preset"
fi

if [[ $no_build -eq 0 ]]; then
    cmake --build --preset "$test_preset" --target rws_core_tests --parallel
fi

ctest --preset "$test_preset"
