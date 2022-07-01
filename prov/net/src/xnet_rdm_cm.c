/*
 * Copyright (c) 2016-2022 Intel Corporation, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
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

#include <stdlib.h>
#include <string.h>
#include <poll.h>

#include <ofi.h>
#include <ofi_util.h>
#include "xnet.h"


/* Include cm_msg with connect() data.  If the connection is accepted,
 * return the version.  The returned version must be <= the requested
 * version, and is used by the active side to fallback to an older
 * protocol version.
 */
struct xnet_rdm_cm {
	uint8_t version;
	uint8_t resv;
	uint16_t port;
	uint32_t pid;
};

static int xnet_match_rdm(struct slist_entry *item, const void *arg)
{
	struct xnet_event *event;
	event = container_of(item, struct xnet_event, list_entry);
	return event->rdm == arg;
}

static void xnet_close_conn(struct xnet_conn *conn)
{
	struct xnet_event *event;
	struct slist_entry *item;

	FI_DBG(&xnet_prov, FI_LOG_EP_CTRL, "closing conn %p\n", conn);
	assert(xnet_progress_locked(xnet_rdm2_progress(conn->rdm)));
	dlist_remove_init(&conn->loopback_entry);

	if (conn->ep) {
		fi_close(&conn->ep->util_ep.ep_fid.fid);
		do {
			item = slist_remove_first_match(
				&xnet_rdm2_progress(conn->rdm)->event_list,
				xnet_match_rdm, conn->rdm);
			if (!item)
				break;

			event = container_of(item, struct xnet_event, list_entry);
			free(event);
		} while (item);
		if (conn->ep->peer)
			util_put_peer(conn->ep->peer);
	}

	conn->ep = NULL;
}

/* MSG EPs under an RDM EP do not write events to the EQ. */
static int xnet_bind_conn(struct xnet_rdm *rdm, struct xnet_ep *ep)
{
	int ret;

	assert(xnet_progress_locked(xnet_rdm2_progress(rdm)));

	ret = fi_ep_bind(&ep->util_ep.ep_fid, &rdm->srx->rx_fid.fid, 0);
	if (ret)
		return ret;

	ret = fi_ep_bind(&ep->util_ep.ep_fid,
			 &rdm->util_ep.rx_cq->cq_fid.fid, FI_RECV);
	if (ret)
		return ret;

	ret = fi_ep_bind(&ep->util_ep.ep_fid,
			 &rdm->util_ep.tx_cq->cq_fid.fid, FI_SEND);
	if (ret)
		return ret;

	if (rdm->util_ep.rx_cntr) {
		ret = fi_ep_bind(&ep->util_ep.ep_fid,
				 &rdm->util_ep.rx_cntr->cntr_fid.fid, FI_RECV);
		if (ret)
			return ret;
	}

	if (rdm->util_ep.tx_cntr) {
		ret = fi_ep_bind(&ep->util_ep.ep_fid,
				 &rdm->util_ep.tx_cntr->cntr_fid.fid, FI_SEND);
		if (ret)
			return ret;
	}

	if (rdm->util_ep.rd_cntr) {
		ret = fi_ep_bind(&ep->util_ep.ep_fid,
				 &rdm->util_ep.rd_cntr->cntr_fid.fid, FI_READ);
		if (ret)
			return ret;
	}

	if (rdm->util_ep.wr_cntr) {
		ret = fi_ep_bind(&ep->util_ep.ep_fid,
				 &rdm->util_ep.wr_cntr->cntr_fid.fid, FI_WRITE);
		if (ret)
			return ret;
	}

	if (rdm->util_ep.rem_rd_cntr) {
		ret = fi_ep_bind(&ep->util_ep.ep_fid,
				 &rdm->util_ep.rem_rd_cntr->cntr_fid.fid,
				 FI_REMOTE_READ);
		if (ret)
			return ret;
	}

	if (rdm->util_ep.rem_wr_cntr) {
		ret = fi_ep_bind(&ep->util_ep.ep_fid,
				 &rdm->util_ep.rem_wr_cntr->cntr_fid.fid,
				 FI_REMOTE_WRITE);
		if (ret)
			return ret;
	}

	return 0;
}

