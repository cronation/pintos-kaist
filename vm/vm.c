/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */

	list_init(&frame_list); // 물리 메모리 user pool에 할당된 프레임의 리스트
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

//////////////////////////////////////////////////// DEBUG
void print_spt() {
	struct hash *h = &thread_current()->spt.hash;
	struct hash_iterator i;

	printf("============= {%s} SUP. PAGE TABLE (%d entries) =============\n", thread_current()->name, hash_size(h));
	printf("   USER VA    | KERN VA (PA) |     TYPE     | STK | WRT | DRT(K/U) \n");

	void *va, *kva;
	enum vm_type type;
	char *type_str, *stack_str, *writable_str, *dirty_k_str, *dirty_u_str;

	hash_first (&i, h);
	struct page *page;
	// uint64_t *pte;
	while (hash_next (&i)) {
		page = hash_entry (hash_cur (&i), struct page, elem);

		va = page->va;
		if (page->frame) {
			kva = page->frame->kva;
			// pte = pml4e_walk(thread_current()->pml4, page->va, 0);
			writable_str = is_writable(page->frame->upte) ? "YES" : "NO";
			// dirty_str = pml4_is_dirty(thread_current()->pml4, page->va) ? "YES" : "NO";
			dirty_k_str = is_dirty(page->frame->kpte) ? "YES" : "NO";
			dirty_u_str = is_dirty(page->frame->upte) ? "YES" : "NO";
		} else {
			kva = NULL;
			dirty_k_str = " - ";
			dirty_u_str = " - ";
		}
		type = page->operations->type;
		if (VM_TYPE(type) == VM_UNINIT) {
			type = page->uninit.type;
			switch (VM_TYPE(type)) {
				case VM_ANON:
					type_str = "UNINIT-ANON";
					break;
				case VM_FILE:
					type_str = "UNINIT-FILE";
					break;
				case VM_PAGE_CACHE:
					type_str = "UNINIT-P.C.";
					break;
				default:
					type_str = "UNKNOWN (#)";
					type_str[9] = VM_TYPE(type) + 48; // 0~7 사이 숫자의 아스키 코드
			}
			stack_str = type & VM_STACK ? "YES" : "NO";
			struct file_page_args *fpargs = (struct file_page_args *) page->uninit.aux;
			writable_str = fpargs->writable ? "(Y)" : "(N)";
		} else {
			stack_str = "NO";
			switch (VM_TYPE(type)) {
				case VM_ANON:
					type_str = "ANON";
					stack_str = page->anon.is_stack ? "YES" : "NO";
					break;
				case VM_FILE:
					type_str = "FILE";
					break;
				case VM_PAGE_CACHE:
					type_str = "PAGE CACHE";
					break;
				default:
					type_str = "UNKNOWN (#)";
					type_str[9] = VM_TYPE(type) + 48; // 0~7 사이 숫자의 아스키 코드
			}
			

		}
		printf(" %12p | %12p | %12s | %3s | %3s |  %3s/%3s \n",
			   va, kva, type_str, stack_str, writable_str, dirty_k_str, dirty_u_str);
	}
}

void print_frame_list() {
	printf("============= FRAME LIST =============\n");
	printf(" KERN VA (PA) |   USER VA    |     TYPE     | STK | WRT | DRT \n");
}


