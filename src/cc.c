// SPDX-License-Identifier: GPL-2.0
#include "cc.h"
#include "device.h"
#include "peer.h"
#include "queueing.h"
#include "socket.h"

#include <linux/debugfs.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/netdevice.h>
#include <linux/seq_file.h>
#include <net/ip.h>
#include <linux/math64.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

DEFINE_STATIC_KEY_FALSE(uwg_cc_active);
DEFINE_STATIC_KEY_FALSE(uwg_cc_bbr_active);

static unsigned int uwg_cc_burst_ms __read_mostly = 10;
module_param_named(cc_burst_ms, uwg_cc_burst_ms, uint, 0644);
MODULE_PARM_DESC(cc_burst_ms, "token bucket burst window in milliseconds");

static const char *const uwg_cc_names[] = {
	[UWG_CC_NONE] = "none",
	[UWG_CC_BRUTAL] = "brutal",
	[UWG_CC_BBR] = "bbr",
};

const char *uwg_cc_mode_name(enum uwg_cc_mode mode)
{
	if (mode > UWG_CC_BBR)
		return "unknown";
	return uwg_cc_names[mode];
}

u32 uwg_cc_flow_hash(const struct sk_buff *skb)
{
	u32 a = 0, b = 0, c = 0, ports = 0;
	u8 buf[sizeof(ports)];

	if (skb->protocol == htons(ETH_P_IP)) {
		const struct iphdr *ip = ip_hdr(skb);

		a = (__force u32)ip->saddr;
		b = (__force u32)ip->daddr;
		c = ip->protocol;
		if (!(ip->frag_off & htons(IP_OFFSET)) &&
		    (ip->protocol == IPPROTO_TCP || ip->protocol == IPPROTO_UDP)) {
			const void *p = skb_header_pointer(skb, ip->ihl * 4,
							   sizeof(buf), buf);

			if (p)
				memcpy(&ports, p, sizeof(ports));
		}
	} else if (skb->protocol == htons(ETH_P_IPV6)) {
		const struct ipv6hdr *ip6 = ipv6_hdr(skb);

		a = (__force u32)ip6->saddr.s6_addr32[0] ^
		    (__force u32)ip6->saddr.s6_addr32[1] ^
		    (__force u32)ip6->saddr.s6_addr32[2] ^
		    (__force u32)ip6->saddr.s6_addr32[3];
		b = (__force u32)ip6->daddr.s6_addr32[0] ^
		    (__force u32)ip6->daddr.s6_addr32[1] ^
		    (__force u32)ip6->daddr.s6_addr32[2] ^
		    (__force u32)ip6->daddr.s6_addr32[3];
		c = ip6->nexthdr;
		if (ip6->nexthdr == IPPROTO_TCP || ip6->nexthdr == IPPROTO_UDP) {
			const void *p = skb_header_pointer(skb,
							   sizeof(*ip6),
							   sizeof(buf), buf);

			if (p)
				memcpy(&ports, p, sizeof(ports));
		}
	}

	return jhash_2words(a ^ ports, b ^ c, 0x9e3779b9);
}

static u64 uwg_cc_cap_bytes(const struct wg_peer *peer)
{
	u64 rate = READ_ONCE(peer->cc.rate_bps);
	u64 cap;

	if (!rate)
		return UWG_CC_MIN_CAP_BYTES;

	cap = mul_u64_u64_div_u64(rate, (u64)uwg_cc_burst_ms * NSEC_PER_MSEC,
				  8 * NSEC_PER_SEC);
	cap = min_t(u64, cap, UWG_CC_MAX_CAP_BYTES);
	return max_t(u64, cap, UWG_CC_MIN_CAP_BYTES);
}

