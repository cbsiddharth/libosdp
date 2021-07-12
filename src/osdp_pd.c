/*
 * Copyright (c) 2019-2021 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "osdp_common.h"
#include "osdp_file.h"
#include "osdp_trs.h"

#ifndef CONFIG_OSDP_STATIC_PD
#include <stdlib.h>
#endif

#define LOG_TAG "PD : "

#define CMD_POLL_DATA_LEN	0
#define CMD_LSTAT_DATA_LEN	0
#define CMD_ISTAT_DATA_LEN	0
#define CMD_OSTAT_DATA_LEN	0
#define CMD_RSTAT_DATA_LEN	0
#define CMD_ID_DATA_LEN		1
#define CMD_CAP_DATA_LEN	1
#define CMD_OUT_DATA_LEN	4
#define CMD_LED_DATA_LEN	14
#define CMD_BUZ_DATA_LEN	5
#define CMD_TEXT_DATA_LEN	6 /* variable length command */
#define CMD_COMSET_DATA_LEN	5
#define CMD_MFG_DATA_LEN	4 /* variable length command */
#define CMD_KEYSET_DATA_LEN	18
#define CMD_CHLNG_DATA_LEN	8
#define CMD_SCRYPT_DATA_LEN	16
#define CMD_ABORT_DATA_LEN	0
#define CMD_ACURXSIZE_DATA_LEN	2
#define CMD_KEEPACTIVE_DATA_LEN 2

#define REPLY_ACK_LEN	       1
#define REPLY_PDID_LEN	       13
#define REPLY_PDCAP_LEN	       1 /* variable length command */
#define REPLY_PDCAP_ENTITY_LEN 3
#define REPLY_LSTATR_LEN       3
#define REPLY_RSTATR_LEN       2
#define REPLY_KEYPAD_LEN       2
#define REPLY_RAW_LEN	       4
#define REPLY_FMT_LEN	       3
#define REPLY_COM_LEN	       6
#define REPLY_NAK_LEN	       2
#define REPLY_MFGREP_LEN       4 /* variable length command */
#define REPLY_CCRYPT_LEN       33
#define REPLY_RMAC_I_LEN       17

enum osdp_pd_error_e {
	OSDP_PD_ERR_NONE = 0,
	OSDP_PD_ERR_NO_DATA = -1,
	OSDP_PD_ERR_GENERIC = -2,
	OSDP_PD_ERR_REPLY = -3,
	OSDP_PD_ERR_IGNORE = -4,
};

/* Implicit cababilities */
static struct osdp_pd_cap osdp_pd_cap[] = {
	/* Driver Implicit cababilities */
	{
		OSDP_PD_CAP_CHECK_CHARACTER_SUPPORT,
		1, /* The PD supports the 16-bit CRC-16 mode */
		0, /* N/A */
	},
	{
		OSDP_PD_CAP_COMMUNICATION_SECURITY,
		1, /* (Bit-0) AES128 support */
		0, /* N/A */
	},
	{
		OSDP_PD_CAP_RECEIVE_BUFFERSIZE,
		BYTE_0(OSDP_PACKET_BUF_SIZE),
		BYTE_1(OSDP_PACKET_BUF_SIZE),
	},
	{ -1, 0, 0 } /* Sentinel */
};

struct pd_event_node {
	queue_node_t node;
	struct osdp_event object;
};

static int pd_event_queue_init(struct osdp_pd *pd)
{
	if (slab_init(&pd->event.slab, sizeof(struct pd_event_node),
		      pd->event.slab_blob, sizeof(pd->event.slab_blob)) < 0) {
		LOG_ERR("Failed to initialize command slab");
		return -1;
	}
	queue_init(&pd->event.queue);
	return 0;
}

static struct osdp_event *pd_event_alloc(struct osdp_pd *pd)
{
	struct pd_event_node *event = NULL;

	if (slab_alloc(&pd->event.slab, (void **)&event)) {
		LOG_ERR("Event slab allocation failed");
		return NULL;
	}
	return &event->object;
}

static void pd_event_free(struct osdp_pd *pd, struct osdp_event *event)
{
	ARG_UNUSED(pd);
	struct pd_event_node *n;

	n = CONTAINER_OF(event, struct pd_event_node, object);
	assert(slab_free(n) == 0);
}

static void pd_event_enqueue(struct osdp_pd *pd, struct osdp_event *event)
{
	struct pd_event_node *n;

	n = CONTAINER_OF(event, struct pd_event_node, object);
	queue_enqueue(&pd->event.queue, &n->node);
}

static int pd_event_dequeue(struct osdp_pd *pd, struct osdp_event **event)
{
	struct pd_event_node *n;
	queue_node_t *node;

	if (queue_dequeue(&pd->event.queue, &node)) {
		return -1;
	}
	n = CONTAINER_OF(node, struct pd_event_node, node);
	*event = &n->object;
	return 0;
}

static int pd_translate_event(struct osdp_event *event, uint8_t *data)
{
	int reply_code = 0;

	switch (event->type) {
	case OSDP_EVENT_CARDREAD:
		if (event->cardread.format == OSDP_CARD_FMT_RAW_UNSPECIFIED ||
		    event->cardread.format == OSDP_CARD_FMT_RAW_WIEGAND) {
			reply_code = REPLY_RAW;
		} else if (event->cardread.format == OSDP_CARD_FMT_ASCII) {
			reply_code = REPLY_FMT;
		} else {
			LOG_ERR("Event: cardread; Error: unknown format");
			break;
		}
		break;
	case OSDP_EVENT_KEYPRESS:
		reply_code = REPLY_KEYPPAD;
		break;
	default:
		LOG_ERR("Unknown event type %d", event->type);
		break;
	}
	if (reply_code == 0) {
		/* POLL command cannot fail even when there are errors here */
		return REPLY_ACK;
	}
	memcpy(data, event, sizeof(struct osdp_event));
	return reply_code;
}

