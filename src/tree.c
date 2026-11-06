/* tree.c - copy-on-write B-tree, superblock alternation and recovery.
 *
 * Write ordering per commit:
 *   pwrite(new pages) -> fsync(file) -> pwrite(super, other slot) -> fsync(file)
 * Nothing reachable from the previous committed root is touched, so a
 * crash either keeps the old generation or exposes the new one.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdio.h>

#include "cowbt.h"

#define SUPER_MAGIC_OFF 8u
#define SUPER_VER_OFF 12u
#define SUPER_GEN_OFF 16u
#define SUPER_ROOT_OFF 20u
#define SUPER_PAGE_OFF 24u

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(unsigned char *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

/* ---- superblock ---- */

static int super_write(bt_db *db, uint32_t gen, uint32_t root)
{
    bt_page p;

    pg_init(&p, BT_SUPER);
    wr32(p.raw + SUPER_MAGIC_OFF, BT_MAGIC);
    wr32(p.raw + SUPER_VER_OFF, BT_VERSION);
    wr32(p.raw + SUPER_GEN_OFF, gen);
    wr32(p.raw + SUPER_ROOT_OFF, root);
    wr32(p.raw + SUPER_PAGE_OFF, BT_PAGE_SIZE);
    return pg_write(db, gen % BT_SUPER_SLOTS, &p);
}

static int super_read(bt_db *db, uint32_t slot, uint32_t *gen, uint32_t *root)
{
    bt_page p;
    int rc = pg_read(db, slot, &p);

    if (rc)
        return rc;
    if (pg_kind(&p) != BT_SUPER)
        return -BT_ECORRUPT;
    if (rd32(p.raw + SUPER_MAGIC_OFF) != BT_MAGIC)
        return -BT_ECORRUPT;
    if (rd32(p.raw + SUPER_VER_OFF) != BT_VERSION)
        return -BT_ECORRUPT;
    if (rd32(p.raw + SUPER_PAGE_OFF) != BT_PAGE_SIZE)
        return -BT_ECORRUPT;
    *gen = rd32(p.raw + SUPER_GEN_OFF);
    *root = rd32(p.raw + SUPER_ROOT_OFF);
    if (*gen == 0)
        return -BT_ECORRUPT;
    return 0;
}

/* ---- open / init ---- */

int bt_commit(bt_db *db);

static int db_init(bt_db *db)
{
    bt_page root;
    int rc;

    pg_init(&root, BT_LEAF);
    rc = pg_write(db, BT_FIRST_DATA_PAGE, &root);
    if (rc)
        return rc;
    db->root = BT_FIRST_DATA_PAGE;
    db->generation = 1;
    db->slot = 1;
    db->high_water = BT_FIRST_DATA_PAGE + 1;
    if (bt_sync(db))
        return -BT_EIO;
    (void)fsync(db->fd);
    rc = super_write(db, 1, BT_FIRST_DATA_PAGE);
    if (rc)
        return rc;
    if (bt_sync(db))
        return -BT_EIO;
    return 0;
}

