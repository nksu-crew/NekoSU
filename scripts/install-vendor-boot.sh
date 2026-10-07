#!/system/bin/sh
#
# install-vendor-boot.sh -- 通过 patched vendor_boot 安装 nksu (LKM)
#
# 原理:
#   vendor_boot 的 ramdisk 内含有厂商内核模块目录 (lib/modules/) 及其
#   modules.load / modules.dep 清单。stock 的 first-stage init 会在启动时
#   按清单加载这些模块。因此只需把 nksu.ko 放进该目录并登记到清单中,
#   无需替换 init。
#
# 流程:
#   1. ncore -u <vendor_boot.img> 解包出 vendor_ramdisk (已解压为原始 cpio)
#   2. toybox cpio 解包 ramdisk
#   3. 定位模块目录 (lib/modules), 写入 nksu.ko
#   4. 更新 modules.load / modules.dep 清单
#   5. toybox cpio 重新打包 (ncore -r 会按原格式重新压缩)
#   6. ncore -r <vendor_boot.img> <new_vendor_ramdisk> 回填生成 <vendor_boot.img>.new
#   7. 修正 v4 vendor_ramdisk_table 条目大小
#   8. 输出 patched vendor_boot 到 Download 目录
#
# 使用: install-vendor-boot.sh [vendor_boot.img] [nksu.ko]
#
# 说明:
#   - 需要 root (通过 `su -c` 运行, 或已处于 root shell)。
#   - ncore / toybox 默认从 /system/bin 查找, 也可用 NKSU_NCORE / NKSU_TOYBOX 覆盖。
#   - 输出固定为: /sdcard/Download/vendor_boot_nksu.img

set -u

# ---------------------------------------------------------------------------
# 参数与路径
# ---------------------------------------------------------------------------
SRC_IMG="${1:-}"
KO_SRC="${2:-}"

OUT_NAME="vendor_boot_nksu.img"
KO_NAME="nksu.ko"

# ncore: 优先环境变量, 其次 /system/bin/ncore, 最后与脚本同目录的 ncore
NCORE="${NKSU_NCORE:-}"
if [ -z "$NCORE" ]; then
  if [ -x /system/bin/ncore ]; then
    NCORE=/system/bin/ncore
  else
    NCORE="$(dirname "$0")/ncore"
  fi
fi

# toybox: 优先环境变量, 其次 /system/bin/toybox (Android 自带), 最后 PATH 中的 toybox
TOYBOX="${NKSU_TOYBOX:-}"
if [ -z "$TOYBOX" ]; then
  if [ -x /system/bin/toybox ]; then
    TOYBOX=/system/bin/toybox
  else
    TOYBOX="$(command -v toybox 2>/dev/null || true)"
  fi
fi

# 工作目录 (root 可写)
WORK="/data/local/tmp/nksu-install.$$"

log()  { printf '[nksu] %s\n' "$*"; }
warn() { printf '[nksu] WARN: %s\n' "$*" >&2; }
die()  { printf '[nksu] ERROR: %s\n' "$*" >&2; exit 1; }

