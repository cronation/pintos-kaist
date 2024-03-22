#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

// P2
#include "threads/init.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "userprog/process.h"
#include <string.h>

//P2
#define PUTBUF_MAX 512 // stdout으로 putbuf할 때의 최대 바이트 수
// 한 페이지 안에 들어갈 수 있는 file_elem 구조체의 최대 개수
#define BLK_MAX ( (int) (PGSIZE / sizeof(struct file_elem) ) )

struct lock file_lock; // 파일 읽기/쓰기 lock

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

// P2
static void halt(void);
static void exit(int status);
static tid_t fork(const char *thread_name, struct intr_frame *if_);
static int exec(const char *cmd_line);
static int wait(tid_t tid);
static bool create(const char *file, unsigned initial_size);
static bool remove(const char *file);
static int open(const char *file_name);
static int filesize(int fd);
static int read(int fd, void *buffer, unsigned size);
static int write(int fd, const void *buffer, unsigned size);
static void seek(int fd, unsigned position);
static unsigned tell(int fd);
static void close(int fd);
static int dup2(int oldfd, int newfd); // P2-EX

static bool fd_elem_fd_less(const struct list_elem *a,
	const struct list_elem *b, void *aux UNUSED);
static bool is_valid_addr(void *p);
static void *get_new_block(void);
static struct fd_elem *get_fd_elem_in_list(int fd);
static int add_file_in_list(struct file *file);


/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
	
	lock_init(&file_lock); // P2
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
	int syscall_no = f->R.rax;
	void *arg1 = (void*) f->R.rdi;
	void *arg2 = (void*) f->R.rsi;
	void *arg3 = (void*) f->R.rdx;
	void *arg4 = (void*) f->R.r10;
	void *arg5 = (void*) f->R.r8;
	void *arg6 = (void*) f->R.r9;
	uint64_t ret = 0; // return value

	switch (syscall_no) {
		/* Projects 2 and later. */
		case SYS_HALT: /* Halt the operating system. */
			halt();
			break;
		case SYS_EXIT: /* Terminate this process. */
			exit((int) arg1);
			break;
		case SYS_FORK: /* Clone current process. */
			ret = (uint64_t) fork(arg1, f); // intr_frame을 추가로 전달
			break;
		case SYS_EXEC: /* Switch current process. */
			ret = (uint64_t) exec(arg1); // 실제로 반환되는 일 없음
			break;
		case SYS_WAIT: /* Wait for a child process to die. */
			ret = (uint64_t) wait(arg1);
			break;
		case SYS_CREATE: /* Create a file. */
			lock_acquire(&file_lock);
			ret = (uint64_t) create(arg1, arg2);
			lock_release(&file_lock);
			break;
		case SYS_REMOVE: /* Delete a file. */
			lock_acquire(&file_lock);
			ret = (uint64_t) remove(arg1);
			lock_release(&file_lock);
			break;
		case SYS_OPEN: /* Open a file. */
			lock_acquire(&file_lock);
			ret = (uint64_t) open(arg1);
			lock_release(&file_lock);
			break;
		case SYS_FILESIZE: /* Obtain a file's size. */
			ret = (uint64_t) filesize(arg1);
			break;
		case SYS_READ: /* Read from a file. */
			lock_acquire(&file_lock);
			ret = (uint64_t) read(arg1, arg2, arg3);
			lock_release(&file_lock);
			break;
		case SYS_WRITE: /* Write to a file. */\
			lock_acquire(&file_lock);
			ret = (uint64_t) write(arg1, arg2, arg3);
			lock_release(&file_lock);
			break;
		case SYS_SEEK: /* Change position in a file. */
			seek(arg1, arg2);
			break;
		case SYS_TELL: /* Report current position in a file. */
			ret = (uint64_t) tell(arg1);
			break;
		case SYS_CLOSE: /* Close a file. */
			lock_acquire(&file_lock);
			close(arg1);
			lock_release(&file_lock);
			break;
		/* Project 3 and optionally project 4. */
		case SYS_MMAP: /* Map a file into memory. */
			printf("syscall_handler(): not implemented (rax = %d)\n", syscall_no);
			break;
		case SYS_MUNMAP: /* Remove a memory mapping. */
			printf("syscall_handler(): not implemented (rax = %d)\n", syscall_no);
			break;
		/* Project 4 only. */
		case SYS_CHDIR: /* Change the current directory. */
			printf("syscall_handler(): not implemented (rax = %d)\n", syscall_no);
			break;
		case SYS_MKDIR: /* Create a directory. */
			printf("syscall_handler(): not implemented (rax = %d)\n", syscall_no);
			break;
		case SYS_READDIR: /* Reads a directory entry. */
			printf("syscall_handler(): not implemented (rax = %d)\n", syscall_no);
			break;
		case SYS_ISDIR: /* Tests if a fd represents a directory. */
			printf("syscall_handler(): not implemented (rax = %d)\n", syscall_no);
			break;
		case SYS_INUMBER: /* Returns the inode number for a fd. */
			printf("syscall_handler(): not implemented (rax = %d)\n", syscall_no);
			break;
		case SYS_SYMLINK: /* Returns the inode number for a fd. */
			printf("syscall_handler(): not implemented (rax = %d)\n", syscall_no);
			break;
		/* Extra for Project 2 */
		case SYS_DUP2: /* Duplicate the file descriptor */
			ret = (uint64_t) dup2(arg1, arg2);
			break;
		
		case SYS_MOUNT:
			printf("syscall_handler(): not implemented (rax = %d)\n", syscall_no);
			break;
		case SYS_UMOUNT:
			printf("syscall_handler(): not implemented (rax = %d)\n", syscall_no);
			break;
		default:
			printf("syscall_handler(): unknown request (rax = %d)\n", syscall_no);
	}

	f->R.rax = ret;
}

