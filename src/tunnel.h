/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UWG_TUNNEL_H
#define _UWG_TUNNEL_H

#include "frame.h"

#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct wg_peer;
struct noise_keypair;

#define UWG_FEC_MAX_K		16
#define UWG_FEC_MAX_BYTES	2048
#define UWG_FEC_MIN_BYTES	256
#define UWG_FEC_RING		256
#define UWG_FEC_WINDOW		4096
#define UWG_FEC_SCHEME_XOR	0

struct uwg_fec_tx {
	u8 *parity[2];
	u64 counters[UWG_FEC_MAX_K];
	u64 staged_counters[UWG_FEC_MAX_K];
	u16 lens[UWG_FEC_MAX_K];
	u16 staged_lens[UWG_FEC_MAX_K];
	u32 key_idx;
	u32 staged_key_idx;
	u16 max_len;
	u16 staged_max_len;
	u16 block_id;
	u16 staged_block_id;
	u8 cur;
	u8 staged;
	u8 count;
	u8 staged_count;
	bool ready;
	spinlock_t lock;
};

struct uwg_fec_slot {
	u64 counter;
	u16 len;
	bool used;
	u8 *data;
};

struct uwg_fec_rx {
	unsigned long *seen;
	u64 seen_max;
	struct uwg_fec_slot *slots;
	spinlock_t lock;
	u16 pos;
	bool started;
};

struct uwg_tunnel {
	u8 fec_k;
	u8 fec_m;
	u8 fec_k_cfg;
	u8 fec_m_cfg;
	bool fec_active;
	u64 last_loss_ts;
	struct uwg_fec_tx tx;
	struct uwg_fec_rx rx;
	atomic64_t recovered;
	atomic64_t recover_fail;
	atomic64_t repairs_sent;
	atomic64_t repairs_rx;
	atomic64_t records;
	atomic64_t tx_calls;
	atomic64_t tx_skip;
	atomic64_t tx_groups;
	atomic64_t tx_dropped;
	atomic64_t reinject_decrypt_fail;
	atomic64_t reinject_replay_fail;
	atomic64_t reinject_delivered;
};

bool uwg_tunnel_probe(const struct sk_buff *skb);
void uwg_tunnel_rx(struct wg_peer *peer, struct sk_buff *skb);
void uwg_tunnel_rx_record(struct wg_peer *peer, const struct sk_buff *skb);
void uwg_tunnel_rx_forget(struct wg_peer *peer, u64 counter);
bool uwg_tunnel_tx_data(struct wg_peer *peer, struct sk_buff *skb,
			struct noise_keypair *keypair);
void uwg_tunnel_tx_flush(struct wg_peer *peer, struct noise_keypair *keypair);
int uwg_fec_set(struct wg_peer *peer, unsigned int k, unsigned int m);
void uwg_tunnel_purge(struct wg_peer *peer);
void uwg_tunnel_clear(struct wg_peer *peer);

#endif /* _UWG_TUNNEL_H */
