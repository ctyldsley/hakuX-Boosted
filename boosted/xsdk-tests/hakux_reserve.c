// hakuX-Boosted: lwarx/stwcx. across hardware threads. A shared counter incremented by reservation loops on 2 to 6
// hardware threads must come out exact, also with plain stores hammering its cache line; and a reservation must be lost
// when another thread stores to the reserved word (a different value, the same value, or by its own stwcx.) between
// this thread's lwarx and stwcx. Written in inline assembly, as C does not emit these sequences.
#include "hakux_mt.h"

enum {
	TIME_LIMIT_SECONDS = 300,
	INCREMENTS = 200000,
	LOSS_ROUNDS = 2000,
	// Spins a handshake allows before the round is given up, as no clock is read between lwarx and stwcx.
	HANDSHAKE_SPINS = 50000000,
	NOISE_WRITERS = 2,
};

// One word incremented by lwarx/stwcx.; retries counts the times stwcx. failed and the loop went round again.
static inline __attribute__((always_inline)) void reserve_add(volatile uint32_t *word, uint32_t *retries)
{
	uint32_t value;
	uint32_t again = *retries;
	__asm__ volatile("1:\tlwarx %0,0,%2\n"
					 "\taddi %0,%0,1\n"
					 "\tstwcx. %0,0,%2\n"
					 "\tbeq+ 2f\n"
					 "\taddi %1,%1,1\n"
					 "\tb 1b\n"
					 "2:"
		: "=&b"(value), "+b"(again)
		: "r"(word)
		: "cr0", "memory");
	(void)value;
	*retries = again;
}

static inline __attribute__((always_inline)) uint32_t load_reserved(volatile uint32_t *word)
{
	uint32_t value;
	__asm__ volatile("lwarx %0,0,%1" : "=r"(value) : "r"(word) : "memory");
	return value;
}

// True if the conditional store succeeded (CR0 EQ).
static inline __attribute__((always_inline)) bool store_conditional(volatile uint32_t *word, uint32_t value)
{
	uint32_t cr;
	__asm__ volatile("stwcx. %1,0,%2\n\tmfcr %0" : "=r"(cr) : "r"(value), "r"(word) : "cr0", "memory");
	return (cr & 0x20000000u) != 0;
}

// The counter and, on the same 128-byte line, a word other threads store to plainly.
static struct {
	volatile uint32_t counter;
	volatile uint32_t noise;
	uint8_t pad[HAKUX_LINE_BYTES - 2 * sizeof(uint32_t)];
} __attribute__((aligned(HAKUX_LINE_BYTES))) line;

static volatile uint32_t ready;
static volatile uint32_t go;
static volatile uint32_t adders_left;

typedef struct {
	uint32_t retries;
} Adder;

static uint32_t add(void *context)
{
	Adder *const adder = context;
	__atomic_add_fetch(&ready, 1, __ATOMIC_SEQ_CST);
	while (!go) {
	}
	uint32_t retries = 0;
	for (uint32_t i = 0; i < INCREMENTS; i++)
		reserve_add(&line.counter, &retries);
	adder->retries = retries;
	__atomic_sub_fetch(&adders_left, 1, __ATOMIC_SEQ_CST);
	return 0;
}

static uint32_t make_noise(void *context)
{
	(void)context;
	__atomic_add_fetch(&ready, 1, __ATOMIC_SEQ_CST);
	while (!go) {
	}
	uint32_t i = 0;
	while (adders_left)
		line.noise = ++i;
	return 0;
}

