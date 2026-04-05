/*
 * test_branch.c — Integration tests for the branch() userspace library.
 *
 * Each test that exercises filesystem branching uses a per-test fixture that
 * mounts/unmounts a BranchFS instance in /tmp.
 *
 * Compile (from libbranch/):
 *   gcc -O2 -Wall -Wextra -D_GNU_SOURCE -Iinclude \
 *       tests/test_branch.c libbranch.a -o tests/test_branch
 *
 * Run:
 *   BRANCHFS_BIN=../target/release/branchfs ./tests/test_branch
 */

#include "branch.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ============================================================
 * Test harness
 * ============================================================ */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

/* ANSI colour codes */
#define GREEN "\033[32m"
#define RED   "\033[31m"
#define RESET "\033[0m"

#define ASSERT(cond, msg) do {                                          \
    tests_run++;                                                        \
    if (cond) {                                                         \
        tests_passed++;                                                 \
        printf("  " GREEN "✓" RESET " %s\n", (msg));                   \
    } else {                                                            \
        tests_failed++;                                                 \
        printf("  " RED "✗" RESET " %s  (at %s:%d)\n",                 \
               (msg), __FILE__, __LINE__);                              \
    }                                                                   \
} while (0)

#define ASSERT_EQ_INT(a, b, msg) do {                                   \
    int _a = (int)(a);                                                  \
    int _b = (int)(b);                                                  \
    tests_run++;                                                        \
    if (_a == _b) {                                                     \
        tests_passed++;                                                 \
        printf("  " GREEN "✓" RESET " %s\n", (msg));                   \
    } else {                                                            \
        tests_failed++;                                                 \
        printf("  " RED "✗" RESET " %s  (got %d, want %d, at %s:%d)\n",\
               (msg), _a, _b, __FILE__, __LINE__);                      \
    }                                                                   \
} while (0)

#define ASSERT_STR_EQ(a, b, msg) do {                                   \
    const char *_a = (a);                                               \
    const char *_b = (b);                                               \
    tests_run++;                                                        \
    if (_a && _b && strcmp(_a, _b) == 0) {                              \
        tests_passed++;                                                 \
        printf("  " GREEN "✓" RESET " %s\n", (msg));                   \
    } else {                                                            \
        tests_failed++;                                                 \
        printf("  " RED "✗" RESET " %s  (got \"%s\", want \"%s\","      \
               " at %s:%d)\n",                                          \
               (msg), _a ? _a : "(null)", _b ? _b : "(null)",           \
               __FILE__, __LINE__);                                     \
    }                                                                   \
} while (0)

/* ============================================================
 * Fixture
 * ============================================================ */

struct fixture {
    char base[PATH_MAX];
    char storage[PATH_MAX];
    char mnt[PATH_MAX];
    char branchfs_bin[PATH_MAX];
    int  mnt_fd;   /* open(mnt, O_RDONLY|O_DIRECTORY) */
};

static struct fixture *fixture_new(const char *name)
{
    struct fixture *f = calloc(1, sizeof(*f));
    if (!f) {
        perror("calloc fixture");
        return NULL;
    }

    /* Resolve branchfs binary */
    const char *bin = getenv("BRANCHFS_BIN");
    if (!bin || bin[0] == '\0')
        bin = "../target/release/branchfs";
    strncpy(f->branchfs_bin, bin, PATH_MAX - 1);

    pid_t pid = getpid();
    snprintf(f->base,    PATH_MAX, "/tmp/branchfs_%s_%d_base",    name, (int)pid);
    snprintf(f->storage, PATH_MAX, "/tmp/branchfs_%s_%d_storage", name, (int)pid);
    snprintf(f->mnt,     PATH_MAX, "/tmp/branchfs_%s_%d_mnt",     name, (int)pid);

    /* Clean up any leftover state from a previous failed run */
    {
        char cmd[PATH_MAX * 3 + 64];
        snprintf(cmd, sizeof(cmd), "%s unmount %s --storage %s >/dev/null 2>&1",
                 f->branchfs_bin, f->mnt, f->storage);
        int _ignored = system(cmd); (void)_ignored;
    }
    {
        char cmd[PATH_MAX + 32];
        snprintf(cmd, sizeof(cmd), "rm -rf %s %s %s", f->base, f->storage, f->mnt);
        int _ignored = system(cmd); (void)_ignored;
    }

    /* Create directories */
    if (mkdir(f->base,    0755) < 0 ||
        mkdir(f->storage, 0755) < 0 ||
        mkdir(f->mnt,     0755) < 0) {
        perror("mkdir fixture dirs");
        free(f);
        return NULL;
    }

