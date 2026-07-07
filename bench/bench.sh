#!/bin/bash
IMAGE="/tmp/disk_tundra_bench.img"
MNT="/tmp/tundra_bench"
DISK="/tmp/ext4_bench.img"
MNT2="/tmp/ext4_bench"

echo "=== TUNDRAFS vs EXT4 ==="

# Чистим старые монтирования
fusermount -u $MNT 2>/dev/null
umount $MNT2 2>/dev/null
pkill tundra_fuse 2>/dev/null
sleep 1

# Создаём образы
rm -f $IMAGE $DISK
dd if=/dev/zero of=$IMAGE bs=1M count=100 2>/dev/null
dd if=/dev/zero of=$DISK bs=1M count=100 2>/dev/null

# Форматируем
./mkfs_tundra $IMAGE 25600 >/dev/null 2>&1
mkfs.ext4 -F $DISK >/dev/null 2>&1

mkdir -p $MNT $MNT2

# Монтируем TundraFS
./tundra_fuse $IMAGE $MNT >/dev/null 2>&1 &
FPID=$!
for i in 1 2 3 4 5; do
    mountpoint -q $MNT && break
    sleep 1
done

# Монтируем ext4
mount -o loop $DISK $MNT2 2>/dev/null

echo ""
echo "1. Seq write 100×4KB"
sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
T0=$(date +%s%N)
for i in $(seq 1 100); do dd if=/dev/urandom of=$MNT/f$i bs=4096 count=1 2>/dev/null; done
T1=$(date +%s%N); TUNDRA_SEQ=$((($T1 - $T0) / 1000000))

sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
T0=$(date +%s%N)
for i in $(seq 1 100); do dd if=/dev/urandom of=$MNT2/f$i bs=4096 count=1 2>/dev/null; done
T1=$(date +%s%N); EXT4_SEQ=$((($T1 - $T0) / 1000000))
echo "  TundraFS: ${TUNDRA_SEQ}ms | ext4: ${EXT4_SEQ}ms"

echo ""
echo "2. Rand read 50×4KB"
sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
T0=$(date +%s%N)
for i in $(seq 1 50); do dd if=$MNT/f$i of=/dev/null bs=4096 count=1 2>/dev/null; done
T1=$(date +%s%N); TUNDRA_READ=$((($T1 - $T0) / 1000000))

sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
T0=$(date +%s%N)
for i in $(seq 1 50); do dd if=$MNT2/f$i of=/dev/null bs=4096 count=1 2>/dev/null; done
T1=$(date +%s%N); EXT4_READ=$((($T1 - $T0) / 1000000))
echo "  TundraFS: ${TUNDRA_READ}ms | ext4: ${EXT4_READ}ms"

echo ""
echo "3. Par write 4×25"
sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
T0=$(date +%s%N)
for t in 1 2 3 4; do (for i in $(seq 1 25); do dd if=/dev/urandom of=$MNT/p${t}_$i bs=4096 count=1 2>/dev/null; done)& done; wait
T1=$(date +%s%N); TUNDRA_PAR=$((($T1 - $T0) / 1000000))

sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
T0=$(date +%s%N)
for t in 1 2 3 4; do (for i in $(seq 1 25); do dd if=/dev/urandom of=$MNT2/p${t}_$i bs=4096 count=1 2>/dev/null; done)& done; wait
T1=$(date +%s%N); EXT4_PAR=$((($T1 - $T0) / 1000000))
echo "  TundraFS: ${TUNDRA_PAR}ms | ext4: ${EXT4_PAR}ms"

# Чистим
kill $FPID 2>/dev/null
fusermount -u $MNT 2>/dev/null
umount $MNT2 2>/dev/null
rm -f $IMAGE $DISK

echo ""
echo "=== DONE ==="
