// hakuX-Boosted: kernel waits across hardware threads, under load. Events (auto and manual), semaphores, mutants,
// wait-any and wait-all with and without timeouts, wait-all including thread objects that end, signal-and-wait, each
// for thousands of rounds with the parties on different hardware threads. Every wait has a timeout, so a lost wake-up
// shows as a FAIL rather than a hang; the watchdog in hakux_mt.h bounds the whole title.
#include "hakux_mt.h"

enum {
	TIME_LIMIT_SECONDS = 300,
	// A wait that should be satisfied soon; generous, as the title also runs in an emulator.
	WAIT_MS = 2000,
	PINGPONG_ROUNDS = 5000,
	PINGPONG_LOADED_ROUNDS = 500,
	BROADCAST_WORKERS = 4,
	BROADCAST_ROUNDS = 1000,
	SEMAPHORE_ITEMS = 5000,
	SEMAPHORE_SLOTS = 16,
	MUTANT_WORKERS = 4,
	MUTANT_ROUNDS = 2000,
	ANY_WORKERS = 4,
	ANY_ROUNDS = 1000,
	ALL_WORKERS = 3,
	ALL_ROUNDS = 1000,
	THREAD_ROUNDS = 300,
	THREE_THREAD_ROUNDS = 200,
	SIGNAL_WAIT_ROUNDS = 5000,
	TIMEOUT_ROUNDS = 200,
	POLL_ROUNDS = 1000,
	SPINNERS = 5,
	// NT's status for a release past a semaphore's limit.
	STATUS_SEMAPHORE_LIMIT_EXCEEDED_ = (int32_t)0xC0000047,
};

static HANDLE event(uint32_t type, uint32_t state)
{
	HANDLE handle;
	const NTSTATUS status = NtCreateEvent(&handle, NULL, type, state);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("waits: NtCreateEvent failed: 0x%08X", (unsigned)status);
		hakux_abort("an event could not be created");
	}
	return handle;
}

static HANDLE semaphore(int32_t count, int32_t limit)
{
	HANDLE handle;
	const NTSTATUS status = NtCreateSemaphore(&handle, NULL, count, limit);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("waits: NtCreateSemaphore failed: 0x%08X", (unsigned)status);
		hakux_abort("a semaphore could not be created");
	}
	return handle;
}

static HANDLE mutant(uint32_t owned)
{
	HANDLE handle;
	const NTSTATUS status = NtCreateMutant(&handle, NULL, owned);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("waits: NtCreateMutant failed: 0x%08X", (unsigned)status);
		hakux_abort("a mutant could not be created");
	}
	return handle;
}

// Background load: threads that spin until told to stop, so waiters compete for their hardware threads.
static volatile uint32_t spinners_stop;
static volatile uint32_t spinner_counts[SPINNERS];

static uint32_t spin(void *context)
{
	volatile uint32_t *const count = context;
	while (!spinners_stop)
		(*count)++;
	return 0;
}

static void spinners_start(HANDLE *threads, uint32_t count, uint32_t first_hardware_thread)
{
	spinners_stop = 0;
	for (uint32_t i = 0; i < count; i++)
		threads[i] = hakux_spawn(spin, (void *)&spinner_counts[i], (first_hardware_thread + i) % XSDK_HARDWARE_THREADS);
}

static void spinners_end(HANDLE *threads, uint32_t count)
{
	spinners_stop = 1;
	for (uint32_t i = 0; i < count; i++)
		hakux_join(threads[i], WAIT_MS * 5);
}

// 1. Two auto-reset events, one each way.
typedef struct {
	HANDLE ping;
	HANDLE pong;
	uint32_t rounds;
	uint32_t done;
	NTSTATUS bad;
} PingPong;

static uint32_t pong(void *context)
{
	PingPong *const p = context;
	for (uint32_t i = 0; i < p->rounds; i++) {
		const NTSTATUS status = hakux_wait_ms(p->ping, WAIT_MS);
		if (status != STATUS_SUCCESS) {
			p->bad = status;
			return 1;
		}
		p->done++;
		if (!NT_SUCCESS(NtSetEvent(p->pong, NULL))) {
			p->bad = STATUS_UNSUCCESSFUL;
			return 1;
		}
	}
	return 0;
}

