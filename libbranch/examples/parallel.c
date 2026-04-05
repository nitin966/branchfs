/*
 * parallel.c — Equivalent of Listing 2 from the paper.
 *
 * "Fork, Explore, Commit: OS Primitives for Agentic Exploration"
 *
 * Demonstrates three parallel branches racing to commit.  The first branch
 * to succeed wins (first-commit-wins); losing branches receive -ESTALE and
 * abort.  The parent waits for all children to exit.
 *
 * Usage:
 *   # Mount BranchFS first:
 *   branchfs mount /tmp/branchfs-data /mnt/ws
 *
 *   # Build and run:
 *   make -C libbranch
 *   gcc examples/parallel.c -Iinclude -L. -lbranch -o parallel
 *   ./parallel /mnt/ws
 */

#include "branch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#define N_BRANCHES 3

/* Simulate some work that each branch does.
 * In a real scenario this would call an LLM, run tests, apply patches, etc. */
static void do_work(int idx, const char *mount_path)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/branch%d_output.txt", mount_path, idx);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "Branch %d result\n", idx);
        fclose(f);
    }
    /* Simulate variable latency so branches finish in different order */
    usleep((unsigned)(50000 * idx));
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <branchfs-mountpoint>\n", argv[0]);
        return 1;
    }
    const char *mount_path = argv[1];

    /* Open the BranchFS mountpoint directory */
    int mount_fd = open(mount_path, O_RDONLY | O_DIRECTORY);
    if (mount_fd < 0) {
        perror("open mountpoint");
        return 1;
    }

    pid_t pids[N_BRANCHES];
    union branch_attr attr = {
        .create = {
            .flags      = BR_FS | BR_MEMORY,
            .mount_fd   = mount_fd,
            .n_branches = N_BRANCHES,
            .child_pids = (uintptr_t)pids,
        }
    };

    printf("[parent] creating %d branches...\n", N_BRANCHES);

    int idx = (int)branch(BR_CREATE, &attr, sizeof(attr));
    if (idx < 0) {
        perror("branch BR_CREATE");
        close(mount_fd);
        return 1;
    }

    if (idx == 0) {
        /* ---- Parent -------------------------------------------------------
         * The parent does not commit or abort.  It just waits for children.
         * In production code the parent could monitor progress, enforce
         * timeouts, or collect results here. */
        close(mount_fd);
        printf("[parent] forked %d children: ", N_BRANCHES);
        for (int i = 0; i < N_BRANCHES; i++)
            printf("%d%s", pids[i], i < N_BRANCHES - 1 ? ", " : "\n");

        int winner = -1;
        for (int reaped = 0; reaped < N_BRANCHES; reaped++) {
            int status;
            pid_t p = wait(&status);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && winner < 0) {
                for (int i = 0; i < N_BRANCHES; i++) {
                    if (pids[i] == p) {
                        winner = i + 1;
                        break;
                    }
                }
            }
        }
        printf("[parent] branch %d won the commit race\n", winner);
        return 0;
    }

    /* ---- Child (idx is 1..N_BRANCHES) ------------------------------------*/
    printf("[branch %d] starting work (pid %d)\n", idx, (int)getpid());

    do_work(idx, mount_path);

    printf("[branch %d] attempting commit...\n", idx);

    union branch_attr commit_attr = { .commit = { .flags = 0 } };
    long ret = branch(BR_COMMIT, &commit_attr, sizeof(commit_attr));
    if (ret == 0) {
        printf("[branch %d] committed — I win!\n", idx);
        return 0;
    }

    if (ret == -ESTALE) {
        printf("[branch %d] lost commit race (ESTALE) — aborting\n", idx);
    } else {
        fprintf(stderr, "[branch %d] commit error: %s\n", idx, strerror(errno));
    }

    union branch_attr abort_attr = { .abort_op = { .flags = 0 } };
    branch(BR_ABORT, &abort_attr, sizeof(abort_attr));
    /* not reached — BR_ABORT calls _exit() */
    return 1;
}
