#!/bin/bash
# =============================================================================
# diametercomm source code format (clang-format)
# =============================================================================
# Checks (or applies) the project C++ code style, using the SAME clang-format
# image as the CI 'format_style' job, so a local pass guarantees the CI passes
# (avoids clang-format version drift between your machine and CI).
#
#   ./format.sh            Check formatting (dry-run -Werror). Exit != 0 on any
#                          violation -- exactly what CI does before building.
#   ./format.sh --fix      Reformat sources in place (clang-format -i).
#   ./format.sh -h|--help  This help.
# =============================================================================
set -e

SCR="$(readlink -f "$0")"
SCR_DIR="$(dirname "${SCR}")"
cd "${SCR_DIR}"

# Same image as .github/workflows/ci.yml (pin via CLANG_FORMAT_IMAGE if needed).
CLANG_FORMAT_IMAGE=${CLANG_FORMAT_IMAGE:-ghcr.io/testillano/clang-format:latest}

usage() {
  cat << EOF

  Usage: $0 [--fix|-h|--help]

         (no args):  check formatting (dry-run -Werror), like the CI job.
         --fix:      reformat sources in place (clang-format -i).

         Uses the CI clang-format Docker image (${CLANG_FORMAT_IMAGE}) so the
         result matches CI regardless of any locally installed clang-format
         version. Override with CLANG_FORMAT_IMAGE=<image>.

EOF
}

case "$1" in
  -h|--help) usage; exit 0 ;;
esac

# Source selection mirrors the CI job (all .hpp/.cpp). 'build' is excluded to
# avoid formatting generated/vendored trees.
mapfile -t sources < <(find . -path ./build -prune -o \( -name "*.hpp" -o -name "*.cpp" \) -print)

if [ ${#sources[@]} -eq 0 ]; then
  echo "No C++ sources found."
  exit 0
fi

if [ "$1" = "--fix" ]; then
  echo "Reformatting ${#sources[@]} file(s) in place with ${CLANG_FORMAT_IMAGE} ..."
  docker run --rm -v "${SCR_DIR}":/data "${CLANG_FORMAT_IMAGE}" -i "${sources[@]}"
  echo "Done."
else
  echo "Checking format of ${#sources[@]} file(s) with ${CLANG_FORMAT_IMAGE} ..."
  if docker run --rm -v "${SCR_DIR}":/data "${CLANG_FORMAT_IMAGE}" --dry-run -Werror "${sources[@]}"; then
    echo "Format OK."
  else
    echo
    echo "Format violations found. Run '$0 --fix' to fix them."
    exit 1
  fi
fi
