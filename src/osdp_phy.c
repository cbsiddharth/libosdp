/*
 * Copyright (c) 2019 Siddharth Chandrasekaran
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include "osdp_common.h"

#define PKT_CONTROL_SQN			0x03
#define PKT_CONTROL_CRC			0x04
#define PKT_CONTROL_SCB			0x08

#define OSDP_PKT_HEADER_SIZE		(sizeof(struct osdp_packet_header))

struct osdp_packet_header {
	uint8_t mark;
	uint8_t som;
	uint8_t pd_address;
	uint8_t len_lsb;
	uint8_t len_msb;
	uint8_t control;
	uint8_t data[0];
};

uint8_t compute_checksum(uint8_t * msg, int length)
{
	uint8_t checksum = 0;
	int i, whole_checksum;

	whole_checksum = 0;
	for (i = 0; i < length; i++) {
		whole_checksum += msg[i];
		checksum = ~(0xff & whole_checksum) + 1;
	}
	return checksum;
}

const char *get_nac_reason(int code)
{
	const char *osdp_nak_reasons[] = {
		"",
		"NAK: Message check character(s) error (bad cksum/crc)",
		"NAK: Command length error",
		"NAK: Unknown Command Code. Command not implemented by PD",
		"NAK: Unexpected sequence number detected in the header",
		"NAK: This PD does not support the security block that was received",
		"NAK: Communication security conditions not met",
		"NAK: BIO_TYPE not supported",
		"NAK: BIO_FORMAT not supported",
		"NAK: Unable to process command record",
	};

	if (code < 0 || code >= OSDP_PD_NAK_SENTINEL)
		code = 0;

	return osdp_nak_reasons[code];
}

static int phy_get_seq_number(struct osdp_pd *p, int do_inc)
{
	/* p->seq_num is set to -1 to reset phy cmd state */
	if (do_inc) {
		p->seq_number += 1;
		if (p->seq_number > 3)
			p->seq_number = 1;
	}
	return p->seq_number & PKT_CONTROL_SQN;
}

uint8_t *phy_packet_get_data(struct osdp_pd *p, const uint8_t *buf)
{
	int sb_len = 0;
	struct osdp_packet_header *pkt;

	pkt = (struct osdp_packet_header *)buf;
	if (pkt->control & PKT_CONTROL_SCB)
		sb_len = pkt->data[0];

	return pkt->data + sb_len;
}

uint8_t *phy_packet_get_smb(struct osdp_pd *p, const uint8_t *buf)
{
	struct osdp_packet_header *pkt;

	pkt = (struct osdp_packet_header *)buf;
	if (pkt->control & PKT_CONTROL_SCB)
		return pkt->data;

	return NULL;
}

int phy_in_sc_handshake(int is_reply, int id)
{
	if (is_reply)
		return (id == REPLY_CCRYPT || id == REPLY_RMAC_I);
	else
		return (id == CMD_CHLNG || id == CMD_SCRYPT);
}

int phy_build_packet_head(struct osdp_pd *p, int id, uint8_t * buf, int maxlen)
{
	int exp_len, pd_mode, sb_len;
	struct osdp_packet_header *pkt;

	pd_mode = isset_flag(p, PD_FLAG_PD_MODE);
	exp_len = sizeof(struct osdp_packet_header);
	if (maxlen < exp_len) {
		osdp_log(LOG_NOTICE, "pkt_buf len err - %d/%d", maxlen,
			 exp_len);
		return -1;
	}

	/* Fill packet header */
	pkt = (struct osdp_packet_header *)buf;
	pkt->mark = 0xFF;
	pkt->som = 0x53;
	pkt->pd_address = p->address & 0x7F;	/* Use only the lower 7 bits */
	if (pd_mode)
		pkt->pd_address |= 0x80;
	pkt->control = phy_get_seq_number(p, !isset_flag(p, PD_FLAG_PD_MODE));
	pkt->control |= PKT_CONTROL_CRC;

	sb_len = 0;
	if (isset_flag(p, PD_FLAG_SC_ACTIVE)) {
		pkt->control |= PKT_CONTROL_SCB;
		pkt->data[0] = sb_len = 2;
	} else if (phy_in_sc_handshake(pd_mode, id)) {
		pkt->control |= PKT_CONTROL_SCB;
		pkt->data[0] = sb_len = 3;
	}

	return sizeof(struct osdp_packet_header) + sb_len;
}

