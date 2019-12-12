/* Copyright 2019 by sysmocom s.f.m.c. GmbH <info@sysmocom.de>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <inttypes.h>

#include <osmocom/core/logging.h>
#include <osmocom/gsm/gsm23003.h>

#include <osmocom/gsupclient/gsup_req.h>

/*! Create a new osmo_gsup_req record, decode GSUP and add to a provided list of requests.
 * This function takes ownership of the msgb, which will, on success, be owned by the returned osmo_gsup_req instance
 * until osmo_gsup_req_free(). If a decoding error occurs, send an error response immediately, and return NULL.
 *
 * When this function returns, the original sender is found in req->source_name. If this is not the immediate peer name,
 * then req->via_proxy is set to the immediate peer, and it is the responsibility of the caller to add req->source_name
 * to the GSUP routes that are serviced by req->via_proxy (usually not relevant for clients with a single GSUP conn).
 *
 * Note: osmo_gsup_req API makes use of OTC_SELECT to allocate volatile buffers for logging. Use of
 * osmo_select_main_ctx() is mandatory when using osmo_gsup_req.
 *
 * \param[in] ctx  Talloc context for allocation of the new request.
 * \param[in] from_peer  The IPA unit name of the immediate GSUP peer from which this msgb was received.
 * \param[in] msg  The GSUP message buffer.
 * \param[in] send_response_cb  User specific method to send a GSUP response message, invoked upon
 *				osmo_gsup_req_respond*() functions.
 * \param[inout] cb_data  Context data to be used freely by the caller.
 * \param[inout] add_to_list  List to which to append this request, or NULL for no list.
 * \return a newly allocated osmo_gsup_req, or NULL on error.
 */
struct osmo_gsup_req *osmo_gsup_req_new(void *ctx, const struct osmo_ipa_name *from_peer, struct msgb *msg,
					osmo_gsup_req_send_response_t send_response_cb, void *cb_data,
					struct llist_head *add_to_list)
{
	static unsigned int next_req_nr = 1;
	struct osmo_gsup_req *req;
	int rc;

	if (!msgb_l2(msg) || !msgb_l2len(msg)) {
		LOGP(DLGSUP, LOGL_ERROR, "Rx GSUP from %s: missing or empty L2 data\n",
		     osmo_ipa_name_to_str(from_peer));
		msgb_free(msg);
		return NULL;
	}

	req = talloc_zero(ctx, struct osmo_gsup_req);
	OSMO_ASSERT(req);
	/* Note: req->gsup is declared const, so that the incoming message cannot be modified by handlers. */
	req->nr = next_req_nr++;
	req->msg = msg;
	req->send_response_cb = send_response_cb;
	req->cb_data = cb_data;
	if (from_peer)
		req->source_name = *from_peer;
	rc = osmo_gsup_decode(msgb_l2(req->msg), msgb_l2len(req->msg), (struct osmo_gsup_message*)&req->gsup);
	if (rc < 0) {
		LOGP(DLGSUP, LOGL_ERROR, "Rx GSUP from %s: cannot decode (rc=%d)\n", osmo_ipa_name_to_str(from_peer), rc);
		osmo_gsup_req_free(req);
		return NULL;
	}

	LOG_GSUP_REQ(req, LOGL_DEBUG, "new request: {%s}\n", osmo_gsup_message_to_str_c(OTC_SELECT, &req->gsup));

	if (req->gsup.source_name_len) {
		if (osmo_ipa_name_set(&req->source_name, req->gsup.source_name, req->gsup.source_name_len)) {
			LOGP(DLGSUP, LOGL_ERROR,
			     "Rx GSUP from %s: failed to decode source_name, message is not routable\n",
			     osmo_ipa_name_to_str(from_peer));
			osmo_gsup_req_respond_msgt(req, OSMO_GSUP_MSGT_ROUTING_ERROR, true);
			return NULL;
		}

		/* The source of the GSUP message is not the immediate GSUP peer; the peer is our proxy for that source.
		 */
		if (osmo_ipa_name_cmp(&req->source_name, from_peer))
			req->via_proxy = *from_peer;
	}

	if (!osmo_imsi_str_valid(req->gsup.imsi)) {
		osmo_gsup_req_respond_err(req, GMM_CAUSE_INV_MAND_INFO, "invalid IMSI: %s",
					  osmo_quote_str(req->gsup.imsi, -1));
		return NULL;
	}

	if (add_to_list)
		llist_add_tail(&req->entry, add_to_list);
	return req;
}

