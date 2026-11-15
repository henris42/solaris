#!/bin/sh
# Flies along with Voyager 2 past a planet, in a GameApi window.
#   ./play.sh [jupiter|saturn|uranus|neptune] [hours from the nearest approach, default -1] [speed, default 600]
# Needs GameApi built in GameApi/GameApi (see README) and ./run.sh run once.
set -e
cd "$(dirname "$0")"
G=GameApi/GameApi
[ -f $G/libGameApi.so ] || { echo "build GameApi first: (cd $G && make -j 8 depbuild)" >&2; exit 1; }
[ -f out/neptune.scene ] || ./run.sh
if [ ! -x out/game ] || [ game.cpp -nt out/game ]; then
  ${CXX:-c++} -std=c++20 -O2 game.cpp -o out/game -I$G -Iorbit -L$G -lGameApi -lGL
fi
# GameApi asks for an antialiased window; on some X servers that exists only through EGL
SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-x11} SDL_VIDEO_X11_FORCE_EGL=${SDL_VIDEO_X11_FORCE_EGL:-1} LD_LIBRARY_PATH=$G exec out/game "${1:-neptune}" "$(pwd)" ${2:+"$2"} ${3:+"$3"}
