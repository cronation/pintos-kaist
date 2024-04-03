/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"

#include "threads/synch.h" // P3

#define STACK_LIM 0x47380000 // 47480000 + 1MB

enum ep_enum { // evict policy
	EP_FIFO = 0, // First in First out
	EP_LLRU = 1, // Lenient LRU
	EP_CLCK = 2, // Clock Algorithm (second wind)
};

enum ep_enum evict_policy = EP_LLRU; // policy 설정

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
	switch (evict_policy) {
		case EP_FIFO:
			// FIFO: frame_list에 먼저 삽입된 (가장 오래된) 프레임 선택
			list_init(&frame_list); // 물리 메모리 user pool에 할당된 프레임의 리스트
			break;
		case EP_LLRU:
			// lenient LRU: 마지막 eivction 이후 access되지 않은 프레임에 대해 FIFO
			list_init(&frame_list);
			list_push_back(&frame_list, &frame_nil.elem); // sentinel 용도
			break;
		case EP_CLCK:
			// clock algorith (second wind)
			frame_nil.elem.next = &frame_nil.elem; // 순환 리스트
			frame_nil.elem.prev = &frame_nil.elem;
			break;
	}

	lock_init(&frame_list_lock);
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


static struct page_elem *new_page_elem(struct page *page);
static struct spt_elem *new_spt_elem(struct supplemental_page_table *spt);


static void subscribe_page(struct supplemental_page_table *spt, struct page *page);
static void unsubscribe_page(struct supplemental_page_table *spt, struct page *page);

static void insert_into_frame_list(struct frame *frame);
static void push_accessed_frame_back(void); // Leninent LRU
static struct frame *second_wind(void); // Clock Algorithm


////// SPT COPY
static bool copy_page(struct page *old_page, struct page *new_page);
static void copy_mmap_hash(struct hash *old_h, struct hash *new_h);


///////// HASH TABLE HELPERS
static bool page_va_less_func (const struct hash_elem *a,
		const struct hash_elem *b, void *aux UNUSED);
static uint64_t page_va_hash_func(const struct hash_elem *e, void *aux UNUSED);
static void page_hash_destructor(struct hash_elem *e, void *aux UNUSED);
static bool mmap_addr_less_func (const struct hash_elem *a,
		const struct hash_elem *b, void *aux UNUSED);
static uint64_t mmap_addr_hash_func(const struct hash_elem *e, void *aux UNUSED);
static void mmap_hash_destructor(struct hash_elem *e, void *aux);



//////////////////////////////////////////////////// DEBUG
void print_spt() {
	print_spt_of(&thread_current()->spt, thread_current()->name);
}

void print_spt_of(struct supplemental_page_table *spt, char *name) {
	struct hash *h = &spt->hash;
	struct hash_iterator i;

	printf("================= {%s} SUP. PAGE TABLE (%d entries) =================\n", name, hash_size(h));
	printf("   USER VA    | KERN VA (PA) |     TYPE     | STK | WRT | DRT(K/U) | SHARE \n");

	void *va, *kva;
	enum vm_type type;
	char *type_str, *stack_str, *writable_str, *dirty_k_str, *dirty_u_str;

	hash_first (&i, h);
	struct page_elem *pe;
	struct page *page;
	uint64_t *upte = pml4e_walk(spt->pml4, va, 0);
	while (hash_next (&i)) {
		pe = hash_entry (hash_cur (&i), struct page_elem, elem);
		page = pe->page;

		va = page->va;
		writable_str = page->writable ? "YES" : "NO";

		if (page->frame) {
			kva = page->frame->kva;
			// dirty_k_str = is_dirty(page->frame->kpte) ? "YES" : "NO";
			// dirty_u_str = is_dirty(upte) ? "YES" : "NO";
			// dirty_k_str = " - ";
			// dirty_u_str = " - ";
			dirty_k_str = pml4_is_dirty(base_pml4, page->frame->kva) ?  "YES" : "NO";
			dirty_u_str = pml4_is_dirty(spt->pml4, page->va) ? "YES" : "NO";
		} else {
			kva = NULL;
			dirty_k_str = " - ";
			dirty_u_str = " - ";
		}
		type = page->operations->type;
		if (VM_TYPE(type) == VM_UNINIT) {
			type = page->uninit.type;
			stack_str = " - ";
			struct uninit_page_args *upargs = (struct uninit_page_args *) page->uninit.aux;
			switch (VM_TYPE(type)) {
				case VM_ANON:
					type_str = "UNINIT-ANON";
					stack_str = upargs->is_stack ? "YES" : "NO";
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
		} else {
			stack_str = " - ";
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
		printf(" %12p | %12p | %12s | %3s | %3s |  %3s/%3s | %5d \n",
			   va, kva, type_str, stack_str, writable_str,
			   dirty_k_str, dirty_u_str, page->share_cnt);
	}
}


////////////////////////////////////////////////////////////////////////////////////

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		void *initializer = NULL;
		switch VM_TYPE(type) {
			case VM_ANON:
				initializer = anon_initializer;
				break;
			case VM_FILE:
				initializer = file_backed_initializer;
				break;
			case VM_PAGE_CACHE:
				printf("[DBG] vm_alloc_page_with_initializer(): PAGE_CACHE type is not implemented\n"); /////	
				break;
			default:
				printf("[DBG] vm_alloc_page_with_initializer(): unknown page type (%d)\n", VM_TYPE(type)); /////
		}

		struct page *page = malloc(sizeof(*page));
		if (!page)
			PANIC("[DBG] vm_alloc_page_with_initializer(): malloc for page failed!\n");

		// 새로운 uninit page를 제작
		uninit_new(page, upage, init, type, aux, initializer);
		// uninit_new 함수가 수정 불가이므로 아래에서 추가 작업 수행
		page->writable = writable;
		list_init(&page->share_list); // 현재 페이지를 '구독'중인 spt의 목록
		page->share_cnt = 0;

		/* TODO: Insert the page into the spt. */
		// spt에 새로운 페이지를 삽입
		if (!spt_insert_page(spt, page)) {
			printf("[DBG] vm_alloc_page_with_initializer(): spt_insert_page failed\n"); /////
			free(page);
			goto err;
		}

		return true;
	} else {
		printf("[DBG] vm_alloc_page_with_initializer(): upage is already in spt!\n");
		goto err;
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	// struct page *page = NULL;
	/* TODO: Fill this function. */
	struct page temp_page;
	temp_page.va = pg_round_down(va);

	struct page_elem temp_pe;
	temp_pe.page = &temp_page;

	struct hash_elem *e = hash_find(&spt->hash, &temp_pe.elem);
	return e ? hash_entry(e, struct page_elem, elem)->page : NULL;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt, struct page *page) {
	// int succ = false;
	/* TODO: Fill this function. */
	ASSERT(spt != NULL); /////////////////////////////////////////////////////////////
	ASSERT(page != NULL); /////////////////////////////////////////////////////////////

	subscribe_page(spt, page); // page의 share_list에 참여시킴

	struct page_elem *pe = new_page_elem(page);
	return !hash_insert(&spt->hash, &pe->elem);
	// return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	struct page_elem temp_pe;
	temp_pe.page = page;

	struct hash_elem *e = hash_delete(&spt->hash, &temp_pe.elem); // spt에서 제거
	ASSERT(e != NULL);
	
	struct page_elem *pe = hash_entry(e, struct page_elem, elem);
	free(pe);

	unsubscribe_page(spt, page);
}

static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;

	switch (evict_policy) {
		case EP_FIFO:
			// FIFO: frame_list에 먼저 삽입된 (가장 오래된) 프레임 선택
			ASSERT(!list_empty(&frame_list));
			victim = list_entry(list_begin(&frame_list), struct frame, elem);
			break;
		case EP_LLRU:
			// lenient LRU: 마지막 eivction 이후 access되지 않은 프레임에 대해 FIFO
			ASSERT(list_next(&frame_nil.elem) != list_end(&frame_list));
			victim = list_entry(list_next(&frame_nil.elem), struct frame, elem);
			break;
		case EP_CLCK:
			// clock algorith (second wind)
			ASSERT(list_next(&frame_nil.elem) != &frame_nil.elem);
			victim = second_wind();
			break;
	}

	return victim;
}