void osmo_gsup_req_free(struct osmo_gsup_req *req)
{
	LOG_GSUP_REQ(req, LOGL_DEBUG, "free\n");
	if (req->msg)
		msgb_free(req->msg);
	if (req->entry.prev)
		llist_del(&req->entry);
	talloc_free(req);
}

int _osmo_gsup_req_respond(struct osmo_gsup_req *req, struct osmo_gsup_message *response,
			   bool error, bool final_response, const char *file, int line)
{
	int rc;

	rc = osmo_gsup_make_response(response, &req->gsup, error, final_response);
	if (rc) {
		LOG_GSUP_REQ_SRC(req, LOGL_ERROR, file, line, "Invalid response (rc=%d): {%s}\n",
				 rc, osmo_gsup_message_to_str_c(OTC_SELECT, response));
		rc = -EINVAL;
		goto exit_cleanup;
	}

	if (!req->send_response_cb) {
		LOG_GSUP_REQ_SRC(req, LOGL_ERROR, file, line, "No send_response_cb set, cannot send: {%s}\n",
				 osmo_gsup_message_to_str_c(OTC_SELECT, response));
		rc = -EINVAL;
		goto exit_cleanup;
	}

	LOG_GSUP_REQ_SRC(req, LOGL_DEBUG, file, line, "Tx response: {%s}\n",
			 osmo_gsup_message_to_str_c(OTC_SELECT, response));
	req->send_response_cb(req, response);

exit_cleanup:
	if (final_response)
		osmo_gsup_req_free(req);
	return rc;
}

int _osmo_gsup_req_respond_msgt(struct osmo_gsup_req *req, enum osmo_gsup_message_type message_type,
				bool final_response, const char *file, int line)
{
	struct osmo_gsup_message response = {
		.message_type = message_type,
	};
	return _osmo_gsup_req_respond(req, &response, OSMO_GSUP_IS_MSGT_ERROR(message_type), final_response,
				      file, line);
}

void _osmo_gsup_req_respond_err(struct osmo_gsup_req *req, enum gsm48_gmm_cause cause,
				const char *file, int line)
{
	struct osmo_gsup_message response = {
		.cause = cause,
	};

	/* No need to answer if we couldn't parse an ERROR message type, only REQUESTs need an error reply. */
	if (!OSMO_GSUP_IS_MSGT_REQUEST(req->gsup.message_type)) {
		osmo_gsup_req_free(req);
		return;
	}

	osmo_gsup_req_respond(req, &response, true, true);
}

/*! This function is implicitly called by the osmo_gsup_req API, if at all possible rather use osmo_gsup_req_respond().
 * This function is non-static mostly to allow unit testing.
 *
 * Set fields, if still unset, that need to be copied from a received message over to its response message, to ensure
 * the response can be routed back to the requesting peer even via GSUP proxies.
 *
 * Note: after calling this function, fields in the reply may reference the same memory as rx and are not deep-copied,
 * as is the usual way we are handling decoded GSUP messages.
 *
 * These fields are set in the reply message, iff they are still unset:
 * - Set reply->message_type to the rx's matching RESULT code (or ERROR code if error == true).
 * - IMSI,
 * - Set reply->destination_name to rx->source_name (for proxy routing),
 * - sm_rp_mr (for SMS),
 * - session_id (for SS/USSD),
 * - if rx->session_state is not NONE, set tx->session_state depending on the final_response argument:
 *   If false, set to OSMO_GSUP_SESSION_STATE_CONTINUE, else OSMO_GSUP_SESSION_STATE_END.
 *
 * If values in reply are already set, they will not be overwritten. The return code is an optional way of finding out
 * whether all values that were already set in 'reply' are indeed matching the 'rx' values that would have been set.
 *
 * \param[in] rx  Received GSUP message that is being replied to.
 * \param[inout] reply  The message that should be the response to rx, either empty or with some values already set up.
 * \return 0 if the resulting message is a valid response for rx, nonzero otherwise. A nonzero rc has no effect on the
 *         values set in the reply message: all unset fields are first updated, and then the rc is determined.
 *         The rc is intended to merely warn if the reply message already contained data that is incompatible with rx,
 *         e.g. a mismatching IMSI.
 */
