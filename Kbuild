nksu-y += src/nksu.o
nksu-y += src/boot/hook_init.o src/boot/init_rc.o
nksu-y += src/hook/handle.o src/hook/syscall.o src/hook/dispatch.o src/hook/get_arg.o
nksu-y += src/manager/manager.o src/manager/ioctl.o
nksu-y += src/module/module.o
nksu-y += src/privilege/privilege.o src/privilege/profile.o src/privilege/ns.o
nksu-y += src/spawn/spawn.o
nksu-y += src/symbol/symbol.o src/symbol/symbol_compat.o
nksu-y += src/fd/anonfd.o src/fd/eventfd.o src/fd/shm_hash.o
nksu-y += src/selinux/rule.o src/selinux/rule_file.o src/selinux/selinux.o src/selinux/policy.o

ifeq ($(CONFIG_NKSU_SYSCALL),y)
	ccflags-y += -DCONFIG_NKSU_SYSCALL=1
endif

obj-$(CONFIG_NKSU) += nksu.o

ifeq ($(CONFIG_NKSU_DEBUG),y)
	ccflags-y += -DCONFIG_NKSU_DEBUG=1
else
	ccflags-y += -O3
endif

ifeq ($(CONFIG_LTO_CLANG),y)
    # Clang LTO
    ccflags-y += -flto=thin
    CFLAGS_nksu.o := -flto=thin
    CFLAGS_src/privilege/privilege.o := -flto=thin
    CFLAGS_src/manager/ioctl.o := -flto=thin
    CFLAGS_src/manager/manager.o := -flto=thin -O3
    CFLAGS_src/module/module.o := -flto=thin
    CFLAGS_src/boot/init_rc.o := -flto=thin
    CFLAGS_src/selinux/rule.o := -flto=thin
    CFLAGS_src/selinux/rule_file.o := -flto=thin
    CFLAGS_src/selinux/selinux.o := -flto=thin
    CFLAGS_src/selinux/policy.o := -flto=thin
    CFLAGS_src/privilege/profile.o := -flto=thin
    CFLAGS_src/privilege/ns.o := -flto=thin
    CFLAGS_src/hook/handle.o := -flto=thin -O3
    CFLAGS_src/spawn/spawn.o := -flto=thin
    CFLAGS_src/symbol/symbol.o := -flto=thin
    CFLAGS_src/symbol/symbol_compat.o := -flto=thin
    CFLAGS_src/fd/anonfd.o := -flto=thin
    CFLAGS_src/fd/eventfd.o := -flto=thin
    CFLAGS_src/fd/shm_hash.o := -flto=thin
    ifeq ($(CONFIG_NKSU_SYSCALL),y)
        CFLAGS_src/hook/syscall.o := -flto=thin -O3
        CFLAGS_src/hook/dispatch.o := -flto=thin -O3
    endif
endif

ccflags-y += -I$(srctree)/security/selinux
ccflags-y += -I$(srctree)/security/selinux/include
ccflags-y += -I$(IDIR)
# Source root: headers are grouped per subsystem, e.g. "hook/handle.h".
ccflags-y += -I$(IDIR)/..
ccflags-y += -I$(objtree)/security/selinux
ccflags-y += -include $(srctree)/include/uapi/asm-generic/errno.h

# Embed the source commit so debug builds can report exactly what is running.
NKSU_SRCDIR := $(if $(src),$(src),$(M))
ifneq ($(NKSU_SRCDIR),)
NKSU_GIT_COMMIT := $(shell git -C $(NKSU_SRCDIR) rev-parse --short=12 HEAD 2>/dev/null)
endif
ifeq ($(NKSU_GIT_COMMIT),)
NKSU_GIT_COMMIT := unknown
endif
ccflags-y += -DNKSU_GIT_COMMIT=\"$(NKSU_GIT_COMMIT)\"

# GNU C11.  The kernel headers use GNU extensions (typeof, statement
# expressions, ...), so the GNU dialect is required; a strict -std=c11
# would not compile them.  This overrides the kernel's own -std=gnu89.
ccflags-y += -std=gnu11
ccflags-y += -Wno-unused-variable
ccflags-y += -Wno-declaration-after-statement
ccflags-y += -Wno-unused-function
ccflags-y += -Werror=implicit-function-declaration
ccflags-y += -Werror=return-type

# CFLAGS_src/manager/manager.o := -O3
# CFLAGS_src/hook/handle.o     := -O3