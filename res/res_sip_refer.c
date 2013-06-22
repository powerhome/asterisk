/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2013, Digium, Inc.
 *
 * Joshua Colp <jcolp@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*** MODULEINFO
	<depend>pjproject</depend>
	<depend>res_sip</depend>
	<depend>res_sip_session</depend>
	<depend>res_sip_pubsub</depend>
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

#include <pjsip.h>
#include <pjsip_ua.h>

#include "asterisk/res_sip.h"
#include "asterisk/res_sip_session.h"
#include "asterisk/module.h"
#include "asterisk/pbx.h"
#include "asterisk/taskprocessor.h"
#include "asterisk/bridging.h"
#include "asterisk/framehook.h"

/*! \brief REFER Progress structure */
struct refer_progress {
	/*! \brief Subscription to provide updates on */
	pjsip_evsub *sub;
	/*! \brief Dialog for subscription */
	pjsip_dialog *dlg;
	/*! \brief Received packet, used to construct final response in case no subscription exists */
	pjsip_rx_data *rdata;
	/*! \brief Frame hook for monitoring REFER progress */
	int framehook;
	/*! \brief Last received subclass in frame hook */
	int subclass;
	/*! \brief Serializer for notifications */
	struct ast_taskprocessor *serializer;
};

/*! \brief REFER Progress notification structure */
struct refer_progress_notification {
	/*! \brief Refer progress structure to send notification on */
	struct refer_progress *progress;
	/*! \brief SIP response code to send */
	int response;
	/*! \brief Subscription state */
	pjsip_evsub_state state;
};

/*! \brief REFER Progress module, used to attach REFER progress structure to subscriptions */
static pjsip_module refer_progress_module = {
	.name = { "REFER Progress", 14 },
	.id = -1,
};

/*! \brief Destructor for REFER Progress notification structure */
static void refer_progress_notification_destroy(void *obj)
{
	struct refer_progress_notification *notification = obj;

	ao2_cleanup(notification->progress);
}

/*! \brief Allocator for REFER Progress notification structure */
static struct refer_progress_notification *refer_progress_notification_alloc(struct refer_progress *progress, int response,
	pjsip_evsub_state state)
{
	struct refer_progress_notification *notification = ao2_alloc(sizeof(*notification), refer_progress_notification_destroy);

	if (!notification) {
		return NULL;
	}

	ao2_ref(progress, +1);
	notification->progress = progress;
	notification->response = response;
	notification->state = state;

	return notification;
}

/*! \brief Serialized callback for subscription notification */
static int refer_progress_notify(void *data)
{
	RAII_VAR(struct refer_progress_notification *, notification, data, ao2_cleanup);
	pjsip_evsub *sub;
	pjsip_tx_data *tdata;

	/* If the subscription has already been terminated we can't send a notification */
	if (!(sub = notification->progress->sub)) {
		ast_debug(3, "Not sending NOTIFY of response '%d' and state '%d' on progress monitor '%p' as subscription has been terminated\n",
			notification->response, notification->state, notification->progress);
		return 0;
	}

	/* If the subscription is being terminated we want to actually remove the progress structure here to
	 * stop a deadlock from occurring - basically terminated changes the state which queues a synchronous task
	 * but we are already running a task... thus it would deadlock */
	if (notification->state == PJSIP_EVSUB_STATE_TERMINATED) {
		ast_debug(3, "Subscription '%p' is being terminated as a result of a NOTIFY, removing REFER progress structure early on progress monitor '%p'\n",
			notification->progress->sub, notification->progress);
		pjsip_dlg_inc_lock(notification->progress->dlg);
		pjsip_evsub_set_mod_data(notification->progress->sub, refer_progress_module.id, NULL);
		pjsip_dlg_dec_lock(notification->progress->dlg);

		/* This is for dropping the reference on the subscription */
		ao2_cleanup(notification->progress);

		notification->progress->sub = NULL;
	}

	ast_debug(3, "Sending NOTIFY with response '%d' and state '%d' on subscription '%p' and progress monitor '%p'\n",
		notification->response, notification->state, sub, notification->progress);

	/* Actually send the notification */
	if (pjsip_xfer_notify(sub, notification->state, notification->response, NULL, &tdata) == PJ_SUCCESS) {
		pjsip_xfer_send_request(sub, tdata);
	}

	return 0;
}