//////////////////////////////////////////////////// DEBUG END

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	// printf("[DBG] vm_alloc_page_with_initializer(): upage at %p\n", upage); //////////////
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		void *initializer = NULL;
		switch VM_TYPE(type) {
			case VM_ANON:
				// printf("[DBG] vm_alloc_page_with_initializer(): received ANON type\n"); /////
				initializer = anon_initializer;
				break;
			case VM_FILE:
				// printf("[DBG] vm_alloc_page_with_initializer(): received FILE type\n"); /////
				initializer = file_backed_initializer;
				break;
			case VM_PAGE_CACHE:
				printf("[DBG] vm_alloc_page_with_initializer(): PAGE_CACHE type is not implemented\n"); /////	
				break;
			default:
				printf("[DBG] vm_alloc_page_with_initializer(): unknown page type (%d)\n", VM_TYPE(type)); /////
		}

		struct page *page = malloc(sizeof(*page));
		if (!page) {
			PANIC("[DBG] vm_alloc_page_with_initializer(): malloc failed!\n"); ////
		}
		/* TODO: Insert the page into the spt. */
		uninit_new(page, upage, init, type, aux, initializer);
		if (!spt_insert_page(spt, page)) {
			printf("[DBG] vm_alloc_page_with_initializer(): spt_insert_page failed\n"); /////
			free(page);
			return false;
		}
		// printf("[DBG] SUCCESS; printing spt\n"); /////
		// print_hash_table(&spt->hash); ////////////////////////
		return true;
	} else {
		printf("[DBG] vm_alloc_page_with_initializer(): upage is already in spt!\n"); //////
		return false;
	}