static void contend(const uint32_t *hardware_threads, uint32_t adders, const uint32_t *noise_threads, uint32_t noisy,
	const char *where)
{
	hakux_step("%u threads add %u each by lwarx/stwcx. %s%s", (unsigned)adders, (unsigned)INCREMENTS, where,
		noisy ? ", with plain stores to the same line" : "");
	Adder state[XSDK_HARDWARE_THREADS] = { { 0 } };
	HANDLE threads[XSDK_HARDWARE_THREADS + NOISE_WRITERS];
	line.counter = 0;
	line.noise = 0;
	ready = 0;
	go = 0;
	adders_left = adders;
	hakux_sync();
	for (uint32_t i = 0; i < adders; i++)
		threads[i] = hakux_spawn(add, &state[i], hardware_threads[i]);
	for (uint32_t i = 0; i < noisy; i++)
		threads[adders + i] = hakux_spawn(make_noise, NULL, noise_threads[i]);
	const int64_t until = hakux_now_ns() + 10000000000LL;
	while (ready < adders + noisy && hakux_now_ns() < until)
		(void)xsdk_thread_sleep(1);
	const int64_t start = hakux_now_ns();
	go = 1;
	for (uint32_t i = 0; i < adders + noisy; i++)
		hakux_join(threads[i], 120000);
	const int64_t elapsed = hakux_now_ns() - start;
	uint32_t retries = 0;
	for (uint32_t i = 0; i < adders; i++)
		retries += state[i].retries;
	const uint32_t expected = adders * INCREMENTS;
	xsdk_log_write("reserve: %u threads %s%s: count %u of %u, %u stwcx. retries, %lld ms", (unsigned)adders, where,
		noisy ? " with noise" : "", (unsigned)line.counter, (unsigned)expected, (unsigned)retries,
		(long long)(elapsed / 1000000));
	hakux_check(line.counter == expected, "%u threads %s%s: the reserved counter is exact (%u of %u)",
		(unsigned)adders, where, noisy ? " with plain stores to its line" : "", (unsigned)line.counter,
		(unsigned)expected);
}

// Reservation loss: thread A takes a reservation, tells B, waits for B to act, then tries stwcx.
typedef enum {
	ACT_STORE_DIFFERENT,
	ACT_STORE_SAME,
	ACT_STWCX_SAME,
	ACT_STORE_SAME_LINE,
	ACT_STORE_OTHER_LINE,
} Act;

static const char *const ACT_NAMES[] = {
	"another thread stores a different value to the word",
	"another thread stores the same value to the word",
	"another thread stores the same value to the word by its own lwarx/stwcx.",
	"another thread stores to another word of the same 128-byte line",
	"another thread stores to a word on another line",
};

static struct {
	volatile uint32_t word;
	volatile uint32_t neighbour;
	uint8_t pad[HAKUX_LINE_BYTES - 2 * sizeof(uint32_t)];
} __attribute__((aligned(HAKUX_LINE_BYTES))) target;

static HakuxLine elsewhere;
static HakuxLine a_round;
static HakuxLine b_round;

typedef struct {
	Act act;
	uint32_t successes;
	uint32_t attempts;
	uint32_t given_up;
} Loss;

static uint32_t loss_a(void *context)
{
	Loss *const loss = context;
	for (uint32_t round = 1; round <= LOSS_ROUNDS; round++) {
		const uint32_t value = load_reserved(&target.word);
		// A store to a line of A's own does not touch the reservation.
		a_round.value = round;
		uint32_t spins = 0;
		while (b_round.value != round && spins < HANDSHAKE_SPINS)
			spins++;
		const bool stored = store_conditional(&target.word, value);
		if (spins >= HANDSHAKE_SPINS) {
			loss->given_up++;
			break;
		}
		loss->attempts++;
		loss->successes += stored;
	}
	a_round.value = UINT32_MAX;
	return 0;
}

static uint32_t loss_b(void *context)
{
	const Loss *const loss = context;
	for (uint32_t round = 1; round <= LOSS_ROUNDS; round++) {
		uint32_t spins = 0;
		while (a_round.value != round) {
			if (a_round.value == UINT32_MAX || ++spins >= HANDSHAKE_SPINS)
				return 1;
		}
		switch (loss->act) {
		case ACT_STORE_DIFFERENT:
			target.word = target.word + 1;
			break;
		case ACT_STORE_SAME:
			target.word = target.word;
			break;
		case ACT_STWCX_SAME: {
			uint32_t tries = 0;
			while (tries++ < 1000) {
				const uint32_t value = load_reserved(&target.word);
				if (store_conditional(&target.word, value))
					break;
			}
			break;
		}
		case ACT_STORE_SAME_LINE:
			target.neighbour = round;
			break;
		default:
			elsewhere.value = round;
			break;
		}
		__asm__ volatile("sync" : : : "memory");
		b_round.value = round;
	}
	return 0;
}