static void pingpong(uint32_t hardware_thread, uint32_t rounds, const char *load)
{
	PingPong p = { event(SYNCHRONIZATION_EVENT, 0), event(SYNCHRONIZATION_EVENT, 0), rounds, 0, STATUS_SUCCESS };
	hakux_step("auto-event ping-pong with hardware thread %u, %u rounds, %s", (unsigned)hardware_thread,
		(unsigned)rounds, load);
	HANDLE thread = hakux_spawn(pong, &p, hardware_thread);
	uint32_t round = 0;
	NTSTATUS bad = STATUS_SUCCESS;
	const int64_t start = hakux_now_ns();
	for (; round < rounds; round++) {
		if (!NT_SUCCESS(NtSetEvent(p.ping, NULL))) {
			bad = STATUS_UNSUCCESSFUL;
			break;
		}
		const NTSTATUS status = hakux_wait_ms(p.pong, WAIT_MS);
		if (status != STATUS_SUCCESS) {
			bad = status;
			break;
		}
	}
	const int64_t elapsed = hakux_now_ns() - start;
	hakux_join(thread, WAIT_MS * 2);
	const bool drained = hakux_wait_ms(p.ping, 0) == STATUS_TIMEOUT && hakux_wait_ms(p.pong, 0) == STATUS_TIMEOUT;
	xsdk_log_write("waits: ping-pong %s: %u of %u rounds, %lld ns a round trip, main 0x%08X worker 0x%08X", load,
		(unsigned)round, (unsigned)rounds, (long long)(round ? elapsed / round : 0), (unsigned)bad,
		(unsigned)p.bad);
	hakux_check(round == rounds && p.done == rounds && bad == STATUS_SUCCESS && p.bad == STATUS_SUCCESS,
		"auto-event ping-pong with hardware thread %u, %s: every wake-up arrives (%u of %u)", (unsigned)hardware_thread,
		load, (unsigned)round, (unsigned)rounds);
	hakux_check(drained, "auto-event ping-pong, %s: each wake consumed its signal, none left over", load);
	(void)NtClose(p.ping);
	(void)NtClose(p.pong);
}

// 2. A manual-reset event wakes every waiter; two of them alternate so a fast waiter cannot pass twice.
typedef struct {
	HANDLE go[2];
	HANDLE done;
	volatile uint32_t arrived;
} Broadcast;

typedef struct {
	Broadcast *b;
	uint32_t rounds;
	NTSTATUS bad;
} BroadcastWorker;

static uint32_t broadcast_wait(void *context)
{
	BroadcastWorker *const w = context;
	for (uint32_t r = 0; r < BROADCAST_ROUNDS; r++) {
		const NTSTATUS status = hakux_wait_ms(w->b->go[r & 1], WAIT_MS);
		if (status != STATUS_SUCCESS) {
			w->bad = status;
			return 1;
		}
		__atomic_add_fetch(&w->b->arrived, 1, __ATOMIC_SEQ_CST);
		w->rounds++;
		if (!NT_SUCCESS(NtReleaseSemaphore(w->b->done, 1, NULL))) {
			w->bad = STATUS_UNSUCCESSFUL;
			return 1;
		}
	}
	return 0;
}

static void broadcast(void)
{
	Broadcast b = { { event(NOTIFICATION_EVENT, 0), event(NOTIFICATION_EVENT, 0) },
		semaphore(0, BROADCAST_WORKERS * 2), 0 };
	BroadcastWorker workers[BROADCAST_WORKERS];
	HANDLE threads[BROADCAST_WORKERS];
	hakux_step("manual-event broadcast to %u waiters on hardware threads 1-4, %u rounds", (unsigned)BROADCAST_WORKERS,
		(unsigned)BROADCAST_ROUNDS);
	for (uint32_t i = 0; i < BROADCAST_WORKERS; i++) {
		workers[i] = (BroadcastWorker){ &b, 0, STATUS_SUCCESS };
		threads[i] = hakux_spawn(broadcast_wait, &workers[i], 1 + i);
	}
	uint32_t round = 0;
	NTSTATUS bad = STATUS_SUCCESS;
	for (; round < BROADCAST_ROUNDS && bad == STATUS_SUCCESS; round++) {
		(void)NtClearEvent(b.go[(round + 1) & 1]);
		(void)NtSetEvent(b.go[round & 1], NULL);
		for (uint32_t k = 0; k < BROADCAST_WORKERS; k++) {
			const NTSTATUS status = hakux_wait_ms(b.done, WAIT_MS);
			if (status != STATUS_SUCCESS) {
				bad = status;
				break;
			}
		}
	}
	for (uint32_t i = 0; i < BROADCAST_WORKERS; i++)
		hakux_join(threads[i], WAIT_MS * 2);
	bool each = true;
	for (uint32_t i = 0; i < BROADCAST_WORKERS; i++)
		each = each && workers[i].rounds == BROADCAST_ROUNDS && workers[i].bad == STATUS_SUCCESS;
	xsdk_log_write("waits: broadcast: %u rounds, %u arrivals, main 0x%08X", (unsigned)round, (unsigned)b.arrived,
		(unsigned)bad);
	hakux_check(bad == STATUS_SUCCESS && each && b.arrived == BROADCAST_WORKERS * BROADCAST_ROUNDS,
		"manual-event broadcast wakes all %u waiters every round (%u of %u arrivals)", (unsigned)BROADCAST_WORKERS,
		(unsigned)b.arrived, (unsigned)(BROADCAST_WORKERS * BROADCAST_ROUNDS));
	hakux_check(hakux_wait_ms(b.done, 0) == STATUS_TIMEOUT, "manual-event broadcast: no extra wake-ups");
	(void)NtClose(b.go[0]);
	(void)NtClose(b.go[1]);
	(void)NtClose(b.done);
}