int bt_open(bt_db *db, const char *path, int writable)
{
    struct stat st;
    uint32_t gens[BT_SUPER_SLOTS], roots[BT_SUPER_SLOTS];
    int valid[BT_SUPER_SLOTS];
    int rc;

    memset(db, 0, sizeof *db);
    db->fd = open(path, (writable ? O_RDWR | O_CREAT : O_RDONLY), 0644);
    if (db->fd < 0)
        return -errno;
    if (flock(db->fd, (writable ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0) {
        rc = -errno;
        close(db->fd);
        return rc;
    }
    if (fstat(db->fd, &st) != 0) {
        rc = -errno;
        close(db->fd);
        return rc;
    }
    if (st.st_size == 0) {
        if (!writable) {
            close(db->fd);
            return -BT_EIO;
        }
        db->writable = 1;
        rc = db_init(db);
        if (rc) {
            close(db->fd);
            return rc;
        }
        return 0;
    }
    if (st.st_size < (off_t)(BT_FIRST_DATA_PAGE + 1) * BT_PAGE_SIZE ||
        st.st_size % BT_PAGE_SIZE != 0) {
        close(db->fd);
        return -BT_EIO;
    }
    db->writable = writable;
    db->high_water = (uint32_t)(st.st_size / BT_PAGE_SIZE);

    for (uint32_t s = 0; s < BT_SUPER_SLOTS; s++) {
        uint32_t g = 0, r = 0;
        valid[s] = super_read(db, s, &g, &r) == 0;
        gens[s] = g;
        roots[s] = r;
        if (valid[s] && (r < BT_FIRST_DATA_PAGE || r >= db->high_water))
            valid[s] = 0;
    }
    /* Highest surviving generation first; a torn generation fails verify
     * and the next one is tried. */
    for (uint32_t attempt = 0; attempt < BT_SUPER_SLOTS; attempt++) {
        int best = -1;
        for (uint32_t s = 0; s < BT_SUPER_SLOTS; s++) {
            if (!valid[s])
                continue;
            if (best < 0 || gens[s] > gens[best])
                best = (int)s;
        }
        if (best < 0)
            break;
        valid[best] = 0;
        db->root = roots[best];
        db->generation = gens[best];
        if (bt_verify(db) == 0) {
            /* Drop pages left behind by interrupted commits. */
            if (writable && ftruncate(db->fd, (off_t)db->high_water * BT_PAGE_SIZE))
                db->writable = 0;
            return 0;
        }
    }
    close(db->fd);
    return -BT_ECORRUPT;
}

int bt_close(bt_db *db)
{
    int rc = 0;

    if (db->fd >= 0) {
        if (close(db->fd) != 0)
            rc = -errno;
        db->fd = -1;
    }
    return rc;
}

/* ---- insert ---- */

struct ins {
    uint32_t id;   /* new copy of this node */
    int split;
    uint32_t sep;  /* first key of the right sibling */
    uint32_t right;
};

static int find_leaf_pos(const bt_page *p, uint32_t key, uint16_t *pos)
{
    uint16_t n = pg_nkeys(p), lo = 0, hi = n;

    while (lo < hi) {
        uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);
        if (pg_leaf_key(p, mid) < key)
            lo = (uint16_t)(mid + 1);
        else
            hi = mid;
    }
    *pos = lo;
    return lo < n && pg_leaf_key(p, lo) == key ? BT_EEXIST : BT_OK;
}

static uint16_t find_child(const bt_page *p, uint32_t key)
{
    uint16_t n = pg_nkeys(p), lo = 0, hi = n;

    while (lo < hi) {
        uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);
        if (pg_sep(p, mid) <= key)
            lo = (uint16_t)(mid + 1);
        else
            hi = mid;
    }
    return lo; /* first separator > key */
}

