/* Copyright 2019 by sysmocom s.f.m.c. GmbH <info@sysmocom.de>
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

#include <osmocom/vty/vty.h>
#include <osmocom/vty/command.h>
#include <osmocom/mslookup/mslookup_client_mdns.h>
#include <osmocom/hlr/hlr_vty.h>
#include <osmocom/hlr/mslookup_server.h>
#include <osmocom/hlr/mslookup_server_mdns.h>
#include <osmocom/gsupclient/gsup_peer_id.h>

struct cmd_node mslookup_node = {
	MSLOOKUP_NODE,
	"%s(config-mslookup)# ",
	1,
};

DEFUN(cfg_mslookup,
      cfg_mslookup_cmd,
      "mslookup",
      "Configure Distributed GSM mslookup")
{
	vty->node = MSLOOKUP_NODE;
	return CMD_SUCCESS;
}

static int mslookup_server_mdns_bind(struct vty *vty, int argc, const char **argv)
{
	const char *ip_str = argc > 0? argv[0] : g_hlr->mslookup.server.mdns.bind_addr.ip;
	const char *port_str = argc > 1? argv[1] : NULL;
	uint16_t port_nr = port_str ? atoi(port_str) : g_hlr->mslookup.server.mdns.bind_addr.port;
	struct osmo_sockaddr_str addr;
	if (osmo_sockaddr_str_from_str(&addr, ip_str, port_nr)
	    || !osmo_sockaddr_str_is_nonzero(&addr)) {
		vty_out(vty, "%% mslookup server: Invalid mDNS bind address: %s %u%s",
			ip_str, port_nr, VTY_NEWLINE);
		return CMD_WARNING;
	}

	g_hlr->mslookup.server.mdns.bind_addr = addr;
	g_hlr->mslookup.server.mdns.enable = true;
	g_hlr->mslookup.server.enable = true;
	mslookup_server_mdns_config_apply();
	return CMD_SUCCESS;
}

#define MDNS_IP46_STR "multicast IPv4 address like " OSMO_MSLOOKUP_MDNS_IP4 \
			" or IPv6 address like " OSMO_MSLOOKUP_MDNS_IP6 "\n"
#define MDNS_PORT_STR "mDNS UDP Port number\n"
#define IP46_STR "IPv4 address like 1.2.3.4 or IPv6 address like a:b:c:d::1\n"
#define PORT_STR "Service-specific port number\n"

struct cmd_node mslookup_server_node = {
	MSLOOKUP_SERVER_NODE,
	"%s(config-mslookup-server)# ",
	1,
};

DEFUN(cfg_mslookup_server,
      cfg_mslookup_server_cmd,
      "server",
      "Enable and configure Distributed GSM mslookup server")
{
	vty->node = MSLOOKUP_SERVER_NODE;
	g_hlr->mslookup.server.enable = true;
	mslookup_server_mdns_config_apply();
	return CMD_SUCCESS;
}

DEFUN(cfg_mslookup_no_server,
      cfg_mslookup_no_server_cmd,
      "no server",
      NO_STR "Disable Distributed GSM mslookup server")
{
	g_hlr->mslookup.server.enable = false;
	mslookup_server_mdns_config_apply();
	return CMD_SUCCESS;
}

DEFUN(cfg_mslookup_server_mdns_bind,
      cfg_mslookup_server_mdns_bind_cmd,
      "mdns [IP] [<1-65535>]",
      "Configure where the mDNS server listens for mslookup requests\n"
      MDNS_IP46_STR MDNS_PORT_STR)
{
	return mslookup_server_mdns_bind(vty, argc, argv);
}

DEFUN(cfg_mslookup_server_no_mdns,
      cfg_mslookup_server_no_mdns_cmd,
      "no mdns",
      NO_STR "Disable server for mDNS mslookup (do not answer remote requests)\n")
{
	g_hlr->mslookup.server.mdns.enable = false;
	mslookup_server_mdns_config_apply();
	return CMD_SUCCESS;
}

struct cmd_node mslookup_server_msc_node = {
	MSLOOKUP_SERVER_MSC_NODE,
	"%s(config-mslookup-server-msc)# ",
	1,
};

DEFUN(cfg_mslookup_server_msc,
      cfg_mslookup_server_msc_cmd,
      "msc .UNIT_NAME",
      "Configure services for individual local MSCs\n"
      "IPA Unit Name of the local MSC to configure\n")
{
	struct osmo_ipa_name msc_name;
	struct mslookup_server_msc_cfg *msc;
	osmo_ipa_name_set_str(&msc_name, argv_concat(argv, argc, 0));

	msc = mslookup_server_msc_get(&msc_name, true);
	if (!msc) {
		vty_out(vty, "%% Error creating MSC %s%s", osmo_ipa_name_to_str(&msc_name), VTY_NEWLINE);
		return CMD_WARNING;
	}
	vty->node = MSLOOKUP_SERVER_MSC_NODE;
	vty->index = msc;
	return CMD_SUCCESS;
}

#define SERVICE_NAME_STR \
	"mslookup service name, e.g. sip.voice or smpp.sms\n"

static struct mslookup_server_msc_cfg *msc_from_node(struct vty *vty)
{
	switch (vty->node) {
	case MSLOOKUP_SERVER_NODE:
		/* On the mslookup.server node, set services on the wildcard msc, without a particular name. */
		return mslookup_server_msc_get(&mslookup_server_msc_wildcard, true);
	case MSLOOKUP_SERVER_MSC_NODE:
		return vty->index;
	default:
		return NULL;
	}
}