/* Get the struct frame, that will be evicted. */
// static struct frame *
// vm_get_victim (void) {
// 	struct frame *victim = NULL;
// 	/* TODO: The policy for eviction is up to you. */
// 	ASSERT(!list_empty(&frame_list));

// 	// FIFO version
// 	victim = list_entry(list_begin(&frame_list), struct frame, elem);

// 	return victim;
// }

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */
	
	// victim을 swap out
	if (!swap_out(victim->page)) {
		PANIC("[DBG] vm_evict_frame(): swap out for victim page (va = %p, kva = %p) failed\n", victim->page->va, victim->kva); ///
	}

	// pml4에서 삭제
	struct list *share_list = &victim->page->share_list;
	struct list_elem *e;
	struct spt_elem *se;
	for (e = list_begin(share_list); e != list_end(share_list); e = list_next(e)) {
		// '구독'중인 pml4에서 모두 삭제
		se = list_entry(e, struct spt_elem, elem);
		pml4_clear_page(se->spt->pml4, victim->page->va);
	}

	// frame_list에서 제거
	list_remove(&victim->elem);
	// page <-> frame 끊기
	victim->page->frame = NULL;
	victim->page = NULL;


	// 받아낸 frame을 반환
	return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	// struct frame *frame = NULL;
	/* TODO: Fill this function. */

	// printf("[DBG] vm_get_frame(): will PAFB\n"); //////////////
	
	// accessed인 프레임은 스택의 맨 뒤로 (lenient LRU 구현)
	if (evict_policy == EP_LLRU && list_next(&frame_nil.elem) != list_end(&frame_list)) {
		push_accessed_frame_back();
	}

	// printf("[DBG] vm_get_frame(): PAFB done\n"); //////////////

	void *kva = palloc_get_page(PAL_USER | PAL_ZERO);

	struct frame *frame = NULL;
	if (kva) {
		// 빈 프레임을 성공적으로 할당받음: 새로운 frame 구조체 생성
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
		// pml4_set_dirty(base_pml4, frame->kva, false);
		// pml4_set_accessed(base_pml4, frame->kva, false);
	}

	// 프레임 리스트에 삽입
	insert_into_frame_list(frame);
	// list_push_back(&frame_list, &frame->elem);

	// dirty, accessed bit을 복구
	pml4_pte_set_dirty(base_pml4, frame->kpte, frame->kva, false);
	pml4_pte_set_accessed(base_pml4, frame->kpte, frame->kva, false);

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
		struct uninit_page_args *upargs = malloc(sizeof(*upargs));
		upargs->is_stack = true;

		if (!vm_alloc_page_with_initializer(VM_ANON, stack_pg, true, NULL, upargs)) {
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
vm_handle_wp (struct page *old_page) {
	// printf("[DBG] vm_handle_wp(): begin, old page (va = %p, kva = %p)\n", old_page->va, old_page->frame->kva); //////////////////////

	// write protect된 page에 write을 하려다가 fault가 난 상황
	
	// 페이지는 물리 메모리 상에 있어야 함
	// 1. 새로운 페이지를 할당받기
	// 2. 페이지 내용을 복사하기
	// 3. 새로운 프레임 할당받기 (기존 페이지가 evict되면 안됨)
	// 4. 기존 페이지에서 unsubscribe
	// 5. 기존 페이지를 spt에서 제거
	// 6. 새로운 페이지를 spt에 넣기
	// 7. 프레임 내용 복사하기

	ASSERT(old_page->frame);
	ASSERT(old_page->share_cnt > 1);

	void *va = old_page->va;

	// 새 페이지 만들기
	struct page *new_page = malloc(sizeof(*new_page));
	if (new_page == NULL) {
		printf("[DBG] vm_handle_wp(): malloc for new_page failed\n"); /////////////////
		return false;
	}
	// 1. 페이지 복사
	if (!copy_page(old_page, new_page)) {
		printf("[DBG] vm_handle_wp(): copy_page() failed\n"); /////////////////
		return false;
	}

	// printf("[DBG] wp {tid = %d} wants to acq lock\n", thread_current()->tid); //////////
	lock_acquire(&frame_list_lock);
	// printf("[DBG] wp {tid = %d} acquired lock\n", thread_current()->tid); //////////
	// 2. 새로운 프레임 할당받기
	struct frame *new_frame = vm_get_frame();
	if (old_page->frame == NULL) {
		// 기존 페이지가 evict되었음, 다시 불러오기
		printf("[DBG] vm_handle_wp(): oops, old page got evicted... bringing it back\n"); ///
		vm_do_claim_page(old_page);
	}
	// 3. 복사한 페이지 세팅
	new_page->frame = new_frame;
	new_frame->page = new_page;
	ASSERT(new_page->va == va);

	// 4. 프레임 복사
	memcpy(new_frame->kva, old_page->frame->kva, PGSIZE);

	// 기존 페이지 제거

	// printf("[DBG] from pml4 (%p) removed va (%p)\n", thread_current()->pml4, va); ////////////

	pml4_clear_page(thread_current()->pml4, va); // pml4에서 제거
	spt_remove_page(&thread_current()->spt, old_page); // spt에서 제거

	// 새 페이지 삽입
	if (!spt_insert_page(&thread_current()->spt, new_page)) { // spt에 삽입
		printf("[DBG] vm_handle_wp(): spt_insert_page() failed\n"); /////////////////
		return false;
	}

	// printf("[DBG] from pml4 (%p) inserted va (%p)\n", thread_current()->pml4, va); ////////////

	if (!pml4_set_page(thread_current()->pml4, va, new_frame->kva, new_page->writable)) { // pml4에 삽입
		printf("[DBG] vm_handle_wp(): pml4_set_page() failed\n"); /////////////////
		return false;
	}
	lock_release(&frame_list_lock);
	// printf("[DBG] wp {tid = %d} released lock\n", thread_current()->tid); //////////

	// printf("[DBG] vm_handle_wp(): success!\n"); //////////////
	return true;
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f, void *addr, bool user,
	bool write, bool not_present) {
	
	bool succ = false;

	// struct supplemental_page_table *spt = &thread_current ()->spt;
	// struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */

	// 페이지 폴트를 확인해보자

	// printf("[DBG] =============== vm_try_handle_fault(): begin ================\n"); /////////////////////////////////
	// printf("[DBG] fault by {%s} | spt at %p, pml4 at %p (double check %p)\n",
	// 	thread_current()->name, &thread_current()->spt, thread_current()->pml4, thread_current()->spt.pml4); /////////////////////////////////
	// printf("[DBG] fault info... addr: %p, user: %d, write: %d, not_present: %d\n", addr, user, write, not_present); //////////
	// printf("[DBG] printing spt...\n"); //////////////////////////////////
	// print_spt(); //////////////////////////////////

	// if (!not_present) {
	// 	print_spt();
	// }

	// spt 확인
	// va가 할당되었다면:
	// 1. 데이터가 frame에 없음 (not present)
	//		1. uninit이라서 없음
	//		2. sawp out 당해서 없음
	// 			-> 둘 다 claim으로 해결
	// 2. write 금지에 시도함
	//		1. 진짜 write 금지임: false 반환
	//		2. write protected임: copy on write 하기
	// va가 할당되지 않았다면:
	// 1. 엉뚱한 접근: terminate
	// 2. stack growth

	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = spt_find_page(spt, addr);

	if (!user) {
		// printf("[DBG] vm_try_handle_fault(): received from kernel\n"); /////////
		// printf("[DBG] fault info... addr: %p, user: %d, write: %d, not_present: %d\n", addr, user, write, not_present); //////////


		// print_frame_list(); ////////////////////////////////////////////////////
		// return false;
	}

	if (page) {
		// printf("[DBG] not present page, va = %p\n", page->va); ///////////////////////////
		// 할당된 va로의 접근
		if (not_present) {
			// uninit이거나 swap out당해서 없음
			// printf("[DBG] is it really not present? let's check...\n"); ////////////////
			// uint64_t *pte = pml4e_walk(thread_current()->pml4, page->va, 0); //////////
			// printf("[DBG] found pte at %p (present: %d)\n", pte, pte ? (*(pte) & PTE_P) : 777);
			
			// printf("[DBG] hf {tid = %d} wants to acq lock\n", thread_current()->tid); //////////
			lock_acquire(&frame_list_lock);
			// printf("[DBG] hf {tid = %d} acquired lock\n", thread_current()->tid); //////////
			succ = vm_do_claim_page (page);	
			lock_release(&frame_list_lock);
			// printf("[DBG] hf {tid = %d} released lock\n", thread_current()->tid); //////////
			// printf("[DBG] handle_fault(): vm_do_claim_page(%p) done, printing spt\n", page->va); ////////////////
			// print_spt(); ///////////////////
			goto done;
		} else if (write) {
			// present지만 write을 시도했다가 fault 발생: 금지된 쓰기

			// ASSERT(VM_TYPE(page->operations->type) == VM_FILE); /////////////////////////
			// ASSERT(!page->file.writable); /////////////////////////


			// printf("[DBG] vm_try_handle_fault(): write on read-only page (CoW WIP)\n"); /////////
			// printf("[DBG] ... addr: %p, user: %d, write: %d, not_present: %d\n", addr, user, write, not_present); //////////
			// print_spt(); ////////////////////////////////////////////////////////////

			// printf("[DBG] vm_handle_fault(): received write fault... checking status\n"); /////
			// uint64_t *pte = pml4e_walk(spt->pml4, page->va, 0); ////////
			// if (!pte)
			// 	PANIC("WTF\n");
			// printf("[DBG] pte at %p, is_writable(%d), is_present(%d)\n", pte, is_writable(pte), (*pte) & PTE_P);

			// CoW
			// share된 상태인지 확인
			// 아니라면 false
			// 맞다면 vm_handle_wp()의 결과를 반환

			if (page->share_cnt > 1 && page->writable) {
				// write-protect 상태의 페이지임

				// printf("[DBG] vm_try_handle_fault(): write protect detected!\n"); /////////////////////////////////
				// printf("[DBG] ... addr: %p, user: %d, write: %d, not_present: %d\n", addr, user, write, not_present); //////////
				// printf("[DBG] printing spt...\n"); //////////////////////////////////
				// print_spt(); //////////////////////////////////

				if (!vm_handle_wp(page)) {
					printf("[DBG] vm_try_handle_fault(): vm_handle_wp() failed!\n"); /////////
					goto done;
				} else {
					// printf("[DBG] vm_try_handle_fault(): vm_handle_wp() done!\n"); /////////

					// printf("[DBG] printing spt\n");
					// print_spt();
					succ = true;
					goto done;
				}
				// return vm_handle_wp(page);
			} else if (page->writable) {
				// 왜인지 모르겠지만 pte에서 write 금지가 있음... pml4 재설정
				printf("[DBG] vm_try_handle_fault(): write protection malfunction detected\n"); ////////
				// pml4_clear_page(spt->pml4, page->va);
				// pml4_set_page(spt->pml4, page->va, page->frame->kva, page->writable);
				goto done;
			} else {
				// read-only 페이지임
				goto done;
			}

			// return false;
		} else {
			printf("[DBG] vm_try_handle_fault(): UNKNOWN ERROR (allocated va and present but fault?)\n"); /////////
			goto done;
		}
	} else {
		// stack growth인지 확인
		void *rsp;
		if (user) {
			rsp = f->rsp;
		} else {
			printf("[DBG] vm_try_handle_fault(): kernel rsp checked...?\n"); //////////////
			rsp = thread_current()->tf.rsp;
		}

		// printf("[DBG] vm_try_handle_fault(): checking for stack growth!\n"); /////////////////////////////////
		// printf("[DBG] ... addr: %p, user: %d, write: %d, not_present: %d\n", addr, user, write, not_present); //////////
		// printf("[DBG] f.rsp = %p, tf.rsp = %p\n", f->rsp, thread_current()->tf.rsp);

		// if (f->rsp == addr + 8) {
		if (rsp -8 <= addr && addr <= rsp +32) {
			if (addr < STACK_LIM) {
				printf("[DBG] vm_try_handle_fault(): stack size limit reached!");
				goto done;
			} else {
					
				// fault address가 rsp (-8 ~ +32) 사이임: stack growth

				// printf("[DBG] stack growth it is!\n"); /////////

				vm_stack_growth(spt, addr);
				succ = true;
				goto done;
			}
		} else {
			// stack growth가 아님
			// printf("[DBG] not stack growth!\n"); /////////
			goto done;
		}
			
	}

done:
	// if (!succ) {
	// 	printf("[DBG] ============== vm_try_handle_fault(): recover failed ============\n"); /////////////////////////////////
	// 	printf("[DBG] fault by {%s} | spt at %p, pml4 at %p (double check %p)\n",
	// 		thread_current()->name, &thread_current()->spt, thread_current()->pml4, thread_current()->spt.pml4); /////////////////////////////////
	// 	printf("[DBG] fault info... addr: %p, user: %d, write: %d, not_present: %d\n", addr, user, write, not_present); //////////
	// 	printf("[DBG] printing spt...\n"); //////////////////////////////////
	// 	print_spt(); //////////////////////////////////
	// }
	return succ;
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
	// struct page *page = NULL;
	/* TODO: Fill this function */

	struct page *page = spt_find_page(&thread_current()->spt, va);

	if (!page)
		PANIC("[DBG] vm_claim_page(): spt_find_page() failed\n");

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */

	// pml4에 삽입
	bool writable = page->writable && page->share_cnt == 1; // write-protect 아니어야 writable
	struct list *share_list = &page->share_list;
	struct list_elem *e;
	struct spt_elem *se;

	for (e = list_begin(share_list); e != list_end(share_list); e = list_next(e)) {
		// 페이지에 subscribe중인 모든 spt에 대해 pml4에 삽입
		se = list_entry(e, struct spt_elem, elem);

		///////////////////////////////////////////////////////////
		// pml4에 이미 present하는지 체크
		// uint64_t *pte = pml4e_walk(se->spt->pml4, page->va, 0);
		// if (pte && (*pte) & PTE_P) {
		// 	PANIC("[DBG] vm_do_claim_page(): page is already present in pml4!\n");
		// }
		///////////////////////////////////////////////////////////
	
		// printf("[DBG] from pml4 (%p) inserted va (%p)\n", se->spt->pml4, page->va); ////////////
	
		pml4_set_page(se->spt->pml4, page->va, frame->kva, writable);
	}
	ASSERT(frame->kpte != NULL); //////////////////////////////////////////////////////

	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	ASSERT(&thread_current()->spt == spt);
	// spt에 pml4를 연결하는 부분은 process.c load() / process.c __do_fork()에 있음
	// pml4 초기화보다 spt 초기화가 먼저 호출되므로


	hash_init(&spt->hash, page_va_hash_func, page_va_less_func, spt);
	// list_init(&spt->mmap_list);
	hash_init(&spt->mmap_hash, mmap_addr_hash_func, mmap_addr_less_func, spt);

	// printf("[DBG] supplemental_page_table_init(%p) done, mmap hash aux = %p\n", spt, spt->mmap_hash.aux); ////////////

}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src) {
	struct hash_iterator i;

	hash_first (&i, &src->hash);
	struct page_elem *pe;
	struct page *page;
	while (hash_next (&i)) {
		pe = hash_entry (hash_cur (&i), struct page_elem, elem);
		page = pe->page;

		// 자신의 spt에 삽입
		if (!spt_insert_page(dst, page)) {
			printf("[DBG] supplemental_page_table_copy(): spt_insert_page failed\n"); /////
			return false;
		}
		if (page->frame) {
			// 물리 메모리 상에 있다면 pml4에도 삽입 (write-protect 상태로)

			// printf("[DBG] from pml4 (%p) inserted va (%p)\n", thread_current()->pml4, page->va); ////////////

			pml4_set_page(dst->pml4, page->va, page->frame->kva, false);
			// dirty 상태 여부도 기존 페이지 주인에게 상속받기
			bool is_dirty = pml4_is_dirty(src->pml4, page->va);
			if (is_dirty) {
				pml4_set_dirty(dst->pml4, page->va, is_dirty);
			}
		}
	}

	// list_init(&dst->mmap_list);
	hash_init(&dst->mmap_hash, mmap_addr_hash_func, mmap_addr_less_func, dst);
	// copy_mmap_list(&src->mmap_list, &dst->mmap_list);
	copy_mmap_hash(&src->mmap_hash, &dst->mmap_hash);

	return true;
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt) {
	// printf("[DBG] supplemental_page_table_kill(): {%s (tid = %d)} wants to kill spt at %p\n", thread_current()->name, thread_current()->tid, spt); ///////
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */

	// process_exec() 하지 않고 thread_create()로 생성된 쓰레드는
	// supplemental_page_table_init()을 수행하지 않으면서 kill을 수행하므로,
	// process_exec()로 생성된 쓰레드 (is_user가 true인 쓰레드)에 대해서만 수행
	if (thread_current()->is_user) {
		// printf("[DBG] supplemental_page_table_kill(%p), {%s}'s spt is at %p, mmap hash aux = %p\n", spt, thread_current()->name, &thread_current()->spt, spt->mmap_hash.aux); //////////////
		// printf("[DBG] supplemental_page_table_kill(%p), mmap hash aux = %p\n", spt, spt->mmap_hash.aux); //////////////
		// printf("[DBG] sk {tid = %d} wants to acq lock\n", thread_current()->tid); //////////
		lock_acquire(&frame_list_lock);
		// printf("[DBG] sk {tid = %d} acquired lock\n", thread_current()->tid); //////////
		hash_clear(&spt->mmap_hash, mmap_hash_destructor); // munmap 정리
		hash_clear(&spt->hash, page_hash_destructor); // spt 정리
		lock_release(&frame_list_lock);
		// printf("[DBG] sk {tid = %d} released lock\n", thread_current()->tid); //////////
	}

	// printf("[DBG] supplemental_page_table_kill(): done\n"); ////////////////////////
}

