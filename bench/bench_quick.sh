#!/bin/bash
TI="/tmp/tt.img"; EI="/tmp/ee.img"; M="/tmp/em"
rm -f $TI $EI
dd if=/dev/zero of=$TI bs=1M count=50 2>/dev/null
dd if=/dev/zero of=$EI bs=1M count=50 2>/dev/null
./mkfs_tundra $TI 12800 >/dev/null 2>&1
mkfs.ext4 -F $EI >/dev/null 2>&1
mkdir -p $M

echo "=== QUICK BENCH (20 files) ==="

# Seq write
T0=$(date +%s%N)
for i in $(seq 1 20); do ./tundra_fs write $TI /f$i "x" >/dev/null 2>&1; done
T1=$(date +%s%N); TS=$((($T1-$T0)/1000000))
mount -o loop $EI $M
T0=$(date +%s%N)
for i in $(seq 1 20); do dd if=/dev/urandom of=$M/f$i bs=4096 count=1 2>/dev/null; done
T1=$(date +%s%N); ES=$((($T1-$T0)/1000000))
umount $M
echo "Seq write: TundraFS=${TS}ms ext4=${ES}ms"

# Rand read
T0=$(date +%s%N)
for i in $(seq 1 20); do ./tundra_fs read $TI /f$i >/dev/null 2>&1; done
T1=$(date +%s%N); TR=$((($T1-$T0)/1000000))
mount -o loop $EI $M
T0=$(date +%s%N)
for i in $(seq 1 20); do dd if=$M/f$i of=/dev/null bs=4096 count=1 2>/dev/null; done
T1=$(date +%s%N); ER=$((($T1-$T0)/1000000))
umount $M
echo "Rand read: TundraFS=${TR}ms ext4=${ER}ms"

# Par write
T0=$(date +%s%N)
for t in 1 2 3 4; do (for i in $(seq 1 5); do ./tundra_fs write $TI /p${t}_$i "p" >/dev/null 2>&1; done) & done; wait
T1=$(date +%s%N); TP=$((($T1-$T0)/1000000))
mount -o loop $EI $M
T0=$(date +%s%N)
for t in 1 2 3 4; do (for i in $(seq 1 5); do dd if=/dev/urandom of=$M/p${t}_$i bs=4096 count=1 2>/dev/null; done) & done; wait
T1=$(date +%s%N); EP=$((($T1-$T0)/1000000))
umount $M
echo "Par write: TundraFS=${TP}ms ext4=${EP}ms"

rm -f $TI $EI
