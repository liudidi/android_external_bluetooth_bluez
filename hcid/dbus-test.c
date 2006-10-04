/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2006  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>

#include <dbus/dbus.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>

#include "hcid.h"
#include "dbus.h"

#define L2INFO_TIMEOUT (2 * 1000)

enum {
	AUDIT_STATE_MTU = 0,
	AUDIT_STATE_FEATURES
};

struct audit {
	bdaddr_t addr;

	/* We need to store the path instead of a pointer to the data
	 * because by the time the audit is processed the adapter
	 * might have gotten removed. Storing only the path allows us to
	 * detect this scenario */
	char adapter_path[PATH_MAX];

	char *requestor;
	DBusConnection *conn;

	GIOChannel *io;
	guint io_id;

	guint timeout;

	int state;

	gboolean got_mtu;
	gboolean got_mask;

	uint16_t mtu;
	uint32_t mask;
};

static struct slist *audits = NULL;

static struct audit *audit_new(DBusConnection *conn, DBusMessage *msg, bdaddr_t *addr)
{
	struct audit *audit;
	const char *path;
	const char *requestor;

	path = dbus_message_get_path(msg);
	requestor = dbus_message_get_sender(msg);

	audit = malloc(sizeof(struct audit));
	if (!audit)
		return NULL;
	memset(audit, 0, sizeof(struct audit));

	audit->requestor = strdup(requestor);
	if (!audit->requestor) {
		free(audit);
		return NULL;
	}

	bacpy(&audit->addr, addr);
	strncpy(audit->adapter_path, path, sizeof(audit->adapter_path) - 1);
	audit->conn = dbus_connection_ref(conn);

	return audit;
}

static void audit_free(struct audit *audit)
{
	free(audit->requestor);
	dbus_connection_unref(audit->conn);
	free(audit);
}

static void audit_requestor_exited(const char *name, struct audit *audit)
{
	debug("AuditRemoteDevice requestor %s exited", name);
	audits = slist_remove(audits, audit);
	if (audit->io)
		g_io_channel_close(audit->io);
	if (audit->timeout)
		g_timeout_remove(audit->timeout);
	audit_free(audit);
}

int audit_addr_cmp(const void *a, const void *b)
{
	const struct audit *audit = a;
	const bdaddr_t *addr = b;

	return bacmp(&audit->addr, addr);
}

static gboolean audit_in_progress(void)
{
	struct slist *l;

	for (l = audits; l != NULL; l = l->next) {
		struct audit *audit = l->data;
		if (audit->io)
			return TRUE;
	}

	return FALSE;
}

static gboolean l2raw_input_timer(struct audit *audit)
{
	error("l2raw_input_timer: Timed out while waiting for input");

	g_io_channel_close(audit->io);
	audits = slist_remove(audits, audit);
	name_listener_remove(audit->conn, audit->requestor,
				(name_cb_t) audit_requestor_exited, audit);
	audit_free(audit);

	return FALSE;
}

static void handle_mtu_response(struct audit *audit, const l2cap_info_rsp *rsp)
{
	switch (btohs(rsp->result)) {
	case 0x0000:
		audit->mtu = btohs(bt_get_unaligned((uint16_t *) rsp->data));
		audit->got_mtu = TRUE;
		debug("Connectionless MTU size is %d", audit->mtu);
		break;
	case 0x0001:
		debug("Connectionless MTU is not supported");
		break;
	}
}

static void handle_features_response(struct audit *audit, const l2cap_info_rsp *rsp)
{
	switch (btohs(rsp->result)) {
	case 0x0000:
		audit->mask = btohl(bt_get_unaligned((uint32_t *) rsp->data));
		audit->got_mask = TRUE;
		debug("Extended feature mask is 0x%04x", audit->mask);
		if (audit->mask & 0x01)
			debug("  Flow control mode");
		if (audit->mask & 0x02)
			debug("  Retransmission mode");
		if (audit->mask & 0x04)
			debug("  Bi-directional QoS");
		break;
	case 0x0001:
		debug("Extended feature mask is not supported");
		break;
	}
}