static void uwg_cc_refill(struct wg_peer *peer, ktime_t now)
{
	struct uwg_cc *cc = &peer->cc;
	u64 rate = READ_ONCE(cc->rate_bps);
	u64 dt;

	if (!rate) {
		cc->tokens = 0;
		cc->last = now;
		return;
	}

	dt = ktime_to_ns(ktime_sub(now, cc->last));
	cc->last = now;
	if (dt)
		cc->tokens = min_t(s64, cc->tokens +
					   (s64)mul_u64_u64_div_u64(rate, dt,
							    8 * NSEC_PER_SEC),
				   (s64)uwg_cc_cap_bytes(peer));
}

static unsigned int uwg_cc_bucket_index(const struct sk_buff *skb)
{
	return PACKET_CB(skb)->flow_hash & (UWG_CC_BUCKETS - 1);
}

static struct sk_buff *uwg_cc_peek_locked(struct uwg_cc *cc)
{
	unsigned int i, idx;

	for (i = 0; i < UWG_CC_BUCKETS; ++i) {
		idx = (cc->cursor + i) & (UWG_CC_BUCKETS - 1);
		if (cc->busy & BIT(idx))
			return skb_peek(&cc->buckets[idx].queue);
	}
	return NULL;
}

static struct sk_buff *uwg_cc_pop_locked(struct uwg_cc *cc)
{
	unsigned int i, idx;
	struct sk_buff *skb;

	for (i = 0; i < UWG_CC_BUCKETS; ++i) {
		idx = (cc->cursor + i) & (UWG_CC_BUCKETS - 1);
		if (!(cc->busy & BIT(idx)))
			continue;
		skb = __skb_dequeue(&cc->buckets[idx].queue);
		if (skb_queue_empty(&cc->buckets[idx].queue))
			cc->busy &= ~BIT(idx);
		cc->cursor = (idx + 1) & (UWG_CC_BUCKETS - 1);
		cc->qlen--;
		return skb;
	}
	return NULL;
}

static void uwg_cc_arm_locked(struct wg_peer *peer, u64 need_bytes)
{
	struct uwg_cc *cc = &peer->cc;
	u64 rate = READ_ONCE(cc->rate_bps);
	u64 delay_ns;

	if (!rate || cc->armed)
		return;

	delay_ns = mul_u64_u64_div_u64(max_t(u64, need_bytes, 1),
				       8 * NSEC_PER_SEC, rate);
	delay_ns = clamp_t(u64, delay_ns, UWG_CC_TIMER_MIN_NS,
			   (u64)uwg_cc_burst_ms * 2 * NSEC_PER_MSEC);
	cc->armed = true;
	hrtimer_start(&cc->timer, ns_to_ktime(delay_ns),
		      HRTIMER_MODE_REL_SOFT);
}

static enum hrtimer_restart uwg_cc_timer(struct hrtimer *timer)
{
	struct uwg_cc *cc = container_of(timer, struct uwg_cc, timer);
	struct wg_peer *peer = container_of(cc, struct wg_peer, cc);
	struct sk_buff_head tx;
	struct sk_buff *skb;
	unsigned int i;

	rcu_read_lock();
	if (!wg_peer_get_maybe_zero(peer)) {
		rcu_read_unlock();
		return HRTIMER_NORESTART;
	}
	rcu_read_unlock();

	__skb_queue_head_init(&tx);

	spin_lock(&cc->lock);
	cc->armed = false;
	uwg_cc_refill(peer, ktime_get());
	for (i = 0; i < UWG_CC_TIMER_BUDGET; ++i) {
		skb = uwg_cc_peek_locked(cc);
		if (!skb)
			break;
		if (cc->tokens < (s64)skb->len &&
		    (skb->len <= uwg_cc_cap_bytes(peer) || cc->tokens < 0))
			break;
		skb = uwg_cc_pop_locked(cc);
		cc->tokens -= (s64)skb->len;
		__skb_queue_tail(&tx, skb);
	}

	if (cc->busy) {
		skb = uwg_cc_peek_locked(cc);
		if (skb)
			uwg_cc_arm_locked(peer, cc->tokens < (s64)skb->len ?
						(s64)skb->len - cc->tokens : 1);
	}
	spin_unlock(&cc->lock);

