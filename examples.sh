#!/bin/bash

# Run echo_server or ping_client examples via Docker.
#
# Usage:
#   ./examples.sh echo_server [bind_address] [port]
#   ./examples.sh ping_client [host] [port] [command_code]
#
# Examples:
#   Terminal 1: ./examples.sh echo_server 0.0.0.0 3868
#   Terminal 2: ./examples.sh ping_client 127.0.0.1 3868 272

DIAMETERCOMM_EXAMPLES_IMAGE=${DIAMETERCOMM_EXAMPLES_IMAGE:-ghcr.io/testillano/diametercomm_examples:latest}

# Build examples image if not available:
if ! docker image inspect ${DIAMETERCOMM_EXAMPLES_IMAGE} &>/dev/null; then
  echo "Building examples image..."
  docker build --target examples -t ${DIAMETERCOMM_EXAMPLES_IMAGE} .
fi

binary=$1
shift 2>/dev/null

if [ -z "${binary}" ]; then
  echo "Usage: $0 <echo_server|ping_client> [args...]"
  echo ""
  echo "  echo_server [bind_address] [port]          (default: 0.0.0.0 3868)"
  echo "  ping_client [host] [port] [command_code]   (default: 127.0.0.1 3868 999)"
  exit 1
fi

docker run --rm -it --network host --entrypoint /opt/${binary} ${DIAMETERCOMM_EXAMPLES_IMAGE} "$@"