/*! \brief Progress monitoring frame hook - examines frames to determine state of transfer */
static struct ast_frame *refer_progress_framehook(struct ast_channel *chan, struct ast_frame *f, enum ast_framehook_event event, void *data)
{
	struct refer_progress *progress = data;
	struct refer_progress_notification *notification = NULL;

	/* We only care about frames *to* the channel */
	if (!f || (event != AST_FRAMEHOOK_EVENT_WRITE)) {
		return f;
	}

	/* Determine the state of the REFER based on the control frames (or voice frames) passing */
	if (f->frametype == AST_FRAME_VOICE && !progress->subclass) {
		/* Media is passing without progress, this means the call has been answered */
		notification = refer_progress_notification_alloc(progress, 200, PJSIP_EVSUB_STATE_TERMINATED);
	} else if (f->frametype == AST_FRAME_CONTROL) {
		progress->subclass = f->subclass.integer;

		/* Based on the control frame being written we can send a NOTIFY advising of the progress */
		if ((f->subclass.integer == AST_CONTROL_RING) || (f->subclass.integer == AST_CONTROL_RINGING)) {
			notification = refer_progress_notification_alloc(progress, 180, PJSIP_EVSUB_STATE_ACTIVE);
		} else if (f->subclass.integer == AST_CONTROL_BUSY) {
			notification = refer_progress_notification_alloc(progress, 486, PJSIP_EVSUB_STATE_TERMINATED);
		} else if (f->subclass.integer == AST_CONTROL_CONGESTION) {
			notification = refer_progress_notification_alloc(progress, 503, PJSIP_EVSUB_STATE_TERMINATED);
		} else if (f->subclass.integer == AST_CONTROL_PROGRESS) {
			notification = refer_progress_notification_alloc(progress, 183, PJSIP_EVSUB_STATE_ACTIVE);
		} else if (f->subclass.integer == AST_CONTROL_PROCEEDING) {
			notification = refer_progress_notification_alloc(progress, 100, PJSIP_EVSUB_STATE_ACTIVE);
		} else if (f->subclass.integer == AST_CONTROL_ANSWER) {
			notification = refer_progress_notification_alloc(progress, 200, PJSIP_EVSUB_STATE_TERMINATED);
		}
	}

	/* If a notification is due to be sent push it to the thread pool */
	if (notification) {
		if (ast_sip_push_task(progress->serializer, refer_progress_notify, notification)) {
			ao2_cleanup(notification);
		}

		/* If the subscription is being terminated we don't need the frame hook any longer */
		if (notification->state == PJSIP_EVSUB_STATE_TERMINATED) {
			ast_debug(3, "Detaching REFER progress monitoring hook from '%s' as subscription is being terminated\n",
				ast_channel_name(chan));
			ast_framehook_detach(chan, progress->framehook);
		}

	}

	return f;
}

/*! \brief Destroy callback for monitoring framehook */
static void refer_progress_framehook_destroy(void *data)
{
	struct refer_progress *progress = data;
	struct refer_progress_notification *notification = refer_progress_notification_alloc(progress, 503, PJSIP_EVSUB_STATE_TERMINATED);

	if (notification && ast_sip_push_task(progress->serializer, refer_progress_notify, notification)) {
		ao2_cleanup(notification);
	}

	ao2_cleanup(progress);
}

/*! \brief Serialized callback for subscription termination */
static int refer_progress_terminate(void *data)
{
	struct refer_progress *progress = data;

	/* The subscription is no longer valid */
	progress->sub = NULL;

	return 0;
}