	while ((skb = __skb_dequeue(&tx)) != NULL) {
		if (!PACKET_CB(skb)->no_account) {
			cc->sent_packets++;
			cc->sent_bytes += skb->len;
		}
		wg_socket_send_skb_to_peer(peer, skb, PACKET_CB(skb)->ds);
	}

	wg_peer_put(peer);
	return HRTIMER_NORESTART;
}

void uwg_cc_init(struct uwg_cc *cc)
{
	memset(cc, 0, sizeof(*cc));
	spin_lock_init(&cc->lock);
	hrtimer_setup(&cc->timer, uwg_cc_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_SOFT);
	uwg_cc_fb_init(cc);
	cc->last = ktime_get();
}

static void uwg_cc_purge_locked(struct wg_peer *peer)
{
	struct uwg_cc *cc = &peer->cc;
	struct sk_buff *skb;

	while ((skb = uwg_cc_pop_locked(cc)) != NULL)
		kfree_skb(skb);
}

void uwg_cc_purge(struct wg_peer *peer)
{
	struct uwg_cc *cc = &peer->cc;

	hrtimer_cancel(&cc->timer);
	uwg_cc_fb_stop(peer);
	spin_lock_bh(&cc->lock);
	cc->armed = false;
	uwg_cc_purge_locked(peer);
	spin_unlock_bh(&cc->lock);
}

int uwg_cc_set(struct wg_peer *peer, enum uwg_cc_mode mode, u64 rate_bps)
{
	struct uwg_cc *cc = &peer->cc;
	struct uwg_cc_bucket *buckets = NULL;
	enum uwg_cc_mode old = READ_ONCE(cc->mode);

	if (mode > UWG_CC_BBR)
		return -EINVAL;
	if (mode != UWG_CC_NONE && !rate_bps) {
		if (mode == UWG_CC_BBR)
			rate_bps = UWG_BBR_MIN_RATE_BPS;
		else
			return -EINVAL;
	}

	if (mode != UWG_CC_NONE) {
		buckets = kcalloc(UWG_CC_BUCKETS, sizeof(*buckets), GFP_KERNEL);
		if (!buckets)
			return -ENOMEM;
		for (int i = 0; i < UWG_CC_BUCKETS; ++i)
			__skb_queue_head_init(&buckets[i].queue);
	}

	hrtimer_cancel(&cc->timer);
	if (old == UWG_CC_BBR && mode != UWG_CC_BBR)
		uwg_cc_fb_stop(peer);
	spin_lock_bh(&cc->lock);
	cc->armed = false;
	uwg_cc_purge_locked(peer);
	kfree(cc->buckets);
	cc->buckets = buckets;
	cc->busy = 0;
	cc->qlen = 0;
	cc->cursor = 0;
	cc->tokens = 0;
	cc->last = ktime_get();
	WRITE_ONCE(cc->mode, mode);
	if (mode == UWG_CC_BBR) {
		cc->sent_packets = 0;
		cc->sent_bytes = 0;
		cc->dropped_packets = 0;
		cc->fb.rx_bytes = 0;
		cc->fb.rx_packets = 0;
		cc->fb.rx_highest = 0;
		uwg_cc_bbr_reset(cc, rate_bps);
		WRITE_ONCE(cc->rate_bps, cc->bbr.pacing_bps);
	} else {
		WRITE_ONCE(cc->rate_bps, mode == UWG_CC_NONE ? 0 : rate_bps);
	}
	spin_unlock_bh(&cc->lock);

	if (mode == UWG_CC_BBR)
		uwg_cc_fb_start(peer);

	if (old == UWG_CC_NONE && mode != UWG_CC_NONE)
		static_branch_inc(&uwg_cc_active);
	else if (old != UWG_CC_NONE && mode == UWG_CC_NONE)
		static_branch_dec(&uwg_cc_active);

	if (mode == UWG_CC_BBR && old != UWG_CC_BBR)
		static_branch_inc(&uwg_cc_bbr_active);
	else if (mode != UWG_CC_BBR && old == UWG_CC_BBR)
		static_branch_dec(&uwg_cc_bbr_active);

	return 0;
}