static int xnet_open_conn(struct xnet_conn *conn, struct fi_info *info)
{
	struct fid_ep *ep_fid;
	int ret;

	assert(xnet_progress_locked(xnet_rdm2_progress(conn->rdm)));
	ret = fi_endpoint(&conn->rdm->util_ep.domain->domain_fid, info,
			  &ep_fid, conn);
	if (ret) {
		XNET_WARN_ERR(FI_LOG_EP_CTRL, "fi_endpoint", ret);
		return ret;
	}

	conn->ep = container_of(ep_fid, struct xnet_ep, util_ep.ep_fid);
	ret = xnet_bind_conn(conn->rdm, conn->ep);
	if (ret)
		goto err;

	conn->ep->peer = conn->peer;
	rxm_ref_peer(conn->peer);
	ret = fi_enable(&conn->ep->util_ep.ep_fid);
	if (ret) {
		XNET_WARN_ERR(FI_LOG_EP_CTRL, "fi_enable", ret);
		goto err;
	}

	return 0;

err:
	fi_close(&conn->ep->util_ep.ep_fid.fid);
	conn->ep = NULL;
	return ret;
}

static int xnet_rdm_connect(struct xnet_conn *conn)
{
	struct xnet_rdm_cm msg;
	struct fi_info *info;
	int ret;

	FI_DBG(&xnet_prov, FI_LOG_EP_CTRL, "connecting %p\n", conn);
	assert(xnet_progress_locked(xnet_rdm2_progress(conn->rdm)));

	info = conn->rdm->pep->info;
	info->dest_addrlen = info->src_addrlen;

	free(info->dest_addr);
	info->dest_addr = mem_dup(&conn->peer->addr, info->dest_addrlen);
	if (!info->dest_addr)
		return -FI_ENOMEM;

	ret = xnet_open_conn(conn, info);
	if (ret)
		return ret;

	msg.version = XNET_RDM_VERSION;
	msg.pid = htonl((uint32_t) getpid());
	msg.resv = 0;
	msg.port = htons(ofi_addr_get_port(info->src_addr));

	ret = fi_connect(&conn->ep->util_ep.ep_fid, info->dest_addr,
			 &msg, sizeof msg);
	if (ret) {
		XNET_WARN_ERR(FI_LOG_EP_CTRL, "fi_connect", ret);
		goto err;
	}
	return 0;

err:
	xnet_close_conn(conn);
	return ret;
}

static void xnet_free_conn(struct xnet_conn *conn)
{
	struct rxm_av *av;

	FI_DBG(&xnet_prov, FI_LOG_EP_CTRL, "free conn %p\n", conn);
	assert(xnet_progress_locked(xnet_rdm2_progress(conn->rdm)));

	if (conn->flags & XNET_CONN_INDEXED)
		ofi_idm_clear(&conn->rdm->conn_idx_map, conn->peer->index);

	util_put_peer(conn->peer);
	av = container_of(conn->rdm->util_ep.av, struct rxm_av, util_av);
	rxm_av_free_conn(av, conn);
}

void xnet_freeall_conns(struct xnet_rdm *rdm)
{
	struct xnet_conn *conn;
	struct dlist_entry *tmp;
	struct rxm_av *av;
	int i, cnt;

	av = container_of(rdm->util_ep.av, struct rxm_av, util_av);
	assert(xnet_progress_locked(xnet_rdm2_progress(rdm)));

	/* We can't have more connections than the current number of
	 * possible peers.
	 */
	cnt = (int) rxm_av_max_peers(av);
	for (i = 0; i < cnt; i++) {
		conn = ofi_idm_lookup(&rdm->conn_idx_map, i);
		if (!conn)
			continue;

		xnet_close_conn(conn);
		xnet_free_conn(conn);
	}

	dlist_foreach_container_safe(&rdm->loopback_list, struct xnet_conn,
				     conn, loopback_entry, tmp) {
		xnet_close_conn(conn);
		xnet_free_conn(conn);
	}
}