////////////////////////////// FOR SYSCALL ////////////////////
// 주어진 주소의 페이지가 writable인지 반환
bool vm_get_addr_writable(void *va) {
	struct page *page = spt_find_page(&thread_current()->spt, va);

	// WIP: page가 없어도 writable이라고 판단해야함?
	return page && page->writable;
}

// P3
// 주어진 주소의 페이지가 spt에 있는지 여부 반환
bool vm_get_addr_readable(void *va) {
	struct page *page = spt_find_page(&thread_current()->spt, va);

	return page;
}

//////////////////////////// STATICS ////////////////////////////////////////////


// hash table에 삽입할 page_elem 만들어서 반환
static struct page_elem *new_page_elem(struct page *page) {
	struct page_elem *pe = malloc(sizeof(*pe));
	if (!pe)
		PANIC("[DBG] new_page_elem(): malloc for page_elem failed!\n");

	pe->page = page;
	return pe;
}

// share_list에 삽입할 spt_elem 만들어서 반환
static struct spt_elem *new_spt_elem(struct supplemental_page_table *spt) {
	struct spt_elem *se = malloc(sizeof(*se));
	if (!se)
		PANIC("[DBG] new_spt_elem(): malloc for spt_elem failed!\n");

	se->spt = spt;
	return se;
}




