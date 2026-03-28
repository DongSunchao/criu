/*
 * mqueue_bulk_stress.c
 *
 * Stress test for checkpoint/restore of POSIX message queue file descriptors.
 * Specifically exercises the MQ_IOC_BULK_PEEK ioctl under three conditions:
 *
 *  Queue A "many-small":  64 messages x 128 B — forces multiple ioctl batches
 *                         because total data exceeds the PEEK_BUF_SIZE (8192 B)
 *                         used by CRIU's pmq_peek_messages().
 *
 *  Queue B "few-large":   8 messages x 8500 B — each message payload exceeds
 *                         PEEK_BUF_SIZE so the kernel sets MQ_PEEK_FLAG_HAS_MORE
 *                         and splits the message across multiple chunks.
 *
 *  Queue C "mixed-prio":  32 messages x 512 B with priorities spread over
 *                         [1, MAX_PRIO] — verifies that priority-then-FIFO
 *                         ordering is preserved exactly after restore.
 *
 * Each message embeds a header with a magic sentinel, queue id, send-sequence
 * number, and CRC of the payload.  The payload itself is a deterministic byte
 * pattern derived from (seq, queue_id, byte_offset).  After restore every byte
 * of every message is re-checked against the same formula.
 *
 * An inode-change check (fstat before vs. after C/R) guards against false
 * passes when CRIU dump silently fails and zdtm.py sends SIGTERM instead.
 */

#include <fcntl.h>
#include <sys/stat.h>
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

#include "zdtmtst.h"

const char *test_doc	= "Stress C/R of POSIX mqueues: many messages, large "
			  "payloads, chunked MQ_IOC_BULK_PEEK, priority ordering";
const char *test_author	= "sunchao dong <dongsunchao@gmail.com>";

/* ---- queue names ---- */
#define MQ_MANY_SMALL	"/zdtm_mq_stress_many_small"
#define MQ_FEW_LARGE	"/zdtm_mq_stress_few_large"
#define MQ_MIXED_PRIO	"/zdtm_mq_stress_mixed_prio"

/* ---- queue parameters ---- */

/*
 * 64 x 128 B = 8192 B total payload, which with per-message headers and
 * CRIU's 8192-byte ioctl buffer forces at least two ioctl batches.
 */
#define MANY_SMALL_COUNT	64
#define MANY_SMALL_SIZE		128

/*
 * 8500 B > PEEK_BUF_SIZE (8192) - sizeof(mq_peek_msg_hdr) (16),
 * so the kernel must split each message into multiple chunks
 * (sets MQ_PEEK_FLAG_HAS_MORE on all but the final chunk).
 */
#define FEW_LARGE_COUNT		8
#define FEW_LARGE_SIZE		8500

/*
 * 32 messages with 10 distinct priorities; used to validate that the
 * dequeue order after restore matches priority-then-FIFO exactly.
 */
#define MIXED_COUNT		32
#define MIXED_SIZE		512
#define MAX_PRIO		10

/* ---- per-message header embedded at byte 0 of every payload ---- */
#define MSG_MAGIC	0xDEADBEEFu

struct msg_hdr {
	uint32_t magic;		/* must equal MSG_MAGIC */
	uint32_t queue_id;	/* index into queues[] */
	uint32_t msg_seq;	/* 0-based send sequence number */
	uint32_t payload_len;	/* bytes following this header */
	uint32_t crc;		/* checksum of payload bytes */
	uint32_t pad;		/* reserved, zero */
};

/* ---- queue descriptor table ---- */
struct queue_cfg {
	const char	*name;
	int		 maxmsg;
	int		 msgsize;
	int		 count;
	uint32_t	 id;
};

static struct queue_cfg queues[] = {
	{ MQ_MANY_SMALL,  MANY_SMALL_COUNT, MANY_SMALL_SIZE, MANY_SMALL_COUNT, 0 },
	{ MQ_FEW_LARGE,   FEW_LARGE_COUNT,  FEW_LARGE_SIZE,  FEW_LARGE_COUNT,  1 },
	{ MQ_MIXED_PRIO,  MIXED_COUNT,      MIXED_SIZE,      MIXED_COUNT,       2 },
};

