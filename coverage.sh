#!/bin/bash
set -euo pipefail

# Homebrew lcov is often missing from restricted PATHs (CI local agents, etc).
export PATH="/opt/homebrew/bin:/usr/local/bin:${PATH}"

die() {
  echo "error: $*" >&2
  exit 1
}

command -v lcov >/dev/null 2>&1 || die "lcov not found; install lcov"
command -v genhtml >/dev/null 2>&1 || die "genhtml not found; install lcov"

lcov_major() {
  local ver
  ver=$(lcov --version 2>/dev/null | grep -oE '[0-9]+' | head -1)
  echo "${ver:-1}"
}

LCOV_MAJOR=$(lcov_major)
LCOV_IGNORE=()
LCOV_BRANCH=()
GENHTML_IGNORE=()
if [[ "${LCOV_MAJOR}" -ge 2 ]]; then
  # lcov 2.x treats a code listed once as a warning and twice as ignored.
  # Apple clang/libc++ trips inconsistent, unsupported, and category checks.
  LCOV_IGNORE=(--ignore-errors deprecated,deprecated,gcov,gcov,source,source,mismatch,mismatch,inconsistent,inconsistent,unused,unused,unsupported,unsupported,format,format,count,count,category,category)
  LCOV_BRANCH=(--rc branch_coverage=1)
  GENHTML_IGNORE=(--ignore-errors source,source,deprecated,deprecated,inconsistent,inconsistent,unsupported,unsupported,category,category)
else
  LCOV_IGNORE=(--ignore-errors gcov,source)
  LCOV_BRANCH=(-rc lcov_branch_coverage=1)
  GENHTML_IGNORE=(--ignore-errors source)
fi

run_lcov() {
  lcov "${LCOV_IGNORE[@]}" "${LCOV_BRANCH[@]}" "$@"
}

BUILD_DIR="${COVERAGE_BUILD_DIR:-cov_build}"
SKIP_BUILD="${COVERAGE_SKIP_BUILD:-0}"

if [[ "${SKIP_BUILD}" != "1" ]]; then
  command -v ninja >/dev/null 2>&1 || die "ninja not found; install ninja"
  mkdir -p "${BUILD_DIR}"
  pushd "./${BUILD_DIR}"
  cmake ../ -DBUILD_TESTING=ON -DCODE_COVERAGE=ON -DDISABLE_TELEMETRY=ON -G Ninja
  find . -name "*.gcda" -delete
  ninja
  jobs=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
  ctest --parallel "${jobs}" --output-on-failure
  popd
fi

run_lcov --directory "./${BUILD_DIR}" --capture --output-file ./code-coverage.info
run_lcov --remove ./code-coverage.info \
    '/usr/include/*' \
    '/Applications/Xcode.app/*' \
    '/home/*/miniconda3/*' \
    '*/vcpkg_installed/*' \
    '*/external/*' \
    '*/test/integration_tests/*' \
    '*/test/unit_tests/*' \
    '*/test/*' \
    '*/proto/*' \
    '*/cov_build/ET.pb*' \
    '*/cov_build/ETerminal.pb*' \
    "*/${BUILD_DIR}/ET.pb*" \
    "*/${BUILD_DIR}/ETerminal.pb*" \
    --output-file ./filtered.info
genhtml filtered.info --branch-coverage "${GENHTML_IGNORE[@]}" --output-directory ./code_coverage_report/
echo "Report generated in code_coverage_report"
echo "html report at code_coverage_report/index.html"

# Unix/Linux HTM library coverage. WIN32/ConPTY branches are preprocessor-
# excluded on this job, so they are not in the denominator.
run_lcov --extract ./filtered.info '*/src/htm/*' --output-file ./htm.info
run_lcov --remove ./htm.info \
    '*/HtmClientMain.cpp' \
    '*/HtmServerMain.cpp' \
    --output-file ./htm-lib.info
echo "=== HTM library coverage (Unix compiled lines) ==="
run_lcov --summary ./htm-lib.info || true

python3 - "${HTM_MIN_LINE_COVERAGE:-80}" ./htm-lib.info <<'PY'
import sys

min_pct = float(sys.argv[1])
path = sys.argv[2]
lf = lh = 0
with open(path) as handle:
    for line in handle:
        if line.startswith("LF:"):
            lf += int(line.split(":", 1)[1])
        elif line.startswith("LH:"):
            lh += int(line.split(":", 1)[1])
pct = (100.0 * lh / lf) if lf else 0.0
print(f"HTM Unix line coverage: {pct:.1f}% ({lh}/{lf}); required {min_pct:.0f}%")
if pct + 1e-9 < min_pct:
    sys.exit(1)
PY
