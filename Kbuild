nksu-y += src/nksu.o src/privilege.o src/ioctl.o src/manager.o

nksu-y += src/selinux/rule.o src/selinux/selinux.o src/selinux/policy.o src/selinux/domain.o

nksu-y += src/profile/profile.o
nksu-y += src/ns.o
nksu-y += src/handle.o
nksu-y += src/symbol.o
nksu-y += src/symbol_compat.o

nksu-y += src/fd/anonfd.o
nksu-y += src/fd/eventfd.o
nksu-y += src/fd/shm_hash.o

ifeq ($(CONFIG_NKSU_SYSCALL),y)
	ccflags-y += -DCONFIG_NKSU_SYSCALL=1
	nksu-y += src/syscall/syscall.o
	nksu-y += src/syscall/dispatch.o
	CFLAGS_src/syscall/syscall.o := -O3
	CFLAGS_src/syscall/dispatch.o := -O3
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
    CFLAGS_src/privilege.o := -flto=thin
    CFLAGS_src/ioctl.o := -flto=thin
    CFLAGS_src/manager.o := -flto=thin -O3
    CFLAGS_src/selinux/rule.o := -flto=thin
    CFLAGS_src/selinux/selinux.o := -flto=thin
    CFLAGS_src/selinux/policy.o := -flto=thin
    CFLAGS_src/selinux/domain.o := -flto=thin
    CFLAGS_src/profile/profile.o := -flto=thin
    CFLAGS_src/ns.o := -flto=thin
    CFLAGS_src/handle.o := -flto=thin -O3
    CFLAGS_src/symbol.o := -flto=thin
    CFLAGS_src/symbol_compat.o := -flto=thin
    CFLAGS_src/fd/anonfd.o := -flto=thin
    CFLAGS_src/fd/eventfd.o := -flto=thin
    CFLAGS_src/fd/shm_hash.o := -flto=thin
    ifeq ($(CONFIG_NKSU_SYSCALL),y)
        CFLAGS_src/syscall/syscall.o := -flto=thin -O3
        CFLAGS_src/syscall/dispatch.o := -flto=thin -O3
    endif
endif

ccflags-y += -I$(srctree)/security/selinux
ccflags-y += -I$(srctree)/security/selinux/include
ccflags-y += -I$(IDIR)
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

# Objects that call resolved (unexported) kernel functions through pointers.
# With CONFIG_CFI_CLANG every indirect call is type-hash checked, and a hash
# mismatch with the running kernel panics in __cfi_check. Disable CFI only for
# these objects; files whose callbacks the kernel invokes (tracepoints, file
# operations) keep CFI so kernel -> module calls still pass.
NKSU_NOCFI := -fno-sanitize=cfi
CFLAGS_src/privilege.o += $(NKSU_NOCFI)
CFLAGS_src/ns.o += $(NKSU_NOCFI)
CFLAGS_src/selinux/selinux.o += $(NKSU_NOCFI)
CFLAGS_src/selinux/policy.o += $(NKSU_NOCFI)
CFLAGS_src/selinux/rule.o += $(NKSU_NOCFI)
CFLAGS_src/selinux/domain.o += $(NKSU_NOCFI)

ccflags-y += -std=gnu99
ccflags-y += -Wno-unused-variable
ccflags-y += -Wno-declaration-after-statement
ccflags-y += -Wno-unused-function
ccflags-y += -Werror=implicit-function-declaration
ccflags-y += -Werror=return-type

# CFLAGS_src/manager.o  := -O3
# CFLAGS_src/handle.o   := -O3