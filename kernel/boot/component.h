/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NKSU_BOOT_COMPONENT_H
#define NKSU_BOOT_COMPONENT_H

enum nksu_init_stage {
	NKSU_STAGE_CORE = 0,
	NKSU_STAGE_EARLY,
	NKSU_STAGE_SELINUX,
	NKSU_STAGE_FEATURE,
	NKSU_STAGE_COUNT,
};

struct nksu_component {
	const char *name;
	enum nksu_init_stage stage;
	int (*init)(void);
	void (*exit)(void);
};

#define NKSU_COMPONENT(_stage, _name, _init, _exit) \
	{ .name = (_name), .stage = (_stage), .init = (_init), .exit = (_exit) }
#define NKSU_CORE_COMPONENT(_name, _init, _exit) \
	NKSU_COMPONENT(NKSU_STAGE_CORE, _name, _init, _exit)
#define NKSU_EARLY_COMPONENT(_name, _init, _exit) \
	NKSU_COMPONENT(NKSU_STAGE_EARLY, _name, _init, _exit)
#define NKSU_SELINUX_COMPONENT(_name, _init, _exit) \
	NKSU_COMPONENT(NKSU_STAGE_SELINUX, _name, _init, _exit)
#define NKSU_FEATURE_COMPONENT(_name, _init, _exit) \
	NKSU_COMPONENT(NKSU_STAGE_FEATURE, _name, _init, _exit)

/* Idempotent: components already initialized by an earlier stage are skipped. */
int nksu_components_init(enum nksu_init_stage stage);
int nksu_components_init_all(void);

/* Reverse-order teardown of @stage and every later stage. */
void nksu_components_exit_from(enum nksu_init_stage stage);

#endif /* NKSU_BOOT_COMPONENT_H */