// 현재 쓰레드를 주어진 page의 share에 참여시킴
static void subscribe_page(struct supplemental_page_table *spt, struct page *page) {
	ASSERT(page != NULL);
	struct spt_elem *se = new_spt_elem(spt); // share_list에 삽입할 구조체

	if (page->share_cnt == 1) {
		// 페이지가 최초로 공유됨
		if (page->frame) {
			// 물리 메모리 상에 있다면 원래 주인의 pte를 write-protect
			struct spt_elem *src_se = list_entry(list_begin(&page->share_list), struct spt_elem, elem);
			

			// printf("[DBG] subscribe_page(): TODO: check if DIRTY is preserved after pml4_set_page\n"); //////

			// uint64_t *pte;
			// printf("[DBG] before reset: checking status\n"); /////
			// pte = pml4e_walk(src_se->spt->pml4, page->va, 0); ////////
			// if (!pte)
			// 	PANIC("WTF\n");
			// printf("[DBG] pte at %p, W(%d), A(%d), D(%d)\n", pte,
			// 	(*pte) & PTE_W, (*pte) & PTE_A, (*pte) & PTE_D);
			

			pml4_set_writable(src_se->spt->pml4, page->va, false);


			// printf("[DBG] from pml4 (%p) removed va (%p)\n", src_se->spt->pml4, page->va); ////////////


			// pml4_clear_page(src_se->spt->pml4, page->va);

			// printf("[DBG] from pml4 (%p) inserted va (%p)\n", src_se->spt->pml4, page->va); ////////////

			// pml4_set_page(src_se->spt->pml4, page->va, page->frame->kva, false);

			// printf("[DBG] after reset: checking status\n"); /////
			// pte = pml4e_walk(src_se->spt->pml4, page->va, 0); ////////
			// if (!pte)
			// 	PANIC("WTF\n");
			// printf("[DBG] pte at %p, W(%d), A(%d), D(%d)\n", pte,
			// 	(*pte) & PTE_W, (*pte) & PTE_A, (*pte) & PTE_D);
		}
	}
	// 자신을 share_list에 삽입
	list_push_back(&page->share_list, &se->elem);
	page->share_cnt++;
}

