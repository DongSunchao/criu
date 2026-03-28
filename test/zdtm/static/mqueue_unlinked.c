/*
 * mqueue_unlinked.c
 *
 * Test C/R of an unlinked POSIX mqueue file descriptor.
 *
 * Scenario: a process calls mq_open() and immediately calls mq_unlink() to
 * remove the queue name from /dev/mqueue.  The fd stays valid; the process
 * sends several messages with varying priorities.
 *
 * This is the mqueue analogue of the "ghost file" scenario for regular files.
 *
 * Current CRIU behaviour (what this test exercises):
 *   dump  - read_fd_link() resolves /proc/self/fd/N and gets the path with
 *            a " (deleted)" suffix added by the VFS after mq_unlink(), e.g.
 *            "/dev/mqueue/zdtm_mq_unlinked_test (deleted)".  strrchr() strips
 *            the directory prefix, leaving "/zdtm_mq_unlinked_test (deleted)"
 *            as the queue name recorded in the image.
 *   restore- pmq_open() creates a new NAMED queue using that literal name
 *            (including the " (deleted)" suffix), replays all saved messages,
 *            and hands the fd to the restored process.  All messages are
 *            intact, but the queue is now NAMED rather than truly unlinked.
 *
 * Ideal future "ghost mqueue" behaviour:
 *   CRIU should detect the " (deleted)" suffix, create a temporary named
 *   queue, immediately unlink it after replaying messages, and return an
 *   anonymous (unlinked) fd — mirroring the ghost-file path for regular fds.
 *
 * False-pass guard:
 *   An inode comparison (fstat before vs. after C/R) detects the case where
 *   CRIU dump silently failed and zdtm.py sent SIGTERM to unblock
 *   test_waitsig(): in that situation the original fd is still alive with its
 *   original inode and all messages still present, which would otherwise look
 *   like a successful restore.
 */

#include <fcntl.h>
#include <sys/stat.h>
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "zdtmtst.h"

const char *test_doc	= "C/R of an mqueue fd whose name was unlinked "
			  "immediately after mq_open() (ghost-mqueue scenario)";
const char *test_author = "sunchao dong <dongsunchao@gmail.com>";

#define MQ_NAME		"/zdtm_mq_unlinked_test"
#define MSG_SIZE	128
#define MSG_COUNT	8
#define MAX_PRIO	5