    /* Seed the base directory */
    {
        char path[PATH_MAX];
        FILE *fp;

        snprintf(path, sizeof(path), "%s/file1.txt", f->base);
        fp = fopen(path, "w");
        if (fp) { fputs("base content\n", fp); fclose(fp); }

        snprintf(path, sizeof(path), "%s/file2.txt", f->base);
        fp = fopen(path, "w");
        if (fp) { fputs("another file\n", fp); fclose(fp); }
    }

    /* Mount BranchFS */
    {
        char cmd[PATH_MAX * 3 + 128];
        snprintf(cmd, sizeof(cmd),
                 "%s mount --base %s --storage %s %s >/dev/null 2>&1",
                 f->branchfs_bin, f->base, f->storage, f->mnt);
        int rc = system(cmd);
        if (rc != 0) {
            fprintf(stderr, "fixture_new: mount failed (rc=%d) cmd: %s\n", rc, cmd);
            free(f);
            return NULL;
        }
    }

    /* Give FUSE a moment to become ready */
    usleep(500000);

    /* Verify mount */
    {
        char ctl[PATH_MAX];
        snprintf(ctl, sizeof(ctl), "%s/.branchfs_ctl", f->mnt);
        if (access(ctl, F_OK) != 0) {
            fprintf(stderr, "fixture_new: .branchfs_ctl not found after mount\n");
            free(f);
            return NULL;
        }
    }

    /* Open the mount directory fd */
    f->mnt_fd = open(f->mnt, O_RDONLY | O_DIRECTORY);
    if (f->mnt_fd < 0) {
        perror("open mnt_fd");
        free(f);
        return NULL;
    }

    return f;
}

static void fixture_free(struct fixture *f)
{
    if (!f) return;

    if (f->mnt_fd >= 0)
        close(f->mnt_fd);

    /* Unmount */
    {
        char cmd[PATH_MAX * 2 + 64];
        snprintf(cmd, sizeof(cmd),
                 "%s unmount %s --storage %s >/dev/null 2>&1",
                 f->branchfs_bin, f->mnt, f->storage);
        int _ignored = system(cmd); (void)_ignored;
    }

    usleep(300000);

    /* Remove temp dirs */
    {
        char cmd[PATH_MAX + 32];
        snprintf(cmd, sizeof(cmd), "rm -rf %s %s %s", f->base, f->storage, f->mnt);
        int _ignored = system(cmd); (void)_ignored;
    }

    free(f);
}

/* ============================================================
 * Test runner
 * ============================================================ */

typedef void (*test_fn)(struct fixture *f);

static void run_test(const char *name, test_fn fn, struct fixture *f)
{
    printf("\n[TEST] %s\n", name);
    fn(f);
    if (tests_failed > 0) {
        /* We detect failures by counting; use a local snapshot */
    }
}

static int print_summary(void)
{
    printf("\n========================================\n");
    printf("Tests run: %d  |  Passed: " GREEN "%d" RESET
           "  |  Failed: " RED "%d" RESET "\n",
           tests_run, tests_passed, tests_failed);
    printf("========================================\n");
    return (tests_failed > 0) ? 1 : 0;
}

/* ============================================================
 * Helper: wait for all children
 * ============================================================ */
static void wait_all(void)
{
    int status;
    while (waitpid(-1, &status, 0) > 0)
        ;
}

/* ============================================================
 * Test 1: test_create_parent_returns_zero
 *   BR_FS|BR_MEMORY, n=1; assert parent gets 0 from branch(BR_CREATE).
 * ============================================================ */
static void test_create_parent_returns_zero(struct fixture *f)
{
    pid_t pids[1] = {0};
    union branch_attr attr = {
        .create = {
            .flags      = BR_FS | BR_MEMORY,
            .mount_fd   = f->mnt_fd,
            .n_branches = 1,
            .child_pids = (uintptr_t)pids,
        }
    };

    long idx = branch(BR_CREATE, &attr, sizeof(attr));

    if (idx > 0) {
        /* child — just exit */
        _exit(0);
    }

    /* parent */
    ASSERT_EQ_INT((int)idx, 0, "parent receives 0 from BR_CREATE");

    wait_all();
}

/* ============================================================
 * Test 2: test_create_child_gets_index
 *   BR_FS|BR_MEMORY, n=3; each child writes its index to a shared array.
 * ============================================================ */