/*! \brief Callback for REFER subscription state changes */
static void refer_progress_on_evsub_state(pjsip_evsub *sub, pjsip_event *event)
{
	struct refer_progress *progress = pjsip_evsub_get_mod_data(sub, refer_progress_module.id);

	/* If being destroyed queue it up to the serializer */
	if (progress && (pjsip_evsub_get_state(sub) == PJSIP_EVSUB_STATE_TERMINATED)) {
		/* To prevent a deadlock race condition we unlock the dialog so other serialized tasks can execute */
		ast_debug(3, "Subscription '%p' has been remotely terminated, waiting for other tasks to complete on progress monitor '%p'\n",
			sub, progress);

		/* It's possible that a task is waiting to remove us already, so bump the refcount of progress so it doesn't get destroyed */
		ao2_ref(progress, +1);
		pjsip_dlg_dec_lock(progress->dlg);
		ast_sip_push_task_synchronous(progress->serializer, refer_progress_terminate, progress);
		pjsip_dlg_inc_lock(progress->dlg);
		ao2_ref(progress, -1);

		ast_debug(3, "Subscription '%p' removed from progress monitor '%p'\n", sub, progress);

		/* Since it was unlocked it is possible for this to have been removed already, so check again */
		if (pjsip_evsub_get_mod_data(sub, refer_progress_module.id)) {
			pjsip_evsub_set_mod_data(sub, refer_progress_module.id, NULL);
			ao2_cleanup(progress);
		}
	}
}

/*! \brief Callback structure for subscription */
static pjsip_evsub_user refer_progress_evsub_cb = {
	.on_evsub_state = refer_progress_on_evsub_state,
};

/*! \brief Destructor for REFER progress sutrcture */
static void refer_progress_destroy(void *obj)
{
	struct refer_progress *progress = obj;

	ast_taskprocessor_unreference(progress->serializer);
}

/*! \brief Internal helper function which sets up a refer progress structure if needed */
static int refer_progress_alloc(struct ast_sip_session *session, pjsip_rx_data *rdata, struct refer_progress **progress)
{
	const pj_str_t str_refer_sub = { "Refer-Sub", 9 };
	pjsip_generic_string_hdr *refer_sub = NULL;
	const pj_str_t str_true = { "true", 4 };
	pjsip_tx_data *tdata;
	pjsip_hdr hdr_list;

	*progress = NULL;

	/* Grab the optional Refer-Sub header, it can be used to suppress the implicit subscription */
	refer_sub = pjsip_msg_find_hdr_by_name(rdata->msg_info.msg, &str_refer_sub, NULL);
	if ((refer_sub && pj_strnicmp(&refer_sub->hvalue, &str_true, 4))) {
		return 0;
	}

	if (!(*progress = ao2_alloc(sizeof(struct refer_progress), refer_progress_destroy))) {
		return -1;
	}

	ast_debug(3, "Created progress monitor '%p' for transfer occurring from channel '%s' and endpoint '%s'\n",
		progress, ast_channel_name(session->channel), ast_sorcery_object_get_id(session->endpoint));

	(*progress)->framehook = -1;

	/* To prevent a potential deadlock we need the dialog so we can lock/unlock */
	(*progress)->dlg = session->inv_session->dlg;

	if (!((*progress)->serializer = ast_sip_create_serializer())) {
		goto error;
	}

	/* Create the implicit subscription for monitoring of this transfer */
	if (pjsip_xfer_create_uas(session->inv_session->dlg, &refer_progress_evsub_cb, rdata, &(*progress)->sub) != PJ_SUCCESS) {
		goto error;
	}

	/* Associate the REFER progress structure with the subscription */
	ao2_ref(*progress, +1);
	pjsip_evsub_set_mod_data((*progress)->sub, refer_progress_module.id, *progress);

	pj_list_init(&hdr_list);
	if (refer_sub) {
		pjsip_hdr *hdr = (pjsip_hdr*)pjsip_generic_string_hdr_create(session->inv_session->dlg->pool, &str_refer_sub, &str_true);

		pj_list_push_back(&hdr_list, hdr);
	}

	/* Accept the REFER request */
	ast_debug(3, "Accepting REFER request for progress monitor '%p'\n", *progress);
	pjsip_xfer_accept((*progress)->sub, rdata, 202, &hdr_list);

	/* Send initial NOTIFY Request */
	ast_debug(3, "Sending initial 100 Trying NOTIFY for progress monitor '%p'\n", *progress);
	if (pjsip_xfer_notify((*progress)->sub, PJSIP_EVSUB_STATE_ACTIVE, 100, NULL, &tdata) == PJ_SUCCESS) {
		pjsip_xfer_send_request((*progress)->sub, tdata);
	}

	return 0;

error:
	ao2_cleanup(*progress);
	*progress = NULL;
	return -1;
}

