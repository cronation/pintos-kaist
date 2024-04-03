#ifndef VM_VM_H
#define VM_VM_H
#include <stdbool.h>
#include "threads/palloc.h"

// P3
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include <string.h> // memcpy, memset
#include "threads/init.h" // base_pml4 확인용 (P3)

enum vm_type {
	/* page not initialized */
	VM_UNINIT = 0,
	/* page not related to the file, aka anonymous page */
	VM_ANON = 1,
	/* page that realated to the file */
	VM_FILE = 2,
	/* page that hold the page cache, for project 4 */
	VM_PAGE_CACHE = 3,

	/* Bit flags to store state */

	/* Auxillary bit flag marker for store information. You can add more
	 * markers, until the value is fit in the int. */
	VM_MARKER_0 = (1 << 3),
	VM_MARKER_1 = (1 << 4),

	/* DO NOT EXCEED THIS VALUE. */
	VM_MARKER_END = (1 << 31),
};

#include "vm/uninit.h"
#include "vm/anon.h"
#include "vm/file.h"
// P3
#include <hash.h> // supplemental page table 자료구조
#include <list.h> // frame table 자료구조

#ifdef EFILESYS
#include "filesys/page_cache.h"
#endif

struct page_operations;
struct thread;

#define VM_TYPE(type) ((type) & 7)

/* The representation of "page".
 * This is kind of "parent class", which has four "child class"es, which are
 * uninit_page, file_page, anon_page, and page cache (project4).
 * DO NOT REMOVE/MODIFY PREDEFINED MEMBER OF THIS STRUCTURE. */
struct page {
	const struct page_operations *operations;
	void *va;              /* Address in terms of user space */
	struct frame *frame;   /* Back reference for frame */

	/* Your implementation */
	bool writable;
	// copy-on-write (P3-EX)
	struct list share_list; // 페이지를 공유중인 spt의 리스트
	int share_cnt;

	/* Per-type data are binded into the union.
	 * Each function automatically detects the current union */
	union {
		struct uninit_page uninit;
		struct anon_page anon;
		struct file_page file;
#ifdef EFILESYS
		struct page_cache page_cache;
#endif
	};
};

// share시에는 같은 페이지가 여러 hash table에 삽입되므로, 개별 구조체를 만들어 삽입
struct page_elem {
	struct page *page;
	struct hash_elem elem;
};


struct list frame_list; // 물리 메모리에 할당된 frame의 리스트
struct frame frame_nil; // sentinel용 (LRU, Clock일 때 사용)
// vm_handle_wp, vm_try_handle_fault, supplemental_page_table_kill에서 사용
struct lock frame_list_lock;

/* The representation of "frame" */
struct frame {
	void *kva;
	struct page *page;
	uint64_t *kpte; // pml4와 연결 (kernel pml4)
	struct list_elem elem; // frame list에 넣기 위한 elem
};

/* The function table for page operations.
 * This is one way of implementing "interface" in C.
 * Put the table of "method" into the struct's member, and
 * call it whenever you needed. */
struct page_operations {
	bool (*swap_in) (struct page *, void *);
	bool (*swap_out) (struct page *);
	void (*destroy) (struct page *);
	enum vm_type type;
};

#define swap_in(page, v) (page)->operations->swap_in ((page), v)
#define swap_out(page) (page)->operations->swap_out (page)
#define destroy(page) \
	if ((page)->operations->destroy) (page)->operations->destroy (page)

/* Representation of current process's memory space.
 * We don't want to force you to obey any specific design for this struct.
 * All designs up to you for this. */
struct supplemental_page_table {
	struct hash hash;
	// struct list mmap_list;
	struct hash mmap_hash;
	uint64_t *pml4; // spt에 대응되는 pml4를 저장
};

// 페이지의 share_list에 spt의 주소를 저장할 구조체
struct spt_elem {
	struct supplemental_page_table *spt;
	struct list_elem elem;
};

// mmap중인 file을 관리하기 위한 구조체
struct mmap_elem {
	struct hash_elem elem;
	struct file *file;
	void *addr;
	int pg_cnt;
};

// uninit page의 aux에 저장되는 구조체
// file backed page의 initialize, swap in에 필요한 정보
// uninit의 aux에 저장되어있다가, file page로 변할 때 정보가 복사됨
struct uninit_page_args {
	// file page lazy load, lazy load segment용
	struct file *file; // anon page initialize시에만 사용
	void *addr; // file page initialize, swap in/out시에만 사용
	off_t ofs;
	uint32_t page_read_bytes;
	uint32_t page_zero_bytes;
	// anon page용
	bool is_stack;
};

#include "threads/thread.h"
void supplemental_page_table_init (struct supplemental_page_table *spt);
bool supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src);
void supplemental_page_table_kill (struct supplemental_page_table *spt);
struct page *spt_find_page (struct supplemental_page_table *spt,
		void *va);
bool spt_insert_page (struct supplemental_page_table *spt, struct page *page);
void spt_remove_page (struct supplemental_page_table *spt, struct page *page);

void vm_init (void);
bool vm_try_handle_fault (struct intr_frame *f, void *addr, bool user,
		bool write, bool not_present);

#define vm_alloc_page(type, upage, writable) \
	vm_alloc_page_with_initializer ((type), (upage), (writable), NULL, NULL)
bool vm_alloc_page_with_initializer (enum vm_type type, void *upage,
		bool writable, vm_initializer *init, void *aux);
void vm_dealloc_page (struct page *page);
bool vm_claim_page (void *va);
enum vm_type page_get_type (struct page *page);

// P3
bool vm_get_page_writable(struct page *page);
bool vm_get_addr_writable(void *va);
bool vm_get_addr_readable(void *va);

void print_spt(); ///////////////////////////////////////// DEBUG

#endif  /* VM_VM_H */
