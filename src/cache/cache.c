/* cache.c - direct-mapped cache of checksum-verified pages. */
#include <stdlib.h>
#include <string.h>

#include "cache.h"

struct slot {
    uint32_t id;
    int used;
    bt_page page;
};

struct bt_cache {
    struct slot slot[BT_CACHE_SLOTS];
    uint64_t hits;
    uint64_t misses;
};

struct bt_cache *cache_new(void)
{
    return calloc(1, sizeof(struct bt_cache));
}

void cache_free(struct bt_cache *c)
{
    free(c);
}

int cache_read(bt_db *db, uint32_t id, bt_page *out)
{
    struct bt_cache *c = db->cache;
    struct slot *s;
    int rc;

    if (!c || id < BT_FIRST_DATA_PAGE)
        return pg_read(db, id, out);
    s = &c->slot[id % BT_CACHE_SLOTS];
    if (s->used && s->id == id) {
        c->hits++;
        *out = s->page;
        return 0;
    }
    c->misses++;
    rc = pg_read(db, id, out);
    if (rc) {
        s->used = 0;
        return rc;
    }
    s->id = id;
    s->page = *out;
    s->used = 1;
    return 0;
}

void cache_counters(const struct bt_cache *c, uint64_t *hits, uint64_t *misses)
{
    *hits = c ? c->hits : 0;
    *misses = c ? c->misses : 0;
}
