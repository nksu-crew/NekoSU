#!/system/bin/sh
#
# install-vendor-boot.sh -- 通过 patched vendor_boot 安装 nksu (LKM)
#
# 把 nksu.ko 写入 vendor_ramdisk 的厂商模块目录并登记到 modules.load /
# modules.dep, stock first-stage init 启动时会自动加载, 无需替换 init。
#
# 使用: install-vendor-boot.sh [vendor_boot.img] [nksu.ko]
#
# 说明:
#   - 以应用自身身份运行, 不使用 su / 不需要 root。
#   - ncore 由 app 以 native lib (nativeLibraryDir/libncore.so) 形式提供,
#     路径通过环境变量 NKSU_NCORE 传入; 脚本不再把 ncore 释放到 filesDir,
#     因为 Android 10+ 不允许执行 /data 下的二进制 (noexec)。
#   - 所有中间文件与输出都位于脚本所在目录 (filesDir/nksu-install)。
#   - toybox 默认从 /system/bin 查找, 可用 NKSU_TOYBOX 覆盖。
#   - 输出默认 <脚本目录>/vendor_boot_nksu.img, 可用 NKSU_OUT 覆盖;
#     工作目录默认 <脚本目录>/work.<pid>, 可用 NKSU_WORK 覆盖。

set -u

SRC_IMG="${1:-}"
KO_SRC="${2:-}"

OUT_NAME="vendor_boot_nksu.img"
KO_NAME="nksu.ko"

SCRIPT_DIR="$(cd "$(dirname "$0")" 2>/dev/null && pwd)"
[ -n "$SCRIPT_DIR" ] || SCRIPT_DIR="$(pwd)"

# --- ncore 由 app 提供 (nativeLibraryDir 下的 libncore.so) -------------------
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

# --- 参数解析 --------------------------------------------------------------

[ -n "$SRC_IMG" ] || die "用法: $0 <vendor_boot.img> [nksu.ko]"
[ -f "$SRC_IMG" ] || die "找不到 vendor_boot: $SRC_IMG"

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
[ -n "$KO_SRC" ] || die "找不到 $KO_NAME, 请作为第二个参数传入"
[ -f "$KO_SRC" ] || die "找不到 ko: $KO_SRC"
KO_SRC="$(abs "$KO_SRC")"

[ -n "$NCORE" ] && [ -x "$NCORE" ] || \
  die "ncore 不可执行: ${NCORE:-<empty>} (由 app 通过 NKSU_NCORE 传入)"

[ -n "$TOYBOX" ] && [ -x "$TOYBOX" ] || die "toybox 不可用 (设置 NKSU_TOYBOX)"

OUT="${NKSU_OUT:-$SCRIPT_DIR/$OUT_NAME}"

# ---------------------------------------------------------------------------
# 特殊函数: 脚本内所有 ncore 调用都走这里。
#
# app 端把 ncore 打包成 native lib 放在 nativeLibraryDir 下 (那是允许执行
# 的挂载点), 而不是释放到 filesDir。Android 10+ 起 /data 分区为 noexec,
# 从 filesDir 执行任何二进制都会失败 (EACCES)。
#
# 之所以封装成函数, 是为了:
#   - 脚本里不直接出现 "$NCORE" 调用, 便于后续替换实现;
#   - 后续如果换用其它执行通道 (比如 app 端 pipe 协议), 只需改这个函数。
# ---------------------------------------------------------------------------
run_ncore() {
  "$NCORE" "$@"
}

log "vendor_boot : $SRC_IMG"
log "nksu.ko     : $KO_SRC"
log "ncore       : $NCORE"
log "toybox      : $TOYBOX"
log "输出        : $OUT"

mkdir -p "$WORK" || die "无法创建工作目录 $WORK"
cd "$WORK" || die "无法进入 $WORK"

# --- 1. ncore 解包 ---------------------------------------------------------

log "ncore -u 解包中..."
run_ncore -u "$SRC_IMG" >"$WORK/unpack.log" 2>&1 || {
  cat "$WORK/unpack.log" >&2
  die "ncore 解包失败"
}
cat "$WORK/unpack.log"

VRAM=""
[ -f "$WORK/vendor_ramdisk" ] && VRAM="$WORK/vendor_ramdisk"

# v4 multi-ramdisk: 取首个非 fragment
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
[ -n "$VRAM" ] || die "解包结果中没有 vendor_ramdisk"

# --- 2. 解压 + cpio 解包 ---------------------------------------------------

RDIR="$WORK/ramdisk"
mkdir -p "$RDIR"

RAMDISK_CPIO="$WORK/vendor_ramdisk.cpio"
MAGIC2="$(head -c2 "$VRAM" | od -An -tx1 | tr -d ' \n')"
MAGIC6="$(head -c6 "$VRAM" | tr -cd '0-9a-zA-Z')"
if [ "$MAGIC6" = "070701" ] || [ "$MAGIC6" = "070702" ]; then
  log "vendor_ramdisk 已是原始 cpio (newc)"
  cp "$VRAM" "$RAMDISK_CPIO"
