"""Bazel rule that builds the nksu out-of-tree kernel module through kbuild.

This is intentionally a thin wrapper: the Linux kernel build system (kbuild)
still performs the actual compilation, because an out-of-tree module must be
built against the exact kernel tree it will be loaded into (generated headers,
`Module.symvers`, modpost, vermagic, ...). Bazel only provides the entry point,
tracks the module sources and caches the resulting `.ko`.

The rule is expected to run where the DDK environment is available, i.e. inside
a `ghcr.io/ylarod/ddk` container or an equivalent setup that exports `KDIR`
(and `ARCH` / `CROSS_COMPILE` / `LLVM` / `PATH`). Those variables are forwarded
to the action through `.bazelrc` (`--action_env=...`).
"""

def _shquote(value):
    """Quotes a string for safe use in a POSIX shell command."""
    return "'" + value.replace("'", "'\\''") + "'"

def _nksu_module_impl(ctx):
    prefix = ctx.attr.out_dir + "/" if ctx.attr.out_dir else ""
    out = ctx.actions.declare_file(prefix + ctx.attr.module_name + ".ko")

    # kbuild drops many intermediate files next to the .ko, so build inside a
    # staging directory derived from the declared output.
    stage = out.path[:-3] + ".stage"

    installs = []
    for src in ctx.files.srcs:
        if src.short_path.startswith("../"):
            fail("nksu_module sources must live in the main workspace: %s" % src.path)
        installs.append((src.path, src.short_path))

    # Allow reproducible debug builds to embed the exact revision. CI passes
    # `--define=nksu_git_commit=$(git rev-parse --short=12 HEAD)`; otherwise the
    # value falls back to "unknown".
    commit = ctx.var.get("nksu_git_commit", "") or "unknown"
    config = " ".join(ctx.attr.config)

    cmd = [
        "set -euo pipefail",
        "rm -rf %s" % _shquote(stage),
    ]
    for src, rel in installs:
        cmd.append("install -D -m 0644 %s %s" % (
            _shquote(src),
            _shquote(stage + "/" + rel),
        ))
    cmd.extend([
        ': "${KDIR:?KDIR is not set; run this inside the DDK container or export KDIR}"',
        "make -C %s KDIR=%s %s NKSU_GIT_COMMIT=%s -j\"$(nproc)\"" % (
            _shquote(stage),
            '"$KDIR"',
            config,
            _shquote(commit),
        ),
        "cp -f %s %s" % (
            _shquote(stage + "/" + ctx.attr.module_name + ".ko"),
            _shquote(out.path),
        ),
    ])

    ctx.actions.run_shell(
        outputs = [out],
        inputs = ctx.files.srcs,
        command = "\n".join(cmd),
        # kbuild must see the DDK environment and write outside the sandbox.
        use_default_shell_env = True,
        execution_requirements = {
            "no-sandbox": "1",
            "no-remote": "1",
        },
        mnemonic = "KbuildNksu",
        progress_message = "Building %s with kbuild" % out.short_path,
    )

    return [DefaultInfo(files = depset([out]))]

nksu_module = rule(
    implementation = _nksu_module_impl,
    attrs = {
        "srcs": attr.label_list(
            allow_files = True,
            doc = "Module sources: C files, headers, Makefile, Kbuild and Kconfig.",
        ),
        "module_name": attr.string(
            default = "nksu",
            doc = "Module name; determines the output `<name>.ko`.",
        ),
        "config": attr.string_list(
            default = [],
            doc = "kbuild CONFIG_* assignments, e.g. `CONFIG_NKSU=m`.",
        ),
        "out_dir": attr.string(
            default = "",
            doc = "Subdirectory of bazel-bin to place `<name>.ko` in.",
        ),
    },
    doc = "Builds an out-of-tree kernel module by invoking the kernel build system.",
)
