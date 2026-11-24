mod init_boot;
mod kallsyms;
mod symbols;
mod vendor_boot;

pub use init_boot::patch_init_boot;
pub use vendor_boot::patch_vendor_boot;
