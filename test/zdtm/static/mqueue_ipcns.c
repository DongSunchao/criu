/*
 * mqueue_ipcns.c
 *
 * Test CRIU checkpoint/restore of a POSIX mqueue file descriptor that lives
 * inside a PRIVATE IPC namespace — the standard configuration of each
 * container in a Kubernetes pod.
 *
 * Design choice: CLONE_NEWIPC only, no CLONE_NEWNS
 * -------------------------------------------------
 * mq_open(2) is a pure kernel syscall that operates directly on the calling
 * process's IPC namespace.  It does NOT go through the /dev/mqueue virtual
 * filesystem; /dev/mqueue is just a convenience view and is not needed for
 * mq_open / mq_send / mq_receive to work.
 *
 * Using CLONE_NEWNS in addition would create a private mount namespace.
 * CRIU then tries to dump every mount in that namespace and may encounter
 * mounts with filesystem types it does not support (e.g. 9p / virtio-fs
 * used in QEMU guests, overlayfs, etc.), causing the dump to fail.
 *
 * By using only CLONE_NEWIPC, the process stays in the host mount namespace
 * (which CRIU handles correctly) while exercising the key property under
 * test: the mqueue fd lives in its own IPC namespace.
 *
 * IPC namespace inode check
 * -------------------------
 * CRIU preserves the exact namespace instance across dump/restore by keeping
 * an ns fd reference open and re-entering via setns(2).  The inode therefore
 * does NOT change after C/R — that is correct behaviour.
 *
 * The test records the HOST IPC namespace inode before unshare() and checks
 * that the restored process is still in a PRIVATE namespace (inode ≠ host),
 * rather than checking for an inode change.
 *
 * Scenario
 * --------
 *   1. Record host IPC namespace inode.
 *   2. unshare(CLONE_NEWIPC): enter a fresh IPC namespace.
 *   3. Verify we are now in a different (private) namespace.
 *   4. Create a mqueue named MQ_NAME (same as posix-mqueue.c — namespace
 *      isolation prevents any conflict).
 *   5. Send MSG_COUNT messages with cycling priorities.
 *   6. C/R cycle.
 *   7. After restore verify:
 *      (a) mqueue fd inode changed      — real C/R took place.
 *      (b) still in a private namespace — not dropped back to host.
 *      (c) all messages intact and in priority order.
 */

#include <fcntl.h>
#include <sys/stat.h>
#include <sched.h>
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

#include "zdtmtst.h"

const char *test_doc	= "C/R of a POSIX mqueue in a private IPC namespace "
			  "(Kubernetes container simulation)";
const char *test_author = "sunchao dong <dongsunchao@gmail.com>";

/* Same name as posix-mqueue.c — proves namespace isolation. */
#define MQ_NAME		"/zdtm_posix_mqueue_test"
#define MSG_SIZE	128
#define MSG_COUNT	6
#define MAX_PRIO	4

struct test_msg {
	uint32_t seq;
	uint32_t prio;
	char	 text[MSG_SIZE - 2 * sizeof(uint32_t)];
};

/*
 * Return the inode number of /proc/self/ns/<name>.
 * Each namespace instance is exposed as a unique inode by the kernel.
 */
static ino_t ns_inode(const char *ns_name)
{
	char path[64];
	struct stat st;

	snprintf(path, sizeof(path), "/proc/self/ns/%s", ns_name);
	if (stat(path, &st) < 0) {
		pr_perror("stat %s", path);
		return 0;
	}
	return st.st_ino;
}

