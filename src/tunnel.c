// SPDX-License-Identifier: GPL-2.0
#include "tunnel.h"
#include "cc.h"
#include "device.h"
#include "messages.h"
#include "noise.h"
#include "peer.h"
#include "queueing.h"
#include "socket.h"

#include <linux/bitmap.h>
#include <linux/moduleparam.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

static unsigned int uwg_fec_stage __read_mostly = 3;
module_param_named(fec_stage, uwg_fec_stage, uint, 0644);
MODULE_PARM_DESC(fec_stage, "0=collect only, 1=build only, 2=encrypt only, 3=full");

static unsigned int uwg_fec_rx_en __read_mostly = 1;
module_param_named(fec_rx, uwg_fec_rx_en, uint, 0644);

static u32 uwg_cipher_offset(const struct wg_peer *peer)
{
	return peer->device->transport_padding + sizeof(struct message_data);
}

static struct sk_buff *uwg_tunnel_skb(struct wg_peer *peer, const u8 *buf,
				      size_t len)
{
	struct sk_buff *skb;

	skb = alloc_skb(len + DATA_PACKET_HEAD_ROOM, GFP_ATOMIC);
	if (unlikely(!skb))
		return NULL;

	skb_reserve(skb, DATA_PACKET_HEAD_ROOM);
	skb_put_data(skb, buf, len);
	skb->dev = peer->device->dev;
	PACKET_CB(skb)->mtu = peer->device->dev->mtu;
	PACKET_CB(skb)->is_keepalive = false;
	return skb;
}

static void uwg_tunnel_send(struct wg_peer *peer, struct noise_keypair *keypair,
			    const u8 *buf, size_t len)
{
	struct sk_buff *skb;

	if (uwg_fec_stage < 2)
		return;

	skb = uwg_tunnel_skb(peer, buf, len);
	if (unlikely(!skb))
		return;

	PACKET_CB(skb)->keypair = keypair;
	PACKET_CB(skb)->nonce = atomic64_inc_return(&keypair->sending_counter) - 1;
	if (unlikely(PACKET_CB(skb)->nonce >= REJECT_AFTER_MESSAGES)) {
		kfree_skb(skb);
		return;
	}
	if (unlikely(!encrypt_packet(skb, keypair))) {
		kfree_skb(skb);
		return;
	}
	if (uwg_fec_stage < 3) {
		kfree_skb(skb);
		return;
	}

	if (static_branch_unlikely(&uwg_cc_active) &&
	    unlikely(READ_ONCE(peer->cc.mode) != UWG_CC_NONE))
		uwg_cc_tx(peer, skb);
	else
		wg_socket_send_skb_to_peer(peer, skb, 0);
	atomic64_inc(&peer->tunnel.repairs_sent);
}

void uwg_tunnel_tx_data(struct wg_peer *peer, struct sk_buff *skb,
			struct noise_keypair *keypair)
{
	struct uwg_tunnel *t = &peer->tunnel;
	struct uwg_fec_tx *tx = &t->tx;
	u32 off = uwg_cipher_offset(peer);
	const u8 *cipher;
	u32 clen, i, k;
	size_t hdr, pos = 0;
	u8 *buf;

	if (unlikely(!t->fec_k) || unlikely(!tx->parity) || unlikely(!keypair))
		return;
	if (unlikely(skb->len <= off))
		return;

	cipher = skb->data + off;
	clen = skb->len - off;
	if (unlikely(clen > UWG_FEC_MAX_BYTES))
		return;

	if (!tx->count)
		tx->key_idx = keypair->remote_index;

	tx->counters[tx->count] = PACKET_CB(skb)->nonce;
	tx->lens[tx->count] = clen;
	if (clen > tx->max_len) {
		memset(tx->parity + tx->max_len, 0, clen - tx->max_len);
		tx->max_len = clen;
	}
	for (i = 0; i < clen; ++i)
		tx->parity[i] ^= cipher[i];
	tx->count++;

	if (tx->count < t->fec_k)
		return;

