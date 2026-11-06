/* main.c - CLI, self tests and the crash harness.
 *
 * The crash harness forks a child that commits batches, starts one more
 * batch, then truncates the file back to the last commit and exits with
 * _exit(2). Every round must reopen, verify and keep all base keys.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cache/cache.h"
#include "cowbt.h"

static const char *msgs[] = {
    [BT_OK] = "ok",
    [BT_NOTFOUND] = "not found",
    [BT_EIO] = "io error",
    [BT_ECORRUPT] = "corrupt page",
    [BT_EEXIST] = "exists",
    [BT_EFULL] = "full"
};

static const char *msg(int rc)
{
    if (rc <= 0)
        return rc == 0 ? "ok" : strerror(-rc);
    return rc < (int)(sizeof msgs / sizeof *msgs) ? msgs[rc] : "error";
}

static int fail(const char *what, int rc)
{
    fprintf(stderr, "%s: %s\n", what, msg(rc));
    return 1;
}

static unsigned long num(const char *s)
{
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    return *end ? 0 : v;
}

static off_t file_len(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_size : -1;
}

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint32_t rnd(uint32_t bound)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state % (bound ? bound : 1));
}

static int cmd_put(const char *path, uint32_t key, uint32_t val)
{
    bt_db db;
    int rc = bt_open(&db, path, 1);

    if (rc)
        return fail("open", rc);
    rc = bt_put(&db, key, val);
    if (!rc)
        rc = bt_commit(&db);
    bt_close(&db);
    return rc ? fail("put", rc) : 0;
}

static int cmd_get(const char *path, uint32_t key)
{
    bt_db db;
    uint32_t val = 0;
    int rc = bt_open(&db, path, 0);

    if (rc)
        return fail("open", rc);
    rc = bt_get(&db, key, &val);
    bt_close(&db);
    if (rc == BT_NOTFOUND)
        return 1;
    if (rc)
        return fail("get", rc);
    printf("%u\n", val);
    return 0;
}

static int cmd_scan(const char *path, uint32_t from, uint32_t limit)
{
    bt_db db;
    bt_cursor c;
    int rc = bt_open(&db, path, 0);
    uint32_t n = 0;

    if (rc)
        return fail("open", rc);
    rc = bt_scan_init(&c, &db, from);
    while (!rc && c.valid && n < limit) {
        printf("%u %u\n", c.key, c.val);
        n++;
        rc = bt_scan_next(&c);
    }
    bt_scan_free(&c);
    bt_close(&db);
    return rc ? fail("scan", rc) : 0;
}

static int cmd_verify(const char *path)
{
    bt_db db;
    uint32_t n = 0;
    int rc = bt_open(&db, path, 0);

    if (rc)
        return fail("open", rc);
    rc = bt_count(&db, &n);
    bt_close(&db);
    if (rc)
        return fail("verify", rc);
    printf("ok keys=%u\n", n);
    return 0;
}

static int cmd_stats(const char *path)
{
    bt_db db;
    uint32_t n = 0;
    int rc = bt_open(&db, path, 0);

    if (rc)
        return fail("open", rc);
    uint64_t hits = 0, misses = 0;

    rc = bt_count(&db, &n);
    cache_counters(db.cache, &hits, &misses);
    printf("generation=%u root=%u pages=%u keys=%u page_size=%u cache_hits=%llu cache_misses=%llu\n",
           db.generation, db.root, db.high_water, n, BT_PAGE_SIZE,
           (unsigned long long)hits, (unsigned long long)misses);
    bt_close(&db);
    return rc ? fail("count", rc) : 0;
}

static int seed_base(const char *path, uint32_t count, uint32_t step)
{
    bt_db db;
    int rc = bt_open(&db, path, 1);

    if (rc)
        return rc;
    for (uint32_t k = 0; k < count; k++) {
        rc = bt_put(&db, k, k * 3 + 1);
        if (rc)
            break;
        if (k % step == 0 && (rc = bt_commit(&db)) != 0)
            break;
    }
    if (!rc)
        rc = bt_commit(&db);
    bt_close(&db);
    return rc;
}

static int reopen_ok(const char *path, uint32_t expect_min)
{
    bt_db db;
    uint32_t n = 0, v = 0;
    int rc = bt_open(&db, path, 1);

    if (rc)
        return rc;
    rc = bt_count(&db, &n);
    if (!rc && n < expect_min)
        rc = BT_ECORRUPT;
    if (!rc)
        rc = bt_get(&db, 0, &v);
    if (!rc && v != 1)
        rc = BT_ECORRUPT;
    bt_close(&db);
    return rc;
}

static int cmd_crash(const char *path, int children, int rounds)
{
    unlink(path);
    int rc = seed_base(path, 400, 100);
    if (rc)
        return fail("seed", rc);
    rng_state ^= (uint64_t)time(NULL);

    for (int child = 0; child < children; child++) {
        pid_t pid = fork();
        if (pid < 0)
            return fail("fork", -errno);
        if (pid == 0) {
            bt_db db;
            off_t marks[8];
            int nmarks = 0;
            if (bt_open(&db, path, 1))
                _exit(3);
            for (int r = 0; r < rounds && nmarks < 8; r++) {
                for (int i = 0; i < 5; i++)
                    if (bt_put(&db, 100000 + rnd(4000), (uint32_t)(child * 100 + r)))
                        _exit(4);
                if (bt_commit(&db))
                    _exit(5);
                marks[nmarks++] = file_len(path);
            }
            /* Uncommitted tail, then lose it the way a power cut would. */
            for (int i = 0; i < 5; i++)
                if (bt_put(&db, 200000 + rnd(4000), (uint32_t)i))
                    _exit(6);
            if (rnd(4) != 0 && nmarks > 0) {
                if (ftruncate(db.fd, marks[nmarks - 1]))
                    _exit(7);
            }
            _exit(2);
        }
        int status = 0;
        if (waitpid(pid, &status, 0) < 0)
            return fail("waitpid", -errno);
        if (!WIFEXITED(status) || WEXITSTATUS(status) > 2)
            return fail("crash child", WEXITSTATUS(status) == 0 ? BT_ECORRUPT : BT_EIO);
        rc = reopen_ok(path, 400);
        if (rc)
            return fail("reopen after crash", rc);
        printf("round %d ok\n", child);
    }
    return 0;
}