static void reservation_loss(Act act, uint32_t a_thread, uint32_t b_thread)
{
	hakux_step("reservation on hardware thread %u while %s (hardware thread %u), %u rounds", (unsigned)a_thread,
		ACT_NAMES[act], (unsigned)b_thread, (unsigned)LOSS_ROUNDS);
	Loss loss = { act, 0, 0, 0 };
	target.word = 0x1000;
	target.neighbour = 0;
	elsewhere.value = 0;
	a_round.value = 0;
	b_round.value = 0;
	hakux_sync();
	HANDLE b = hakux_spawn(loss_b, &loss, b_thread);
	HANDLE a = hakux_spawn(loss_a, &loss, a_thread);
	hakux_join(a, 120000);
	hakux_join(b, 120000);
	xsdk_log_write("reserve: loss, %s: stwcx. succeeded %u of %u, %u handshakes given up", ACT_NAMES[act],
		(unsigned)loss.successes, (unsigned)loss.attempts, (unsigned)loss.given_up);
	const bool complete = loss.attempts == LOSS_ROUNDS && loss.given_up == 0;
	switch (act) {
	case ACT_STORE_SAME_LINE:
		hakux_check(complete, "every handshake completed when %s", ACT_NAMES[act]);
		xsdk_log_write("INFO: reserve: when %s, stwcx. succeeded %u of %u (the console's reservation granule is the "
			"128-byte line, so it should fail; reported only)",
			ACT_NAMES[act], (unsigned)loss.successes, (unsigned)loss.attempts);
		break;
	case ACT_STORE_OTHER_LINE:
		hakux_check(complete && loss.successes * 2 >= loss.attempts,
			"stwcx. keeps its reservation when %s (%u of %u succeeded)", ACT_NAMES[act], (unsigned)loss.successes,
			(unsigned)loss.attempts);
		break;
	default:
		hakux_check(complete && loss.successes == 0, "stwcx. fails when %s (%u of %u succeeded)", ACT_NAMES[act],
			(unsigned)loss.successes, (unsigned)loss.attempts);
		break;
	}
}

int main(void)
{
	hakux_begin("reserve", "hakuX-Boosted lwarx/stwcx. across hardware threads", TIME_LIMIT_SECONDS);
	// An adder spins on hardware thread 0 too; the main thread outranks it so it still runs to start and join them.
	(void)xsdk_thread_set_priority(XSDK_CURRENT_THREAD, XSDK_PRIORITY_MAX, NULL);
	static const uint32_t same_core[] = { 0, 1 };
	static const uint32_t two_cores[] = { 0, 2 };
	static const uint32_t all[] = { 0, 2, 4, 1, 3, 5 };
	static const uint32_t noise_places[] = { 1, 3 };
	contend(same_core, 2, NULL, 0, "on one core (0, 1)");
	contend(two_cores, 2, NULL, 0, "on two cores (0, 2)");
	contend(all, 3, NULL, 0, "on three cores (0, 2, 4)");
	contend(all, 4, NULL, 0, "on 0, 2, 4, 1");
	contend(all, 5, NULL, 0, "on 0, 2, 4, 1, 3");
	contend(all, 6, NULL, 0, "on all six hardware threads");
	static const uint32_t noisy_adders[] = { 0, 2 };
	contend(noisy_adders, 2, noise_places, NOISE_WRITERS, "on 0 and 2, noise from 1 and 3");
	reservation_loss(ACT_STORE_OTHER_LINE, 2, 4);
	reservation_loss(ACT_STORE_DIFFERENT, 2, 4);
	reservation_loss(ACT_STORE_SAME, 2, 4);
	reservation_loss(ACT_STWCX_SAME, 2, 4);
	reservation_loss(ACT_STORE_SAME_LINE, 2, 4);
	reservation_loss(ACT_STORE_DIFFERENT, 2, 3);
	reservation_loss(ACT_STORE_SAME, 2, 3);
	return hakux_end();
}