void uwg_cc_clear(struct wg_peer *peer)
{
	struct uwg_cc *cc = &peer->cc;
	enum uwg_cc_mode old = READ_ONCE(cc->mode);

	hrtimer_try_to_cancel(&cc->timer);
	uwg_cc_fb_stop(peer);
	spin_lock_bh(&cc->lock);
	cc->armed = false;
	WRITE_ONCE(cc->mode, UWG_CC_NONE);
	WRITE_ONCE(cc->rate_bps, 0);
	uwg_cc_purge_locked(peer);
	kfree(cc->buckets);
	cc->buckets = NULL;
	spin_unlock_bh(&cc->lock);

	if (old != UWG_CC_NONE)
		static_branch_dec(&uwg_cc_active);
	if (old == UWG_CC_BBR)
		static_branch_dec(&uwg_cc_bbr_active);
}

bool uwg_cc_tx(struct wg_peer *peer, struct sk_buff *skb)
{
	struct uwg_cc *cc = &peer->cc;
	bool send_now = false;
	unsigned int idx;

	spin_lock_bh(&cc->lock);
	if (unlikely(PACKET_CB(skb)->is_service)) {
		spin_unlock_bh(&cc->lock);
		wg_socket_send_skb_to_peer(peer, skb, PACKET_CB(skb)->ds);
		return true;
	}
	uwg_cc_refill(peer, ktime_get());

	if (unlikely(READ_ONCE(cc->mode) == UWG_CC_BBR))
		uwg_cc_fb_kick_locked(peer);

	if (!cc->qlen && (cc->tokens >= (s64)skb->len ||
			 (skb->len > uwg_cc_cap_bytes(peer) && cc->tokens >= 0))) {
		cc->tokens -= (s64)skb->len;
		send_now = true;
	} else if (cc->qlen >= UWG_CC_MAX_QUEUED) {
		spin_unlock_bh(&cc->lock);
		cc->dropped_packets++;
		DEV_STATS_INC(peer->device->dev, tx_dropped);
		kfree_skb(skb);
		return true;
	} else {
		idx = uwg_cc_bucket_index(skb);
		__skb_queue_tail(&cc->buckets[idx].queue, skb);
		cc->busy |= BIT(idx);
		cc->qlen++;
		uwg_cc_arm_locked(peer, cc->tokens < (s64)skb->len ?
					(s64)skb->len - cc->tokens : 1);
	}
	spin_unlock_bh(&cc->lock);

	if (send_now) {
		if (!PACKET_CB(skb)->no_account) {
			cc->sent_packets++;
			cc->sent_bytes += skb->len;
		}
		wg_socket_send_skb_to_peer(peer, skb, PACKET_CB(skb)->ds);
	}

	return true;
}

static struct dentry *uwg_cc_debugfs_root;
static DEFINE_MUTEX(uwg_cc_dfs_lock);
static bool uwg_cc_dfs_up;
static atomic_t uwg_cc_dev_index = ATOMIC_INIT(0);

unsigned int uwg_cc_next_index(void)
{
	return (unsigned int)atomic_inc_return(&uwg_cc_dev_index);
}

static int uwg_cc_apply(struct wg_device *wg, u64 id, bool all,
			enum uwg_cc_mode mode, u64 bps)
{
	struct wg_peer *peer;
	int ret = 0;

	mutex_lock(&wg->device_update_lock);
	list_for_each_entry(peer, &wg->peer_list, peer_list) {
		if (!all && peer->internal_id != id)
			continue;
		ret = uwg_cc_set(peer, mode, bps);
		if (ret)
			break;
	}
	mutex_unlock(&wg->device_update_lock);
	return ret;
}

