#!/bin/sh
# Builds the game for a browser (WebAssembly) into web/, which then holds everything the page
# needs and can be copied to any web server as it is.
#   ./web.sh
#   python3 tools/serve.py web 8791     then open http://127.0.0.1:8791/?planet=neptune
# Needs Emscripten (em++), ./run.sh run once, and GameApi built for the browser:
#   (cd GameApi/GameApi && make -f Makefile.LinuxEm -j 8 EMSCRIPTEN_PATH=<your emsdk>/)
# GAMEAPI_WASM names another folder with that build.
# The scenes, the maps and Voyager's pictures are packed into the page (game.data);
# the models are copied to web/out and web/models/probe and fetched as the page runs.
set -e
cd "$(dirname "$0")"
V=$(pwd)
G=${GAMEAPI_WASM:-$V/GameApi/GameApi}
[ -f "$G/libGameApi.bc" ] || { echo "build GameApi for the browser first (see the top of web.sh)" >&2; exit 1; }
[ -f out/neptune.scene ] || ./run.sh
S=out/web-files
mkdir -p $S/out $S/textures $S/data $S/models/probe web/out web/models/probe
cp out/*.scene out/star-dots.png $S/out/
cp textures/*.jpg textures/*.png $S/textures/
cp data/shots_*.txt $S/data/
cp models/probe/*.jpg $S/models/probe/
cp out/ball-*.glb out/ring.glb out/stars-*.glb web/out/
cp models/probe/*.glb web/models/probe/
# GameApi's own link line for its browser program, with the game in its place
cd "$G/web_page"
line=$(make -n -W test.cpp -f Makefile.LinuxEm pthreads_both_highmem | grep -m1 'em++')
line=$(printf '%s' "$line" | sed "s#web_page/test.cpp#$V/game.cpp -I$V -I$V/orbit --preload-file $V/$S@/#; s#-std=c++11#-std=c++20#; s#-o web_page/engine_highmem.js#-o $V/web/game.js#")
eval "$line"
cd "$V"
du -sh web
ls -l web/game.js web/game.wasm web/game.data