static void test_create_child_gets_index(struct fixture *f)
{
    /* Shared result array: shared_results[child_index - 1] = child_index */
    int *shared_results = mmap(NULL, 3 * sizeof(int),
                               PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared_results == MAP_FAILED) {
        perror("mmap shared_results");
        ASSERT(0, "mmap shared_results should succeed");
        return;
    }
    memset(shared_results, 0, 3 * sizeof(int));

    pid_t pids[3] = {0};
    union branch_attr attr = {
        .create = {
            .flags      = BR_FS | BR_MEMORY,
            .mount_fd   = f->mnt_fd,
            .n_branches = 3,
            .child_pids = (uintptr_t)pids,
        }
    };

    long idx = branch(BR_CREATE, &attr, sizeof(attr));

    if (idx > 0) {
        /* child: record my index */
        shared_results[idx - 1] = (int)idx;
        /* abort cleanly — no commit needed for this test */
        union branch_attr a = { .abort_op = { .flags = 0 } };
        branch(BR_ABORT, &a, sizeof(a));
        /* unreachable */
        _exit(0);
    }

    /* parent */
    wait_all();

    ASSERT_EQ_INT(shared_results[0], 1, "child 1 wrote index 1");
    ASSERT_EQ_INT(shared_results[1], 2, "child 2 wrote index 2");
    ASSERT_EQ_INT(shared_results[2], 3, "child 3 wrote index 3");

    munmap(shared_results, 3 * sizeof(int));
}

/* ============================================================
 * Test 3: test_commit_file_appears_in_base
 *   BR_FS|BR_MEMORY|BR_ISOLATE, n=1; child writes branch_result_1.txt,
 *   commits. Parent checks base dir.
 * ============================================================ */
static void test_commit_file_appears_in_base(struct fixture *f)
{
    pid_t pids[1] = {0};
    union branch_attr attr = {
        .create = {
            .flags      = BR_FS | BR_MEMORY | BR_ISOLATE,
            .mount_fd   = f->mnt_fd,
            .n_branches = 1,
            .child_pids = (uintptr_t)pids,
        }
    };

    long idx = branch(BR_CREATE, &attr, sizeof(attr));

    if (idx > 0) {
        /*
         * With BR_ISOLATE the branch is bind-mounted over the original mount
         * point, so writes to f->mnt go to this child's branch.
         */
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/branch_result_1.txt", f->mnt);

        FILE *fp = fopen(path, "w");
        if (fp) {
            fputs("branch content\n", fp);
            fclose(fp);
        } else {
            _exit(1);
        }

        union branch_attr c = { .commit = { .flags = 0 } };
        long ret = branch(BR_COMMIT, &c, sizeof(c));
        _exit(ret == 0 ? 0 : 1);
    }

    /* parent */
    int status = 0;
    wait_all();
    (void)status;

    char base_path[PATH_MAX];
    snprintf(base_path, sizeof(base_path), "%s/branch_result_1.txt", f->base);

    /* Give the commit a moment to flush */
    usleep(200000);

    ASSERT(access(base_path, F_OK) == 0,
           "branch_result_1.txt appears in base after commit");
}

/* ============================================================
 * Test 4: test_abort_discards_changes
 *   BR_FS|BR_MEMORY|BR_ISOLATE, n=1; child writes abort_test.txt, then
 *   aborts. Parent asserts file does NOT appear in base.
 * ============================================================ */
static void test_abort_discards_changes(struct fixture *f)
{
    pid_t pids[1] = {0};
    union branch_attr attr = {
        .create = {
            .flags      = BR_FS | BR_MEMORY | BR_ISOLATE,
            .mount_fd   = f->mnt_fd,
            .n_branches = 1,
            .child_pids = (uintptr_t)pids,
        }
    };

    long idx = branch(BR_CREATE, &attr, sizeof(attr));

    if (idx > 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/abort_test.txt", f->mnt);

        FILE *fp = fopen(path, "w");
        if (fp) {
            fputs("should be discarded\n", fp);
            fclose(fp);
        }

        union branch_attr a = { .abort_op = { .flags = 0 } };
        branch(BR_ABORT, &a, sizeof(a));
        /* unreachable */
        _exit(0);
    }

    /* parent */
    wait_all();

    usleep(200000);

    char base_path[PATH_MAX];
    snprintf(base_path, sizeof(base_path), "%s/abort_test.txt", f->base);

    ASSERT(access(base_path, F_OK) != 0,
           "abort_test.txt does NOT appear in base after abort");
}

