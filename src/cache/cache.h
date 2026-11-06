/* cache.h - small verified-page cache for read paths.
 *
 * Pages reachable from a committed root are immutable, so a slot keyed by
 * page id can never go stale. Direct mapped, 64 slots, CRC is checked once
 * when the page is loaded.
 */
#ifndef BT_CACHE_H
#define BT_CACHE_H

#include "cowbt.h"

#define BT_CACHE_SLOTS 64u

struct bt_cache;

struct bt_cache *cache_new(void);
void cache_free(struct bt_cache *c);
int cache_read(bt_db *db, uint32_t id, bt_page *out);
void cache_counters(const struct bt_cache *c, uint64_t *hits, uint64_t *misses);

#endif