int main(int argc, char **argv)
{
	mqd_t		mq;
	struct mq_attr	attr = {
		.mq_maxmsg  = MSG_COUNT,
		.mq_msgsize = MSG_SIZE,
	};
	struct mq_attr	cur;
	struct stat	mq_before, mq_after;
	struct test_msg msg;
	ino_t		host_ipcns, private_ipcns, restored_ipcns;
	unsigned int	last_prio;
	int		i, ret = 0;

	test_init(argc, argv);

	/*
	 * Record the HOST IPC namespace inode before unshare().
	 * After C/R the restored process must NOT be in the host namespace —
	 * we use this value as the sentinel for "dropped back to host ns".
	 */
	host_ipcns = ns_inode("ipc");

	/*
	 * Enter a private IPC namespace only — do NOT request CLONE_NEWNS.
	 * Staying in the host mount namespace lets CRIU dump mounts normally.
	 */
	if (unshare(CLONE_NEWIPC) < 0) {
		pr_perror("unshare(CLONE_NEWIPC)");
		exit(1);
	}

	private_ipcns = ns_inode("ipc");
	if (private_ipcns == host_ipcns) {
		fail("unshare did not create a new IPC namespace");
		exit(1);
	}
	test_msg("Entered private IPC namespace "
		 "(host ns inode %lu → private ns inode %lu)\n",
		 (unsigned long)host_ipcns, (unsigned long)private_ipcns);

	mq_unlink(MQ_NAME);

	mq = mq_open(MQ_NAME, O_CREAT | O_RDWR, 0666, &attr);
	if (mq == (mqd_t)-1) {
		pr_perror("mq_open");
		exit(1);
	}

	for (i = 0; i < MSG_COUNT; i++) {
		unsigned int prio = (unsigned int)(i % MAX_PRIO) + 1;

		memset(&msg, 0, sizeof(msg));
		msg.seq  = (uint32_t)i;
		msg.prio = prio;
		snprintf(msg.text, sizeof(msg.text),
			 "ipcns-msg-%d-prio%u", i, prio);

		if (mq_send(mq, (char *)&msg, sizeof(msg), prio) < 0) {
			pr_perror("mq_send msg %d", i);
			mq_close(mq);
			exit(1);
		}
	}
	test_msg("Sent %d messages in private IPC namespace\n", MSG_COUNT);

	if (fstat(mq, &mq_before) < 0) {
		pr_perror("fstat before dump");
		mq_close(mq);
		exit(1);
	}

	test_daemon();
	test_waitsig();

	/* ---- post-restore verification ---- */

	if (fstat(mq, &mq_after) < 0) {
		pr_perror("fstat after restore");
		fail("mqueue fd invalid after restore");
		exit(1);
	}

	/*
	 * Mqueue fd inode must change: CRIU calls mq_unlink + mq_open on
	 * restore, which always produces a new inode.  An unchanged inode
	 * means dump failed and SIGTERM was used to unblock test_waitsig().
	 */
	if (mq_after.st_ino == mq_before.st_ino) {
		fail("mqueue inode unchanged — C/R did not happen");
		mq_close(mq);
		exit(1);
	}

	/*
	 * After restore the process must still be in a PRIVATE IPC namespace.
	 * CRIU may restore into the same instance (inode == private_ipcns, via
	 * setns) or a fresh one (new inode) — both are correct.  What must NOT
	 * happen is being dropped back into the host namespace.
	 */
	restored_ipcns = ns_inode("ipc");
	if (restored_ipcns == host_ipcns) {
		fail("restored into host IPC namespace — "
		     "CRIU lost the private IPC namespace");
		mq_close(mq);
		exit(1);
	}
	if (restored_ipcns == private_ipcns)
		test_msg("Restored into same IPC namespace instance "
			 "(inode %lu, expected)\n",
			 (unsigned long)restored_ipcns);
	else
		test_msg("Restored into new IPC namespace instance "
			 "(inode %lu → %lu)\n",
			 (unsigned long)private_ipcns,
			 (unsigned long)restored_ipcns);

	/* Verify message count. */
	if (mq_getattr(mq, &cur) < 0) {
		pr_perror("mq_getattr after restore");
		mq_close(mq);
		exit(1);
	}
	if (cur.mq_curmsgs != MSG_COUNT) {
		fail("message count mismatch: got %ld expected %d",
		     cur.mq_curmsgs, MSG_COUNT);
		mq_close(mq);
		exit(1);
	}

	/* Drain and verify messages in priority order. */
	last_prio = ~0u;
	for (i = 0; i < MSG_COUNT; i++) {
		unsigned int prio;
		ssize_t	n;

		n = mq_receive(mq, (char *)&msg, sizeof(msg), &prio);
		if (n < 0) {
			pr_perror("mq_receive at %d", i);
			fail("mq_receive failed");
			ret = 1;
			break;
		}
		if (prio > last_prio) {
			fail("priority inversion at recv %d: prio %u > prev %u",
			     i, prio, last_prio);
			ret = 1;
			break;
		}
		last_prio = prio;
		if (msg.prio != prio) {
			fail("msg %d: embedded prio %u != recv prio %u",
			     i, msg.prio, prio);
			ret = 1;
			break;
		}
		test_msg("recv[%d]: seq=%u prio=%u text='%s'\n",
			 i, msg.seq, prio, msg.text);
	}

	mq_close(mq);
	mq_unlink(MQ_NAME);

	if (ret)
		exit(1);

	pass();
	return 0;
}
