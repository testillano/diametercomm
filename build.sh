#!/bin/bash
# =============================================================================
# diametercomm build script (flat multi-stage model)
# =============================================================================
set -e

SCR="$(readlink -f "$0")"
SCR_DIR="$(dirname "${SCR}")"
cd "${SCR_DIR}"

DOCKERFILE=Dockerfile
registry=ghcr.io/testillano

parse_arg() {
  grep "^ARG ${1}=" "${DOCKERFILE}" | head -1 | cut -d= -f2
}

make_procs__dflt=$(grep processor /proc/cpuinfo -c)
build_type__dflt=$(parse_arg build_type)
boost_ver__dflt=$(parse_arg boost_ver)
ert_logger_ver__dflt=$(parse_arg ert_logger_ver)
nlohmann_json_ver__dflt=$(parse_arg nlohmann_json_ver)
pboettch_jsonschemavalidator_ver__dflt=$(parse_arg pboettch_jsonschemavalidator_ver)
jupp0r_prometheuscpp_ver__dflt=$(parse_arg jupp0r_prometheuscpp_ver)
civetweb_civetweb_ver__dflt=$(parse_arg civetweb_civetweb_ver)
ert_metrics_ver__dflt=$(parse_arg ert_metrics_ver)
ert_diametercodec_ver__dflt=$(parse_arg ert_diametercodec_ver)
google_test_ver__dflt=$(parse_arg google_test_ver)
image_tag__dflt=latest

usage() {
  cat << EOF

  Usage: $0 [--builder|--image]

         (no args):   builds everything (--image).
         --builder:   builds deps stage only.
         --image:     full build: deps + compile + unit-test image.

         Environment variables (override any version):
           image_tag, make_procs, build_type, boost_ver, ert_logger_ver,
           nlohmann_json_ver, pboettch_jsonschemavalidator_ver,
           jupp0r_prometheuscpp_ver, civetweb_civetweb_ver,
           ert_metrics_ver, ert_diametercodec_ver, google_test_ver

         Other: DBUILD_XTRA_OPTS (extra docker build options)

         Examples:
           $0 --image
           DBUILD_XTRA_OPTS=--no-cache $0 --image

EOF
}

resolve() {
  local var=$1
  local val="${!var}"
  [ -z "${val}" ] && val="$(eval echo \$${var}__dflt)"
  echo "${val}"
}

build_args() {
  local bargs=""
  bargs+=" --build-arg make_procs=$(resolve make_procs)"
  bargs+=" --build-arg build_type=$(resolve build_type)"
  bargs+=" --build-arg boost_ver=$(resolve boost_ver)"
  bargs+=" --build-arg ert_logger_ver=$(resolve ert_logger_ver)"
  bargs+=" --build-arg nlohmann_json_ver=$(resolve nlohmann_json_ver)"
  bargs+=" --build-arg pboettch_jsonschemavalidator_ver=$(resolve pboettch_jsonschemavalidator_ver)"
  bargs+=" --build-arg jupp0r_prometheuscpp_ver=$(resolve jupp0r_prometheuscpp_ver)"
  bargs+=" --build-arg civetweb_civetweb_ver=$(resolve civetweb_civetweb_ver)"
  bargs+=" --build-arg ert_metrics_ver=$(resolve ert_metrics_ver)"
  bargs+=" --build-arg ert_diametercodec_ver=$(resolve ert_diametercodec_ver)"
  bargs+=" --build-arg google_test_ver=$(resolve google_test_ver)"
  echo "${bargs}"
}

build_builder() {
  echo "=== Build diametercomm_builder (deps stage) ==="
  local tag=$(resolve image_tag)
  docker build --target deps \
    -t ${registry}/diametercomm_builder:${tag} \
    $(build_args) ${DBUILD_XTRA_OPTS} .
  echo "Built: ${registry}/diametercomm_builder:${tag}"
}

build_image() {
  echo "=== Build diametercomm_ut (unit-test image) ==="
  local tag=$(resolve image_tag)
  docker build --target unit-test \
    -t ${registry}/diametercomm_ut:${tag} \
    $(build_args) ${DBUILD_XTRA_OPTS} .
  echo "Built: ${registry}/diametercomm_ut:${tag}"
}

case "$1" in
  --builder) build_builder ;;
  --image)   build_image ;;
  -h|--help) usage; exit 0 ;;
  "")        build_image ;;
  *)         echo "Unknown: $1"; usage; exit 1 ;;
esac
