#!/bin/sh
#
# ENTRYPOINT BUILDER SCRIPT
#
BUILD_TYPE=${BUILD_TYPE:-Release}
MAKE_PROCS=${MAKE_PROCS:-$(grep processor /proc/cpuinfo -c)}

[ -f CMakeLists.txt ] && cmake -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" "$1" . && [ -n "$1" ] && shift
make -j"${MAKE_PROCS}" $@

exit $?
