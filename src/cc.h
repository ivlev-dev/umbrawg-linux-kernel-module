/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UWG_CC_H
#define _UWG_CC_H

#include <linux/hrtimer.h>
#include <linux/jump_label.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct wg_peer;
struct wg_device;
struct dentry;

enum uwg_cc_mode {
	UWG_CC_NONE = 0,
	UWG_CC_BRUTAL,
	UWG_CC_BBR,
};

#define UWG_CC_BUCKETS		8
#define UWG_CC_MAX_QUEUED	4096
#define UWG_CC_MIN_QUEUED	128
#define UWG_CC_QUEUE_NS		(3ULL * NSEC_PER_MSEC)
#define UWG_CC_TIMER_BUDGET	1024
#define UWG_CC_TIMER_MIN_NS	100000ULL
#define UWG_CC_MIN_CAP_BYTES	4096
#define UWG_CC_MAX_CAP_BYTES	(1024 * 1024)

#define UWG_BRUTAL_SLOTS		5
#define UWG_BRUTAL_MIN_SAMPLES	50
#define UWG_BRUTAL_MIN_ACK_PCT	80
#define UWG_BRUTAL_CWND_GAIN	2

extern struct static_key_false uwg_cc_active;
extern struct static_key_false uwg_cc_bbr_active;

enum uwg_cc_phase {
	UWG_BBR_STARTUP = 0,
	UWG_BBR_DRAIN,
	UWG_BBR_PROBE_BW,
	UWG_BBR_PROBE_RTT,
};

#define UWG_BBR_BW_SLOTS		8
#define UWG_BBR_BW_SLOT_NS		(250ULL * NSEC_PER_MSEC)
#define UWG_BBR_SAMPLES		16
#define UWG_BBR_SAMPLE_MIN_NS		(10ULL * NSEC_PER_MSEC)
#define UWG_BBR_MIN_RATE_BPS		64000ULL
#define UWG_BBR_MAX_RATE_BPS		100000000000ULL
#define UWG_BBR_STARTUP_MAX_NS		(10ULL * NSEC_PER_SEC)
#define UWG_BBR_MIN_ROUND_NS		(10ULL * NSEC_PER_MSEC)
#define UWG_BBR_BW_STEP_NS		(100ULL * NSEC_PER_MSEC)
#define UWG_BBR_MIN_RTT_WINDOW_NS	(10ULL * NSEC_PER_SEC)
#define UWG_BBR_PROBE_RTT_NS		(10ULL * NSEC_PER_SEC)
#define UWG_BBR_PROBE_RTT_DURATION_NS	(200ULL * NSEC_PER_MSEC)
#define UWG_BBR_FB_TIMEOUT_NS		(2ULL * NSEC_PER_SEC)
#define UWG_BBR_IDLE_NS			(200ULL * NSEC_PER_MSEC)
#define UWG_BBR_PEAK_WINDOW_NS		(10ULL * NSEC_PER_SEC)
#define UWG_BBR_GAINS			8
#define UWG_BBR_GAIN_HIGH		2885
#define UWG_BBR_INIT_BYTES		(32 * 1420)
#define UWG_BBR_GAIN_DRAIN		347
#define UWG_BBR_GAIN_CWND		2000
#define UWG_BBR_STARTUP_TARGET		1250
#define UWG_BBR_STARTUP_ROUNDS		3
#define UWG_BBR_LOSS_FULL_PIPE		2
#define UWG_BBR_RAMP_PCT		15

struct uwg_cc_sample {
	u64 bytes;
	u64 ts;
};

struct uwg_cc_brutal {
	u64 base_bps;
	u64 pace_bps;
	u64 prev_sent_packets;
	u64 prev_rx_packets;
	u64 slots_ts[UWG_BRUTAL_SLOTS];
	u32 slots_acked[UWG_BRUTAL_SLOTS];
	u32 slots_lost[UWG_BRUTAL_SLOTS];
	u32 ack_pct;
	u32 samples;
};

