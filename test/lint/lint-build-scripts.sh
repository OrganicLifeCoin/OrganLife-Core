#!/usr/bin/env bash
#
# Ensure build scripts do not compile or run unit tests as part of normal builds.

export LC_ALL=C

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
EXIT_CODE=0

check_absent() {
    local pattern="$1"
    shift
    if rg -n "$pattern" "$@" >/dev/null; then
        echo "unexpected build-script test hook matched pattern: $pattern"
        rg -n "$pattern" "$@" || true
        EXIT_CODE=1
    fi
}

check_absent 'VERIFY_REWARD_SCHEDULE' \
    "$REPO_ROOT/scripts/build-depends.sh" \
    "$REPO_ROOT/scripts/build_all.sh"

check_absent 'verify_reward_schedule' \
    "$REPO_ROOT/scripts/build-depends.sh"

check_absent 'test/test_pivx' \
    "$REPO_ROOT/scripts/build-depends.sh" \
    "$REPO_ROOT/scripts/build_all.sh"

exit "$EXIT_CODE"
