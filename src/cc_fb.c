// SPDX-License-Identifier: GPL-2.0
#include "cc.h"
#include "device.h"
#include "frame.h"
#include "messages.h"
#include "noise.h"
#include "peer.h"
#include "queueing.h"
#include "timers.h"

#include <linux/moduleparam.h>
#include <linux/unaligned.h>

static unsigned int uwg_cc_fb_ms __read_mostly = 20;
module_param_named(cc_fb_ms, uwg_cc_fb_ms, uint, 0644);
MODULE_PARM_DESC(cc_fb_ms, "feedback probe interval in milliseconds");

static struct sk_buff *uwg_cc_make_skb(struct wg_peer *peer, const u8 *buf,
				       size_t len)
{
	struct sk_buff *skb;

	skb = alloc_skb(DATA_PACKET_HEAD_ROOM + len, GFP_ATOMIC);
	if (unlikely(!skb))
		return NULL;

	skb_reserve(skb, DATA_PACKET_HEAD_ROOM);
	skb_put_data(skb, buf, len);
	skb->dev = peer->device->dev;
	PACKET_CB(skb)->mtu = peer->device->dev->mtu;
	PACKET_CB(skb)->is_service = true;
	PACKET_CB(skb)->ds = 0;
	PACKET_CB(skb)->flow_hash = 0;
	return skb;
}

static void uwg_cc_enqueue(struct wg_peer *peer, struct sk_buff *skb)
{
	spin_lock_bh(&peer->staged_packet_queue.lock);
	__skb_queue_tail(&peer->staged_packet_queue, skb);
	spin_unlock_bh(&peer->staged_packet_queue.lock);
	wg_packet_send_staged_packets(peer);
}

static void uwg_cc_fb_send(struct wg_peer *peer, u64 echo_ts, u64 echo_drops)
{
	struct uwg_cc *cc = &peer->cc;
	u8 buf[UWG_CC_FEEDBACK_LEN];
	struct sk_buff *skb;

	buf[0] = UWG_FRAME_MAGIC;
	buf[1] = UWG_FRAME_CC_FEEDBACK;
	buf[2] = 0;
	buf[3] = 0;
	put_unaligned_le64(echo_ts, buf + 4);
	put_unaligned_le64(cc->fb.rx_bytes, buf + 12);
	put_unaligned_le64(cc->fb.rx_packets, buf + 20);
	put_unaligned_le32(0, buf + 28);
	put_unaligned_le64(cc->fb.rx_highest, buf + 32);
	put_unaligned_le64(echo_drops, buf + 40);

	skb = uwg_cc_make_skb(peer, buf, sizeof(buf));
	if (unlikely(!skb))
		return;
	atomic64_inc(&cc->fb.feedback_sent);
	uwg_cc_enqueue(peer, skb);
}

static void uwg_cc_fb_send_request(struct wg_peer *peer)
{
	u8 buf[UWG_CC_REQUEST_LEN];
	struct sk_buff *skb;

	buf[0] = UWG_FRAME_MAGIC;
	buf[1] = UWG_FRAME_CC_REQUEST;
	buf[2] = 0;
	buf[3] = 0;
	put_unaligned_le64(ktime_get_ns(), buf + 4);
	put_unaligned_le64(READ_ONCE(peer->cc.dropped_packets), buf + 12);

	skb = uwg_cc_make_skb(peer, buf, sizeof(buf));
	if (unlikely(!skb))
		return;
	atomic64_inc(&peer->cc.fb.requests_sent);
	uwg_cc_enqueue(peer, skb);
}

static bool uwg_cc_peer_has_key(struct wg_peer *peer)
{
	struct noise_keypair *keypair;
	bool ok = false;

	rcu_read_lock_bh();
	keypair = rcu_dereference_bh(peer->keypairs.current_keypair);
	ok = keypair && READ_ONCE(keypair->sending.is_valid) &&
	     !wg_birthdate_has_expired(keypair->sending.birthdate,
				       REJECT_AFTER_TIME);
	rcu_read_unlock_bh();
	return ok;
}

static enum hrtimer_restart uwg_cc_fb_timer(struct hrtimer *timer)
{
	struct uwg_cc *cc = container_of(timer, struct uwg_cc, fb.timer);
	struct wg_peer *peer = container_of(cc, struct wg_peer, cc);
	u64 now = ktime_get_ns();
	u64 interval = (u64)uwg_cc_fb_ms * NSEC_PER_MSEC;
	bool restart = false;

	if (unlikely(!wg_peer_get_maybe_zero(peer)))
		return HRTIMER_NORESTART;

	if (READ_ONCE(cc->mode) != UWG_CC_NONE && uwg_cc_peer_has_key(peer)) {
		u64 last_rx = READ_ONCE(cc->fb.last_rx_ns);

		uwg_cc_fb_send_request(peer);
		if (last_rx && now - last_rx > UWG_BBR_FB_TIMEOUT_NS)
			interval = 500 * NSEC_PER_MSEC;
	}

