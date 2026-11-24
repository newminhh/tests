#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
PATCHER_SRC="${REPO_ROOT}/src/patcher"

export PYTHONPATH="${PATCHER_SRC}:${PYTHONPATH:-}"

echo "[part7] Running Level 3 unit self-tests (gates, sanitize, diffverify)..."
python3 "${PATCHER_SRC}/re_patcher/level3/gates.py"
python3 "${PATCHER_SRC}/re_patcher/level3/sanitize.py"
python3 "${PATCHER_SRC}/re_patcher/level3/diffverify.py"

echo "[part7] Running Part 7 Level 3 end-to-end suite..."
python3 "${SCRIPT_DIR}/test_level3_suite.py"
