// hakuX-Boosted: memory-ordering litmus tests between hardware threads, counting the reorderings each pattern shows
// over millions of iterations. Message passing with and without lwsync on either side, store buffering with none,
// lwsync and sync, and a ring buffer modelled on Gears of War's: the writer stores a slot, lwsync, advances the write
// pointer; the reader polls the pointers and reads the slot with no barrier. Barrier-protected patterns must show no
// reordering on any correct machine (FAIL otherwise); the ring's unbarriered read is checked too, as the game relies on
// it. Unprotected message passing and store buffering are reported, not judged. Every loop stops at its iteration
// count or a time budget, whichever comes first.
#include "hakux_mt.h"

enum {
	TIME_LIMIT_SECONDS = 300,
	PHASE_SECONDS = 10,
	TIME_CHECK_MASK = 4095,
	MP_ITERATIONS = 4000000,
	SB_ITERATIONS = 1000000,
	RING_ITEMS = 4000000,
	RING_SLOTS = 16,
	WRITER_HARDWARE_THREAD = 2,
	OTHER_CORE_HARDWARE_THREAD = 4,
	SAME_CORE_HARDWARE_THREAD = 3,
};

typedef enum { FENCE_NONE, FENCE_LWSYNC, FENCE_SYNC, FENCE_CTRL_ISYNC } Fence;

static const char *const FENCE_NAMES[] = { "none", "lwsync", "sync", "ctrl+isync" };

// The barrier between two accesses; ctrl+isync is a compare and branch on the loaded value, then isync, which orders
// a later load after the earlier one on PowerPC.
static inline __attribute__((always_inline)) void fence(Fence kind, uint32_t loaded)
{
	switch (kind) {
	case FENCE_LWSYNC:
		__asm__ volatile("lwsync" : : : "memory");
		break;
	case FENCE_SYNC:
		__asm__ volatile("sync" : : : "memory");
		break;
	case FENCE_CTRL_ISYNC:
		__asm__ volatile("cmpw %0,%0\n\tbne- 1f\n1:\tisync" : : "r"(loaded) : "memory", "cr0");
		break;
	default:
		__asm__ volatile("" : : : "memory");
		break;
	}
}

static volatile uint32_t stop;
static int64_t phase_end_ns;

static void phase_begin(void)
{
	stop = 0;
	phase_end_ns = hakux_now_ns() + (int64_t)PHASE_SECONDS * 1000000000;
}

// Called every TIME_CHECK_MASK + 1 iterations; true once the phase must end.
static bool phase_over(uint32_t i)
{
	if ((i & TIME_CHECK_MASK) == 0 && hakux_now_ns() >= phase_end_ns)
		stop = 1;
	return stop != 0;
}

// Message passing: the writer stores data then the flag, both counting up; a reader that sees flag f and data below f
// read the data before the flag's store became visible to it.
static HakuxLine mp_data;
static HakuxLine mp_flag;
static volatile uint32_t mp_writer_done;

typedef struct {
	Fence fence;
	uint32_t written;
	uint32_t reads;
	uint32_t advanced;
	uint32_t stale;
} Mp;

static uint32_t mp_write(void *context)
{
	Mp *const mp = context;
	uint32_t i = 1;
	for (; i <= MP_ITERATIONS; i++) {
		mp_data.value = i;
		fence(mp->fence, 0);
		mp_flag.value = i;
		if (phase_over(i))
			break;
	}
	mp->written = i - 1;
	mp_writer_done = 1;
	return 0;
}

typedef struct {
	Mp *mp;
	Fence fence;
} MpReader;

static uint32_t mp_read(void *context)
{
	MpReader *const r = context;
	uint32_t reads = 0;
	uint32_t stale = 0;
	uint32_t advanced = 0;
	uint32_t last = 0;
	while (!mp_writer_done) {
		const uint32_t f = mp_flag.value;
		fence(r->fence, f);
		const uint32_t d = mp_data.value;
		stale += d < f;
		advanced += f != last;
		last = f;
		reads++;
		if (phase_over(reads))
			break;
	}
	r->mp->reads = reads;
	r->mp->stale = stale;
	r->mp->advanced = advanced;
	return 0;
}

