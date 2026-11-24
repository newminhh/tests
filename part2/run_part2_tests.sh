#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
VM_EXEC="${ROOT_DIR}/vm/vm-exec.sh"
REMOTE_ROOT="/root/newminhh"

LOG_FILE="$(mktemp)"
trap 'rm -f "$LOG_FILE"' EXIT

if "${VM_EXEC}" -- "true" 2>/dev/null; then
  echo "=== [Part 2] Syncing src/common, src/compat, and tests/part2 to newminhh-x32 VM ==="
  "${VM_EXEC}" --sync "${ROOT_DIR}/src/common" "${REMOTE_ROOT}/src/common"
  "${VM_EXEC}" --sync "${ROOT_DIR}/src/compat" "${REMOTE_ROOT}/src/compat"
  "${VM_EXEC}" --sync "${ROOT_DIR}/tests/part2" "${REMOTE_ROOT}/tests/part2"

  echo "=== [Part 2] Building libnewminhh_compat.so and installing inside x32 VM ==="
  "${VM_EXEC}" -- "make -C ${REMOTE_ROOT}/src/compat clean all install"

  echo "=== [Part 2] Building and running test_compat inside x32 VM ==="
  "${VM_EXEC}" -- "make -C ${REMOTE_ROOT}/tests/part2 clean all && ${REMOTE_ROOT}/tests/part2/test_compat" 2>&1 | tee "$LOG_FILE"
else
  echo "=== [Part 2] (x32 VM not reachable; running tests locally on host) ==="
  make -C "${ROOT_DIR}/src/compat" all
  make -C "${ROOT_DIR}/tests/part2" clean all
  (cd "${ROOT_DIR}/tests/part2" && ./test_compat) 2>&1 | tee "$LOG_FILE"
fi

grep -q "^PASS test_outgoing_marshaling$" "$LOG_FILE"
grep -q "^PASS test_incoming_bridge$" "$LOG_FILE"
grep -q "^PASS test_callbacks_and_trampoline_pool$" "$LOG_FILE"
grep -q "^PASS test_syscalls$" "$LOG_FILE"
grep -q "^PASS test_struct_repacking$" "$LOG_FILE"
grep -q "^PASS test_dl_and_wrappers$" "$LOG_FILE"
grep -q "^PASS test_cstd_features$" "$LOG_FILE"
grep -q "^PASS test_lookup_runtime$" "$LOG_FILE"
grep -q "^ALL_PART2_TESTS_PASSED$" "$LOG_FILE"

# Verify structured diagnostics were emitted on negative test paths
grep -q "\[newminhh:syscall\] ERROR: unsupported i386 syscall nr=9999" "$LOG_FILE"
grep -q "\[newminhh:wrappers\] ERROR: unsupported symbol 'gnu_get_libc_version'" "$LOG_FILE"
grep -q "\[newminhh:dl\] ERROR:" "$LOG_FILE"

echo "=== [Part 2] ALL VERIFICATION CHECKS PASSED ==="