/* ============================================================
 * Test 5: test_memory_cow
 *   Verifies that fork-based COW gives child its own copy of parent memory.
 * ============================================================ */
static void test_memory_cow(struct fixture *f)
{
    int val = 42;

    pid_t pids[1] = {0};
    union branch_attr attr = {
        .create = {
            .flags      = BR_FS | BR_MEMORY,
            .mount_fd   = f->mnt_fd,
            .n_branches = 1,
            .child_pids = (uintptr_t)pids,
        }
    };

    long idx = branch(BR_CREATE, &attr, sizeof(attr));

    if (idx > 0) {
        /* child: modify local copy */
        val = 99;
        /* abort the FS branch so no side-effects */
        union branch_attr a = { .abort_op = { .flags = 0 } };
        branch(BR_ABORT, &a, sizeof(a));
        /* unreachable */
        _exit(0);
    }

    /* parent */
    wait_all();

    ASSERT_EQ_INT(val, 42,
        "parent's local variable unchanged after child modified its copy");
}

/* ============================================================
 * Test 6: test_first_commit_wins
 *   BR_FS|BR_MEMORY|BR_ISOLATE, n=2.
 *   Child 1 commits immediately; child 2 sleeps then tries to commit,
 *   expecting -ESTALE, then aborts.
 *   Parent checks base for "branch1\n".
 * ============================================================ */
