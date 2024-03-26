/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"

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

// mmap중인 file을 관리하기 위한 구조체
struct mmap_elem {
	struct list_elem elem;
	struct file *file;
	void *addr;
	int pg_cnt;
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;

	struct file_page_args *fpargs = (struct file_page_args *) page->uninit.aux;

	// uninit_page의 aux에 저장된 정보 읽기
	struct file *file = fpargs->file;
	off_t ofs = fpargs->ofs;
	uint32_t page_read_bytes = fpargs->page_read_bytes;
	uint32_t page_zero_bytes = fpargs->page_zero_bytes;
	bool writable = fpargs->writable;
	// free(fpargs);

	// file_page에 옮겨적기
	struct file_page *file_page = &page->file;
	file_page->file = file;
	file_page->ofs = ofs;
	file_page->page_read_bytes = page_read_bytes;
	file_page->page_zero_bytes = page_zero_bytes;
	file_page->writable = writable;
}

// 필요 시 파일에 write-back
static void file_backed_write_back(struct page *page) {
	ASSERT(VM_TYPE(page->operations->type) == VM_FILE); ///////////////////////////////
	// ASSERT(pml4e_walk(thread_current()->pml4, page->va, 0) != NULL); ////////////////// page의 
	ASSERT(page->frame != NULL); ////////////////////

	// if (pml4_is_dirty(thread_current()->pml4, page->va) ||
	// if (pml4_is_dirty(thread_current()->pml4, page->va) ||
	// 	pml4_is_dirty(base_pml4, page->frame->kva)) {
	if (is_dirty(page->frame->upte) || is_dirty(page->frame->kpte)) {
		// user pml4 또는, kernel pml4의 pte가 dirty라면 write-back

		// printf("[DBG] file_backed_destroy(): page va {%p}", page->va); ////////////////

		file_seek(page->file.file, page->file.ofs);
		file_write(page->file.file, page->va, page->file.page_read_bytes);


		// if (file_write(page->file.file, page->va, page->file.page_read_bytes) !=
		// 	page->file.page_read_bytes) {
		// 	printf("[DBG] file_backed_write_back(): file_write() failed!\n"); //////////
		// 	return false;
		// }

		// pml4_set_dirty(base_pml4, page->frame->kva, false); // kernel pte dirty를 복구
	}

	return true;
}

/* Swap in the page by read contents from the file. */
// file_page_lazy_load()와 거의 동일
static bool
file_backed_swap_in (struct page *page, void *kva UNUSED) {
	struct file_page *file_page = &page->file;

	struct file *file = file_page->file;
	off_t ofs = file_page->ofs;
	uint32_t page_read_bytes = file_page->page_read_bytes;
	uint32_t page_zero_bytes = file_page->page_zero_bytes;

	ASSERT (page_read_bytes + page_zero_bytes == PGSIZE);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);

	/* Load this page. */
	if (file_read (file, page->va, page_read_bytes) != (int) page_read_bytes) {
		printf("[DBG] lazy_load_file_page(): file_read failed!\n"); /////////////////
		return false;
	}
	memset (page->va + page_read_bytes, 0, page_zero_bytes);

	pml4_pte_set_dirty(thread_current()->pml4, page->frame->upte, page->va, false); // pte dirty를 복구
	pml4_pte_set_dirty(base_pml4, page->frame->kpte, page->frame->kva, false); // pte dirty를 복구
	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	// struct file_page *file_page UNUSED = &page->file;
	file_backed_write_back(page);
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	// struct file_page *file_page = &page->file;
	file_backed_write_back(page);
	hash_delete(&thread_current()->spt.hash, &page->elem);
}

// mmap으로 만든 uninit page의 initializer
// process.c lazy_load_segment()와 동일한 코드
bool file_page_lazy_load(struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
	// printf("[DBG] lazy_load_segment(): begin\n"); //////////////////////////////////
	struct file_page_args *fpargs = (struct file_page_args*) aux;
	struct file *file = fpargs->file;
	off_t ofs = fpargs->ofs;
	uint32_t page_read_bytes = fpargs->page_read_bytes;
	uint32_t page_zero_bytes = fpargs->page_zero_bytes;

	// printf("[DBG] fpargs at %p\n", fpargs); //////////////////////////////////
	// printf(" file at %p, ofs = %d, read b = %d, zero b = %d\n", file, ofs, page_read_bytes, page_zero_bytes); ///////
	// bool writable = fpargs->writable;

	ASSERT (page_read_bytes + page_zero_bytes == PGSIZE);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	/* Do calculate how to fill this page.
		* We will read PAGE_READ_BYTES bytes from FILE
		* and zero the final PAGE_ZERO_BYTES bytes. */

	/* Get a page of memory. */
	// uint8_t *kpage = page->frame->kva;
	// // printf("[DBG] kpage at %p\n", kpage); //////////////////////////////////
	// ASSERT(kpage != NULL);

	// /* Load this page. */
	// if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
	// 	printf("[DBG] lazy_load_file_page(): file_read failed!\n"); /////////////////
	// 	return false;
	// }
	// memset (kpage + page_read_bytes, 0, page_zero_bytes);

	/* Load this page. */
	if (file_read (file, page->va, page_read_bytes) != (int) page_read_bytes) {
		printf("[DBG] lazy_load_file_page(): file_read failed!\n"); /////////////////
		return false;
	}
	memset (page->va + page_read_bytes, 0, page_zero_bytes);


	free(fpargs); // file_backed_initializer와 file_page_lazy_load에서 사용이 모두 끝남

	pml4_pte_set_dirty(thread_current()->pml4, page->frame->upte, page->va, false); // pte dirty를 복구
	pml4_pte_set_dirty(base_pml4, page->frame->kpte, page->frame->kva, false); // pte dirty를 복구
	return true;
}

