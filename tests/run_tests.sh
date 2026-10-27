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

echo "=== numeric argument validation"
# Stop at a missing input file to test valid limits without running 10000
# optimization steps or starting 256 threads.
CHECKS=0
expect_error() {
        local expected=$1
        shift
        if "$EXE" "$OUT/nonexistent-input/validation.jpg" -q "$@" > "$OUT/validation.log" 2>&1; then
                echo "unexpected success: $*"; exit 1
        fi
        if ! grep -Fq "$expected" "$OUT/validation.log"; then
                echo "unexpected diagnostic for: $*"
                cat "$OUT/validation.log"
                exit 1
        fi
        CHECKS=$((CHECKS + 1))
}
expect_valid() {
        expect_error 'could not open input file' "$@"
}

for flag in --second-order-weight --probability-weight; do
        for value in '' bad nan NaN 'nan(1)' inf -inf 1e1000 -0.1 \
                '0.1,' '0.1,0.2' ',0.1,0.2' '0.1,,0.2' \
                '0.1,0.2,0.3,' '0.1,0.2,0.3,0.4' '0.1junk' \
                '0.1,0.2,0.3junk' '0.1,nan,0.1' '0.1,0.1,inf'; do
                expect_error weight -s "$flag=$value"
        done
        for value in 0 1 1e-3 '0,0,0' '0,1,0' ' 0.1 , 0.2 , 0.3 '; do
                expect_valid -s "$flag=$value"
        done
done
for value in 10.1 '10.1,0,0' '0,10.1,0' '0,0,10.1'; do
        expect_error weight -s -w "$value"
done
expect_valid -w 10
expect_valid -s -w 10,0,10
for value in 1.1 '1.1,0,0' '0,1.1,0' '0,0,1.1'; do
        expect_error 'probability weight' -p "$value"
done

for value in '' bad 0 -1 10001 4294967297 -4294967295 \
        -18446744073709551615 18446744073709551617 1.5 1e2 \
        '1,' '1,2' '1,,2' '1,2,3,' '1,2,3,4' '1junk' '1,2,3junk' \
        '1,0,1' '1,1,10001'; do
        expect_error 'iteration count' -s --iterations="$value"
done
for value in 1 10000 '1,2,10000' ' 1 , 2 , 3 '; do
        expect_valid -s -i "$value"
done
expect_valid --second-order-weight 0.3 --probability-weight 0.001 --iterations 50
expect_error 'separated components' -w 0.3,0.1,0.1
expect_error 'separated components' -i 50,30,30
expect_valid -p 0.001,0.002,0.001

"$EXE" --help > "$OUT/help.log" || true
if grep -q 'compiled without support for threads' "$OUT/help.log"; then
        expect_error 'compiled without support for threads' -t 1
else
        for value in '' bad 0 -1 257 4294967297 -4294967295 \
                -18446744073709551615 18446744073709551617 1.5 1e2 \
                '1,' '1,2,3' '1junk'; do
                expect_error 'thread count' --threads="$value"
        done
        expect_valid -t 1
        expect_valid --threads 256
fi
echo "$CHECKS numeric argument checks passed"

echo "=== single-value component defaults"
"$EXE" "$DIR/drawing_q40.jpg" -q -s -i 1 -w 0.3 -p 0.001 -o "$OUT/single_values.png"
"$EXE" "$DIR/drawing_q40.jpg" -q -s -i 1,1,1 -w 0.3,0,0 -p 0.001,0.001,0.001 -o "$OUT/triplet_values.png"
cmp "$OUT/single_values.png" "$OUT/triplet_values.png"

echo "=== case-insensitive JPEG and PNG output names"
INDEX=0
for extension in jpg JPG JpG jpeg JPEG JpEg; do
        INDEX=$((INDEX + 1))
        input="$OUT/jpeg_name_$INDEX.$extension"
        cp "$DIR/drawing_q40.jpg" "$input"
        "$EXE" "$input" -q -i 1
        test -s "$OUT/jpeg_name_$INDEX.png"
        cmp "$DIR/drawing_q40.jpg" "$input"
done
for extension in png PNG PnG; do
        INDEX=$((INDEX + 1))
        input="$OUT/png_name_$INDEX.$extension"
        cp "$DIR/drawing_q40_decoded.png" "$input"
        "$EXE" "$input" -q -i 1 -J 40 --jpeg-grid 0,0 --jpeg-subsampling 420
        test -s "$OUT/png_name_$INDEX.restored.png"
        cmp "$DIR/drawing_q40_decoded.png" "$input"
done

echo
echo "all tests passed; results in $OUT"