static int cmd_selftest(const char *path)
{
    uint32_t n = 0, v = 0;
    bt_db db;
    int rc;

    unlink(path);
    rc = seed_base(path, 300, 50);
    if (rc)
        return fail("insert", rc);

    rc = bt_open(&db, path, 1);
    if (rc)
        return fail("open", rc);
    rc = bt_verify(&db);
    if (!rc)
        rc = bt_count(&db, &n);
    if (!rc && n != 300)
        rc = BT_ECORRUPT;
    if (!rc)
        rc = bt_get(&db, 137, &v);
    if (!rc && v != 137 * 3 + 1)
        rc = BT_ECORRUPT;
    /* Overwrite must replace, not duplicate. */
    if (!rc)
        rc = bt_put(&db, 137, 7);
    if (!rc)
        rc = bt_commit(&db);
    if (!rc)
        rc = bt_get(&db, 137, &v);
    if (!rc && v != 7)
        rc = BT_ECORRUPT;
    if (!rc)
        rc = bt_verify(&db);

    /* Uncommitted work disappears on reopen. */
    if (!rc)
        rc = bt_put(&db, 9999, 42);
    if (!rc) {
        bt_close(&db);
        rc = bt_open(&db, path, 0);
        if (!rc) {
            rc = bt_get(&db, 9999, &v);
            if (rc == BT_NOTFOUND)
                rc = 0;
            else if (!rc)
                rc = BT_ECORRUPT;
        }
    }
    if (rc) {
        bt_close(&db);
        return fail("durability", rc);
    }

    /* A torn page must fall back to the previous generation. */
    uint32_t torn = db.root;
    bt_close(&db);
    int fd = open(path, O_RDWR);
    if (fd < 0)
        return fail("open raw", -errno);
    unsigned char junk[16];
    memset(junk, 0xFF, sizeof junk);
    if (pwrite(fd, junk, sizeof junk, (off_t)torn * BT_PAGE_SIZE + 100) !=
        (ssize_t)sizeof junk) {
        close(fd);
        return fail("torn write", -errno);
    }
    close(fd);
    rc = reopen_ok(path, 300);
    if (rc)
        return fail("fallback after torn page", rc);

    /* Cursor range and ordering. */
    rc = bt_open(&db, path, 0);
    if (rc)
        return fail("open", rc);
    bt_cursor c;
    rc = bt_scan_init(&c, &db, 200);
    int seen = 0;
    uint32_t prev = 0;
    while (!rc && c.valid) {
        if (seen && c.key <= prev)
            rc = BT_ECORRUPT;
        prev = c.key;
        seen++;
        rc = bt_scan_next(&c);
    }
    bt_scan_free(&c);
    bt_close(&db);
    if (!rc && seen == 0)
        rc = BT_ECORRUPT;
    if (rc)
        return fail("scan", rc);

    /* Repeated lookups must hit the page cache. */
    rc = bt_open(&db, path, 0);
    if (rc)
        return fail("open", rc);
    uint64_t hits0 = 0, hits1 = 0, misses = 0;
    cache_counters(db.cache, &hits0, &misses);
    for (int i = 0; i < 3 && !rc; i++)
        rc = bt_get(&db, 42, &v);
    cache_counters(db.cache, &hits1, &misses);
    bt_close(&db);
    if (!rc && hits1 <= hits0)
        rc = BT_ECORRUPT;
    if (rc)
        return fail("cache", rc);

    printf("selftest ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return fail("usage", -EINVAL);

    if (!strcmp(argv[1], "init") && argc >= 3) {
        bt_db db;
        int rc = bt_open(&db, argv[2], 1);
        if (!rc)
            bt_close(&db);
        return rc ? fail("init", rc) : 0;
    }
    if (!strcmp(argv[1], "put") && argc >= 5)
        return cmd_put(argv[2], (uint32_t)num(argv[3]), (uint32_t)num(argv[4]));
    if (!strcmp(argv[1], "get") && argc >= 4)
        return cmd_get(argv[2], (uint32_t)num(argv[3]));
    if (!strcmp(argv[1], "scan") && argc >= 3)
        return cmd_scan(argv[2], argc > 3 ? (uint32_t)num(argv[3]) : 0,
                        argc > 4 ? (uint32_t)num(argv[4]) : 20);
    if (!strcmp(argv[1], "verify") && argc >= 3)
        return cmd_verify(argv[2]);
    if (!strcmp(argv[1], "stats") && argc >= 3)
        return cmd_stats(argv[2]);
    if (!strcmp(argv[1], "crash") && argc >= 3)
        return cmd_crash(argv[2], argc > 3 ? (int)num(argv[3]) : 6,
                         argc > 4 ? (int)num(argv[4]) : 4);
    if (!strcmp(argv[1], "fill") && argc >= 4) {
        bt_db db;
        uint32_t count = (uint32_t)num(argv[3]);
        int rc = bt_open(&db, argv[2], 1);
        if (rc)
            return fail("open", rc);
        for (uint32_t i = 0; i < count && !rc; i++) {
            rc = bt_put(&db, i, i);
            if (!rc && i % 16 == 0)
                rc = bt_commit(&db);
        }
        if (!rc)
            rc = bt_verify(&db);
        if (!rc)
            rc = bt_commit(&db);
        if (!rc)
            rc = bt_verify(&db);
        bt_close(&db);
        return rc ? fail("fill", rc) : 0;
    }
    if (!strcmp(argv[1], "selftest") && argc >= 3)
        return cmd_selftest(argv[2]);

    fprintf(stderr, "commands: init put get scan verify stats crash selftest\n");
    return 1;
}