/* Do the mmap */
// process.c load_segment()와 거의 유사
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
// 	return NULL;
// }

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

	// if (offset + length > file_length(file)) {
	// 	// length가 너무 김
	// 	printf("[DBG] do_mmap(): length is too long (offset + length(%d) > file length(%d)\n", length, file_length(file)); /////////////////////////////
	// 	return NULL;
	// }

	int pg_cnt = (length -1) / PGSIZE +1;

	for (int i = 0; i < pg_cnt; i++) {
		if (spt_find_page(&thread_current()->spt.hash, addr + PGSIZE * i)) {
			// 할당될 페이지가 다른 페이지와 겹치면 실패
			// printf("[DBG] do_mmap(): requested addr overlaps with existing page (page at %p)\n", addr + PGSIZE * i); //////////
			return NULL;
		}
	}

	void *ret = addr; // addr은 바뀌므로 반환 주소를 저장

	file = file_reopen(file); // fd가 닫혀도 유효해야 하므로 file 개체를 새로 생성
	struct mmap_elem *me = malloc(sizeof(*me));
	if (me == NULL) {
		printf("[DBG] do_mmap(): malloc for mmap_elem failed!\n"); ////////////////////
		return NULL;
	}

	me->file = file;
	me->addr = addr;
	me->pg_cnt = pg_cnt;
	list_push_back(&thread_current()->mmap_list, &me->elem); // mmap중인 파일의 리스트에 삽입

	// 할당 시작
	uint32_t read_bytes = length < file_length(file) ? length : file_length(file);
	uint32_t zero_bytes = pg_round_up(length) - read_bytes;
	// // uint32_t read_bytes, zero_bytes; // = length < file_length(file) ? length : file_length(file);
	// // uint32_t zero_bytes = ;
	// if (length < file_length(file)) {
	// 	read_bytes = length;
	// 	// zero_bytes = pg_round_up(length) - length;
	// } else {
	// 	read_bytes = file_length(file);
		
	// }
	

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		// void *aux = NULL;
		struct file_page_args *fpargs = malloc(sizeof(*fpargs));
		fpargs->file = file;
		fpargs->ofs = offset;
		fpargs->page_read_bytes = page_read_bytes;
		fpargs->page_zero_bytes = page_zero_bytes;
		fpargs->writable = writable;
		
		if (!vm_alloc_page_with_initializer (VM_FILE, addr,
					writable, file_page_lazy_load, fpargs))
		{
			printf("[DBG] do_mmap(): vm_alloc_page_with_initializer failed!\n"); ///
			return NULL;
		}
		// printf("[DBG] load_segment(): vm_alloc_page_with_initializer success\n"); ///

			// return false;
		// if (!vm_alloc_page_with_initializer (VM_ANON, upage,
		// 			writable, lazy_load_segment, aux))
		// 	return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		addr += PGSIZE;
		offset += PGSIZE;
	}
	return ret;
}

/* Do the munmap */
void
do_munmap (void *addr) {
// 	return;
// }
	// printf("[DBG] do_munmap(): begin\n"); /////////////////////////////////////
	struct list *mmap_list = &thread_current()->mmap_list;
	struct list_elem *e;
	struct mmap_elem *me;
	for (e = list_begin(mmap_list); e != list_end(mmap_list); e = list_next(e)) {
		// mmap_list에서 addr에 map된 파일을 확인
		me = list_entry(e, struct mmap_elem, elem);

		if (me->addr == addr) {
			// 발견
			break;
		}
	}
	// printf("[DBG] do_munmap(): after iterations\n"); /////////////////////////////////////
	if (e == list_end(mmap_list)) {
		// mmap_list에 없음
		PANIC("[DBG] do_munmap(): no mmap for addr(%p) found!\n", addr);
	}

	// printf("[DBG] do_munmap(): freeing pages\n"); /////////////////////////////////////

	// printf("[DBG] do_munmap(): printing spt\n"); //////////////////
	// print_hash_table(&thread_current()->spt.hash); ///////

	// printf("addr = %p, pg_cnt = %d\n", me->addr, me->pg_cnt); ///////////

	struct page *page;
	// mmap으로 생성된 file_page를 모두 제거
	for (int i = 0; i < me->pg_cnt; i++) {
		// printf("[DBG] do_munmap(): free page #%d (at %p)\n", i, addr); /////////////////////////////////////
		page = spt_find_page(&thread_current()->spt, addr + PGSIZE * i);
		ASSERT(page != NULL);

		// printf("[DBG] do_munmap(): found page #%d (at %p)\n", i, page->va); /////////////////////////////////////

		vm_dealloc_page(page);
	}

	// printf("[DBG] do_munmap(): dealloc done all\n"); /////////////////////////////////////

	file_close(me->file);
	list_remove(&me->elem);
	free(me);
}
