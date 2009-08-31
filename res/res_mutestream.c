/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Olle E. Johansson <oej@edvina.net>
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

/*! \file
 *
 * \brief MUTE audiohooks
 *
 * \author Olle E. Johansson <oej@edvina.net>
 *
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision: 89545 $")

#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>

#include "asterisk/options.h"
#include "asterisk/logger.h"
#include "asterisk/channel.h"
#include "asterisk/module.h"
#include "asterisk/config.h"
#include "asterisk/file.h"
#include "asterisk/pbx.h"
#include "asterisk/frame.h"
#include "asterisk/utils.h"
#include "asterisk/audiohook.h"
#include "asterisk/manager.h"



/* Our own datastore */
struct mute_information {
	struct ast_audiohook audiohook;
	int mute_write;
	int mute_read;
};


#define TRUE 1
#define FALSE 0

/*! Datastore destroy audiohook callback */
static void destroy_callback(void *data)
{
	struct mute_information *mute = data;

	/* Destroy the audiohook, and destroy ourselves */
	ast_audiohook_destroy(&mute->audiohook);
	free(mute);
	ast_module_unref(ast_module_info->self);

	return;
}

/*! \brief Static structure for datastore information */
static const struct ast_datastore_info mute_datastore = {
	.type = "mute",
	.destroy = destroy_callback
};

/*! \brief Wipe out all audio samples from an ast_frame. Clean it. */
static void ast_frame_clear(struct ast_frame *frame)
{
	struct ast_frame *next;

	for (next = AST_LIST_NEXT(frame, frame_list);
		frame;
		frame = next, next = frame ? AST_LIST_NEXT(frame, frame_list) : NULL) {
 		memset(frame->data, 0, frame->datalen);
        }
}


/*! \brief The callback from the audiohook subsystem. We basically get a frame to have fun with */
static int mute_callback(struct ast_audiohook *audiohook, struct ast_channel *chan, struct ast_frame *frame, enum ast_audiohook_direction direction)
{
	struct ast_datastore *datastore = NULL;
	struct mute_information *mute = NULL;


	/* If the audiohook is stopping it means the channel is shutting down.... but we let the datastore destroy take care of it */
	if (audiohook->status == AST_AUDIOHOOK_STATUS_DONE) {
		return 0;
	}

	ast_channel_lock(chan);
	/* Grab datastore which contains our mute information */
	if (!(datastore = ast_channel_datastore_find(chan, &mute_datastore, NULL))) {
		if (option_debug > 1)
			ast_log(LOG_DEBUG, " *** Can't find any datastore to use. Bad. \n");
		return 0;
	}

	mute = datastore->data;


	/* If this is audio then allow them to increase/decrease the gains */
	if (frame->frametype == AST_FRAME_VOICE) {
		if (option_debug > 3)
			ast_log(LOG_DEBUG, "Audio frame - direction %s  mute READ %s WRITE %s\n", direction == AST_AUDIOHOOK_DIRECTION_READ ? "read" : "write", mute->mute_read ? "on" : "off", mute->mute_write ? "on" : "off");
		
		/* Based on direction of frame grab the gain, and confirm it is applicable */
		if ((direction == AST_AUDIOHOOK_DIRECTION_READ && mute->mute_read) || (direction == AST_AUDIOHOOK_DIRECTION_WRITE && mute->mute_write)) {
			/* Ok, we just want to reset all audio in this frame. Keep NOTHING, thanks. */
 			ast_frame_clear(frame);
		}
	} 
	ast_channel_unlock(chan);

	return 0;
}

/*! \brief Initialize mute hook on channel, but don't activate it */
static struct ast_datastore *initialize_mutehook(struct ast_channel *chan)
{
	struct ast_datastore *datastore = NULL;
	struct mute_information *mute = NULL;

	if (option_debug > 2 )
		ast_log(LOG_DEBUG, "Initializing new Mute Audiohook \n");

	/* Allocate a new datastore to hold the reference to this mute_datastore and audiohook information */
	if (!(datastore = ast_channel_datastore_alloc(&mute_datastore, NULL))) {
		return NULL;
	}

	if (!(mute = ast_calloc(1, sizeof(*mute)))) {
		ast_channel_datastore_free(datastore);
		return NULL;
	}
	ast_audiohook_init(&mute->audiohook, AST_AUDIOHOOK_TYPE_MANIPULATE, "Mute");
	mute->audiohook.manipulate_callback = mute_callback;
	datastore->data = mute;
	return datastore;
}

/*! \brief Add or activate mute audiohook on channel */
static int mute_add_audiohook(struct ast_channel *chan, struct mute_information *mute, struct ast_datastore *datastore)
{
		/* Activate the settings */
		ast_channel_datastore_add(chan, datastore);
		if(ast_audiohook_attach(chan, &mute->audiohook)) {
			ast_log(LOG_ERROR, "Failed to attach audiohook for muting channel %s\n", chan->name);
			return -1;
		} 
		ast_module_ref(ast_module_info->self);
		if (option_debug) {
			ast_log(LOG_DEBUG, "*** Initialized audiohook on channel %s\n", chan->name);
		}
		return 0;
}

