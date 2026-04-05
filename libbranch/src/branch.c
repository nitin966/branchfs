/*
 * branch.c — Userspace prototype of the branch() syscall.
 *
 * Implements the interface from branch.h using:
 *   - fork(2)          for BR_MEMORY (copy-on-write memory)
 *   - unshare(2)       for BR_ISOLATE (private mount namespace)
 *   - mount(2)         for BR_ISOLATE (bind-mount branch over workspace root)
 *   - FS_IOC_BRANCH_*  ioctls on BranchFS for BR_FS
 *
 * Internal per-process state is stored in _branch_state (set in each child
 * after BR_CREATE).  BR_COMMIT and BR_ABORT consult this state to know which
 * branch ctl file to ioctl and which sibling pids to kill.
 */

#include "branch.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sched.h>

/* ---------------------------------------------------------------------------
 * BranchFS ioctl command codes (from src/platform/linux.rs)
 *
 *  FS_IOC_BRANCH_CREATE = _IOR('b', 0, char[128])  → 0x8080_6200
 *  FS_IOC_BRANCH_COMMIT = _IO ('b', 1)             → 0x0000_6201
 *  FS_IOC_BRANCH_ABORT  = _IO ('b', 2)             → 0x0000_6202
 *
 * CREATE takes a 128-byte output buffer; COMMIT/ABORT take no data.
 * ------------------------------------------------------------------------- */
#define FS_IOC_BRANCH_CREATE  0x80806200u
#define FS_IOC_BRANCH_COMMIT  0x00006201u
#define FS_IOC_BRANCH_ABORT   0x00006202u

#define MAX_BRANCHES  64          /* maximum n_branches for BR_CREATE */
#define CTL_FILE      ".branchfs_ctl"

/* ---------------------------------------------------------------------------
 * Shared memory layout (mmap MAP_SHARED|MAP_ANONYMOUS, inherited by children)
 *
 * The parent forks N children and writes each child's pid into this struct.
 * A child spin-waits on `ready` before proceeding, ensuring all sibling pids
 * are recorded before any branch can attempt to commit.
 * ------------------------------------------------------------------------- */
struct branch_shared {
    atomic_int ready;               /* set to 1 by parent after all forks  */
    int        n_branches;
    pid_t      pids[MAX_BRANCHES];  /* child pids, indexed 0..n_branches-1 */
};

/* ---------------------------------------------------------------------------
 * Per-process state (set in each child after BR_CREATE, zero in parent)
 * ------------------------------------------------------------------------- */
static struct {
    char                branch_name[BRANCH_NAME_MAX]; /* this branch's name  */
    int                 branch_ctl_fd;                /* /@name/.branchfs_ctl*/
    char                mount_path[PATH_MAX];         /* resolved mountpoint */
    struct branch_shared *shared;                     /* shared pid table    */
    int                 my_index;                     /* 0-based child index */
    int                 n_branches;                   /* total branch count  */
    int                 active;                       /* 1 if inside a branch*/
} _branch_state;

/* ---------------------------------------------------------------------------
 * resolve_fd_path — Resolve an fd to its filesystem path via /proc/self/fd.
 * Writes into `buf` (size `bufsz`).  Returns 0 on success, -1 on error.
 * ------------------------------------------------------------------------- */
static int resolve_fd_path(int fd, char *buf, size_t bufsz)
{
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
    ssize_t len = readlink(proc_path, buf, bufsz - 1);
    if (len < 0)
        return -1;
    buf[len] = '\0';
    return 0;
}

/* ---------------------------------------------------------------------------
 * open_ctl — Open <base_path>/<subpath>/.branchfs_ctl for ioctl use.
 * If subpath is NULL, opens <base_path>/.branchfs_ctl (the root ctl).
 * Returns the fd on success or -1 with errno set.
 * ------------------------------------------------------------------------- */
static int open_ctl(const char *base_path, const char *subpath)
{
    char path[PATH_MAX];
    if (subpath)
        snprintf(path, sizeof(path), "%s/%s/%s", base_path, subpath, CTL_FILE);
    else
        snprintf(path, sizeof(path), "%s/%s", base_path, CTL_FILE);

    return open(path, O_RDWR);
}

/* ---------------------------------------------------------------------------
 * close_extra_fds — Close all fds > 2 except `keep_fd`.
 * Uses /proc/self/fd to enumerate open fds efficiently.
 * ------------------------------------------------------------------------- */