static void message_passing(Fence writer, Fence reader, uint32_t reader_hardware_thread, bool must_hold)
{
	hakux_step("message passing, writer %s on hardware thread %u, reader %s on %u, up to %u stores or %u s",
		FENCE_NAMES[writer], (unsigned)WRITER_HARDWARE_THREAD, FENCE_NAMES[reader], (unsigned)reader_hardware_thread,
		(unsigned)MP_ITERATIONS, (unsigned)PHASE_SECONDS);
	Mp mp = { writer, 0, 0, 0, 0 };
	MpReader r = { &mp, reader };
	mp_data.value = 0;
	mp_flag.value = 0;
	mp_writer_done = 0;
	hakux_sync();
	phase_begin();
	HANDLE readers = hakux_spawn(mp_read, &r, reader_hardware_thread);
	HANDLE writers = hakux_spawn(mp_write, &mp, WRITER_HARDWARE_THREAD);
	hakux_join(writers, (PHASE_SECONDS + 30) * 1000);
	hakux_join(readers, (PHASE_SECONDS + 30) * 1000);
	xsdk_log_write("order: mp writer=%s reader=%s threads %u,%u: %u stores, %u reads, %u saw a new flag, %u stale",
		FENCE_NAMES[writer], FENCE_NAMES[reader], (unsigned)WRITER_HARDWARE_THREAD, (unsigned)reader_hardware_thread,
		(unsigned)mp.written, (unsigned)mp.reads, (unsigned)mp.advanced, (unsigned)mp.stale);
	if (must_hold)
		hakux_check(mp.stale == 0 && mp.advanced > 0,
			"message passing with writer %s and reader %s never reads stale data (%u stale in %u reads)",
			FENCE_NAMES[writer], FENCE_NAMES[reader], (unsigned)mp.stale, (unsigned)mp.reads);
	else
		xsdk_log_write("INFO: order: message passing with writer %s and reader %s: %u stale in %u reads (allowed by "
			"the architecture, reported only)",
			FENCE_NAMES[writer], FENCE_NAMES[reader], (unsigned)mp.stale, (unsigned)mp.reads);
}

// Store buffering: each thread stores its own word then loads the other's, both tagged with the round number, after a
// spin barrier per round. Sequential consistency forbids both loads missing the other's store of the same round.
static HakuxLine sb_x;
static HakuxLine sb_y;
static HakuxLine sb_arrive[2];
static uint8_t sb_old[2][SB_ITERATIONS];
static volatile uint32_t sb_rounds[2];

typedef struct {
	uint32_t self;
	Fence fence;
} Sb;

static uint32_t sb_run(void *context)
{
	const Sb *const s = context;
	HakuxLine *const mine = s->self ? &sb_y : &sb_x;
	HakuxLine *const theirs = s->self ? &sb_x : &sb_y;
	volatile uint32_t *const my_arrival = &sb_arrive[s->self].value;
	volatile uint32_t *const their_arrival = &sb_arrive[!s->self].value;
	uint32_t i = 1;
	for (; i <= SB_ITERATIONS; i++) {
		*my_arrival = i;
		uint32_t spins = 0;
		while (*their_arrival < i) {
			if (phase_over(++spins))
				goto done;
		}
		mine->value = i;
		fence(s->fence, 0);
		sb_old[s->self][i - 1] = theirs->value < i;
		if (phase_over(i))
			break;
	}
done:
	sb_rounds[s->self] = i - 1;
	return 0;
}

