#!/bin/bash
# Coverage report generation using Docker
# Builds a debug unit-test image and runs lcov inside it.

set -e

registry=ghcr.io/testillano

git_root_dir="$(git rev-parse --show-toplevel 2>/dev/null)"
[ -z "$git_root_dir" ] && { echo "Go into the git repository !" ; exit 1 ; }

cd "${git_root_dir}"
rm -rf coverage

echo "Building debug image for coverage..."
docker build --target build \
  --build-arg build_type=Debug \
  --build-arg make_procs=$(grep processor /proc/cpuinfo -c) \
  -t ${registry}/diametercomm:latest-cov .

echo "Running coverage collection..."
docker run --rm \
  -e DEBIAN_FRONTEND=noninteractive \
  -v "${PWD}/coverage:/output" \
  --entrypoint /bin/bash \
  ${registry}/diametercomm:latest-cov -c '
    apt-get update && apt-get install -y lcov bc >/dev/null 2>&1
    cd /code
    CAPTURE_OPTS="--ignore-errors empty --ignore-errors inconsistent --ignore-errors mismatch"
    lcov --capture --directory ./src --directory ./ut --initial --output-file base-coverage.info ${CAPTURE_OPTS}
    ./build/Debug/bin/unit-test
    lcov --capture --directory ./src --directory ./ut --output-file test-coverage.info --ignore-errors gcov,gcov ${CAPTURE_OPTS}
    lcov -a base-coverage.info -a test-coverage.info -o total-coverage.info
    lcov --extract total-coverage.info "/code/src/*" --output-file extracted-coverage.info
    lcov --remove extracted-coverage.info "main.cpp" --output-file final-coverage.info --ignore-errors unused
    genhtml final-coverage.info --output-directory /output
    cp final-coverage.info /output/lcov.info
  '

echo "Coverage report: coverage/index.html"
firefox coverage/index.html &
