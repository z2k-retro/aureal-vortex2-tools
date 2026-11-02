#!/usr/bin/env bash
# Prepare and read back the media for an a3dtest session.
#   ./a3dtest-media.sh iso        build ~/a3dtest/a3dtest.iso containing A3DTEST.EXE
#   ./a3dtest-media.sh disk       create an empty 64 MB FAT16 results disk ~/a3dtest/results.img
#   ./a3dtest-media.sh log        print A3DTEST.LOG from the results disk
#   ./a3dtest-media.sh clear      delete A3DTEST.LOG from the results disk (before a fresh run)
# Needs: genisoimage, dosfstools, mtools, util-linux (sudo apt install genisoimage dosfstools mtools)
set -e
export MTOOLS_SKIP_CHECK=1   # Windows/mkfs geometry mismatch makes mtools refuse the disk otherwise
DIR=${A3DTEST_DIR:-$HOME/a3dtest}
EXE=${EXE:-$(dirname "$0")/a3dtest.exe}
IMG=$DIR/results.img
mkdir -p "$DIR"
case "${1:-}" in
  iso)
    stage=$(mktemp -d); cp "$EXE" "$stage/A3DTEST.EXE"
    printf 'Run D: or E:\\A3DTEST.EXE (whichever is the CD).\r\nNo arguments runs every test (about 6 minutes).\r\nThe log goes to the results disk as A3DTEST.LOG.\r\n' > "$stage/README.TXT"
    genisoimage -quiet -o "$DIR/a3dtest.iso" -J -r -V A3DTEST "$stage"; rm -rf "$stage"
    echo "built $DIR/a3dtest.iso" ;;
  disk)
    [ -e "$IMG" ] && { echo "$IMG exists; delete it first to recreate"; exit 1; }
    truncate -s 64M "$IMG"
    echo 'start=2048, type=06, bootable' | sfdisk -q "$IMG"
    mkfs.fat -F 16 -n RESULTS -h 2048 --offset 2048 "$IMG" $(( (64*1024*1024/512 - 2048) / 2 )) >/dev/null
    echo "created $IMG (FAT16, one partition)" ;;
  log)   mtype -i "$IMG@@1M" ::A3DTEST.LOG ;;
  clear) mdel -i "$IMG@@1M" ::A3DTEST.LOG 2>/dev/null || true; echo "cleared" ;;
  *) sed -n '2,8p' "$0"; exit 1 ;;
esac