/*! \brief Structure for attended transfer task */
struct refer_attended {
	/*! \brief Transferer session */
	struct ast_sip_session *transferer;
	/*! \brief Transferer channel */
	struct ast_channel *transferer_chan;
	/*! \brief Second transferer session */
	struct ast_sip_session *transferer_second	;
	/*! \brief Optional refer progress structure */
	struct refer_progress *progress;
};

/*! \brief Destructor for attended transfer task */
static void refer_attended_destroy(void *obj)
{
	struct refer_attended *attended = obj;

	ao2_cleanup(attended->transferer);
	ast_channel_unref(attended->transferer_chan);
	ao2_cleanup(attended->transferer_second);
}

/*! \brief Allocator for attended transfer task */
static struct refer_attended *refer_attended_alloc(struct ast_sip_session *transferer, struct ast_sip_session *transferer_second,
	struct refer_progress *progress)
{
	struct refer_attended *attended = ao2_alloc(sizeof(*attended), refer_attended_destroy);

	if (!attended) {
		return NULL;
	}

	ao2_ref(transferer, +1);
	attended->transferer = transferer;
	ast_channel_ref(transferer->channel);
	attended->transferer_chan = transferer->channel;
	ao2_ref(transferer_second, +1);
	attended->transferer_second = transferer_second;

	if (progress) {
		ao2_ref(progress, +1);
		attended->progress = progress;
	}

	return attended;
}

/*! \brief Task for attended transfer */
static int refer_attended(void *data)
{
	RAII_VAR(struct refer_attended *, attended, data, ao2_cleanup);
	int response = 0;

	ast_debug(3, "Performing a REFER attended transfer - Transferer #1: %s Transferer #2: %s\n",
		ast_channel_name(attended->transferer_chan), ast_channel_name(attended->transferer_second->channel));

	switch (ast_bridge_transfer_attended(attended->transferer_chan, attended->transferer_second->channel)) {
	case AST_BRIDGE_TRANSFER_INVALID:
		response = 400;
		break;
	case AST_BRIDGE_TRANSFER_NOT_PERMITTED:
		response = 403;
		break;
	case AST_BRIDGE_TRANSFER_FAIL:
		response = 500;
		break;
	case AST_BRIDGE_TRANSFER_SUCCESS:
		response = 200;
		ast_sip_session_defer_termination(attended->transferer);
		break;
	}

	ast_debug(3, "Final response for REFER attended transfer - Transferer #1: %s Transferer #2: %s is '%d'\n",
		ast_channel_name(attended->transferer_chan), ast_channel_name(attended->transferer_second->channel), response);

	if (attended->progress && response) {
		struct refer_progress_notification *notification = refer_progress_notification_alloc(attended->progress, response, PJSIP_EVSUB_STATE_TERMINATED);

		if (notification) {
			refer_progress_notify(notification);
		}
	}

	return 0;
}

/*! \brief Structure for blind transfer callback details */
struct refer_blind {
	/*! \brief Context being used for transfer */
	const char *context;
	/*! \brief Optional progress structure */
	struct refer_progress *progress;
	/*! \brief REFER message */
	pjsip_rx_data *rdata;
	/*! \brief Optional Replaces header */
	pjsip_replaces_hdr *replaces;
	/*! \brief Optional Refer-To header */
	pjsip_sip_uri *refer_to;
};