static void test_first_commit_wins(struct fixture *f)
{
    /* shared flag: 1 if child 2 got ESTALE as expected */
    int *got_estale = mmap(NULL, sizeof(int),
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (got_estale == MAP_FAILED) {
        perror("mmap got_estale");
        ASSERT(0, "mmap got_estale should succeed");
        return;
    }
    *got_estale = 0;

    pid_t pids[2] = {0};
    union branch_attr attr = {
        .create = {
            .flags      = BR_FS | BR_MEMORY | BR_ISOLATE,
            .mount_fd   = f->mnt_fd,
            .n_branches = 2,
            .child_pids = (uintptr_t)pids,
        }
    };

    long idx = branch(BR_CREATE, &attr, sizeof(attr));

    if (idx == 1) {
        /* child 1: write and commit immediately */
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/winner.txt", f->mnt);
        FILE *fp = fopen(path, "w");
        if (fp) { fputs("branch1\n", fp); fclose(fp); }

        union branch_attr c = { .commit = { .flags = 0 } };
        branch(BR_COMMIT, &c, sizeof(c));
        _exit(0);
    }

    if (idx == 2) {
        /* child 2: sleep, then try to commit */
        usleep(100000); /* 100 ms */

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/winner.txt", f->mnt);
        FILE *fp = fopen(path, "w");
        if (fp) { fputs("branch2\n", fp); fclose(fp); }

        union branch_attr c = { .commit = { .flags = 0 } };
        long ret = branch(BR_COMMIT, &c, sizeof(c));
        if (ret == -ESTALE)
            *got_estale = 1;

        /* abort since we lost the race */
        union branch_attr a = { .abort_op = { .flags = 0 } };
        branch(BR_ABORT, &a, sizeof(a));
        _exit(0);
    }

    /* parent */
    wait_all();
    usleep(200000);

    /* Read winner.txt from base */
    char base_path[PATH_MAX];
    snprintf(base_path, sizeof(base_path), "%s/winner.txt", f->base);

    char contents[64] = {0};
    FILE *fp = fopen(base_path, "r");
    if (fp) {
        if (!fgets(contents, sizeof(contents), fp)) contents[0] = '\0';
        fclose(fp);
    }

    ASSERT_STR_EQ(contents, "branch1\n",
                  "base/winner.txt contains \"branch1\\n\" (first commit wins)");
    ASSERT_EQ_INT(*got_estale, 1,
                  "second child received -ESTALE on commit");

    munmap(got_estale, sizeof(int));
}

/* ============================================================
 * Test 7: test_close_fds
 *   BR_CLOSE_FDS; child verifies the parent's pipe write-end is closed.
 * ============================================================ */
static void test_close_fds(struct fixture *f)
{
    /* shared flag written by child: 1 = fd was closed (EBADF), 0 = still open */
    int *fd_closed = mmap(NULL, sizeof(int),
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (fd_closed == MAP_FAILED) {
        perror("mmap fd_closed");
        ASSERT(0, "mmap fd_closed should succeed");
        return;
    }
    *fd_closed = 0;

    /* Open a pipe in the parent */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        ASSERT(0, "pipe() should succeed");
        munmap(fd_closed, sizeof(int));
        return;
    }
    int write_end = pipefd[1];

    pid_t pids[1] = {0};
    union branch_attr attr = {
        .create = {
            .flags      = BR_FS | BR_MEMORY | BR_CLOSE_FDS,
            .mount_fd   = f->mnt_fd,
            .n_branches = 1,
            .child_pids = (uintptr_t)pids,
        }
    };

    long idx = branch(BR_CREATE, &attr, sizeof(attr));

    if (idx > 0) {
        /* child: try to write to the parent's pipe write-end */
        errno = 0;
        ssize_t n = write(write_end, "x", 1);
        if (n < 0 && errno == EBADF)
            *fd_closed = 1;
        else
            *fd_closed = 0;

        union branch_attr c = { .commit = { .flags = 0 } };
        branch(BR_COMMIT, &c, sizeof(c));
        _exit(0);
    }

    /* parent: close our own pipe ends */
    close(pipefd[0]);
    close(pipefd[1]);

    wait_all();

    ASSERT_EQ_INT(*fd_closed, 1,
                  "child's pipe write-end was closed (EBADF) due to BR_CLOSE_FDS");

    munmap(fd_closed, sizeof(int));
}

/* ============================================================
 * Tests 8–10: validation — no filesystem needed
 * ============================================================ */

static void test_invalid_n_branches_zero(struct fixture *f)
{
    (void)f; /* no fixture needed */

    union branch_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.create.flags      = BR_FS | BR_MEMORY;
    attr.create.mount_fd   = -1;
    attr.create.n_branches = 0;
    attr.create.child_pids = 0;

    errno = 0;
    long ret = branch(BR_CREATE, &attr, sizeof(attr));

    ASSERT_EQ_INT((int)ret, -1, "n_branches=0 returns -1");
    ASSERT_EQ_INT(errno, EINVAL, "n_branches=0 sets errno=EINVAL");
}

static void test_invalid_n_branches_too_many(struct fixture *f)
{
    (void)f;

    union branch_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.create.flags      = BR_FS | BR_MEMORY;
    attr.create.mount_fd   = -1;
    attr.create.n_branches = 65; /* one above MAX_BRANCHES (64) */
    attr.create.child_pids = 0;

    errno = 0;
    long ret = branch(BR_CREATE, &attr, sizeof(attr));

    ASSERT_EQ_INT((int)ret, -1, "n_branches=65 returns -1");
    ASSERT_EQ_INT(errno, EINVAL, "n_branches=65 sets errno=EINVAL");
}

static void test_invalid_op(struct fixture *f)
{
    (void)f;

    union branch_attr attr;
    memset(&attr, 0, sizeof(attr));

    errno = 0;
    long ret = branch(999, &attr, sizeof(attr));

    ASSERT_EQ_INT((int)ret, -1, "op=999 returns -1");
    ASSERT_EQ_INT(errno, EINVAL, "op=999 sets errno=EINVAL");
}

/* ============================================================
 * main
 * ============================================================ */

int main(void)
{
    printf("BranchFS userspace library test suite\n");
    printf("======================================\n");

    /* ----------------------------------------------------------
     * Tests that require a mounted BranchFS instance
     * ---------------------------------------------------------- */

#define RUN_WITH_FIXTURE(name) do {                 \
    struct fixture *_f = fixture_new(#name);        \
    if (!_f) {                                      \
        printf("\n[TEST] %s\n", #name);             \
        ASSERT(0, "fixture_new failed — skipping"); \
    } else {                                        \
        run_test(#name, name, _f);                  \
        fixture_free(_f);                           \
    }                                               \
} while (0)

    RUN_WITH_FIXTURE(test_create_parent_returns_zero);
    RUN_WITH_FIXTURE(test_create_child_gets_index);
    RUN_WITH_FIXTURE(test_commit_file_appears_in_base);
    RUN_WITH_FIXTURE(test_abort_discards_changes);
    RUN_WITH_FIXTURE(test_memory_cow);
    RUN_WITH_FIXTURE(test_first_commit_wins);
    RUN_WITH_FIXTURE(test_close_fds);

#undef RUN_WITH_FIXTURE

    /* ----------------------------------------------------------
     * Validation tests — no fixture needed
     * ---------------------------------------------------------- */

    run_test("test_invalid_n_branches_zero",    test_invalid_n_branches_zero,    NULL);
    run_test("test_invalid_n_branches_too_many", test_invalid_n_branches_too_many, NULL);
    run_test("test_invalid_op",                 test_invalid_op,                 NULL);

    return print_summary();
}
