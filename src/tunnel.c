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

static bool uwg_fec_reinject_en __read_mostly = true;
module_param_named(fec_reinject, uwg_fec_reinject_en, bool, 0644);
MODULE_PARM_DESC(fec_reinject, "reinject recovered packets (diagnostic)");

static bool uwg_fec_send_en __read_mostly = true;
module_param_named(fec_send, uwg_fec_send_en, bool, 0644);
MODULE_PARM_DESC(fec_send, "actually transmit FEC repairs (diagnostic)");

static bool uwg_fec_tx_en __read_mostly = true;
module_param_named(fec_tx, uwg_fec_tx_en, bool, 0644);
MODULE_PARM_DESC(fec_tx, "build and send FEC repairs");

static bool uwg_fec_rx_en __read_mostly = true;
module_param_named(fec_rx, uwg_fec_rx_en, bool, 0644);
MODULE_PARM_DESC(fec_rx, "record and recover FEC groups");

static bool uwg_fec_store_en __read_mostly = true;
module_param_named(fec_store, uwg_fec_store_en, bool, 0644);
MODULE_PARM_DESC(fec_store, "store payloads in the recovery ring (diagnostic)");

static u32 uwg_cipher_offset(const struct wg_peer *peer)
{
	return peer->device->transport_padding + sizeof(struct message_data);
}

static void uwg_xor(u8 *dst, const u8 *src, size_t len)
{
	size_t i;

	for (i = 0; i + sizeof(u64) <= len; i += sizeof(u64)) {
		u64 a = get_unaligned((u64 *)(dst + i));
		u64 b = get_unaligned((const u64 *)(src + i));

		put_unaligned(a ^ b, (u64 *)(dst + i));
	}
	for (; i < len; ++i)
		dst[i] ^= src[i];
}

static bool uwg_payload_copy(const struct sk_buff *skb, u32 off, u8 *dst,
			     u32 len)
{
	if (likely(!skb_is_nonlinear(skb))) {
		memcpy(dst, skb->data + off, len);
		return true;
	}

	return skb_copy_bits(skb, off, dst, len) == 0;
}

static void uwg_xor_skb(u8 *dst, const struct sk_buff *skb, u32 off, u32 len)
{
	u8 chunk[256];
	u32 done = 0;

	while (done < len) {
		u32 n = min_t(u32, len - done, sizeof(chunk));

		if (unlikely(skb_copy_bits(skb, off + done, chunk, n) != 0))
			return;
		uwg_xor(dst + done, chunk, n);
		done += n;
	}
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
	memset(PACKET_CB(skb), 0, sizeof(struct packet_cb));
	PACKET_CB(skb)->mtu = peer->device->dev->mtu;
	PACKET_CB(skb)->is_keepalive = false;
	return skb;
}