// 현재 쓰레드를 주어진 page의 share에서 제외시킴
static void unsubscribe_page(struct supplemental_page_table *spt, struct page *page) {
	ASSERT(page != NULL);
	struct list *share_list = &page->share_list;
	struct list_elem *e;
	struct spt_elem *se;

	// share_list에서 자신의 spt_elem을 탐색
	for (e = list_begin(share_list); e != list_end(share_list); e = list_next(e)) {
		se = list_entry(e, struct spt_elem, elem);
		if (se->spt == spt) {
			break;
		}
	}
	ASSERT(e != list_end(share_list)); // share_list안에는 무조건 자기 자신이 존재해야 함
	list_remove(&se->elem); // share_list로부터 삭제
	free(se);
	page->share_cnt--;

	if (page->share_cnt == 1) {
		ASSERT(!list_empty(&page->share_list));
		// 페이지가 더 이상 공유되지 않음
		if (page->frame) {
			// 물리 메모리 상에 있다면 write-protect 해제
			struct spt_elem *src_se = list_entry(list_begin(&page->share_list), struct spt_elem, elem);
			// printf("[DBG] unsubscribe_page(): lifting write-protection at va = %p, kva = %p\n", page->va, page->frame->kva); /////////
			// printf("[DBG] spt at %p, pml4 at %p\n", src_se->spt, src_se->spt->pml4); //////

			// pml4_set_writable(src_se->spt->pml4, page->va, page->writable);

			// printf("[DBG] unsubscribe_page(): TODO: check if DIRTY is preserved after pml4_set_page\n"); //////

			// uint64_t *pte;
			// printf("[DBG] before reset: checking status\n"); /////
			// pte = pml4e_walk(src_se->spt->pml4, page->va, 0); ////////
			// if (!pte)
			// 	PANIC("WTF\n");
			// printf("[DBG] pte at %p, W(%d), A(%d), D(%d)\n", pte,
			// 	(*pte) & PTE_W, (*pte) & PTE_A, (*pte) & PTE_D);
			

			pml4_set_writable(src_se->spt->pml4, page->va, page->writable);

			// printf("[DBG] from pml4 (%p) removed va (%p)\n", src_se->spt->pml4, page->va); ////////////

			// pml4_clear_page(src_se->spt->pml4, page->va);

			// printf("[DBG] from pml4 (%p) inserted va (%p)\n", src_se->spt->pml4, page->va); ////////////

			// pml4_set_page(src_se->spt->pml4, page->va, page->frame->kva, page->writable);

			// printf("[DBG] after reset: checking status\n"); /////
			// pte = pml4e_walk(src_se->spt->pml4, page->va, 0); ////////
			// if (!pte)
			// 	PANIC("WTF\n");
			// printf("[DBG] pte at %p, W(%d), A(%d), D(%d)\n", pte,
			// 	(*pte) & PTE_W, (*pte) & PTE_A, (*pte) & PTE_D);

			// printf("[DBG] checking the lift result:\n"); ////////////////
			// uint64_t *pte = pml4e_walk(src_se->spt->pml4, page->va, 0); ////////
			// if (!pte)
			// 	PANIC("WTF\n");
			// printf("[DBG] pte at %p, is_writable(%d), is_present(%d)\n", pte, is_writable(pte), (*pte) & PTE_P);
		}
	} else if (page->share_cnt == 0) {
		// 더 이상 share중인 페이지가 없다면 페이지를 삭제
		if (page->frame) {
			// 물리 메모리 상에 있다면 프레임을 반환
			list_remove(&page->frame->elem);
			free(page->frame);
		}
		vm_dealloc_page (page);
	}
}