void syscall_terminate(void) {
	exit(-1);
}

// ============================= [SYSTEM CALLS] ================================

//P2
static void halt(void) {
	power_off();
}

static void exit(int status) {
	thread_current()->exit_status = status;
	thread_exit();
}

static tid_t fork(const char *thread_name, struct intr_frame *if_) {
	if (!is_valid_addr(thread_name)) {
		exit(-1);
	}

	return process_fork(thread_name, if_);
}

static int exec(const char *cmd_line) {
	if (!is_valid_addr(cmd_line)) {
		exit(-1);
	}

	char *cmd_copy = palloc_get_page(0);
	if (cmd_copy == NULL) {
		return -1;
	}

	strlcpy (cmd_copy, cmd_line, PGSIZE);
	process_exec(cmd_copy);

	// process_exec()가 실패한 경우
	exit(-1);
	NOT_REACHED();
}

static int wait(tid_t tid) {
	return process_wait(tid);
}

static bool create(const char *file, unsigned initial_size) {
	if (!is_valid_addr(file) || strlen(file) == 0) {
		lock_release(&file_lock);
		exit(-1);
	}

	return filesys_create(file, initial_size);
}

static bool remove(const char *file) {
	if (!is_valid_addr(file)) {
		lock_release(&file_lock);
		exit(-1);
	}

	return filesys_remove (file);
}

static int open(const char *file_name) {
	if (!is_valid_addr(file_name)) {
		lock_release(&file_lock);
		exit(-1);
	}

	struct file *file = filesys_open (file_name);
	if (file == NULL) {
		return -1;
	}

	return add_file_in_list(file);
}

static int filesize(int fd) {
	struct fd_elem *fde = get_fd_elem_in_list(fd);
	if (fde == NULL) {
		// fd에 해당하는 파일이 file_list에 없음
		return -1;
	}
	
	if (fde->std_no == -1) {
		// stdin이나 stdout이 아닌 진짜 파일
		return file_length(fde->fe->file);
	} else {
		return -1;
	}
}

static int read(int fd, void *buffer, unsigned size) {
	if (!is_valid_addr(buffer) || !is_valid_addr(buffer + size -1)) {
		// buffer의 시작과 끝 주소를 확인
		lock_release(&file_lock);
		exit(-1);
	}

	struct fd_elem *fde = get_fd_elem_in_list(fd);
	if (fde == NULL) {
		// fd에 해당하는 파일이 file_list에 없음
		return -1;
	}

	if (fde->std_no == STDIN_FILENO) {
		// stdin에서 읽기
		for (unsigned i = 0; i < size; i++) {
			*((uint8_t *) buffer + i) = input_getc();
		}
		return size;
	} else if (fde->std_no == STDOUT_FILENO) {
		// stdout에서 읽기: 에러
		return -1;
	} else {
		return file_read (fde->fe->file, buffer, size);
	}
}

