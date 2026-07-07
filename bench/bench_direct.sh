#!/bin/bash
TUNDRA_IMG="/tmp/tundra_direct.img"
EXT4_IMG="/tmp/ext4_direct.img"
MNT="/tmp/ext4_mnt"

echo "=== TUNDRAFS vs EXT4 (direct, no FUSE) ==="

rm -f $TUNDRA_IMG $EXT4_IMG
dd if=/dev/zero of=$TUNDRA_IMG bs=1M count=100 2>/dev/null
dd if=/dev/zero of=$EXT4_IMG bs=1M count=100 2>/dev/null

./mkfs_tundra $TUNDRA_IMG 25600 >/dev/null 2>&1
mkfs.ext4 -F $EXT4_IMG >/dev/null 2>&1
mkdir -p $MNT

echo ""
echo "1. Sequential write 100 files × 4KB"
sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
T0=$(date +%s%N)
for i in $(seq 1 100); do
    ./tundra_fs write $TUNDRA_IMG /f$i "data" >/dev/null 2>&1
done
T1=$(date +%s%N); TUNDRA_SEQ=$((($T1 - $T0) / 1000000))

mount -o loop $EXT4_IMG $MNT
sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
T0=$(date +%s%N)
for i in $(seq 1 100); do
    dd if=/dev/urandom of=$MNT/f$i bs=4096 count=1 2>/dev/null
done
T1=$(date +%s%N); EXT4_SEQ=$((($T1 - $T0) / 1000000))
umount $MNT
echo "  TundraFS: ${TUNDRA_SEQ}ms | ext4: ${EXT4_SEQ}ms"

echo ""
echo "2. Random read 50 files"
sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
T0=$(date +%s%N)
for i in $(seq 1 50); do
    ./tundra_fs read $TUNDRA_IMG /f$i >/dev/null 2>&1
done
T1=$(date +%s%N); TUNDRA_READ=$((($T1 - $T0) / 1000000))

mount -o loop $EXT4_IMG $MNT
sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
T0=$(date +%s%N)
for i in $(seq 1 50); do
    dd if=$MNT/f$i of=/dev/null bs=4096 count=1 2>/dev/null
done
T1=$(date +%s%N); EXT4_READ=$((($T1 - $T0) / 1000000))
umount $MNT
echo "  TundraFS: ${TUNDRA_READ}ms | ext4: ${EXT4_READ}ms"

echo ""
echo "3. Parallel write 4×25"
sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
T0=$(date +%s%N)
for t in 1 2 3 4; do
    (for i in $(seq 1 25); do ./tundra_fs write $TUNDRA_IMG /p${t}_$i "p" >/dev/null 2>&1; done) &
done
wait
T1=$(date +%s%N); TUNDRA_PAR=$((($T1 - $T0) / 1000000))

mount -o loop $EXT4_IMG $MNT
sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
T0=$(date +%s%N)
for t in 1 2 3 4; do
    (for i in $(seq 1 25); do dd if=/dev/urandom of=$MNT/p${t}_$i bs=4096 count=1 2>/dev/null; done) &
done
wait
T1=$(date +%s%N); EXT4_PAR=$((($T1 - $T0) / 1000000))
umount $MNT
echo "  TundraFS: ${TUNDRA_PAR}ms | ext4: ${EXT4_PAR}ms"

rm -f $TUNDRA_IMG $EXT4_IMG
echo "=== DONE ==="