static void insert_into_frame_list(struct frame *frame) {
	if (evict_policy == EP_CLCK) {
		// clock algorithm은 기존 list 구조체 대신 순환 리스트로 구현
		// sentinel 직전에 삽입
		list_insert(&frame_nil.elem, &frame->elem);
	} else {
		// if (frame->kva == NULL) { ///////////////////
		// 	printf("[DBG] insert_into_frame_list(): frame at %p, page at %p, kva = %p\n", frame, frame->page, frame->kva);
		// }
		list_push_back(&frame_list, &frame->elem);
	}
}

// EVICT POLICY

// Lenient LRU에서 사용
// frame_list에서 accessed 비트가 표시된 페이지들을 뒤로 밀고 accessed를 제거
static void push_accessed_frame_back(void) {
	// printf("[DBG] push(): nil elem at %p\n", &frame_nil.elem); //////////////
	// printf("\n[DBG] push_accessed_frame_back() {%s} start, printing frames\n", thread_current()->name); /////////
	// print_frame_list(); ////////////
// 	return;
// }
	// printf("[DBG] push start->"); /////////////
	struct list *share_list;
	struct list_elem *e, *e2;

	struct frame *frame;
	void *va;
	bool accessed;

	struct spt_elem *se;
	struct lock *share_list_lock;

	struct list_elem *nil_elem = &frame_nil.elem; // sentinel

	list_remove(nil_elem);
	list_push_back(&frame_list, nil_elem); // sentinel을 맨 뒤로 보내기

	for (e = list_begin(&frame_list); e != nil_elem; e = list_next(e)) {
		// printf("1->"); /////////////
		frame = list_entry(e, struct frame, elem);
		// printf("2->"); /////////////
		// printf("[DBG] PAFB(): iterating... e at %p, frame at %p, page at %p\n", e, frame, frame->page); ///////
		if (frame->page == NULL) { ////////////////////////
			printf("[DBG] PAFB(): pageless frame detected!\n");
			if (frame == &frame_nil) {
				PANIC("[DBG] i found sentinel... wtf\n");
			}
			printf("[DBG] frame at %p, kva = %p\n", frame, frame->kva);
			print_spt();
			PANIC("I DIE\n");
		}

		va = frame->page->va;
		accessed = false;
		// '구독'중인 모든 spt의 pml4의 accessed bit 확인
		share_list = &frame->page->share_list;
		// share_list_lock = &frame->page->share_list_lock;
		// printf("3->"); /////////////
		// lock_acquire(share_list_lock);
		// printf("4->"); /////////////
		for (e2 = list_begin(share_list); e2 != list_end(share_list); e2 = list_next(e2)) {
			se = list_entry(e2, struct spt_elem, elem);

			// if (va == NULL) {
			// 	printf("va is null\n");
			// } else if (pg_ofs(va) != 0) {
			// 	printf("va is weird (%p)\n", va);
			// } else if (se->spt == NULL) {
			// 	printf("spt is null\n");
			// } else if (se->spt->pml4 == NULL) {
			// 	printf("pml4 is null\n");
			// } else {
			// 	uint64_t *pte = pml4e_walk(se->spt->pml4, va, 0);
			// }


			if (pml4_is_accessed(se->spt->pml4, va)) {
				accessed = true;
				pml4_set_accessed(se->spt->pml4, va, false); // 복구
				break;
			}
		}
		// lock_release(share_list_lock);
		// printf("4->"); /////////////
		// 커널 pml4의 accessed bit 확인
		if (!accessed && (*frame->kpte) & PTE_A) {
			// user page 모두 accessed 아닐 때만 확인
			accessed = true;
			pml4_set_accessed(base_pml4, frame->kva, false); // 복구
		}

		// printf("5->"); /////////////

		// accessed인 프레임은 맨 뒤로 보냄
		if (accessed) {
			e = list_prev(e);
			list_remove(&frame->elem);
			list_push_back(&frame_list, &frame->elem);
		}
		// printf("6->"); /////////////
	}
	// printf("done\n"); ///////////////

	list_remove(nil_elem);
	list_push_front(&frame_list, nil_elem); // sentinel 다시 맨 앞으로
}