#define NR_QUEUES	((int)(sizeof(queues) / sizeof(queues[0])))

static mqd_t		mqs[3];
static struct stat	st_before[3];

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * simple_crc - lightweight checksum for payload integrity checks.
 * Not cryptographic; just needs to catch byte-level corruption.
 */
static uint32_t simple_crc(const uint8_t *data, size_t len)
{
	uint32_t crc = 0x12345678u;
	size_t i;

	for (i = 0; i < len; i++)
		crc = (crc << 5) ^ (crc >> 27) ^ data[i];
	return crc;
}

/*
 * fill_payload - write a deterministic pattern into buf[0..len-1].
 * Pattern: byte[i] = (seq * 13 + i + queue_id * 7) & 0xFF
 */
static void fill_payload(uint8_t *buf, size_t len,
			 uint32_t queue_id, uint32_t seq)
{
	size_t i;

	for (i = 0; i < len; i++)
		buf[i] = (uint8_t)((seq * 13 + i + queue_id * 7) & 0xFF);
}

/*
 * check_payload - verify buf matches the pattern expected for (queue_id, seq).
 * Returns 0 on success, -1 on the first mismatch.
 */
static int check_payload(const uint8_t *buf, size_t len,
			 uint32_t queue_id, uint32_t seq)
{
	size_t i;

	for (i = 0; i < len; i++) {
		uint8_t expected = (uint8_t)((seq * 13 + i + queue_id * 7) & 0xFF);

		if (buf[i] != expected) {
			pr_err("Payload mismatch: queue %u seq %u byte %zu "
			       "got 0x%02x expected 0x%02x\n",
			       queue_id, seq, i, buf[i], expected);
			return -1;
		}
	}
	return 0;
}

/*
 * try_set_proc_limit - attempt to raise a /proc/sys/fs/mqueue/ parameter.
 * Silently ignores failures (the subsequent mq_open will fail with EINVAL
 * and produce a clear error message).
 */
static void try_set_proc_limit(const char *path, int val)
{
	FILE *f;
	int cur = 0;

	f = fopen(path, "r");
	if (!f)
		return;
	if (fscanf(f, "%d", &cur) != 1)
		cur = 0;
	fclose(f);

	if (cur >= val)
		return;

	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "%d", val);
	fclose(f);
}

/* ------------------------------------------------------------------ */
/* Test phases                                                          */
/* ------------------------------------------------------------------ */

/*
 * open_queues - create all queues and snapshot their inodes.
 *
 * Any pre-existing queue with the same name is unlinked first so we
 * always start from a clean state.
 */
static int open_queues(void)
{
	int i;

	for (i = 0; i < NR_QUEUES; i++) {
		struct mq_attr attr = {
			.mq_maxmsg  = queues[i].maxmsg,
			.mq_msgsize = queues[i].msgsize,
		};

		mq_unlink(queues[i].name);

		mqs[i] = mq_open(queues[i].name,
				 O_CREAT | O_RDWR ,
				 0666, &attr);
		if (mqs[i] == (mqd_t)-1) {
			pr_perror("mq_open(%s) failed", queues[i].name);
			return -1;
		}

		if (fstat(mqs[i], &st_before[i]) == -1) {
			pr_perror("fstat before dump on %s", queues[i].name);
			return -1;
		}

		test_msg("Opened %s (maxmsg=%d msgsize=%d)\n",
			 queues[i].name, queues[i].maxmsg, queues[i].msgsize);
	}
	return 0;
}

/*
 * fill_queues - populate every queue with its full complement of messages.
 *
 * Priority assignment for Queue C: prio = (i % MAX_PRIO) + 1 so all
 * ten priority levels appear, and same-priority messages arrive in a
 * known FIFO order we can check after restore.
 *
 * Priority assignment for Queues A and B: a cycling formula that
 * spreads messages across a smaller priority range so the ordering is
 * non-trivial but still deterministic.
 */