static void store_buffering(Fence kind, uint32_t second_hardware_thread, bool must_hold)
{
	hakux_step("store buffering with %s, hardware threads %u and %u, up to %u rounds or %u s", FENCE_NAMES[kind],
		(unsigned)WRITER_HARDWARE_THREAD, (unsigned)second_hardware_thread, (unsigned)SB_ITERATIONS,
		(unsigned)PHASE_SECONDS);
	sb_x.value = 0;
	sb_y.value = 0;
	sb_arrive[0].value = 0;
	sb_arrive[1].value = 0;
	for (uint32_t i = 0; i < SB_ITERATIONS; i++)
		sb_old[0][i] = sb_old[1][i] = 0;
	hakux_sync();
	phase_begin();
	Sb s0 = { 0, kind };
	Sb s1 = { 1, kind };
	HANDLE t1 = hakux_spawn(sb_run, &s1, second_hardware_thread);
	HANDLE t0 = hakux_spawn(sb_run, &s0, WRITER_HARDWARE_THREAD);
	hakux_join(t0, (PHASE_SECONDS + 30) * 1000);
	hakux_join(t1, (PHASE_SECONDS + 30) * 1000);
	const uint32_t rounds = sb_rounds[0] < sb_rounds[1] ? sb_rounds[0] : sb_rounds[1];
	uint32_t both = 0;
	uint32_t one = 0;
	for (uint32_t i = 0; i < rounds; i++) {
		both += sb_old[0][i] && sb_old[1][i];
		one += sb_old[0][i] != sb_old[1][i];
	}
	xsdk_log_write("order: sb %s threads %u,%u: %u rounds, %u with both loads old, %u with one", FENCE_NAMES[kind],
		(unsigned)WRITER_HARDWARE_THREAD, (unsigned)second_hardware_thread, (unsigned)rounds, (unsigned)both,
		(unsigned)one);
	if (must_hold)
		hakux_check(both == 0 && rounds > 0, "store buffering with %s never lets both loads miss (%u in %u rounds)",
			FENCE_NAMES[kind], (unsigned)both, (unsigned)rounds);
	else
		xsdk_log_write("INFO: order: store buffering with %s: both loads old in %u of %u rounds (allowed by the "
			"architecture, reported only)",
			FENCE_NAMES[kind], (unsigned)both, (unsigned)rounds);
}

// The ring: slots of four words, each encoding the item's sequence number differently, so a slot read part-old shows.
typedef struct {
	volatile uint32_t a;
	volatile uint32_t b;
	volatile uint32_t c;
	volatile uint32_t d;
} Slot;

enum { SLOT_XOR = 0x5A5A5A5Au, SLOT_ADD = 0x13579BDFu };

static Slot ring_slots[RING_SLOTS] __attribute__((aligned(HAKUX_LINE_BYTES)));
static HakuxLine ring_write;
static HakuxLine ring_read;

typedef struct {
	Fence reader_fence;
	uint32_t written;
	uint32_t read;
	uint32_t stale;
	uint32_t newer;
	uint32_t polls;
} Ring;

static uint32_t ring_writer(void *context)
{
	Ring *const ring = context;
	uint32_t w = 0;
	uint32_t spins = 0;
	while (w < RING_ITEMS) {
		if (w - ring_read.value >= RING_SLOTS) {
			if (phase_over(++spins))
				break;
			continue;
		}
		Slot *const s = &ring_slots[w % RING_SLOTS];
		s->a = w;
		s->b = w ^ SLOT_XOR;
		s->c = w + SLOT_ADD;
		s->d = ~w;
		__asm__ volatile("lwsync" : : : "memory");
		ring_write.value = w + 1;
		w++;
		if (phase_over(w))
			break;
	}
	ring->written = w;
	return 0;
}

static uint32_t ring_reader(void *context)
{
	Ring *const ring = context;
	uint32_t r = 0;
	uint32_t stale = 0;
	uint32_t newer = 0;
	uint32_t polls = 0;
	while (r < RING_ITEMS) {
		const uint32_t w = ring_write.value;
		polls++;
		if (w == r) {
			if (phase_over(polls))
				break;
			continue;
		}
		fence(ring->reader_fence, w);
		while (r != w) {
			const Slot *const s = &ring_slots[r % RING_SLOTS];
			const uint32_t seen[4] = { s->a, s->b ^ SLOT_XOR, s->c - SLOT_ADD, ~s->d };
			bool old = false;
			bool ahead = false;
			for (uint32_t k = 0; k < 4; k++) {
				old = old || (int32_t)(seen[k] - r) < 0;
				ahead = ahead || (int32_t)(seen[k] - r) > 0;
			}
			stale += old;
			newer += ahead;
			r++;
			ring_read.value = r;
		}
		if (stop)
			break;
	}
	ring->read = r;
	ring->stale = stale;
	ring->newer = newer;
	ring->polls = polls;
	stop = 1;
	return 0;
}

