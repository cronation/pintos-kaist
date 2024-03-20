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
#include "filesys/filesys.h"
#include "filesys/file.h"
// #include "threads/malloc.h"
#include "userprog/process.h"
// #include "threads/interrupt.h"
// #include <string.h>
#include "threads/palloc.h"
// #include "threads/synch.h"

#define PUTBUF_MAX 512 // stdout으로 putbuf할 때의 최대 바이트 수

// struct lock file_lock; // 읽기

void syscall_entry (void);
void syscall_handler (struct intr_frame *);


// FILE DESCRIPTOR //////////////////////////////


// struct list file_list; // 열려있는 파일의 리스트, fd순으로 정렬되어있음

// struct file_elem stdin_fe;
// struct file_elem stdout_fe;

// file_elem의 fd를 비교 (P2)
bool file_elem_fd_less(const struct list_elem *a,
	const struct list_elem *b, void *aux) {
	struct file_elem *fea = list_entry(a, struct file_elem, elem);
	struct file_elem *feb = list_entry(b, struct file_elem, elem);

	return fea->fd < feb->fd;
}


// file_list에서 fd에 해당하는 file_elem 구조체를 반환
static struct file_elem *get_file_in_list(int fd) {
	struct list *file_list = &thread_current()->file_list;
	struct list_elem *e;
	struct file_elem *fe;

	for (e = list_begin(file_list); e != list_end(file_list); e = list_next(e)) {
		fe = list_entry(e, struct file_elem, elem);
		if (fe->fd == fd) {
			break;
		}
	}

	if (e == list_end(file_list)) {
		// fd에 해당하는 요소가 없음
		return NULL;
	}

	return fe;
}

// file을 file_list에 추가하고 file descriptor를 반환
static int add_file_in_list(struct file *file) {
	struct list *file_list = &thread_current()->file_list;
	struct list_elem *e;
	struct file_elem *fe;

	// 파일에 할당되지않은 descriptor를 탐색
	int idx = 0;
	for (e = list_begin(file_list); e != list_end(file_list); e = list_next(e)) {
		fe = list_entry(e, struct file_elem, elem);
		if (fe->fd != idx) {
			// file_list에서 idx에 해당하는 fd가 미할당 상태
			break;
		}
		idx++;
	}

	// printf("[DBG] add_file_in_list(): new descriptor will be %d\n", idx); /////////////////////////////
	struct file_elem *new_fe = malloc(sizeof(*new_fe));
	// struct file_elem *new_fe = palloc_get_page(PAL_USER);
	new_fe->fd = idx;
	new_fe->file = file;
	new_fe->std_no = -1;
	list_insert_ordered(file_list, &new_fe->elem, file_elem_fd_less, NULL);
	// printf("[DBG] add_file_in_list(): inserted new file_elem into file_list\n"); //////////////////////

	return new_fe->fd;
}











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
	
	// list_init(&file_list); // P2
	
	// // struct file_elem *stdin_fe = malloc(sizeof(*stdin_fe));
	// struct file_elem *stdin_fe = palloc_get_page(PAL_USER);
	// stdin_fe->fd = 0;
	// stdin_fe->file = NULL;
	// stdin_fe->std_no = STDIN_FILENO;
	// // list_insert_ordered(&file_list, &stdin_fe.elem, file_elem_fd_less, NULL);
	// list_push_back(&file_list, &stdin_fe->elem);

	// // struct file_elem *stdout_fe = malloc(sizeof(*stdout_fe));
	// struct file_elem *stdout_fe = palloc_get_page(PAL_USER);
	// stdout_fe->fd = 1;
	// stdout_fe->file = NULL;
	// stdout_fe->std_no = STDOUT_FILENO;
	// // list_insert_ordered(&file_list, &stdout_fe.elem, file_elem_fd_less, NULL);
	// list_push_back(&file_list, &stdout_fe->elem);
}

static bool is_valid_addr(void *p) {
	// 유효한 주소가 아니면 terminate
	//////////////////////////////////////////////////////////////////////////// WIP

	if (p == NULL || p >= KERN_BASE) {
		// printf("[DBG] is_valid_addr(): NULL or kernel address!\n"); /////////////////////
		return false;
	} else if (pml4_get_page (thread_current()->pml4, p) == NULL) {
		// 매핑되지 않은 주소
		// printf("[DBG] is_valid_addr(): invalid address!\n"); /////////////////////
		return false;
	}

	return true;
}

