#!/bin/sh
# Writes glyphs.hpp: the letters of the game's panels, as a picture of each
# printable ASCII character (space to ~), 9 by 18 pixels, from DejaVu Sans Mono
# (a free font, Bitstream Vera licence). One hex digit a pixel, 0 clear to f solid.
# The result is in the repository; run this only to change the font. Needs ImageMagick.
set -e
cd "$(dirname "$0")/.."
{
  echo "// Written by tools/font.sh: DejaVu Sans Mono, 14 px. Characters 32 to 126, each 9 wide and"
  echo "// 18 high, rows from the top, one hex digit a pixel (0 clear, f solid)."
  echo "constexpr int kGlyphW = 9, kGlyphH = 18;"
  echo "constexpr char kGlyphs[] ="
  i=32
  while [ $i -le 126 ]; do
    ch=$(printf "\\$(printf '%03o' $i)")
    case "$ch" in '%') txt='%%' ;; '\') txt='\\' ;; '@') txt='\@' ;; *) txt="$ch" ;; esac
    if [ $i -eq 32 ]; then
      bits=$(printf '%0162d' 0)
    else
      bits=$(convert -background black -fill white -font DejaVu-Sans-Mono -pointsize 14 "label:$txt" -gravity NorthWest -extent 9x18 -depth 8 gray:- |
             od -An -v -tu1 | awk '{ for (i = 1; i <= NF; i++) printf "%x", int(($i + 8) / 17) }')
    fi
    [ ${#bits} -eq 162 ] || { echo "character $i: ${#bits} pixels" >&2; exit 1; }
    echo "    \"$bits\""
    i=$((i + 1))
  done
  echo "    ;"
} > glyphs.hpp
echo "glyphs.hpp: $(grep -c '^    "' glyphs.hpp) characters"