elif [ "$MAGIC2" = "1f8b" ]; then
  log "vendor_ramdisk 为 gzip, 解压中..."
  gzip -cd "$VRAM" > "$RAMDISK_CPIO" || die "gzip 解压失败"
else
  warn "未知 vendor_ramdisk 头 ($MAGIC2), 按原始数据复制"
  cp "$VRAM" "$RAMDISK_CPIO"
fi

log "toybox cpio 解包中..."
( cd "$RDIR" && "$TOYBOX" cpio -idm --no-preserve-owner < "$RAMDISK_CPIO" ) || die "cpio 解包失败"

# --- 3. 写入 nksu.ko -------------------------------------------------------

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
  warn "ramdisk 中未找到厂商模块目录, 创建 lib/modules"
  MODDIR="lib/modules"
fi
mkdir -p "$RDIR/$MODDIR" || die "无法创建模块目录 $MODDIR"

log "模块目录: $MODDIR"
cp "$KO_SRC" "$RDIR/$MODDIR/$KO_NAME" || die "拷贝 $KO_NAME 失败"
chmod 0644 "$RDIR/$MODDIR/$KO_NAME"

# --- 4. 更新 modules.load / modules.dep ------------------------------------

append_line_unique() {
  file="$1"
  line="$2"
  [ -f "$RDIR/$file" ] || return 1
  if grep -qxF "$line" "$RDIR/$file" 2>/dev/null; then
    log "  $file 已包含 $KO_NAME"
    return 0
  fi
  if [ -s "$RDIR/$file" ] && [ -n "$(tail -c1 "$RDIR/$file")" ]; then
    echo "" >> "$RDIR/$file"
  fi
  echo "$line" >> "$RDIR/$file"
  log "  已更新 $file"
}

LOAD_FILE="$MODDIR/modules.load"
if [ -f "$RDIR/$LOAD_FILE" ]; then
  append_line_unique "$LOAD_FILE" "$KO_NAME"
else
  log "  未找到 modules.load, 创建 $LOAD_FILE"
  echo "$KO_NAME" > "$RDIR/$LOAD_FILE"
fi

DEP_FILE="$MODDIR/modules.dep"
if [ -f "$RDIR/$DEP_FILE" ]; then
  append_line_unique "$DEP_FILE" "/$MODDIR/$KO_NAME:"
else
  log "  未找到 modules.dep, 创建 $DEP_FILE"
  echo "/$MODDIR/$KO_NAME:" > "$RDIR/$DEP_FILE"
fi

# --- 5. 重新打包 -----------------------------------------------------------

NEW_RAMDISK="$WORK/vendor_ramdisk.new"
log "toybox cpio 打包中..."
( cd "$RDIR" && find . | "$TOYBOX" cpio -o -H newc ) > "$NEW_RAMDISK" || die "cpio 打包失败"

# --- 6. ncore 回填 ---------------------------------------------------------

TARGET_IMG="$WORK/vendor_boot.img"
cp "$SRC_IMG" "$TARGET_IMG" || die "拷贝 vendor_boot 失败"

log "ncore -r 回填 vendor_ramdisk..."
run_ncore -r "$TARGET_IMG" "$NEW_RAMDISK" >"$WORK/replace.log" 2>&1 || {
  cat "$WORK/replace.log" >&2
  die "ncore 回填失败"
}
cat "$WORK/replace.log"

NEW_IMG="$TARGET_IMG.new"
[ -f "$NEW_IMG" ] || die "未生成 patched 镜像: $NEW_IMG"

# --- 6.5 修正 v4 vendor_ramdisk_table size ---------------------------------

# ncore -r 更新头部 vendor_ramdisk_size, 但不同步 v4 表中各条目 ramdisk_size。
# 头字段 (LE u32): page_size@12, vendor_ramdisk_size@24, dtb_size@2100,
#                  table_size@2112, entry_num@2116, entry_size@2120
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
  [ "$TSIZE" -gt 0 ] 2>/dev/null || return 0   # v3 无表
  [ "$TNUM" -eq 1 ] 2>/dev/null || {
    warn "vendor_ramdisk_table 有 $TNUM 个条目, 跳过自动修正"
    return 0
  }

  align() { echo $(( (($1) + PAGE - 1) / PAGE * PAGE )); }
  HDR=$(align 2128)
  VROFF=$HDR
  DTB_OFF=$(( VROFF + $(align "$VRSIZE") ))
  TOFF=$(( DTB_OFF + $(align "$DTBSIZE") ))

  ENTRY_SIZE=$("$TOYBOX" od -An -tu4 -j "$TOFF" -N4 "$img" | tr -d ' ')
  [ "$ENTRY_SIZE" = "$VRSIZE" ] && {
    log "  v4 表条目已正确 (size=$VRSIZE)"
    return 0
  }

  log "  修正 v4 表条目 ramdisk_size: $ENTRY_SIZE -> $VRSIZE"
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

cp "$NEW_IMG" "$OUT" || die "写出失败: $OUT"
chmod 0644 "$OUT" 2>/dev/null || true

log "完成: $OUT"
log "请取出该镜像后用 fastboot flash vendor_boot 刷入 (先测试再刷)。"

exit 0