// 3. A bounded buffer of two semaphores, two producers and two consumers.
typedef struct {
	HANDLE items;
	HANDLE space;
	volatile uint32_t produced;
	volatile uint32_t consumed;
	volatile int32_t most_items;
	volatile uint32_t bad;
} Buffer;

static uint32_t produce(void *context)
{
	Buffer *const b = context;
	for (uint32_t i = 0; i < SEMAPHORE_ITEMS; i++) {
		if (hakux_wait_ms(b->space, WAIT_MS) != STATUS_SUCCESS) {
			__atomic_add_fetch(&b->bad, 1, __ATOMIC_SEQ_CST);
			return 1;
		}
		__atomic_add_fetch(&b->produced, 1, __ATOMIC_SEQ_CST);
		int32_t previous = -1;
		if (!NT_SUCCESS(NtReleaseSemaphore(b->items, 1, &previous))) {
			__atomic_add_fetch(&b->bad, 1, __ATOMIC_SEQ_CST);
			return 1;
		}
		int32_t most = b->most_items;
		while (previous + 1 > most &&
			!__atomic_compare_exchange_n(&b->most_items, &most, previous + 1, false, __ATOMIC_SEQ_CST,
				__ATOMIC_SEQ_CST)) {
		}
	}
	return 0;
}

static uint32_t consume(void *context)
{
	Buffer *const b = context;
	for (uint32_t i = 0; i < SEMAPHORE_ITEMS; i++) {
		if (hakux_wait_ms(b->items, WAIT_MS) != STATUS_SUCCESS) {
			__atomic_add_fetch(&b->bad, 1, __ATOMIC_SEQ_CST);
			return 1;
		}
		__atomic_add_fetch(&b->consumed, 1, __ATOMIC_SEQ_CST);
		if (!NT_SUCCESS(NtReleaseSemaphore(b->space, 1, NULL))) {
			__atomic_add_fetch(&b->bad, 1, __ATOMIC_SEQ_CST);
			return 1;
		}
	}
	return 0;
}

static void semaphores(void)
{
	Buffer b = { semaphore(0, SEMAPHORE_SLOTS), semaphore(SEMAPHORE_SLOTS, SEMAPHORE_SLOTS), 0, 0, 0, 0 };
	hakux_step("semaphore bounded buffer, %u slots, 2 producers (1, 3) and 2 consumers (2, 4), %u items each",
		(unsigned)SEMAPHORE_SLOTS, (unsigned)SEMAPHORE_ITEMS);
	HANDLE threads[4] = {
		hakux_spawn(produce, &b, 1),
		hakux_spawn(consume, &b, 2),
		hakux_spawn(produce, &b, 3),
		hakux_spawn(consume, &b, 4),
	};
	for (uint32_t i = 0; i < 4; i++)
		hakux_join(threads[i], (uint32_t)WAIT_MS * 30);
	xsdk_log_write("waits: semaphores: produced %u consumed %u, most items %d, failed waits %u",
		(unsigned)b.produced, (unsigned)b.consumed, (int)b.most_items, (unsigned)b.bad);
	hakux_check(b.bad == 0 && b.produced == 2 * SEMAPHORE_ITEMS && b.consumed == 2 * SEMAPHORE_ITEMS,
		"semaphore bounded buffer passes every item (%u of %u)", (unsigned)b.consumed,
		(unsigned)(2 * SEMAPHORE_ITEMS));
	hakux_check(b.most_items <= SEMAPHORE_SLOTS, "semaphore count never passes its limit (most %d of %u)",
		(int)b.most_items, (unsigned)SEMAPHORE_SLOTS);
	hakux_check(hakux_wait_ms(b.items, 0) == STATUS_TIMEOUT, "semaphore drained to zero after the run");
	const NTSTATUS over = NtReleaseSemaphore(b.space, 1, NULL);
	hakux_check(!NT_SUCCESS(over), "a release past the limit is refused (0x%08X, NT gives 0x%08X)", (unsigned)over,
		(unsigned)STATUS_SEMAPHORE_LIMIT_EXCEEDED_);
	(void)NtClose(b.items);
	(void)NtClose(b.space);
}

// 4. A mutant guarding a read-modify-write that is not atomic, taken recursively.
typedef struct {
	HANDLE mutant;
	volatile uint32_t shared;
	volatile uint32_t inside;
	volatile uint32_t overlaps;
	volatile uint32_t bad;
} Guarded;

