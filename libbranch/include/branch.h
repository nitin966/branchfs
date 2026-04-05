/*
 * branch.h — Userspace prototype of the branch() syscall interface.
 *
 * Mirrors the interface from Listing 1 of:
 *   "Fork, Explore, Commit: OS Primitives for Agentic Exploration"
 *
 * The branch() syscall is described in Section 5 of the paper as a kernel
 * design proposal. This library implements the same interface in userspace
 * using fork(2), unshare(2), and BranchFS FS_IOC_BRANCH_* ioctls.
 *
 * Usage:
 *   int mount_fd = open("/mnt/branchfs", O_RDONLY | O_DIRECTORY);
 *
 *   pid_t pids[3];
 *   union branch_attr attr = {
 *       .create = {
 *           .flags      = BR_FS | BR_MEMORY,
 *           .mount_fd   = mount_fd,
 *           .n_branches = 3,
 *           .child_pids = (uintptr_t)pids,
 *       }
 *   };
 *   int idx = branch(BR_CREATE, &attr, sizeof(attr));
 *   if (idx > 0) {
 *       // child: do work, then commit or abort
 *       union branch_attr c = { .commit = { .flags = 0 } };
 *       if (branch(BR_COMMIT, &c, sizeof(c)) == 0)
 *           exit(0);   // committed; siblings will be killed
 *       // lost race (ESTALE) — fall through to abort
 *       union branch_attr a = { .abort = { .flags = 0 } };
 *       branch(BR_ABORT, &a, sizeof(a));  // does not return
 *   }
 *   // parent: wait for winner
 *   int status;
 *   wait(&status);
 */

#ifndef BRANCH_H
#define BRANCH_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Operations (first argument to branch())
 * ------------------------------------------------------------------------- */

#define BR_CREATE  1   /* Fork N branches; returns 0 in parent, 1..N in children */
#define BR_COMMIT  2   /* Commit current branch; kills siblings on success        */
#define BR_ABORT   3   /* Abort current branch; does not return                   */

/* ---------------------------------------------------------------------------
 * Flags for BR_CREATE
 * ------------------------------------------------------------------------- */

/*
 * BR_FS — Branch the filesystem state via BranchFS ioctls.
 * Each child gets its own copy-on-write snapshot of the filesystem.
 * Requires a BranchFS mountpoint (mount_fd must be set).
 */
#define BR_FS        (1u << 0)

/*
 * BR_MEMORY — Branch memory state via fork(2) copy-on-write.
 * Each child inherits the parent's address space as COW pages; writes in one
 * child do not affect other children or the parent.
 * This is an approximation of the kernel page-table COW described in the
 * paper; the actual kernel implementation is planned for Linux 6.19+.
 */
#define BR_MEMORY    (1u << 1)

/*
 * BR_ISOLATE — Mount namespace isolation.
 * Each child calls unshare(CLONE_NEWNS) and bind-mounts its branch directory
 * over the workspace root, so the branch appears at the original path rather
 * than the /@branchname/ virtual path.
 * Requires BR_FS.
 */
#define BR_ISOLATE   (1u << 2)

/*
 * BR_CLOSE_FDS — Close all file descriptors > 2 in children (except the
 * internal branch control fd).  Prevents leaking parent's open files into
 * branch children.
 */
#define BR_CLOSE_FDS (1u << 3)

/* ---------------------------------------------------------------------------
 * Attribute union (second argument to branch())
 * ------------------------------------------------------------------------- */

#define BRANCH_NAME_MAX 128  /* max length of a BranchFS branch name */

union branch_attr {
    struct {
        uint32_t  flags;       /* BR_FS | BR_MEMORY | BR_ISOLATE | BR_CLOSE_FDS */
        int32_t   mount_fd;    /* open(2) fd of the BranchFS mountpoint dir      */
        uint32_t  n_branches;  /* number of branch children to create            */
        uintptr_t child_pids;  /* pid_t* output array (n_branches entries)       */
    } create;

    struct {
        uint32_t  flags;       /* reserved, pass 0                               */
    } commit;

    struct {
        uint32_t  flags;       /* reserved, pass 0                               */
    } abort_op;                /* named abort_op to avoid clash with BR_ABORT    */
};

/* ---------------------------------------------------------------------------
 * Primary entry point
 *
 * Returns:
 *   BR_CREATE:  0 in the parent process; 1..n_branches in each child (index)
 *               -1 on error (errno set)
 *   BR_COMMIT:  0 on success; -ESTALE if another branch already committed
 *               (first-commit-wins); -1 on other errors (errno set)
 *   BR_ABORT:   does not return (calls _exit(0) after aborting)
 * ------------------------------------------------------------------------- */
long branch(int op, union branch_attr *attr, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* BRANCH_H */
