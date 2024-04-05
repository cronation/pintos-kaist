/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"

#define STACK_LIM 0x47380000 // 47480000 + 1MB

enum ep_enum { // evict policy
	EP_FIFO = 0, // First in First out
	EP_LLRU = 1, // Lenient LRU
	EP_CLCK = 2, // Clock Algorithm (second wind)
};

enum ep_enum evict_policy = EP_CLCK;


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

static void subscribe_page(struct supplemental_page_table *spt,
														struct page *page);
static void unsubscribe_page(struct supplemental_page_table *spt,
														struct page *page);

// Evict policy helpers
static void insert_into_frame_list(struct frame *frame);
static void push_accessed_frame_back(void); // Leninent LRU
static struct frame *second_wind(void); // Clock Algorithm

// SPT copy helpers
static bool copy_page(struct page *old_page, struct page *new_page);
static void copy_mmap_hash(struct hash *old_h, struct hash *new_h);

// Hash table helpers
static bool page_va_less_func (const struct hash_elem *a,
		const struct hash_elem *b, void *aux UNUSED);
static uint64_t page_va_hash_func(const struct hash_elem *e, void *aux UNUSED);
static void page_hash_destructor(struct hash_elem *e, void *aux UNUSED);
static bool mmap_addr_less_func (const struct hash_elem *a,
		const struct hash_elem *b, void *aux UNUSED);
static uint64_t mmap_addr_hash_func(const struct hash_elem *e, void *aux UNUSED);
static void mmap_hash_destructor(struct hash_elem *e, void *aux);


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
				printf("[DBG] vm_alloc_page_with_initializer(): PAGE_CACHE type is not implemented\n");	
				break;
			default:
				printf("[DBG] vm_alloc_page_with_initializer(): unknown page type (%d)\n", VM_TYPE(type));
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
			printf("[DBG] vm_alloc_page_with_initializer(): spt_insert_page failed\n");
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
	struct page temp_page;
	struct page_elem temp_pe;
	temp_page.va = pg_round_down(va);
	temp_pe.page = &temp_page;

	struct hash_elem *e = hash_find(&spt->hash, &temp_pe.elem);
	return e ? hash_entry(e, struct page_elem, elem)->page : NULL;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt, struct page *page) {
	subscribe_page(spt, page); // page의 share_list에 참여시킴

	struct page_elem *pe = new_page_elem(page); // spt에 삽입할 구조체
	return !hash_insert(&spt->hash, &pe->elem);
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	struct page_elem temp_pe;
	temp_pe.page = page;

	struct hash_elem *e = hash_delete(&spt->hash, &temp_pe.elem);
	struct page_elem *pe = hash_entry(e, struct page_elem, elem);
	free(pe);

	unsubscribe_page(spt, page); // page의 share_list에서 제거
}

