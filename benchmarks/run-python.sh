#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

parse_mode "$@"
require_command uv
ensure_native_archive

PYTHON_PROJECT="$REPO_ROOT/ext/python"
PYTHON_BENCHMARK="$PYTHON_PROJECT/benchmark.py"
PYTHON_CONTENTION="$PYTHON_PROJECT/benchmark_multithread.py"
PYTHON_IMPLEMENTATIONS="caeneus,plain_dict,cachetools_lru,lru_dict"
PYTHON_CACHE="$PYTHON_IMPLEMENTATIONS"
PYTHON_WORKLOAD="${PYTHON_WORKLOAD:-write_then_read}"
if [[ "$CAENEUS_ONLY" == true ]]; then
    PYTHON_CACHE=caeneus
fi

print_section "Preparing Python benchmark environment"
rm -rf "$PYTHON_PROJECT/build/" "$PYTHON_PROJECT/dist/" "$PYTHON_PROJECT"/*.egg-info
CC=clang CXX=clang++ uv sync --project "$PYTHON_PROJECT" --reinstall --no-cache --extra benchmarks

print_section "Python comparison benchmark ($BENCHMARK_MODE)"
if [[ "$BENCHMARK_MODE" == quick ]]; then
    uv run --project "$PYTHON_PROJECT" python "$PYTHON_BENCHMARK" \
        --profile A \
        --cache caeneus \
        --operations 10000 \
        --keys 2048 \
        --workers 1 \
        --sample-rate 100 \
        --workload "$PYTHON_WORKLOAD"
else
    uv run --project "$PYTHON_PROJECT" python "$PYTHON_BENCHMARK" \
        --profile all \
        --cache "$PYTHON_CACHE" \
        --workers 1 \
        --workload "$PYTHON_WORKLOAD"
fi

print_section "Python contention benchmark ($BENCHMARK_MODE)"
if [[ "$BENCHMARK_MODE" == quick ]]; then
    uv run --project "$PYTHON_PROJECT" python "$PYTHON_CONTENTION" \
        --profile read_hot \
        --implementation caeneus \
        --api get_into \
        --workers 1,2 \
        --mode shared \
        --operations-per-worker 2000 \
        --keys-per-worker 128 \
        --sample-rate 100
else
    uv run --project "$PYTHON_PROJECT" python "$PYTHON_CONTENTION" \
        --profile all \
        --implementation caeneus \
        --api get_into \
        --workers 1,2,4,8 \
        --mode both
fi
