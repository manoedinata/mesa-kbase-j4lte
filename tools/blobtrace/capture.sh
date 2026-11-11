#!/bin/sh
# Capture a program run under the 32-bit Mali blob with blobspy, then decode.
#   tools/blobtrace/capture.sh <name> <device command...>
# Result: work/re/<name>/{subNNNN.bin,subNNNN.maps,ioctl.log,decode.txt,run.txt}
set -eu
TOP=$(cd "$(dirname "$0")/../.." && pwd)
NAME=$1; shift
D=/data/local/tmp/bs-$NAME
adb push "$TOP/out/blobtrace/libblobspy.so" /data/local/tmp/ >/dev/null
adb shell "rm -rf $D; mkdir -p $D"
adb shell "BLOBSPY_DIR=$D LD_PRELOAD=/data/local/tmp/libblobspy.so $*" > /dev/null 2>&1 || true
L=$TOP/work/re/$NAME
rm -rf "$L"; mkdir -p "$L"
adb pull "$D/." "$L/" >/dev/null
adb shell "$*" > "$L/run.txt" 2>&1 || true
"$TOP/out/blobtrace/blobdecode" "$L"/sub*.bin > "$L/decode.txt" 2>&1 || echo "decode failed for $NAME"
adb shell "rm -rf $D"
echo "$NAME: $(ls "$L"/sub*.bin | wc -l) submits, $(wc -l < "$L/decode.txt") decode lines, $(tail -1 "$L/run.txt")"