int osmo_gsup_make_response(struct osmo_gsup_message *reply,
			    const struct osmo_gsup_message *rx, bool error, bool final_response)
{
	int rc = 0;

	if (!reply->message_type) {
		if (error)
			reply->message_type = OSMO_GSUP_TO_MSGT_ERROR(rx->message_type);
		else
			reply->message_type = OSMO_GSUP_TO_MSGT_RESULT(rx->message_type);
	}

	if (*reply->imsi == '\0')
		OSMO_STRLCPY_ARRAY(reply->imsi, rx->imsi);

	if (reply->message_class == OSMO_GSUP_MESSAGE_CLASS_UNSET)
		reply->message_class = rx->message_class;

	if (!reply->destination_name || !reply->destination_name_len) {
		reply->destination_name = rx->source_name;
		reply->destination_name_len = rx->source_name_len;
	}

	/* RP-Message-Reference is mandatory for SM Service */
	if (!reply->sm_rp_mr)
		reply->sm_rp_mr = rx->sm_rp_mr;

	/* For SS/USSD, it's important to keep both session state and ID IEs */
	if (!reply->session_id)
		reply->session_id = rx->session_id;
	if (rx->session_state != OSMO_GSUP_SESSION_STATE_NONE
	    && reply->session_state == OSMO_GSUP_SESSION_STATE_NONE) {
		if (final_response || rx->session_state == OSMO_GSUP_SESSION_STATE_END)
			reply->session_state = OSMO_GSUP_SESSION_STATE_END;
		else
			reply->session_state = OSMO_GSUP_SESSION_STATE_CONTINUE;
	}

	if (strcmp(reply->imsi, rx->imsi))
		rc |= 1 << 0;
	if (reply->message_class != rx->message_class)
		rc |= 1 << 1;
	if (rx->sm_rp_mr && (!reply->sm_rp_mr || *rx->sm_rp_mr != *reply->sm_rp_mr))
		rc |= 1 << 2;
	if (reply->session_id != rx->session_id)
		rc |= 1 << 3;
	return rc;
}

/*! Print the most important value of a GSUP message to a string buffer in human readable form.
 * \param[out] buf  The buffer to write to.
 * \param[out] buflen  sizeof(buf).
 * \param[in] msg  GSUP message to print.
 */
size_t osmo_gsup_message_to_str_buf(char *buf, size_t buflen, const struct osmo_gsup_message *msg)
{
	struct osmo_strbuf sb = { .buf = buf, .len = buflen };
	if (!msg) {
		OSMO_STRBUF_PRINTF(sb, "NULL");
		return sb.chars_needed;
	}

	if (msg->message_class)
		OSMO_STRBUF_PRINTF(sb, "%s ", osmo_gsup_message_class_name(msg->message_class));

	OSMO_STRBUF_PRINTF(sb, "%s:", osmo_gsup_message_type_name(msg->message_type));

	OSMO_STRBUF_PRINTF(sb, " imsi=");
	OSMO_STRBUF_APPEND(sb, osmo_quote_cstr_buf, msg->imsi, strnlen(msg->imsi, sizeof(msg->imsi)));

	if (msg->cause)
		OSMO_STRBUF_PRINTF(sb, " cause=%s", get_value_string(gsm48_gmm_cause_names, msg->cause));

	switch (msg->cn_domain) {
	case OSMO_GSUP_CN_DOMAIN_CS:
		OSMO_STRBUF_PRINTF(sb, " cn_domain=CS");
		break;
	case OSMO_GSUP_CN_DOMAIN_PS:
		OSMO_STRBUF_PRINTF(sb, " cn_domain=PS");
		break;
	default:
		if (msg->cn_domain)
			OSMO_STRBUF_PRINTF(sb, " cn_domain=?(%d)", msg->cn_domain);
		break;
	}

	if (msg->source_name_len) {
		OSMO_STRBUF_PRINTF(sb, " source_name=");
		OSMO_STRBUF_APPEND(sb, osmo_quote_cstr_buf, (char*)msg->source_name, msg->source_name_len);
	}

	if (msg->destination_name_len) {
		OSMO_STRBUF_PRINTF(sb, " destination_name=");
		OSMO_STRBUF_APPEND(sb, osmo_quote_cstr_buf, (char*)msg->destination_name, msg->destination_name_len);
	}

	if (msg->session_id)
		OSMO_STRBUF_PRINTF(sb, " session_id=%" PRIu32, msg->session_id);
	if (msg->session_state)
		OSMO_STRBUF_PRINTF(sb, " session_state=%s", osmo_gsup_session_state_name(msg->session_state));

	if (msg->sm_rp_mr)
		OSMO_STRBUF_PRINTF(sb, " sm_rp_mr=%" PRIu8, *msg->sm_rp_mr);

	return sb.chars_needed;
}

/*! Same as  osmo_gsup_message_to_str_buf() but returns a talloc allocated string. */
char *osmo_gsup_message_to_str_c(void *ctx, const struct osmo_gsup_message *msg)
{
	OSMO_NAME_C_IMPL(ctx, 64, "ERROR", osmo_gsup_message_to_str_buf, msg)
}
