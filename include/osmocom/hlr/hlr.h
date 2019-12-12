/* OsmoHLR generic header */

/* (C) 2017 sysmocom s.f.m.c. GmbH <info@sysmocom.de>
 * All Rights Reserved
 *
 * Author: Max Suraev <msuraev@sysmocom.de>
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

#pragma once

#include <stdbool.h>
#include <osmocom/core/linuxlist.h>
#include <osmocom/gsm/ipa.h>
#include <osmocom/core/tdef.h>

#define HLR_DEFAULT_DB_FILE_PATH "hlr.db"

struct hlr_euse;
struct osmo_gsup_conn;
enum osmo_gsup_message_type;

extern struct osmo_tdef g_hlr_tdefs[];

struct hlr {
	/* GSUP server pointer */
	struct osmo_gsup_server *gs;

	/* DB context */
	char *db_file_path;
	struct db_context *dbc;

	/* Control Interface */
	struct ctrl_handle *ctrl;
	const char *ctrl_bind_addr;

	/* Local bind addr */
	char *gsup_bind_addr;
	struct ipaccess_unit gsup_unit_name;

	struct llist_head euse_list;
	struct hlr_euse *euse_default;
	struct llist_head iuse_list;

	/* NCSS (call independent) session guard timeout value */
	int ncss_guard_timeout;

	struct llist_head ussd_routes;

	struct llist_head ss_sessions;

	bool store_imei;

	bool subscr_create_on_demand;
	/* Bitmask of DB_SUBSCR_FLAG_* */
	uint8_t subscr_create_on_demand_flags;
	unsigned int subscr_create_on_demand_rand_msisdn_len;
};

extern struct hlr *g_hlr;

struct hlr_subscriber;

void osmo_hlr_subscriber_update_notify(struct hlr_subscriber *subscr);
int hlr_subscr_nam(struct hlr *hlr, struct hlr_subscriber *subscr, bool nam_val, bool is_ps);
