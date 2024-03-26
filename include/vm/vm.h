#ifndef VM_VM_H
#define VM_VM_H
#include <stdbool.h>
#include "threads/palloc.h"
#include "threads/mmu.h" // P3
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
	VM_STACK = (1 << 3),
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
#include <bitmap.h> // swap disk table 자료구조
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
	struct hash_elem elem; // hash table에 넣기 위한 elem (P3)
	struct write_protector *write_protector; // copy-on-write (P3-EX)

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

struct list frame_list; // 물리 메모리에 할당된 frame의 리스트

/* The representation of "frame" */
struct frame {
	void *kva;
	struct page *page;
	uint64_t *upte; // pml4와 연결 (user pml4)
	uint64_t *kpte; // pml4와 연결 (kernel pml4)
	struct list_elem elem;
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

struct bitmap *swap_bitmap; // swap disk에 할당된 sector의 비트맵

#define swap_in(page, v) (page)->operations->swap_in ((page), v)
#define swap_out(page) (page)->operations->swap_out (page)
#define destroy(page) \
	if ((page)->operations->destroy) (page)->operations->destroy (page)

/* Representation of current process's memory space.
 * We don't want to force you to obey any specific design for this struct.
 * All designs up to you for this. */
struct supplemental_page_table {
	struct hash hash;
};

// copy-on-write에 사용 (P3-EX)
struct write_protector {
	int share_cnt; // 현재 페이지를 공유중인 프로세스의 수
	// bool ori_writable;
};

// file backed page의 initialize, swap in에 필요한 정보
// uninit의 aux에 저장되어있다가, file page로 변할 때 정보가 복사됨
struct file_page_args {
	struct file *file;
	off_t ofs;
	uint32_t page_read_bytes;
	uint32_t page_zero_bytes;
	bool writable;
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

void print_hash_table(struct hash *h); ///////////////////////////////////////// DEBUG

#endif  /* VM_VM_H */
