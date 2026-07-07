#!/bin/bash
set -euo pipefail

if [ -d cov_build ]; then
    NEW_BUILD=0
else
    NEW_BUILD=1
fi

mkdir -p cov_build
pushd ./cov_build
cmake ../ -DBUILD_TEST=ON -DBUILD_GTEST=ON -DCODE_COVERAGE=ON -DDISABLE_TELEMETRY=ON -G Ninja
find . -name "*.gcda" -print0 | xargs -0 rm -f
ninja
ctest --parallel
popd
lcov --directory ./cov_build --capture --output-file ./code-coverage.info -rc lcov_branch_coverage=1
lcov --remove ./code-coverage.info \
    '/usr/include/*' \
    '/home/*/miniconda3/*' \
    '*/vcpkg_installed/*' \
    '*/external/*' \
    '*/test/integration_tests/*' \
    '*/test/unit_tests/*' \
    '*/test/*' \
    '*/proto/*' \
    '*/cov_build/ET.pb*' \
    '*/cov_build/ETerminal.pb*' \
    --output-file ./filtered.info -rc lcov_branch_coverage=1
genhtml filtered.info --branch-coverage --output-directory ./code_coverage_report/
echo "Report generated in code_coverage_report"
echo "html report at code_coverage_report/index.html"