err:
	printf("[DBG] vm_alloc_page_with_initializer(): unknown error?\n"); /////
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	// printf("[DBG] set_find_page() begin\n"); ///////////////////////////////
	// struct page *page = NULL;
	/* TODO: Fill this function. */
	struct page temp_page;
	temp_page.va = pg_round_down(va);


	// printf("printing spt\n"); ////////////////////////////////////////////////////////////
	// print_hash_table(spt); ////////////////////////////////////////////////////////////

	// printf("[DBG] set_find_page() before hash_find\n"); ///////////////////////////////
	struct hash_elem *e = hash_find(&spt->hash, &temp_page.elem);

	if (!e) {
		// printf("[DBG] set_find_page(): no page found; return NULL\n"); ///////////////////////////////
		return NULL;
	}

	// printf("[DBG] set_find_page() before hash_entry\n"); ///////////////////////////////
	// printf("e at %p\n", e); ////////////////////

	struct page *page = hash_entry(e, struct page, elem);

	// printf("[DBG] set_find_page() before ->page\n"); ///////////////////////////////

	// printf("[DBG] set_find_page() before return\n"); ///////////////////////////////

	// printf("[DBG] page->va at %p\n", page->va); /////////////

	return page;

	// return page;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt, struct page *page) {
	ASSERT(spt != NULL); /////////////////////////////////////////////////////////////
	ASSERT(page != NULL); /////////////////////////////////////////////////////////////
	int succ = false;
	/* TODO: Fill this function. */

	return !hash_insert(&spt->hash, &page->elem);

	// return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	/* TODO: The policy for eviction is up to you. */

	// 프레임 리스트 순회
	// access된 프레임을 모두 맨 뒤로 이동, access는 false로 변경
	// 맨 앞 요소 반환

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
// lenient LRU: 마지막 eivction 이후 access되지 않은 프레임에 대해 FIFO
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */
	
	// victim을 swap out
	// list에서 제거
	// page <-> frame 끊기
	// 받아낸 frame을 반환

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	/* TODO: Fill this function. */
	void *kva = palloc_get_page(PAL_USER | PAL_ZERO);

	struct frame *frame = NULL;
	if (kva) {
		// 빈 프레임을 성공적으로 할당받음
		// 새로운 frame 구조체 생성
		frame = calloc(sizeof(*frame), 1);
		if (!frame) {
			PANIC("[DBG] vm_get_frame(): malloc for frame failed\n");
		}
		// frame의 kva, kernel pml4의 pte는 절대 변하지 않음
		frame->kva = kva;
		frame->kpte = pml4e_walk(base_pml4, kva, 0);
	} else {
		// 빈 프레임이 없음: evict하여 공간 확보
		frame = vm_evict_frame();
	}

	// 프레임 리스트에 삽입

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (struct supplemental_page_table *spt, void *addr) {
	void *stack_pg = pg_round_down(addr);

	while (spt_find_page(spt, stack_pg) == NULL) {
		// 할당된 페이지를 만날 때까지 새로운 스택 페이지를 할당
		if (!vm_alloc_page(VM_ANON | VM_STACK, stack_pg, true)) {
			PANIC("[DBG] vm_stack_growth(): vm_alloc_page() failed!\n"); //////
			return;
		}
		
		if (!vm_claim_page(stack_pg)) {
			PANIC("[DBG] vm_stack_growth(): vm_claim_page() failed!\n"); //////
			return;
		}

		stack_pg += PGSIZE;
	}

	ASSERT(VM_TYPE(spt_find_page(spt, stack_pg)->operations->type) == VM_ANON); //////////////////////
	ASSERT(spt_find_page(spt, stack_pg)->anon.is_stack); //////////////////////
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */

	// 페이지 폴트를 확인해보자

	// printf("[DBG] vm_try_handle_fault(): begin\n"); /////////////////////////////////
	// printf("[DBG] ... addr: %p, user: %d, write: %d, not_present: %d\n", addr, user, write, not_present); //////////
	// printf("[DBG] printing spt...\n"); //////////////////////////////////
	// print_spt(); //////////////////////////////////
	



	// spt 확인
	// va가 할당되었다면:
	// 1. 데이터가 frame에 없음 (not present)
	//		1. uninit이라서 없음
	//		2. sawp out 당해서 없음
	// 2. write 금지에 시도함
	//		1. 진짜 write 금지임: false 반환
	//		2. write protected임: copy on write 하기
	// va가 할당되지 않았다면:
	// 1. 엉뚱한 접근: terminate
	// 2. stack growth

	page = spt_find_page(spt, addr);

	if (!user) {
		printf("[DBG] vm_try_handle_fault(): received from kernel\n"); /////////
		// return false;
	}

	if (page) {
		// 할당된 va로의 접근
		if (not_present) {
			// uninit이거나 swap out당해서 없음
			return vm_do_claim_page (page);
		} else if (write) {
			// present지만 write을 시도했다가 fault 발생: 금지된 쓰기
			ASSERT(VM_TYPE(page->operations->type) == VM_FILE); /////////////////////////
			ASSERT(!page->file.writable); /////////////////////////
			// printf("[DBG] vm_try_handle_fault(): write on read-only page (CoW WIP)\n"); /////////
			return false;
		} else {
			printf("[DBG] vm_try_handle_fault(): UNKNOWN ERROR (allocated va and present but fault?)\n"); /////////
			return false;
		}
	} else {
		// stack growth인지 확인

		// printf("[DBG] vm_try_handle_fault(): anallocated va... is this stack growth?\n"); /////////
		// // printf("[DBG] ... addr: %p, user: %d, write: %d, not_present: %d\n", addr, user, write, not_present); //////////
		// printf("[DBG] addr: %p, rsp in f: %p\n", addr, f->rsp); ///////////////////////////
		// printf("[DBG] printing spt...\n"); //////////////////////////////////
		// print_hash_table(&spt->hash); //////////////////////////////////

		if (pg_no(f->rsp) == pg_no(addr) || pg_no(f->rsp) == pg_no(addr) -1) {
			// rsp와 fault page가 일치하거나 한 페이지 차이: stack growth

			// printf("[DBG] yes this is stack growth\n"); ///////////////////////////
			if (pg_no(f->rsp) == pg_no(addr) -1) { //////////////////////////////////////////////////////
				printf("[DBG] vm_try_handle_fault(): analigned stack growth it seems... (rsp at %p, fault addr at %p)\n", f->rsp, addr); /////
			}

			vm_stack_growth(spt, addr);
			return true;
		} else {
			// stack growth가 아님
			return false;
		}
			
	}



	// if (!user) {
	// 	printf("[DBG] vm_try_handle_fault(): received from kernel\n"); /////////
	// 	return false;
	// }
	// if (not_present) {
	// 	page = spt_find_page(spt, addr);
	// 	if (!page) {
	// 		// printf("[DBG] vm_try_handle_fault(): page is not in spt\n"); ////////
	// 		return false;
	// 	}
	// 	// printf("[DBG] vm_try_handle_fault(): page found, attempting to claim...\n"); ////////
	// 	return vm_do_claim_page (page);
	// } else {
	// 	// bogus fault
	// }

	// return vm_do_claim_page (page);

	printf("[DBG] vm_try_handle_fault(): something went wrong...\n");
	return false;
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va) {
	struct page *page = NULL;
	/* TODO: Fill this function */
	// printf("[DBG] vm_claim_page(%p) start\n", va); //////////////////////
	page = spt_find_page(&thread_current()->spt, va);

	if (!page) {
		PANIC("[DBG] vm_claim_page(): spt_find_page() failed\n");
	}

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	// writable 여부 체크 - file page이거나 file page가 될 uninit인 경우만 false일 수 있음음
	bool writable = vm_get_page_writable(page);

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	// printf("[DBG] vm_do_claim_page(): now setting page\n"); ////////////////////////////////
	// bool result = pml4_set_page(thread_current()->pml4, page->va, frame->kva, writable);
	// printf("[DBG] vm_do_claim_page(): set page result: %d\n", result); ////////////////////////////////



	pml4_set_page(thread_current()->pml4, page->va, frame->kva, writable);
	// swap out시 accessed bit 확인을 위해 pte를 저장
	frame->upte = pml4e_walk(thread_current()->pml4, page->va, 0);

	ASSERT(frame->upte != NULL); //////////////////////////////////////////////////////
	ASSERT(frame->kpte != NULL); //////////////////////////////////////////////////////



	// printf("[DBG] vm_do_claim_page(): pml4_set_page done!\n"); ////////////
	// uint64_t *pte = pml4e_walk(thread_current()->pml4, page->va, 0); ////////////
	// printf("is this writable? %d\n", is_writable(pte)); /////////////////////////

	return swap_in (page, frame->kva);
}

