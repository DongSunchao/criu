/*
 * mqueue_ipcns.c
 *
 * Test CRIU checkpoint/restore of a POSIX mqueue file descriptor that lives
 * inside a PRIVATE IPC namespace — the standard configuration of each
 * container in a Kubernetes pod.
 *
 * Scenario
 * --------
 *   1. unshare(CLONE_NEWIPC | CLONE_NEWNS): enter a fresh IPC namespace and
 *      a private mount namespace so that the remount of /dev/mqueue is local.
 *   2. Remount mqueue so /dev/mqueue reflects the NEW IPC namespace.  Without
 *      this step the host-namespace mqueue mount would be kept, making queue
 *      names invisible via /proc/self/fd and confusing CRIU's dump path.
 *   3. Create a mqueue named MQ_NAME — identical to the name used in the
 *      basic posix-mqueue.c test — to demonstrate isolation: the two queues
 *      coexist independently in their respective IPC namespaces.
 *   4. Send MSG_COUNT messages with cycling priorities.
 *   5. Record the IPC namespace inode (/proc/self/ns/ipc) and the mqueue fd
 *      inode before handing control to CRIU.
 *   6. After restore verify:
 *      (a) mqueue fd inode changed   — real C/R took place.
 *      (b) IPC namespace inode changed — restored into a fresh namespace.
 *      (c) All MSG_COUNT messages are present and in priority order.
 *
 * User-Namespace / UID-remapping note
 * ------------------------------------
 * Testing POSIX mqueue ownership across a uid_map / gid_map (as used by
 * Kubernetes user-namespace pods) requires writing /proc/PID/uid_map and
 * verifying kuid ↔ uid translation at restore time.  That scenario is
 * tracked by mqueue_fown.c; this test focuses on IPC namespace isolation.
 */

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mount.h>
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
#define MQ_MOUNT	"/dev/mqueue"
#define MSG_SIZE	128
#define MSG_COUNT	6
#define MAX_PRIO	4

struct test_msg {
	uint32_t seq;
	uint32_t prio;
	char	 text[MSG_SIZE - 2 * sizeof(uint32_t)];
};

/*
 * Read the inode number of a /proc/self/ns/<name> symlink target.
 * The kernel exposes each namespace as a special file whose inode number
 * uniquely identifies the namespace instance.
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

/*
 * Remount the mqueue filesystem in the current (private) mount namespace so
 * that /dev/mqueue reflects the queues of the new IPC namespace, not those of
 * the parent namespace that were inherited at unshare() time.
 */
static int remount_mqueue(void)
{
	if (umount2(MQ_MOUNT, MNT_DETACH) < 0 && errno != EINVAL) {
		pr_perror("umount2 " MQ_MOUNT);
		return -1;
	}
	if (mount("mqueue", MQ_MOUNT, "mqueue", 0, NULL) < 0) {
		pr_perror("mount mqueue → " MQ_MOUNT);
		return -1;
	}
	return 0;
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
	ino_t		ipcns_before, ipcns_after;
	unsigned int	last_prio;
	int		i, ret = 0;

	test_init(argc, argv);

	/*
	 * Enter a private IPC namespace (simulating a container) plus a
	 * private mount namespace so the mqueue remount stays local.
	 *
	 * Requires CAP_SYS_ADMIN — the test descriptor sets the suid flag so
	 * the binary runs as root.
	 */
	if (unshare(CLONE_NEWIPC | CLONE_NEWNS) < 0) {
		pr_perror("unshare(CLONE_NEWIPC | CLONE_NEWNS)");
		exit(1);
	}

	if (remount_mqueue() < 0)
		exit(1);

	ipcns_before = ns_inode("ipc");
	test_msg("Entered private IPC namespace (ns inode %lu)\n",
		 (unsigned long)ipcns_before);

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
	 * Inode must change: CRIU recreates the queue from the saved image.
	 * An unchanged inode means dump failed and SIGTERM unblocked us.
	 */
	if (mq_after.st_ino == mq_before.st_ino) {
		fail("mqueue inode unchanged — C/R did not happen");
		mq_close(mq);
		exit(1);
	}

	/*
	 * IPC namespace inode must change: CRIU creates a fresh namespace
	 * for the restored process; the namespace instance is new even though
	 * the queue contents are identical.
	 */
	ipcns_after = ns_inode("ipc");
	if (ipcns_after == ipcns_before) {
		fail("IPC namespace inode unchanged — "
		     "CRIU may not have restored a private IPC namespace");
		mq_close(mq);
		exit(1);
	}
	test_msg("IPC namespace correctly recreated (ns inode %lu → %lu)\n",
		 (unsigned long)ipcns_before, (unsigned long)ipcns_after);

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

	/* Receive and verify each message (blocking fd, loop by count). */
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
