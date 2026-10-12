# C code coverage

The `JLS_COVERAGE` CMake option instruments the library and the unit
tests for gcov.  It needs gcc or clang.  gcovr formats the report.

    cmake -S . -B build_cov -DCMAKE_BUILD_TYPE=Debug -DJLS_COVERAGE=ON
    cmake --build build_cov -j8
    ctest --test-dir build_cov
    pip install gcovr
    gcovr -r . --filter src/ --object-directory build_cov
    gcovr -r . --filter src/ --object-directory build_cov --txt-metric branch
    gcovr -r . --filter src/ --object-directory build_cov \
        --html-details build_cov/coverage/index.html

gcov counters accumulate across runs.  To measure a single run, delete
the `*.gcda` files in `build_cov` or start from a fresh build directory.

With clang, give gcovr the matching gcov wrapper, such as
`--gcov-executable "llvm-cov gcov"`.

The Linux CI job `coverage` in .github/workflows/packaging.yml uploads
the HTML report as the `coverage` artifact.

To keep ctest fast, test/read_fault_test samples its I/O fault points,
and test/writer_test runs fewer split, duplicate, and I/O fault cases,
with the same coverage.  Before a release, or after changing the writer,
recovery, repair, or copy, run every case with `--full`:

    <build>/test/read_fault_test --full    # about a minute (Release)
    <build>/test/writer_test --full        # a few seconds

See doc/plans/completed/code_coverage.md for the baseline and the uncovered
code.
