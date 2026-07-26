#!/bin/bash

# Narrow test selection by mean '--gtest_filter', i.e.:
#
# $ ./ut.sh --gtest_list_tests
# $ ./ut.sh --gtest_filter=PeerConnection_test.*

DIAMETERCOMM_UT_IMAGE=${DIAMETERCOMM_UT_IMAGE:-ghcr.io/testillano/diametercomm_ut:latest}

# Build unit-test image if not available:
if ! docker image inspect ${DIAMETERCOMM_UT_IMAGE} &>/dev/null; then
  echo "Building unit-test image..."
  docker build --target unit-test -t ${DIAMETERCOMM_UT_IMAGE} .
fi

docker run --rm -it ${DIAMETERCOMM_UT_IMAGE} $@