/* Get the struct frame, that will be evicted. */
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
			// lenient LRU: 마지막 eivction 이후 access되지 않은 프레임 중 FIFO
			ASSERT(!list_empty(&frame_list));
			victim = list_entry(list_begin(&frame_list), struct frame, elem);
			break;
		case EP_CLCK:
			// clock algorith (second wind)
			ASSERT(list_next(&frame_nil.elem) != &frame_nil.elem); // 비었는지
			victim = second_wind();
			break;
	}

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim = vm_get_victim ();

	// victim을 swap out
	if (!swap_out(victim->page)) {
		PANIC("[DBG] vm_evict_frame(): swap out for victim page failed\n");
	}

	// '구독'중인 모든 spt의 pml4에서 삭제
	struct list *share_list = &victim->page->share_list;
	struct list_elem *e;
	struct spt_elem *se;
	for (e = list_begin(share_list);
		 e != list_end(share_list); e = list_next(e)) {
		se = list_entry(e, struct spt_elem, elem);
		pml4_clear_page(se->spt->pml4, victim->page->va);
	}

	list_remove(&victim->elem); // frame_list에서 제거
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
	if (evict_policy == EP_LLRU && !list_empty(&frame_list)) {
		// accessed인 프레임을 스택의 맨 뒤로 보냄 (lenient LRU)
		push_accessed_frame_back();
	}

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
	}

	// 프레임 리스트에 삽입
	insert_into_frame_list(frame);

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

		if (!vm_alloc_page_with_initializer(VM_ANON, stack_pg,
											true, NULL, upargs)) {
			PANIC("[DBG] vm_stack_growth(): vm_alloc_page() failed!\n");
			return;
		}
		
		if (!vm_claim_page(stack_pg)) {
			PANIC("[DBG] vm_stack_growth(): vm_claim_page() failed!\n");
			return;
		}

		stack_pg += PGSIZE;
	}
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *old_page) {
	// write protect된 page에 write을 하려다가 fault가 난 상황
	
	// 페이지는 물리 메모리 상에 있어야 함
	// 1. 새로운 페이지를 할당받기
	// 2. 페이지 내용을 복사하기
	// 3. 새로운 프레임 할당받기 (기존 페이지가 evict되면 안됨)
	// 4. 프레임 내용 복사하기
	// 5. 기존 페이지를 spt에서 제거
	// 6. 새로운 페이지를 spt에 넣기

	ASSERT(old_page->frame);
	ASSERT(old_page->share_cnt > 1);

	void *va = old_page->va;

	// 새 페이지 만들기
	struct page *new_page = malloc(sizeof(*new_page));
	if (new_page == NULL) {
		printf("[DBG] vm_handle_wp(): malloc for new_page failed\n");
		return false;
	}
	// 페이지 복사
	if (!copy_page(old_page, new_page)) {
		printf("[DBG] vm_handle_wp(): copy_page() failed\n");
		free(new_page);
		return false;
	}

	lock_acquire(&frame_list_lock);

	// 새로운 프레임 할당받기
	struct frame *new_frame = vm_get_frame();
	if (old_page->frame == NULL) {
		// 기존 페이지가 evict되었음, 다시 불러오기
		vm_do_claim_page(old_page);
	}

	// 복사한 페이지 세팅
	new_page->frame = new_frame;
	new_frame->page = new_page;
	ASSERT(new_page->va == va);

	// 프레임 복사
	memcpy(new_frame->kva, old_page->frame->kva, PGSIZE);

	// 기존 페이지 제거
	pml4_clear_page(thread_current()->pml4, va); // pml4에서 제거
	spt_remove_page(&thread_current()->spt, old_page); // spt에서 제거

	// 새 페이지 삽입
	if (!spt_insert_page(&thread_current()->spt, new_page)) { // spt에 삽입
		printf("[DBG] vm_handle_wp(): spt_insert_page() failed\n");
		return false;
	}

	if (!pml4_set_page(thread_current()->pml4, va,
					   new_frame->kva, new_page->writable)) { // pml4에 삽입
		printf("[DBG] vm_handle_wp(): pml4_set_page() failed\n");
		return false;
	}

	lock_release(&frame_list_lock);
	return true;
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f, void *addr, bool user,
	bool write, bool not_present) {

	bool succ = false;

	// spt 확인
	// 1. va가 할당된 경우
	// 		a. 데이터가 frame에 없음 (not present)
	//			i. uninit이라서 없음
	//			ii. sawp out 당해서 없음
	// 				-> 둘 다 claim으로 해결
	// 		b. write 금지에 시도함
	//			i. 진짜 write 금지임: false 반환
	//			ii. write protected임: copy on write 하기
	// 2. va가 할당되지 않은 경우
	// 		a. stack growth
	// 		b. 엉뚱한 접근: terminate

	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = spt_find_page(spt, addr);

	if (page) {
		// 할당된 va로의 접근
		if (not_present) {
			// uninit이거나 swap out당해서 없음
			lock_acquire(&frame_list_lock);
			succ = vm_do_claim_page (page);	
			lock_release(&frame_list_lock);
			goto done;
		} else if (write) {
			// present지만 write을 시도했다가 fault 발생: 금지된 쓰기
			if (page->share_cnt > 1 && page->writable) {
				// write-protect 상태의 페이지임
				if (!vm_handle_wp(page)) {
					printf("[DBG] vm_try_handle_fault(): vm_handle_wp() failed!\n");
					goto done;
				} else {
					succ = true;
					goto done;
				}
			} else {
				// read-only 페이지임
				goto done;
			}
		} else {
			printf("[DBG] vm_try_handle_fault(): UNKNOWN ERROR\n");
			goto done;
		}
	} else {
		void *rsp = user ? f->rsp : thread_current()->tf.rsp;

		// stack growth인지 확인
		if (rsp -8 <= addr && addr <= rsp +32) {
			// fault address가 rsp (-8 ~ +32) 사이임: stack growth
			if (addr < STACK_LIM) {
				printf("[DBG] vm_try_handle_fault(): stack size limit reached!");
				goto done;
			} else {	
				vm_stack_growth(spt, addr);
				succ = true;
				goto done;
			}
		} else {
			// stack growth가 아님
			goto done;
		}
	}

done:
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

	// pml4에 삽입
	bool writable = page->writable && page->share_cnt == 1;
	struct list *share_list = &page->share_list;
	struct list_elem *e;
	struct spt_elem *se;

	for (e = list_begin(share_list);
		 e != list_end(share_list); e = list_next(e)) {
		// 페이지에 subscribe중인 모든 spt에 대해 pml4에 삽입
		se = list_entry(e, struct spt_elem, elem);
		pml4_set_page(se->spt->pml4, page->va, frame->kva, writable);
	}

	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	ASSERT(&thread_current()->spt == spt);
	// spt에 pml4 연결은 process.c load() / process.c __do_fork()에서 이루어짐
	// - pml4 초기화보다 spt 초기화가 먼저 호출되므로

	hash_init(&spt->hash, page_va_hash_func, page_va_less_func, spt);
	hash_init(&spt->mmap_hash, mmap_addr_hash_func, mmap_addr_less_func, spt);
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
			printf("[DBG] supplemental_page_table_copy(): spt_insert_page failed\n");
			return false;
		}
		if (page->frame) {
			// 물리 메모리 상에 있다면 pml4에도 삽입 (write-protect 상태로)
			pml4_set_page(dst->pml4, page->va, page->frame->kva, false);

			// dirty 상태 여부도 기존 페이지 주인에게 상속받기
			bool is_dirty = pml4_is_dirty(src->pml4, page->va);
			if (is_dirty) {
				pml4_set_dirty(dst->pml4, page->va, is_dirty);
			}
		}
	}

	hash_init(&dst->mmap_hash, mmap_addr_hash_func, mmap_addr_less_func, dst);
	copy_mmap_hash(&src->mmap_hash, &dst->mmap_hash);

	return true;
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt) {
	// process_exec() 하지 않고 thread_create()로 생성된 쓰레드는
	// supplemental_page_table_init()을 수행하지 않으면서 kill을 수행하므로,
	// process_exec()로 생성된 쓰레드 (is_user가 true인 쓰레드)에 대해서만 수행
	if (thread_current()->is_user) {
		lock_acquire(&frame_list_lock);
		hash_clear(&spt->mmap_hash, mmap_hash_destructor); // munmap 정리
		hash_clear(&spt->hash, page_hash_destructor); // spt 정리
		lock_release(&frame_list_lock);
	}
}