static struct xnet_conn *
xnet_alloc_conn(struct xnet_rdm *rdm, struct util_peer_addr *peer)
{
	struct xnet_conn *conn;
	struct rxm_av *av;

	assert(xnet_progress_locked(xnet_rdm2_progress(rdm)));
	av = container_of(rdm->util_ep.av, struct rxm_av, util_av);
	conn = rxm_av_alloc_conn(av);
	if (!conn) {
		XNET_WARN_ERR(FI_LOG_EP_CTRL, "rxm_av_alloc_conn", -FI_ENOMEM);
		return NULL;
	}

	conn->rdm = rdm;
	conn->flags = 0;
	dlist_init(&conn->loopback_entry);

	conn->peer = peer;
	rxm_ref_peer(peer);

	FI_DBG(&xnet_prov, FI_LOG_EP_CTRL, "allocated conn %p\n", conn);
	return conn;
}

static struct xnet_conn *
xnet_add_conn(struct xnet_rdm *rdm, struct util_peer_addr *peer)
{
	struct xnet_conn *conn;

	assert(xnet_progress_locked(xnet_rdm2_progress(rdm)));
	conn = ofi_idm_lookup(&rdm->conn_idx_map, peer->index);
	if (conn)
		return conn;

	conn = xnet_alloc_conn(rdm, peer);
	if (!conn)
		return NULL;

	if (ofi_idm_set(&rdm->conn_idx_map, peer->index, conn) < 0) {
		xnet_free_conn(conn);
		XNET_WARN_ERR(FI_LOG_EP_CTRL, "ofi_idm_set", -FI_ENOMEM);
		return NULL;
	}

	conn->flags |= XNET_CONN_INDEXED;
	return conn;
}

/* The returned conn is only valid if the function returns success.
 * This is called from data transfer ops, which return ssize_t, so
 * we return that rather than int.
 */
ssize_t xnet_get_conn(struct xnet_rdm *rdm, fi_addr_t addr,
		      struct xnet_conn **conn)
{
	struct util_peer_addr **peer;
	ssize_t ret;

	assert(xnet_progress_locked(xnet_rdm2_progress(rdm)));
	peer = ofi_av_addr_context(rdm->util_ep.av, addr);
	*conn = xnet_add_conn(rdm, *peer);
	if (!*conn)
		return -FI_ENOMEM;

	if (!(*conn)->ep) {
		ret = xnet_rdm_connect(*conn);
		if (ret)
			return ret;
	}

	if ((*conn)->ep->state != XNET_CONNECTED)
		return -FI_EAGAIN;

	return 0;
}

void xnet_process_connect(struct fi_eq_cm_entry *cm_entry)
{
	struct xnet_rdm_cm *msg;
	struct xnet_conn *conn;

	assert(cm_entry->fid->fclass == XNET_CLASS_CM);
	conn = cm_entry->fid->context;

	assert(xnet_progress_locked(xnet_rdm2_progress(conn->rdm)));
	msg = (struct xnet_rdm_cm *) cm_entry->data;
	conn->remote_pid = ntohl(msg->pid);
}