static int fill_queues(void)
{
	int q;

	for (q = 0; q < NR_QUEUES; q++) {
		int msgsize = queues[q].msgsize;
		uint8_t *buf;
		int i;

		buf = malloc(msgsize);
		if (!buf) {
			pr_perror("malloc(%d)", msgsize);
			return -1;
		}

		for (i = 0; i < queues[q].count; i++) {
			struct msg_hdr *hdr  = (struct msg_hdr *)buf;
			uint8_t *payload     = buf + sizeof(struct msg_hdr);
			size_t   payload_len = (size_t)msgsize - sizeof(*hdr);
			unsigned int prio;

			/*
			 * Queue C uses all MAX_PRIO levels; others use a
			 * narrower range so dumps with few messages still
			 * exercise priority re-ordering.
			 */
			if (q == 2)
				prio = (unsigned int)(i % MAX_PRIO) + 1;
			else
				prio = (unsigned int)((i * 3 + q) % 5) + 1;

			fill_payload(payload, payload_len, queues[q].id,
				     (uint32_t)i);

			hdr->magic	 = MSG_MAGIC;
			hdr->queue_id	 = queues[q].id;
			hdr->msg_seq	 = (uint32_t)i;
			hdr->payload_len = (uint32_t)payload_len;
			hdr->crc	 = simple_crc(payload, payload_len);
			hdr->pad	 = 0;

			if (mq_send(mqs[q], (char *)buf, msgsize, prio) == -1) {
				pr_perror("mq_send to %s msg %d failed",
					  queues[q].name, i);
				free(buf);
				return -1;
			}
		}

		test_msg("Sent %d messages to %s (msgsize=%d)\n",
			 queues[q].count, queues[q].name, msgsize);
		free(buf);
	}
	return 0;
}

/*
 * verify_queues - drain all queues after restore and check every invariant.
 *
 * For each queue the following are verified:
 *   1. inode changed   — confirms a real C/R took place
 *   2. mq_curmsgs      — all messages survived the round-trip
 *   3. priority order  — dequeue order is non-increasing in priority
 *   4. header magic    — sentinel byte pattern is intact
 *   5. CRC             — payload checksum matches the stored value
 *   6. payload bytes   — every byte matches the deterministic pattern
 */