static int uwg_fec_apply(struct wg_device *wg, u64 id, bool all,
			 unsigned int k, unsigned int m)
{
	struct wg_peer *peer;
	int ret = 0;

	mutex_lock(&wg->device_update_lock);
	list_for_each_entry(peer, &wg->peer_list, peer_list) {
		if (!all && peer->internal_id != id)
			continue;
		ret = uwg_fec_set(peer, k, m);
		if (ret)
			break;
	}
	mutex_unlock(&wg->device_update_lock);
	return ret;
}

static ssize_t uwg_cc_debugfs_write(struct file *file,
				    const char __user *user_buf,
				    size_t count, loff_t *ppos)
{
	struct wg_device *wg = netdev_priv(file_inode(file)->i_private);
	char buf[64], cmd[16] = {};
	u64 id = 0, a = 0, b = 0;
	enum uwg_cc_mode mode;
	bool all;
	int n;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;
	buf[count] = 0;

	all = !strncmp(buf, "all", 3);
	if (all)
		n = sscanf(buf, "%*s %15s %llu %llu", cmd, &a, &b);
	else
		n = sscanf(buf, "%llu %15s %llu %llu", &id, cmd, &a, &b);
	if (n < 1)
		return -EINVAL;

	if (!strcmp(cmd, "fec")) {
		n = uwg_fec_apply(wg, id, all, (unsigned int)a,
				  (unsigned int)b);
		return n < 0 ? n : count;
	}
	if (!strcmp(cmd, "off")) {
		n = uwg_fec_apply(wg, id, all, 0, 0);
		if (n < 0)
			return n;
		n = uwg_cc_apply(wg, id, all, UWG_CC_NONE, 0);
		return n < 0 ? n : count;
	}

	if (!strcmp(cmd, "none"))
		mode = UWG_CC_NONE;
	else if (!strcmp(cmd, "brutal"))
		mode = UWG_CC_BRUTAL;
	else if (!strcmp(cmd, "bbr"))
		mode = UWG_CC_BBR;
	else
		return -EINVAL;

	n = uwg_cc_apply(wg, id, all, mode, a * 1000ULL * 1000ULL);
	if (n < 0)
		return n;
	return count;
}

static int uwg_cc_show(struct seq_file *seq, void *offset)
{
	struct net_device *dev = seq->private;
	struct wg_device *wg = netdev_priv(dev);
	struct wg_peer *peer;

	seq_printf(seq, "netdev %s\n", dev->name);
	seq_printf(seq, "netns %u\n", dev_net(dev)->ns.inum);

	mutex_lock(&wg->device_update_lock);
	list_for_each_entry(peer, &wg->peer_list, peer_list) {
		struct uwg_cc *cc = &peer->cc;

		seq_printf(seq, "peer %llu %s %llu fec=%u/%u\n",
			   peer->internal_id,
			   uwg_cc_mode_name(READ_ONCE(cc->mode)),
			   div_u64(READ_ONCE(cc->rate_bps), 1000ULL * 1000ULL),
			   peer->tunnel.fec_k, peer->tunnel.fec_m);
		if (peer->tunnel.fec_k)
			seq_printf(seq,
				   "  fec: sent=%lld rx=%lld rec=%lld recovered=%lld fail=%lld reinject=%lld/%lld/%lld calls=%lld skip=%lld groups=%lld drop=%lld\n",
				   atomic64_read(&peer->tunnel.repairs_sent),
				   atomic64_read(&peer->tunnel.repairs_rx),
				   atomic64_read(&peer->tunnel.records),
				   atomic64_read(&peer->tunnel.recovered),
				   atomic64_read(&peer->tunnel.recover_fail),
				   atomic64_read(&peer->tunnel.reinject_delivered),
				   atomic64_read(&peer->tunnel.reinject_decrypt_fail),
				   atomic64_read(&peer->tunnel.reinject_replay_fail),
				   atomic64_read(&peer->tunnel.tx_calls),
				   atomic64_read(&peer->tunnel.tx_skip),
				   atomic64_read(&peer->tunnel.tx_groups),
				   atomic64_read(&peer->tunnel.tx_dropped));
		if (READ_ONCE(cc->mode) != UWG_CC_BBR)
			continue;
		seq_printf(seq,
			   "  bbr: phase=%s pace_bps=%llu bw_bps=%llu full_bw_bps=%llu loss=%u%% min_rtt_us=%llu rtt_us=%llu req=%lld/%lld fb=%lld/%lld rx=%llu/%llu qlen=%u\n",
			   uwg_cc_phase_name(cc->bbr.phase),
			   cc->bbr.pacing_bps, cc->bbr.bw_max_bps,
			   cc->bbr.full_bw_bps, cc->bbr.loss_pct,
			   div_u64(cc->bbr.min_rtt_ns, 1000),
			   div_u64(cc->bbr.last_rtt_ns, 1000),
			   atomic64_read(&cc->fb.requests_sent),
			   atomic64_read(&cc->fb.requests_rx),
			   atomic64_read(&cc->fb.feedback_sent),
			   atomic64_read(&cc->fb.feedback_rx),
			   cc->fb.rx_bytes, cc->fb.rx_packets,
			   READ_ONCE(cc->qlen));
		seq_printf(seq, "  pacer: sent=%llu/%llu drop=%llu tokens=%lld\n",
			   cc->sent_packets, cc->sent_bytes,
			   cc->dropped_packets, cc->tokens);
	}
	mutex_unlock(&wg->device_update_lock);
	return 0;
}