/*! \brief Blind transfer callback function */
static void refer_blind_callback(struct ast_channel *chan, void *user_data, enum ast_transfer_type transfer_type)
{
	struct refer_blind *refer = user_data;
	const pj_str_t str_referred_by = { "Referred-By", 11 };
	pjsip_generic_string_hdr *referred_by = pjsip_msg_find_hdr_by_name(refer->rdata->msg_info.msg, &str_referred_by, NULL);

	pbx_builtin_setvar_helper(chan, "SIPTRANSFER", "yes");

	/* If progress monitoring is being done attach a frame hook so we can monitor it */
	if (refer->progress) {
		struct ast_framehook_interface hook = {
			.version = AST_FRAMEHOOK_INTERFACE_VERSION,
			.event_cb = refer_progress_framehook,
			.destroy_cb = refer_progress_framehook_destroy,
			.data = refer->progress,
		};

		/* We need to bump the reference count up on the progress structure since it is in the frame hook now */
		ao2_ref(refer->progress, +1);

		/* If we can't attach a frame hook for whatever reason send a notification of success immediately */
		if ((refer->progress->framehook = ast_framehook_attach(chan, &hook)) < 0) {
			struct refer_progress_notification *notification = refer_progress_notification_alloc(refer->progress, 200,
				PJSIP_EVSUB_STATE_TERMINATED);

			ast_log(LOG_WARNING, "Could not attach REFER transfer progress monitoring hook to channel '%s' - assuming success\n",
				ast_channel_name(chan));

			if (notification) {
				refer_progress_notify(notification);
			}

			ao2_cleanup(refer->progress);
		}
	}

	if (!ast_strlen_zero(refer->context)) {
		pbx_builtin_setvar_helper(chan, "SIPREFERRINGCONTEXT", refer->context);
	}

	if (referred_by) {
		char *uri = referred_by->hvalue.ptr;

		uri[referred_by->hvalue.slen] = '\0';
		pbx_builtin_setvar_helper(chan, "SIPREFERREDBYHDR", uri);
	}

	if (refer->replaces) {
		char replaces[512];

		pjsip_hdr_print_on(refer->replaces, replaces, sizeof(replaces));
		pbx_builtin_setvar_helper(chan, "SIPREPLACESHDR", replaces);
	}

	if (refer->refer_to) {
		char refer_to[PJSIP_MAX_URL_SIZE];

		pjsip_uri_print(PJSIP_URI_IN_REQ_URI, refer->refer_to, refer_to, sizeof(refer_to));
		pbx_builtin_setvar_helper(chan, "SIPREFERTOHDR", refer_to);
	}
}

static int refer_incoming_attended_request(struct ast_sip_session *session, pjsip_rx_data *rdata, pjsip_sip_uri *target_uri,
	pjsip_param *replaces_param, struct refer_progress *progress)
{
	const pj_str_t str_replaces = { "Replaces", 8 };
	pj_str_t replaces_content;
	pjsip_replaces_hdr *replaces;
	int parsed_len;
	pjsip_dialog *dlg;

	pj_strdup_with_null(rdata->tp_info.pool, &replaces_content, &replaces_param->value);

	/* Parsing the parameter as a Replaces header easily grabs the needed information */
	if (!(replaces = pjsip_parse_hdr(rdata->tp_info.pool, &str_replaces, replaces_content.ptr,
		pj_strlen(&replaces_content), &parsed_len))) {
		ast_log(LOG_ERROR, "Received REFER request on channel '%s' from endpoint '%s' with invalid Replaces header, rejecting\n",
			ast_channel_name(session->channel), ast_sorcery_object_get_id(session->endpoint));
		return 400;
	}

	/* See if the dialog is local, or remote */
	if ((dlg = pjsip_ua_find_dialog(&replaces->call_id, &replaces->to_tag, &replaces->from_tag, PJ_TRUE))) {
		RAII_VAR(struct ast_sip_session *, other_session, ast_sip_dialog_get_session(dlg), ao2_cleanup);
		struct refer_attended *attended;

		pjsip_dlg_dec_lock(dlg);

		if (!other_session) {
			ast_debug(3, "Received REFER request on channel '%s' from endpoint '%s' for local dialog but no session exists on it\n",
				ast_channel_name(session->channel), ast_sorcery_object_get_id(session->endpoint));
			return 603;
		}

		/* We defer actually doing the attended transfer to the other session so no deadlock can occur */
		if (!(attended = refer_attended_alloc(session, other_session, progress))) {
			ast_log(LOG_ERROR, "Received REFER request on channel '%s' from endpoint '%s' for local dialog but could not allocate structure to complete, rejecting\n",
				ast_channel_name(session->channel), ast_sorcery_object_get_id(session->endpoint));
			return 500;
		}

		/* Push it to the other session, which will have both channels with minimal locking */
		if (ast_sip_push_task(other_session->serializer, refer_attended, attended)) {
			ao2_cleanup(attended);
			return 500;
		}

		ast_debug(3, "Attended transfer from '%s' pushed to second channel serializer\n",
			ast_channel_name(session->channel));

		return 200;
	} else {
		const char *context = (session->channel ? pbx_builtin_getvar_helper(session->channel, "TRANSFER_CONTEXT") : "");
		struct refer_blind refer = { 0, };

		if (ast_strlen_zero(context)) {
			context = session->endpoint->context;
		}

		if (!ast_exists_extension(NULL, context, "external_replaces", 1, NULL)) {
			ast_log(LOG_ERROR, "Received REFER for remote session on channel '%s' from endpoint '%s' but 'external_replaces' context does not exist for handling\n",
				ast_channel_name(session->channel), ast_sorcery_object_get_id(session->endpoint));
			return 404;
		}

		refer.context = context;
		refer.progress = progress;
		refer.rdata = rdata;
		refer.replaces = replaces;
		refer.refer_to = target_uri;

		switch (ast_bridge_transfer_blind(session->channel, "external_replaces", context, refer_blind_callback, &refer)) {
		case AST_BRIDGE_TRANSFER_INVALID:
			return 400;
		case AST_BRIDGE_TRANSFER_NOT_PERMITTED:
			return 403;
		case AST_BRIDGE_TRANSFER_FAIL:
			return 500;
		case AST_BRIDGE_TRANSFER_SUCCESS:
			ast_sip_session_defer_termination(session);
			return 200;
		}

		return 503;
	}

	return 0;
}