	spin_lock_bh(&cc->lock);
	if (READ_ONCE(cc->mode) == UWG_CC_BBR) {
		uwg_cc_bbr_tick(cc, now);
		WRITE_ONCE(cc->rate_bps, cc->bbr.pacing_bps);
		cc->fb.armed = true;
		restart = true;
	} else if (READ_ONCE(cc->mode) == UWG_CC_BRUTAL) {
		uwg_cc_brutal_tick(cc, now);
		cc->fb.armed = true;
		restart = true;
	} else {
		cc->fb.armed = false;
	}
	spin_unlock_bh(&cc->lock);

	wg_peer_put(peer);

	if (restart) {
		hrtimer_forward_now(timer, ns_to_ktime(interval));
		return HRTIMER_RESTART;
	}
	return HRTIMER_NORESTART;
}

void uwg_cc_fb_init(struct uwg_cc *cc)
{
	memset(&cc->fb, 0, sizeof(cc->fb));
	hrtimer_setup(&cc->fb.timer, uwg_cc_fb_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_SOFT);
}

void uwg_cc_fb_kick_locked(struct wg_peer *peer)
{
	struct uwg_cc_fb *fb = &peer->cc.fb;

	if (fb->armed)
		return;
	fb->armed = true;
	hrtimer_start(&fb->timer,
		      ns_to_ktime((u64)uwg_cc_fb_ms * NSEC_PER_MSEC),
		      HRTIMER_MODE_REL_SOFT);
}

void uwg_cc_fb_start(struct wg_peer *peer)
{
	struct uwg_cc *cc = &peer->cc;

	spin_lock_bh(&cc->lock);
	WRITE_ONCE(cc->fb.last_rx_ns, ktime_get_ns());
	uwg_cc_fb_kick_locked(peer);
	spin_unlock_bh(&cc->lock);
}

void uwg_cc_fb_stop(struct wg_peer *peer)
{
	struct uwg_cc *cc = &peer->cc;

	hrtimer_try_to_cancel(&cc->fb.timer);
	spin_lock_bh(&cc->lock);
	cc->fb.armed = false;
	spin_unlock_bh(&cc->lock);
}

static void uwg_cc_fb_note(struct uwg_cc_fb *fb, u64 counter)
{
	if (counter > fb->rx_highest)
		fb->rx_highest = counter;
}

void uwg_cc_fb_account(struct wg_peer *peer, const struct sk_buff *skb,
		       unsigned int wire_len, u64 counter)
{
	struct uwg_cc_fb *fb = &peer->cc.fb;

	if (READ_ONCE(peer->cc.mode) == UWG_CC_NONE)
		return;

	fb->rx_bytes += wire_len;
	fb->rx_packets++;
	uwg_cc_fb_note(fb, counter);
}

bool uwg_cc_fb_rx(struct wg_peer *peer, const struct sk_buff *skb, u8 kind,
		  u64 nonce)
{
	u8 buf[UWG_CC_FEEDBACK_LEN];
	u64 now = ktime_get_ns();

	switch (kind) {
	case UWG_FRAME_CC_REQUEST: {
		u64 echo_drops;

		if (unlikely(skb_copy_bits(skb, 0, buf,
					   UWG_CC_REQUEST_LEN) < 0))
			return true;
		atomic64_inc(&peer->cc.fb.requests_rx);
		echo_drops = get_unaligned_le64(buf + 12);
		uwg_cc_fb_send(peer, get_unaligned_le64(buf + 4), echo_drops);
		return true;
	}
	case UWG_FRAME_CC_FEEDBACK: {
		u64 echo_ts, rtt, echo_drops;

		if (unlikely(skb_copy_bits(skb, 0, buf,
					   UWG_CC_FEEDBACK_LEN) < 0))
			return true;
		echo_ts = get_unaligned_le64(buf + 4);
		rtt = now > echo_ts ? now - echo_ts : 0;
		echo_drops = get_unaligned_le64(buf + 40);
		atomic64_inc(&peer->cc.fb.feedback_rx);
		WRITE_ONCE(peer->cc.fb.last_rx_ns, now);
		switch (READ_ONCE(peer->cc.mode)) {
		case UWG_CC_BBR:
			uwg_cc_bbr_on_feedback(peer, rtt,
					       get_unaligned_le64(buf + 12),
					       get_unaligned_le64(buf + 20),
					       get_unaligned_le64(buf + 32),
					       echo_drops, now);
			break;
		case UWG_CC_BRUTAL:
			uwg_cc_brutal_on_feedback(peer, rtt,
						  get_unaligned_le64(buf + 20),
						  get_unaligned_le64(buf + 32),
						  echo_drops, now);
			break;
		default:
			break;
		}
		return true;
	}
	default:
		return false;
	}
}