static uint32_t guarded_add(void *context)
{
	Guarded *const g = context;
	for (uint32_t i = 0; i < MUTANT_ROUNDS; i++) {
		if (hakux_wait_ms(g->mutant, WAIT_MS) != STATUS_SUCCESS || hakux_wait_ms(g->mutant, 0) != STATUS_SUCCESS) {
			__atomic_add_fetch(&g->bad, 1, __ATOMIC_SEQ_CST);
			return 1;
		}
		if (__atomic_add_fetch(&g->inside, 1, __ATOMIC_SEQ_CST) != 1)
			__atomic_add_fetch(&g->overlaps, 1, __ATOMIC_SEQ_CST);
		const uint32_t value = g->shared;
		for (volatile uint32_t delay = 0; delay < 20; delay++) {
		}
		g->shared = value + 1;
		__atomic_sub_fetch(&g->inside, 1, __ATOMIC_SEQ_CST);
		if (!NT_SUCCESS(NtReleaseMutant(g->mutant, 0)) || !NT_SUCCESS(NtReleaseMutant(g->mutant, 0))) {
			__atomic_add_fetch(&g->bad, 1, __ATOMIC_SEQ_CST);
			return 1;
		}
	}
	return 0;
}

static void mutants(void)
{
	Guarded g = { mutant(0), 0, 0, 0, 0 };
	HANDLE threads[MUTANT_WORKERS];
	hakux_step("mutant contention, %u threads on hardware threads 1-4, %u rounds each, recursive acquire",
		(unsigned)MUTANT_WORKERS, (unsigned)MUTANT_ROUNDS);
	for (uint32_t i = 0; i < MUTANT_WORKERS; i++)
		threads[i] = hakux_spawn(guarded_add, &g, 1 + i);
	for (uint32_t i = 0; i < MUTANT_WORKERS; i++)
		hakux_join(threads[i], (uint32_t)WAIT_MS * 30);
	xsdk_log_write("waits: mutants: total %u, overlaps %u, failed waits or releases %u", (unsigned)g.shared,
		(unsigned)g.overlaps, (unsigned)g.bad);
	hakux_check(g.bad == 0 && g.overlaps == 0 && g.shared == MUTANT_WORKERS * MUTANT_ROUNDS,
		"mutant excludes the others: total %u of %u, %u overlaps", (unsigned)g.shared,
		(unsigned)(MUTANT_WORKERS * MUTANT_ROUNDS), (unsigned)g.overlaps);
	hakux_check(NtReleaseMutant(g.mutant, 0) == STATUS_MUTANT_NOT_OWNED, "mutant is free and unowned after the run");
	(void)NtClose(g.mutant);
}

// 5 and 6. Workers that each signal their own auto event, then wait for an acknowledgement.
typedef struct {
	HANDLE signal;
	HANDLE ack;
	uint32_t rounds;
	uint32_t done;
	NTSTATUS bad;
} Signaller;

static uint32_t signaller(void *context)
{
	Signaller *const s = context;
	for (uint32_t i = 0; i < s->rounds; i++) {
		if (!NT_SUCCESS(NtSetEvent(s->signal, NULL))) {
			s->bad = STATUS_UNSUCCESSFUL;
			return 1;
		}
		const NTSTATUS status = hakux_wait_ms(s->ack, WAIT_MS);
		if (status != STATUS_SUCCESS) {
			s->bad = status;
			return 1;
		}
		s->done++;
	}
	return 0;
}

