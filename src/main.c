// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2019 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 * Copyright (C) 2024 AmneziaVPN <admin@amnezia.org>. All Rights Reserved.
 */

#include "version.h"
#include "uwg_identity.h"
#include "device.h"
#include "noise.h"
#include "queueing.h"
#include "ratelimiter.h"
#include "netlink.h"
#include "uapi/wireguard.h"

#include <linux/init.h>
#include <linux/module.h>
#include <net/genetlink.h>
#include <net/rtnetlink.h>

static int __init wg_mod_init(void)
{
	int ret;

	ret = wg_allowedips_slab_init();
	if (ret < 0)
		goto err_allowedips;

#ifdef DEBUG
	ret = -ENOTRECOVERABLE;
	if (!wg_allowedips_selftest() || !wg_packet_counter_selftest() ||
	    !wg_ratelimiter_selftest())
		goto err_peer;
#endif
	wg_noise_init();

	ret = wg_peer_init();
	if (ret < 0)
		goto err_peer;

	ret = wg_device_init();
	if (ret < 0)
		goto err_device;

	ret = wg_genetlink_init();
	if (ret < 0)
		goto err_netlink;

	uwg_cc_debugfs_init();

	pr_info(UWG_NAME " " WIREGUARD_VERSION " loaded. See github.com/ivlev-dev/umbrawg-linux-kernel-module for information.\n");
	pr_info("Copyright (C) 2015-2019 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.\n");
	pr_info("Copyright (C) 2024-2025 AmneziaVPN <admin@amnezia.org>. All Rights Reserved.\n");

	return 0;

err_netlink:
	wg_device_uninit();
err_device:
	wg_peer_uninit();
err_peer:
	wg_allowedips_slab_uninit();
err_allowedips:
	return ret;
}

static void __exit wg_mod_exit(void)
{
	uwg_cc_debugfs_uninit();
	wg_genetlink_uninit();
	wg_device_uninit();
	wg_peer_uninit();
	wg_allowedips_slab_uninit();
}

module_param(bogus_endpoints, int, 0600);
module_param(bogus_endpoints_prefix, charp, 0600);
module_param(bogus_endpoints_prefix6, charp, 0600);
module_init(wg_mod_init);
module_exit(wg_mod_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION(UWG_NAME " secure network tunnel");
MODULE_AUTHOR("Jason A. Donenfeld <Jason@zx2c4.com>, AmneziaVPN <admin@amnezia.org>");
MODULE_VERSION(WIREGUARD_VERSION);
MODULE_ALIAS_RTNL_LINK(UWG_NAME);
MODULE_ALIAS_GENL_FAMILY(UWG_NAME);
MODULE_INFO(intree, "Y");
