#!/bin/sh
# Fetches the surface maps of the planets and moons from NASA 3D Resources
# (github.com/nasa/NASA-3D-Resources, "Images and Textures": free and without
# copyright) and makes them small: 1024 x 512, JPEG. The results are already in
# textures/; run this only to refresh them. Needs curl and ImageMagick.
#
# NASA has no map of Uranus (the planet shows no features) and none of the Sun:
# those two keep a plain colour.
set -e
cd "$(dirname "$0")"
mkdir -p textures
get() {  # get <our name> <NASA folder and file name>
  url=$(printf '%s' "$2" | sed 's/ /%20/g; s/(/%28/g; s/)/%29/g')
  curl -s -m 120 -A curl -L -o "textures/$1.orig.jpg" \
    "https://raw.githubusercontent.com/nasa/NASA-3D-Resources/master/Images%20and%20Textures/$url/$url.jpg"
  convert "textures/$1.orig.jpg" -resize '1024x512!' -strip -interlace none -quality 85 "textures/$1.jpg"
  rm "textures/$1.orig.jpg"
  printf "%-10s %s\n" "$1" "$(identify -format '%wx%h %b' "textures/$1.jpg")"
}
get jupiter  "Jupiter"
get io       "Jupiter - Io (A)"
get europa   "Jupiter - Europa"
get ganymede "Jupiter - Ganymede"
get callisto "Jupiter - Callisto"
get saturn   "Saturn"
get tethys   "Saturn - Tethys"
get dione    "Saturn - Dione"
get rhea     "Saturn - Rhea"
get titan    "Saturn - Titan"
get neptune  "Neptune"
get triton   "Neptune - Triton"
get ariel    "Uranus - Ariel"
get umbriel  "Uranus - Umbriel"
get titania  "Uranus - Titania"
get oberon   "Uranus - Oberon"
get miranda  "Uranus - Miranda"