struct uwg_cc_bbr {
	enum uwg_cc_phase phase;
	u64 pacing_bps;
	u64 bw_slots[UWG_BBR_BW_SLOTS];
	u64 bw_bucket;
	u64 bw_max_bps;
	u64 last_bw_bps;
	u64 peak_bw_bps;
	u64 peak_ts;
	u64 full_bw_bps;
	u64 prev_sent_bytes;
	u64 last_tick_ts;
	u64 base_bps;
	u64 safe_bw_bps;
	u32 loss_pct;
	u32 slow_cnt;
	u64 prev_tx;
	u64 prev_rx;
	u64 min_rtt_ns;
	u64 min_rtt_ts;
	u64 last_rtt_ns;
	u64 cycle_ts;
	u64 startup_ts;
	u64 bw_ts;
	u32 cycle_idx;
	u64 probe_ts;
	u64 last_fb_ts;
	struct uwg_cc_sample samples[UWG_BBR_SAMPLES];
	u32 sample_head;
	u32 sample_count;
};

struct uwg_cc_fb {
	struct hrtimer timer;
	bool armed;
	u64 rx_bytes;
	u64 rx_packets;
	u64 rx_highest;
	u64 last_rx_ns;
	atomic64_t requests_sent;
	atomic64_t requests_rx;
	atomic64_t feedback_sent;
	atomic64_t feedback_rx;
};

struct uwg_cc_bucket {
	struct sk_buff_head queue;
};

struct uwg_cc_loss {
	u64 prev_highest;
	u64 prev_rx;
	u64 prev_drop;
	bool started;
};

struct uwg_cc {
	enum uwg_cc_mode mode;
	u64 rate_bps;
	s64 tokens;
	ktime_t last;
	struct hrtimer timer;
	spinlock_t lock;
	struct uwg_cc_bucket *buckets;
	unsigned long busy;
	unsigned int qlen;
	unsigned int cursor;
	bool armed;
	u64 sent_packets;
	u64 sent_bytes;
	u64 dropped_packets;
	struct uwg_cc_loss loss;
	struct uwg_cc_bbr bbr;
	struct uwg_cc_brutal brutal;
	struct uwg_cc_fb fb;
};

void uwg_cc_init(struct uwg_cc *cc);
int uwg_cc_set(struct wg_peer *peer, enum uwg_cc_mode mode, u64 rate_bps);
void uwg_cc_clear(struct wg_peer *peer);
void uwg_cc_purge(struct wg_peer *peer);
bool uwg_cc_tx(struct wg_peer *peer, struct sk_buff *skb);
u32 uwg_cc_flow_hash(const struct sk_buff *skb);
const char *uwg_cc_mode_name(enum uwg_cc_mode mode);
void uwg_cc_loss_update(struct uwg_cc *cc, u64 recv_highest, u64 recv_packets,
			u64 echo_drops, u64 *acked, u64 *lost);

void uwg_cc_fb_init(struct uwg_cc *cc);
void uwg_cc_fb_start(struct wg_peer *peer);
void uwg_cc_fb_stop(struct wg_peer *peer);
void uwg_cc_fb_kick_locked(struct wg_peer *peer);
void uwg_cc_fb_account(struct wg_peer *peer, const struct sk_buff *skb,
		       unsigned int wire_len, u64 counter);
bool uwg_cc_fb_rx(struct wg_peer *peer, const struct sk_buff *skb, u8 kind,
		  u64 nonce);

void uwg_cc_bbr_reset(struct uwg_cc *cc, u64 start_bps);
void uwg_cc_bbr_tick(struct uwg_cc *cc, u64 now_ns);
void uwg_cc_bbr_on_feedback(struct wg_peer *peer, u64 rtt_ns,
			    u64 recv_bytes, u64 recv_packets, u64 recv_highest,
			    u64 echo_drops, u64 now_ns);
const char *uwg_cc_phase_name(enum uwg_cc_phase phase);

void uwg_cc_brutal_reset(struct uwg_cc *cc, u64 base_bps);
void uwg_cc_brutal_tick(struct uwg_cc *cc, u64 now_ns);
void uwg_cc_brutal_on_feedback(struct wg_peer *peer, u64 rtt_ns,
			       u64 recv_packets, u64 recv_highest, u64 echo_drops,
			       u64 now_ns);

unsigned int uwg_cc_next_index(void);
int uwg_cc_debugfs_init(void);
void uwg_cc_debugfs_uninit(void);
void uwg_cc_debugfs_add_device(struct wg_device *wg);
void uwg_cc_debugfs_remove_device(struct wg_device *wg);

#endif /* _UWG_CC_H */
