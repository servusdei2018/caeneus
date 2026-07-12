#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

parse_mode "$@"
require_command zig

print_section "Zig native benchmark ($BENCHMARK_MODE)"
if [[ "$BENCHMARK_MODE" == quick ]]; then
    (
        cd "$REPO_ROOT"
        zig build run -Doptimize=ReleaseFast -- --quick
    )
else
    (
        cd "$REPO_ROOT"
        zig build run -Doptimize=ReleaseFast
    )
fi