/*
 * Each message carries its expected send-sequence and the priority value
 * so that the post-restore check can validate priority ordering without
 * relying on any global state.
 */
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
	unsigned int	last_prio;
	int		i, ret = 0;
	mqd_t check_mq;
    char ghost_name[256];

	test_init(argc, argv);

	/* Clean up any leftover queue from a previous run. */
	mq_unlink(MQ_NAME);

	mq = mq_open(MQ_NAME, O_CREAT | O_RDWR, 0666, &attr);
	if (mq == (mqd_t)-1) {
		pr_perror("mq_open failed");
		exit(1);
	}

	/*
	 * Unlink the queue name from the filesystem immediately.
	 *
	 * After this call:
	 *   - /dev/mqueue/zdtm_mq_unlinked_test no longer exists.
	 *   - The fd is still valid and fully functional.
	 *   - readlink(/proc/self/fd/N) returns the path with " (deleted)".
	 *
	 * Any CRIU dump must handle this "deleted" state correctly.
	 */
	if (mq_unlink(MQ_NAME) == -1) {
		pr_perror("mq_unlink failed");
		mq_close(mq);
		exit(1);
	}

	test_msg("Queue '%s' unlinked; fd %d still open\n", MQ_NAME, (int)mq);

	/*
	 * Send MSG_COUNT messages cycling through priorities [1, MAX_PRIO].
	 * Messages with the same priority are in FIFO order; the expected
	 * dequeue sequence is highest-priority first, then FIFO within a tier.
	 */
	for (i = 0; i < MSG_COUNT; i++) {
		unsigned int prio = (unsigned int)(i % MAX_PRIO) + 1;

		memset(&msg, 0, sizeof(msg));
		msg.seq  = (uint32_t)i;
		msg.prio = prio;
		snprintf(msg.text, sizeof(msg.text),
			 "unlinked-msg-%d-prio%u", i, prio);

		if (mq_send(mq, (char *)&msg, sizeof(msg), prio) == -1) {
			pr_perror("mq_send failed for msg %d", i);
			mq_close(mq);
			exit(1);
		}
	}

	test_msg("Sent %d messages on the unlinked mqueue fd\n", MSG_COUNT);

	/*
	 * Record the inode of the live fd.  After a real C/R the restored fd
	 * points to a freshly created queue (new inode).  If the inode is
	 * unchanged after test_waitsig() we know C/R never happened.
	 */
	if (fstat(mq, &st_before) == -1) {
		pr_perror("fstat before dump failed");
		mq_close(mq);
		exit(1);
	}

	test_daemon();
	test_waitsig();

	/* ---------- post-restore verification ---------- */
	snprintf(ghost_name, sizeof(ghost_name), "%s (deleted)", MQ_NAME);

	/*
	 * Verify that neither the original name nor the old "(deleted)" suffix
	 * name exists in the mqueue filesystem after restore.  With the ghost-
	 * mqueue implementation CRIU strips the suffix at dump time, creates a
	 * temporary queue (/criu-ghost-mq-XXXX), and unlinks it immediately
	 * after replaying messages, so /dev/mqueue must be clean.
	 */
	check_mq = mq_open(MQ_NAME, O_RDONLY);
	if (check_mq != (mqd_t)-1) {
		fail("Original queue name '%s' reappeared in VFS after restore",
		     MQ_NAME);
		mq_close(check_mq);
		exit(1);
	}

	check_mq = mq_open(ghost_name, O_RDONLY);
	if (check_mq != (mqd_t)-1) {
		fail("Ghost-suffix queue '%s' visible in VFS after restore — "
		     "CRIU did not properly unlink the temporary queue",
		     ghost_name);
		mq_close(check_mq);
		exit(1);
	}

	test_msg("VFS clean after restore: neither '%s' nor '%s' visible\n",
		 MQ_NAME, ghost_name);

	if (fstat(mq, &st_after) == -1) {
		pr_perror("fstat after restore failed");
		fail("mqueue fd invalid after restore");
		mq_close(mq);
		exit(1);
	}

	if (st_after.st_ino == st_before.st_ino) {
		fail("mqueue inode unchanged — C/R did not take place "
		     "(or CRIU does not support unlinked mqueue fds)");
		mq_close(mq);
		exit(1);
	}

	/* Verify the restored queue holds exactly MSG_COUNT messages. */
	if (mq_getattr(mq, &cur) == -1) {
		pr_perror("mq_getattr after restore failed");
		mq_close(mq);
		exit(1);
	}
	if (cur.mq_curmsgs != MSG_COUNT) {
		fail("Message count mismatch after restore: got %ld, expected %d",
		     cur.mq_curmsgs, MSG_COUNT);
		mq_close(mq);
		exit(1);
	}

	/*
	 * Receive all messages in dequeue order (highest priority first, then
	 * FIFO within the same priority tier) and check:
	 *   1. Priority is non-increasing across successive receives.
	 *   2. The embedded prio field matches the received prio tag.
	 *   3. The embedded seq is within the expected range.
	 *
	 * We iterate exactly MSG_COUNT times.  The fd was opened without
	 * O_NONBLOCK so mq_receive() would block if called on an empty queue;
	 * counting iterations prevents a hang.
	 */
	last_prio = ~0u;
	for (i = 0; i < MSG_COUNT; i++) {
		unsigned int prio;
		ssize_t	n;

		n = mq_receive(mq, (char *)&msg, sizeof(msg), &prio);
		if (n == -1) {
			pr_perror("mq_receive failed at iteration %d", i);
			fail("Failed to receive all messages after restore");
			ret = 1;
			break;
		}

		/* Priority ordering: must be non-increasing. */
		if (prio > last_prio) {
			fail("Priority inversion at receive %d: "
			     "prio %u > previous %u", i, prio, last_prio);
			ret = 1;
			break;
		}
		last_prio = prio;

		/* Consistency: embedded prio must match the dequeue tag. */
		if (msg.prio != prio) {
			fail("msg %d: embedded prio %u != received prio %u",
			     i, msg.prio, prio);
			ret = 1;
			break;
		}

		test_msg("recv[%d]: seq=%u prio=%u text='%s'\n",
			 i, msg.seq, prio, msg.text);
	}

	mq_close(mq);

	/*
	 * Attempt to clean up the ghost temp name just in case — this is a
	 * no-op (ENOENT) because CRIU already unlinked it during restore.
	 */
	mq_unlink(ghost_name);	/* no-op: already unlinked by CRIU */

	if (ret)
		exit(1);

	pass();
	return 0;
}