	k = tx->count;
	hdr = 8 + 8 * k + 2 * k + 4 + 2;
	buf = kmalloc(hdr + tx->max_len, GFP_ATOMIC);
	if (likely(buf) && uwg_fec_stage >= 1) {
		buf[pos++] = UWG_TUNNEL_MAGIC;
		buf[pos++] = UWG_TK_FEC_REPAIR;
		buf[pos++] = UWG_FEC_SCHEME_XOR;
		buf[pos++] = 0;
		put_unaligned_le16(tx->block_id, buf + pos);
		pos += 2;
		buf[pos++] = k;
		buf[pos++] = 1;
		for (i = 0; i < k; ++i) {
			put_unaligned_le64(tx->counters[i], buf + pos);
			pos += 8;
		}
		for (i = 0; i < k; ++i) {
			put_unaligned_le16(tx->lens[i], buf + pos);
			pos += 2;
		}
		put_unaligned_le32(tx->key_idx, buf + pos);
		pos += 4;
		put_unaligned_le16(tx->max_len, buf + pos);
		pos += 2;
		memcpy(buf + pos, tx->parity, tx->max_len);
		pos += tx->max_len;

		uwg_tunnel_send(peer, keypair, buf, pos);
	}
	kfree(buf);

	tx->block_id++;
	tx->count = 0;
	if (tx->max_len)
		memset(tx->parity, 0, tx->max_len);
	tx->max_len = 0;
}

static bool uwg_fec_seen(const struct uwg_fec_rx *rx, u64 counter)
{
	if (counter > rx->seen_max)
		return false;
	if (rx->seen_max - counter >= UWG_FEC_WINDOW)
		return true;
	return test_bit(counter & (UWG_FEC_WINDOW - 1), rx->seen);
}

static void uwg_fec_mark(struct uwg_fec_rx *rx, u64 counter)
{
	u64 c;

	if (!rx->started) {
		bitmap_zero(rx->seen, UWG_FEC_WINDOW);
		rx->seen_max = counter;
		rx->started = true;
	}

	if (counter > rx->seen_max) {
		if (counter - rx->seen_max >= UWG_FEC_WINDOW)
			bitmap_zero(rx->seen, UWG_FEC_WINDOW);
		else
			for (c = rx->seen_max + 1; c <= counter; ++c)
				clear_bit(c & (UWG_FEC_WINDOW - 1), rx->seen);
		rx->seen_max = counter;
	}
	set_bit(counter & (UWG_FEC_WINDOW - 1), rx->seen);
}

static struct uwg_fec_slot *uwg_fec_slot_get(struct uwg_fec_rx *rx,
					     u64 counter)
{
	struct uwg_fec_slot *slot = &rx->slots[counter & (UWG_FEC_RING - 1)];

	return slot->used && slot->counter == counter ? slot : NULL;
}

void uwg_tunnel_rx_record(struct wg_peer *peer, const struct sk_buff *skb)
{
	struct uwg_tunnel *t = &peer->tunnel;
	struct uwg_fec_rx *rx = &t->rx;
	struct uwg_fec_slot *slot;
	u32 off = sizeof(struct message_data);
	u32 clen;
	u64 counter;

	if (unlikely(!uwg_fec_rx_en || !t->fec_k || !rx->seen))
		return;
	if (unlikely(skb->len <= off))
		return;

	clen = skb->len - off;
	if (unlikely(clen > UWG_FEC_MAX_BYTES))
		return;

	counter = ((const struct message_data *)skb->data)->counter;
	atomic64_inc(&t->records);

	spin_lock_bh(&rx->lock);
	uwg_fec_mark(rx, counter);
	if (unlikely(uwg_fec_slot_get(rx, counter))) {
		spin_unlock_bh(&rx->lock);
		return;
	}
	slot = &rx->slots[counter & (UWG_FEC_RING - 1)];
	if (unlikely(!slot->data)) {
		spin_unlock_bh(&rx->lock);
		return;
	}
	memcpy(slot->data, skb->data + off, clen);
	slot->counter = counter;
	slot->len = clen;
	slot->used = true;
	spin_unlock_bh(&rx->lock);
}