DEFUN(cfg_mslookup_server_msc_service,
      cfg_mslookup_server_msc_service_cmd,
      "service NAME at IP <1-65535>",
      "Configure addresses of local services, as sent in replies to remote mslookup requests.\n"
      SERVICE_NAME_STR "at\n" IP46_STR PORT_STR)
{
	/* If this command is run on the 'server' node, it produces an empty unit name and serves as wildcard for all
	 * MSCs. If on a 'server' / 'msc' node, set services only for that MSC Unit Name. */
	struct mslookup_server_msc_cfg *msc = msc_from_node(vty);
	const char *service = argv[0];
	const char *ip_str = argv[1];
	const char *port_str = argv[2];
	struct osmo_sockaddr_str addr;

	if (!msc) {
		vty_out(vty, "%% Error: no MSC object on this node%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	if (osmo_sockaddr_str_from_str(&addr, ip_str, atoi(port_str))
	    || !osmo_sockaddr_str_is_nonzero(&addr)) {
		vty_out(vty, "%% mslookup server: Invalid address for service %s: %s %s%s",
			service, ip_str, port_str, VTY_NEWLINE);
		return CMD_WARNING;
	}

	if (mslookup_server_msc_service_set(msc, service, &addr)) {
		vty_out(vty, "%% mslookup server: Error setting service %s to %s %s%s",
			service, ip_str, port_str, VTY_NEWLINE);
		return CMD_WARNING;
	}
	return CMD_SUCCESS;
}

#define NO_SERVICE_AND_NAME_STR NO_STR "Remove one or more service address entries\n" SERVICE_NAME_STR

DEFUN(cfg_mslookup_server_msc_no_service,
      cfg_mslookup_server_msc_no_service_cmd,
      "no service NAME",
      NO_SERVICE_AND_NAME_STR)
{
	/* If this command is run on the 'server' node, it produces an empty unit name and serves as wildcard for all
	 * MSCs. If on a 'server' / 'msc' node, set services only for that MSC Unit Name. */
	struct mslookup_server_msc_cfg *msc = msc_from_node(vty);
	const char *service = argv[0];

	if (!msc) {
		vty_out(vty, "%% Error: no MSC object on this node%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	if (mslookup_server_msc_service_del(msc, service, NULL) < 1) {
		vty_out(vty, "%% mslookup server: cannot remove service '%s'%s",
			service, VTY_NEWLINE);
		return CMD_WARNING;
	}
	return CMD_SUCCESS;
}

DEFUN(cfg_mslookup_server_msc_no_service_addr,
      cfg_mslookup_server_msc_no_service_addr_cmd,
      "no service NAME at IP <1-65535>",
      NO_SERVICE_AND_NAME_STR "at\n" IP46_STR PORT_STR)
{
	/* If this command is run on the 'server' node, it produces an empty unit name and serves as wildcard for all
	 * MSCs. If on a 'server' / 'msc' node, set services only for that MSC Unit Name. */
	struct mslookup_server_msc_cfg *msc = msc_from_node(vty);
	const char *service = argv[0];
	const char *ip_str = argv[1];
	const char *port_str = argv[2];
	struct osmo_sockaddr_str addr;

	if (!msc) {
		vty_out(vty, "%% Error: no MSC object on this node%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	if (osmo_sockaddr_str_from_str(&addr, ip_str, atoi(port_str))
	    || !osmo_sockaddr_str_is_nonzero(&addr)) {
		vty_out(vty, "%% mslookup server: Invalid address for 'no service' %s: %s %s%s",
			service, ip_str, port_str, VTY_NEWLINE);
		return CMD_WARNING;
	}

	if (mslookup_server_msc_service_del(msc, service, &addr) < 1) {
		vty_out(vty, "%% mslookup server: cannot remove service '%s' to %s %s%s",
			service, ip_str, port_str, VTY_NEWLINE);
		return CMD_WARNING;
	}
	return CMD_SUCCESS;
}

void config_write_msc_services(struct vty *vty, const char *indent, struct mslookup_server_msc_cfg *msc)
{
	struct mslookup_service_host *e;

	llist_for_each_entry(e, &msc->service_hosts, entry) {
		if (osmo_sockaddr_str_is_nonzero(&e->host_v4))
			vty_out(vty, "%sservice %s at %s %u%s", indent, e->service, e->host_v4.ip, e->host_v4.port,
				VTY_NEWLINE);
		if (osmo_sockaddr_str_is_nonzero(&e->host_v6))
			vty_out(vty, "%sservice %s at %s %u%s", indent, e->service, e->host_v6.ip, e->host_v6.port,
				VTY_NEWLINE);
	}
}

int config_write_mslookup(struct vty *vty)
{
	if (!g_hlr->mslookup.server.enable
	    && llist_empty(&g_hlr->mslookup.server.local_site_services))
		return CMD_SUCCESS;

	vty_out(vty, "mslookup%s", VTY_NEWLINE);

	if (g_hlr->mslookup.server.enable || !llist_empty(&g_hlr->mslookup.server.local_site_services)) {
		struct mslookup_server_msc_cfg *msc;

		vty_out(vty, " server%s", VTY_NEWLINE);

		if (g_hlr->mslookup.server.mdns.enable
		    && osmo_sockaddr_str_is_nonzero(&g_hlr->mslookup.server.mdns.bind_addr))
			vty_out(vty, "  mdns bind %s %u%s",
				g_hlr->mslookup.server.mdns.bind_addr.ip,
				g_hlr->mslookup.server.mdns.bind_addr.port,
				VTY_NEWLINE);

		msc = mslookup_server_msc_get(&mslookup_server_msc_wildcard, false);
		if (msc)
			config_write_msc_services(vty, " ", msc);

		llist_for_each_entry(msc, &g_hlr->mslookup.server.local_site_services, entry) {
			if (!osmo_ipa_name_cmp(&mslookup_server_msc_wildcard, &msc->name))
				continue;
			vty_out(vty, " msc %s%s", osmo_ipa_name_to_str(&msc->name), VTY_NEWLINE);
			config_write_msc_services(vty, "  ", msc);
		}

		/* If the server is disabled, still output the above to not lose the service config. */
		if (!g_hlr->mslookup.server.enable)
			vty_out(vty, " no server%s", VTY_NEWLINE);
	}

	return CMD_SUCCESS;
}

DEFUN(do_mslookup_show_services,
      do_mslookup_show_services_cmd,
      "show mslookup services",
      SHOW_STR "Distributed GSM / mslookup related information\n"
      "List configured service addresses as sent to remote mslookup requests\n")
{
	struct mslookup_server_msc_cfg *msc;
	struct mslookup_service_host *local_hlr = mslookup_server_get_local_gsup_addr();

	vty_out(vty, "Local GSUP HLR address returned in mslookup responses for local IMSIs:");
	if (osmo_sockaddr_str_is_nonzero(&local_hlr->host_v4))
		vty_out(vty, " " OSMO_SOCKADDR_STR_FMT,
			OSMO_SOCKADDR_STR_FMT_ARGS(&local_hlr->host_v4));
	if (osmo_sockaddr_str_is_nonzero(&local_hlr->host_v6))
		vty_out(vty, " " OSMO_SOCKADDR_STR_FMT,
			OSMO_SOCKADDR_STR_FMT_ARGS(&local_hlr->host_v6));
	vty_out(vty, "%s", VTY_NEWLINE);

	msc = mslookup_server_msc_get(&mslookup_server_msc_wildcard, false);
	if (msc)
		config_write_msc_services(vty, "", msc);

	llist_for_each_entry(msc, &g_hlr->mslookup.server.local_site_services, entry) {
		if (!osmo_ipa_name_cmp(&mslookup_server_msc_wildcard, &msc->name))
			continue;
		vty_out(vty, "msc %s%s", osmo_ipa_name_to_str(&msc->name), VTY_NEWLINE);
		config_write_msc_services(vty, " ", msc);
	}
	return CMD_SUCCESS;
}

void dgsm_vty_init(void)
{
	install_element(CONFIG_NODE, &cfg_mslookup_cmd);

	install_node(&mslookup_node, config_write_mslookup);
	install_element(MSLOOKUP_NODE, &cfg_mslookup_server_cmd);
	install_element(MSLOOKUP_NODE, &cfg_mslookup_no_server_cmd);

	install_node(&mslookup_server_node, NULL);
	install_element(MSLOOKUP_SERVER_NODE, &cfg_mslookup_server_mdns_bind_cmd);
	install_element(MSLOOKUP_SERVER_NODE, &cfg_mslookup_server_no_mdns_cmd);
	install_element(MSLOOKUP_SERVER_NODE, &cfg_mslookup_server_msc_service_cmd);
	install_element(MSLOOKUP_SERVER_NODE, &cfg_mslookup_server_msc_no_service_cmd);
	install_element(MSLOOKUP_SERVER_NODE, &cfg_mslookup_server_msc_no_service_addr_cmd);
	install_element(MSLOOKUP_SERVER_NODE, &cfg_mslookup_server_msc_cmd);

	install_node(&mslookup_server_msc_node, NULL);
	install_element(MSLOOKUP_SERVER_MSC_NODE, &cfg_mslookup_server_msc_service_cmd);
	install_element(MSLOOKUP_SERVER_MSC_NODE, &cfg_mslookup_server_msc_no_service_cmd);
	install_element(MSLOOKUP_SERVER_MSC_NODE, &cfg_mslookup_server_msc_no_service_addr_cmd);

	install_element_ve(&do_mslookup_show_services_cmd);
}