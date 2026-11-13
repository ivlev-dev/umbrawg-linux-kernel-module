// SPDX-License-Identifier: GPL-2.0
#include "cc.h"
#include "peer.h"

#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/string.h>

static const u8 uwg_bbr_gains[8] = { 5, 4, 4, 4, 4, 4, 4, 4 };

const char *uwg_cc_phase_name(enum uwg_cc_phase phase)
{
	switch (phase) {
	case UWG_BBR_STARTUP:
		return "startup";
	case UWG_BBR_DRAIN:
		return "drain";
	case UWG_BBR_PROBE_BW:
		return "probe_bw";
	case UWG_BBR_PROBE_RTT:
		return "probe_rtt";
	default:
		return "unknown";
	}
}

static void uwg_bbr_expire_window(struct uwg_cc_bbr *b, u64 now)
{
	u64 bucket = div_u64(now, UWG_BBR_BW_SLOT_NS);
	u64 diff;
	unsigned int i;

	if (bucket == b->bw_bucket)
		return;

	diff = bucket - b->bw_bucket;
	if (diff >= UWG_BBR_BW_SLOTS) {
		memset(b->bw_slots, 0, sizeof(b->bw_slots));
	} else {
		for (i = 1; i <= diff; ++i)
			b->bw_slots[(b->bw_bucket + i) &
				    (UWG_BBR_BW_SLOTS - 1)] = 0;
	}
	b->bw_bucket = bucket;
}

static u64 uwg_bbr_window_max(struct uwg_cc_bbr *b, u64 now)
{
	u64 max_bw = 0;
	unsigned int i;

	uwg_bbr_expire_window(b, now);
	for (i = 0; i < UWG_BBR_BW_SLOTS; ++i)
		max_bw = max(max_bw, b->bw_slots[i]);
	return max_bw;
}

static void uwg_bbr_note_bw(struct uwg_cc_bbr *b, u64 rate, u64 now)
{
	unsigned int idx;

	uwg_bbr_expire_window(b, now);
	idx = div_u64(now, UWG_BBR_BW_SLOT_NS) & (UWG_BBR_BW_SLOTS - 1);
	if (rate > b->bw_slots[idx])
		b->bw_slots[idx] = rate;
}

static u64 uwg_bbr_rate_sample(struct uwg_cc_bbr *b, u64 now, u64 cur_bytes)
{
	u32 i, idx = 0, oldest;
	bool found = false;

	if (b->sample_count < 2)
		return 0;

	oldest = (b->sample_head + UWG_BBR_SAMPLES - b->sample_count) &
		 (UWG_BBR_SAMPLES - 1);
	for (i = 0; i < b->sample_count; ++i) {
		idx = (oldest + i) & (UWG_BBR_SAMPLES - 1);
		if (now - b->samples[idx].ts >= UWG_BBR_SAMPLE_MIN_NS) {
			found = true;
			break;
		}
	}
	if (!found)
		idx = oldest;

	if (now <= b->samples[idx].ts)
		return 0;
	if (cur_bytes < b->samples[idx].bytes) {
		b->sample_count = 0;
		return 0;
	}

	return mul_u64_u64_div_u64(cur_bytes - b->samples[idx].bytes,
				   8 * NSEC_PER_SEC, now - b->samples[idx].ts);
}

static void uwg_bbr_push_sample(struct uwg_cc_bbr *b, u64 bytes, u64 now)
{
	b->samples[b->sample_head].bytes = bytes;
	b->samples[b->sample_head].ts = now;
	b->sample_head = (b->sample_head + 1) & (UWG_BBR_SAMPLES - 1);
	if (b->sample_count < UWG_BBR_SAMPLES)
		b->sample_count++;
}

static void uwg_bbr_note_loss(struct uwg_cc_bbr *b, u64 tx_packets,
			      u64 rx_packets)
{
	u64 d_tx, d_rx, lost;

	if (tx_packets < b->prev_tx || rx_packets < b->prev_rx) {
		b->prev_tx = tx_packets;
		b->prev_rx = rx_packets;
		b->loss_pct = 0;
		return;
	}

	d_tx = tx_packets - b->prev_tx;
	d_rx = rx_packets - b->prev_rx;
	b->prev_tx = tx_packets;
	b->prev_rx = rx_packets;
	if (!d_tx) {
		b->loss_pct = 0;
		return;
	}
	lost = d_tx > d_rx ? d_tx - d_rx : 0;
	b->loss_pct = (u32)min_t(u64, div_u64(lost * 100, d_tx), 100);
}

void uwg_cc_bbr_reset(struct uwg_cc *cc, u64 start_bps)
{
	struct uwg_cc_bbr *b = &cc->bbr;
	u64 now = ktime_get_ns();

	memset(b, 0, sizeof(*b));
	start_bps = clamp_t(u64, start_bps, UWG_BBR_MIN_RATE_BPS,
			    UWG_BBR_MAX_RATE_BPS);
	b->phase = UWG_BBR_STARTUP;
	b->pacing_bps = start_bps;
	b->full_bw_bps = start_bps;
	b->safe_bw_bps = 0;
	b->bw_bucket = div_u64(now, UWG_BBR_BW_SLOT_NS);
	b->min_rtt_ts = now;
	b->last_fb_ts = now;
	b->cycle_ts = now;
	b->startup_ts = now;
	b->bw_ts = now;
}

