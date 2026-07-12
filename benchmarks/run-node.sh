#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

parse_mode "$@"
require_command node
require_command npm
ensure_native_archive

NODE_PROJECT="$REPO_ROOT/ext/node"
NODE_CACHE=all
if [[ "$CAENEUS_ONLY" == true ]]; then
    NODE_CACHE=caeneus
fi
if [[ ! -d "$NODE_PROJECT/node_modules" ]]; then
    print_section "Preparing Node.js benchmark environment"
    npm --prefix "$NODE_PROJECT" install
fi

print_section "Node.js comparison benchmark ($BENCHMARK_MODE)"
if [[ "$BENCHMARK_MODE" == quick ]]; then
    npm --prefix "$NODE_PROJECT" run benchmark -- \
        --profile A \
        --cache caeneus \
        --operations 10000 \
        --keys 2048 \
        --workers 1 \
        --sample-rate 100
else
    npm --prefix "$NODE_PROJECT" run benchmark -- \
        --profile all \
        --cache "$NODE_CACHE" \
        --workers 1
fi
