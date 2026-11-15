#!/bin/sh
# Fetches the surface maps and makes them ready. The results are already in
# textures/; run this only to refresh them. Needs curl and ImageMagick.
#
# Planets as the eye would see them (visible light): Solar System Scope
# (solarsystemscope.com/textures), licence CC BY 4.0, made from NASA data.
# The Sun has no map (it is white), and the stars are not a picture: they come
# from a catalogue (fetch.sh, data/stars.txt).
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
sss ring    2k_saturn_ring_alpha.png

get() {  # get <our name> <NASA folder and file name>
  url=$(printf '%s' "$2" | sed 's/ /%20/g; s/(/%28/g; s/)/%29/g')
  curl -s -m 120 -A curl -L -o "textures/$1.orig.jpg" \
    "https://raw.githubusercontent.com/nasa/NASA-3D-Resources/master/Images%20and%20Textures/$url/$url.jpg"
  convert "textures/$1.orig.jpg" -resize '1024x512!' -strip -interlace none -quality 85 "textures/$1.jpg"
  rm "textures/$1.orig.jpg"
  printf "%-10s %s\n" "$1" "$(identify -format '%wx%h %b' "textures/$1.jpg")"
}
# Io: the U.S. Geological Survey's global mosaic (Galileo colour over Galileo and
# Voyager detail, 1 km a pixel, public domain; 190 MB to fetch). Its colours are
# from Galileo's violet, green and near-infrared filters: to the eye Io is the
# same, but paler.
curl -s -m 900 -A "Mozilla/5.0" -L -o textures/io.tif "https://planetarymaps.usgs.gov/mosaic/Io_GalileoSSI-Voyager_Global_Mosaic_ClrMerge_1km.tif"
convert textures/io.tif -resize '4096x2048!' -colorspace sRGB -type TrueColor -strip -interlace none -quality 90 textures/io.jpg 2>/dev/null
rm textures/io.tif
printf "%-10s %s\n" io "$(identify -format '%wx%h %b' textures/io.jpg)"
get europa   "Jupiter - Europa"
get ganymede "Jupiter - Ganymede"
get callisto "Jupiter - Callisto"
get tethys   "Saturn - Tethys"
get dione    "Saturn - Dione"
get rhea     "Saturn - Rhea"
get titan    "Saturn - Titan"
# NASA's Titan map has the right shading (the darker northern hood of 1981) in a
# far too red orange: brought to the tan-orange of Titan's haze in visible light
convert textures/titan.jpg -modulate 112,50,107 -strip -interlace none -quality 90 textures/titan.jpg
get mimas    "Saturn - Mimas"
get enceladus "Saturn - Enceladus"
get iapetus  "Saturn - Iapetus"
get triton   "Neptune - Triton"
get ariel    "Uranus - Ariel"
get umbriel  "Uranus - Umbriel"
get titania  "Uranus - Titania"
get oberon   "Uranus - Oberon"
get miranda  "Uranus - Miranda"

# sunlight on a ball: black, clear where the Sun is overhead and darkening to the night
# side (the brightness of a matt ball: the cosine of the Sun's distance from overhead).
# The night side keeps a fifth of its brightness, so that it can still be seen.
convert -size 4x256 xc:black -alpha set -channel A -fx "0.8*(j<h/2 ? 1-pow(cos(pi*j/h),0.75) : 1)" +channel textures/shade.png
