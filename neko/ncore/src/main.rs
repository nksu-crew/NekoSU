// SPDX-License-Identifier: GPL-3.0-or-later
#![deny(clippy::all, clippy::pedantic)]
#![warn(clippy::nursery)]
#![allow(
    clippy::module_name_repetitions,
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    clippy::cast_precision_loss,
    clippy::doc_markdown,
    clippy::too_many_lines,
    clippy::cast_possible_wrap,
    clippy::redundant_field_names
)]

#[cfg(target_os = "android")]
mod assets;
#[cfg(target_os = "android")]
mod boot;
#[cfg(target_os = "android")]
mod cli;
#[cfg(target_os = "android")]
mod defs;
#[cfg(target_os = "android")]
mod manager;
#[cfg(target_os = "android")]
mod module;
#[cfg(target_os = "android")]
mod preinit;
#[cfg(target_os = "android")]
mod selinux;
#[cfg(target_os = "android")]
mod system;

fn main() -> anyhow::Result<()> {
    #[cfg(target_os = "android")]
    {
        cli::run()
    }
    #[cfg(not(target_os = "android"))]
    {
        println!("ncore can only run on Android");
        Ok(())
    }
}