// P3
// 주어진 페이지가 writable인지 반환
bool vm_get_page_writable(struct page *page) {
	// writable 여부 체크 - file page이거나 file page가 될 uninit인 경우만 해당

	enum vm_type type = VM_TYPE(page->operations->type);
	if (type == VM_FILE) {
		// file page
		return page->file.writable;
	}else if (type == VM_UNINIT) {
		type = VM_TYPE(page->uninit.type);
		if (type == VM_FILE) {
			// file page로 생성된 uninit page
			struct file_page_args *fpargs = (struct file_page_args *) page->uninit.aux;
			return fpargs->writable;
		}
	}

	return true;
}

// 주어진 주소의 페이지가 writable인지 반환
bool vm_get_addr_writable(void *va) {
	struct page *page = spt_find_page(&thread_current()->spt, va);
	if (!page) { ///////////////////////////////////////////////////////////////////////
		printf("[DBG] vm_get_addr_writable(): page for va(%p) not found!\n", va);
		return false;
	}

	// if (!vm_get_page_writable(page)) { ////////////////////////////////////////////////
	// 	printf("[DBG] vm_get_addr_writable(): page(va: %p) is not writable!\n", va);
	// 	print_hash_table(&thread_current()->spt.hash);
	// 	return false;
	// }

	return page && vm_get_page_writable(page);
}

// P3
// 주어진 주소의 페이지가 spt에 있는지 여부 반환
bool vm_get_addr_readable(void *va) {
	// printf("[DBG] vm_get_addr_readable(%p) begin\n", va); ///////////////////////////
	// print_hash_table(&thread_current()->spt.hash); ////////////////////////////



	struct page *page = spt_find_page(&thread_current()->spt, va);
	// if (!page) { ///////////////////////////////////////////////////////////////////////
	// 	printf("[DBG] vm_get_addr_writable(): page for va(%p) not found!\n", va);
	// 	return false;
	// }

	return page;
}

