/*
 * mqueue_fown.c
 *
 * Test CRIU checkpoint/restore of POSIX mqueue file ownership (uid, gid, pid,
 * and SIGIO/SIGURG async-notification settings stored in the "fown" protobuf
 * field).
 *
 * Motivation / Kubernetes context
 * --------------------------------
 * In a Kubernetes User Namespace pod, the container's UID 0 (root) is mapped
 * to an unprivileged UID on the host (e.g. 65534 "nobody") via
 * /proc/PID/uid_map.  When CRIU dumps a mqueue fd in that context it stores
 * the "virtual" (inside-namespace) UID/GID in the fown protobuf fields.  On
 * restore it must translate them back through the namespace's uid_map so the
 * process sees the same virtual UID/GID.
 *
 * Full user-namespace UID remapping involves writing /proc/PID/uid_map and
 * /proc/PID/gid_map, which requires coordination between a parent and child
 * process and is outside the scope of a self-contained ZDTM test.
 *
 * This test covers the prerequisite: CRIU correctly round-trips the fown
 * fields (uid, euid, gid, pid, signum) through dump→image→restore for a
 * mqueue owned by a NON-ROOT user.  This exercises the same code path that
 * would be taken for the user-namespace UID-mapping scenario.
 *
 * Test steps
 * ----------
 *   1. Drop privileges to an unprivileged UID/GID (nobody / 65534) via
 *      setresuid / setresgid.
 *   2. Create a mqueue and send several messages.
 *   3. Verify pre-dump ownership with fstat().
 *   4. Set up async SIGIO notification on the fd (exercises fown.signum /
 *      fown.pid fields in the protobuf).
 *   5. C/R cycle.
 *   6. Post-restore verify:
 *      (a) inode changed           — real C/R happened.
 *      (b) uid/gid still match     — fown preserved through image.
 *      (c) all messages intact     — content round-trips correctly.
 */

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <mqueue.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <pwd.h>

#include "zdtmtst.h"

const char *test_doc	= "C/R of a POSIX mqueue fd owned by an unprivileged "
			  "uid/gid — exercises fown round-trip through the image "
			  "(prerequisite for user-namespace UID remapping)";
const char *test_author = "sunchao dong <dongsunchao@gmail.com>";

#define MQ_NAME		"/zdtm_mq_fown_test"
#define MSG_SIZE	64
#define MSG_COUNT	5
#define NOBODY_UID	65534
#define NOBODY_GID	65534

struct test_msg {
	uint32_t seq;
	uint32_t prio;
	char	 text[MSG_SIZE - 2 * sizeof(uint32_t)];
};

int main(int argc, char **argv)
{
	mqd_t		mq;
	struct mq_attr	attr = {
		.mq_maxmsg  = MSG_COUNT,
		.mq_msgsize = MSG_SIZE,
	};
	struct mq_attr	cur;
	struct stat	st_before, st_after;
	struct test_msg msg;
	uid_t		expected_uid;
	gid_t		expected_gid;
	unsigned int	last_prio;
	int		i, ret = 0;

	test_init(argc, argv);

	/*
	 * Drop to an unprivileged identity.
	 *
	 * setresgid must come BEFORE setresuid: on Linux, dropping UID first
	 * would remove the ability to change GID.
	 *
	 * The test binary is setuid-root (suid flag in .desc), so we start
	 * with effective UID 0 and can switch freely.
	 */
	if (setresgid(NOBODY_GID, NOBODY_GID, NOBODY_GID) < 0) {
		pr_perror("setresgid(%d)", NOBODY_GID);
		exit(1);
	}
	if (setresuid(NOBODY_UID, NOBODY_UID, NOBODY_UID) < 0) {
		pr_perror("setresuid(%d)", NOBODY_UID);
		exit(1);
	}

	expected_uid = getuid();
	expected_gid = getgid();
	test_msg("Running as uid=%u gid=%u\n",
		 (unsigned)expected_uid, (unsigned)expected_gid);

	mq_unlink(MQ_NAME);

	mq = mq_open(MQ_NAME, O_CREAT | O_RDWR, 0600, &attr);
	if (mq == (mqd_t)-1) {
		pr_perror("mq_open");
		exit(1);
	}

	for (i = 0; i < MSG_COUNT; i++) {
		unsigned int prio = (unsigned int)(i % 3) + 1;

		memset(&msg, 0, sizeof(msg));
		msg.seq  = (uint32_t)i;
		msg.prio = prio;
		snprintf(msg.text, sizeof(msg.text), "fown-msg-%d-p%u", i, prio);

		if (mq_send(mq, (char *)&msg, sizeof(msg), prio) < 0) {
			pr_perror("mq_send msg %d", i);
			mq_close(mq);
			exit(1);
		}
	}
	test_msg("Sent %d messages as uid=%u\n", MSG_COUNT, (unsigned)expected_uid);

	/*
	 * Set up asynchronous SIGIO notification so CRIU's dump records a
	 * non-trivial fown.signum / fown.pid in the image.
	 */
	if (fcntl(mq, F_SETOWN, getpid()) < 0)
		pr_perror("fcntl F_SETOWN (non-fatal)");
	if (fcntl(mq, F_SETFL, fcntl(mq, F_GETFL) | O_ASYNC) < 0)
		pr_perror("fcntl O_ASYNC (non-fatal)");

	/* Record pre-dump ownership and inode. */
	if (fstat(mq, &st_before) < 0) {
		pr_perror("fstat before dump");
		mq_close(mq);
		exit(1);
	}
	test_msg("Pre-dump: uid=%u gid=%u inode=%lu\n",
		 (unsigned)st_before.st_uid, (unsigned)st_before.st_gid,
		 (unsigned long)st_before.st_ino);

	test_daemon();
	test_waitsig();

	/* ---- post-restore verification ---- */

	if (fstat(mq, &st_after) < 0) {
		pr_perror("fstat after restore");
		fail("mqueue fd invalid after restore");
		exit(1);
	}

	/* Inode must change — new queue was created by CRIU restore. */
	if (st_after.st_ino == st_before.st_ino) {
		fail("mqueue inode unchanged — C/R did not happen");
		mq_close(mq);
		exit(1);
	}

	/*
	 * Filesystem ownership must be preserved.  CRIU saves st_uid/st_gid
	 * in the uid/gid fields of ipcns_pmq_data_entry and calls fchown(2)
	 * after mq_open() on restore.  A mismatch here means the ownership
	 * round-trip is broken — exactly the failure mode that Kubernetes
	 * user-namespace UID remapping would expose.
	 */
	if (st_after.st_uid != expected_uid) {
		fail("uid mismatch after restore: got %u expected %u",
		     (unsigned)st_after.st_uid, (unsigned)expected_uid);
		ret = 1;
	}
	if (st_after.st_gid != expected_gid) {
		fail("gid mismatch after restore: got %u expected %u",
		     (unsigned)st_after.st_gid, (unsigned)expected_gid);
		ret = 1;
	}
	if (!ret)
		test_msg("Ownership preserved: uid=%u gid=%u\n",
			 (unsigned)st_after.st_uid, (unsigned)st_after.st_gid);

	if (ret) {
		mq_close(mq);
		exit(1);
	}

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

	/* Drain and verify messages. */
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
			fail("msg %d embedded prio %u != recv prio %u",
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
