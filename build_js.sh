#!/usr/bin/env bash

# Propagate failures properly
set -e

BULLET=false

while [[ "$#" -gt 0 ]]; do
    case $1 in
        --bullet) BULLET=true ;;
        *) echo "Unknown parameter passed: $1"; exit 1 ;;
    esac
    shift
done
git submodule update --init --recursive

mkdir -p build_corrade-rc
pushd build_corrade-rc
cmake ../src \
    -DBUILD_GUI_VIEWERS=OFF \
    -DBUILD_PYTHON_BINDINGS=OFF \
    -DBUILD_ASSIMP_SUPPORT=OFF \
    -DBUILD_DATATOOL=OFF \
    -DBUILD_PTEX_SUPPORT=OFF
cmake --build . --target corrade-rc --
popd

mkdir -p build_js
cd build_js


EXE_LINKER_FLAGS="-s USE_WEBGL2=1"
cmake ../src \
    -DCORRADE_RC_EXECUTABLE=../build_corrade-rc/RelWithDebInfo/bin/corrade-rc \
    -DBUILD_GUI_VIEWERS=ON \
    -DBUILD_PYTHON_BINDINGS=OFF \
    -DBUILD_ASSIMP_SUPPORT=OFF \
    -DBUILD_DATATOOL=OFF \
    -DBUILD_PTEX_SUPPORT=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$EMSCRIPTEN" \
    -DCMAKE_TOOLCHAIN_FILE="../src/deps/corrade/toolchains/generic/Emscripten-wasm.cmake" \
    -DCMAKE_INSTALL_PREFIX="." \
    -DCMAKE_CXX_FLAGS="-s FORCE_FILESYSTEM=1 -s ALLOW_MEMORY_GROWTH=1" \
    -DCMAKE_EXE_LINKER_FLAGS="${EXE_LINKER_FLAGS}" \
    -DBUILD_WITH_BULLET="$( if ${BULLET} ; then echo ON ; else echo OFF; fi )"

cmake --build . -- -j 8 #TODO: Set to 8 cores only on CirelcCI
cmake --build . --target install -- -j 8

echo "Done building."
echo "Run:"
echo "python2 -m SimpleHTTPServer 8000"
echo "Or:"
echo "python3 -m http.server"
echo "Then open in a browser:"
echo "http://0.0.0.0:8000/build_js/esp/bindings_js/bindings.html?scene=skokloster-castle.glb"
echo "Or open in a VR-capable browser:"
echo "http://0.0.0.0:8000/build_js/esp/bindings_js/webvr.html?scene=skokloster-castle.glb"
