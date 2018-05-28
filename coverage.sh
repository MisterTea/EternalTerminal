pushd ./build
cmake ../ -DBUILD_TEST=ON -DBUILD_GTEST=ON
make -j8
find . -name "*.gcda" -print0 | xargs -0 rm
popd
./build/test/et-test
lcov --directory ./build --capture --output-file ./code-coverage.info -rc lcov_branch_coverage=1
genhtml code-coverage.info --branch-coverage --output-directory ./code_coverage_report/
echo "Report generated in code_coverage_report"
open code_coverage_report/index.html