static void wait_any(void)
{
	hakux_step("wait-any picks the lowest signalled index, single thread");
	HANDLE events[ANY_WORKERS];
	HANDLE acks[ANY_WORKERS];
	for (uint32_t i = 0; i < ANY_WORKERS; i++) {
		events[i] = event(SYNCHRONIZATION_EVENT, 0);
		acks[i] = event(SYNCHRONIZATION_EVENT, 0);
	}
	(void)NtSetEvent(events[3], NULL);
	(void)NtSetEvent(events[1], NULL);
	const NTSTATUS first = hakux_wait_many_ms(ANY_WORKERS, events, WAIT_ANY, 0);
	const NTSTATUS second = hakux_wait_many_ms(ANY_WORKERS, events, WAIT_ANY, 0);
	const NTSTATUS third = hakux_wait_many_ms(ANY_WORKERS, events, WAIT_ANY, 0);
	hakux_check(first == STATUS_WAIT_0 + 1 && second == STATUS_WAIT_0 + 3 && third == STATUS_TIMEOUT,
		"wait-any returns 1, then 3, then times out, consuming one auto event each (0x%X 0x%X 0x%X)",
		(unsigned)first, (unsigned)second, (unsigned)third);

	hakux_step("wait-any across %u signalling threads (1-4), %u rounds each", (unsigned)ANY_WORKERS,
		(unsigned)ANY_ROUNDS);
	Signaller workers[ANY_WORKERS];
	HANDLE threads[ANY_WORKERS];
	for (uint32_t i = 0; i < ANY_WORKERS; i++) {
		workers[i] = (Signaller){ events[i], acks[i], ANY_ROUNDS, 0, STATUS_SUCCESS };
		threads[i] = hakux_spawn(signaller, &workers[i], 1 + i);
	}
	uint32_t counts[ANY_WORKERS] = { 0 };
	NTSTATUS bad = STATUS_SUCCESS;
	for (uint32_t t = 0; t < ANY_WORKERS * ANY_ROUNDS; t++) {
		const NTSTATUS status = hakux_wait_many_ms(ANY_WORKERS, events, WAIT_ANY, WAIT_MS);
		if (status < STATUS_WAIT_0 || status >= STATUS_WAIT_0 + ANY_WORKERS) {
			bad = status;
			break;
		}
		counts[status - STATUS_WAIT_0]++;
		(void)NtSetEvent(acks[status - STATUS_WAIT_0], NULL);
	}
	for (uint32_t i = 0; i < ANY_WORKERS; i++)
		hakux_join(threads[i], WAIT_MS * 2);
	bool each = true;
	for (uint32_t i = 0; i < ANY_WORKERS; i++)
		each = each && counts[i] == ANY_ROUNDS && workers[i].done == ANY_ROUNDS && workers[i].bad == STATUS_SUCCESS;
	xsdk_log_write("waits: wait-any: counts %u %u %u %u, main 0x%08X", (unsigned)counts[0], (unsigned)counts[1],
		(unsigned)counts[2], (unsigned)counts[3], (unsigned)bad);
	hakux_check(bad == STATUS_SUCCESS && each, "wait-any sees every signal from every thread exactly once");
	hakux_check(hakux_wait_many_ms(ANY_WORKERS, events, WAIT_ANY, 1) == STATUS_TIMEOUT,
		"wait-any with a 1 ms timeout times out once all are consumed");
	for (uint32_t i = 0; i < ANY_WORKERS; i++) {
		(void)NtClose(events[i]);
		(void)NtClose(acks[i]);
	}
}

typedef struct {
	HANDLE event;
	uint32_t delay_ms;
} Late;

static uint32_t set_late(void *context)
{
	const Late *const late = context;
	(void)xsdk_thread_sleep(late->delay_ms);
	return NT_SUCCESS(NtSetEvent(late->event, NULL)) ? 0 : 1;
}

