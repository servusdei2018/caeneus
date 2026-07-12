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

require_command go
export CGO_ENABLED=1

archive="$(native_archive)"
if [[ -f "$archive" ]]; then
    export CGO_LDFLAGS="$archive"
else
    staging_dir="$(mktemp -d)"
    trap 'rm -rf "$staging_dir"' EXIT
    print_section "Staging native archive for Go"
    go -C "$REPO_ROOT/ext/go" run ./cmd/caeneus-native \
        --local \
        --dest "$staging_dir" >/dev/null
    staged_archive="$staging_dir/$(go env GOOS)_$(go env GOARCH)/libcaeneus.a"
    [[ -f "$staged_archive" ]] ||
        die "Go native staging did not produce: $staged_archive"
    export CGO_LDFLAGS="$staged_archive"
fi

print_section "Go comparison benchmarks ($BENCHMARK_MODE)"
if [[ "$BENCHMARK_MODE" == quick ]]; then
    CAENEUS_GO_QUICK=1 go -C "$REPO_ROOT/ext/go/benchmarks" test \
        -bench='^BenchmarkProfileA_Caeneus$' \
        -benchmem \
        -benchtime=1s \
        -count="$count" \
        -timeout=5m
elif [[ "$CAENEUS_ONLY" == true ]]; then
    go -C "$REPO_ROOT/ext/go/benchmarks" test \
        -bench='^BenchmarkProfile[AB]_Caeneus$' \
        -benchmem \
        -count="$count" \
        -timeout=30m
else
    go -C "$REPO_ROOT/ext/go/benchmarks" test \
        -bench=. \
        -benchmem \
        -count="$count" \
        -timeout=30m
fi