static int refer_incoming_blind_request(struct ast_sip_session *session, pjsip_rx_data *rdata, pjsip_sip_uri *target,
	struct refer_progress *progress)
{
	const char *context = (session->channel ? pbx_builtin_getvar_helper(session->channel, "TRANSFER_CONTEXT") : "");
	char exten[AST_MAX_EXTENSION];
	struct refer_blind refer = { 0, };

	/* If no explicit transfer context has been provided use their configured context */
	if (ast_strlen_zero(context)) {
		context = session->endpoint->context;
	}

	/* Using the user portion of the target URI see if it exists as a valid extension in their context */
	ast_copy_pj_str(exten, &target->user, sizeof(exten));
	if (!ast_exists_extension(NULL, context, exten, 1, NULL)) {
		ast_log(LOG_ERROR, "Channel '%s' from endpoint '%s' attempted blind transfer to '%s@%s' but target does not exist\n",
			ast_channel_name(session->channel), ast_sorcery_object_get_id(session->endpoint), exten, context);
		return 404;
	}

	refer.context = context;
	refer.progress = progress;
	refer.rdata = rdata;

	switch (ast_bridge_transfer_blind(session->channel, exten, context, refer_blind_callback, &refer)) {
	case AST_BRIDGE_TRANSFER_INVALID:
		return 400;
	case AST_BRIDGE_TRANSFER_NOT_PERMITTED:
		return 403;
	case AST_BRIDGE_TRANSFER_FAIL:
		return 500;
	case AST_BRIDGE_TRANSFER_SUCCESS:
		ast_sip_session_defer_termination(session);
		return 200;
	}

	return 503;
}

/*! \brief Structure used to retrieve channel from another session */
struct invite_replaces {
	/*! \brief Session we want the channel from */
	struct ast_sip_session *session;
	/*! \brief Channel from the session (with reference) */
	struct ast_channel *channel;
	/*! \brief Bridge the channel is in */
	struct ast_bridge *bridge;
};

/*! \brief Task for invite replaces */
static int invite_replaces(void *data)
{
	struct invite_replaces *invite = data;

	if (!invite->session->channel) {
		return -1;
	}

	ast_channel_ref(invite->session->channel);
	invite->channel = invite->session->channel;

	ast_channel_lock(invite->channel);
	invite->bridge = ast_channel_get_bridge(invite->channel);
	ast_channel_unlock(invite->channel);

	return 0;
}