static void uwg_fec_reinject(struct wg_peer *peer, u32 key_idx, u64 counter,
			     const u8 *cipher, u32 clen)
{
	struct sk_buff *skb;
	struct message_data *hdr;
	u8 *buf;
	size_t len = sizeof(*hdr) + clen;

	buf = kmalloc(len, GFP_ATOMIC);
	if (unlikely(!buf))
		return;

	hdr = (struct message_data *)buf;
	hdr->header.type = cpu_to_le32(MESSAGE_DATA);
	hdr->key_idx = key_idx;
	hdr->counter = cpu_to_le64(counter);
	memcpy(buf + sizeof(*hdr), cipher, clen);

	skb = alloc_skb(len + SKB_HEADER_LEN, GFP_ATOMIC);
	if (unlikely(!skb)) {
		kfree(buf);
		return;
	}
	skb_reserve(skb, SKB_HEADER_LEN);
	skb_put_data(skb, buf, len);
	kfree(buf);
	skb->dev = peer->device->dev;
	PACKET_CB(skb)->recovered = true;

	wg_packet_consume_data(peer->device, skb);
}

static void uwg_fec_rx_repair(struct wg_peer *peer, const u8 *p, u32 len)
{
	struct uwg_tunnel *t = &peer->tunnel;
	struct uwg_fec_rx *rx = &t->rx;
	u32 key_idx, max_len, off, lens_off, missing_len;
	u8 scheme, k, m;
	int missing = -1;
	unsigned int i;
	u8 *cipher;

	if (unlikely(!t->fec_k) || unlikely(len < 18))
		return;

	scheme = p[2];
	k = p[6];
	m = p[7];
	if (scheme != UWG_FEC_SCHEME_XOR || k < 2 || k > UWG_FEC_MAX_K || m != 1)
		return;

	off = 8 + 8 * k;
	if (len < off + 2 * k + 4 + 2)
		return;
	lens_off = off;
	off += 2 * k;
	key_idx = get_unaligned_le32(p + off);
	off += 4;
	max_len = get_unaligned_le16(p + off);
	off += 2;
	if (len < off + max_len || max_len > UWG_FEC_MAX_BYTES)
		return;

	spin_lock_bh(&rx->lock);
	for (i = 0; i < k; ++i) {
		u64 counter = get_unaligned_le64(p + 8 + 8 * i);

		if (uwg_fec_seen(rx, counter))
			continue;
		if (missing >= 0) {
			spin_unlock_bh(&rx->lock);
			return;
		}
		missing = i;
	}

	if (missing < 0) {
		spin_unlock_bh(&rx->lock);
		atomic64_inc(&t->recover_fail);
		return;
	}

	missing_len = get_unaligned_le16(p + lens_off + 2 * missing);
	if (missing_len > max_len) {
		spin_unlock_bh(&rx->lock);
		return;
	}

	cipher = kmalloc(max_len, GFP_ATOMIC);
	if (unlikely(!cipher)) {
		spin_unlock_bh(&rx->lock);
		return;
	}
	memcpy(cipher, p + off, max_len);

	for (i = 0; i < k; ++i) {
		struct uwg_fec_slot *slot;
		u32 j;
		u64 counter;

		if (i == missing)
			continue;
		counter = get_unaligned_le64(p + 8 + 8 * i);
		slot = uwg_fec_slot_get(rx, counter);
		if (unlikely(!slot)) {
			spin_unlock_bh(&rx->lock);
			kfree(cipher);
			return;
		}
		for (j = 0; j < slot->len; ++j)
			cipher[j] ^= slot->data[j];
	}
	spin_unlock_bh(&rx->lock);

	atomic64_inc(&t->recovered);
	uwg_fec_reinject(peer, key_idx,
			 get_unaligned_le64(p + 8 + 8 * missing), cipher,
			 missing_len);
	kfree(cipher);
}