static void ring_buffer(Fence reader_fence, uint32_t reader_hardware_thread)
{
	hakux_step("ring buffer, writer lwsync on hardware thread %u, reader %s on %u, up to %u items or %u s",
		(unsigned)WRITER_HARDWARE_THREAD, FENCE_NAMES[reader_fence], (unsigned)reader_hardware_thread,
		(unsigned)RING_ITEMS, (unsigned)PHASE_SECONDS);
	for (uint32_t i = 0; i < RING_SLOTS; i++) {
		// Slot i first holds item i - RING_SLOTS, so an unwritten slot reads as old.
		const uint32_t before = i - RING_SLOTS;
		ring_slots[i] = (Slot){ before, before ^ SLOT_XOR, before + SLOT_ADD, ~before };
	}
	ring_write.value = 0;
	ring_read.value = 0;
	hakux_sync();
	phase_begin();
	Ring ring = { reader_fence, 0, 0, 0, 0, 0 };
	HANDLE reader = hakux_spawn(ring_reader, &ring, reader_hardware_thread);
	HANDLE writer = hakux_spawn(ring_writer, &ring, WRITER_HARDWARE_THREAD);
	hakux_join(writer, (PHASE_SECONDS + 30) * 1000);
	hakux_join(reader, (PHASE_SECONDS + 30) * 1000);
	xsdk_log_write("order: ring reader=%s threads %u,%u: %u written, %u read, %u polls, %u stale slots, %u newer "
		"(overwritten before read)",
		FENCE_NAMES[reader_fence], (unsigned)WRITER_HARDWARE_THREAD, (unsigned)reader_hardware_thread,
		(unsigned)ring.written, (unsigned)ring.read, (unsigned)ring.polls, (unsigned)ring.stale,
		(unsigned)ring.newer);
	hakux_check(ring.stale == 0 && ring.read > 0,
		"ring with reader %s on hardware thread %u never reads a stale slot (%u stale in %u items)",
		FENCE_NAMES[reader_fence], (unsigned)reader_hardware_thread, (unsigned)ring.stale, (unsigned)ring.read);
	xsdk_log_write("INFO: order: ring reader=%s on %u: %u slots overwritten before the reader's loads (a load-store "
		"reordering on the reader's side, reported only)",
		FENCE_NAMES[reader_fence], (unsigned)reader_hardware_thread, (unsigned)ring.newer);
}

int main(void)
{
	hakux_begin("order", "hakuX-Boosted memory-ordering litmus tests between hardware threads", TIME_LIMIT_SECONDS);
	// The Gears of War shape first, then its controls.
	ring_buffer(FENCE_NONE, OTHER_CORE_HARDWARE_THREAD);
	ring_buffer(FENCE_NONE, SAME_CORE_HARDWARE_THREAD);
	ring_buffer(FENCE_LWSYNC, OTHER_CORE_HARDWARE_THREAD);
	message_passing(FENCE_NONE, FENCE_NONE, OTHER_CORE_HARDWARE_THREAD, false);
	message_passing(FENCE_LWSYNC, FENCE_NONE, OTHER_CORE_HARDWARE_THREAD, false);
	message_passing(FENCE_LWSYNC, FENCE_LWSYNC, OTHER_CORE_HARDWARE_THREAD, true);
	message_passing(FENCE_LWSYNC, FENCE_CTRL_ISYNC, OTHER_CORE_HARDWARE_THREAD, true);
	message_passing(FENCE_SYNC, FENCE_SYNC, OTHER_CORE_HARDWARE_THREAD, true);
	message_passing(FENCE_LWSYNC, FENCE_LWSYNC, SAME_CORE_HARDWARE_THREAD, true);
	store_buffering(FENCE_NONE, OTHER_CORE_HARDWARE_THREAD, false);
	store_buffering(FENCE_LWSYNC, OTHER_CORE_HARDWARE_THREAD, false);
	store_buffering(FENCE_SYNC, OTHER_CORE_HARDWARE_THREAD, true);
	store_buffering(FENCE_SYNC, SAME_CORE_HARDWARE_THREAD, true);
	return hakux_end();
}
