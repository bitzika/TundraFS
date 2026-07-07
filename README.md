# TundraFS - Next Generation File System

## Build Status
- Linux: FUSE ✅
- Windows: Dokany (in progress)
- macOS: macFUSE (in progress)

## Quick Start
```bash
# Create disk
./mkfs_tundra disk.tundra 1000000

# Write files
./tundra_fs write disk.tundra /hello.txt "Hello World"

# Read files
./tundra_fs read disk.tundra /hello.txt

# Mount
mkdir /tmp/tundra
./tundra_fuse disk.tundra /tmp/tundra
ls /tmp/tundra

# Check
./tundra_fsck disk.tundra
