/* (C) 2018 by sysmocom - s.f.m.c. GmbH <info@sysmocom.de>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/* This is kept separate to be able to override the actual sending functions from unit tests. */

#include <errno.h>

#include "gsup_server.h"
#include "gsup_router.h"

#include <osmocom/core/logging.h>

/*! Send a msgb to a given address using routing.
 * \param[in] gs gsup server
 * \param[in] addr IPA name of the client (SGSN, MSC/VLR). Although this is passed like a blob, together with the
 *                 length, it must be nul-terminated! This is for legacy reasons, see the discussion here:
 *                 https://gerrit.osmocom.org/#/c/osmo-hlr/+/13048/
 * \param[in] addrlen length of addr, *including the nul-byte* (strlen(addr) + 1).
 * \param[in] msg message buffer
 */
int osmo_gsup_addr_send(struct osmo_gsup_server *gs,
			const uint8_t *addr, size_t addrlen,
			struct msgb *msg)
{
	struct osmo_gsup_conn *conn;

	conn = gsup_route_find(gs, addr, addrlen);
	if (!conn) {
		LOGP(DLGSUP, LOGL_ERROR,
		     "Cannot find route for addr %s\n", osmo_quote_str((const char*)addr, addrlen));
		msgb_free(msg);
		return -ENODEV;
	}

	return osmo_gsup_conn_send(conn, msg);
}

/*! Send a msgb to a given address using routing.
 * \param[in] gs  gsup server
 * \param[in] gt  IPA unit name of the client (SGSN, MSC/VLR, proxy).
 * \param[in] msg  message buffer
 */
int osmo_gsup_gt_send(struct osmo_gsup_server *gs, const struct global_title *gt, struct msgb *msg)
{
	if (gt->val[gt->len - 1]) {
		/* Is not nul terminated. But for legacy reasons we (still) require that. */
		if (gt->len >= sizeof(gt->val)) {
			LOGP(DLGSUP, LOGL_ERROR, "Global title (IPA unit name) is too long: %s\n",
			     global_title_name(gt));
			return -EINVAL;
		}
		struct global_title gt2 = *gt;
		gt2.val[gt->len] = '\0';
		gt2.len++;
		return osmo_gsup_addr_send(gs, gt2.val, gt2.len, msg);
	}
	return osmo_gsup_addr_send(gs, gt->val, gt->len, msg);
}

int osmo_gsup_gt_enc_send(struct osmo_gsup_server *gs, const struct global_title *gt,
			  const struct osmo_gsup_message *gsup)
{
	struct msgb *msg = osmo_gsup_msgb_alloc("GSUP Tx");
	int rc;
	rc = osmo_gsup_encode(msg, gsup);
	if (rc) {
		LOGP(DLGSUP, LOGL_ERROR, "IMSI-%s: Cannot encode GSUP: %s\n",
		     gsup->imsi, osmo_gsup_message_type_name(gsup->message_type));
		msgb_free(msg);
		return -EINVAL;
	}

	LOGP(DLGSUP, LOGL_DEBUG, "IMSI-%s: Tx: %s\n", gsup->imsi, osmo_gsup_message_type_name(gsup->message_type));
	return osmo_gsup_gt_send(gs, gt, msg);
}
