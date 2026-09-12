#!/bin/bash
# Runs the jpeg2png tests against a built executable.
#
#   tests/run_tests.sh [./jpeg2png] [output-dir]
#
# Works in bash on Linux/macOS and in the MSYS2 shells on Windows.
set -euo pipefail

EXE=${1:-./jpeg2png}
OUT=${2:-test-results}
DIR=$(dirname "$0")/images
mkdir -p "$OUT"

echo "=== version"
"$EXE" --version || true

echo "=== JPEG input"
"$EXE" "$DIR/drawing_q40.jpg" -q -o "$OUT/from_jpeg.png"

echo "=== PNG input with a decoded JPEG inside (grid, quality and subsampling are estimated)"
"$EXE" "$DIR/drawing_q40_decoded.png" -q -o "$OUT/from_png.png" 2> "$OUT/from_png.log"
cat "$OUT/from_png.log"
grep -q "quality ~4[0-2]" "$OUT/from_png.log"
grep -q "grid at (0,0)" "$OUT/from_png.log"

echo "=== cropped PNG (the block grid is offset by 3,5 pixels)"
"$EXE" "$DIR/drawing_q40_cropped.png" -q -o "$OUT/from_cropped_png.png" 2> "$OUT/from_cropped_png.log"
cat "$OUT/from_cropped_png.log"
grep -q "grid at (5,3)" "$OUT/from_cropped_png.log"

echo "=== forced quality and grid"
"$EXE" "$DIR/drawing_q40_decoded.png" -q -J 40 --jpeg-grid 0,0 --jpeg-subsampling 420 -o "$OUT/from_png_forced.png"

echo "=== a clean PNG must be rejected"
if "$EXE" "$DIR/drawing.png" -q -o "$OUT/should_not_exist.png" 2> "$OUT/clean.log"; then
        echo "clean PNG was not rejected"; exit 1
fi
cat "$OUT/clean.log"
grep -q "no JPEG structure" "$OUT/clean.log"

echo "=== default output name for PNG input"
cp "$DIR/drawing_q40_decoded.png" "$OUT/named.png"
"$EXE" "$OUT/named.png" -q
test -f "$OUT/named.restored.png"

echo
echo "all tests passed; results in $OUT"
