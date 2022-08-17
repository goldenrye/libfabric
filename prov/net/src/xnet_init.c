/*
 * Copyright (c) 2017-2022 Intel Corporation. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *	   Redistribution and use in source and binary forms, with or
 *	   without modification, are permitted provided that the following
 *	   conditions are met:
 *
 *		- Redistributions of source code must retain the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer.
 *
 *		- Redistributions in binary form must reproduce the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer in the documentation and/or other materials
 *		  provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <rdma/fi_errno.h>
#include <rdma/fabric.h>

#include <ofi_prov.h>
#include "xnet.h"

#include <sys/types.h>
#include <ofi_util.h>
#include <stdlib.h>


static char xnet_prov_name[FI_NAME_MAX] = "net";

static int xnet_getinfo(uint32_t version, const char *node, const char *service,
			uint64_t flags, const struct fi_info *hints,
			struct fi_info **info)
{
	return ofi_ip_getinfo(&xnet_util_prov, version, node, service, flags,
			     hints, info);
}

struct xnet_port_range xnet_ports = {
	.low  = 0,
	.high = 0,
};

int xnet_nodelay = -1;

int xnet_staging_sbuf_size = 9000;
int xnet_prefetch_rbuf_size = 9000;
size_t xnet_default_tx_size = 256;
size_t xnet_default_rx_size = 256;
size_t xnet_zerocopy_size = SIZE_MAX;
int xnet_poll_fairness = 0;
int xnet_poll_cooldown = 0;
int xnet_disable_autoprog;


static void xnet_init_env(void)
{
	char *param = NULL;
	size_t tx_size;
	size_t rx_size;

	/* Allow renaming the provider for testing */
	fi_param_define(&xnet_prov, "prov_name", FI_PARAM_STRING,
			"Rename provider for testing");
	fi_param_get_str(&xnet_prov, "prov_name", &param);
	if (param && strlen(param) && strlen(param) < sizeof(xnet_prov_name))
		strncpy(xnet_prov_name, param, strlen(param));

	/* Checked in util code */
	fi_param_define(&xnet_prov, "iface", FI_PARAM_STRING,
			"Specify interface name");

	fi_param_define(&xnet_prov, "port_low_range", FI_PARAM_INT,
			"define port low range");
	fi_param_define(&xnet_prov, "port_high_range", FI_PARAM_INT,
			"define port high range");
	fi_param_get_int(&xnet_prov, "port_high_range", &xnet_ports.high);
	fi_param_get_int(&xnet_prov, "port_low_range", &xnet_ports.low);

	if (xnet_ports.high > XNET_PORT_MAX_RANGE)
		xnet_ports.high = XNET_PORT_MAX_RANGE;

	if (xnet_ports.low < 0 || xnet_ports.high < 0 ||
	    xnet_ports.low > xnet_ports.high) {
		FI_WARN(&xnet_prov, FI_LOG_EP_CTRL,"User provided "
			"port range invalid. Ignoring. \n");
		xnet_ports.low  = 0;
		xnet_ports.high = 0;
	}

	fi_param_define(&xnet_prov, "tx_size", FI_PARAM_SIZE_T,
			"define default tx context size (default: %zu)",
			xnet_default_tx_size);
	fi_param_define(&xnet_prov, "rx_size", FI_PARAM_SIZE_T,
			"define default rx context size (default: %zu)",
			xnet_default_rx_size);
	if (!fi_param_get_size_t(&xnet_prov, "tx_size", &tx_size))
		xnet_default_tx_size = tx_size;
	if (!fi_param_get_size_t(&xnet_prov, "rx_size", &rx_size))
		xnet_default_rx_size = rx_size;

	fi_param_define(&xnet_prov, "nodelay", FI_PARAM_BOOL,
			"overrides default TCP_NODELAY socket setting");
	fi_param_get_bool(&xnet_prov, "nodelay", &xnet_nodelay);

	fi_param_define(&xnet_prov, "staging_sbuf_size", FI_PARAM_INT,
			"size of buffer used to coalesce iovec's or "
			"send requests before posting to the kernel, "
			"set to 0 to disable");
	fi_param_define(&xnet_prov, "prefetch_rbuf_size", FI_PARAM_INT,
			"size of buffer used to prefetch received data from "
			"the kernel, set to 0 to disable");
	fi_param_define(&xnet_prov, "zerocopy_size", FI_PARAM_SIZE_T,
			"lower threshold where zero copy transfers will be "
			"used, if supported by the platform, set to -1 to "
			"disable (default: %zu)", xnet_zerocopy_size);
	fi_param_get_int(&xnet_prov, "staging_sbuf_size",
			 &xnet_staging_sbuf_size);
	fi_param_get_int(&xnet_prov, "prefetch_rbuf_size",
			 &xnet_prefetch_rbuf_size);
	fi_param_get_size_t(&xnet_prov, "zerocopy_size", &xnet_zerocopy_size);

	fi_param_define(&xnet_prov, "poll_fairness", FI_PARAM_INT,
			"This counter value balances calling poll() on a list "
			"of sockets marked as active, versus all sockets being "
			"monitored.  This variable controls the number of times "
			"that the active sockets are checked before the full "
			"set is.  A value of 0 disables the active "
			"list.  Default (%d)", xnet_poll_fairness);
	fi_param_get_int(&xnet_prov, "poll_fairness", &xnet_poll_fairness);
	fi_param_define(&xnet_prov, "poll_cooldown", FI_PARAM_INT,
			"This value only applies if poll_fairness is active. "
			"This determines the number of iterations that a socket "
			"will remain marked as active without receiving data "
			"before being removed from the active set. "
			"Default (%d)", xnet_poll_cooldown);
	fi_param_get_int(&xnet_prov, "poll_cooldown", &xnet_poll_cooldown);

	fi_param_define(&xnet_prov, "disable_auto_progress", FI_PARAM_BOOL,
			"prevent auto-progress thread from starting");
	fi_param_get_bool(&xnet_prov, "disable_auto_progress",
			&xnet_disable_autoprog);
}

static void xnet_fini(void)
{
	/* empty as of now */
}

struct fi_provider xnet_prov = {
	.name = xnet_prov_name,
	.version = OFI_VERSION_DEF_PROV,
	.fi_version = OFI_VERSION_LATEST,
	.getinfo = xnet_getinfo,
	.fabric = xnet_create_fabric,
	.cleanup = xnet_fini,
};

XNET_INI
{
#if HAVE_NET_DL
	ofi_pmem_init();
#endif
	xnet_init_env();
	return &xnet_prov;
}
