#!/usr/bin/env bash
set -euo pipefail

BENCHMARKS_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$BENCHMARKS_DIR/.." && pwd)"
export ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$REPO_ROOT/.zig-global-cache}"

die() {
    printf 'benchmark error: %s\n' "$*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

print_section() {
    printf '\n===== %s =====\n' "$*"
}

native_archive() {
    case "$(uname -s)" in
        Linux|Darwin)
            printf '%s\n' "$REPO_ROOT/zig-out/lib/libcaeneus.a"
            ;;
        *)
            die "benchmark runners currently require Linux or macOS"
            ;;
    esac
}

ensure_native_archive() {
    local archive
    archive="$(native_archive)"
    if [[ -f "$archive" ]]; then
        return
    fi

    require_command zig
    print_section "Building native ReleaseFast library"
    (
        cd "$REPO_ROOT"
        zig build -Doptimize=ReleaseFast
    )
    [[ -f "$archive" ]] || die "native archive was not produced: $archive"
}

parse_mode() {
    BENCHMARK_MODE=full
    CAENEUS_ONLY=false
    while (($#)); do
        case "$1" in
            --quick)
                BENCHMARK_MODE=quick
                ;;
            --full)
                BENCHMARK_MODE=full
                ;;
            --caeneus-only)
                CAENEUS_ONLY=true
                ;;
            *)
                die "unknown option: $1 (expected --quick, --full, or --caeneus-only)"
                ;;
        esac
        shift
    done
}
