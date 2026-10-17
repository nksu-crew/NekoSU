// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Volume-key safe mode.
 *
 * The module watches KEY_VOLUMEUP from early boot.  Three key-down events set
 * a "safe mode requested" flag.  The kernel does not write any file and does
 * not reboot: the flag is reported through the control ioctl
 * (IOC_GET_SAFEMODE) and ksud acts on it -- creating
 * /data/adb/nksu/safe_module and skipping module loading -- then disarms the
 * watcher with IOC_DISARM_SAFEMODE.
 *
 * An ioctl is used instead of a procfs node because the control fd is an
 * anonymous inode, so SELinux path checks never get in the way.
 *
 * Key detection uses the input subsystem: a small input_handler matching
 * EV_KEY/KEY_VOLUMEUP devices, opened on connect.  The event callback runs in
 * the input device's context (possibly atomic), so it only bumps a counter and
 * latches the flag.
 */

#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "klog.h"
#include "boot/safe_mode.h"

#define NKSU_SAFE_PRESSES   3

static atomic_t nksu_safe_presses = ATOMIC_INIT(0);
/* Latched once the press threshold is reached. */
static bool nksu_safe_requested __read_mostly;
/* Set once the watcher is told to stop (ksud's last stage, or no ksud). */
static bool nksu_safe_disarmed __read_mostly;
static bool nksu_safe_input_registered;

bool nksu_safe_mode_requested(void)
{
	return READ_ONCE(nksu_safe_requested);
}

/* ---- input handler: count volume-up key-down events ---- */

/*
 * Use the per-event callback: its signature is unchanged from 5.10 through
 * 6.12, while input_handler->events() gained a return value and dropped the
 * const on its value array in newer kernels.
 */
static void nksu_safe_event(struct input_handle *handle, unsigned int type,
			    unsigned int code, int value)
{
	if (READ_ONCE(nksu_safe_disarmed) || READ_ONCE(nksu_safe_requested))
		return;

	/* value 1 is a key-down; 2 is autorepeat and 0 is key-up. */
	if (type == EV_KEY && code == KEY_VOLUMEUP && value == 1) {
		if (atomic_inc_return(&nksu_safe_presses) >= NKSU_SAFE_PRESSES) {
			WRITE_ONCE(nksu_safe_requested, true);
			pr_alert("[safe_mode] volume-up pressed %d times, safe mode requested\n",
				 NKSU_SAFE_PRESSES);
		}
	}
}

static int nksu_safe_connect(struct input_handler *handler,
			     struct input_dev *dev,
			     const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "nksu-safe";

	error = input_register_handle(handle);
	if (error)
		goto err_free;

	error = input_open_device(handle);
	if (error)
		goto err_unregister;

	return 0;

err_unregister:
	input_unregister_handle(handle);
err_free:
	kfree(handle);
	return error;
}

static void nksu_safe_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

/* Only attach to devices that actually expose a volume-up key. */
static const struct input_device_id nksu_safe_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			 INPUT_DEVICE_ID_MATCH_KEYBIT,
		.evbit = { BIT_MASK(EV_KEY) },
		.keybit = { [BIT_WORD(KEY_VOLUMEUP)] = BIT_MASK(KEY_VOLUMEUP) },
	},
	{ },
};

static struct input_handler nksu_safe_handler = {
	.event = nksu_safe_event,
	.connect = nksu_safe_connect,
	.disconnect = nksu_safe_disconnect,
	.name = "nksu-safe",
	.id_table = nksu_safe_ids,
};

/* ---- public interface ---- */

void nksu_safe_mode_disable(void)
{
	if (READ_ONCE(nksu_safe_disarmed))
		return;

	WRITE_ONCE(nksu_safe_disarmed, true);
	pr_info("[safe_mode] volume-up watcher disarmed\n");
}

int nksu_safe_mode_init(void)
{
	int ret;

	/*
	 * The manager scan may have already disarmed the watcher before this
	 * component came up; in that case stay off (the flag is not reset here).
	 */
	if (READ_ONCE(nksu_safe_disarmed))
		return 0;

	atomic_set(&nksu_safe_presses, 0);

	/*
	 * Safe mode is an optional feature: any failure here disables it but
	 * must not abort the rest of the module's bring-up, so every error path
	 * cleans up and returns success.
	 */
	ret = input_register_handler(&nksu_safe_handler);
	if (ret) {
		pr_err("[safe_mode] input handler registration failed: %d\n",
		       ret);
		return 0;
	}
	nksu_safe_input_registered = true;

	pr_info("[safe_mode] watching volume-up keys for safe mode\n");
	return 0;
}

void nksu_safe_mode_exit(void)
{
	if (nksu_safe_input_registered) {
		input_unregister_handler(&nksu_safe_handler);
		nksu_safe_input_registered = false;
	}
}