// clock algorithm에서 사용
// accessed bit이 0인 첫 번째 프레임을 탐색, 1인 프레임은 0으로 만들고 스킵
static struct frame *second_wind(void) {
	struct list *share_list;
	struct list_elem *e, *e2;

	struct frame *frame;
	void *va;
	bool accessed;

	struct spt_elem *se;
	struct lock *share_list_lock;

	struct list_elem *nil_elem = &frame_nil.elem; // sentinel

	e = list_next(nil_elem);
	list_remove(nil_elem); // sentinel 뽑아놓기
	
	while (e) {
		frame = list_entry(e, struct frame, elem);

		va = frame->page->va;
		accessed = false;

		// '구독'중인 모든 spt의 pml4의 accessed bit 확인
		share_list = &frame->page->share_list;
		for (e2 = list_begin(share_list); e2 != list_end(share_list); e2 = list_next(e2)) {
			se = list_entry(e2, struct spt_elem, elem);

			if (pml4_is_accessed(se->spt->pml4, va)) {
				accessed = true;
				pml4_set_accessed(se->spt->pml4, va, false); // 복구
				break;
			}
		}

		// 커널 pml4의 accessed bit 확인
		if (!accessed && (*frame->kpte) & PTE_A) {
			// user page 모두 accessed 아닐 때만 확인
			accessed = true;
			pml4_set_accessed(base_pml4, frame->kva, false); // 복구
		}

		// accessed 0인 경우 evict할 페이지로 선택
		if (!accessed) {
			break;
		} else {
			e = list_next(e);
		}
	}

	list_insert(e, nil_elem); // sentinel을 탐색 지점으로 이동

	return frame;
}


////////////// COPY SPT

// old_page의 내용을 new_page로 똑같이 복사
static bool copy_page(struct page *old_page, struct page *new_page) {
	enum vm_type type = old_page->operations->type;

	// 페이지를 복사
	memcpy(new_page, old_page, sizeof(struct page));
	// 프레임 때기
	new_page->frame = NULL;
	// 페이지 초기화
	new_page->writable = old_page->writable;
	list_init(&new_page->share_list);
	new_page->share_cnt = 0;
	
	switch(VM_TYPE(type)) {
		case VM_UNINIT:
			PANIC("[DBG] copy_page(): why did uninit page write faulted??\n"); ////
		case VM_ANON:
			break;
		case VM_FILE:
			break;
		case VM_PAGE_CACHE:
			printf("[DBG] copy_page_cache_page() is not implemented yet\n");
			break;
		default:
			printf("[DBG] copy_page(): unknown page type (%d)\n", VM_TYPE(type)); /////
	}

	return true;
}

