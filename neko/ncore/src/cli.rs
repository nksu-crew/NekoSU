// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * NekoSU ncore command line.
 *
 * Upstream ksud carries boot patching, LKM late-loading, sulog, feature and
 * profile commands. NekoSU does boot patching in the manager's libjni, loads
 * its LKM from vendor_boot and drives profiles/features through the manager
 * JNI, so only the module runtime, su/resetprop entry points and daemon
 * install remain.
 *
 * Manager recognition moved from the kernel module into `src/manager/` (a
 * user-space port of manager/abx.c + manager.c).  There is no fsnotify
 * observer here, so scanning is triggered explicitly via `ncore manager ...`.
 */
use anyhow::Result;
use clap::Parser;

use android_logger::Config;
use log::LevelFilter;

use crate::{
    boot::init_event,
    defs, manager, module,
    system::{resetprop, su, utils},
};

#[derive(Parser, Debug)]
#[command(author, version = defs::FULL_VERSION, about, long_about = None)]
struct Args {
    #[command(subcommand)]
    command: Commands,
}

#[derive(clap::Subcommand, Debug)]
enum Commands {
    /// Manage KernelSU modules
    Module {
        #[command(subcommand)]
        command: Module,
    },

    /// SELinux policy patch tool
    Sepolicy {
        #[command(subcommand)]
        command: Sepolicy,
    },

    /// Trigger `post-fs-data` event
    PostFsData,

    /// Trigger `service` event
    Services,

    /// Trigger `boot-complete` event
    BootCompleted,

    /// Install the ncore userspace component
    Install,

    /// Manager recognition and /data/adb/ncore bootstrap
    Manager {
        #[command(subcommand)]
        command: ManagerCmd,
    },

    /// Resetprop - Magisk-compatible system property tool
    #[command(disable_help_flag = true)]
    Resetprop {
        #[arg(trailing_var_arg = true, allow_hyphen_values = true, num_args = 0..)]
        args: Vec<String>,
    },
}

#[derive(clap::Subcommand, Debug)]
enum ManagerCmd {
    /// Find the manager package by signing certificate and print its details
    Scan {
        /// SHA-256 of the manager's DER signing certificate (64 hex chars).
        /// If omitted, the build-time NKSU_MANAGER_CERTHASH is used.
        #[arg(default_value = "")]
        cert_hash: String,
        /// Override packages.xml path (default: /data/system/packages.xml)
        #[arg(long, value_name = "PATH")]
        packages_xml: Option<String>,
    },
}

#[derive(clap::Subcommand, Debug)]
enum Sepolicy {
    /// Patch the live kernel policy with a rule
    Patch { sepolicy: String },

    /// Apply rules from a file
    Apply { file: String },

    /// Check whether a rule is valid/supported without applying it
    Check { sepolicy: String },
}

#[derive(clap::Subcommand, Debug)]
enum Module {
    /// Install module <ZIP>
    Install { zip: String },

    /// Undo module uninstall mark <id>
    UndoUninstall { id: String },

    /// Uninstall module <id>
    Uninstall { id: String },

    /// Enable module <id>
    Enable { id: String },

    /// Disable module <id>
    Disable { id: String },

    /// Run action for module <id>
    Action { id: String },

    /// List all modules as JSON
    List {
        #[arg(long)]
        json: bool,
    },
}

pub fn run() -> Result<()> {
    android_logger::init_once(
        Config::default()
            .with_max_level(crate::debug_select!(LevelFilter::Trace, LevelFilter::Info))
            .with_tag("NekoSU"),
    );

    // The kernel redirects an execve() of /system/bin/su to this binary while
    // keeping argv[0] intact.
    let arg0 = std::env::args().next().unwrap_or_default();
    if arg0 == "su" || arg0.ends_with("/su") {
        return su::root_shell();
    }

    if arg0.ends_with("resetprop") {
        let all_args: Vec<String> = std::env::args().collect();
        resetprop::resetprop_main(&all_args);
    }

    let cli = Args::parse();
    log::info!("command: {:?}", cli.command);

    let result = match cli.command {
        Commands::PostFsData => init_event::on_post_fs_data(),
        Commands::Services => {
            init_event::on_services();
            Ok(())
        }
        Commands::BootCompleted => {
            init_event::on_boot_completed();
            Ok(())
        }
        Commands::Install => utils::install(),
        Commands::Manager { command } => match command {
            ManagerCmd::Scan {
                cert_hash,
                packages_xml,
            } => manager::cli::resolve_cert_hash(cert_hash)
                .and_then(|h| manager::cli::run_manager_scan(&h, packages_xml)),
        },
        Commands::Resetprop { args } => {
            let mut full_args = vec!["resetprop".to_string()];
            full_args.extend(args);
            resetprop::resetprop_main(&full_args)
        }
        Commands::Sepolicy { command } => match command {
            Sepolicy::Patch { sepolicy } => crate::selinux::sepolicy::live_patch(&sepolicy),
            Sepolicy::Apply { file } => crate::selinux::sepolicy::apply_file(file),
            Sepolicy::Check { sepolicy } => crate::selinux::sepolicy::check_rule(&sepolicy),
        },
        Commands::Module { command } => {
            utils::switch_mnt_ns(1)?;
            match command {
                Module::Install { zip } => module::install_module(&zip),
                Module::UndoUninstall { id } => module::undo_uninstall_module(&id),
                Module::Uninstall { id } => module::uninstall_module(&id),
                Module::Enable { id } => module::enable_module(&id),
                Module::Disable { id } => module::disable_module(&id),
                Module::Action { id } => module::run_action(&id),
                Module::List { json: _ } => module::list_modules(),
            }
        }
    };

    if let Err(e) = &result {
        log::error!("Error: {e:?}");
    }
    result
}