static int insert_rec(bt_db *db, uint32_t id, uint32_t key, uint32_t val, struct ins *out,
                      int depth)
{
    bt_page old, left, right;
    int rc;

    rc = pg_read(db, id, &old);
    if (rc) {
        fprintf(stderr, "read %u: %d\n", id, rc);
        return rc;
    }
    memset(out, 0, sizeof *out);

    if (pg_kind(&old) == BT_LEAF) {
        uint16_t n = pg_nkeys(&old), pos, i;
        int found = find_leaf_pos(&old, key, &pos);

        pg_init(&left, BT_LEAF);
        if (found == BT_EEXIST) {
            for (i = 0; i < n; i++)
                pg_leaf_set(&left, i, pg_leaf_key(&old, i), pg_leaf_val(&old, i));
            pg_leaf_set(&left, pos, key, val);
            pg_set_nkeys(&left, n);
            out->id = pg_alloc(db);
            return pg_write(db, out->id, &left);
        }
        if (n < BT_MAX_KEYS) {
            for (i = 0; i < pos; i++)
                pg_leaf_set(&left, i, pg_leaf_key(&old, i), pg_leaf_val(&old, i));
            pg_leaf_set(&left, pos, key, val);
            for (i = pos; i < n; i++)
                pg_leaf_set(&left, (uint16_t)(i + 1), pg_leaf_key(&old, i),
                            pg_leaf_val(&old, i));
            pg_set_nkeys(&left, (uint16_t)(n + 1));
            out->id = pg_alloc(db);
            return pg_write(db, out->id, &left);
        }
        /* Split: left gets MAX_KEYS/2 cells, right gets the rest. */
        {
            uint16_t total = (uint16_t)(n + 1), mid = (uint16_t)(total / 2);
            pg_init(&right, BT_LEAF);
            for (i = 0; i < total; i++) {
                uint32_t k = i < pos ? pg_leaf_key(&old, i)
                            : i == pos ? key
                            : pg_leaf_key(&old, (uint16_t)(i - 1));
                uint32_t v = i < pos ? pg_leaf_val(&old, i)
                            : i == pos ? val
                            : pg_leaf_val(&old, (uint16_t)(i - 1));
                if (i < mid)
                    pg_leaf_set(&left, i, k, v);
                else
                    pg_leaf_set(&right, (uint16_t)(i - mid), k, v);
            }
            pg_set_nkeys(&left, mid);
            pg_set_nkeys(&right, (uint16_t)(total - mid));
            out->id = pg_alloc(db);
            out->right = pg_alloc(db);
            rc = pg_write(db, out->id, &left);
            if (!rc)
                rc = pg_write(db, out->right, &right);
            if (rc)
                return rc;
            out->split = 1;
            out->sep = pg_leaf_key(&right, 0);
            return 0;
        }
    }

    if (pg_kind(&old) != BT_INTERNAL)
        return -BT_ECORRUPT;
    {
        uint16_t n = pg_nkeys(&old), pos = find_child(&old, key), i;
        uint32_t kids[BT_MAX_KEYS + 2];
        struct ins sub;

        rc = insert_rec(db, pg_child(&old, pos), key, val, &sub, depth + 1);
        if (rc)
            return rc;
        if (n > BT_MAX_KEYS)
            return -BT_ECORRUPT;
        pg_init(&left, BT_INTERNAL);

        if (!sub.split) {
            for (i = 0; i <= n; i++)
                pg_set_child(&left, i, i == pos ? sub.id : pg_child(&old, i));
            for (i = 0; i < n; i++)
                pg_set_sep(&left, i, pg_sep(&old, i));
            pg_set_nkeys(&left, n);
            out->id = pg_alloc(db);
            return pg_write(db, out->id, &left);
        }
        if (n < BT_MAX_KEYS) {
            uint16_t total = (uint16_t)(n + 1);
            for (i = 0; i <= total; i++) {
                if (i == pos)
                    kids[i] = sub.id;
                else if (i == pos + 1)
                    kids[i] = sub.right;
                else if (i < pos)
                    kids[i] = pg_child(&old, i);
                else
                    kids[i] = pg_child(&old, (uint16_t)(i - 1));
            }
            for (i = 0; i < total; i++)
                pg_set_sep(&left, i, i < pos ? pg_sep(&old, i)
                                  : i == pos ? sub.sep : pg_sep(&old, (uint16_t)(i - 1)));
            for (i = 0; i <= total; i++)
                pg_set_child(&left, i, kids[i]);
            pg_set_nkeys(&left, total);
            out->id = pg_alloc(db);
            return pg_write(db, out->id, &left);
        }
        /* Overflow: promote the middle key and split into two nodes. */
        {
            uint16_t total = (uint16_t)(n + 1), mid = (uint16_t)(total / 2);

            for (i = 0; i <= total; i++) {
                if (i == pos)
                    kids[i] = sub.id;
                else if (i == pos + 1)
                    kids[i] = sub.right;
                else if (i < pos)
                    kids[i] = pg_child(&old, i);
                else
                    kids[i] = pg_child(&old, (uint16_t)(i - 1));
            }
            pg_init(&right, BT_INTERNAL);
            for (i = 0; i < total; i++) {
                uint32_t k = i < pos ? pg_sep(&old, i)
                            : i == pos ? sub.sep
                            : pg_sep(&old, (uint16_t)(i - 1));
                if (i < mid)
                    pg_set_sep(&left, i, k);
                else if (i > mid)
                    pg_set_sep(&right, (uint16_t)(i - mid - 1), k);
                else
                    out->sep = k;
            }
            for (i = 0; i <= mid; i++)
                pg_set_child(&left, i, kids[i]);
            for (i = mid + 1; i <= total; i++)
                pg_set_child(&right, (uint16_t)(i - mid - 1), kids[i]);
            pg_set_nkeys(&left, mid);
            pg_set_nkeys(&right, (uint16_t)(total - mid - 1));
            if (pg_nkeys(&right) == 0 || pg_nkeys(&left) > BT_MAX_KEYS)
                return -BT_ECORRUPT;
            out->id = pg_alloc(db);
            out->right = pg_alloc(db);
            rc = pg_write(db, out->id, &left);
            if (!rc)
                rc = pg_write(db, out->right, &right);
            if (rc)
                return rc;
            out->split = 1;
            return 0;
        }
    }
}