static void wait_all(void)
{
	HANDLE events[ALL_WORKERS];
	HANDLE acks[ALL_WORKERS];
	for (uint32_t i = 0; i < ALL_WORKERS; i++) {
		events[i] = event(SYNCHRONIZATION_EVENT, 0);
		acks[i] = event(SYNCHRONIZATION_EVENT, 0);
	}

	hakux_step("wait-all with one object missing times out and consumes none");
	(void)NtSetEvent(events[0], NULL);
	(void)NtSetEvent(events[1], NULL);
	const NTSTATUS missing = hakux_wait_many_ms(ALL_WORKERS, events, WAIT_ALL, 5);
	const NTSTATUS kept0 = hakux_wait_ms(events[0], 0);
	const NTSTATUS kept1 = hakux_wait_ms(events[1], 0);
	hakux_check(missing == STATUS_TIMEOUT && kept0 == STATUS_SUCCESS && kept1 == STATUS_SUCCESS,
		"wait-all short of one object times out and leaves the others signalled (0x%X 0x%X 0x%X)",
		(unsigned)missing, (unsigned)kept0, (unsigned)kept1);

	hakux_step("wait-all whose last object is set 20 ms later by hardware thread 3");
	(void)NtSetEvent(events[0], NULL);
	(void)NtSetEvent(events[1], NULL);
	Late late = { events[2], 20 };
	HANDLE setter = hakux_spawn(set_late, &late, 3);
	const NTSTATUS completed = hakux_wait_many_ms(ALL_WORKERS, events, WAIT_ALL, WAIT_MS);
	hakux_join(setter, WAIT_MS);
	hakux_check(completed == STATUS_SUCCESS &&
			hakux_wait_many_ms(ALL_WORKERS, events, WAIT_ANY, 0) == STATUS_TIMEOUT,
		"wait-all completes when another thread sets the last object, and consumes all three (0x%X)",
		(unsigned)completed);

	hakux_step("wait-all of a semaphore, an auto event, a manual event and a free mutant");
	HANDLE sem = semaphore(2, 4);
	HANDLE manual = event(NOTIFICATION_EVENT, 1);
	HANDLE mut = mutant(0);
	(void)NtSetEvent(events[0], NULL);
	const HANDLE mixed[4] = { sem, events[0], manual, mut };
	const NTSTATUS all_mixed = hakux_wait_many_ms(4, mixed, WAIT_ALL, WAIT_MS);
	int32_t sem_previous = -1;
	const NTSTATUS sem_released = NtReleaseSemaphore(sem, 1, &sem_previous);
	const NTSTATUS auto_after = hakux_wait_ms(events[0], 0);
	const NTSTATUS manual_after = hakux_wait_ms(manual, 0);
	const NTSTATUS mut_release = NtReleaseMutant(mut, 0);
	const NTSTATUS mut_again = NtReleaseMutant(mut, 0);
	hakux_check(all_mixed == STATUS_SUCCESS && NT_SUCCESS(sem_released) && sem_previous == 1 &&
			auto_after == STATUS_TIMEOUT && manual_after == STATUS_SUCCESS && NT_SUCCESS(mut_release) &&
			mut_again == STATUS_MUTANT_NOT_OWNED,
		"mixed wait-all takes one semaphore count (left %d), clears the auto event, keeps the manual one, owns the mutant",
		(int)sem_previous);
	(void)NtClose(sem);
	(void)NtClose(manual);
	(void)NtClose(mut);

	hakux_step("wait-all across %u signalling threads (1, 2, 4), %u rounds", (unsigned)ALL_WORKERS,
		(unsigned)ALL_ROUNDS);
	static const uint32_t places[ALL_WORKERS] = { 1, 2, 4 };
	Signaller workers[ALL_WORKERS];
	HANDLE threads[ALL_WORKERS];
	for (uint32_t i = 0; i < ALL_WORKERS; i++) {
		workers[i] = (Signaller){ events[i], acks[i], ALL_ROUNDS, 0, STATUS_SUCCESS };
		threads[i] = hakux_spawn(signaller, &workers[i], places[i]);
	}
	uint32_t round = 0;
	uint32_t leftovers = 0;
	NTSTATUS bad = STATUS_SUCCESS;
	for (; round < ALL_ROUNDS; round++) {
		const NTSTATUS status = hakux_wait_many_ms(ALL_WORKERS, events, WAIT_ALL, WAIT_MS);
		if (status != STATUS_SUCCESS) {
			bad = status;
			break;
		}
		if (hakux_wait_many_ms(ALL_WORKERS, events, WAIT_ANY, 0) != STATUS_TIMEOUT)
			leftovers++;
		for (uint32_t i = 0; i < ALL_WORKERS; i++)
			(void)NtSetEvent(acks[i], NULL);
	}
	for (uint32_t i = 0; i < ALL_WORKERS; i++)
		hakux_join(threads[i], WAIT_MS * 2);
	xsdk_log_write("waits: wait-all: %u of %u rounds, %u left signalled, main 0x%08X", (unsigned)round,
		(unsigned)ALL_ROUNDS, (unsigned)leftovers, (unsigned)bad);
	hakux_check(bad == STATUS_SUCCESS && round == ALL_ROUNDS && leftovers == 0,
		"wait-all across threads completes every round and consumes every event (%u of %u, %u left over)",
		(unsigned)round, (unsigned)ALL_ROUNDS, (unsigned)leftovers);
	for (uint32_t i = 0; i < ALL_WORKERS; i++) {
		(void)NtClose(events[i]);
		(void)NtClose(acks[i]);
	}
}

// 7. Wait-all including thread objects, which become signalled, and stay signalled, when the thread ends.
typedef struct {
	HANDLE start;
	uint32_t how;
} Ending;

static uint32_t end_thread(void *context)
{
	const Ending *const e = context;
	if (hakux_wait_ms(e->start, WAIT_MS) != STATUS_SUCCESS)
		return 0xBAD;
	switch (e->how) {
	case 1:
		ExTerminateThread(1);
		break;
	case 2:
		for (volatile uint32_t i = 0; i < 20000; i++) {
		}
		break;
	default:
		break;
	}
	return e->how;
}