static int uwg_cc_debugfs_open(struct inode *inode, struct file *file)
{
	int ret;

	dev_hold(inode->i_private);
	ret = single_open(file, uwg_cc_show, inode->i_private);
	if (ret)
		dev_put(inode->i_private);
	return ret;
}

static int uwg_cc_debugfs_release(struct inode *inode, struct file *file)
{
	dev_put(inode->i_private);
	return single_release(inode, file);
}

static const struct file_operations uwg_cc_debugfs_fops = {
	.open		= uwg_cc_debugfs_open,
	.read		= seq_read,
	.write		= uwg_cc_debugfs_write,
	.llseek		= seq_lseek,
	.release	= uwg_cc_debugfs_release,
};

int uwg_cc_debugfs_init(void)
{
	uwg_cc_debugfs_root = debugfs_create_dir("umbrawg", NULL);
	if (IS_ERR(uwg_cc_debugfs_root)) {
		uwg_cc_debugfs_root = NULL;
		return 0;
	}
	if (uwg_cc_debugfs_root)
		uwg_cc_dfs_up = true;
	return 0;
}

void uwg_cc_debugfs_uninit(void)
{
	mutex_lock(&uwg_cc_dfs_lock);
	uwg_cc_dfs_up = false;
	debugfs_remove_recursive(uwg_cc_debugfs_root);
	uwg_cc_debugfs_root = NULL;
	mutex_unlock(&uwg_cc_dfs_lock);
}

void uwg_cc_debugfs_add_device(struct wg_device *wg)
{
	char name[32];

	mutex_lock(&uwg_cc_dfs_lock);
	if (uwg_cc_dfs_up) {
		snprintf(name, sizeof(name), "dev%u", wg->cc_index);
		wg->cc_dentry = debugfs_create_file(name, 0600,
						    uwg_cc_debugfs_root,
						    wg->dev,
						    &uwg_cc_debugfs_fops);
		if (IS_ERR(wg->cc_dentry))
			wg->cc_dentry = NULL;
	}
	mutex_unlock(&uwg_cc_dfs_lock);
}

void uwg_cc_debugfs_remove_device(struct wg_device *wg)
{
	struct dentry *dentry;

	mutex_lock(&uwg_cc_dfs_lock);
	dentry = wg->cc_dentry;
	wg->cc_dentry = NULL;
	if (uwg_cc_dfs_up && dentry)
		debugfs_remove(dentry);
	mutex_unlock(&uwg_cc_dfs_lock);
}