///////////////////////// DEBUG
void print_if(void *if_, char *desc) {
	struct intr_frame *f = if_;
	printf("============== [DBG] {%s} %s: examining intr_frame ==============\n", thread_current()->name, desc);
	printf("r15: %p (%lld)\n", f->R.r15, f->R.r15);
	printf("r14: %p (%lld)\n", f->R.r14, f->R.r14);
	printf("r13: %p (%lld)\n", f->R.r13, f->R.r13);
	printf("r12: %p (%lld)\n", f->R.r12, f->R.r12);
	printf("r11: %p (%lld)\n", f->R.r11, f->R.r11);
	printf("r10: %p (%lld)\n", f->R.r10, f->R.r10);
	printf(" r8: %p (%lld)\n", f->R.r8, f->R.r8);
	printf(" r9: %p (%lld)\n", f->R.r9, f->R.r9);
	printf("rsi: %p (%lld)\n", f->R.rsi, f->R.rsi);
	printf("rdi: %p (%lld)\n", f->R.rdi, f->R.rdi);
	printf("rbp: %p (%lld)\n", f->R.rbp, f->R.rbp);
	printf("rdx: %p (%lld)\n", f->R.rdx, f->R.rdx);
	printf("rcx: %p (%lld)\n", f->R.rcx, f->R.rcx);
	printf("rbx: %p (%lld)\n", f->R.rbx, f->R.rbx);
	printf("rax: %p (%lld)\n", f->R.rax, f->R.rax);
	printf("\n");
	printf(" es: %p (%d)\n", f->es, f->es);
	printf(" ds: %p (%d)\n", f->ds, f->ds);
	printf("v_n: %p (%lld)\n", f->vec_no, f->vec_no);
	printf("e_c: %p (%lld)\n", f->error_code, f->error_code);
	printf("rip: %p (%lld)\n", f->rip, f->rip);
	printf(" cs: %p (%d)\n", f->cs, f->cs);
	printf("efl: %p (%lld)\n", f->eflags, f->eflags);
	printf("rsp: %p (%lld)\n", f->rsp, f->rsp);
	printf(" ss: %p (%d)\n", f->ss, f->ss);
}

////////////////////// SYSCALL FUNCITONS /////////////////////////////////////////////

static void halt(void) {
	power_off();
}

static void exit(int status) {
	thread_current()->exit_status = status;
	// printf("THREAD EXITING!!!\n"); ///////////////////
	thread_exit();
}

tid_t fork(const char *thread_name, struct intr_frame *if_) {
	if (!is_valid_addr(thread_name)) {
		exit(-1);
	}
	tid_t ret = process_fork(thread_name, if_);
	// printf("[DBG] fork(): {%s} process_fork() is done! (child tid = %d)\n", thread_current()->name, ret); ////////////

	// print_if(if_, "end of fork"); //////////////////

	// return 19;
	return ret;
	// return process_fork(thread_name, if_);
}

int exec(const char *cmd_line) {
	if (!is_valid_addr(cmd_line)) {
		exit(-1);
	}

	char *cmd_copy = palloc_get_page(0);
	// char *dummy = palloc_get_page(0); //////////////////////
	if (cmd_copy == NULL) {
		return -1;
	}

	// printf("[DBG] exec(): copying cmd_line - %s at %p\n", cmd_line, cmd_line); //////////
	// printf("[DBG] exec(): will copy to %p\n", cmd_copy); //////

	strlcpy (cmd_copy, cmd_line, PGSIZE);
	// strlcpy (dummy, cmd_copy, PGSIZE); //////////////

	// printf("[DBG] exec(): copied cmd_copy - %s at %p\n", cmd_copy, cmd_copy); ///////////////

	process_exec(cmd_copy);

	// process_exec()가 실패한 경우
	exit(-1);
	NOT_REACHED();

	return -1;
}

int wait(tid_t tid) {
	// printf("[DBG] {%s} called system cal WAIT(%d)!!!\n", thread_current()->name, tid); ///////////
	int ret = process_wait(tid);
	// printf("[DBG] syscall wait(): {%s} done waiting, exit status is %d\n", thread_current()->name, ret); //////////
	return ret;
	// return process_wait(tid);
}