static int pd_cmd_cap_ok(struct osdp_pd *pd, struct osdp_cmd *cmd)
{
	struct osdp_pd_cap *cap = NULL;

	/* Validate the cmd_id against a PD capabilities where applicable */
	switch (pd->cmd_id) {
	case CMD_ISTAT:
		cap = &pd->cap[OSDP_PD_CAP_CONTACT_STATUS_MONITORING];
		if (cap->num_items == 0 || cap->compliance_level == 0) {
			break;
		}
		return 0; /* Remove this when REPLY_ISTATR is supported */
	case CMD_OSTAT:
		cap = &pd->cap[OSDP_PD_CAP_OUTPUT_CONTROL];
		if (cap->num_items == 0 || cap->compliance_level == 0) {
			break;
		}
		return 0; /* Remove this when REPLY_OSTATR is supported */
	case CMD_OUT:
		cap = &pd->cap[OSDP_PD_CAP_OUTPUT_CONTROL];
		if (cmd->output.output_no + 1 > cap->num_items) {
			LOG_DBG("CAP check: output_no(%d) > cap->num_items(%d)",
				cmd->output.output_no + 1, cap->num_items);
			break;
		}
		if (cap->compliance_level == 0) {
			break;
		}
		return 1;
	case CMD_LED:
		cap = &pd->cap[OSDP_PD_CAP_READER_LED_CONTROL];
		if (cmd->led.led_number + 1 > cap->num_items) {
			LOG_DBG("CAP check: LED(%d) > cap->num_items(%d)",
				cmd->led.led_number + 1, cap->num_items);
			break;
		}
		if (cap->compliance_level == 0) {
			break;
		}
		return 1;
	case CMD_BUZ:
		cap = &pd->cap[OSDP_PD_CAP_READER_AUDIBLE_OUTPUT];
		if (cap->num_items == 0 || cap->compliance_level == 0) {
			break;
		}
		return 1;
	case CMD_TEXT:
		cap = &pd->cap[OSDP_PD_CAP_READER_TEXT_OUTPUT];
		if (cap->num_items == 0 || cap->compliance_level == 0) {
			break;
		}
		return 1;
	case CMD_CHLNG:
	case CMD_SCRYPT:
	case CMD_KEYSET:
		cap = &pd->cap[OSDP_PD_CAP_COMMUNICATION_SECURITY];
		if (cap->compliance_level == 0) {
			pd->reply_id = REPLY_NAK;
			pd->ephemeral_data[0] = OSDP_PD_NAK_SC_UNSUP;
			return 0;
		}
		return 1;
	}

	pd->reply_id = REPLY_NAK;
	pd->ephemeral_data[0] = OSDP_PD_NAK_CMD_UNKNOWN;
	return 0;
}

