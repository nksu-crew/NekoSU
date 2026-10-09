#!/system/bin/sh
# nksu userspace module loader.
#
# This file is embedded verbatim into src/init_rc.c (nksu_rc_script); keep the
# two in sync -- the kernel writes this text to /dev/nksu/modules.sh at boot.
#
# Exec'd by the rc that nksu injects into init.rc (see src/init_rc.c) at the
# well-defined boot stages.  It replaces the old kernel-side loader: the
# enumeration, the boot-hook ordering and the metamodule mount all happen
# here, exactly like KernelSU's ksud but without a resident daemon.
#
#   post-fs-data   post-fs-data.d, each module's sepolicy.rule, the
#                  metamodule's post-fs-data.sh, the regular modules'
#                  post-fs-data.sh, then the metamodule's metamount.sh
#   late_start     service.d, metamodule service.sh, regular service.sh
#                  (all detached: a service.sh may keep a daemon)
#
# sepolicy.rule cannot be applied from shell, so each file path is handed to
# the kernel through /proc/nksu/sepolicy (see selinux/rule_file.c).

STAGE="$1"
export PATH=/sbin:/system/sbin:/system/bin:/system/xbin
export ANDROID_ROOT=/system

MODULES_DIR=/data/adb/modules
POST_FS_DATA_D=/data/adb/post-fs-data.d
SERVICE_D=/data/adb/service.d
SEPOLICY_SINK=/proc/nksu/sepolicy
STATE_DIR=/dev/nksu

# init does not necessarily keep the loader's stdout, so mirror every line to
# the kernel log; that is where nksu's other diagnostics show up.
log() {
    echo "nksu: $*"
    echo "nksu: $*" > /dev/kmsg 2>/dev/null
}

# A module is skipped when it is disabled or pending removal.
enabled() {
    [ -d "$1" ] || return 1
    [ -e "$1/disable" ] && return 1
    [ -e "$1/remove" ] && return 1
    return 0
}

is_metamodule() {
    [ -f "$1/module.prop" ] || return 1
    grep -Eq '^[[:space:]]*metamodule[[:space:]]*=[[:space:]]*(1|true)' "$1/module.prop" 2>/dev/null
}

# /data/adb is mounted with /data, but on FBE devices the module tree only
# becomes readable once vold finishes decrypting.  Reading it too early finds
# zero modules and (with the .done guard) would skip the whole stage, so wait
# (bounded, ~2s) for it before enumerating.  A missing /data/adb means nothing
# is installed, so do not stall.
wait_modules() {
    [ -d /data/adb ] || return 0
    n=0
    while [ ! -d "$MODULES_DIR" ] && [ "$n" -lt 20 ]; do
        sleep 0.1
        n=$((n + 1))
    done
}

apply_sepolicy() {
    [ -e "$SEPOLICY_SINK" ] || return 0
    for d in "$MODULES_DIR"/*; do
        enabled "$d" || continue
        [ -f "$d/sepolicy.rule" ] && echo "$d/sepolicy.rule" > "$SEPOLICY_SINK"
    done
}

# $1 directory, $2 wait|nowait
run_dir_scripts() {
    [ -d "$1" ] || return 0
    for f in "$1"/*.sh; do
        [ -f "$f" ] || continue
        if [ "$2" = nowait ]; then sh "$f" & else sh "$f"; fi
    done
}

# $1 hook, $2 wait|nowait, $3 meta|regular
run_hooks() {
    for d in "$MODULES_DIR"/*; do
        enabled "$d" || continue
        if [ "$3" = meta ]; then
            is_metamodule "$d" || continue
        else
            is_metamodule "$d" && continue
        fi
        [ -f "$d/$1" ] || continue
        if [ "$2" = nowait ]; then sh "$d/$1" & else sh "$d/$1"; fi
    done
}

do_post_fs_data() {
    wait_modules

    count=0
    for d in "$MODULES_DIR"/*; do
        [ -d "$d" ] && count=$((count + 1))
    done
    log "post-fs-data: $count module dir(s)"

    run_dir_scripts "$POST_FS_DATA_D" wait
    apply_sepolicy
    run_hooks post-fs-data.sh wait meta
    run_hooks post-fs-data.sh wait regular
    log "metamodule mount"
    run_hooks metamount.sh wait meta
    log "post-fs-data done"
}

do_late_start() {
    log "late_start stage"
    run_dir_scripts "$SERVICE_D" nowait
    run_hooks service.sh nowait meta
    run_hooks service.sh nowait regular
    log "late_start done"
}

log "loader start: stage=$STAGE"

case "$STAGE" in
    post-fs-data)
        [ -e "$STATE_DIR/.post-fs-data.done" ] && exit 0
        : > "$STATE_DIR/.post-fs-data.done"
        do_post_fs_data
        ;;
    late_start)
        [ -e "$STATE_DIR/.late-start.done" ] && exit 0
        : > "$STATE_DIR/.late-start.done"
        do_late_start
        ;;
    *)
        log "unknown stage: $STAGE"
        exit 1
        ;;
esac

exit 0