static bool create(const char *file, unsigned initial_size) {
	if (!is_valid_addr(file) || strlen(file) == 0) {
		exit(-1);
	}

	return filesys_create(file, initial_size);
}

static bool remove(const char *file) {
	if (!is_valid_addr(file)) {
		exit(-1);
	}

	return filesys_remove (file);
}

static int open(const char *file_name) {
	// return 2; /////////////////////////////// 불구만들기 //////////////////////////////////////
	if (!is_valid_addr(file_name)) {
		exit(-1);
	}

	struct file *file = filesys_open (file_name);

	if (file == NULL) {
		return -1;
	}

	return add_file_in_list(file);
}

static int filesize(int fd) {
	struct file_elem *fe = get_file_in_list(fd);
	if (fe == NULL) {
		// fd에 해당하는 파일이 file_list에 없음
		return -1;
	}

	if (fe->std_no == STDIN_FILENO || fe->std_no == STDOUT_FILENO) {
		return -1;
	}
	
	if (fe->file) {
		return file_length(fe->file);
	} else {
		return -1;
	}
}

static int read(int fd, void *buffer, unsigned size) {
	if (!is_valid_addr(buffer) || !is_valid_addr(buffer + size -1)) {
		// buffer의 시작과 끝 주소를 확인
		exit(-1);
	}

	struct file_elem *fe = get_file_in_list(fd);
	if (fe == NULL) {
		// fd에 해당하는 파일이 file_list에 없음
		return -1;
	}

	if (fe->std_no == STDIN_FILENO) {
		// stdin에서 읽기
		for (unsigned i = 0; i < size; i++) {
			*((uint8_t *) buffer + i) = input_getc();
		}
		return size;
	} else if (fe->std_no == STDOUT_FILENO) {
		// stdout에서 읽기: 에러
		return -1;
	}
	
	if (fe->file) {
		// 파일에서 읽기
		return file_read (fe->file, buffer, size);
	} else {
		return -1;
	}
}

static int write(int fd, const void *buffer, unsigned size) {
	if (!is_valid_addr(buffer) || !is_valid_addr(buffer + size -1)) {
		// buffer의 시작과 끝 주소를 확인
		exit(-1);
	}

	struct file_elem *fe = get_file_in_list(fd);
	if (fe == NULL) {
		// fd에 해당하는 파일이 file_list에 없음
		return -1;
	}

	if (fe->std_no == STDIN_FILENO) {
		// stdin으로 출력: 에러
		return -1;
	} else if (fe->std_no == STDOUT_FILENO) {
		// stdout으로 출력
		// thread_yield(); //////////////////////
		// printf("[DBG] syscall write(): {%s} is now talking\n", thread_current()->name); ///////////
		unsigned bytes_left = size;
		unsigned bytes_to_write;
		void *p = buffer;
		while (bytes_left > 0) {
			bytes_to_write = bytes_left < PUTBUF_MAX ? bytes_left : PUTBUF_MAX; // min(bytes_left, PUTBUF_MAX)
			putbuf(p, bytes_to_write);
			p += bytes_to_write;
			bytes_left -= bytes_to_write;
		}
		return size;
	}
	
	if (fe->file) {
		// 파일에 쓰기
		return file_write (fe->file, buffer, size);
	} else {
		return -1;
	}
}

static void seek(int fd, unsigned position) {
	struct file_elem *fe = get_file_in_list(fd);
	if (fe == NULL) {
		// fd에 해당하는 파일이 file_list에 없음
		return;
	}

	if (fe->std_no == STDIN_FILENO || fe->std_no == STDOUT_FILENO) {
		return;
	}
	
	if (fe->file) {
		file_seek (fe->file, position);
	}
	return;
}

static unsigned tell(int fd) {
	struct file_elem *fe = get_file_in_list(fd);
	if (fe == NULL) {
		// fd에 해당하는 파일이 file_list에 없음
		return 0;
	}

	if (fe->std_no == STDIN_FILENO || fe->std_no == STDOUT_FILENO) {
		return 0;
	}
	
	if (fe->file) {
		return file_tell (fe->file);
	} else {
		return 0;
	}
}