int bt_put(bt_db *db, uint32_t key, uint32_t val)
{
    struct ins ins;
    int rc;

    if (!db->writable)
        return -EROFS;
    rc = insert_rec(db, db->root, key, val, &ins, 0);
    if (rc)
        return rc;
    if (ins.split) {
        bt_page root;
        pg_init(&root, BT_INTERNAL);
        pg_set_child(&root, 0, ins.id);
        pg_set_sep(&root, 0, ins.sep);
        pg_set_child(&root, 1, ins.right);
        pg_set_nkeys(&root, 1);
        db->root = pg_alloc(db);
        rc = pg_write(db, db->root, &root);
        if (rc)
            return rc;
    } else {
        db->root = ins.id;
    }
    return 0;
}

/* ---- lookup ---- */

int bt_get(bt_db *db, uint32_t key, uint32_t *val)
{
    bt_page p;
    uint32_t id = db->root;
    int rc;

    for (int depth = 0; depth <= (int)BT_MAX_DEPTH; depth++) {
        rc = pg_read(db, id, &p);
        if (rc)
            return rc;
        if (pg_kind(&p) == BT_LEAF) {
            uint16_t n = pg_nkeys(&p), lo = 0, hi = n;
            while (lo < hi) {
                uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);
                if (pg_leaf_key(&p, mid) < key)
                    lo = (uint16_t)(mid + 1);
                else
                    hi = mid;
            }
            if (lo < n && pg_leaf_key(&p, lo) == key) {
                *val = pg_leaf_val(&p, lo);
                return BT_OK;
            }
            return BT_NOTFOUND;
        }
        if (pg_kind(&p) != BT_INTERNAL)
            return -BT_ECORRUPT;
        id = pg_child(&p, find_child(&p, key));
    }
    return -BT_ECORRUPT;
}

/* ---- verify ---- */

struct vctx {
    uint32_t prev;
    uint32_t max_id;
    int have_prev;
    int max_depth;
};

static int verify_rec(bt_db *db, uint32_t id, uint32_t lo, uint32_t hi,
                      int depth, struct vctx *ctx)
{
    bt_page p;
    int rc;

    if (depth > (int)BT_MAX_DEPTH || id < BT_FIRST_DATA_PAGE || id >= db->high_water) {
        fprintf(stderr, "BAD ID %u depth=%d hw=%u\n", id, depth, db->high_water);
        return -BT_ECORRUPT;
    }
    if (id > ctx->max_id)
        ctx->max_id = id;
    rc = pg_read(db, id, &p);
    if (rc)
        return rc == -BT_ECORRUPT ? rc : -BT_ECORRUPT;

    if (depth > ctx->max_depth)
        ctx->max_depth = depth;
    if (pg_kind(&p) == BT_LEAF) {
        uint16_t n = pg_nkeys(&p);
        if (depth == 0 && n == 0)
            return 0;
        if (n == 0 || n > BT_MAX_KEYS)
            return -BT_ECORRUPT;
        for (uint16_t i = 0; i < n; i++) {
            uint32_t k = pg_leaf_key(&p, i);
            if (k < lo || k >= hi) {
                fprintf(stderr, "BAD LEAF KEY %u id=%u i=%u lo=%u hi=%u n=%u\n",
                        k, id, i, lo, hi, n);
                return -BT_ECORRUPT;
            }
            if (i > 0 && k <= pg_leaf_key(&p, (uint16_t)(i - 1))) {
                fprintf(stderr, "LEAF UNSORTED id=%u i=%u\n", id, i);
                return -BT_ECORRUPT;
            }
            if (ctx->have_prev && k <= ctx->prev) {
                fprintf(stderr, "GLOBAL ORDER prev=%u k=%u id=%u\n", ctx->prev, k, id);
                return -BT_ECORRUPT;
            }
            ctx->prev = k;
            ctx->have_prev = 1;
        }
        return 0;
    }
    if (pg_kind(&p) != BT_INTERNAL)
        return -BT_ECORRUPT;
    {
        uint16_t n = pg_nkeys(&p);
        if (n > BT_MAX_KEYS)
            return -BT_ECORRUPT;
        for (uint16_t i = 0; i < n; i++) {
            uint32_t k = pg_sep(&p, i);
            if (k < lo || k >= hi) {
                fprintf(stderr, "BAD SEP %u id=%u depth=%d i=%u lo=%u hi=%u n=%u\n",
                        k, id, depth, i, lo, hi, n);
                return -BT_ECORRUPT;
            }
            if (i > 0 && k <= pg_sep(&p, (uint16_t)(i - 1))) {
                fprintf(stderr, "SEP UNSORTED id=%u i=%u\n", id, i);
                return -BT_ECORRUPT;
            }
        }
        for (uint16_t i = 0; i <= n; i++) {
            uint32_t clo = i == 0 ? lo : pg_sep(&p, (uint16_t)(i - 1));
            uint32_t chi = i == n ? hi : pg_sep(&p, i);
            rc = verify_rec(db, pg_child(&p, i), clo, chi, depth + 1, ctx);
            if (rc)
                return rc;
        }
    }
    return 0;
}

