// SPDX-License-Identifier: GPL-2.0
#include "cc.h"
#include "peer.h"

#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/string.h>

/*
 * Brutal is a fixed-rate pacer with loss compensation, the same model used by
 * Hysteria 2: the configured rate is the goodput target, and the pacer is
 * allowed to exceed it by the measured packet loss ratio so that the receiver
 * still gets the configured bandwidth on a lossy path.
 */
static u32 uwg_brutal_ack_pct(const struct uwg_cc_brutal *br, u64 now)
{
	u64 ts = div_u64(now, NSEC_PER_SEC);
	u64 acked = 0, lost = 0;
	unsigned int i;

	for (i = 0; i < UWG_BRUTAL_SLOTS; ++i) {
		if (!br->slots_ts[i] || ts < br->slots_ts[i] ||
		    ts - br->slots_ts[i] >= UWG_BRUTAL_SLOTS)
			continue;
		acked += br->slots_acked[i];
		lost += br->slots_lost[i];
	}

	if (acked + lost < UWG_BRUTAL_MIN_SAMPLES)
		return 100;

	return (u32)clamp_t(u64, div_u64(acked * 100, acked + lost),
			    UWG_BRUTAL_MIN_ACK_PCT, 100);
}

void uwg_cc_brutal_reset(struct uwg_cc *cc, u64 base_bps)
{
	struct uwg_cc_brutal *br = &cc->brutal;

	memset(br, 0, sizeof(*br));
	br->base_bps = base_bps;
	br->pace_bps = base_bps;
	br->ack_pct = 100;
}

void uwg_cc_brutal_tick(struct uwg_cc *cc, u64 now)
{
	struct uwg_cc_brutal *br = &cc->brutal;
	u32 pct = uwg_brutal_ack_pct(br, now);

	br->ack_pct = pct;
	br->pace_bps = mul_u64_u64_div_u64(br->base_bps, 100, pct);
}

void uwg_cc_brutal_on_feedback(struct wg_peer *peer, u64 rtt_ns,
			       u64 recv_packets, u64 recv_highest, u64 echo_drops,
			       u64 now)
{
	struct uwg_cc *cc = &peer->cc;
	struct uwg_cc_brutal *br = &cc->brutal;
	u64 ts = div_u64(now, NSEC_PER_SEC);
	u64 acked, lost;
	u32 idx;

	(void)rtt_ns;

	spin_lock_bh(&cc->lock);
	if (READ_ONCE(cc->mode) != UWG_CC_BRUTAL)
		goto out;

	uwg_cc_loss_update(cc, recv_highest, recv_packets, echo_drops,
			   &acked, &lost);

	idx = (u32)(ts % UWG_BRUTAL_SLOTS);
	if (br->slots_ts[idx] != ts) {
		br->slots_acked[idx] = 0;
		br->slots_lost[idx] = 0;
		br->slots_ts[idx] = ts;
	}
	br->slots_acked[idx] += (u32)min_t(u64, acked, U32_MAX);
	br->slots_lost[idx] += (u32)min_t(u64, lost, U32_MAX);

	uwg_cc_brutal_tick(cc, now);
out:
	spin_unlock_bh(&cc->lock);
}
