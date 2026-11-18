#!/bin/sh
# Builds the game for a browser (WebAssembly) into out/web.
#   ./web.sh
#   python3 tools/serve.py . 8791     then open http://127.0.0.1:8791/out/web/?planet=neptune
# Needs Emscripten (em++), ./run.sh run once, and GameApi built for the browser:
#   (cd GameApi/GameApi && make -f Makefile.LinuxEm -j 8 EMSCRIPTEN_PATH=<your emsdk>/)
# GAMEAPI_WASM names another folder with that build.
# The scenes, the maps and Voyager's pictures are packed into the page (game.data);
# the models are fetched from out/ and models/probe/ as the page runs.
set -e
cd "$(dirname "$0")"
V=$(pwd)
G=${GAMEAPI_WASM:-$V/GameApi/GameApi}
[ -f "$G/libGameApi.bc" ] || { echo "build GameApi for the browser first (see the top of web.sh)" >&2; exit 1; }
[ -f out/neptune.scene ] || ./run.sh
S=out/web/files
mkdir -p $S/out $S/textures $S/data $S/models/probe
cp out/*.scene out/star-dots.png $S/out/
cp textures/*.jpg textures/*.png $S/textures/
cp data/shots_*.txt $S/data/
cp models/probe/*.jpg $S/models/probe/
cp web/index.html out/web/
# GameApi's own link line for its browser program, with the game in its place
cd "$G/web_page"
line=$(make -n -W test.cpp -f Makefile.LinuxEm pthreads_both_highmem | grep -m1 'em++')
line=$(printf '%s' "$line" | sed "s#web_page/test.cpp#$V/game.cpp -I$V -I$V/orbit --preload-file $V/$S@/#; s#-std=c++11#-std=c++20#; s#-o web_page/engine_highmem.js#-o $V/out/web/game.js#")
eval "$line"
ls -l "$V/out/web/game.js" "$V/out/web/game.wasm" "$V/out/web/game.data"