static void close_extra_fds(int keep_fd)
{
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 65536) maxfd = 1024;
    for (int f = 3; f < (int)maxfd; f++) {
        if (f != keep_fd)
            close(f);
    }
}

/* ---------------------------------------------------------------------------
 * do_isolate — Set up mount namespace isolation for the current child.
 *
 * Steps:
 *   1. unshare(CLONE_NEWNS)  — private mount namespace
 *   2. mount --bind <mount_path>/@<branch_name>  <mount_path>
 *      so the branch's view appears at the original workspace path.
 *
 * Returns 0 on success, -1 on error.
 * ------------------------------------------------------------------------- */
static int do_isolate(const char *mount_path, const char *branch_name)
{
    /* Get a private mount namespace */
    if (unshare(CLONE_NEWNS) < 0)
        return -1;

    /* Prevent mount propagation from leaking to parent namespace */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        return -1;

    /* Build source path: <mount_path>/@<branch_name> */
    char src[PATH_MAX];
    snprintf(src, sizeof(src), "%s/@%s", mount_path, branch_name);

    /* Bind-mount the branch directory over the workspace root */
    if (mount(src, mount_path, NULL, MS_BIND | MS_REC, NULL) < 0)
        return -1;

    return 0;
}

/* ---------------------------------------------------------------------------
 * branch_create — Implement BR_CREATE.
 *
 * For each of attr->create.n_branches:
 *   1. Issue FS_IOC_BRANCH_CREATE ioctl on the root ctl file → branch name
 *   2. fork()
 *   3. Child: set up state, optionally isolate, return 1-based index
 *   4. Parent: record child pid
 *
 * Parent returns 0.  Children return 1..n_branches.
 * On error, returns -1.
 * ------------------------------------------------------------------------- */
static long branch_create(union branch_attr *attr)
{
    uint32_t flags      = attr->create.flags;
    int      mount_fd   = attr->create.mount_fd;
    uint32_t n_branches = attr->create.n_branches;
    pid_t   *out_pids   = (pid_t *)(uintptr_t)attr->create.child_pids;

    if (n_branches == 0 || n_branches > MAX_BRANCHES) {
        errno = EINVAL;
        return -1;
    }

    /* Resolve the mountpoint path */
    char mount_path[PATH_MAX];
    if (resolve_fd_path(mount_fd, mount_path, sizeof(mount_path)) < 0)
        return -1;

    /* At minimum we need BR_FS to create filesystem branches.
     * Without BR_FS, a pure BR_MEMORY branch is still useful (fork-only). */
    int root_ctl_fd = -1;
    if (flags & BR_FS) {
        root_ctl_fd = open_ctl(mount_path, NULL);
        if (root_ctl_fd < 0)
            return -1;
    }

    /* Allocate shared memory for sibling pid table */
    struct branch_shared *shared = mmap(
        NULL, sizeof(*shared),
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS,
        -1, 0);
    if (shared == MAP_FAILED) {
        if (root_ctl_fd >= 0) close(root_ctl_fd);
        return -1;
    }
    atomic_init(&shared->ready, 0);
    shared->n_branches = (int)n_branches;

    /* Branch names to pass to children */
    char branch_names[MAX_BRANCHES][BRANCH_NAME_MAX];
    memset(branch_names, 0, sizeof(branch_names));

    /* --- Fork N children ------------------------------------------------- */
    for (uint32_t i = 0; i < n_branches; i++) {
        /* Create a filesystem branch (before fork so the name is known) */
        if (flags & BR_FS) {
            char name_buf[BRANCH_NAME_MAX] = {0};
            if (ioctl(root_ctl_fd, FS_IOC_BRANCH_CREATE, name_buf) < 0) {
                /* Abort already-created branches */
                for (uint32_t j = 0; j < i; j++) {
                    int bctl = open_ctl(mount_path, branch_names[j]);
                    if (bctl >= 0) {
                        ioctl(bctl, FS_IOC_BRANCH_ABORT, NULL);
                        close(bctl);
                    }
                }
                if (root_ctl_fd >= 0) close(root_ctl_fd);
                munmap(shared, sizeof(*shared));
                return -1;
            }
            strncpy(branch_names[i], name_buf, BRANCH_NAME_MAX - 1);
        }

        pid_t pid = fork();
        if (pid < 0) {
            /* Fork failed; clean up what we've done */
            if (root_ctl_fd >= 0) close(root_ctl_fd);
            munmap(shared, sizeof(*shared));
            return -1;
        }

        if (pid == 0) {
            /* ---- Child --------------------------------------------------- */
            if (root_ctl_fd >= 0) close(root_ctl_fd);

            /* Wait until parent has recorded all sibling pids */
            while (!atomic_load(&shared->ready))
                ; /* spin — short wait until all forks are done */

            /* Set up per-process branch state */
            memset(&_branch_state, 0, sizeof(_branch_state));
            strncpy(_branch_state.mount_path, mount_path, PATH_MAX - 1);
            strncpy(_branch_state.branch_name, branch_names[i], BRANCH_NAME_MAX - 1);
            _branch_state.shared     = shared;
            _branch_state.my_index   = (int)i;
            _branch_state.n_branches = (int)n_branches;
            _branch_state.active     = 1;

            if (flags & BR_FS) {
                /* Build virtual path for this branch's ctl file:
                 * <mount_path>/@<branch_name>/.branchfs_ctl */
                char vpath[PATH_MAX];
                snprintf(vpath, sizeof(vpath), "@%s", branch_names[i]);
                _branch_state.branch_ctl_fd = open_ctl(mount_path, vpath);
                if (_branch_state.branch_ctl_fd < 0) {
                    perror("branch: open branch ctl");
                    _exit(1);
                }
            }

            if (flags & BR_ISOLATE) {
                if (do_isolate(mount_path, branch_names[i]) < 0) {
                    perror("branch: isolate");
                    /* Non-fatal: continue without isolation */
                }
            }

            if (flags & BR_CLOSE_FDS) {
                close_extra_fds(_branch_state.branch_ctl_fd);
            }

            /* Return 1-based child index to the child's caller */
            return (long)i + 1;
        }

        /* ---- Parent: record this child's pid ----------------------------- */
        shared->pids[i] = pid;
        if (out_pids)
            out_pids[i] = pid;
    }

    /* Signal all children that the pid table is complete */
    atomic_store(&shared->ready, 1);

    if (root_ctl_fd >= 0)
        close(root_ctl_fd);

    /* Parent returns 0 */
    return 0;
}