static gboolean l2raw_data_callback(GIOChannel *io, GIOCondition cond, struct audit *audit)
{
	unsigned char buf[48];
	l2cap_cmd_hdr *cmd = (l2cap_cmd_hdr *) buf;
	l2cap_info_req *req = (l2cap_info_req *) (buf + L2CAP_CMD_HDR_SIZE);
	l2cap_info_rsp *rsp = (l2cap_info_rsp *) (buf + L2CAP_CMD_HDR_SIZE);
	int sk;

	if (cond & G_IO_NVAL) {
		g_io_channel_unref(io);
		return FALSE;
	}

	if (audit->timeout) {
		g_timeout_remove(audit->timeout);
		audit->timeout = 0;
	}

	if (cond & (G_IO_ERR | G_IO_HUP))
		goto failed;

	sk = g_io_channel_unix_get_fd(io);

	memset(buf, 0, sizeof(buf));

	if (recv(sk, buf, L2CAP_CMD_HDR_SIZE + L2CAP_INFO_RSP_SIZE + 2, 0) < 0) {
		error("Can't receive info response: %s (%d)", strerror(errno), errno);
		goto failed;
	}

	switch (audit->state) {
	case AUDIT_STATE_MTU:
		handle_mtu_response(audit, rsp);

		memset(buf, 0, sizeof(buf));
		cmd->code  = L2CAP_INFO_REQ;
		cmd->ident = 42;
		cmd->len   = htobs(2);
		req->type  = htobs(0x0002);

		if (send(sk, buf, L2CAP_CMD_HDR_SIZE + L2CAP_INFO_REQ_SIZE, 0) < 0) {
			error("Can't send info request:", strerror(errno), errno);
			goto failed;
		}

		audit->timeout = g_timeout_add(L2INFO_TIMEOUT, (GSourceFunc)
						l2raw_input_timer, audit);

		audit->state = AUDIT_STATE_FEATURES;

		return TRUE;

	case AUDIT_STATE_FEATURES:
		handle_features_response(audit, rsp);
		break;
	}

failed:
	g_io_channel_close(io);
	g_io_channel_unref(io);
	audits = slist_remove(audits, audit);
	name_listener_remove(audit->conn, audit->requestor,
				(name_cb_t) audit_requestor_exited, audit);
	audit_free(audit);

	return FALSE;
}

static gboolean l2raw_connect_complete(GIOChannel *io, GIOCondition cond, struct audit *audit)
{
	unsigned char buf[48];
	l2cap_cmd_hdr *cmd = (l2cap_cmd_hdr *) buf;
	l2cap_info_req *req = (l2cap_info_req *) (buf + L2CAP_CMD_HDR_SIZE);
	socklen_t len;
	int sk, ret;

	if (cond & G_IO_NVAL) {
		g_io_channel_unref(io);
		return FALSE;
	}

	if (cond & (G_IO_ERR | G_IO_HUP)) {
		error("Error on raw l2cap socket");
		goto failed;
	}

	sk = g_io_channel_unix_get_fd(io);

	len = sizeof(ret);
	if (getsockopt(sk, SOL_SOCKET, SO_ERROR, &ret, &len) < 0) {
		error("Can't get socket error: %s (%d)", strerror(errno), errno);
		goto failed;
	}

	if (ret != 0) {
		error("l2raw_connect failed: %s (%d)", strerror(ret), ret);
		goto failed;
	}

	debug("AuditRemoteDevice: connected");

	/* Send L2CAP info request */
	memset(buf, 0, sizeof(buf));
	cmd->code  = L2CAP_INFO_REQ;
	cmd->ident = 42;
	cmd->len   = htobs(2);
	req->type  = htobs(0x0001);

	if (send(sk, buf, L2CAP_CMD_HDR_SIZE + L2CAP_INFO_REQ_SIZE, 0) < 0) {
		error("Can't send info request: %s (%d)", strerror(errno), errno);
		goto failed;
	}

	audit->timeout = g_timeout_add(L2INFO_TIMEOUT, (GSourceFunc)
			l2raw_input_timer, audit);

	audit->io_id = g_io_add_watch(audit->io,
					G_IO_IN | G_IO_NVAL | G_IO_HUP | G_IO_ERR,
					(GIOFunc) l2raw_data_callback, audit);

	return FALSE;

failed:
	g_io_channel_close(io);
	g_io_channel_unref(io);
	audits = slist_remove(audits, audit);
	name_listener_remove(audit->conn, audit->requestor,
				(name_cb_t) audit_requestor_exited, audit);
	audit_free(audit);

	return FALSE;
}

