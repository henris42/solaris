#!/bin/sh
# Fetches the surface maps and makes them ready. The results are already in
# textures/; run this only to refresh them. Needs curl and ImageMagick.
#
# Planets and Sun as the eye would see them (visible light): Solar System Scope
# (solarsystemscope.com/textures), licence CC BY 4.0, made from NASA data.
# Stars: NASA Scientific Visualization Studio, Deep Star Maps 2020
# (svs.gsfc.nasa.gov/4851): the real sky from the Hipparcos, Tycho and Gaia
# catalogues, in celestial coordinates, centred at 0 h right ascension, which
# increases to the left.
# Moons: NASA 3D Resources (github.com/nasa/NASA-3D-Resources, "Images and
# Textures"), which NASA describes as free and without copyright.
set -e
cd "$(dirname "$0")"
mkdir -p textures
sss() {  # sss <our name> <their file> [size]
  curl -s -m 180 -A "Mozilla/5.0" -L -o "textures/$1.orig" "https://www.solarsystemscope.com/textures/download/$2"
  case "$2" in
    *.png) convert "textures/$1.orig" -strip "textures/$1.png" ;;
    *) convert "textures/$1.orig" -resize "${3:-2048x1024}!" -strip -interlace none -quality 88 "textures/$1.jpg" ;;
  esac
  rm "textures/$1.orig"
  printf "%-10s %s\n" "$1" "$(identify -format '%wx%h %b' textures/$1.*)"
}
sss jupiter 2k_jupiter.jpg
sss saturn  2k_saturn.jpg
sss uranus  2k_uranus.jpg
sss neptune 2k_neptune.jpg
sss sun     2k_sun.jpg 1024x512
sss ring    2k_saturn_ring_alpha.png

get() {  # get <our name> <NASA folder and file name>
  url=$(printf '%s' "$2" | sed 's/ /%20/g; s/(/%28/g; s/)/%29/g')
  curl -s -m 120 -A curl -L -o "textures/$1.orig.jpg" \
    "https://raw.githubusercontent.com/nasa/NASA-3D-Resources/master/Images%20and%20Textures/$url/$url.jpg"
  convert "textures/$1.orig.jpg" -resize '1024x512!' -strip -interlace none -quality 85 "textures/$1.jpg"
  rm "textures/$1.orig.jpg"
  printf "%-10s %s\n" "$1" "$(identify -format '%wx%h %b' "textures/$1.jpg")"
}
get io       "Jupiter - Io (A)"
get europa   "Jupiter - Europa"
get ganymede "Jupiter - Ganymede"
get callisto "Jupiter - Callisto"
get tethys   "Saturn - Tethys"
get dione    "Saturn - Dione"
get rhea     "Saturn - Rhea"
get titan    "Saturn - Titan"
get triton   "Neptune - Triton"
get ariel    "Uranus - Ariel"
get umbriel  "Uranus - Umbriel"
get titania  "Uranus - Titania"
get oberon   "Uranus - Oberon"
get miranda  "Uranus - Miranda"

# sunlight on a ball: black, clear where the Sun is overhead and darkening to the night
# side (the brightness of a matt ball: the cosine of the Sun's distance from overhead)
convert -size 4x256 xc:black -alpha set -channel A -fx "j<h/2 ? 1-pow(cos(pi*j/h),0.75) : 1" +channel textures/shade.png

# the sky: NASA's star map comes with the full range of brightness (EXR); brought to 8 bits here
curl -s -m 900 -A "Mozilla/5.0" -L -o textures/stars.exr "https://svs.gsfc.nasa.gov/vis/a000000/a004800/a004851/starmap_2020_4k.exr"
convert textures/stars.exr -evaluate multiply 2.0 -gamma 1.6 -depth 8 -strip -interlace none -quality 86 textures/stars.jpg
rm textures/stars.exr
printf "%-10s %s\n" stars "$(identify -format '%wx%h %b' textures/stars.jpg)"