/*! \brief Mute dialplan function */
static int func_mute_write(struct ast_channel *chan, char *cmd, char *data, const char *value)
{
	struct ast_datastore *datastore = NULL;
	struct mute_information *mute = NULL;
	int is_new = 0;

	if (!(datastore = ast_channel_datastore_find(chan, &mute_datastore, NULL))) {
		if (!(datastore = initialize_mutehook(chan))) {
			return 0;
		}
		is_new = 1;
	} 

	mute = datastore->data;

	if (!strcasecmp(data, "out")) {
		mute->mute_write = ast_true(value);
		if (option_debug > 1) {
			ast_log(LOG_DEBUG, "%s channel - outbound *** \n", ast_true(value) ? "Muting" : "Unmuting");
		}
	} else if (!strcasecmp(data, "in")) {
		mute->mute_read = ast_true(value);
		if (option_debug > 1) {
			ast_log(LOG_DEBUG, "%s channel - inbound *** \n", ast_true(value) ? "Muting" : "Unmuting");
		}
	} else if (!strcasecmp(data,"all")) {
		mute->mute_write = mute->mute_read = ast_true(value);
	}

	if (is_new) {
		mute_add_audiohook(chan, mute, datastore);
	}

	return 0;
}

/* Function for debugging - might be useful */
static struct ast_custom_function mute_function = {
        .name = "MUTESTREAM",
        .write = func_mute_write,
	.synopsis = "Muting streams in the channel",
	.syntax = "MUTESTREAM(in|out|all) = true|false",
	.desc = "The mute function mutes either inbound (to the PBX) or outbound"
		"audio. \"all\" indicates both directions",
};

static int manager_mutestream(struct mansession *s, const struct message *m)
{
	const char *channel = astman_get_header(m, "Channel");
	const char *id = astman_get_header(m,"ActionID");
	const char *state = astman_get_header(m,"State");
	const char *direction = astman_get_header(m,"Direction");
	char idText[256] = "";
	struct ast_channel *c = NULL;
	struct ast_datastore *datastore = NULL;
	struct mute_information *mute = NULL;
	int is_new = 0;
	int turnon = TRUE;

	if (ast_strlen_zero(channel)) {
		astman_send_error(s, m, "Channel not specified");
		return 0;
	}
	if (ast_strlen_zero(state)) {
		astman_send_error(s, m, "State not specified");
		return 0;
	}
	if (ast_strlen_zero(direction)) {
		astman_send_error(s, m, "Direction not specified");
		return 0;
	}
	/* Ok, we have everything */
	if (!ast_strlen_zero(id)) {
		snprintf(idText, sizeof(idText), "ActionID: %s\r\n", id);
	}

	c = ast_get_channel_by_name_locked(channel);
	if (!c) {
		astman_send_error(s, m, "No such channel");
		return 0;
	}

	if (!(datastore = ast_channel_datastore_find(c, &mute_datastore, NULL))) {
		if (!(datastore = initialize_mutehook(c))) {
			ast_channel_unlock(c);
			return 0;
		}
		is_new = 1;
	} 
	mute = datastore->data;
	turnon = ast_true(state);

	if (!strcasecmp(direction, "in")) {
		mute->mute_read = turnon;
	} else if (!strcasecmp(direction, "out")) {
		mute->mute_write = turnon;
	} else if (!strcasecmp(direction, "all")) {
		mute->mute_read = mute->mute_write = turnon;
	}
	
	if (is_new) {
		mute_add_audiohook(c, mute, datastore);
	}
	ast_channel_unlock(c);

	astman_append(s, "Response: Success\r\n"
				   "%s"
				   "\r\n\r\n", idText);
	return 0;
}


static char mandescr_mutestream[] =
"Description: Mute an incoming or outbound audio stream in a channel.\n"
"Variables: \n"
"  Channel: <name>           The channel you want to mute.\n"
"  Direction: in | out |all  The stream you wan to mute.\n"
"  State: on | off           Whether to turn mute on or off.\n"
"  ActionID: <id>            Optional action ID for this AMI transaction.\n";


static int reload(void)
{
	return 0;
}

static int load_module(void)
{
	ast_custom_function_register(&mute_function);

	ast_manager_register2("MuteStream", EVENT_FLAG_SYSTEM, manager_mutestream,
                        "Mute an audio stream", mandescr_mutestream);
	return 0;
}

static int unload_module(void)
{
	ast_custom_function_unregister(&mute_function);
	/* Unregister AMI actions */
        ast_manager_unregister("MuteStream");

	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_GLOBAL_SYMBOLS, "MUTE resource",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );
