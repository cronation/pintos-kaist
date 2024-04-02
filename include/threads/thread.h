#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#include "threads/synch.h" // P2
#include "filesys/file.h" // P2
#ifdef VM
#include "vm/vm.h"
#endif

// P2
// file_list에 포함되는 구조체
struct file_elem {
	struct file_elem *migrate; // fork시에 리스트 복사를 위한 주소
	struct list_elem elem;
	struct file *file;
	int open_cnt;
	int __pad; // fd_elem과 같은 구조를 가지도록 하는 패딩
};

// fd_list에 포함되는 구조체
struct fd_elem {
	struct file_elem *migrate; // fork시에 리스트 복사를 위한 주소
	struct list_elem elem;
	struct file_elem *fe;
	int fd;
	int std_no; // stdin: 0, stdout: 1, 일반 파일: -1
};

/* States in a thread's life cycle. */
enum thread_status {
	THREAD_RUNNING,     /* Running thread. */
	THREAD_READY,       /* Not running but ready to run. */
	THREAD_BLOCKED,     /* Waiting for an event to trigger. */
	THREAD_DYING        /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

// P1-AS
#define NICE_MIN -20
#define NICE_MAX 20

/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */
struct thread {
	/* Owned by thread.c. */
	tid_t tid;                          /* Thread identifier. */
	enum thread_status status;          /* Thread state. */
	char name[16];                      /* Name (for debugging purposes). */
	int priority;                       /* Priority. */
	// P1-AS
	int nice;
	int recent_cpu; // in 17.14 format
	// P1-PS
	int ori_priority; // donate받기 전의 기존 priority
	struct list lock_list; // 쓰레드가 hold중인 lock의 리스트: donor 확인용
	struct thread *donee_t; // 쓰레드가 acquire 대기중인 lock의 holder
	// P1-AC
	int64_t wake_tick; // 깨어날 시각

	/* Shared between thread.c and synch.c. */ // AND alarm clock (P1-AC)
	struct list_elem elem;              /* List element. */
	struct list_elem elem_2; // 전역변수 all_list에 사용되는 elem (P1-AS)

#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4;                     /* Page map level 4 */
	// P2
	struct file *exe_file; // 실행중인 프로그램의 파일 구조체
	struct list fd_page_list; // file_elem, fd_elem을 저장할 페이지의 리스트
	struct list file_list; // 열려있는 파일 구조체의 리스트
	struct list fd_list; // 열려있는 fd의 리스트, fd순으로 정렬되어있음
	tid_t p_tid; // 부모 쓰레드의 tid
	struct semaphore wait_sema; // 부모가 현재 쓰레드 종료를 대기
	struct semaphore reap_sema; // 현재 쓰레드가 부모의 wait 호출을 대기
	int exit_status;
	bool is_user; // process_exit() 호출 시 print여부를 결정하는 변수
#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf;               /* Information for switching */
	unsigned magic;                     /* Detects stack overflow. */
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_sec(void); // P1-AS
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

// P1-AC
void thread_sleep_until(int64_t wake_tick);
int64_t thread_wake_sleepers(int64_t cur_tick);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);
void thread_preempt (void); // P1-AC

void thread_set_priority (int);
int thread_get_priority (void);
void thread_donate_priority(struct thread *donor, struct thread *donee); // P1-PS
void thread_recalculate_donate(struct thread *t); // P1-PS

int thread_get_load_avg (void);
int thread_get_recent_cpu (void);
void thread_set_nice (int);
int thread_get_nice (void);

bool thread_wake_tick_less(const struct list_elem *a,
	const struct list_elem *b, void *aux); // P1-AC
bool thread_priority_less(const struct list_elem *a,
	const struct list_elem *b, void *aux); // P1-AS

// P2
struct thread *thread_get_by_id(tid_t tid);
bool thread_dup_file_list(struct thread *old_t, struct thread *new_t);
void thread_clear_fd_page_list(struct thread *t);
int thread_wait(tid_t child_tid);

void do_iret (struct intr_frame *tf);


int thread_wait(tid_t child_tid); // P2

#endif /* threads/thread.h */
