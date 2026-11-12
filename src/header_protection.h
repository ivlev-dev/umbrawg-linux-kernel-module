#ifndef _UWG_HEADER_PROTECTION
#define _UWG_HEADER_PROTECTION

#include "messages.h"

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <crypto/chacha.h>

struct wg_device;

enum header_protection_lengths {
	HEADER_PROTECTION_KEY_SIZE = CHACHA_KEY_SIZE,
	HEADER_PROTECTION_NONCE_SIZE = 12,
};

struct uwg_hp_key {
	u32 key[CHACHA_KEY_WORDS];
	struct rcu_head rcu;
};

struct header_protection {
	struct uwg_hp_key __rcu *key;
	struct mutex update_lock;
};

bool uwg_header_protection_enabled(struct header_protection *p);
bool uwg_header_protection_init(struct chacha_state *state,
				struct wg_device *dev, const u8 *nonce);
int uwg_header_protection_set_key(struct header_protection *p,
				  const u8 *key);
void uwg_header_protection_get_key(struct header_protection *p, u8 *key);
void uwg_header_protection_clear(struct header_protection *p);

#endif
