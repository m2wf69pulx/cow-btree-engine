/* page.c - fixed-size page encoding, checksums and positional I/O.
 *
 * Page header: crc32 (over bytes 4..end), kind, nkeys. Leaf cells are
 * (key, value) pairs at offset 8; internal pages store nkeys+1 child
 * pointers followed by nkeys separator keys.
 */
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "cowbt.h"

#define HDR 8u
#define CELL 8u

uint32_t bt_crc32(const void *buf, size_t len)
{
    static uint32_t table[256];
    static int ready;
    const unsigned char *p = buf;
    uint32_t c;

    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int b = 0; b < 8; b++)
                v = (v & 1u) ? (v >> 1) ^ 0xEDB88320u : v >> 1;
            table[i] = v;
        }
        ready = 1;
    }
    c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void put16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static uint16_t get16(const unsigned char *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void put32(unsigned char *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void pg_init(bt_page *p, uint16_t kind)
{
    memset(p->raw, 0, sizeof p->raw);
    put16(p->raw + 4, kind);
    put16(p->raw + 6, 0);
}

uint16_t pg_kind(const bt_page *p) { return get16(p->raw + 4); }
uint16_t pg_nkeys(const bt_page *p) { return get16(p->raw + 6); }
void pg_set_nkeys(bt_page *p, uint16_t n) { put16(p->raw + 6, n); }

uint32_t pg_leaf_key(const bt_page *p, uint16_t i)
{
    return get32(p->raw + HDR + (size_t)i * CELL);
}

uint32_t pg_leaf_val(const bt_page *p, uint16_t i)
{
    return get32(p->raw + HDR + (size_t)i * CELL + 4);
}

void pg_leaf_set(bt_page *p, uint16_t i, uint32_t k, uint32_t v)
{
    put32(p->raw + HDR + (size_t)i * CELL, k);
    put32(p->raw + HDR + (size_t)i * CELL + 4, v);
}

uint32_t pg_child(const bt_page *p, uint16_t i)
{
    return get32(p->raw + HDR + (size_t)i * 4);
}

void pg_set_child(bt_page *p, uint16_t i, uint32_t id)
{
    put32(p->raw + HDR + (size_t)i * 4, id);
}

uint32_t pg_sep(const bt_page *p, uint16_t i)
{
    return get32(p->raw + HDR + (size_t)(BT_MAX_KEYS + 1) * 4 + (size_t)i * 4);
}

void pg_set_sep(bt_page *p, uint16_t i, uint32_t k)
{
    put32(p->raw + HDR + (size_t)(BT_MAX_KEYS + 1) * 4 + (size_t)i * 4, k);
}

static int pread_full(int fd, void *buf, size_t len, off_t off)
{
    unsigned char *p = buf;

    while (len > 0) {
        ssize_t n = pread(fd, p, len, off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (n == 0)
            return -EIO;
        p += n;
        len -= (size_t)n;
        off += n;
    }
    return 0;
}

static int pwrite_full(int fd, const void *buf, size_t len, off_t off)
{
    const unsigned char *p = buf;

    while (len > 0) {
        ssize_t n = pwrite(fd, p, len, off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        p += n;
        len -= (size_t)n;
        off += n;
    }
    return 0;
}

int pg_read(bt_db *db, uint32_t id, bt_page *p)
{
    int rc;

    rc = pread_full(db->fd, p->raw, BT_PAGE_SIZE, (off_t)id * BT_PAGE_SIZE);
    if (rc)
        return rc;
    if (bt_crc32(p->raw + 4, BT_PAGE_SIZE - 4) != get32(p->raw))
        return -BT_ECORRUPT;
    return 0;
}

int pg_write(bt_db *db, uint32_t id, const bt_page *p)
{
    off_t off = (off_t)id * BT_PAGE_SIZE;

    if (pwrite_full(db->fd, p->raw, BT_PAGE_SIZE, off))
        return -EIO;
    put32(((bt_page *)p)->raw, bt_crc32(p->raw + 4, BT_PAGE_SIZE - 4));
    return pwrite_full(db->fd, p->raw, 4, off);
}

uint32_t pg_alloc(bt_db *db)
{
    return db->high_water++;
}

int bt_sync(bt_db *db)
{
    return fsync(db->fd) == 0 ? 0 : -errno;
}
