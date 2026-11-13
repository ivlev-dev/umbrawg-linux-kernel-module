// SPDX-License-Identifier: GPL-2.0
#include "cc.h"
#include "peer.h"

#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/string.h>

/* Pacing gains of the PROBE_BW cycle, in per mille. */
static const u16 uwg_bbr_gains[UWG_BBR_GAINS] = {
	1250, 750, 1000, 1000, 1000, 1000, 1000, 1000
};

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

static u64 uwg_bbr_gain(u64 rate, u32 gain)
{
	return mul_u64_u64_div_u64(rate, gain, 1000);
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

	if (!rate)
		return;

	uwg_bbr_expire_window(b, now);
	idx = div_u64(now, UWG_BBR_BW_SLOT_NS) & (UWG_BBR_BW_SLOTS - 1);
	if (rate > b->bw_slots[idx])
		b->bw_slots[idx] = rate;
}

static u64 uwg_bbr_rate_sample(struct uwg_cc_bbr *b, u64 now, u64 cur_bytes)
{
	u32 i, idx = 0;
	bool found = false;

	if (b->sample_count < 2)
		return 0;

	/* The newest interval long enough to be meaningful. */
	for (i = b->sample_count; i > 0; --i) {
		idx = (b->sample_head + UWG_BBR_SAMPLES - i) &
		      (UWG_BBR_SAMPLES - 1);
		if (now - b->samples[idx].ts >= UWG_BBR_SAMPLE_MIN_NS) {
			found = true;
			break;
		}
	}
	if (!found)
		return 0;

	if (now <= b->samples[idx].ts)
		return 0;
	if (cur_bytes < b->samples[idx].bytes) {
		b->sample_count = 0;
		return 0;
	}

	return mul_u64_u64_div_u64(cur_bytes - b->samples[idx].bytes,
				   8 * (u64)NSEC_PER_SEC,
				   now - b->samples[idx].ts);
}

static void uwg_bbr_push_sample(struct uwg_cc_bbr *b, u64 bytes, u64 now)
{
	b->samples[b->sample_head].bytes = bytes;
	b->samples[b->sample_head].ts = now;
	b->sample_head = (b->sample_head + 1) & (UWG_BBR_SAMPLES - 1);
	if (b->sample_count < UWG_BBR_SAMPLES)
		b->sample_count++;
}

static void uwg_bbr_note_loss(struct uwg_cc_bbr *b, u64 acked, u64 lost)
{
	u64 total = acked + lost;

	if (!total) {
		b->loss_pct = 0;
		return;
	}

	b->loss_pct = (u32)min_t(u64, div_u64(lost * 100, total), 100);
}

static void uwg_bbr_enter_drain(struct uwg_cc_bbr *b, u64 now)
{
	b->phase = UWG_BBR_DRAIN;
	b->cycle_ts = now;
	b->full_bw_bps = b->bw_max_bps;
	b->pacing_bps = max_t(u64, b->pacing_bps, UWG_BBR_MIN_RATE_BPS);
}

static void uwg_bbr_enter_probe_bw(struct uwg_cc_bbr *b, u64 now)
{
	b->phase = UWG_BBR_PROBE_BW;
	b->cycle_idx = 0;
	b->cycle_ts = now;
}

static void uwg_bbr_enter_probe_rtt(struct uwg_cc_bbr *b, u64 now)
{
	b->phase = UWG_BBR_PROBE_RTT;
	b->probe_ts = now;
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
	b->base_bps = start_bps;
	b->bw_bucket = div_u64(now, UWG_BBR_BW_SLOT_NS);
	b->min_rtt_ts = now;
	b->last_fb_ts = now;
	b->cycle_ts = now;
	b->startup_ts = now;
	b->bw_ts = now;
	b->last_tick_ts = now;
}