/* ---------------------------------------------------------------------------
 * branch_commit — Implement BR_COMMIT.
 *
 * 1. Issue FS_IOC_BRANCH_COMMIT on the child's branch ctl fd.
 * 2. On success: kill siblings, return 0.
 * 3. On ESTALE: return -ESTALE (lost first-commit-wins race).
 * 4. On other error: return -1.
 * ------------------------------------------------------------------------- */
static long branch_commit(void)
{
    if (!_branch_state.active) {
        errno = EINVAL;
        return -1;
    }

    int ret = 0;

    if (_branch_state.branch_ctl_fd >= 0) {
        ret = ioctl(_branch_state.branch_ctl_fd, FS_IOC_BRANCH_COMMIT, NULL);
        if (ret < 0) {
            if (errno == ESTALE)
                return -ESTALE;
            return -1;
        }
    }

    /* Commit succeeded — kill all sibling branches */
    struct branch_shared *shared = _branch_state.shared;
    if (shared && atomic_load(&shared->ready)) {
        for (int i = 0; i < shared->n_branches; i++) {
            if (i == _branch_state.my_index)
                continue;
            pid_t sib = shared->pids[i];
            if (sib > 0)
                kill(sib, SIGTERM);
        }
    }

    _branch_state.active = 0;
    return 0;
}

/* ---------------------------------------------------------------------------
 * branch_abort — Implement BR_ABORT.
 *
 * Issues FS_IOC_BRANCH_ABORT and calls _exit(0).  Does not return.
 * ------------------------------------------------------------------------- */
static void branch_abort(void)
{
    if (_branch_state.active && _branch_state.branch_ctl_fd >= 0)
        ioctl(_branch_state.branch_ctl_fd, FS_IOC_BRANCH_ABORT, NULL);

    _exit(0);
}

/* ---------------------------------------------------------------------------
 * branch — Public entry point (mirrors paper Listing 1)
 * ------------------------------------------------------------------------- */
long branch(int op, union branch_attr *attr, size_t size)
{
    if (!attr || size < sizeof(*attr)) {
        errno = EINVAL;
        return -1;
    }

    switch (op) {
    case BR_CREATE:
        return branch_create(attr);

    case BR_COMMIT:
        return branch_commit();

    case BR_ABORT:
        branch_abort();
        /* unreachable */
        return -1;

    default:
        errno = EINVAL;
        return -1;
    }
}
