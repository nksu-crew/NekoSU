#!/system/bin/sh
set -u

SRC_IMG="${1:-}"
KO_SRC="${2:-}"

OUT_NAME="vendor_boot_nksu.img"
KO_NAME="nksu.ko"

SCRIPT_DIR="$(cd "$(dirname "$0")" 2>/dev/null && pwd)"
[ -n "$SCRIPT_DIR" ] || SCRIPT_DIR="$(pwd)"

NCORE="${NKSU_NCORE:-}"

TOYBOX="${NKSU_TOYBOX:-}"
if [ -z "$TOYBOX" ]; then
  if [ -x /system/bin/toybox ]; then
    TOYBOX=/system/bin/toybox
  else
    TOYBOX="$(command -v toybox 2>/dev/null || true)"
  fi
fi

WORK="${NKSU_WORK:-$SCRIPT_DIR/work.$$}"

log()  { printf '[nksu] %s\n' "$*"; }
warn() { printf '[nksu] WARN: %s\n' "$*" >&2; }
die()  { printf '[nksu] ERROR: %s\n' "$*" >&2; exit 1; }

cleanup() {
  [ -n "${WORK:-}" ] && [ -d "$WORK" ] && rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

[ -n "$SRC_IMG" ] || die "usage: $0 <vendor_boot.img> [nksu.ko]"
[ -f "$SRC_IMG" ] || die "can't find vendor_boot: $SRC_IMG"

abs() {
  case "$1" in
    /*) printf '%s' "$1" ;;
    *)  printf '%s/%s' "$(pwd)" "$1" ;;
  esac
}
SRC_IMG="$(abs "$SRC_IMG")"

if [ -z "$KO_SRC" ]; then
  for cand in "$SCRIPT_DIR/$KO_NAME" "$(pwd)/$KO_NAME"; do
    if [ -f "$cand" ]; then
      KO_SRC="$cand"
      break
    fi
  done
fi
[ -n "$KO_SRC" ] || die "can't find $KO_NAME, please pass it in as the second argument"
[ -f "$KO_SRC" ] || die "can't find ko: $KO_SRC"
KO_SRC="$(abs "$KO_SRC")"

[ -n "$NCORE" ] && [ -x "$NCORE" ] || \
  die "ncore is not executable: ${NCORE:-<empty>}"

[ -n "$TOYBOX" ] && [ -x "$TOYBOX" ] || die "toybox unavailable"

# Resolve OUT before cd, otherwise a relative NKSU_OUT would land in $WORK
# and be removed by cleanup.
OUT="$(abs "${NKSU_OUT:-$SCRIPT_DIR/$OUT_NAME}")"

run_ncore() {
  "$NCORE" "$@"
}

log "vendor_boot : $SRC_IMG"
log "nksu.ko     : $KO_SRC"
log "ncore       : $NCORE"
log "toybox      : $TOYBOX"
log "output      : $OUT"

mkdir -p "$WORK" || die "can't create $WORK"
cd "$WORK" || die "can't cd into $WORK"

log "unpacking vendor boot"
run_ncore -u "$SRC_IMG" >"$WORK/unpack.log" 2>&1 || {
  cat "$WORK/unpack.log" >&2
  die "unpack failed"
}
cat "$WORK/unpack.log"

VRAM=""
[ -f "$WORK/vendor_ramdisk" ] && VRAM="$WORK/vendor_ramdisk"

# v4 multi-ramdisk: take the first non-fragment entry
if [ -z "$VRAM" ]; then
  for f in "$WORK"/vendor_ramdisk.*; do
    [ -f "$f" ] || continue
    case "$f" in
      *fragment*) continue ;;
    esac
    VRAM="$f"
    break
  done
fi
[ -n "$VRAM" ] || die "unpacked but can't find vendor_ramdisk"

RDIR="$WORK/ramdisk"
mkdir -p "$RDIR"

RAMDISK_CPIO="$WORK/vendor_ramdisk.cpio"
MAGIC2="$(head -c2 "$VRAM" | od -An -tx1 | tr -d ' \n')"
MAGIC6="$(head -c6 "$VRAM" | tr -cd '0-9a-zA-Z')"
if [ "$MAGIC6" = "070701" ] || [ "$MAGIC6" = "070702" ]; then
  log "vendor_ramdisk is already raw cpio (newc)"
  cp "$VRAM" "$RAMDISK_CPIO"
elif [ "$MAGIC2" = "1f8b" ]; then
  log "vendor_ramdisk is gzip, decompressing..."
  gzip -cd "$VRAM" > "$RAMDISK_CPIO" || die "gzip decompress failed"
else
  warn "unknown vendor_ramdisk header ($MAGIC2), copying as raw"
  cp "$VRAM" "$RAMDISK_CPIO"
fi

log "toybox cpio unpacking..."
( cd "$RDIR" && "$TOYBOX" cpio -idm --no-preserve-owner < "$RAMDISK_CPIO" ) || die "cpio unpack failed"

MODDIR=""
for cand in \
  "lib/modules" \
  "vendor/lib/modules" \
  "lib/modules/$(uname -r)" \
  "usr/lib/modules"
do
  if [ -d "$RDIR/$cand" ] || [ -f "$RDIR/$cand/modules.load" ]; then
    MODDIR="$cand"
    break
  fi
done

if [ -z "$MODDIR" ]; then
  warn "no vendor module dir found in ramdisk, creating lib/modules"
  MODDIR="lib/modules"
fi
mkdir -p "$RDIR/$MODDIR" || die "can't create module dir $MODDIR"

log "ko dir: $MODDIR"
cp "$KO_SRC" "$RDIR/$MODDIR/$KO_NAME" || die "copy $KO_NAME failed"
chmod 0644 "$RDIR/$MODDIR/$KO_NAME"

append_line_unique() {
  file="$1"
  line="$2"
  [ -f "$RDIR/$file" ] || return 1
  if grep -qxF "$line" "$RDIR/$file" 2>/dev/null; then
    log "  $file already contains $KO_NAME"
    return 0
  fi
  if [ -s "$RDIR/$file" ] && [ -n "$(tail -c1 "$RDIR/$file")" ]; then
    echo "" >> "$RDIR/$file"
  fi
  echo "$line" >> "$RDIR/$file"
  log "  updated $file"
}

LOAD_FILE="$MODDIR/modules.load"
if [ -f "$RDIR/$LOAD_FILE" ]; then
  append_line_unique "$LOAD_FILE" "$KO_NAME"
else
  log "  modules.load not found, creating $LOAD_FILE"
  echo "$KO_NAME" > "$RDIR/$LOAD_FILE"
fi

DEP_FILE="$MODDIR/modules.dep"
if [ -f "$RDIR/$DEP_FILE" ]; then
  append_line_unique "$DEP_FILE" "/$MODDIR/$KO_NAME:"
else
  log "  modules.dep not found, creating $DEP_FILE"
  echo "/$MODDIR/$KO_NAME:" > "$RDIR/$DEP_FILE"
fi

NEW_RAMDISK="$WORK/vendor_ramdisk.new"
log "toybox cpio packing..."
( cd "$RDIR" && find . | "$TOYBOX" cpio -o -H newc ) > "$NEW_RAMDISK" || die "cpio packing failed"

TARGET_IMG="$WORK/vendor_boot.img"
cp "$SRC_IMG" "$TARGET_IMG" || die "copy vendor_boot failed"

log "repackaging vendor_ramdisk..."
run_ncore -r "$TARGET_IMG" "$NEW_RAMDISK" >"$WORK/replace.log" 2>&1 || {
  cat "$WORK/replace.log" >&2
  die "repack failed"
}
cat "$WORK/replace.log"

NEW_IMG="$TARGET_IMG.new"
[ -f "$NEW_IMG" ] || die "patched image not created: $NEW_IMG"

# ncore -r updates the header field vendor_ramdisk_size but does not sync the
# per-entry ramdisk_size in the v4 vendor_ramdisk_table.
# Header fields (LE u32): page_size@12, vendor_ramdisk_size@24, dtb_size@2100,
#                         table_size@2112, entry_num@2116, entry_size@2120
fix_v4_table() {
  img="$1"

  read_u32() {
    "$TOYBOX" od -An -tu4 -j "$1" -N4 "$img" | tr -d ' '
  }

  PAGE=$(read_u32 12)
  VRSIZE=$(read_u32 24)
  DTBSIZE=$(read_u32 2100)
  TSIZE=$(read_u32 2112)
  TNUM=$(read_u32 2116)

  [ -n "$TSIZE" ] || return 0
  [ "$TSIZE" -gt 0 ] 2>/dev/null || return 0   # no table on v3
  [ "$TNUM" -eq 1 ] 2>/dev/null || {
    warn "vendor_ramdisk_table has $TNUM entries, skipping auto fix"
    return 0
  }

  align() { echo $(( (($1) + PAGE - 1) / PAGE * PAGE )); }
  HDR=$(align 2128)
  VROFF=$HDR
  DTB_OFF=$(( VROFF + $(align "$VRSIZE") ))
  TOFF=$(( DTB_OFF + $(align "$DTBSIZE") ))

  ENTRY_SIZE=$("$TOYBOX" od -An -tu4 -j "$TOFF" -N4 "$img" | tr -d ' ')
  [ "$ENTRY_SIZE" = "$VRSIZE" ] && {
    log "  v4 table entry already correct (size=$VRSIZE)"
    return 0
  }

  log "  fixing v4 table entry ramdisk_size: $ENTRY_SIZE -> $VRSIZE"
  fmt=""
  shift_i=0
  while [ "$shift_i" -lt 32 ]; do
    byte=$(( (VRSIZE >> shift_i) & 0xFF ))
    oct=$(printf '%03o' "$byte")
    fmt="${fmt}\\$oct"
    shift_i=$(( shift_i + 8 ))
  done
  printf "$fmt" | dd of="$img" bs=1 seek="$TOFF" conv=notrunc 2>/dev/null
}

fix_v4_table "$NEW_IMG"

cp "$NEW_IMG" "$OUT" || die "write failed: $OUT"
chmod 0644 "$OUT" 2>/dev/null || true

log "done: $OUT"
log "flash it with 'fastboot flash vendor_boot' (test before flashing)"

exit 0