static int write(int fd, const void *buffer, unsigned size) {
	if (!is_valid_addr(buffer) || !is_valid_addr(buffer + size -1)) {
		// buffer의 시작과 끝 주소를 확인
		lock_release(&file_lock);
		exit(-1);
	}

	struct fd_elem *fde = get_fd_elem_in_list(fd);
	if (fde == NULL) {
		// fd에 해당하는 파일이 file_list에 없음
		return -1;
	}

	if (fde->std_no == STDIN_FILENO) {
		// stdin으로 출력: 에러
		return -1;
	} else if (fde->std_no == STDOUT_FILENO) {
		// stdout으로 출력
		unsigned bytes_left = size;
		unsigned bytes_to_write;
		void *p = buffer;
		while (bytes_left > 0) {
			bytes_to_write = bytes_left < PUTBUF_MAX ? bytes_left : PUTBUF_MAX;
			putbuf(p, bytes_to_write);
			p += bytes_to_write;
			bytes_left -= bytes_to_write;
		}
		return size;
	} else {
		return file_write (fde->fe->file, buffer, size);
	}
}

static void seek(int fd, unsigned position) {
	struct fd_elem *fde = get_fd_elem_in_list(fd);
	if (fde == NULL) {
		// fd에 해당하는 파일이 file_list에 없음
		return;
	}

	if (fde->std_no == -1) {
		file_seek (fde->fe->file, position);
	}
	return;
}

static unsigned tell(int fd) {
	struct fd_elem *fde = get_fd_elem_in_list(fd);
	if (fde == NULL) {
		// fd에 해당하는 파일이 file_list에 없음
		return 0;
	}

	if (fde->std_no == -1) {
		return file_tell (fde->fe->file);
	} else {
		return 0;
	}
}

static void close(int fd) {
	struct fd_elem *fde = get_fd_elem_in_list(fd);

	if (fde == NULL) {
		// fd에 해당하는 요소가 없음
		lock_release(&file_lock);
		exit(-1);
	}

	if (fde->std_no == -1) {
		if (fde->fe->open_cnt == 1) {
			// 해당 file_elem을 참조하는 마지막 fd_elem임
			file_close(fde->fe->file);
			list_remove(&fde->fe->elem); // file_list에서 삭제
			fde->fe->elem.prev = NULL; // fd_page 상에서 빈 블록임을 표시
		}
		fde->fe->open_cnt--;
	}

	list_remove(&fde->elem); // fd_list에서 삭제
	fde->elem.prev = NULL; // fd_page 상에서 빈 블록임을 표시
}

// P2-EX
static int dup2(int oldfd, int newfd) {
	struct fd_elem *old_fde = get_fd_elem_in_list(oldfd);
	if (old_fde == NULL) {
		// fd에 해당하는 파일이 fd_list에 없음
		return -1;
	}

	if (oldfd == newfd) {
		return newfd;
	}

	struct fd_elem *new_fde = get_fd_elem_in_list(newfd);

	if (new_fde == NULL) {
		// new_fde 존재하지 않음: old_fde를 복사한 새로운 new_fde를 생성
		new_fde = (struct fd_elem *) get_new_block();
		new_fde->fd = newfd;
		new_fde->fe = old_fde->fe;
		new_fde->std_no = old_fde->std_no;

		list_insert_ordered(&thread_current()->fd_list,
							&new_fde->elem, fd_elem_fd_less, NULL);

		if (new_fde->fe) {
			new_fde->fe->open_cnt++;
		}
	} else {
		// newfd가 이미 존재: 기존 파일을 닫고 oldfd를 복사
		if (new_fde->std_no == -1) {
			// stdin이나 stdout이 아닌 진짜 파일
			new_fde->fe->open_cnt--;
			if (new_fde->fe->open_cnt == 0) {
				// 해당 file_elem을 참조하는 마지막 fd_elem인 경우
				file_close(new_fde->fe->file);
				list_remove(&new_fde->fe->elem); // file_list에서 삭제
				new_fde->fe->elem.prev = NULL; // fd_page 상에서 빈 블록임을 표시
			}
		}

		if (old_fde->std_no == -1) {
			old_fde->fe->open_cnt++;
		}

		new_fde->fe = old_fde->fe;
		new_fde->std_no = old_fde->std_no;
	}

	return new_fde->fd;
}