// syscall.c 전용 함수
// 주어진 주소의 페이지가 writable인지 반환
bool vm_get_addr_writable(void *va) {
	struct page *page = spt_find_page(&thread_current()->spt, va);

	// WIP: page가 없어도 writable이라고 판단해야함?

	// page가 없어도 잠재적 stack growth 가능성이 있으므로 true 반환
	return page && page->writable;
}

// 주어진 주소의 페이지가 spt에 있는지 여부 반환
bool vm_get_addr_readable(void *va) {
	struct page *page = spt_find_page(&thread_current()->spt, va);
	return page;
}

////////////////////////////////// STATICS /////////////////////////////////////

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
static void subscribe_page(struct supplemental_page_table *spt,
													struct page *page) {
	ASSERT(page != NULL);
	struct spt_elem *se = new_spt_elem(spt); // share_list에 삽입할 구조체

	if (page->share_cnt == 1) {
		// 페이지가 최초로 공유됨
		if (page->frame) {
			// 물리 메모리 상에 있다면 원래 주인의 pte를 write-protect
			struct spt_elem *src_se = list_entry(list_begin(&page->share_list),
												 struct spt_elem, elem);
			pml4_set_writable(src_se->spt->pml4, page->va, false);
		}
	}
	// 자신을 share_list에 삽입
	list_push_back(&page->share_list, &se->elem);
	page->share_cnt++;
}