void uwg_cc_bbr_tick(struct uwg_cc *cc, u64 now)
{
	struct uwg_cc_bbr *b = &cc->bbr;
	u64 rtt = max_t(u64, b->min_rtt_ns, UWG_BBR_MIN_ROUND_NS);
	u64 bw = uwg_bbr_window_max(b, now);
	u64 pacing = b->pacing_bps;
	u64 sent = READ_ONCE(cc->sent_bytes);

	/*
	 * No traffic means no new information: freeze the state machine instead
	 * of letting startup rounds expire or the pacer decay while idle.
	 */
	if (sent == b->prev_sent_bytes && !cc->qlen)
		return;
	b->prev_sent_bytes = sent;

	if (now - b->last_tick_ts > UWG_BBR_IDLE_NS) {
		/* Traffic resumed after a pause: restart the current round. */
		b->cycle_ts = now;
		b->startup_ts = now;
		b->slow_cnt = 0;
		b->sample_count = 0;
	}
	b->last_tick_ts = now;

	if (bw)
		b->last_bw_bps = bw;
	else
		bw = b->last_bw_bps;

	/*
	 * A long peak filter on top of the short one. The inner transport is
	 * itself a congestion controller: when it backs off after a loss, the
	 * delivered rate collapses for a while and a short-only estimate would
	 * follow it down, turning one loss into a permanent rate cut.
	 */
	if (bw >= b->peak_bw_bps) {
		b->peak_bw_bps = bw;
		b->peak_ts = now;
	} else if (now - b->peak_ts > UWG_BBR_PEAK_WINDOW_NS) {
		b->peak_bw_bps = bw;
		b->peak_ts = now;
	}
	bw = b->peak_bw_bps;
	b->bw_max_bps = bw;

	switch (b->phase) {
	case UWG_BBR_STARTUP:
		/*
		 * A round is one bandwidth-filter slot, not one RTT: the delivery
		 * rate is sampled in 100 ms slots, so checking for growth faster
		 * than that would end startup on stale data.
		 */
		if (now - b->cycle_ts >= UWG_BBR_BW_STEP_NS) {
			b->cycle_ts = now;
			if (bw && bw * 1000 >=
				  b->safe_bw_bps * UWG_BBR_STARTUP_TARGET) {
				b->safe_bw_bps = bw;
				b->slow_cnt = 0;
			} else if (++b->slow_cnt >= UWG_BBR_STARTUP_ROUNDS) {
				uwg_bbr_enter_drain(b, now);
				break;
			}
			if (b->loss_pct >= UWG_BBR_LOSS_FULL_PIPE ||
			    now - b->startup_ts > UWG_BBR_STARTUP_MAX_NS) {
				uwg_bbr_enter_drain(b, now);
				break;
			}
		}
		if (bw)
			pacing = max_t(u64, pacing,
				       uwg_bbr_gain(bw, UWG_BBR_GAIN_HIGH));
		else if (b->min_rtt_ns)
			/* Initial window over the measured RTT, as BBR does. */
			pacing = max_t(u64, pacing,
				       mul_u64_u64_div_u64(UWG_BBR_INIT_BYTES,
							   8 * (u64)NSEC_PER_SEC,
							   b->min_rtt_ns));
		else
			pacing = min_t(u64, pacing +
				       div_u64(pacing * UWG_BBR_RAMP_PCT, 100),
				       UWG_BBR_MAX_RATE_BPS);
		break;
	case UWG_BBR_DRAIN:
		pacing = bw ? uwg_bbr_gain(bw, UWG_BBR_GAIN_DRAIN) :
			      max_t(u64, b->pacing_bps / 2,
				    UWG_BBR_MIN_RATE_BPS);
		if (!cc->qlen ||
		    now - b->cycle_ts >= max_t(u64, 2 * rtt, 20 * NSEC_PER_MSEC))
			uwg_bbr_enter_probe_bw(b, now);
		break;
	case UWG_BBR_PROBE_BW: {
		u32 gain;

		if (now - b->cycle_ts >=
		    max_t(u64, rtt, UWG_BBR_BW_STEP_NS)) {
			b->cycle_idx = (b->cycle_idx + 1) % UWG_BBR_GAINS;
			b->cycle_ts = now;
		}
		gain = uwg_bbr_gains[b->cycle_idx];
		if (b->loss_pct >= UWG_BBR_LOSS_FULL_PIPE && gain > 1000)
			gain = 1000;
		pacing = bw ? uwg_bbr_gain(bw, gain) : b->pacing_bps;

		if (now - b->min_rtt_ts > UWG_BBR_MIN_RTT_WINDOW_NS)
			uwg_bbr_enter_probe_rtt(b, now);
		break;
	}
	case UWG_BBR_PROBE_RTT:
		pacing = bw ? bw : b->pacing_bps;
		if (now - b->probe_ts >= UWG_BBR_PROBE_RTT_DURATION_NS) {
			b->min_rtt_ts = now;
			uwg_bbr_enter_probe_bw(b, now);
		}
		break;
	default:
		pacing = UWG_BBR_MIN_RATE_BPS;
		break;
	}

	if (now - b->last_fb_ts > UWG_BBR_FB_TIMEOUT_NS)
		pacing = min_t(u64, pacing, b->pacing_bps);

	b->pacing_bps = clamp_t(u64, pacing, UWG_BBR_MIN_RATE_BPS,
				UWG_BBR_MAX_RATE_BPS);
}

void uwg_cc_bbr_on_feedback(struct wg_peer *peer, u64 rtt_ns,
			    u64 recv_bytes, u64 recv_packets, u64 recv_highest,
			    u64 echo_drops, u64 now)
{
	struct uwg_cc *cc = &peer->cc;
	struct uwg_cc_bbr *b = &cc->bbr;
	u64 rate, acked, lost;

	spin_lock_bh(&cc->lock);
	if (READ_ONCE(cc->mode) != UWG_CC_BBR)
		goto out;

	uwg_bbr_push_sample(b, recv_bytes, now);
	rate = uwg_bbr_rate_sample(b, now, recv_bytes);
	uwg_bbr_note_bw(b, rate, now);
	uwg_cc_loss_update(cc, recv_highest, recv_packets, echo_drops,
			   &acked, &lost);
	uwg_bbr_note_loss(b, acked, lost);
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
