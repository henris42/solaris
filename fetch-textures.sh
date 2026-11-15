#!/bin/sh
# Fetches the surface maps and makes them ready. The results are already in
# textures/; run this only to refresh them. Needs curl and ImageMagick.
#
# Planets as the eye would see them (visible light): Solar System Scope
# (solarsystemscope.com/textures), licence CC BY 4.0, made from NASA data.
# The Sun has no map (it is white), and the stars are not a picture: they come
# from a catalogue (fetch.sh, data/stars.txt).
# Moons: see below, each with its source.
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

# ---- the moons ----
# Where newer maps than Voyager's exist they are used; what nobody has seen is
# made up to look like what has been seen (tools/fill.cpp: fractal ground and
# craters with the seen surface's brightness, colour and roughness). The made-up
# parts are a guess, not a map. The maps are 2048 by 1024 (Io 4096 by 2048).
${CXX:-c++} -std=c++20 -O2 tools/fill.cpp -o textures/fill
fill() {  # fill <name> <picture> <craters 0..1> <seed> [<from> <to>]: the unseen parts made up, written as textures/<name>.jpg
  convert "$2" -colorspace sRGB -type TrueColor ppm:- | textures/fill "$3" "$4" ${5:+"$5"} ${6:+"$6"} | convert ppm:- -strip -interlace none -quality 88 "textures/$1.jpg"
  rm "$2"
  printf "%-10s %s\n" "$1" "$(identify -format '%wx%h %b' "textures/$1.jpg")"
}
usgs() {  # usgs <name> <file at the U.S. Geological Survey>: a global mosaic (public domain), 100 to 300 MB each
  curl -s -m 1800 -A "Mozilla/5.0" -L -o "textures/$1.tif" "https://planetarymaps.usgs.gov/mosaic/$2"
  convert "textures/$1.tif" -resize "${3:-2048x1024}!" -colorspace sRGB -type TrueColor -strip "textures/$1.png" 2>/dev/null
  rm "textures/$1.tif"
}
# Jupiter's moons: Galileo and Voyager mosaics. Io and Ganymede are in colour (Io's
# from Galileo's violet, green and near-infrared filters: to the eye Io is the same,
# but paler). Europa and Callisto are in grey and get one tint each: Europa's
# cream, Callisto's brown-grey. Their unseen parts are narrow strips at the poles.
usgs io Io_GalileoSSI-Voyager_Global_Mosaic_ClrMerge_1km.tif 4096x2048
convert textures/io.png -strip -interlace none -quality 90 textures/io.jpg && rm textures/io.png
printf "%-10s %s\n" io "$(identify -format '%wx%h %b' textures/io.jpg)"
usgs europa Europa_Voyager_GalileoSSI_global_mosaic_500m.tif
convert textures/europa.png -colorspace sRGB -type TrueColor -channel G -evaluate multiply 0.95 -channel B -evaluate multiply 0.86 +channel -define png:color-type=2 textures/europa.png
fill europa textures/europa.png 0.0 11
usgs ganymede Ganymede_Voyager_GalileoSSI_Global_ClrMosaic_1435m.tif
fill ganymede textures/ganymede.png 0.5 12
usgs callisto Callisto_Voyager_GalileoSSI_global_mosaic_1km.tif
convert textures/callisto.png -colorspace sRGB -type TrueColor -channel G -evaluate multiply 0.92 -channel B -evaluate multiply 0.80 +channel -define png:color-type=2 textures/callisto.png
fill callisto textures/callisto.png 1.0 13
# Triton: Voyager 2's colour mosaic; the northern two fifths were in the dark
usgs triton Triton_Voyager2_ClrMosaic_GlobalFill_600m.tif
fill triton textures/triton.png 0.15 3 0.3 0.97

# Saturn's moons: the Cassini maps of 2014 (NASA/JPL-Caltech/Space Science Institute/
# Lunar and Planetary Institute, PIA18434 to PIA18439), complete. Their colours are
# stretched (infrared, green, ultraviolet): brought down to the near white of ice
# in visible light, and brightened, since these are the brightest surfaces there are. Turned half way round, to have longitude 0 in the middle like
# the other maps.
pia() {  # pia <name> <number>
  curl -s -m 600 -A "Mozilla/5.0" -L -o "textures/$1.orig.jpg" "https://assets.science.nasa.gov/content/dam/science/psd/photojournal/pia/pia18/pia$2/PIA$2.jpg"
  convert "textures/$1.orig.jpg" -resize '2048x1024!' -roll +1024+0 -modulate 100,30,100 -level 0%,80%,1.3 -strip -interlace none -quality 88 "textures/$1.jpg"
  rm "textures/$1.orig.jpg"
  printf "%-10s %s\n" "$1" "$(identify -format '%wx%h %b' "textures/$1.jpg")"
}
pia dione 18434
pia enceladus 18435
pia iapetus 18436
pia mimas 18437
pia rhea 18438
pia tethys 18439

# Titan and Uranus's moons: NASA 3D Resources (github.com/nasa/NASA-3D-Resources,
# "Images and Textures"), which NASA describes as free and without copyright.
get() {  # get <our name> <NASA folder and file name>: fetched as textures/<name>.png
  url=$(printf '%s' "$2" | sed 's/ /%20/g; s/(/%28/g; s/)/%29/g')
  curl -s -m 120 -A curl -L -o "textures/$1.orig.jpg" \
    "https://raw.githubusercontent.com/nasa/NASA-3D-Resources/master/Images%20and%20Textures/$url/$url.jpg"
  convert "textures/$1.orig.jpg" -resize '1024x512!' -strip "textures/$1.png"
  rm "textures/$1.orig.jpg"
}
# NASA's Titan map has the right shading (the darker northern hood of 1981) in a
# far too red orange: brought to the tan-orange of Titan's haze in visible light
get titan "Saturn - Titan"
convert textures/titan.png -modulate 112,50,107 -strip -interlace none -quality 90 textures/titan.jpg && rm textures/titan.png
printf "%-10s %s\n" titan "$(identify -format '%wx%h %b' textures/titan.jpg)"
# Voyager 2 saw only the southern halves of Uranus's moons: their north was in the dark
get miranda "Uranus - Miranda"; fill miranda textures/miranda.png 0.6 21
get ariel   "Uranus - Ariel";   fill ariel   textures/ariel.png   0.6 22
get umbriel "Uranus - Umbriel"; fill umbriel textures/umbriel.png 1.0 23
get titania "Uranus - Titania"; fill titania textures/titania.png 0.8 24
get oberon  "Uranus - Oberon";  fill oberon  textures/oberon.png  1.0 25
rm textures/fill

# sunlight on a ball: black, clear where the Sun is overhead and darkening to the night
# side (the brightness of a matt ball: the cosine of the Sun's distance from overhead).
# The night side keeps a fifth of its brightness, so that it can still be seen.
convert -size 4x256 xc:black -alpha set -channel A -fx "0.8*(j<h/2 ? 1-pow(cos(pi*j/h),0.75) : 1)" +channel textures/shade.png
