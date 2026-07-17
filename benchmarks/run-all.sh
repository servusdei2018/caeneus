#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

BENCHMARK_MODE=full
CAENEUS_ONLY=false
count=3
while (($#)); do
    case "$1" in
        --quick)
            BENCHMARK_MODE=quick
            count=1
            ;;
        --full)
            BENCHMARK_MODE=full
            ;;
        --caeneus-only)
            CAENEUS_ONLY=true
            ;;
        --count)
            (($# >= 2)) || die "--count requires a positive integer"
            count="$2"
            shift
            ;;
        *)
            die "unknown option: $1 (expected --quick, --full, --caeneus-only, or --count N)"
            ;;
    esac
    shift
done

[[ "$count" =~ ^[1-9][0-9]*$ ]] ||
    die "--count must be a positive integer"

require_command zig
require_command go
require_command uv
require_command node
require_command npm

print_section "Building native ReleaseFast library"
(
    cd "$REPO_ROOT"
    zig build -Doptimize=ReleaseFast -Dcpu=native
)

"$SCRIPT_DIR/run-zig.sh" "--$BENCHMARK_MODE"
if [[ "$CAENEUS_ONLY" == true || "$BENCHMARK_MODE" == quick ]]; then
    "$SCRIPT_DIR/run-go.sh" "--$BENCHMARK_MODE" --caeneus-only --count "$count"
    "$SCRIPT_DIR/run-python.sh" "--$BENCHMARK_MODE" --caeneus-only
    "$SCRIPT_DIR/run-node.sh" "--$BENCHMARK_MODE" --caeneus-only
else
    "$SCRIPT_DIR/run-go.sh" "--$BENCHMARK_MODE" --count "$count"
    "$SCRIPT_DIR/run-python.sh" "--$BENCHMARK_MODE"
    "$SCRIPT_DIR/run-node.sh" "--$BENCHMARK_MODE"
fi