bool uwg_tunnel_probe(const struct sk_buff *skb)
{
	u8 head[2];

	if (skb_copy_bits(skb, 0, head, sizeof(head)) < 0)
		return false;
	if (head[0] != UWG_TUNNEL_MAGIC)
		return false;
	return head[1] == UWG_TK_FEC_REPAIR || head[1] == UWG_TK_CC_FEEDBACK ||
	       head[1] == UWG_TK_CC_REQUEST;
}

void uwg_tunnel_rx(struct wg_peer *peer, struct sk_buff *skb)
{
	u8 head[4];
	u8 *p;
	u32 len = skb->len;

	if (!uwg_fec_rx_en)
		return;
	if (skb_copy_bits(skb, 0, head, sizeof(head)) < 0 || len < 8)
		return;

	switch (head[1]) {
	case UWG_TK_FEC_REPAIR:
		p = kmalloc(len, GFP_ATOMIC);
		if (p && skb_copy_bits(skb, 0, p, len) == 0) {
			atomic64_inc(&peer->tunnel.repairs_rx);
			uwg_fec_rx_repair(peer, p, len);
		}
		kfree(p);
		break;
	default:
		break;
	}
}

int uwg_fec_set(struct wg_peer *peer, unsigned int k, unsigned int m)
{
	struct uwg_tunnel *t = &peer->tunnel;
	struct uwg_fec_tx *tx = &t->tx;
	struct uwg_fec_rx *rx = &t->rx;
	struct uwg_fec_slot *slots = NULL;
	u8 *parity, *slotdata = NULL;
	unsigned long *seen;
	unsigned int i, ring;

	if (k == 0 || m == 0) {
		uwg_tunnel_clear(peer);
		return 0;
	}
	if (k < 2 || k > UWG_FEC_MAX_K || m != 1)
		return -EINVAL;

	parity = kzalloc(UWG_FEC_MAX_BYTES, GFP_KERNEL);
	if (!parity)
		return -ENOMEM;
	seen = bitmap_zalloc(UWG_FEC_WINDOW, GFP_KERNEL);
	if (!seen)
		goto err_parity;

	ring = UWG_FEC_RING;
	slotdata = kcalloc(ring * UWG_FEC_MAX_BYTES, 1, GFP_KERNEL);
	if (!slotdata)
		goto err_seen;
	slots = kcalloc(ring, sizeof(*slots), GFP_KERNEL);
	if (!slots)
		goto err_slotdata;

	uwg_tunnel_clear(peer);

	t->fec_k = k;
	t->fec_m = m;
	tx->parity = parity;
	tx->count = 0;
	tx->max_len = 0;
	tx->block_id = 0;
	rx->seen = seen;
	rx->slots = slots;
	spin_lock_init(&rx->lock);
	rx->started = false;
	rx->pos = 0;
	for (i = 0; i < ring; ++i) {
		rx->slots[i].data = slotdata + (size_t)i * UWG_FEC_MAX_BYTES;
		rx->slots[i].used = false;
		rx->slots[i].len = 0;
		rx->slots[i].counter = 0;
	}
	return 0;

err_slotdata:
	kfree(slotdata);
err_seen:
	bitmap_free(seen);
err_parity:
	kfree(parity);
	return -ENOMEM;
}

void uwg_tunnel_clear(struct wg_peer *peer)
{
	struct uwg_tunnel *t = &peer->tunnel;
	struct uwg_fec_tx *tx = &t->tx;
	struct uwg_fec_rx *rx = &t->rx;
	unsigned int i;

	if (!t->fec_k && !tx->parity && !rx->seen)
		return;

	t->fec_k = 0;
	t->fec_m = 0;
	kfree(tx->parity);
	tx->parity = NULL;
	tx->count = 0;
	tx->max_len = 0;
	bitmap_free(rx->seen);
	rx->seen = NULL;
	rx->started = false;
	rx->pos = 0;
	if (rx->slots) {
		kfree(rx->slots[0].data);
		for (i = 0; i < UWG_FEC_RING; ++i)
			rx->slots[i].data = NULL;
		kfree(rx->slots);
		rx->slots = NULL;
	}
}