static void wait_all_threads(void)
{
	hakux_step("wait-all of a thread that ends and an auto event, %u rounds on hardware threads 1-5",
		(unsigned)THREAD_ROUNDS);
	HANDLE start = event(NOTIFICATION_EVENT, 0);
	HANDLE ev = event(SYNCHRONIZATION_EVENT, 0);
	uint32_t early_ok = 0;
	uint32_t ended_ok = 0;
	uint32_t stays_ok = 0;
	uint32_t consumed_ok = 0;
	NTSTATUS first_bad = STATUS_SUCCESS;
	for (uint32_t r = 0; r < THREAD_ROUNDS; r++) {
		Ending e = { start, r % 3 };
		(void)NtClearEvent(start);
		HANDLE thread = hakux_spawn(end_thread, &e, 1 + r % 5);
		(void)NtSetEvent(ev, NULL);
		const HANDLE both[2] = { thread, ev };
		// The thread waits for start, so this must time out without taking the event.
		if (hakux_wait_many_ms(2, both, WAIT_ALL, 1) == STATUS_TIMEOUT)
			early_ok++;
		(void)NtSetEvent(start, NULL);
		// Every fourth round the thread has surely ended before the wait; otherwise the wait races its end.
		if (r % 4 == 3)
			(void)hakux_wait_ms(thread, WAIT_MS);
		const NTSTATUS status = hakux_wait_many_ms(2, both, WAIT_ALL, WAIT_MS);
		if (status == STATUS_SUCCESS)
			ended_ok++;
		else if (first_bad == STATUS_SUCCESS)
			first_bad = status;
		if (hakux_wait_ms(thread, 0) == STATUS_SUCCESS)
			stays_ok++;
		if (hakux_wait_ms(ev, 0) == STATUS_TIMEOUT)
			consumed_ok++;
		else
			(void)NtClearEvent(ev);
		hakux_join(thread, WAIT_MS);
	}
	xsdk_log_write("waits: thread wait-all: early timeout %u, completed %u, thread stays signalled %u, event "
		"consumed %u of %u; first bad 0x%08X",
		(unsigned)early_ok, (unsigned)ended_ok, (unsigned)stays_ok, (unsigned)consumed_ok, (unsigned)THREAD_ROUNDS,
		(unsigned)first_bad);
	hakux_check(early_ok == THREAD_ROUNDS,
		"wait-all on a running thread and a set event times out (%u of %u)", (unsigned)early_ok,
		(unsigned)THREAD_ROUNDS);
	hakux_check(ended_ok == THREAD_ROUNDS,
		"wait-all completes once the thread ends, by return or ExTerminateThread (%u of %u)", (unsigned)ended_ok,
		(unsigned)THREAD_ROUNDS);
	hakux_check(stays_ok == THREAD_ROUNDS && consumed_ok == THREAD_ROUNDS,
		"the ended thread stays signalled (%u) and the event is consumed (%u), of %u", (unsigned)stays_ok,
		(unsigned)consumed_ok, (unsigned)THREAD_ROUNDS);

	hakux_step("wait-all of three threads ending together on hardware threads 1, 3, 5, %u rounds",
		(unsigned)THREE_THREAD_ROUNDS);
	uint32_t three_ok = 0;
	for (uint32_t r = 0; r < THREE_THREAD_ROUNDS; r++) {
		Ending e[3] = { { start, r % 3 }, { start, (r + 1) % 3 }, { start, (r + 2) % 3 } };
		(void)NtClearEvent(start);
		HANDLE threads[3] = { hakux_spawn(end_thread, &e[0], 1), hakux_spawn(end_thread, &e[1], 3),
			hakux_spawn(end_thread, &e[2], 5) };
		(void)NtSetEvent(start, NULL);
		if (hakux_wait_many_ms(3, threads, WAIT_ALL, WAIT_MS) == STATUS_SUCCESS &&
			hakux_wait_many_ms(3, threads, WAIT_ALL, 0) == STATUS_SUCCESS)
			three_ok++;
		for (uint32_t i = 0; i < 3; i++)
			hakux_join(threads[i], WAIT_MS);
	}
	hakux_check(three_ok == THREE_THREAD_ROUNDS, "wait-all of three ending threads completes and stays complete (%u of %u)",
		(unsigned)three_ok, (unsigned)THREE_THREAD_ROUNDS);
	(void)NtClose(start);
	(void)NtClose(ev);
}

// 8. Signal-and-wait ping-pong: each side sets the other's event and sleeps on its own in one call.
typedef struct {
	HANDLE a_to_b;
	HANDLE b_to_a;
	uint32_t done;
	NTSTATUS bad;
} SignalWait;

static uint32_t signal_wait_b(void *context)
{
	SignalWait *const s = context;
	const int64_t timeout = -(int64_t)WAIT_MS * 10000;
	NTSTATUS status = hakux_wait_ms(s->a_to_b, WAIT_MS);
	for (uint32_t i = 1; i < SIGNAL_WAIT_ROUNDS && status == STATUS_SUCCESS; i++) {
		s->done++;
		status = NtSignalAndWaitForSingleObjectEx(s->b_to_a, s->a_to_b, 0, 0, &timeout);
	}
	if (status != STATUS_SUCCESS) {
		s->bad = status;
		return 1;
	}
	s->done++;
	return NT_SUCCESS(NtSetEvent(s->b_to_a, NULL)) ? 0 : 1;
}

