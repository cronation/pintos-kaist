#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;
static struct list sleep_list; // P1-AC

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

#define PRI_MAX_DEPTH 8 // prioriy donation 최대 재귀 깊이 (P1-PS)

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

// TEST FOR DEBUG ========================================================================
void TEST() {
	// TEST1();
	// TEST2();
}

struct testruct {
	int val;
	struct list_elem elem;
};

bool testruct_less(struct list_elem *a, struct list_elem *b, void *aux) {
	struct testruct *ta = list_entry(a, struct testruct, elem);
	struct testruct *tb = list_entry(b, struct testruct, elem);

	return ta->val < tb->val;
}

void dummy() {
	int old_pri = thread_current()->priority;
	int new_pri = 40-old_pri;
	printf("[DBG] thread %s: lowering my priority from %d to %d\n", thread_current()->name, old_pri, new_pri);
	thread_set_priority(new_pri);
	for (;;) {
		printf("[DBG] thread %s: why am i printing? i go sleep...\n", thread_current()->name);
		thread_sleep_until(99999999999);
	}
}

void TEST2() {
	// block, unblock시 
	enum intr_level old_level = intr_disable();
	printf("================================= TEST =================================\n");

	struct thread *ta = thread_create("ta", 22, dummy, NULL);
	struct thread *tb = thread_create("tb", 24, dummy, NULL);
	struct thread *tc = thread_create("tc", 25, dummy, NULL);
	struct thread *td = thread_create("td", 23, dummy, NULL);

	print_ready();

	printf("[DBG] thread %s: lowering my priority from %d to %d\n", thread_current()->name, 31, 21);
	thread_set_priority(21);

	print_ready();

	printf("========================================================================\n");
	intr_set_level(old_level);
}

void TEST1() {
	// list_insert_ordered 동작 테스트
	enum intr_level old_level = intr_disable();
	printf("================================= TEST =================================\n");

	struct list l;
	list_init(&l);

	struct testruct a1; a1.val = 1;
	struct testruct a2; a2.val = 2;
	struct testruct a3; a3.val = 3;
	struct testruct a4; a4.val = 4;
	struct testruct a5; a5.val = 5;
	struct testruct a6; a6.val = 6;
	
	// list_push_back(&l,&a1.elem);
	// list_push_back(&l,&a2.elem);
	// list_push_back(&l,&a3.elem);
	// list_push_back(&l,&a4.elem);
	// list_push_back(&l,&a5.elem);
	// list_push_back(&l,&a6.elem);

	list_insert_ordered(&l, &a1.elem, testruct_less, NULL);
	list_insert_ordered(&l, &a3.elem, testruct_less, NULL);
	list_insert_ordered(&l, &a2.elem, testruct_less, NULL);
	list_insert_ordered(&l, &a4.elem, testruct_less, NULL);
	list_insert_ordered(&l, &a6.elem, testruct_less, NULL);
	list_insert_ordered(&l, &a5.elem, testruct_less, NULL);

	int idx;
	struct testruct *a;

	printf("printing all list elements...\n");
	idx = 0;
	for (struct list_elem *e = list_begin(&l); e != list_end(&l); e = list_next(e)) {
		idx++;
		a = list_entry(e, struct testruct, elem);
		printf("#%d: val = %d\n", idx, a->val);
	}

	printf("increasing value of a3 to 10\n");
	a3.val = 10;

	list_sort_elem(&a3.elem, testruct_less, NULL);

	printf("printing all list elements...\n");
	idx = 0;
	for (struct list_elem *e = list_begin(&l); e != list_end(&l); e = list_next(e)) {
		idx++;
		a = list_entry(e, struct testruct, elem);
		printf("#%d: val = %d\n", idx, a->val);
	}

	printf("decreasing value of a1 to -10\n");
	a1.val = -10;

	list_sort_elem(&a1.elem, testruct_less, NULL);

	printf("printing all list elements...\n");
	idx = 0;
	for (struct list_elem *e = list_begin(&l); e != list_end(&l); e = list_next(e)) {
		idx++;
		a = list_entry(e, struct testruct, elem);
		printf("#%d: val = %d\n", idx, a->val);
	}

	printf("increasing value of a3 to 15\n");
	a3.val = 15;

	list_sort_elem(&a3.elem, testruct_less, NULL);

	printf("printing all list elements...\n");
	idx = 0;
	for (struct list_elem *e = list_begin(&l); e != list_end(&l); e = list_next(e)) {
		idx++;
		a = list_entry(e, struct testruct, elem);
		printf("#%d: val = %d\n", idx, a->val);
	}

	printf("========================================================================\n");
	intr_set_level(old_level);
}