static void uwg_tunnel_send(struct wg_peer *peer, struct noise_keypair *keypair,
			    const u8 *buf, size_t len)
{
	struct sk_buff *skb;

	skb = uwg_tunnel_skb(peer, buf, len);
	if (unlikely(!skb))
		return;

	PACKET_CB(skb)->keypair = keypair;
	PACKET_CB(skb)->no_account = true;
	if (unlikely(!uwg_fec_send_en)) {
		kfree_skb(skb);
		return;
	}
	PACKET_CB(skb)->nonce = atomic64_inc_return(&keypair->sending_counter) - 1;
	if (unlikely(PACKET_CB(skb)->nonce >= REJECT_AFTER_MESSAGES)) {
		kfree_skb(skb);
		return;
	}
	if (unlikely(!encrypt_packet(skb, keypair))) {
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

static void uwg_fec_tx_reset(struct uwg_fec_tx *tx)
{
	tx->count = 0;
	tx->max_len = 0;
	memset(tx->parity[tx->cur], 0, UWG_FEC_MAX_BYTES);
}

bool uwg_tunnel_tx_data(struct wg_peer *peer, struct sk_buff *skb,
			struct noise_keypair *keypair)
{
	struct uwg_tunnel *t = &peer->tunnel;
	struct uwg_fec_tx *tx = &t->tx;
	u32 off = uwg_cipher_offset(peer);
	u32 clen;
	bool staged = false;

	if (unlikely(!READ_ONCE(t->fec_k)) || unlikely(!READ_ONCE(uwg_fec_tx_en)) ||
	    unlikely(!keypair) || unlikely(!tx->parity[0]))
		goto skip;
	if (unlikely(skb->len <= off))
		goto skip;

	clen = skb->len - off;
	if (unlikely(clen > UWG_FEC_MAX_BYTES || clen < UWG_FEC_MIN_BYTES))
		goto skip;

	spin_lock_bh(&tx->lock);
	atomic64_inc(&t->tx_calls);

	if (!tx->count)
		tx->key_idx = keypair->remote_index;
	tx->counters[tx->count] = PACKET_CB(skb)->nonce;
	tx->lens[tx->count] = clen;
	if (clen > tx->max_len) {
		memset(tx->parity[tx->cur] + tx->max_len, 0,
		       clen - tx->max_len);
		tx->max_len = clen;
	}
	if (likely(!skb_is_nonlinear(skb)))
		uwg_xor(tx->parity[tx->cur], skb->data + off, clen);
	else
		uwg_xor_skb(tx->parity[tx->cur], skb, off, clen);

	if (++tx->count < READ_ONCE(t->fec_k)) {
		spin_unlock_bh(&tx->lock);
		return false;
	}

	if (unlikely(tx->ready)) {
		atomic64_inc(&t->tx_dropped);
		uwg_fec_tx_reset(tx);
		spin_unlock_bh(&tx->lock);
		return false;
	}

	tx->staged_count = tx->count;
	tx->staged_max_len = tx->max_len;
	tx->staged_key_idx = tx->key_idx;
	tx->staged_block_id = tx->block_id;
	memcpy(tx->staged_counters, tx->counters,
	       sizeof(tx->staged_counters[0]) * tx->count);
	memcpy(tx->staged_lens, tx->lens, sizeof(tx->staged_lens[0]) * tx->count);
	tx->staged = tx->cur;
	tx->cur ^= 1;
	tx->ready = true;
	tx->block_id++;
	uwg_fec_tx_reset(tx);
	atomic64_inc(&t->tx_groups);
	staged = true;

	spin_unlock_bh(&tx->lock);
	return staged;

skip:
	atomic64_inc(&t->tx_skip);
	return false;
}

void uwg_tunnel_tx_flush(struct wg_peer *peer, struct noise_keypair *keypair)
{
	struct uwg_tunnel *t = &peer->tunnel;
	struct uwg_fec_tx *tx = &t->tx;
	unsigned int k = READ_ONCE(t->fec_k);
	size_t cap, pos = 0;
	u32 i, count, max_len, key_idx, block_id;
	u8 *buf;

	if (unlikely(!keypair) || unlikely(!k) || unlikely(!tx->parity[0]))
		return;

	cap = 8 + 10 * k + 6 + UWG_FEC_MAX_BYTES;
	buf = kmalloc(cap, GFP_ATOMIC);
	if (unlikely(!buf))
		return;

	spin_lock_bh(&tx->lock);
	if (unlikely(!tx->ready)) {
		spin_unlock_bh(&tx->lock);
		kfree(buf);
		return;
	}

	count = tx->staged_count;
	max_len = tx->staged_max_len;
	key_idx = tx->staged_key_idx;
	block_id = tx->staged_block_id;

	buf[pos++] = UWG_FRAME_MAGIC;
	buf[pos++] = UWG_FRAME_FEC_REPAIR;
	buf[pos++] = UWG_FEC_SCHEME_XOR;
	buf[pos++] = 0;
	put_unaligned_le16(block_id, buf + pos);
	pos += 2;
	buf[pos++] = count;
	buf[pos++] = 1;
	for (i = 0; i < count; ++i) {
		put_unaligned_le64(tx->staged_counters[i], buf + pos);
		pos += 8;
	}
	for (i = 0; i < count; ++i) {
		put_unaligned_le16(tx->staged_lens[i], buf + pos);
		pos += 2;
	}
	put_unaligned_le32(key_idx, buf + pos);
	pos += 4;
	put_unaligned_le16(max_len, buf + pos);
	pos += 2;
	memcpy(buf + pos, tx->parity[tx->staged], max_len);
	pos += max_len;
	tx->ready = false;

	spin_unlock_bh(&tx->lock);

	uwg_tunnel_send(peer, keypair, buf, pos);
	kfree(buf);
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

void uwg_tunnel_rx_forget(struct wg_peer *peer, u64 counter)
{
	struct uwg_fec_rx *rx = &peer->tunnel.rx;
	struct uwg_fec_slot *slot;

	if (unlikely(!rx->seen || !rx->slots))
		return;
	spin_lock_bh(&rx->lock);
	if (rx->started && rx->seen_max - counter < UWG_FEC_WINDOW)
		clear_bit(counter & (UWG_FEC_WINDOW - 1), rx->seen);
	slot = &rx->slots[counter & (UWG_FEC_RING - 1)];
	if (slot->used && slot->counter == counter)
		slot->used = false;
	spin_unlock_bh(&rx->lock);
}

void uwg_tunnel_rx_record(struct wg_peer *peer, const struct sk_buff *skb)
{
	struct uwg_tunnel *t = &peer->tunnel;
	struct uwg_fec_rx *rx = &t->rx;
	struct uwg_fec_slot *slot;
	u32 off = sizeof(struct message_data);
	u32 clen;
	u64 counter;

	if (unlikely(!t->fec_k || !rx->seen))
		return;
	if (unlikely(!uwg_fec_rx_en))
		return;
	if (unlikely(skb->len <= off))
		return;

	clen = skb->len - off;
	if (unlikely(clen > UWG_FEC_MAX_BYTES ||
		     clen < UWG_FEC_MIN_BYTES))
		return;

	counter = le64_to_cpu(((const struct message_data *)skb->data)->counter);
	atomic64_inc(&t->records);

	spin_lock_bh(&rx->lock);
	uwg_fec_mark(rx, counter);
	if (unlikely(!uwg_fec_store_en)) {
		spin_unlock_bh(&rx->lock);
		return;
	}
	if (unlikely(uwg_fec_slot_get(rx, counter))) {
		spin_unlock_bh(&rx->lock);
		return;
	}
	slot = &rx->slots[counter & (UWG_FEC_RING - 1)];
	if (unlikely(!slot->data)) {
		spin_unlock_bh(&rx->lock);
		return;
	}
	if (unlikely(!uwg_payload_copy(skb, off, slot->data, clen))) {
		spin_unlock_bh(&rx->lock);
		return;
	}
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

	skb = alloc_skb(sizeof(*hdr) + clen + SKB_HEADER_LEN, GFP_ATOMIC);
	if (unlikely(!skb))
		return;
	skb_reserve(skb, SKB_HEADER_LEN);
	hdr = skb_put(skb, sizeof(*hdr));
	hdr->header.type = cpu_to_le32(MESSAGE_DATA);
	hdr->key_idx = key_idx;
	hdr->counter = cpu_to_le64(counter);
	skb_put_data(skb, cipher, clen);
	skb->dev = peer->device->dev;
	PACKET_CB(skb)->recovered = true;

	wg_packet_consume_data(peer->device, skb, true);
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
	if (unlikely(!uwg_fec_rx_en))
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
		uwg_xor(cipher, slot->data, slot->len);
	}
	spin_unlock_bh(&rx->lock);

	atomic64_inc(&t->recovered);
	if (likely(uwg_fec_reinject_en))
		uwg_fec_reinject(peer, key_idx,
				 get_unaligned_le64(p + 8 + 8 * missing), cipher,
				 missing_len);
	kfree(cipher);
}

bool uwg_tunnel_probe(const struct sk_buff *skb)
{
	u8 head[2];

	if (skb->len < 2)
		return false;
	if (skb_copy_bits(skb, 0, head, sizeof(head)) < 0)
		return false;
	return head[0] == UWG_FRAME_MAGIC && head[1] == UWG_FRAME_FEC_REPAIR;
}

void uwg_tunnel_rx(struct wg_peer *peer, struct sk_buff *skb)
{
	if (unlikely(skb->len < 8))
		return;

	if (skb->data[1] != UWG_FRAME_FEC_REPAIR)
		return;

	atomic64_inc(&peer->tunnel.repairs_rx);
	if (unlikely(skb_is_nonlinear(skb)) && skb_linearize(skb))
		return;
	uwg_fec_rx_repair(peer, skb->data, skb->len);
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

	parity = kvcalloc(2, UWG_FEC_MAX_BYTES, GFP_KERNEL);
	if (!parity)
		return -ENOMEM;
	seen = bitmap_zalloc(UWG_FEC_WINDOW, GFP_KERNEL);
	if (!seen)
		goto err_parity;

	ring = UWG_FEC_RING;
	slotdata = kvcalloc(ring, UWG_FEC_MAX_BYTES, GFP_KERNEL);
	if (!slotdata)
		goto err_seen;
	slots = kcalloc(ring, sizeof(*slots), GFP_KERNEL);
	if (!slots)
		goto err_slotdata;

	uwg_tunnel_clear(peer);

	t->fec_k = k;
	t->fec_m = m;
	tx->parity[0] = parity;
	tx->parity[1] = parity + UWG_FEC_MAX_BYTES;
	spin_lock_init(&tx->lock);
	tx->cur = 0;
	tx->staged = 0;
	tx->count = 0;
	tx->staged_count = 0;
	tx->max_len = 0;
	tx->staged_max_len = 0;
	tx->block_id = 0;
	tx->staged_block_id = 0;
	tx->ready = false;
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
	kvfree(slotdata);
err_seen:
	bitmap_free(seen);
err_parity:
	kvfree(parity);
	return -ENOMEM;
}

void uwg_tunnel_purge(struct wg_peer *peer)
{
	struct uwg_tunnel *t = &peer->tunnel;
	struct uwg_fec_tx *tx = &t->tx;
	struct uwg_fec_rx *rx = &t->rx;
	unsigned int i;

	if (tx->parity[0]) {
		spin_lock_bh(&tx->lock);
		tx->ready = false;
		uwg_fec_tx_reset(tx);
		spin_unlock_bh(&tx->lock);
	}
	spin_lock_bh(&rx->lock);
	if (rx->seen) {
		bitmap_zero(rx->seen, UWG_FEC_WINDOW);
		rx->started = false;
	}
	for (i = 0; rx->slots && i < UWG_FEC_RING; ++i)
		rx->slots[i].used = false;
	spin_unlock_bh(&rx->lock);
}

void uwg_tunnel_clear(struct wg_peer *peer)
{
	struct uwg_tunnel *t = &peer->tunnel;
	struct uwg_fec_tx *tx = &t->tx;
	struct uwg_fec_rx *rx = &t->rx;
	unsigned int i;

	if (!t->fec_k && !tx->parity[0] && !rx->seen)
		return;

	t->fec_k = 0;
	t->fec_m = 0;
	kvfree(tx->parity[0]);
	tx->parity[0] = NULL;
	tx->parity[1] = NULL;
	tx->count = 0;
	tx->max_len = 0;
	tx->ready = false;
	bitmap_free(rx->seen);
	rx->seen = NULL;
	rx->started = false;
	rx->pos = 0;
	if (rx->slots) {
		kvfree(rx->slots[0].data);
		for (i = 0; i < UWG_FEC_RING; ++i)
			rx->slots[i].data = NULL;
		kfree(rx->slots);
		rx->slots = NULL;
	}
}