static void signal_and_wait(void)
{
	hakux_step("signal-and-wait ping-pong with hardware thread 3, %u rounds", (unsigned)SIGNAL_WAIT_ROUNDS);
	SignalWait s = { event(SYNCHRONIZATION_EVENT, 0), event(SYNCHRONIZATION_EVENT, 0), 0, STATUS_SUCCESS };
	HANDLE thread = hakux_spawn(signal_wait_b, &s, 3);
	const int64_t timeout = -(int64_t)WAIT_MS * 10000;
	uint32_t round = 0;
	NTSTATUS bad = STATUS_SUCCESS;
	for (; round < SIGNAL_WAIT_ROUNDS; round++) {
		const NTSTATUS status = NtSignalAndWaitForSingleObjectEx(s.a_to_b, s.b_to_a, 0, 0, &timeout);
		if (status != STATUS_SUCCESS) {
			bad = status;
			break;
		}
	}
	hakux_join(thread, WAIT_MS * 2);
	xsdk_log_write("waits: signal-and-wait: %u and %u of %u rounds, 0x%08X 0x%08X", (unsigned)round,
		(unsigned)s.done, (unsigned)SIGNAL_WAIT_ROUNDS, (unsigned)bad, (unsigned)s.bad);
	hakux_check(round == SIGNAL_WAIT_ROUNDS && s.done == SIGNAL_WAIT_ROUNDS && bad == STATUS_SUCCESS &&
			s.bad == STATUS_SUCCESS,
		"signal-and-wait never loses a wake-up (%u of %u)", (unsigned)round, (unsigned)SIGNAL_WAIT_ROUNDS);
	hakux_check(hakux_wait_ms(s.a_to_b, 0) == STATUS_TIMEOUT && hakux_wait_ms(s.b_to_a, 0) == STATUS_TIMEOUT,
		"signal-and-wait leaves no signal behind");
	(void)NtClose(s.a_to_b);
	(void)NtClose(s.b_to_a);
}

// 9. Timeouts while every other hardware thread spins.
static void timeouts_under_load(void)
{
	HANDLE spinners[SPINNERS];
	hakux_step("timeouts while hardware threads 1-5 spin");
	spinners_start(spinners, SPINNERS, 1);
	HANDLE never[3] = { event(SYNCHRONIZATION_EVENT, 0), event(NOTIFICATION_EVENT, 0), event(SYNCHRONIZATION_EVENT, 0) };
	uint32_t polls_ok = 0;
	int64_t poll_most = 0;
	for (uint32_t i = 0; i < POLL_ROUNDS; i++) {
		const int64_t start = hakux_now_ns();
		const NTSTATUS status = hakux_wait_ms(never[0], 0);
		const int64_t took = hakux_now_ns() - start;
		poll_most = took > poll_most ? took : poll_most;
		polls_ok += status == STATUS_TIMEOUT;
	}
	uint32_t timeouts_ok = 0;
	int64_t least = INT64_MAX;
	int64_t most = 0;
	int64_t total = 0;
	for (uint32_t i = 0; i < TIMEOUT_ROUNDS; i++) {
		const int64_t start = hakux_now_ns();
		const NTSTATUS status = i % 3 == 0 ? hakux_wait_ms(never[0], 2)
			: i % 3 == 1                   ? hakux_wait_many_ms(3, never, WAIT_ANY, 2)
										   : hakux_wait_many_ms(3, never, WAIT_ALL, 2);
		const int64_t took = hakux_now_ns() - start;
		least = took < least ? took : least;
		most = took > most ? took : most;
		total += took;
		timeouts_ok += status == STATUS_TIMEOUT;
	}
	pingpong(2, PINGPONG_LOADED_ROUNDS, "every hardware thread loaded");
	spinners_end(spinners, SPINNERS);
	xsdk_log_write("waits: under load: 0 ms polls %u of %u timed out, slowest %lld ns; 2 ms waits %u of %u timed out, "
		"%lld..%lld ns, mean %lld ns",
		(unsigned)polls_ok, (unsigned)POLL_ROUNDS, (long long)poll_most, (unsigned)timeouts_ok,
		(unsigned)TIMEOUT_ROUNDS, (long long)least, (long long)most, (long long)(total / TIMEOUT_ROUNDS));
	hakux_check(polls_ok == POLL_ROUNDS, "a 0 ms wait on an unsignalled event times out (%u of %u)", (unsigned)polls_ok,
		(unsigned)POLL_ROUNDS);
	hakux_check(timeouts_ok == TIMEOUT_ROUNDS,
		"2 ms single, wait-any and wait-all waits time out under load (%u of %u)", (unsigned)timeouts_ok,
		(unsigned)TIMEOUT_ROUNDS);
	hakux_check(least >= 1000000 && most <= 200000000,
		"2 ms timeouts under load last between 1 ms and 200 ms (%lld..%lld ns)", (long long)least, (long long)most);
	for (uint32_t i = 0; i < 3; i++)
		(void)NtClose(never[i]);
}

int main(void)
{
	hakux_begin("waits", "hakuX-Boosted kernel waits across hardware threads", TIME_LIMIT_SECONDS);
	pingpong(1, PINGPONG_ROUNDS, "same core (0 and 1)");
	pingpong(4, PINGPONG_ROUNDS, "other core (0 and 4)");
	broadcast();
	semaphores();
	mutants();
	wait_any();
	wait_all();
	wait_all_threads();
	signal_and_wait();
	timeouts_under_load();
	return hakux_end();
}
