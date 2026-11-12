/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UWG_CC_H
#define _UWG_CC_H

#include <linux/hrtimer.h>
#include <linux/jump_label.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct wg_peer;

enum uwg_cc_mode {
	UWG_CC_NONE = 0,
	UWG_CC_BRUTAL,
	UWG_CC_BBR,
};

#define UWG_CC_BUCKETS		8
#define UWG_CC_MAX_QUEUED	1024
#define UWG_CC_TIMER_BUDGET	64
#define UWG_CC_MIN_CAP_BYTES	16384

extern struct static_key_false uwg_cc_active;

struct uwg_cc_bucket {
	struct sk_buff_head queue;
};

struct uwg_cc {
	enum uwg_cc_mode mode;
	u64 rate_bps;
	u64 tokens;
	ktime_t last;
	struct hrtimer timer;
	spinlock_t lock;
	struct uwg_cc_bucket *buckets;
	unsigned long busy;
	unsigned int qlen;
	unsigned int cursor;
	bool armed;
};

void uwg_cc_init(struct uwg_cc *cc);
int uwg_cc_set(struct wg_peer *peer, enum uwg_cc_mode mode, u64 rate_bps);
void uwg_cc_clear(struct wg_peer *peer);
void uwg_cc_purge(struct wg_peer *peer);
bool uwg_cc_tx(struct wg_peer *peer, struct sk_buff *skb);
u32 uwg_cc_flow_hash(const struct sk_buff *skb);
const char *uwg_cc_mode_name(enum uwg_cc_mode mode);

struct wg_device;
struct dentry;

unsigned int uwg_cc_next_index(void);
int uwg_cc_debugfs_init(void);
void uwg_cc_debugfs_uninit(void);
void uwg_cc_debugfs_add_device(struct wg_device *wg);
void uwg_cc_debugfs_remove_device(struct wg_device *wg);

#endif /* _UWG_CC_H */