static DBusHandlerResult audit_remote_device(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	DBusError err;
	bdaddr_t dba;
	const char *address;
	struct audit *audit;
	struct hci_dbus_data *dbus_data = data;

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &address,
				DBUS_TYPE_INVALID);
	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if (check_address(address) < 0)
		return error_invalid_arguments(conn, msg);

	str2ba(address, &dba);

	/* check if there is a pending discover: requested by D-Bus/non clients */
	if (dbus_data->disc_active || (dbus_data->pdisc_active && !dbus_data->pinq_idle))
		return error_discover_in_progress(conn, msg);

	pending_remote_name_cancel(dbus_data);

	if (dbus_data->bonding)
		return error_bonding_in_progress(conn, msg);

	if (slist_find(dbus_data->pin_reqs, &dba, pin_req_cmp))
		return error_bonding_in_progress(conn, msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	audit = audit_new(conn, msg, &dba);
	if (!audit) {
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}

	if (!audit_in_progress()) {
		int sk;

		sk = l2raw_connect(dbus_data->address, &dba);
		if (sk < 0) {
			audit_free(audit);
			dbus_message_unref(reply);
			return error_connection_attempt_failed(conn, msg, 0);
		}

		audit->io = g_io_channel_unix_new(sk);
		audit->io_id = g_io_add_watch(audit->io,
						G_IO_OUT | G_IO_NVAL | G_IO_HUP | G_IO_ERR,
						(GIOFunc) l2raw_connect_complete, audit);
	}

	name_listener_add(conn, dbus_message_get_sender(msg),
				(name_cb_t) audit_requestor_exited, audit);

	audits = slist_append(audits, audit);

	return send_reply_and_unref(conn, reply);
}

static DBusHandlerResult cancel_audit_remote_device(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	DBusError err;
	const char *address;
	bdaddr_t dba;
	struct slist *l;
	struct audit *audit;

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &address,
				DBUS_TYPE_INVALID);
	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if (check_address(address) < 0)
		return error_invalid_arguments(conn, msg);

	str2ba(address, &dba);

	l = slist_find(audits, &dba, audit_addr_cmp);
	if (!l)
		return error_not_in_progress(conn, msg, "Audit not in progress");

	audit = l->data;

	if (strcmp(audit->requestor, dbus_message_get_sender(msg)))
		return error_not_authorized(conn, msg);

	if (audit->io)
		g_io_channel_close(audit->io);
	if (audit->timeout)
		g_timeout_remove(audit->timeout);

	audits = slist_remove(audits, audit);
	name_listener_remove(audit->conn, audit->requestor,
				(name_cb_t) audit_requestor_exited, audit);
	audit_free(audit);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	return send_reply_and_unref(conn, reply);
}

static struct service_data methods[] = {
	{ "AuditRemoteDevice",		audit_remote_device		},
	{ "CancelAuditRemoteDevice",	cancel_audit_remote_device	},
	{ NULL, NULL }
};

DBusHandlerResult handle_test_method(DBusConnection *conn, DBusMessage *msg, void *data)
{
	service_handler_func_t handler;

	if (!hcid_dbus_use_experimental())
		return error_unknown_method(conn, msg);

	handler = find_service_handler(methods, msg);

	if (handler)
		return handler(conn, msg, data);

	return error_unknown_method(conn, msg);
}
