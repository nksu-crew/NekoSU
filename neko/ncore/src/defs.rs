// SPDX-License-Identifier: GPL-3.0-or-later
#[cfg(target_os = "android")]
mod android {
    use const_format::concatcp;
    use std::time::Duration;

    pub const BOOT_STAGE_TIMEOUT: Duration = Duration::from_secs(35);
    pub const HOT_INSTALL_SCRIPT_TIMEOUT: Duration = Duration::from_secs(30);
    pub const BOOTLOG_TIMEOUT: &str = "30s";

    pub const ADB_DIR: &str = "/data/adb/";
    pub const WORKING_DIR: &str = concatcp!(ADB_DIR, "nksu/");
    pub const BINARY_DIR: &str = concatcp!(WORKING_DIR, "bin/");
    pub const LOG_DIR: &str = concatcp!(WORKING_DIR, "log/");

    pub const KSURC_PATH: &str = concatcp!(WORKING_DIR, ".ksurc");
    pub const DAEMON_PATH: &str = concatcp!(ADB_DIR, "ncore");

    pub const DAEMON_LINK_PATH: &str = concatcp!(BINARY_DIR, "ncore");

    pub const MODULE_DIR: &str = concatcp!(ADB_DIR, "modules/");
    pub const MODULE_UPDATE_DIR: &str = concatcp!(ADB_DIR, "modules_update/");
    pub const METAMODULE_DIR: &str = concatcp!(ADB_DIR, "metamodule/");
    // Replaced module dirs are parked here until the next boot
    pub const HOT_INSTALL_TRASH_DIR: &str = concatcp!(WORKING_DIR, "hot_install_trash/");

    pub const MODULE_WEB_DIR: &str = "webroot";
    pub const MODULE_ACTION_SH: &str = "action.sh";
    pub const DISABLE_FILE_NAME: &str = "disable";
    pub const UPDATE_FILE_NAME: &str = "update";
    pub const REMOVE_FILE_NAME: &str = "remove";
    pub const MODULE_PREINIT_DIR: &str = "preinit";

    // Written by installer.sh when customize.sh requests a hot install
    pub const HOT_INSTALL_MARKER: &str = ".ksu_hot_install";

    // Module config system
    pub const MODULE_CONFIG_DIR: &str = concatcp!(WORKING_DIR, "module_configs/");
    pub const PERSIST_CONFIG_NAME: &str = "persist.config";
    pub const TEMP_CONFIG_NAME: &str = "tmp.config";

    // Metamodule support
    pub const METAMODULE_MOUNT_SCRIPT: &str = "metamount.sh";
    pub const METAMODULE_METAINSTALL_SCRIPT: &str = "metainstall.sh";
    pub const METAMODULE_METAUNINSTALL_SCRIPT: &str = "metauninstall.sh";

}

#[allow(unused)]
pub const VERSION_CODE: &str = env!("VERSION_CODE");
#[cfg(target_os = "android")]
pub const VERSION_NAME: &str = env!("VERSION_NAME");
#[cfg(target_os = "android")]
pub const FULL_VERSION: &str =
    const_format::formatcp!("{VERSION_NAME} (uapi: {})", KERNEL_SU_UAPI_VERSION);

pub const MANAGER_CERT_HASH: &str = env!("NKSU_MANAGER_CERTHASH");

#[cfg(target_os = "android")]
pub use android::*;

pub const ANTIBRICK_PATH: &str = "/data/adb/nksu/antibrick";

pub const KERNEL_SU_UAPI_VERSION: u32 = 5;