int bt_verify(bt_db *db)
{
    struct vctx ctx = {0, 0, 0, 0};
    int rc = verify_rec(db, db->root, 0, UINT32_MAX, 0, &ctx);

    if (rc)
        return rc;
    db->high_water = ctx.max_id + 1;
    return 0;
}

int bt_count(bt_db *db, uint32_t *out)
{
    bt_cursor c;
    int rc = bt_scan_init(&c, db, 0);
    uint32_t n = 0;

    if (rc)
        return rc;
    while (c.valid) {
        n++;
        rc = bt_scan_next(&c);
        if (rc)
            break;
    }
    bt_scan_free(&c);
    if (!rc)
        *out = n;
    return rc;
}

/* ---- commit ---- */

int bt_commit(bt_db *db)
{
    uint32_t gen;
    int rc;

    if (!db->writable)
        return -EROFS;
    rc = bt_sync(db);
    if (rc)
        return rc;
    gen = db->generation + 1;
    db->slot = gen % BT_SUPER_SLOTS;
    rc = super_write(db, gen, db->root);
    if (rc)
        return rc;
    rc = bt_sync(db);
    if (rc)
        return rc;
    db->generation = gen;
    return 0;
}

/* ---- cursor ---- */

int bt_scan_init(bt_cursor *c, bt_db *db, uint32_t from)
{
    bt_page p;
    int rc;

    memset(c, 0, sizeof *c);
    c->db = db;
    c->page[0] = db->root;
    c->depth = 0;

    for (;;) {
        uint32_t id = c->page[c->depth];
        rc = pg_read(db, id, &p);
        if (rc)
            return rc;
        if (pg_kind(&p) == BT_LEAF) {
            uint16_t n = pg_nkeys(&p), lo = 0, hi = n;
            while (lo < hi) {
                uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);
                if (pg_leaf_key(&p, mid) < from)
                    lo = (uint16_t)(mid + 1);
                else
                    hi = mid;
            }
            c->idx[c->depth] = (uint16_t)(lo - 1);
            return bt_scan_next(c);
        }
        if (pg_kind(&p) != BT_INTERNAL)
            return -BT_ECORRUPT;
        if (c->depth + 1 >= (int)BT_MAX_DEPTH)
            return -BT_ECORRUPT;
        uint16_t pos = find_child(&p, from);
        c->idx[c->depth] = pos;
        c->page[++c->depth] = pg_child(&p, pos);
    }
}

int bt_scan_next(bt_cursor *c)
{
    bt_page p;
    int rc;

    while (c->depth >= 0) {
        uint32_t id = c->page[c->depth];
        rc = pg_read(c->db, id, &p);
        if (rc)
            return rc;
        if (pg_kind(&p) == BT_LEAF) {
            c->idx[c->depth]++;
            if (c->idx[c->depth] < pg_nkeys(&p)) {
                c->key = pg_leaf_key(&p, c->idx[c->depth]);
                c->val = pg_leaf_val(&p, c->idx[c->depth]);
                c->valid = 1;
                return 0;
            }
            c->depth--;
            if (c->depth < 0)
                break;
            continue;
        }
        /* internal: descend into the child after the one we came from */
        if (c->idx[c->depth] + 1 > pg_nkeys(&p)) {
            c->depth--;
            continue;
        }
        if (c->depth + 1 >= (int)BT_MAX_DEPTH)
            return -BT_ECORRUPT;
        c->idx[c->depth]++;
        c->page[c->depth + 1] = pg_child(&p, c->idx[c->depth]);
        c->idx[c->depth + 1] = (uint16_t)-1;
        c->depth++;
    }
    c->valid = 0;
    return BT_OK;
}

void bt_scan_free(bt_cursor *c)
{
    c->valid = 0;
    c->depth = 0;
}