static int refer_incoming_invite_request(struct ast_sip_session *session, struct pjsip_rx_data *rdata)
{
	pjsip_dialog *other_dlg = NULL;
	pjsip_tx_data *packet;
	int response = 0;
	RAII_VAR(struct ast_sip_session *, other_session, NULL, ao2_cleanup);
	struct invite_replaces invite;
	RAII_VAR(struct ast_bridge *, bridge, NULL, ao2_cleanup);

	/* If a Replaces header is present make sure it is valid */
	if (pjsip_replaces_verify_request(rdata, &other_dlg, PJ_TRUE, &packet) != PJ_SUCCESS) {
		response = packet->msg->line.status.code;
		pjsip_tx_data_dec_ref(packet);
		goto end;
	}

	/* If no other dialog exists then this INVITE request does not have a Replaces header */
	if (!other_dlg) {
		return 0;
	}

	other_session = ast_sip_dialog_get_session(other_dlg);
	pjsip_dlg_dec_lock(other_dlg);

	if (!other_session) {
		response = 481;
		ast_debug(3, "INVITE with Replaces received on channel '%s' from endpoint '%s', but requested session does not exist\n",
			ast_channel_name(session->channel), ast_sorcery_object_get_id(session->endpoint));
		goto end;
	}

	invite.session = other_session;

	if (ast_sip_push_task_synchronous(other_session->serializer, invite_replaces, &invite)) {
		response = 481;
		goto end;
	}

	ast_setstate(session->channel, AST_STATE_RING);
	ast_raw_answer(session->channel);

	if (!invite.bridge) {
		struct ast_channel *chan = session->channel;

		/* This will use a synchronous task but we aren't operating in the serializer at this point in time, so it
		 * won't deadlock */
		if (!ast_channel_move(invite.channel, session->channel)) {
			ast_hangup(chan);
		} else {
			response = 500;
		}
	} else {
		if (ast_bridge_impart(invite.bridge, session->channel, invite.channel, NULL, 1)) {
			response = 500;
		}
	}

	if (!response) {
		ast_debug(3, "INVITE with Replaces successfully completed on channels '%s' and '%s'\n",
			ast_channel_name(session->channel), ast_channel_name(invite.channel));
	}

	ast_channel_unref(invite.channel);
	ao2_cleanup(invite.bridge);

end:
	if (response) {
		ast_debug(3, "INVITE with Replaces failed on channel '%s', sending response of '%d'\n",
			ast_channel_name(session->channel), response);
		session->defer_terminate = 1;
		ast_hangup(session->channel);
		session->channel = NULL;

		if (pjsip_inv_end_session(session->inv_session, response, NULL, &packet) == PJ_SUCCESS) {
			ast_sip_session_send_response(session, packet);
		}
	}

	return 1;
}

static int refer_incoming_refer_request(struct ast_sip_session *session, struct pjsip_rx_data *rdata)
{
	const pj_str_t str_refer_to = { "Refer-To", 8 };
	pjsip_generic_string_hdr *refer_to;
	char *uri;
	const pj_str_t str_to = { "To", 2 };
	pjsip_fromto_hdr *target;
	pjsip_sip_uri *target_uri;
	RAII_VAR(struct refer_progress *, progress, NULL, ao2_cleanup);
	const pj_str_t str_replaces = { "Replaces", 8 };
	pjsip_param *replaces;
	int response;

	/* A Refer-To header is required */
	if (!(refer_to = pjsip_msg_find_hdr_by_name(rdata->msg_info.msg, &str_refer_to, NULL))) {
		pjsip_dlg_respond(session->inv_session->dlg, rdata, 400, NULL, NULL, NULL);
		ast_debug(3, "Received a REFER without Refer-To on channel '%s' from endpoint '%s'\n",
			ast_channel_name(session->channel), ast_sorcery_object_get_id(session->endpoint));
		return 0;
	}
	uri = refer_to->hvalue.ptr;
	uri[refer_to->hvalue.slen] = '\0';

	/* Parse the provided URI string as a To header so we can get the target */
	if (!(target = pjsip_parse_hdr(rdata->tp_info.pool, &str_to, refer_to->hvalue.ptr, refer_to->hvalue.slen, NULL)) ||
		(!PJSIP_URI_SCHEME_IS_SIP(target->uri) && !PJSIP_URI_SCHEME_IS_SIPS(target->uri))) {
		pjsip_dlg_respond(session->inv_session->dlg, rdata, 400, NULL, NULL, NULL);
		ast_debug(3, "Received a REFER without a parseable Refer-To ('%s') on channel '%s' from endpoint '%s'\n",
			uri, ast_channel_name(session->channel), ast_sorcery_object_get_id(session->endpoint));
		return 0;
	}
	target_uri = pjsip_uri_get_uri(target->uri);

	/* Set up REFER progress subscription if requested/possible */
	if (refer_progress_alloc(session, rdata, &progress)) {
		pjsip_dlg_respond(session->inv_session->dlg, rdata, 500, NULL, NULL, NULL);
		ast_debug(3, "Could not set up subscription for REFER on channel '%s' from endpoint '%s'\n",
			ast_channel_name(session->channel), ast_sorcery_object_get_id(session->endpoint));
		return 0;
	}

	/* Determine if this is an attended or blind transfer */
	if ((replaces = pjsip_param_find(&target_uri->header_param, &str_replaces)) ||
		(replaces = pjsip_param_find(&target_uri->other_param, &str_replaces))) {
		response = refer_incoming_attended_request(session, rdata, target_uri, replaces, progress);
	} else {
		response = refer_incoming_blind_request(session, rdata, target_uri, progress);
	}

	if (!progress) {
		/* The transferer has requested no subscription, so send a final response immediately */
		pjsip_tx_data *tdata;
		const pj_str_t str_refer_sub = { "Refer-Sub", 9 };
		const pj_str_t str_false = { "false", 5 };
		pjsip_hdr *hdr;

		ast_debug(3, "Progress monitoring not requested for REFER on channel '%s' from endpoint '%s', sending immediate response of '%d'\n",
			ast_channel_name(session->channel), ast_sorcery_object_get_id(session->endpoint), response);

		if (pjsip_dlg_create_response(session->inv_session->dlg, rdata, response, NULL, &tdata) != PJ_SUCCESS) {
			pjsip_dlg_respond(session->inv_session->dlg, rdata, response, NULL, NULL, NULL);
			return 0;
		}

		hdr = (pjsip_hdr*)pjsip_generic_string_hdr_create(tdata->pool, &str_refer_sub, &str_false);
		pjsip_msg_add_hdr(tdata->msg, hdr);

		pjsip_dlg_send_response(session->inv_session->dlg, pjsip_rdata_get_tsx(rdata), tdata);
	} else if (response != 200) {
		/* Since this failed we can send a final NOTIFY now and terminate the subscription */
		struct refer_progress_notification *notification = refer_progress_notification_alloc(progress, response, PJSIP_EVSUB_STATE_TERMINATED);

		if (notification) {
			/* The refer_progress_notify function will call ao2_cleanup on this for us */
			refer_progress_notify(notification);
		}
	}

	return 0;
}