// 현재 쓰레드를 주어진 page의 share에서 제외시킴
static void unsubscribe_page(struct supplemental_page_table *spt,
													struct page *page) {
	ASSERT(page != NULL);
	struct list *share_list = &page->share_list;
	struct list_elem *e;
	struct spt_elem *se;

	// share_list에서 자신의 spt_elem을 탐색
	for (e = list_begin(share_list);
		 e != list_end(share_list); e = list_next(e)) {
		se = list_entry(e, struct spt_elem, elem);
		if (se->spt == spt) {
			break;
		}
	}
	list_remove(&se->elem); // share_list로부터 삭제
	free(se);
	page->share_cnt--;


	if (page->share_cnt == 1) {
		ASSERT(!list_empty(&page->share_list));
		// 페이지가 더 이상 공유되지 않음
		if (page->frame) {
			// 물리 메모리 상에 있다면 write-protect 해제
			struct spt_elem *src_se = list_entry(list_begin(&page->share_list),
												 struct spt_elem, elem);
			pml4_set_writable(src_se->spt->pml4, page->va, page->writable);
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

// ======================= [Evict policy helpers] ==============================
static void insert_into_frame_list(struct frame *frame) {
	if (evict_policy == EP_CLCK) {
		// clock algorithm은 기존 list 구조체 대신 순환 리스트로 구현
		// sentinel 직전에 삽입
		list_insert(&frame_nil.elem, &frame->elem);
	} else {
		list_push_back(&frame_list, &frame->elem);
	}
}

// Lenient LRU에서 사용
// frame_list에서 accessed 비트가 표시된 페이지들을 뒤로 밀고 accessed를 제거
static void push_accessed_frame_back(void) {
	struct list_elem *nil_elem = &frame_nil.elem; // 마지막 원소 확인용 sentinel
	list_push_back(&frame_list, nil_elem);

	struct frame *frame;
	void *va;
	bool accessed;

	struct list *share_list;
	struct list_elem *e, *e2;
	struct spt_elem *se;

	for (e = list_begin(&frame_list); e != nil_elem; e = list_next(e)) {
		frame = list_entry(e, struct frame, elem);

		va = frame->page->va;
		accessed = false;
		// '구독'중인 모든 spt의 pml4의 accessed bit 확인
		share_list = &frame->page->share_list;
		for (e2 = list_begin(share_list);
			 e2 != list_end(share_list); e2 = list_next(e2)) {
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

		// accessed인 프레임은 맨 뒤로 보냄
		if (accessed) {
			e = list_prev(e);
			list_remove(&frame->elem);
			list_push_back(&frame_list, &frame->elem);
		}
	}

	list_remove(nil_elem);
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
		for (e2 = list_begin(share_list);
			 e2 != list_end(share_list); e2 = list_next(e2)) {
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

// ========================= [SPT copy helpers] ================================
static bool copy_page(struct page *old_page, struct page *new_page) {
	enum vm_type type = old_page->operations->type;
	
	memcpy(new_page, old_page, sizeof(struct page)); // 페이지를 복사
	new_page->frame = NULL; // 프레임 연결 제거
	// 페이지 초기화
	new_page->writable = old_page->writable;
	list_init(&new_page->share_list);
	new_page->share_cnt = 0;
	
	switch(VM_TYPE(type)) {
		case VM_UNINIT:
			PANIC("[DBG] copy_page(): uninit page cannot write-fault\n");
		case VM_ANON:
			break;
		case VM_FILE:
			break;
		case VM_PAGE_CACHE:
			printf("[DBG] copy_page_cache_page() is not implemented yet\n");
			break;
		default:
			printf("[DBG] copy_page(): unknown page type (%d)\n", VM_TYPE(type));
	}

	return true;
}

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

// ======================= [Hash table functions] ==============================
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
	struct supplemental_page_table *spt = (struct supplemental_page_table*) aux;
	struct page_elem *pe = hash_entry(e, struct page_elem, elem);
	struct page *page = pe->page;

	if (page->frame) { // 프레임에 있다면 pml4에서 제거
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
	struct supplemental_page_table *spt = (struct supplemental_page_table*) aux;

	struct mmap_elem *me = hash_entry(e, struct mmap_elem, elem);
	void *addr = me->addr;

	struct page *page;
	// mmap으로 생성된 file_page를 모두 제거
	for (int i = 0; i < me->pg_cnt; i++) {
		page = spt_find_page(spt, addr + PGSIZE * i);
		
		if (page->frame) {
			file_backed_write_back(page, me->file);
		}
		spt_remove_page(&thread_current()->spt, page);
	}

	file_close(me->file);
	free(me);
}
