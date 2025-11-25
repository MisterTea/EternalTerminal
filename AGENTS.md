# Testing

- To run unit tests: `pushd build; ninja && ctest --parallel; popd`
- To get code coverage: `bash coverage.sh`
- Any time a new test is added, you must run cmake for cmake/ctest to recognize the new test.