static void close(int fd) {
	struct file_elem *fe = get_file_in_list(fd);

	if (fe == NULL) {
		// fd에 해당하는 요소가 없음
		exit(-1);
	}

	// file이 NULL이면 복사된 fd에 의해 이미 닫힌 파일

	if (fe->file != NULL) {
		// 이미 닫힌 파일이 아닌 경우
		struct list *file_list = &thread_current()->file_list;
		struct list_elem *e;
		struct file_elem *dup_fe;

		// 복사된 fd를 모두 찾아 file을 NULL로 변경
		for (e = list_begin(file_list); e != list_end(file_list); e = list_next(e)) {
			dup_fe = list_entry(e, struct file_elem, elem);
			if (dup_fe->file == fe->file) {
				dup_fe->file = NULL;
			}
		}
	}

	file_close(fe->file);
	list_remove(&fe->elem);
	free(fe);
	// palloc_free_page(fe);
}

// P2-E
int dup2(int oldfd, int newfd) {
	// return 2; //////////////////////////////////////////// 불구만들기 ////////////////////////////////////////
	struct file_elem *old_fe = get_file_in_list(oldfd);
	if (old_fe == NULL) {
		// fd에 해당하는 파일이 file_list에 없음
		return -1;
	}

	if (oldfd == newfd) {
		return newfd;
	}

	struct file_elem *new_fe = get_file_in_list(newfd);

	if (new_fe == NULL) {
		// newfd가 기존에 존재하지 않음: oldfd를 복사한 새로운 newfd를 생성
		struct list *file_list = &thread_current()->file_list;
		new_fe = malloc(sizeof(*new_fe));
		// new_fe = palloc_get_page(PAL_USER);
		new_fe->fd = newfd;
		new_fe->file = old_fe->file;
		new_fe->std_no = old_fe->std_no;
		list_insert_ordered(file_list, &new_fe->elem, file_elem_fd_less, NULL);
	} else {
		// newfd가 이미 존재: 기존 파일을 닫고 oldfd를 복사
		file_close(new_fe->file);
		new_fe->file = old_fe->file;
		new_fe->std_no = old_fe->std_no;
	}

	return new_fe->fd;
}