bool page_va_less_func (const struct hash_elem *a,
		const struct hash_elem *b, void *aux UNUSED) {
	struct page *pa = hash_entry(a, struct page, elem);
	struct page *pb = hash_entry(b, struct page, elem);

	return pa->va < pb->va;
}

uint64_t page_va_hash_func(const struct hash_elem *e, void *aux UNUSED) {
	struct page *page = hash_entry(e, struct page, elem);
	return hash_bytes(&page->va, sizeof(void*));
}


/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	hash_init(&thread_current()->spt.hash, page_va_hash_func,
			  page_va_less_func, NULL);
}

// static struct page *alloc_page_from_uninit(struct page *src_pg) {
// 	struct page *page = malloc(sizeof(*page));
// 	if (!page) {
// 		PANIC("[DBG] alloc_page_from_uninit(): malloc failed!\n"); ////
// 	}

// 	uninit_new(page, src_pg->va, src_pg->uninit.init, src_pg->uninit.type,
// 			   src_pg->uninit.aux, src_pg->uninit.page_initializer);

// 	return true;
// }

// static struct page *alloc_page_from_anon(struct page *src_pg) {
// 	struct page *page = malloc(sizeof(*page));
// 	// if (!page) {
// 	// 	PANIC("[DBG] alloc_page_from_anon(): malloc failed!\n"); ////
// 	// }

// 	// uninit_new(page, src_pg->va, , src_pg->uninit.type,
// 	// 		   src_pg->uninit.aux, src_pg->uninit.page_initializer);

// 	// return true;
// }

// static struct page *alloc_page_from_file(struct page *src_pg) {
// 	struct page *page = malloc(sizeof(*page));
// 	if (!page) {
// 		PANIC("[DBG] alloc_page_from_file(): malloc failed!\n"); ////
// 	}

// 	uninit_new(page, src_pg->va, src_pg->uninit.init, src_pg->uninit.type,
// 			   src_pg->uninit.aux, src_pg->uninit.page_initializer);

// 	return true;
// }

// static struct page *alloc_page_from_page_cache(struct page *src_pg) {
// 	printf("[DBG] alloc_page_from_page_cache() is not implemented yet\n"); /////
// 	return false;
// }

// static bool alloc_page_from_src(struct page *src_pg) {
// 	enum vm_type type = src_pg->operations->type;
// 	switch(VM_TYPE(type)) {
// 		case VM_UNINIT:
// 			alloc_page_from_uninit(src_pg);
// 			break;
// 		case VM_ANON:
// 			alloc_page_from_anon(src_pg);
// 			break;
// 		case VM_FILE:
// 			alloc_page_from_file(src_pg);
// 			break;
// 		case VM_PAGE_CACHE:
// 			alloc_page_from_page_cache(src_pg);
// 			break;
// 		default:
// 			printf("[DBG] alloc_page_from_src(): unknown page type (%d)\n", VM_TYPE(type)); /////
// 	}
// }

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src) {
	// struct hash_iterator i;

	// void *va, *kva;
	// enum vm_type type;

	// hash_first (&i, &src->hash);
	// struct page *old_page;
	// while (hash_next (&i)) {
	// 	page = hash_entry (hash_cur (&i), struct page, elem);
	// }


	// // insert
	// if (!spt_insert_page(dst, page)) {
	// 	printf("[DBG] alloc_page_from_uninit(): spt_insert_page failed\n"); /////
	// 	free(page);
	// 	return false;
	// }

	// // claim
}

static void page_hash_destructor(struct hash_elem *e, void *aux UNUSED) {
	struct page *page = hash_entry(e, struct page, elem);
	// destroy(page);
	vm_dealloc_page(page);
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	// printf("[DBG] SOMEONE CALLED DESTRUCTOR\n"); ////////////////////////////////////
	// print_hash_table(&spt->hash); //////////////////////////
	// hash_destroy(&spt->hash, page_hash_destructor);
	
	hash_clear(&spt->hash, page_hash_destructor);
}
