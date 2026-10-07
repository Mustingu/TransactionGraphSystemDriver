#!/bin/bash
# Build vendored Teseo out of source: systems/Teseo/build/libteseo.a.
# After editing systems/Teseo, rerun this incremental build script and relink bench_run:
#   ./build_teseo.sh && cd build && make -j$(nproc)
set -e
cd "$(dirname "$0")/systems/Teseo"
mkdir -p build
cd build
# --enable-optimize disables assertions through configure's enable_assert=auto,
# avoiding assertion overhead in storage scans; --disable-debug disables debug mode.
if [ ! -f Makefile ]; then
    echo ">>> Configuring Teseo (../configure --enable-optimize --disable-debug)"
    ../configure --enable-optimize --disable-debug
fi
make -j"$(nproc)"
echo ">>> Teseo build complete: $(pwd)/libteseo.a"