static void xnet_process_connreq(struct fi_eq_cm_entry *cm_entry)
{
	struct xnet_rdm *rdm;
	struct xnet_rdm_cm *msg;
	union ofi_sock_ip peer_addr;
	struct util_peer_addr *peer;
	struct xnet_conn *conn;
	struct rxm_av *av;
	int ret, cmp;

	assert(cm_entry->fid->fclass == FI_CLASS_PEP);
	rdm = cm_entry->fid->context;
	assert(xnet_progress_locked(xnet_rdm2_progress(rdm)));
	msg = (struct xnet_rdm_cm *) cm_entry->data;

	memcpy(&peer_addr, cm_entry->info->dest_addr,
	       cm_entry->info->dest_addrlen);
	ofi_addr_set_port(&peer_addr.sa, ntohs(msg->port));

	av = container_of(rdm->util_ep.av, struct rxm_av, util_av);
	peer = util_get_peer(av, &peer_addr);
	if (!peer) {
		XNET_WARN_ERR(FI_LOG_EP_CTRL, "util_get_peer", -FI_ENOMEM);
		goto reject;
	}

	conn = xnet_add_conn(rdm, peer);
	if (!conn)
		goto put;

	FI_INFO(&xnet_prov, FI_LOG_EP_CTRL, "connreq for %p\n", conn);
	if (!conn->ep)
		goto accept;

	switch (conn->ep->state) {
	case XNET_CONNECTING:
	case XNET_REQ_SENT:
		/* simultaneous connections */
		cmp = ofi_addr_cmp(&xnet_prov, &peer_addr.sa, &rdm->addr.sa);
		if (cmp < 0) {
			/* let our request finish */
			FI_INFO(&xnet_prov, FI_LOG_EP_CTRL,
				"simultaneous, reject peer %p\n", conn);
			goto put;
		} else if (cmp > 0) {
			/* accept peer's request */
			FI_INFO(&xnet_prov, FI_LOG_EP_CTRL,
				"simultaneous, accept peer %p\n", conn);
			xnet_close_conn(conn);
		} else {
			/* connecting to ourself, create loopback conn */
			FI_INFO(&xnet_prov, FI_LOG_EP_CTRL,
				"loopback conn %p\n", conn);
			conn = xnet_alloc_conn(rdm, peer);
			if (!conn)
				goto put;

			dlist_insert_tail(&conn->loopback_entry,
					  &rdm->loopback_list);
		}
		break;
	case XNET_ACCEPTING:
	case XNET_CONNECTED:
		if (conn->remote_pid == ntohl(msg->pid)) {
			FI_INFO(&xnet_prov, FI_LOG_EP_CTRL,
				"simultaneous, reject peer\n");
			goto put;
		} else {
			FI_INFO(&xnet_prov, FI_LOG_EP_CTRL,
				"old connection exists, replacing %p\n", conn);
			xnet_close_conn(conn);
		}
		break;
	default:
		assert(0);
		xnet_close_conn(conn);
		break;
	}

accept:
	conn->remote_pid = ntohl(msg->pid);
	ret = xnet_open_conn(conn, cm_entry->info);
	if (ret)
		goto free;

	msg->pid = htonl((uint32_t) getpid());
	ret = fi_accept(&conn->ep->util_ep.ep_fid, msg, sizeof(*msg));
	if (ret)
		goto close;

	return;

close:
	xnet_close_conn(conn);
free:
	xnet_free_conn(conn);
put:
	util_put_peer(peer);
reject:
	(void) fi_reject(&rdm->pep->util_pep.pep_fid, cm_entry->info->handle,
			 msg, sizeof(*msg));
	fi_freeinfo(cm_entry->info);
}

void xnet_handle_events(struct xnet_progress *progress)
{
	struct xnet_event *event;
	struct slist_entry *item;
	struct xnet_rdm_cm *msg;
	struct xnet_conn *conn;

	ofi_genlock_held(&progress->rdm_lock);
	while (!slist_empty(&progress->event_list)) {
		item = slist_remove_head(&progress->event_list);
		event = container_of(item, struct xnet_event, list_entry);

		FI_INFO(&xnet_prov, FI_LOG_EP_CTRL, "event %s\n",
			fi_tostr(&event->event, FI_TYPE_EQ_EVENT));

		switch (event->event) {
		case FI_CONNREQ:
			xnet_process_connreq(&event->cm_entry);
			break;
		case FI_CONNECTED:
			conn = event->cm_entry.fid->context;
			msg = (struct xnet_rdm_cm *) event->cm_entry.data;
			conn->remote_pid = ntohl(msg->pid);
			break;
		case FI_SHUTDOWN:
			conn = event->cm_entry.fid->context;
			xnet_close_conn(conn);
			xnet_free_conn(conn);
			break;
		default:
			assert(0);
			break;
		}
		free(event);
	};
}
