/* cowbt.h - copy-on-write B-tree storage engine.
 *
 * Invariants:
 *  1. No page is mutated after it becomes reachable from a committed root.
 *     A put rewrites every node on its path into fresh pages.
 *  2. The root pointer lives in one of two superblock slots. A commit
 *     fsyncs all data pages before writing the next generation into the
 *     other slot, so a crash at any point leaves at least one slot whose
 *     tree is fully durable.
 *  3. Every page carries a CRC32; a torn page makes its generation
 *     unloadable and open() falls back to the older generation.
 */
#ifndef COWBT_H
#define COWBT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define BT_PAGE_SIZE 4096u
#define BT_MAGIC 0x31425443u /* "CTB1" */
#define BT_VERSION 1u
#define BT_MAX_KEYS 32u
#define BT_MAX_DEPTH 16u
#define BT_SUPER_SLOTS 2u
#define BT_FIRST_DATA_PAGE 2u

enum bt_kind { BT_LEAF = 1, BT_INTERNAL = 2, BT_SUPER = 3 };

enum {
    BT_OK = 0,
    BT_NOTFOUND = 1,
    BT_EIO = 2,
    BT_ECORRUPT = 3,
    BT_EEXIST = 4,
    BT_EFULL = 5
};

typedef struct {
    unsigned char raw[BT_PAGE_SIZE];
} bt_page;

struct bt_cache;

typedef struct {
    int fd;
    uint32_t root;
    uint32_t generation;
    uint32_t high_water;
    uint32_t slot;
    struct bt_cache *cache;
    int writable;
} bt_db;

typedef struct {
    bt_db *db;
    uint32_t page[BT_MAX_DEPTH];
    uint16_t idx[BT_MAX_DEPTH];
    int depth;
    uint32_t key;
    uint32_t val;
    int valid;
} bt_cursor;

/* page.c */
uint32_t bt_crc32(const void *buf, size_t len);
void pg_init(bt_page *p, uint16_t kind);
uint16_t pg_kind(const bt_page *p);
uint16_t pg_nkeys(const bt_page *p);
void pg_set_nkeys(bt_page *p, uint16_t n);
uint32_t pg_leaf_key(const bt_page *p, uint16_t i);
uint32_t pg_leaf_val(const bt_page *p, uint16_t i);
void pg_leaf_set(bt_page *p, uint16_t i, uint32_t k, uint32_t v);
uint32_t pg_child(const bt_page *p, uint16_t i);
void pg_set_child(bt_page *p, uint16_t i, uint32_t id);
uint32_t pg_sep(const bt_page *p, uint16_t i);
void pg_set_sep(bt_page *p, uint16_t i, uint32_t k);
int pg_read(bt_db *db, uint32_t id, bt_page *p);
int pg_write(bt_db *db, uint32_t id, const bt_page *p);
uint32_t pg_alloc(bt_db *db);
int bt_sync(bt_db *db);

/* tree.c */
int bt_open(bt_db *db, const char *path, int writable);
int bt_close(bt_db *db);
int bt_put(bt_db *db, uint32_t key, uint32_t val);
int bt_get(bt_db *db, uint32_t key, uint32_t *val);
int bt_commit(bt_db *db);
int bt_verify(bt_db *db);
int bt_count(bt_db *db, uint32_t *out);
int bt_scan_init(bt_cursor *c, bt_db *db, uint32_t from);
int bt_scan_next(bt_cursor *c);
void bt_scan_free(bt_cursor *c);

#endif
