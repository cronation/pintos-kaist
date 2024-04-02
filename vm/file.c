/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "threads/malloc.h" // P3
// #include "threads/synch.h" // P3
#include <list.h> // P3

// struct lock file_lock;

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

// P3
static bool file_page_lazy_load(struct page *page, void *aux);
static struct file *get_file_from_hash(struct hash *h, void *addr);

/* The initializer of file vm */
void
vm_file_init (void) {
	// lock_init(&file_lock);
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;

	struct uninit_page_args *upargs = (struct uninit_page_args *) page->uninit.aux;

	// uninit_page의 aux에 저장된 정보 읽기
	// struct file *file = upargs->file;
	void *addr = upargs->addr;
	off_t ofs = upargs->ofs;
	uint32_t page_read_bytes = upargs->page_read_bytes;
	uint32_t page_zero_bytes = upargs->page_zero_bytes;


	// file_page에 옮겨적기
	struct file_page *file_page = &page->file;
	// file_page->file = file;
	file_page->addr = addr;
	file_page->ofs = ofs;
	file_page->page_read_bytes = page_read_bytes;
	file_page->page_zero_bytes = page_zero_bytes;

	return true;
}

// 필요 시 파일에 write-back
void file_backed_write_back(struct page *page, struct file *file) {
	ASSERT(VM_TYPE(page->operations->type) == VM_FILE); ///////////////////////////////
	ASSERT(page->frame != NULL); ////////////////////

	// printf("[DBG] file_backed_write_back(): begin\n"); /////////

	bool u_dirty = pml4_is_dirty(thread_current()->pml4, page->va);
	bool k_dirty = (*page->frame->kpte) & PTE_D;
	if (u_dirty || k_dirty) {
		// user pml4 또는, kernel pml4의 pte가 dirty라면 write-back

		// file_seek(page->file.file, page->file.ofs);
		file_seek(file, page->file.ofs);

		// printf("[DBG] file_backed_write_back(): file at %p, va = %p, read_bytes = %d\n",
		// 	page->file.file, page->va, page->file.page_read_bytes); ///////////////
		// printf("[DBG] offset = %d, zero bytes = %d\n", page->file.ofs, page->file.page_zero_bytes); ////

		// lock_acquire(&file_lock);
		int bytes_written = file_write(file, page->frame->kva, page->file.page_read_bytes); // destory는 pml4 삭제 후 호출되므로 kva로 삭제
		// lock_release(&file_lock);

		if (bytes_written != page->file.page_read_bytes) {
			printf("[DBG] file_backed_write_back(): error while writting back... (bytes to write: %d, actual bytes: %d)\n", page->file.page_read_bytes, bytes_written); /////
		}
	}
}

/* Swap in the page by read contents from the file. */
// file_page_lazy_load()와 거의 동일
static bool
file_backed_swap_in (struct page *page, void *kva UNUSED) {
	struct file_page *file_page = &page->file;

	// // mmap_hash에서 page에 해당되는 파일 구조체 가져오기
	// struct mmap_elem temp_me;
	// temp_me.addr = file_page->addr;

	// struct hash_elem *e = hash_find(&thread_current()->spt.mmap_hash, &temp_me.addr);
	// struct file *file = hash_entry(e, struct mmap_elem, elem)->file;
	struct file *file = get_file_from_hash(&thread_current()->spt.mmap_hash, file_page->addr);

	// 이외 정보는 file_page 구조체 안에서 가져오기
	off_t ofs = file_page->ofs;
	uint32_t page_read_bytes = file_page->page_read_bytes;
	uint32_t page_zero_bytes = file_page->page_zero_bytes;

	ASSERT (page_read_bytes + page_zero_bytes == PGSIZE);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);

	/* Load this page. */
	// lock_acquire(&file_lock);
	if (file_read (file, page->va, page_read_bytes) != (int) page_read_bytes) {
		printf("[DBG] lazy_load_file_page(): file_read failed!\n"); /////////////////
		// lock_release(&file_lock);
		return false;
	}
	// lock_release(&file_lock);

	memset (page->va + page_read_bytes, 0, page_zero_bytes);

	// pml4_set_dirty(thread_current()->pml4, page->va, false); // pte dirty를 복구
	// pml4_set_dirty(base_pml4, page->frame->kva, false); // pte dirty를 복구
	// pml4_pte_set_dirty(thread_current()->pml4, page->frame->upte, page->va, false); // pte dirty를 복구
	// pml4_pte_set_dirty(base_pml4, page->frame->kpte, page->frame->kva, false); // pte dirty를 복구

	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file *file = get_file_from_hash(&thread_current()->spt.mmap_hash, page->file.addr);
	file_backed_write_back(page, file);
	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	// destroy는 항상 write-back 함수를 호출한 뒤에 호출됨...

	// printf("[DBG] file_backed_destroy() for va = %p begin\n", page->va); ///////////
	// if (page->frame) {
	// 	// 메모리 상에 있는 경우만 write-back 필요
	// 	// write back은 munmap에서 하기로 수정
	// 	// file_backed_write_back(page); 
	// 	// frame list에서 제거
	// 	list_remove(&page->frame->elem);
	// 	free(page->frame);
	// }
}