int phy_build_packet_tail(struct osdp_pd *p, uint8_t * buf, int len, int maxlen)
{
	int i, is_cmd, data_len;
	uint8_t *data;
	uint16_t crc16;
	struct osdp_packet_header *pkt;

	/* expect head to be prefilled */
	if (buf[0] != 0xFF || buf[1] != 0x53)
		return -1;

	is_cmd = !isset_flag(p, PD_FLAG_PD_MODE);
	pkt = (struct osdp_packet_header *)buf;
	if (pkt->control & PKT_CONTROL_SCB) {
		if (pkt->data[1] == SCS_17) {
			data = pkt->data + 2 + 1;
			data_len = len - OSDP_PKT_HEADER_SIZE - 2 - 1;
			len -= data_len;
			data_len = osdp_encrypt_data(p, data, data_len);
			len += data_len;
		}
		pkt->len_lsb = byte_0(len - 1 + 2 + 4);
		pkt->len_lsb = byte_0(len - 1 + 2 + 4);
		osdp_compute_mac(p, is_cmd, buf + 1, len - 1);

		/* extend the buf with 4 MAC bytes */
		for (i=0; i<4; i++)
			buf[len + i] = p->sc.c_mac[i];
		len += 4;
	} else {
		/* fill packet length into header w/ 2b crc; wo/ 1b mark */
		pkt->len_lsb = byte_0(len - 1 + 2);
		pkt->len_msb = byte_1(len - 1 + 2);
	}

	/* fill crc16 */
	crc16 = crc16_itu_t(0x1D0F, buf + 1, len - 1);	/* excluding mark byte */
	buf[len + 0] = byte_0(crc16);
	buf[len + 1] = byte_1(crc16);
	len += 2;

	return len;
}

int phy_check_packet(const uint8_t * buf, int len)
{
	int pkt_len;
	struct osdp_packet_header *pkt;

	pkt = (struct osdp_packet_header *)buf;

	/* validate packet header */
	if (pkt->mark != 0xFF || pkt->som != 0x53) {
		return -1;
	}
	pkt_len = (pkt->len_msb << 8) | pkt->len_lsb;
	if (pkt_len != len - 1) {
		osdp_log(LOG_ERR, "packet length mismatch %d/%d", pkt_len,
			 len - 1);
		return 1;
	}
	return 0;
}

int phy_decode_packet(struct osdp_pd *p, uint8_t * buf, int blen)
{
	int pkt_len, pd_mode;
	uint16_t comp, cur;
	struct osdp_packet_header *pkt;

	pd_mode = isset_flag(p, PD_FLAG_PD_MODE);
	pkt = (struct osdp_packet_header *)buf;

	/* validate packet header */
	if (!pd_mode && !(pkt->pd_address & 0x80)) {
		osdp_log(LOG_ERR, "reply without MSB set 0x%02x",
			 pkt->pd_address);
		return -1;
	}
	if ((pkt->pd_address & 0x7F) != (p->address & 0x7F)) {
		osdp_log(LOG_ERR, "invalid pd address %d",
			 (pkt->pd_address & 0x7F));
		return -1;
	}
	pkt_len = (pkt->len_msb << 8) | pkt->len_lsb;
	if (pkt_len != blen - 1) {
		osdp_log(LOG_ERR, "packet length mismatch %d/%d", pkt_len,
			 blen - 1);
		return -1;
	}
	cur = pkt->control & PKT_CONTROL_SQN;
	comp = phy_get_seq_number(p, pd_mode);
	if (comp != cur && !isset_flag(p, PD_FLAG_SKIP_SEQ_CHECK)) {
		osdp_log(LOG_ERR, "packet seq mismatch %d/%d", comp, cur);
		return -1;
	}
	blen -= sizeof(struct osdp_packet_header);	/* consume header */

	/* validate CRC/checksum */
	if (pkt->control & PKT_CONTROL_CRC) {
		cur = (buf[pkt_len] << 8) | buf[pkt_len - 1];
		blen -= 2;	/* consume 2byte CRC */
		comp = crc16_itu_t(0x1D0F, buf + 1, pkt_len - 2);
		if (comp != cur) {
			osdp_log(LOG_ERR, "invalid crc 0x%04x/0x%04x", comp, cur);
			return -1;
		}
	} else {
		cur = buf[blen - 1];
		blen -= 1;	/* consume 1byte checksum */
		comp = compute_checksum(buf + 1, pkt_len - 1);
		if (comp != cur) {
			osdp_log(LOG_ERR, "invalid checksum 0x%02x/0x%02x",
				 comp, cur);
			return -1;
		}
	}

	/* copy decoded message block into cmd buf */
	memcpy(buf, pkt->data, blen);
	return blen;
}

void phy_state_reset(struct osdp_pd *pd)
{
	pd->phy_state = 0;
	pd->seq_number = -1;
}