static int pd_decode_command(struct osdp_pd *pd, uint8_t *buf, int len)
{
	int i, ret = OSDP_PD_ERR_GENERIC, pos = 0;
	struct osdp_cmd cmd;
	struct osdp_event *event;

	pd->reply_id = 0;
	pd->cmd_id = cmd.id = buf[pos++];
	len--;

	if (is_enforce_secure(pd) && !sc_is_active(pd)) {
		/**
		 * Only CMD_ID, CMD_CAP and SC handshake commands (CMD_CHLNG
		 * and CMD_SCRYPT) are allowed when SC is inactive and
		 * ENFORCE_SECURE was requested.
		 */
		if (pd->cmd_id != CMD_ID && pd->cmd_id != CMD_CAP &&
		    pd->cmd_id != CMD_CHLNG && pd->cmd_id != CMD_SCRYPT) {
			LOG_ERR("CMD(%02x) not allowed due to ENFORCE_SECURE",
				pd->cmd_id);
			pd->reply_id = REPLY_NAK;
			pd->ephemeral_data[0] = OSDP_PD_NAK_SC_COND;
			return OSDP_PD_ERR_REPLY;
		}
	}

/* helper macro, can be called from switch cases below */
#define PD_CMD_CAP_CHECK(pd, cmd)                                              \
	if (!pd_cmd_cap_ok(pd, cmd)) {                                         \
		LOG_INF("PD is not capable of handling CMD(%02x); "            \
			"Reply with NAK_CMD_UNKNOWN",                          \
			pd->cmd_id);                                           \
		ret = OSDP_PD_ERR_REPLY;                                       \
		break;                                                         \
	}

#define ASSERT_LENGTH(got, exp)                                                \
	if (got != exp) {                                                      \
		LOG_ERR("CMD(%02x) length error! Got:%d, Exp:%d", pd->cmd_id,  \
			got, exp);                                             \
		return OSDP_PD_ERR_GENERIC;                                    \
	}

	switch (pd->cmd_id) {
	case CMD_POLL:
		ASSERT_LENGTH(len, CMD_POLL_DATA_LEN);
		/* Check if we have external events in the queue */
		if (pd_event_dequeue(pd, &event) == 0) {
			ret = pd_translate_event(event, pd->ephemeral_data);
			pd->reply_id = ret;
			pd_event_free(pd, event);
		} else {
			pd->reply_id = REPLY_ACK;
		}
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_LSTAT:
		ASSERT_LENGTH(len, CMD_LSTAT_DATA_LEN);
		pd->reply_id = REPLY_LSTATR;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_ISTAT:
		ASSERT_LENGTH(len, CMD_ISTAT_DATA_LEN);
		PD_CMD_CAP_CHECK(pd, NULL);
		pd->reply_id = REPLY_ISTATR;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_OSTAT:
		ASSERT_LENGTH(len, CMD_OSTAT_DATA_LEN);
		PD_CMD_CAP_CHECK(pd, NULL);
		pd->reply_id = REPLY_OSTATR;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_RSTAT:
		ASSERT_LENGTH(len, CMD_RSTAT_DATA_LEN);
		pd->reply_id = REPLY_RSTATR;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_ID:
		ASSERT_LENGTH(len, CMD_ID_DATA_LEN);
		pos++; /* Skip reply type info. */
		pd->reply_id = REPLY_PDID;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_CAP:
		ASSERT_LENGTH(len, CMD_CAP_DATA_LEN);
		pos++; /* Skip reply type info. */
		pd->reply_id = REPLY_PDCAP;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_OUT:
		ASSERT_LENGTH(len, CMD_OUT_DATA_LEN);
		if (!pd->command_callback) {
			break;
		}
		cmd.id = OSDP_CMD_OUTPUT;
		cmd.output.output_no = buf[pos++];
		cmd.output.control_code = buf[pos++];
		cmd.output.timer_count = buf[pos++];
		cmd.output.timer_count |= buf[pos++] << 8;
		PD_CMD_CAP_CHECK(pd, &cmd);
		ret = pd->command_callback(pd->command_callback_arg, &cmd);
		if (ret != 0) {
			pd->reply_id = REPLY_NAK;
			pd->ephemeral_data[0] = OSDP_PD_NAK_RECORD;
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		pd->reply_id = REPLY_ACK;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_LED:
		ASSERT_LENGTH(len, CMD_LED_DATA_LEN);
		if (!pd->command_callback) {
			break;
		}
		cmd.id = OSDP_CMD_LED;
		cmd.led.reader = buf[pos++];
		cmd.led.led_number = buf[pos++];

		cmd.led.temporary.control_code = buf[pos++];
		cmd.led.temporary.on_count = buf[pos++];
		cmd.led.temporary.off_count = buf[pos++];
		cmd.led.temporary.on_color = buf[pos++];
		cmd.led.temporary.off_color = buf[pos++];
		cmd.led.temporary.timer_count = buf[pos++];
		cmd.led.temporary.timer_count |= buf[pos++] << 8;

		cmd.led.permanent.control_code = buf[pos++];
		cmd.led.permanent.on_count = buf[pos++];
		cmd.led.permanent.off_count = buf[pos++];
		cmd.led.permanent.on_color = buf[pos++];
		cmd.led.permanent.off_color = buf[pos++];
		PD_CMD_CAP_CHECK(pd, &cmd);
		ret = pd->command_callback(pd->command_callback_arg, &cmd);
		if (ret != 0) {
			pd->reply_id = REPLY_NAK;
			pd->ephemeral_data[0] = OSDP_PD_NAK_RECORD;
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		pd->reply_id = REPLY_ACK;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_BUZ:
		ASSERT_LENGTH(len, CMD_BUZ_DATA_LEN);
		if (!pd->command_callback) {
			break;
		}
		cmd.id = OSDP_CMD_BUZZER;
		cmd.buzzer.reader = buf[pos++];
		cmd.buzzer.control_code = buf[pos++];
		cmd.buzzer.on_count = buf[pos++];
		cmd.buzzer.off_count = buf[pos++];
		cmd.buzzer.rep_count = buf[pos++];
		PD_CMD_CAP_CHECK(pd, &cmd);
		ret = pd->command_callback(pd->command_callback_arg, &cmd);
		if (ret != 0) {
			pd->reply_id = REPLY_NAK;
			pd->ephemeral_data[0] = OSDP_PD_NAK_RECORD;
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		pd->reply_id = REPLY_ACK;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_TEXT:
		if (len < CMD_TEXT_DATA_LEN || !pd->command_callback) {
			break;
		}
		cmd.id = OSDP_CMD_TEXT;
		cmd.text.reader = buf[pos++];
		cmd.text.control_code = buf[pos++];
		cmd.text.temp_time = buf[pos++];
		cmd.text.offset_row = buf[pos++];
		cmd.text.offset_col = buf[pos++];
		cmd.text.length = buf[pos++];
		if (cmd.text.length > OSDP_CMD_TEXT_MAX_LEN ||
		    ((len - CMD_TEXT_DATA_LEN) < cmd.text.length) ||
		    cmd.text.length > OSDP_CMD_TEXT_MAX_LEN) {
			break;
		}
		for (i = 0; i < cmd.text.length; i++) {
			cmd.text.data[i] = buf[pos++];
		}
		PD_CMD_CAP_CHECK(pd, &cmd);
		ret = pd->command_callback(pd->command_callback_arg, &cmd);
		if (ret != 0) {
			pd->reply_id = REPLY_NAK;
			pd->ephemeral_data[0] = OSDP_PD_NAK_RECORD;
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		pd->reply_id = REPLY_ACK;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_COMSET:
		ASSERT_LENGTH(len, CMD_COMSET_DATA_LEN);
		if (!pd->command_callback) {
			break;
		}
		cmd.id = OSDP_CMD_COMSET;
		cmd.comset.address = buf[pos++];
		cmd.comset.baud_rate = buf[pos++];
		cmd.comset.baud_rate |= buf[pos++] << 8;
		cmd.comset.baud_rate |= buf[pos++] << 16;
		cmd.comset.baud_rate |= buf[pos++] << 24;
		if (cmd.comset.address >= 0x7F ||
		    (cmd.comset.baud_rate != 9600 &&
		     cmd.comset.baud_rate != 19200 &&
		     cmd.comset.baud_rate != 38400 &&
		     cmd.comset.baud_rate != 115200 &&
		     cmd.comset.baud_rate != 230400)) {
			LOG_ERR("COMSET Failed! command discarded");
			cmd.comset.address = pd->address;
			cmd.comset.baud_rate = pd->baud_rate;
		}
		ret = pd->command_callback(pd->command_callback_arg, &cmd);
		if (ret != 0) {
			pd->reply_id = REPLY_NAK;
			pd->ephemeral_data[0] = OSDP_PD_NAK_RECORD;
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		memcpy(pd->ephemeral_data, &cmd, sizeof(struct osdp_cmd));
		pd->reply_id = REPLY_COM;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_MFG:
		if (len < CMD_MFG_DATA_LEN || !pd->command_callback) {
			break;
		}
		cmd.id = OSDP_CMD_MFG;
		cmd.mfg.vendor_code = buf[pos++]; /* vendor_code */
		cmd.mfg.vendor_code |= buf[pos++] << 8;
		cmd.mfg.vendor_code |= buf[pos++] << 16;
		cmd.mfg.command = buf[pos++];
		cmd.mfg.length = len - CMD_MFG_DATA_LEN;
		if (cmd.mfg.length > OSDP_CMD_MFG_MAX_DATALEN) {
			LOG_ERR("cmd length error");
			break;
		}
		for (i = 0; i < cmd.mfg.length; i++) {
			cmd.mfg.data[i] = buf[pos++];
		}
		ret = pd->command_callback(pd->command_callback_arg, &cmd);
		if (ret < 0) { /* Errors */
			pd->reply_id = REPLY_NAK;
			pd->ephemeral_data[0] = OSDP_PD_NAK_RECORD;
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		if (ret > 0) { /* App wants to send a REPLY_MFGREP to the CP */
			memcpy(pd->ephemeral_data, &cmd,
			       sizeof(struct osdp_cmd));
			pd->reply_id = REPLY_MFGREP;
		} else {
			pd->reply_id = REPLY_ACK;
		}
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_ACURXSIZE:
		ASSERT_LENGTH(len, CMD_ACURXSIZE_DATA_LEN);
		pd->peer_rx_size = buf[pos] | (buf[pos + 1] << 8);
		pd->reply_id = REPLY_ACK;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_KEEPACTIVE:
		ASSERT_LENGTH(len, CMD_KEEPACTIVE_DATA_LEN);
		pd->sc_tstamp += buf[pos] | (buf[pos + 1] << 8);
		pd->reply_id = REPLY_ACK;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_ABORT:
		ASSERT_LENGTH(len, CMD_ABORT_DATA_LEN);
		osdp_file_tx_abort(pd);
		pd->reply_id = REPLY_ACK;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_FILETRANSFER:
		ret = osdp_file_cmd_tx_decode(pd, buf + pos, len);
		if (ret == 0) {
			ret = OSDP_PD_ERR_NONE;
			pd->reply_id = REPLY_FTSTAT;
			break;
		}
		break;
	case CMD_KEYSET:
		PD_CMD_CAP_CHECK(pd, &cmd);
		ASSERT_LENGTH(len, CMD_KEYSET_DATA_LEN);
		/**
		 * For CMD_KEYSET to be accepted, PD must be
		 * ONLINE and SC_ACTIVE.
		 */
		if (!sc_is_active(pd)) {
			pd->reply_id = REPLY_NAK;
			pd->ephemeral_data[0] = OSDP_PD_NAK_SC_COND;
			LOG_ERR("Keyset with SC inactive");
			break;
		}
		/* only key_type == 1 (SCBK) and key_len == 16 is supported */
		if (buf[pos] != 1 || buf[pos + 1] != 16) {
			LOG_ERR("Keyset invalid len/type: %d/%d", buf[pos],
				buf[pos + 1]);
			break;
		}
		cmd.id = OSDP_CMD_KEYSET;
		cmd.keyset.type = buf[pos++];
		cmd.keyset.length = buf[pos++];
		memcpy(cmd.keyset.data, buf + pos, 16);
		memcpy(pd->sc.scbk, buf + pos, 16);
		ret = OSDP_PD_ERR_NONE;
		if (pd->command_callback) {
			ret = pd->command_callback(pd->command_callback_arg,
						   &cmd);
		} else {
			LOG_WRN("Keyset without command callback trigger");
		}
		if (ret != 0) {
			pd->reply_id = REPLY_NAK;
			pd->ephemeral_data[0] = OSDP_PD_NAK_RECORD;
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		CLEAR_FLAG(pd, PD_FLAG_SC_USE_SCBKD);
		CLEAR_FLAG(pd, OSDP_FLAG_INSTALL_MODE);
		pd->reply_id = REPLY_ACK;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_CHLNG:
		PD_CMD_CAP_CHECK(pd, &cmd);
		ASSERT_LENGTH(len, CMD_CHLNG_DATA_LEN);
		sc_deactivate(pd);
		osdp_sc_setup(pd);
		for (i = 0; i < CMD_CHLNG_DATA_LEN; i++) {
			pd->sc.cp_random[i] = buf[pos++];
		}
		pd->reply_id = REPLY_CCRYPT;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_SCRYPT:
		PD_CMD_CAP_CHECK(pd, &cmd);
		ASSERT_LENGTH(len, CMD_SCRYPT_DATA_LEN);
		for (i = 0; i < CMD_SCRYPT_DATA_LEN; i++) {
			pd->sc.cp_cryptogram[i] = buf[pos++];
		}
		pd->reply_id = REPLY_RMAC_I;
		ret = OSDP_PD_ERR_NONE;
		break;
	default:
		LOG_ERR("Unknown command ID %02x", pd->cmd_id);
		pd->reply_id = REPLY_NAK;
		pd->ephemeral_data[0] = OSDP_PD_NAK_CMD_UNKNOWN;
		ret = OSDP_PD_ERR_NONE;
		break;
	}

	if (ret != 0 && ret != OSDP_PD_ERR_REPLY) {
		LOG_ERR("Invalid command structure. CMD: %02x, Len: %d ret: %d",
			pd->cmd_id, len, ret);
		pd->reply_id = REPLY_NAK;
		pd->ephemeral_data[0] = OSDP_PD_NAK_CMD_LEN;
		return OSDP_PD_ERR_REPLY;
	}

	if (pd->cmd_id != CMD_POLL) {
		LOG_DBG("CMD: %02x REPLY: %02x", pd->cmd_id, pd->reply_id);
	}

	return ret;
}

/**
 * Returns:
 * +ve: length of command
 * -ve: error
 */
static int pd_build_reply(struct osdp_pd *pd, uint8_t *buf, int max_len)
{
	int i, data_off, len = 0, ret = -1;
	uint8_t t1, *smb;
	struct osdp_event *event;
	struct osdp_cmd *cmd;

	data_off = osdp_phy_packet_get_data_offset(pd, buf);
	smb = osdp_phy_packet_get_smb(pd, buf);
	buf += data_off;
	max_len -= data_off;

#define ASSERT_BUF_LEN(need)                                                   \
	if (max_len < need) {                                                  \
		LOG_ERR("OOM at build REPLY(%02x) - have:%d, need:%d",         \
			pd->reply_id, max_len, need);                          \
		return OSDP_PD_ERR_GENERIC;                                    \
	}

	switch (pd->reply_id) {
	case REPLY_ACK:
		ASSERT_BUF_LEN(REPLY_ACK_LEN);
		buf[len++] = pd->reply_id;
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_PDID:
		ASSERT_BUF_LEN(REPLY_PDID_LEN);
		buf[len++] = pd->reply_id;

		buf[len++] = BYTE_0(pd->id.vendor_code);
		buf[len++] = BYTE_1(pd->id.vendor_code);
		buf[len++] = BYTE_2(pd->id.vendor_code);

		buf[len++] = pd->id.model;
		buf[len++] = pd->id.version;

		buf[len++] = BYTE_0(pd->id.serial_number);
		buf[len++] = BYTE_1(pd->id.serial_number);
		buf[len++] = BYTE_2(pd->id.serial_number);
		buf[len++] = BYTE_3(pd->id.serial_number);

		buf[len++] = BYTE_3(pd->id.firmware_version);
		buf[len++] = BYTE_2(pd->id.firmware_version);
		buf[len++] = BYTE_1(pd->id.firmware_version);
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_PDCAP:
		ASSERT_BUF_LEN(REPLY_PDCAP_LEN);
		buf[len++] = pd->reply_id;
		for (i = 1; i < OSDP_PD_CAP_SENTINEL; i++) {
			if (pd->cap[i].function_code != i) {
				continue;
			}
			if (max_len < REPLY_PDCAP_ENTITY_LEN) {
				LOG_ERR("Out of buffer space!");
				break;
			}
			buf[len++] = i;
			buf[len++] = pd->cap[i].compliance_level;
			buf[len++] = pd->cap[i].num_items;
			max_len -= REPLY_PDCAP_ENTITY_LEN;
		}
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_LSTATR:
		ASSERT_BUF_LEN(REPLY_LSTATR_LEN);
		buf[len++] = pd->reply_id;
		buf[len++] = ISSET_FLAG(pd, PD_FLAG_TAMPER);
		buf[len++] = ISSET_FLAG(pd, PD_FLAG_POWER);
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_RSTATR:
		ASSERT_BUF_LEN(REPLY_RSTATR_LEN);
		buf[len++] = pd->reply_id;
		buf[len++] = ISSET_FLAG(pd, PD_FLAG_R_TAMPER);
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_KEYPPAD:
		event = (struct osdp_event *)pd->ephemeral_data;
		ASSERT_BUF_LEN(REPLY_KEYPAD_LEN + event->keypress.length);
		buf[len++] = pd->reply_id;
		buf[len++] = (uint8_t)event->keypress.reader_no;
		buf[len++] = (uint8_t)event->keypress.length;
		for (i = 0; i < event->keypress.length; i++) {
			buf[len++] = event->keypress.data[i];
		}
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_RAW:
		event = (struct osdp_event *)pd->ephemeral_data;
		t1 = (event->cardread.length + 7) / 8;
		ASSERT_BUF_LEN(REPLY_RAW_LEN + t1);
		buf[len++] = pd->reply_id;
		buf[len++] = (uint8_t)event->cardread.reader_no;
		buf[len++] = (uint8_t)event->cardread.format;
		buf[len++] = BYTE_0(event->cardread.length);
		buf[len++] = BYTE_1(event->cardread.length);
		for (i = 0; i < t1; i++) {
			buf[len++] = event->cardread.data[i];
		}
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_FMT:
		event = (struct osdp_event *)pd->ephemeral_data;
		ASSERT_BUF_LEN(REPLY_FMT_LEN + event->cardread.length);
		buf[len++] = pd->reply_id;
		buf[len++] = (uint8_t)event->cardread.reader_no;
		buf[len++] = (uint8_t)event->cardread.direction;
		buf[len++] = (uint8_t)event->cardread.length;
		for (i = 0; i < event->cardread.length; i++) {
			buf[len++] = event->cardread.data[i];
		}
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_COM:
		ASSERT_BUF_LEN(REPLY_COM_LEN);
		/**
		 * If COMSET succeeds, the PD must reply with the old params and
		 * then switch to the new params from then then on. We have the
		 * new params in the commands struct that we just enqueued so
		 * we can peek at tail of command queue and set that to
		 * pd->addr/pd->baud_rate.
		 */
		cmd = (struct osdp_cmd *)pd->ephemeral_data;
		buf[len++] = pd->reply_id;
		buf[len++] = cmd->comset.address;
		buf[len++] = BYTE_0(cmd->comset.baud_rate);
		buf[len++] = BYTE_1(cmd->comset.baud_rate);
		buf[len++] = BYTE_2(cmd->comset.baud_rate);
		buf[len++] = BYTE_3(cmd->comset.baud_rate);

		pd->address = (int)cmd->comset.address;
		pd->baud_rate = (int)cmd->comset.baud_rate;
		LOG_INF("COMSET Succeeded! New PD-Addr: %d; Baud: %d",
			pd->address, pd->baud_rate);
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_NAK:
		ASSERT_BUF_LEN(REPLY_NAK_LEN);
		buf[len++] = pd->reply_id;
		buf[len++] = pd->ephemeral_data[0];
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_MFGREP:
		cmd = (struct osdp_cmd *)pd->ephemeral_data;
		ASSERT_BUF_LEN(REPLY_MFGREP_LEN + cmd->mfg.length);
		buf[len++] = pd->reply_id;
		buf[len++] = BYTE_0(cmd->mfg.vendor_code);
		buf[len++] = BYTE_1(cmd->mfg.vendor_code);
		buf[len++] = BYTE_2(cmd->mfg.vendor_code);
		buf[len++] = cmd->mfg.command;
		for (i = 0; i < cmd->mfg.length; i++) {
			buf[len++] = cmd->mfg.data[i];
		}
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_FTSTAT:
		buf[len++] = pd->reply_id;
		ret = osdp_file_cmd_stat_build(pd, buf + len, max_len);
		if (ret <= 0) {
			break;
		}
		len += ret;
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_CCRYPT:
		if (smb == NULL) {
			break;
		}
		ASSERT_BUF_LEN(REPLY_CCRYPT_LEN);
		osdp_get_rand(pd->sc.pd_random, 8);
		osdp_compute_session_keys(TO_CTX(pd));
		osdp_compute_pd_cryptogram(pd);
		buf[len++] = pd->reply_id;
		for (i = 0; i < 8; i++) {
			buf[len++] = pd->sc.pd_client_uid[i];
		}
		for (i = 0; i < 8; i++) {
			buf[len++] = pd->sc.pd_random[i];
		}
		for (i = 0; i < 16; i++) {
			buf[len++] = pd->sc.pd_cryptogram[i];
		}
		smb[0] = 3; /* length */
		smb[1] = SCS_12; /* type */
		smb[2] = ISSET_FLAG(pd, PD_FLAG_SC_USE_SCBKD) ? 0 : 1;
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_RMAC_I:
		if (smb == NULL) {
			break;
		}
		ASSERT_BUF_LEN(REPLY_RMAC_I_LEN);
		osdp_compute_rmac_i(pd);
		buf[len++] = pd->reply_id;
		for (i = 0; i < 16; i++) {
			buf[len++] = pd->sc.r_mac[i];
		}
		smb[0] = 3; /* length */
		smb[1] = SCS_14; /* type */
		if (osdp_verify_cp_cryptogram(pd) == 0) {
			smb[2] = 1; /* CP auth succeeded */
			sc_activate(pd);
			pd->sc_tstamp = osdp_millis_now();
			if (ISSET_FLAG(pd, PD_FLAG_SC_USE_SCBKD)) {
				LOG_WRN("SC Active with SCBK-D");
			} else {
				LOG_INF("SC Active");
			}
		} else {
			smb[2] = 0; /* CP auth failed */
			LOG_WRN("failed to verify CP_crypt");
		}
		ret = OSDP_PD_ERR_NONE;
		break;
	}

	if (smb && (smb[1] > SCS_14) && sc_is_active(pd)) {
		smb[0] = 2; /* length */
		smb[1] = (len > 1) ? SCS_18 : SCS_16;
	}

	if (ret != 0) {
		/* catch all errors and report it as a RECORD error to CP */
		LOG_ERR("Failed to build REPLY(%02x); Sending NAK instead!",
			pd->reply_id);
		ASSERT_BUF_LEN(REPLY_NAK_LEN);
		buf[0] = REPLY_NAK;
		buf[1] = OSDP_PD_NAK_RECORD;
		len = 2;
	}

	return len;
}

/**
 * pd_send_reply - blocking send; doesn't handle partials
 * Returns:
 *   0 - success
 *  -1 - failure
 */
static int pd_send_reply(struct osdp_pd *pd)
{
	int ret, len;

	/* init packet buf with header */
	len = osdp_phy_packet_init(pd, pd->rx_buf, sizeof(pd->rx_buf));
	if (len < 0) {
		return OSDP_PD_ERR_GENERIC;
	}

	/* fill reply data */
	ret = pd_build_reply(pd, pd->rx_buf, sizeof(pd->rx_buf));
	if (ret <= 0) {
		return OSDP_PD_ERR_GENERIC;
	}
	len += ret;

	/* finalize packet */
	len = osdp_phy_packet_finalize(pd, pd->rx_buf, len, sizeof(pd->rx_buf));
	if (len < 0) {
		return OSDP_PD_ERR_GENERIC;
	}

	/* flush rx to remove any invalid data. */
	if (pd->channel.flush) {
		pd->channel.flush(pd->channel.data);
	}

	ret = pd->channel.send(pd->channel.data, pd->rx_buf, len);
	if (ret != len) {
		LOG_ERR("Channel send for %d bytes failed! ret: %d", len, ret);
		return OSDP_PD_ERR_GENERIC;
	}

	if (IS_ENABLED(CONFIG_OSDP_PACKET_TRACE)) {
		if (pd->cmd_id != CMD_POLL) {
			osdp_dump(pd->rx_buf, pd->rx_buf_len,
				  "OSDP: PD[%d]: Sent", pd->address);
		}
	}

	return OSDP_PD_ERR_NONE;
}

static int pd_decode_packet(struct osdp_pd *pd, int *one_pkt_len)
{
	int err, len;
	uint8_t *buf;

	err = osdp_phy_check_packet(pd, pd->rx_buf, pd->rx_buf_len, &len);

	/* Translate phy error codes to PD errors */
	switch(err) {
	case OSDP_ERR_PKT_NONE: break;
	case OSDP_ERR_PKT_NACK: return OSDP_PD_ERR_REPLY;
	case OSDP_ERR_PKT_WAIT: return OSDP_PD_ERR_NO_DATA;
	case OSDP_ERR_PKT_SKIP: return OSDP_PD_ERR_IGNORE;
	case OSDP_ERR_PKT_FMT: return OSDP_PD_ERR_GENERIC;
	default: return err; /* propagate other errors as-is */
	}

	*one_pkt_len = len;

	len = osdp_phy_decode_packet(pd, pd->rx_buf, len, &buf);
	if (len <= 0) {
		if (len == OSDP_ERR_PKT_NACK) {
			return OSDP_PD_ERR_REPLY; /* Send a NAK */
		}
		return OSDP_PD_ERR_GENERIC; /* fatal errors */
	}

	return pd_decode_command(pd, buf, len);
}

static int pd_receive_and_process_command(struct osdp_pd *pd)
{
	uint8_t *buf;
	int len, err, remaining, pos;

	buf = pd->rx_buf + pd->rx_buf_len;
	remaining = sizeof(pd->rx_buf) - pd->rx_buf_len;

	len = pd->channel.recv(pd->channel.data, buf, remaining);
	if (len <= 0) { /* No data received */
		return OSDP_PD_ERR_NO_DATA;
	}

	/**
	 * We received some data on the bus; update pd->tstamp. A rouge CP can
	 * send one byte at a time to extend this command's window but that
	 * shouldn't cause any issues related to secure channel as it has it's
	 * own timestamp.
	 */
	pd->tstamp = osdp_millis_now();
	pd->rx_buf_len += len;

	if (IS_ENABLED(CONFIG_OSDP_PACKET_TRACE)) {
		/**
		 * A crude way of identifying and not printing poll messages
		 * when CONFIG_OSDP_PACKET_TRACE is enabled. This is an early
		 * print to catch errors so keeping it simple.
		 * OSDP_CMD_ID_OFFSET + 2 is also checked as the CMD_ID can be
		 * pushed back by 2 bytes if secure channel block is present in
		 * header.
		 */
		pos = OSDP_CMD_ID_OFFSET;
		if (sc_is_active(pd)) {
			pos += 2;
		}
		if (pd->rx_buf_len > pos && pd->rx_buf[pos] != CMD_POLL) {
			osdp_dump(pd->rx_buf, pd->rx_buf_len,
				  "OSDP: PD[%d]: Received", pd->address);
		}
	}

	err = pd_decode_packet(pd, &len);

	/* We are done with the packet (error or not). Remove processed bytes */
	remaining = pd->rx_buf_len - len;
	if (remaining) {
		memmove(pd->rx_buf, pd->rx_buf + len, remaining);
	}

	/**
	 * Store remaining length that needs to be processed.
	 * State machine will be updated accordingly.
	 */
	pd->rx_buf_len = remaining;

	return err;
}

static inline void pd_error_reset(struct osdp_pd *pd)
{
	sc_deactivate(pd);
	if (pd->channel.flush) {
		pd->channel.flush(pd->channel.data);
	}
	pd->rx_buf_len = 0;
}

static void osdp_pd_update(struct osdp_pd *pd)
{
	int ret;

	/**
	 * If secure channel is established, we need to make sure that
	 * the session is valid before accepting a command.
	 */
	if (sc_is_active(pd) &&
	    osdp_millis_since(pd->sc_tstamp) > OSDP_PD_SC_TIMEOUT_MS) {
		LOG_INF("PD SC session timeout!");
		sc_deactivate(pd);
	}

	ret = pd_receive_and_process_command(pd);

	if (ret == OSDP_PD_ERR_IGNORE) {
		return;
	}

	if (ret == OSDP_PD_ERR_NO_DATA) {
		if (pd->rx_buf_len == 0 ||
		    osdp_millis_since(pd->tstamp) < OSDP_RESP_TOUT_MS) {
			return;
		}
		LOG_DBG("rx_buf: %d", pd->rx_buf_len);
		hexdump(pd->rx_buf, pd->rx_buf_len, "Buf");
	}

	if (ret != OSDP_PD_ERR_NONE && ret != OSDP_PD_ERR_REPLY) {
		LOG_ERR("CMD receive error/timeout - err:%d", ret);
		pd_error_reset(pd);
		return;
	}

	if (ret == OSDP_PD_ERR_NONE && sc_is_active(pd)) {
		pd->sc_tstamp = osdp_millis_now();
	}

	ret = pd_send_reply(pd);
	if (ret != OSDP_PD_ERR_NONE) {
		/**
		 * PD received and decoded a valid command from CP but failed to
		 * send the intended respone?? This should not happen but if it
		 * did, we cannot do anything about it, just complain about it
		 * and limp back home.
		 */
		LOG_ERR("REPLY send failed! CP may be waiting..");
		return;
	}

	if (TO_CTX(pd)->command_complete_callback) {
		TO_CTX(pd)->command_complete_callback(pd->cmd_id);
	}
}

static void osdp_pd_set_attributes(struct osdp_pd *pd, struct osdp_pd_cap *cap,
				   struct osdp_pd_id *id)
{
	int fc;

	while (cap && ((fc = cap->function_code) > 0)) {
		if (fc >= OSDP_PD_CAP_SENTINEL) {
			break;
		}
		pd->cap[fc].function_code = cap->function_code;
		pd->cap[fc].compliance_level = cap->compliance_level;
		pd->cap[fc].num_items = cap->num_items;
		cap++;
	}
	if (id != NULL) {
		memcpy(&pd->id, id, sizeof(struct osdp_pd_id));
	}
}

/* --- Exported Methods --- */

OSDP_EXPORT
osdp_t *osdp_pd_setup(osdp_pd_info_t *info)
{
	struct osdp_pd *pd;
	struct osdp_cp *cp;
	struct osdp *ctx;

	assert(info);

	osdp_log_ctx_set(info->address);

#ifndef CONFIG_OSDP_STATIC_PD
	ctx = calloc(1, sizeof(struct osdp));
	if (ctx == NULL) {
		LOG_ERR("Failed to allocate osdp context");
		return NULL;
	}

	ctx->cp = calloc(1, sizeof(struct osdp_cp));
	if (ctx->cp == NULL) {
		LOG_ERR("Failed to allocate osdp_cp context");
		goto error;
	}

	ctx->pd = calloc(1, sizeof(struct osdp_pd));
	if (ctx->pd == NULL) {
		LOG_ERR("Failed to allocate osdp_pd context");
		goto error;
	}
#else
	static struct osdp g_osdp_ctx;
	static struct osdp_cp g_osdp_cp_ctx;
	static struct osdp_pd g_osdp_pd_ctx;

	ctx = &g_osdp_ctx;
	ctx->cp = &g_osdp_cp_ctx;
	ctx->pd = &g_osdp_pd_ctx;
#endif

	ctx->magic = 0xDEADBEAF;
	cp = TO_CP(ctx);
	cp->__parent = ctx;
	cp->num_pd = 1;

	SET_CURRENT_PD(ctx, 0);
	pd = TO_PD(ctx, 0);

	pd->__parent = ctx;
	pd->offset = 0;
	pd->baud_rate = info->baud_rate;
	pd->address = info->address;
	pd->flags = info->flags;
	pd->seq_number = -1;
	memcpy(&pd->channel, &info->channel, sizeof(struct osdp_channel));

	if (pd_event_queue_init(pd)) {
		goto error;
	}

	if (info->scbk == NULL) {
		if (is_enforce_secure(pd)) {
			LOG_ERR("SCBK must be provided in ENFORCE_SECURE");
			goto error;
		}
		LOG_WRN("SCBK not provided. PD is in INSTALL_MODE");
		SET_FLAG(pd, OSDP_FLAG_INSTALL_MODE);
	} else {
		memcpy(pd->sc.scbk, info->scbk, 16);
	}
	SET_FLAG(pd, PD_FLAG_SC_CAPABLE);
	if (IS_ENABLED(CONFIG_OSDP_SKIP_MARK_BYTE)) {
		SET_FLAG(pd, PD_FLAG_PKT_SKIP_MARK);
	}
	osdp_pd_set_attributes(pd, info->cap, &info->id);
	osdp_pd_set_attributes(pd, osdp_pd_cap, NULL);

	SET_FLAG(pd, PD_FLAG_PD_MODE); /* used in checks in phy */

	LOG_INF("PD setup complete");
	return (osdp_t *)ctx;

error:
	osdp_pd_teardown((osdp_t *)ctx);
	return NULL;
}

OSDP_EXPORT
void osdp_pd_teardown(osdp_t *ctx)
{
	assert(ctx);

#ifndef CONFIG_OSDP_STATIC_PD
	safe_free(TO_PD(ctx, 0));
	safe_free(TO_CP(ctx));
	safe_free(ctx);
#endif
}

OSDP_EXPORT
void osdp_pd_refresh(osdp_t *ctx)
{
	input_check(ctx);
	struct osdp_pd *pd = GET_CURRENT_PD(ctx);

	osdp_log_ctx_set(pd->address);
	osdp_pd_update(pd);
}

OSDP_EXPORT
void osdp_pd_set_capabilities(osdp_t *ctx, struct osdp_pd_cap *cap)
{
	input_check(ctx);
	struct osdp_pd *pd = GET_CURRENT_PD(ctx);

	osdp_pd_set_attributes(pd, cap, NULL);
}

OSDP_EXPORT
void osdp_pd_set_command_callback(osdp_t *ctx, pd_commnand_callback_t cb,
				  void *arg)
{
	input_check(ctx);
	struct osdp_pd *pd = GET_CURRENT_PD(ctx);

	pd->command_callback_arg = arg;
	pd->command_callback = cb;
}

OSDP_EXPORT
int osdp_pd_notify_event(osdp_t *ctx, struct osdp_event *event)
{
	input_check(ctx);
	struct osdp_event *ev;
	struct osdp_pd *pd = GET_CURRENT_PD(ctx);

	ev = pd_event_alloc(pd);
	if (ev == NULL) {
		return -1;
	}

	memcpy(ev, event, sizeof(struct osdp_event));
	pd_event_enqueue(pd, ev);
	return 0;
}

#ifdef UNIT_TESTING

/**
 * Force export some private methods for testing.
 */
void (*test_osdp_pd_update)(struct osdp_pd *pd) = osdp_pd_update;
int (*test_pd_decode_packet)(struct osdp_pd *pd, int *len) = pd_decode_packet;

#endif /* UNIT_TESTING */
