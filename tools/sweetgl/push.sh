#!/bin/sh
# Write out/sweetgl.dylib over /usr/lib/sweetgl.dylib on the phone (TWRP,
# adb): only the file's own blocks on sda35 (extents from delta11, see
# NOTES.md; user-approved for this file), then read back after dropping
# caches. Refuses unless the first extent starts with a Mach-O header.
set -e
H=$(cd "$(dirname "$0")" && pwd)
D=$H/../../research/phone-root/delta11
O=$H/out/sweetgl.dylib
T=${TMPDIR:-/tmp}/sweetgl-push.$$
[ "$(stat -f %z "$O")" = 67108864 ] || { echo "$O is not 64 MiB"; exit 1; }
[ "$(awk '{n += $2} END {print n}' "$D/dylib-extents.txt")" = 16384 ] || { echo "extents do not cover 64 MiB"; exit 1; }
FIRST=$(head -1 "$D/dylib-extents.txt" | cut -d' ' -f1)
adb push "$D/dylib-extents.txt" "$D/../delta10/readruns.sh" "$D/../delta10/writeruns.sh" "$O" /tmp/ >/dev/null
adb shell "dd if=/dev/block/sda35 bs=4096 skip=$FIRST count=1 2>/dev/null | head -c 4" > $T.magic
[ "$(xxd -p $T.magic)" = cffaedfe ] || { echo "extent does not start with a Mach-O header: $(xxd -p $T.magic)"; exit 1; }
adb shell 'sh /tmp/writeruns.sh /tmp/dylib-extents.txt /tmp/sweetgl.dylib && echo 3 > /proc/sys/vm/drop_caches && sh /tmp/readruns.sh /tmp/dylib-extents.txt /tmp/back && rm /tmp/sweetgl.dylib'
adb pull /tmp/back $T.back >/dev/null
adb shell rm /tmp/back
cmp $T.back "$O" && echo "VERIFY OK: $(shasum -a 256 "$O" | cut -c1-16)"
rm -f $T.magic $T.back