void uwg_cc_bbr_tick(struct uwg_cc *cc, u64 now)
{
	struct uwg_cc_bbr *b = &cc->bbr;
	u64 rtt = max_t(u64, b->min_rtt_ns, 1000);
	u64 pacing = b->pacing_bps;

	b->bw_max_bps = uwg_bbr_window_max(b, now);

	switch (b->phase) {
	case UWG_BBR_STARTUP: {
		bool saturated = false;

		if (now - b->cycle_ts < UWG_BBR_STARTUP_STEP_NS)
			break;

		b->cycle_ts = now;
		if (b->min_rtt_ns && b->last_rtt_ns >
		    max_t(u64, b->min_rtt_ns * 2, NSEC_PER_MSEC))
			saturated = true;
		if (!saturated && b->bw_max_bps) {
			if (b->bw_max_bps >= b->safe_bw_bps * 11 / 10) {
				b->safe_bw_bps = b->bw_max_bps;
				b->slow_cnt = 0;
			} else if (++b->slow_cnt >= 5) {
				saturated = true;
			}
		}
		if (now - b->startup_ts > UWG_BBR_STARTUP_MAX_NS)
			saturated = true;

		if (saturated) {
			u64 safe = b->safe_bw_bps ? b->safe_bw_bps :
				(b->bw_max_bps ? b->bw_max_bps :
				 b->pacing_bps / 2);

			b->full_bw_bps = max_t(u64, safe, UWG_BBR_MIN_RATE_BPS);
			b->safe_bw_bps = b->full_bw_bps;
			b->phase = UWG_BBR_DRAIN;
			pacing = b->full_bw_bps;
		} else {
			pacing = min_t(u64, b->pacing_bps +
					       b->pacing_bps / 5,
					       UWG_BBR_MAX_RATE_BPS);
		}
		break;
	}
	case UWG_BBR_DRAIN:
		pacing = max_t(u64, b->full_bw_bps, UWG_BBR_MIN_RATE_BPS);
		if (!cc->qlen || now - b->cycle_ts >
		    max_t(u64, 2 * rtt, 20 * NSEC_PER_MSEC)) {
			b->phase = UWG_BBR_PROBE_BW;
			b->cycle_idx = 0;
			b->cycle_ts = now;
		}
		break;
	case UWG_BBR_PROBE_BW: {
		u64 interval = max_t(u64, 2 * rtt, 100 * NSEC_PER_MSEC);
		u8 gain;

		if (now - b->bw_ts >= UWG_BBR_BW_STEP_NS) {
			b->bw_ts = now;
			if (b->loss_pct > 10)
				b->full_bw_bps = max_t(u64,
						b->full_bw_bps -
						b->full_bw_bps / 10,
						UWG_BBR_MIN_RATE_BPS);
			else if (b->full_bw_bps < UWG_BBR_MAX_RATE_BPS)
				b->full_bw_bps += min_t(u64,
						b->full_bw_bps / 4 + 1000,
						UWG_BBR_MAX_RATE_BPS -
						b->full_bw_bps);
		}
		if (now - b->cycle_ts >= interval) {
			b->cycle_idx = (b->cycle_idx + 1) & 7;
			b->cycle_ts = now;
		}
		gain = uwg_bbr_gains[b->cycle_idx];
		pacing = mul_u64_u64_div_u64(b->full_bw_bps, gain, 4);
		if (gain > 4 && b->last_rtt_ns && b->min_rtt_ns &&
		    b->last_rtt_ns > mul_u64_u64_div_u64(b->min_rtt_ns, 3, 2))
			pacing = b->full_bw_bps;
		if (now - b->min_rtt_ts > UWG_BBR_PROBE_RTT_NS) {
			b->phase = UWG_BBR_PROBE_RTT;
			b->probe_ts = now;
		}
		break;
	}
	case UWG_BBR_PROBE_RTT:
		pacing = max_t(u64, b->full_bw_bps / 2, UWG_BBR_MIN_RATE_BPS);
		if (now - b->probe_ts >= UWG_BBR_PROBE_RTT_DURATION_NS) {
			b->phase = UWG_BBR_PROBE_BW;
			b->min_rtt_ts = now;
			b->cycle_ts = now;
		}
		break;
	default:
		pacing = UWG_BBR_MIN_RATE_BPS;
		break;
	}

	if (now - b->last_fb_ts > UWG_BBR_FB_TIMEOUT_NS)
		pacing = min_t(u64, pacing, max_t(u64, b->pacing_bps,
						   UWG_BBR_MIN_RATE_BPS));

	b->pacing_bps = clamp_t(u64, pacing, UWG_BBR_MIN_RATE_BPS,
				UWG_BBR_MAX_RATE_BPS);
}

void uwg_cc_bbr_on_feedback(struct wg_peer *peer, u64 rtt_ns,
			    u64 recv_bytes, u64 recv_packets, u64 now)
{
	struct uwg_cc *cc = &peer->cc;
	struct uwg_cc_bbr *b = &cc->bbr;
	u64 rate;

	spin_lock_bh(&cc->lock);
	if (READ_ONCE(cc->mode) != UWG_CC_BBR)
		goto out;

	uwg_bbr_push_sample(b, recv_bytes, now);
	rate = uwg_bbr_rate_sample(b, now, recv_bytes);
	uwg_bbr_note_bw(b, rate, now);
	uwg_bbr_note_loss(b, READ_ONCE(cc->sent_packets), recv_packets);
	b->last_fb_ts = now;

	if (rtt_ns) {
		b->last_rtt_ns = rtt_ns;
		if (!b->min_rtt_ns || rtt_ns < b->min_rtt_ns ||
		    now - b->min_rtt_ts > UWG_BBR_MIN_RTT_WINDOW_NS) {
			b->min_rtt_ns = rtt_ns;
			b->min_rtt_ts = now;
		}
	}

	uwg_cc_bbr_tick(cc, now);
	WRITE_ONCE(cc->rate_bps, b->pacing_bps);
out:
	spin_unlock_bh(&cc->lock);
}