// =========================================================================================

// P1-AC
// thread안의 elem에 대해 wake_tick을 비교
bool thread_wake_tick_less(const struct list_elem *a,
	const struct list_elem *b, void *aux) {
	struct thread *ta = list_entry(a, struct thread, elem);
	struct thread *tb = list_entry(b, struct thread, elem);

	return ta->wake_tick < tb->wake_tick;
}

// P1-PS
// thread안의 elem에 대해 priority를 비교
bool thread_priority_great(const struct list_elem *a,
	const struct list_elem *b, void *aux) {
	struct thread *ta = list_entry(a, struct thread, elem);
	struct thread *tb = list_entry(b, struct thread, elem);

	return ta->priority > tb->priority;
}

// lock안의 elem에 대해 semaphore.waiters의 begin의 priority를 비교
// semaphore.waiters는 priority순으로 정렬되어있다고 가정
bool lock_priority_great(const struct list_elem *a,
	const struct list_elem *b, void *aux) {
	struct lock *la = list_entry(a, struct lock, elem);
	struct lock *lb = list_entry(b, struct lock, elem);

	if (list_empty(&la->semaphore.waiters)) {
		return 0;
	} else if (list_empty(&lb->semaphore.waiters)) {
		return 1;
	} else {
		struct thread *ta = list_entry(list_begin(&la->semaphore.waiters),
										struct thread, elem);
		struct thread *tb = list_entry(list_begin(&lb->semaphore.waiters),
										struct thread, elem);
		return ta->priority > tb->priority;
	}

}

// DEBUG (P1-AC)
void print_sleepers(int64_t cur_tick) {
	enum intr_level old_level = intr_disable ();

	struct list_elem *e;
	struct thread *t;

	printf("[DEBUG] PRINTING ALL SLEEPERS (cur tick=%lld)\n", cur_tick);
	for (e = list_begin(&sleep_list); e != list_end(&sleep_list); e = e->next) {
		t = list_entry(e, struct thread, elem);
		printf("thread#%d at %p, wake_tick=%lld\n", t->tid, e, t->wake_tick);
	}
	printf("tail at %p\n", list_end(&sleep_list));
	intr_set_level (old_level);
}

// DEBUG (P1-PS)
void print_ready() {
	enum intr_level old_level = intr_disable ();

	struct list_elem *e;
	struct thread *t;

	printf("[DEBUG] PRINTING ALL READY THREADS\n");
	for (e = list_begin(&ready_list); e != list_end(&ready_list); e = e->next) {
		t = list_entry(e, struct thread, elem);
		printf("thread#%d (%s) at %p, priority=%d\n", t->tid, t->name, e, t->priority);
		// printf("thread#%d (%s) at %p\n", t->tid, t->name, e);
	}
	printf("tail at %p\n", list_end(&ready_list));
	intr_set_level (old_level);
}

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globla thread context */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&destruction_req);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();

	list_init(&sleep_list); // P1-AC
}

// P1-AC
// 쓰레드를 wake_tick까지 block
void thread_sleep_until(int64_t wake_tick) {
	struct thread *t = thread_current();
	t->wake_tick = wake_tick; // 깨어날 시각

	enum intr_level old_level = intr_disable ();

	// 현재 쓰레드를 sleep_list에 삽입
	// wake_tick에 대해 오름차순으로 정렬
	list_insert_ordered(&sleep_list, &t->elem, thread_wake_tick_less, NULL);
	thread_block();

	intr_set_level (old_level);
}