static bool file_page_lazy_load(struct page *page, void *aux) {
	struct uninit_page_args *upargs = (struct uninit_page_args*) aux;
	struct file *file = get_file_from_hash(&thread_current()->spt.mmap_hash, page->file.addr);
	off_t ofs = upargs->ofs;
	uint32_t page_read_bytes = upargs->page_read_bytes;
	uint32_t page_zero_bytes = upargs->page_zero_bytes;

	ASSERT (page_read_bytes + page_zero_bytes == PGSIZE);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	if (file_read (file, page->va, page_read_bytes) != (int) page_read_bytes) {
		printf("[DBG] file_page_lazy_load(): file_read failed!\n"); /////////////////
		return false;
	}
	memset (page->va + page_read_bytes, 0, page_zero_bytes);

	free(upargs); // file_backed_initializer와 file_page_lazy_load에서 사용이 모두 끝남

	// memset 과정에서 켜진 dirty, accessed bit을 복구
	uint64_t *pte = pml4e_walk(thread_current()->pml4, page->va, 0);
	pml4_pte_set_dirty(thread_current()->pml4, pte, page->va, 0);
	pml4_pte_set_accessed(thread_current()->pml4, pte, page->va, 0);

	// printf("[DBG] file_page_lazy_load(): done, printing spt\n"); ////////////////
	// print_spt(); ///////////////////

	return true;
}