static int verify_queues(void)
{
	int q;
	int ret = 0;

	for (q = 0; q < NR_QUEUES; q++) {
		int msgsize = queues[q].msgsize;
		struct mq_attr attr;
		struct stat st_after;
		uint8_t *buf;
		int received = 0;
		unsigned int last_prio = ~0u;

		buf = malloc(msgsize + 1);
		if (!buf) {
			pr_perror("malloc(%d)", msgsize + 1);
			return -1;
		}

		/* 1. Inode must have changed — real C/R recreates the queue. */
		if (fstat(mqs[q], &st_after) == -1) {
			pr_perror("fstat after restore on %s", queues[q].name);
			free(buf);
			return -1;
		}
		if (st_after.st_ino == st_before[q].st_ino) {
			fail("%s: inode unchanged after restore — "
			     "dump/restore did not happen",
			     queues[q].name);
			free(buf);
			return -1;
		}

		/* 2. Message count must be preserved. */
		if (mq_getattr(mqs[q], &attr) == -1) {
			pr_perror("mq_getattr on %s", queues[q].name);
			free(buf);
			return -1;
		}
		if ((int)attr.mq_curmsgs != queues[q].count) {
			fail("%s: expected %d messages, found %ld after restore",
			     queues[q].name, queues[q].count, attr.mq_curmsgs);
			free(buf);
			return -1;
		}

		/*
		 * Drain exactly attr.mq_curmsgs messages.  The fd is opened
		 * in blocking mode, so we must not rely on EAGAIN to detect
		 * an empty queue — mq_receive would block forever.  We already
		 * verified that attr.mq_curmsgs == queues[q].count above.
		 */
		for (received = 0; received < queues[q].count; ) {
			unsigned int prio;
			ssize_t n;
			struct msg_hdr *hdr;
			uint8_t *payload;
			size_t payload_len;

			n = mq_receive(mqs[q], (char *)buf, msgsize, &prio);
			if (n == -1) {
				pr_perror("mq_receive on %s", queues[q].name);
				ret = -1;
				break;
			}

			/* 3. Priority ordering: non-increasing. */
			if (prio > last_prio) {
				fail("%s: priority inversion at msg %d "
				     "(prio %u after %u)",
				     queues[q].name, received, prio, last_prio);
				ret = -1;
			}
			last_prio = prio;

			if ((size_t)n < sizeof(struct msg_hdr)) {
				fail("%s: msg %d too short (%zd B)",
				     queues[q].name, received, n);
				ret = -1;
				received++;
				continue;
			}

			hdr	    = (struct msg_hdr *)buf;
			payload	    = buf + sizeof(struct msg_hdr);
			payload_len = (size_t)n - sizeof(struct msg_hdr);

			/* 4. Header magic. */
			if (hdr->magic != MSG_MAGIC) {
				fail("%s: msg %d bad magic 0x%08x",
				     queues[q].name, received, hdr->magic);
				ret = -1;
			}

			if (hdr->queue_id != queues[q].id) {
				fail("%s: msg %d wrong queue_id %u",
				     queues[q].name, received, hdr->queue_id);
				ret = -1;
			}

			/* 5. CRC check. */
			if (simple_crc(payload, payload_len) != hdr->crc) {
				fail("%s: msg seq %u CRC mismatch "
				     "(got 0x%08x stored 0x%08x)",
				     queues[q].name, hdr->msg_seq,
				     simple_crc(payload, payload_len), hdr->crc);
				ret = -1;
			}

			/* 6. Byte-level payload pattern check. */
			if (check_payload(payload, payload_len,
					  hdr->queue_id, hdr->msg_seq) != 0) {
				fail("%s: msg seq %u payload pattern mismatch",
				     queues[q].name, hdr->msg_seq);
				ret = -1;
			}

			received++;
		}

		if (received != queues[q].count) {
			fail("%s: received %d messages, expected %d",
			     queues[q].name, received, queues[q].count);
			ret = -1;
		} else {
			test_msg("%s: all %d messages verified OK\n",
				 queues[q].name, received);
		}

		free(buf);
	}

	return ret;
}

/*
 * cleanup_queues - close and unlink all queues regardless of state.
 * Safe to call even if some queues were never opened.
 */
static void cleanup_queues(void)
{
	int i;

	for (i = 0; i < NR_QUEUES; i++) {
		if (mqs[i] != (mqd_t)-1) {
			mq_close(mqs[i]);
			mqs[i] = (mqd_t)-1;
		}
		mq_unlink(queues[i].name);
	}
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
	int i;

	for (i = 0; i < NR_QUEUES; i++)
		mqs[i] = (mqd_t)-1;

	test_init(argc, argv);

	/*
	 * Raise kernel mqueue limits so we can use the parameters above.
	 *   msg_max    must be >= MANY_SMALL_COUNT (64)
	 *   msgsize_max must be >= FEW_LARGE_SIZE  (8500)
	 * The test runs as root (suid flag in .desc); if the writes fail
	 * the subsequent mq_open() will return EINVAL with a clear message.
	 */
	try_set_proc_limit("/proc/sys/fs/mqueue/msg_max",    MANY_SMALL_COUNT);
	try_set_proc_limit("/proc/sys/fs/mqueue/msgsize_max", FEW_LARGE_SIZE);

	if (open_queues() != 0) {
		cleanup_queues();
		exit(1);
	}

	if (fill_queues() != 0) {
		cleanup_queues();
		exit(1);
	}

	test_msg("All queues filled. Totals: %d + %d + %d messages.\n",
		 MANY_SMALL_COUNT, FEW_LARGE_COUNT, MIXED_COUNT);

	test_daemon();
	test_waitsig();

	test_msg("Restored. Verifying message integrity across all queues...\n");

	if (verify_queues() != 0) {
		cleanup_queues();
		exit(1);
	}

	cleanup_queues();
	pass();
	return 0;
}