// ============================= [MISC FUNC] ===================================

// fd_elem의 fd를 비교
static bool fd_elem_fd_less(const struct list_elem *a,
	const struct list_elem *b, void *aux UNUSED) {
	struct fd_elem *fdea = list_entry(a, struct fd_elem, elem);
	struct fd_elem *fdeb = list_entry(b, struct fd_elem, elem);

	return fdea->fd < fdeb->fd;
}

// 유효한 주소인지 확인
static bool is_valid_addr(void *p) {
	if (p == NULL || p >= KERN_BASE) {
		return false;
	} else if (pml4_get_page (thread_current()->pml4, p) == NULL) {
		// 매핑되지 않은 주소
		return false;
	}

	return true;
}

// file_elem 또는 fd_elem을 저장하기 위한 공간을 할당
// file_elem과 fd_elem은 같은 크기와 구조를 가지고 있으므로 겸용으로 사용 가능
static void *get_new_block(void) {
	struct list *pg_list = &thread_current()->fd_page_list;
	struct list_elem *e;

	struct file_elem *new_fe = NULL;

	struct file_elem *start_p;
	struct file_elem *p;

	// fd_page_list에서 먼저 공간을 탐색
	for (e = list_begin(pg_list); e != list_end(pg_list); e = list_next(e)) {
		start_p = list_entry(e, struct file_elem, elem);
		for (int i = 1; i < BLK_MAX; i++) {
			p = start_p + i; // 현재 페이지에서 i번째 블록의 주소
			if (p->elem.prev == NULL) { // elem.prev이 NULL이면 할당되지 않은 블록
				new_fe = p;
				break;
			}
		}
	}

	if (new_fe == NULL) {
		// 할당할 빈 블록이 하나도 없음: 새 페이지를 할당
		// 페이지 헤더 역할을 할 file_elem 구조체 (실제로 file_elem으로 사용 X)
		struct file_elem *new_pg = palloc_get_page(PAL_ZERO);
		if (new_pg == NULL) {
			// 할당 실패
			exit(-1);
		}
		list_push_front(pg_list, &new_pg->elem); // fd_page_list에 삽입
		new_fe = new_pg +1; // 헤더 직후의 블록을 할당
	}

	return (void *) new_fe;
}

// fd_list에서 해당하는 fd_elem 반환
static struct fd_elem *get_fd_elem_in_list(int fd) {
	struct list *fd_list = &thread_current()->fd_list;
	struct list_elem *e;
	struct fd_elem *fde;

	for (e = list_begin(fd_list); e != list_end(fd_list); e = list_next(e)) {
		fde = list_entry(e, struct fd_elem, elem);
		if (fde->fd == fd)
			break;
	}

	if (e == list_end(fd_list)) { // fd에 해당하는 요소가 없음
		return NULL;
	}

	return fde;
}

static int add_file_in_list(struct file *file) {
	struct list *fd_list = &thread_current()->fd_list;
	struct list_elem *e;
	struct fd_elem *fde;

	// file_list를 순회하여 파일에 할당되지않은 descriptor를 탐색
	int idx = 0;
	for (e = list_begin(fd_list); e != list_end(fd_list); e = list_next(e)) {
		fde = list_entry(e, struct fd_elem, elem);
		if (fde->fd != idx) { // file_list에서 idx에 해당하는 fd가 미할당 상태
			break;
		}
		idx++;
	}

	// 새로운 file_elem 구조체
	struct file_elem *new_fe = (struct file_elem *) get_new_block();

	new_fe->file = file;
	new_fe->open_cnt = 1;
	list_push_back(&thread_current()->file_list, &new_fe->elem);

	// 새로운 fd 구조체
	struct fd_elem *new_fde = (struct fd_elem *) get_new_block();
	new_fde->fe = new_fe;
	new_fde->fd = idx;
	new_fde->std_no = -1;
	list_insert_ordered(&thread_current()->fd_list, &new_fde->elem,
						fd_elem_fd_less, NULL);

	return new_fde->fd;
}