/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
	int syscall_no = f->R.rax;
	void *arg1 = f->R.rdi;
	void *arg2 = f->R.rsi;
	void *arg3 = f->R.rdx;
	void *arg4 = f->R.r10;
	void *arg5 = f->R.r8;
	void *arg6 = f->R.r9;

	void *ret = 0; // return value

	tid_t ttt; /////////////////////////////////////////////

	switch (syscall_no) {
		/* Projects 2 and later. */
		case SYS_HALT: /* Halt the operating system. */
			halt();
			break;
		case SYS_EXIT: /* Terminate this process. */
			exit(arg1);
			break;
		case SYS_FORK: /* Clone current process. */
			// printf("[DBG] syscall_handler(): {%s} intr_frame address is %p\n", thread_current()->name, f); ////////////////////
			// print_if(f, "before fork"); //////////////////

			// ret = (uint64_t) fork(arg1, f);

			// printf("HEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEELP\n");
			// printf("[DBG] ttt = %d\n", ttt); ///////////////////////////
			ttt = fork(arg1, f);
			// printf("[DBG] syscall_handler(): fork() is done! ttt = %d\n", thread_current()->name, ttt); //////////
			ret = (void *) ttt;
			// printf("[DBG] syscall_handler(): fork() is done! ttt = %d, return value is %lld\n", thread_current()->name, ttt, ret); //////////
			break;
		case SYS_EXEC: /* Switch current process. */
			ret = (uint64_t) exec(arg1); // 실제로 반환되는 일 없음
			break;
		case SYS_WAIT: /* Wait for a child process to die. */
			ret = (uint64_t) wait(arg1);
			break;
		case SYS_CREATE: /* Create a file. */
			ret = (uint64_t) create(arg1, arg2);
			break;
		case SYS_REMOVE: /* Delete a file. */
			ret = (uint64_t) remove(arg1);
			break;
		case SYS_OPEN: /* Open a file. */
			ret = (uint64_t) open(arg1);
			break;
		case SYS_FILESIZE: /* Obtain a file's size. */
			ret = (uint64_t) filesize(arg1);
			break;
		case SYS_READ: /* Read from a file. */
			ret = (uint64_t) read(arg1, arg2, arg3);
			break;
		case SYS_WRITE: /* Write to a file. */\
			ret = (uint64_t) write(arg1, arg2, arg3);
			break;
		case SYS_SEEK: /* Change position in a file. */
			seek(arg1, arg2);
			break;
		case SYS_TELL: /* Report current position in a file. */
			ret = (uint64_t) tell(arg1);
			break;
		case SYS_CLOSE: /* Close a file. */
			close(arg1);
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

	// do_iret(f);

	// // TODO: Your implementation goes here.
	// // printf ("system call!\n");
	// printf("[DBG] examining intr_frame\n");
	// printf("r15: %p (%lld)\n", f->R.r15, f->R.r15);
	// printf("r14: %p (%lld)\n", f->R.r14, f->R.r14);
	// printf("r13: %p (%lld)\n", f->R.r13, f->R.r13);
	// printf("r12: %p (%lld)\n", f->R.r12, f->R.r12);
	// printf("r11: %p (%lld)\n", f->R.r11, f->R.r11);
	// printf("r10: %p (%lld)\n", f->R.r10, f->R.r10);
	// printf(" r9: %p (%lld)\n", f->R.r9, f->R.r9);
	// printf(" r8: %p (%lld)\n", f->R.r8, f->R.r8);
	// printf("rsi: %p (%lld)\n", f->R.rsi, f->R.rsi);
	// printf("rdi: %p (%lld)\n", f->R.rdi, f->R.rdi);
	// printf("rbp: %p (%lld)\n", f->R.rbp, f->R.rbp);
	// printf("rdx: %p (%lld)\n", f->R.rdx, f->R.rdx);
	// printf("rcx: %p (%lld)\n", f->R.rcx, f->R.rcx);
	// printf("rbx: %p (%lld)\n", f->R.rbx, f->R.rbx);
	// printf("rax: %p (%lld)\n", f->R.rax, f->R.rax);

	// if (f->R.rax == 10) {
	// 	// write()
	// 	// *((char *) f->R.rsi + f->R.rdx) = '\0';
	// 	printf("%s", f->R.rsi);
	// 	// write (f->R.rdi, f->R.rsi, f->R.rdx);
	// 	f->R.rax = f->R.rdx;
	// 	// printf("[DBG] printf done\n"); ////////////////
	// } else if (f->R.rax == 1) {
	// 	// exit()
	// 	// printf("[DBG] {%s} is exiting\n", thread_current()->name); //////////////////
	// 	// printf("what is my thread? {%s}\n", thread_current()->name); ///////////////
	// 	// return f->R.rdi;
	// 	thread_exit();
	// } else {
	// 	// printf("[DBG] syscall_handler(): HEX DUMP\n"); ///////////////////////
	// 	// hex_dump(0, f, 128, 1); //////////////////////////////////////
	// 	printf("[DBG] examining intr_frame\n");
	// 	printf("r15: %p (%lld)\n", f->R.r15, f->R.r15);
	// 	printf("r14: %p (%lld)\n", f->R.r14, f->R.r14);
	// 	printf("r13: %p (%lld)\n", f->R.r13, f->R.r13);
	// 	printf("r12: %p (%lld)\n", f->R.r12, f->R.r12);
	// 	printf("r11: %p (%lld)\n", f->R.r11, f->R.r11);
	// 	printf("r10: %p (%lld)\n", f->R.r10, f->R.r10);
	// 	printf(" r9: %p (%lld)\n", f->R.r9, f->R.r9);
	// 	printf(" r8: %p (%lld)\n", f->R.r8, f->R.r8);
	// 	printf("rsi: %p (%lld)\n", f->R.rsi, f->R.rsi);
	// 	printf("rdi: %p (%lld)\n", f->R.rdi, f->R.rdi);
	// 	printf("rbp: %p (%lld)\n", f->R.rbp, f->R.rbp);
	// 	printf("rdx: %p (%lld)\n", f->R.rdx, f->R.rdx);
	// 	printf("rcx: %p (%lld)\n", f->R.rcx, f->R.rcx);
	// 	printf("rbx: %p (%lld)\n", f->R.rbx, f->R.rbx);
	// 	printf("rax: %p (%lld)\n", f->R.rax, f->R.rax);
	// }

	// do_iret(f); //

	// printf("[DBG] syscall_handler(): why am i printing? i die...\n"); ///////////
	// thread_exit ();
}

void syscall_terminate(void) {
	exit(-1);
}