cleanup() {
  [ -n "${WORK:-}" ] && [ -d "$WORK" ] && rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# 参数解析与校验
# ---------------------------------------------------------------------------
[ -n "$SRC_IMG" ] || die "用法: $0 <vendor_boot.img> [nksu.ko]"
[ -f "$SRC_IMG" ] || die "找不到 vendor_boot: $SRC_IMG"

abs() {
  case "$1" in
    /*) printf '%s' "$1" ;;
    *)  printf '%s/%s' "$(pwd)" "$1" ;;
  esac
}
SRC_IMG="$(abs "$SRC_IMG")"

# 如果未显式给出 nksu.ko, 在脚本同目录与常见位置查找
if [ -z "$KO_SRC" ]; then
  SCRIPT_DIR="$(cd "$(dirname "$0")" 2>/dev/null && pwd)"
  for cand in \
    "$SCRIPT_DIR/$KO_NAME" \
    "$(pwd)/$KO_NAME" \
    "/data/local/tmp/$KO_NAME" \
    "/sdcard/Download/$KO_NAME"
  do
    if [ -f "$cand" ]; then
      KO_SRC="$cand"
      break
    fi
  done
fi
[ -n "$KO_SRC" ] || die "找不到 $KO_NAME, 请作为第二个参数传入"
[ -f "$KO_SRC" ] || die "找不到 ko: $KO_SRC"
KO_SRC="$(abs "$KO_SRC")"

[ -x "$NCORE" ]  || die "ncore 不可执行: ${NCORE:-<empty>}"
[ -n "$TOYBOX" ] && [ -x "$TOYBOX" ] || die "toybox 不可用 (设置 NKSU_TOYBOX)"

# ---------------------------------------------------------------------------
# 输出目录 (Download)
# ---------------------------------------------------------------------------
DOWNLOAD=""
for cand in /sdcard/Download /storage/emulated/0/Download; do
  if [ -d "$cand" ]; then
    DOWNLOAD="$cand"
    break
  fi
done
[ -n "$DOWNLOAD" ] || die "找不到 Download 目录"
OUT="$DOWNLOAD/$OUT_NAME"

log "vendor_boot : $SRC_IMG"
log "nksu.ko     : $KO_SRC"
log "ncore       : $NCORE"
log "toybox      : $TOYBOX"
log "输出        : $OUT"

mkdir -p "$WORK" || die "无法创建工作目录 $WORK"
cd "$WORK" || die "无法进入 $WORK"

# ---------------------------------------------------------------------------
# 1. ncore 解包
# ---------------------------------------------------------------------------
log "ncore -u 解包中..."
"$NCORE" -u "$SRC_IMG" >"$WORK/unpack.log" 2>&1 || {
  cat "$WORK/unpack.log" >&2
  die "ncore 解包失败"
}
cat "$WORK/unpack.log"

VRAM=""
[ -f "$WORK/vendor_ramdisk" ] && VRAM="$WORK/vendor_ramdisk"

# v4 可能拆成 multi-ramdisk (vendor_ramdisk.<name>), 选取首个 non-fragment。
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

# ---------------------------------------------------------------------------
# 2. 解压 + cpio 解包
# ---------------------------------------------------------------------------
RDIR="$WORK/ramdisk"
mkdir -p "$RDIR"

RAMDISK_CPIO="$WORK/vendor_ramdisk.cpio"
# ncore -u 已将 vendor_ramdisk 解压为原始 cpio (newc, 070701)。
# 若来源异常仍带压缩层, 这里做一次兼容性判断。
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

# ---------------------------------------------------------------------------
# 3. 定位模块目录, 写入 nksu.ko
# ---------------------------------------------------------------------------
# 常见的厂商模块目录 (按优先级)。stock init 从 modules.load 读取清单。
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

# 若不存在则创建 lib/modules (标准位置)
if [ -z "$MODDIR" ]; then
  warn "ramdisk 中未找到厂商模块目录, 创建 lib/modules"
  MODDIR="lib/modules"
fi
mkdir -p "$RDIR/$MODDIR" || die "无法创建模块目录 $MODDIR"

log "模块目录: $MODDIR"
cp "$KO_SRC" "$RDIR/$MODDIR/$KO_NAME" || die "拷贝 $KO_NAME 失败"
chmod 0644 "$RDIR/$MODDIR/$KO_NAME"

# ---------------------------------------------------------------------------
# 4. 更新模块清单 (modules.load / modules.dep)
# ---------------------------------------------------------------------------
# modules.load 格式: 每行一个模块文件名 (如 miev.ko)
# modules.dep  格式: /lib/modules/<name>.ko: <dep1> <dep2>
append_line_unique() {
  file="$1"
  line="$2"
  [ -f "$RDIR/$file" ] || return 1
  if grep -qxF "$line" "$RDIR/$file" 2>/dev/null; then
    log "  $file 已包含 $KO_NAME"
    return 0
  fi
  # 确保文件以换行结尾
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

# ---------------------------------------------------------------------------
# 5. 重新打包 + 压缩
# ---------------------------------------------------------------------------
NEW_RAMDISK="$WORK/vendor_ramdisk.new"
log "toybox cpio 打包中..."
( cd "$RDIR" && find . | "$TOYBOX" cpio -o -H newc ) > "$NEW_RAMDISK" || die "cpio 打包失败"

# ncore -r 会按原镜像中该 section 的压缩格式 (如 lz4_leg/gzip) 自动重新压缩,
# 因此这里始终输出原始 cpio 即可。

# ---------------------------------------------------------------------------
# 6. ncore 回填
# ---------------------------------------------------------------------------
TARGET_IMG="$WORK/vendor_boot.img"
cp "$SRC_IMG" "$TARGET_IMG" || die "拷贝 vendor_boot 失败"

log "ncore -r 回填 vendor_ramdisk..."
"$NCORE" -r "$TARGET_IMG" "$NEW_RAMDISK" >"$WORK/replace.log" 2>&1 || {
  cat "$WORK/replace.log" >&2
  die "ncore 回填失败"
}
cat "$WORK/replace.log"

NEW_IMG="$TARGET_IMG.new"
[ -f "$NEW_IMG" ] || die "未生成 patched 镜像: $NEW_IMG"

# ---------------------------------------------------------------------------
# 6.5 修正 v4 vendor_ramdisk_table 中的 size 字段
# ---------------------------------------------------------------------------
# ncore -r 会更新头部的 vendor_ramdisk_size, 但不会同步 v4 表中各条目的
# ramdisk_size。若设备按表加载, 陈旧的大小会截断 ramdisk, 这里就地修正。
fix_v4_table() {
  img="$1"

  # 读取头字段 (little-endian u32): page_size@12, vendor_ramdisk_size@24,
  # dtb_size@2100, table_size@2112, entry_num@2116, entry_size@2120
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

  # 计算表偏移: align(2128) + align(vrsize) + align(dtb)
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
  # 写入小端 u32 (使用 printf 的 \xNN 转义)
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

# ---------------------------------------------------------------------------
# 7. 输出到 Download
# ---------------------------------------------------------------------------
cp "$NEW_IMG" "$OUT" || die "写出失败: $OUT"
chmod 0644 "$OUT" 2>/dev/null || true

log "完成: $OUT"
log "请用 fastboot flash vendor_boot 刷入 (先测试再刷)。"

exit 0