// P1-AC
// sleep_list 리스트에서 깨울 시간이 지난 쓰레드들을 unblock
int64_t thread_wake_sleepers(int64_t cur_tick) {
	if (list_empty(&sleep_list)) {
		return;
	}

	enum intr_level old_level = intr_disable ();

	struct list_elem *e;
	struct thread *t;

	for (e = list_begin(&sleep_list); e != list_end(&sleep_list); e = e->next) {
		t = list_entry(e, struct thread, elem);
		// sleep_list는 wake_tick 오름차순, priority 내림차순으로 정렬되어있음
		if (cur_tick >= t->wake_tick) {
			// 깨어날 시각이 지났으면 깨우기
			e = list_remove(e);
			e = e->prev; // thread_unblock을 하면 t의 next/prev가 바뀌므로 미리 조정
			t->wake_tick = __INT64_MAX__; // sleep중이지 않은 쓰레드의 wake_tick은 MAX로 설정
			// *** priority가 변경될 때, 해당 쓰레드가 sleep중인 경우에는 sleep_list에서 재정렬되는 것을 방지
			thread_unblock(t);
		} else {
			// 나머지는 시간이 남았으므로 스킵
			break;
		}
	}

	intr_set_level (old_level);

	if (list_empty(&sleep_list)) {
		return __INT64_MAX__;
	} else {
		// 다음으로 쓰레드를 깨울 시각을 timer_sleep()에게 전달
		t = list_entry(list_begin(&sleep_list), struct thread, elem);
		return t->wake_tick;
	}
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) {
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable ();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down (&idle_started);

	// DEBUG
	TEST();
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* Allocate thread. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */
	thread_unblock (t);

	thread_yield(); // P1-PS thread_yield()를 thread_unblock() 끝에 넣어도될까?????

	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
// void
// thread_unblock (struct thread *t) {
// 	enum intr_level old_level;

// 	ASSERT (is_thread (t));

// 	old_level = intr_disable ();
// 	ASSERT (t->status == THREAD_BLOCKED);
// 	list_push_back (&ready_list, &t->elem);
// 	t->status = THREAD_READY;
// 	intr_set_level (old_level);
// }

// P1-PS
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	list_insert_ordered (&ready_list, &t->elem, thread_priority_great, NULL);
	t->status = THREAD_READY;
	intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
		// list_push_back (&ready_list, &curr->elem);
		list_insert_ordered (&ready_list, &curr->elem, thread_priority_great, NULL); // P1-PS
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY. */
// void
// thread_set_priority (int new_priority) {
// 	thread_current ()->priority = new_priority;
// }

// P1-PS
// priority를 낮추고 필요 시 yield
void
thread_set_priority (int new_priority) {
	struct thread *t = thread_current();
	int old_priority = t->priority;

	t->ori_priority = new_priority;

	// 가장 높은 donor를 확인
	// lock_list는 각 lock 안의 최우선 donor의 priority에 대해 정렬되어있다고 가정
	int don_priority = PRI_MIN;
	if (!list_empty(&t->lock_list)) {
		struct lock *l = list_entry(list_begin(&t->lock_list), struct lock, elem);
		if (!list_empty(&l->semaphore.waiters)) {
			struct thread *donor_t = list_entry(list_begin(&l->semaphore.waiters),
												struct thread, elem);
			don_priority = donor_t->priority; // 최우선 donor
		}
	}

	// donor의 priority를 받음
	if (new_priority > don_priority) {
		t->priority = new_priority;
	} else {
		t->priority = don_priority;
	}

	// // 수정된 priority를 반영하도록 ready_list에서 재정렬
	// list_sort_elem(&t->elem, thread_priority_great, NULL);

	if (t->priority < old_priority) {
		// priority가 감소하면 양보
		thread_yield();
	}
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) {
	// return thread_don_priority(thread_current(), 0);
	return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* Let someone else run. */
		intr_disable ();
		thread_block ();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->ori_priority = priority; // P1-PS
	t->wake_tick = __INT64_MAX__; // P1-AC
	list_init(&t->lock_list); // P1-PS
	t->donee_t = NULL; // P1-PS
	
	t->magic = THREAD_MAGIC;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}