// static void copy_mmap_list(struct list *old_l, struct list *new_l) {
// 	struct list_elem *e;
// 	struct mmap_elem *old_me;
// 	struct mmap_elem *new_me;
// 	for (e = list_begin(old_l); e != list_end(old_l); e = list_next(e)) {
// 		old_me = list_entry(e, struct mmap_elem, elem);
// 		new_me = malloc(sizeof(*new_me));
// 		if (!new_me) {
// 			PANIC("copy_mmap_list(): malloc for new_me failed!\n");
// 		}
// 		new_me->addr = old_me->addr;
// 		new_me->pg_cnt = old_me->pg_cnt;
// 		list_push_back(new_l, &new_me->elem);
// 	}
// }

static void copy_mmap_hash(struct hash *old_h, struct hash *new_h) {
	struct hash_iterator i;

	struct hash_elem *e;
	struct mmap_elem *old_me;
	struct mmap_elem *new_me;

	hash_first (&i, old_h);
	while (hash_next (&i))
   	{
		old_me = hash_entry (hash_cur (&i), struct mmap_elem, elem);
		new_me = malloc(sizeof(*new_me));
		if (!new_me) {
			PANIC("copy_mmap_hash(): malloc for new_me failed!\n");
		}

		new_me->file = file_reopen(old_me->file);
		new_me->addr = old_me->addr;
		new_me->pg_cnt = old_me->pg_cnt;

		hash_insert(old_h, &new_me->elem);
	}
}

// mmap_hash의 모든 요소를 munmap하고 hash를 빈 상태로 만들기
// static void clear_mmap_hash(struct hash *h) {
// 	hash_clear(h, mmap_hash_destructor);
// }

// WIP
// static void clear_mmap_hash(struct list *old_l, struct list *new_l) {
// 	struct list_elem *e;
// 	struct mmap_elem *old_me;
// 	struct mmap_elem *new_me;
// 	for (e = list_begin(old_l); e != list_end(old_l); e = list_next(e)) {
// 		old_me = list_entry(e, struct mmap_elem, elem);
// 		new_me = malloc(sizeof(*new_me));
// 		if (!new_me) {
// 			PANIC("copy_mmap_list(): malloc for new_me failed!\n");
// 		}
// 		new_me->addr = old_me->addr;
// 		new_me->pg_cnt = old_me->pg_cnt;
// 		list_push_back(new_l, &new_me->elem);
// 	}
// }






///////// HASH TABLE HELPERS
static bool page_va_less_func (const struct hash_elem *a,
		const struct hash_elem *b, void *aux UNUSED) {
	struct page_elem *pea = hash_entry(a, struct page_elem, elem);
	struct page_elem *peb = hash_entry(b, struct page_elem, elem);

	return pea->page->va < peb->page->va;
}

static uint64_t page_va_hash_func(const struct hash_elem *e, void *aux UNUSED) {
	struct page_elem *pe = hash_entry(e, struct page_elem, elem);

	return hash_bytes(&pe->page->va, sizeof(void*));
}

static void page_hash_destructor(struct hash_elem *e, void *aux) {
	struct supplemental_page_table *spt = (struct supplemental_page_table *) aux;
	struct page_elem *pe = hash_entry(e, struct page_elem, elem);
	struct page *page = pe->page;

	// printf("[DBG] checking page from destructor\n"); ///////////
	// printf("[DBG] spt at %p, pml4 at %p, page va = %p\n", spt, spt->pml4, page->va); //////

	if (page->frame) {
		pml4_clear_page(spt->pml4, page->va);
	}
	unsubscribe_page(spt, page);

	free(pe);
}

static bool mmap_addr_less_func (const struct hash_elem *a,
		const struct hash_elem *b, void *aux UNUSED) {
	struct mmap_elem *mea = hash_entry(a, struct mmap_elem, elem);
	struct mmap_elem *meb = hash_entry(b, struct mmap_elem, elem);

	return mea->addr < meb->addr;
}

static uint64_t mmap_addr_hash_func(const struct hash_elem *e, void *aux UNUSED) {
	struct mmap_elem *me = hash_entry(e, struct mmap_elem, elem);

	return hash_bytes(&me->addr, sizeof(void*));
}

static void mmap_hash_destructor(struct hash_elem *e, void *aux) {
	// printf("[DBG] mmap_hash_destructor(): begin\n"); /////////////////////
	// print_spt(); ///////////////////
	struct supplemental_page_table *spt = (struct supplemental_page_table *) aux;

	struct mmap_elem *me = hash_entry(e, struct mmap_elem, elem);
	void *addr = me->addr;
	// printf("[DBG] mmap_hash_destructor(): pg_cnt = %d\n", me->pg_cnt); ///////////////////////

	struct page *page;
	// mmap으로 생성된 file_page를 모두 제거
	for (int i = 0; i < me->pg_cnt; i++) {
		page = spt_find_page(spt, addr + PGSIZE * i);
		// printf("[DBG] i = %d, va = %p\n", i, page->va); //////////////////
		ASSERT(page != NULL); ////////////////////////////////////////
		
		if (page->frame) {
			// printf("[DBG] mmap_hash_destructor(): calling write-back (va = %p, kva = %p)\n", page->va, page->frame->kva); ////////////////////////
			file_backed_write_back(page, me->file);
		}
		spt_remove_page(&thread_current()->spt, page);
	}

	file_close(me->file);
	free(me);
}