/* Do the mmap */
// process.c load_segment()와 거의 유사
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	
	// WIP TODO: 예외처리 정리해서 모으기

	if (file == NULL) {
		// printf("[DBG] do_mmap(): received NULL file\n"); /////////////////////////////
		return NULL;
	}

	if (addr == NULL) {
		// printf("[DBG] do_mmap(): received NULL addr\n"); /////////////////////////////
		return NULL;
	}

	if (pg_ofs(addr) != 0) {
		// align되지 않은 주소를 받음
		// printf("[DBG] do_mmap(): received unaligned addr(%p)\n", addr); /////////////////////////////
		return NULL;
	}

	if (length == 0) {
		// printf("[DBG] do_mmap(): received unaligned addr(%p)\n", addr); /////////////////////////////
		return NULL;
	}

	if (pg_ofs(offset) != 0) {
		// offset이 페이지 단위가 아님
		return NULL;
	}

	// printf("[DBG] do_mmap(): offset = %d, file_length = %d\n", offset, file_length(file));

	if (offset > file_length(file)) {
		// offset이 파일 길이를 넘어감
		return NULL;
	}

	int pg_cnt = (length -1) / PGSIZE +1;

	for (int i = 0; i < pg_cnt; i++) {
		if (spt_find_page(&thread_current()->spt, addr + PGSIZE * i)) {
			// 할당될 페이지가 다른 페이지와 겹치면 실패
			// printf("[DBG] do_mmap(): requested addr overlaps with existing page (page at %p)\n", addr + PGSIZE * i); //////////
			return NULL;
		}
	}

	// 예외처리 끝

	void *cur_pg = addr;

	struct mmap_elem *me = malloc(sizeof(*me));
	if (me == NULL) {
		printf("[DBG] do_mmap(): malloc for mmap_elem failed!\n"); ////////////////////
		return NULL;
	}

	// printf("[DBG] do_mmap(): start mapping pg_cnt = %d (addr = %p, offset = %d)\n", pg_cnt, addr, offset); //////

	me->addr = addr;
	me->pg_cnt = pg_cnt;
	me->file = file_reopen(file);
	// list_push_back(&thread_current()->spt.mmap_list, &me->elem); // mmap중인 파일의 리스트에 삽입
	hash_insert(&thread_current()->spt.mmap_hash, &me->elem);

	// 할당 시작
	uint32_t read_bytes = length < (size_t) file_length(file) - offset ? length : (size_t) file_length(file) - offset;
	uint32_t zero_bytes = pg_round_up(length) - read_bytes;
	
	while (read_bytes > 0 || zero_bytes > 0) {
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		struct uninit_page_args *upargs = malloc(sizeof(*upargs));
		// upargs->file = file_reopen(file); // 페이지 별로 별도의 파일 구조체를 제공
		upargs->addr = addr;
		upargs->ofs = offset;
		upargs->page_read_bytes = page_read_bytes;
		upargs->page_zero_bytes = page_zero_bytes;
		
		if (!vm_alloc_page_with_initializer (VM_FILE, cur_pg,
					writable, file_page_lazy_load, upargs))
		{
			printf("[DBG] do_mmap(): vm_alloc_page_with_initializer failed!\n"); ///
			return NULL;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		cur_pg += PGSIZE;
		offset += PGSIZE;
	}

	
	// printf("[DBG] do_mmap(): done mapping (addr = %p, offset = %d)\n", addr, offset); //////

	// printf("[DBG] do_mmap(%p): done, printing spt\n", addr); ////////////////
	// print_spt(); ///////////////////

	return addr;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	// printf("[DBG] do_munmap(%p): start, printing spt\n", addr); ////////////////
	// print_spt(); ///////////////////

	// struct list *mmap_list = &thread_current()->spt.mmap_list;
	struct hash *mmap_hash = &thread_current()->spt.mmap_hash;
	// struct list_elem *e;
	struct hash_elem *e;
	struct mmap_elem temp_me;
	temp_me.addr = addr;
	
	e = hash_find(mmap_hash, &temp_me.elem);
	if (!e) {
		// mmap_hash에 없음
		PANIC("[DBG] do_munmap(): no mmap for addr(%p) found!\n", addr);
	}
	struct mmap_elem *me = hash_entry(e, struct mmap_elem, elem);


	// printf("[DBG] do_munmap(): begin\n"); /////////////////

	struct page *page;
	// mmap으로 생성된 file_page를 모두 제거
	for (int i = 0; i < me->pg_cnt; i++) {
		page = spt_find_page(&thread_current()->spt, addr + PGSIZE * i);
		ASSERT(page != NULL); ////////////////////////////////////////

		if (page->frame) {
			file_backed_write_back(page, me->file);
		}
		spt_remove_page(&thread_current()->spt, page);
	}

	// printf("[DBG] do_munmap(): done\n"); /////////////////

	// list_remove(&me->elem);
	file_close(me->file);
	hash_delete(mmap_hash, &me->elem);
	free(me);
}

// mmap_hash에서 addr에 해당하는 파일 구조체 포인터를 반환
static struct file *get_file_from_hash(struct hash *h, void *addr) {
	struct mmap_elem temp_me;
	temp_me.addr = addr;

	struct hash_elem *e = hash_find(&thread_current()->spt.mmap_hash, &temp_me.elem);
	if (e == NULL) {
		printf("[DBG] get_file_from_hash(): addr not found in mmap_hash\n");
		return NULL;
	}
	return hash_entry(e, struct mmap_elem, elem)->file;
}

// vm으로 이전
// void file_clear_mmap_list(struct list *mmap_list) {
// 	struct list_elem *e;
// 	struct mmap_elem *me;
// 	while(!list_empty(mmap_list)) {
// 		e = list_pop_front(mmap_list);
// 		me = list_entry(e, struct mmap_elem, elem);
// 		free(me);
// 	}
// }