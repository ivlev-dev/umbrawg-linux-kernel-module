#include "header_protection.h"
#include "device.h"

#include <linux/slab.h>
#include <linux/unaligned.h>

bool uwg_header_protection_enabled(struct header_protection *p)
{
	return rcu_access_pointer(p->key) != NULL;
}

bool uwg_header_protection_init(struct chacha_state *state,
				struct wg_device *dev, const u8 *nonce)
{
	struct header_protection *p = &dev->header_protection;
	struct uwg_hp_key *key;
	u8 iv[16];
	bool res = false;

	memset(iv, 0, 4);
	memcpy(iv + 4, nonce, HEADER_PROTECTION_NONCE_SIZE);

	rcu_read_lock();
	key = rcu_dereference(p->key);
	if (key) {
		chacha_init(state, key->key, iv);
		res = true;
	}
	rcu_read_unlock();

	return res;
}

int uwg_header_protection_set_key(struct header_protection *p, const u8 *key)
{
	struct uwg_hp_key *new, *old;
	int i;

	new = kmalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return -ENOMEM;

	for (i = 0; i < CHACHA_KEY_WORDS; ++i)
		new->key[i] = get_unaligned_le32(key + i * sizeof(u32));

	mutex_lock(&p->update_lock);
	old = rcu_dereference_protected(p->key,
					lockdep_is_held(&p->update_lock));
	rcu_assign_pointer(p->key, new);
	mutex_unlock(&p->update_lock);

	if (old)
		kfree_rcu(old, rcu);

	return 0;
}

void uwg_header_protection_get_key(struct header_protection *p, u8 *key)
{
	struct uwg_hp_key *hp;
	int i;

	rcu_read_lock();
	hp = rcu_dereference(p->key);
	if (hp) {
		for (i = 0; i < CHACHA_KEY_WORDS; ++i)
			put_unaligned_le32(hp->key[i], key + i * sizeof(u32));
	} else {
		memset(key, 0, HEADER_PROTECTION_KEY_SIZE);
	}
	rcu_read_unlock();
}

void uwg_header_protection_clear(struct header_protection *p)
{
	struct uwg_hp_key *old;

	mutex_lock(&p->update_lock);
	old = rcu_dereference_protected(p->key,
					lockdep_is_held(&p->update_lock));
	rcu_assign_pointer(p->key, NULL);
	mutex_unlock(&p->update_lock);

	if (old)
		kfree_rcu(old, rcu);
}