static int refer_incoming_request(struct ast_sip_session *session, pjsip_rx_data *rdata)
{
	if (!pjsip_method_cmp(&rdata->msg_info.msg->line.req.method, pjsip_get_refer_method())) {
		return refer_incoming_refer_request(session, rdata);
	} else if (!pjsip_method_cmp(&rdata->msg_info.msg->line.req.method, &pjsip_invite_method)) {
		return refer_incoming_invite_request(session, rdata);
	} else {
		return 0;
	}
}

static void refer_outgoing_request(struct ast_sip_session *session, struct pjsip_tx_data *tdata)
{
	const char *replaces;

	if (pjsip_method_cmp(&tdata->msg->line.req.method, &pjsip_invite_method) ||
		!session->channel ||
		(session->inv_session->state != PJSIP_INV_STATE_CALLING) ||
		!(replaces = pbx_builtin_getvar_helper(session->channel, "SIPREPLACESHDR"))) {
		return;
	}

	ast_sip_add_header(tdata, "Replaces", replaces);
}

static struct ast_sip_session_supplement refer_supplement = {
	.priority = AST_SIP_SESSION_SUPPLEMENT_PRIORITY_CHANNEL + 1,
	.incoming_request = refer_incoming_request,
	.outgoing_request = refer_outgoing_request,
};

static int load_module(void)
{
	const pj_str_t str_norefersub = { "norefersub", 10 };

	pjsip_replaces_init_module(ast_sip_get_pjsip_endpoint());
	pjsip_xfer_init_module(ast_sip_get_pjsip_endpoint());
	pjsip_endpt_add_capability(ast_sip_get_pjsip_endpoint(), NULL, PJSIP_H_SUPPORTED, NULL, 1, &str_norefersub);

	ast_sip_register_service(&refer_progress_module);
	ast_sip_session_register_supplement(&refer_supplement);

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	ast_sip_session_unregister_supplement(&refer_supplement);
	ast_sip_unregister_service(&refer_progress_module);

	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "SIP Blind and Attended Transfer Support",
		.load = load_module,
		.unload = unload_module,
		.load_pri = AST_MODPRI_APP_DEPEND,
		   );
