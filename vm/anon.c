/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include <bitmap.h> // swap disk table 자료구조 (P3)

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);
// P3
static void write_page_to_swap_disk(struct page *page);
static void read_page_from_swap_disk(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

// swap table (P3)
struct bitmap *swap_bitmap; // swap disk에 할당된 sector의 비트맵
#define pg_to_sec(pg_no) (PGSIZE / DISK_SECTOR_SIZE * (pg_no))

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	swap_disk = disk_get(1, 1);
	// swap disk에 저장할 수 있는 페이지 수와 같은 크기의 비트맵 생성
	swap_bitmap = bitmap_create(disk_size(swap_disk) * DISK_SECTOR_SIZE/PGSIZE);
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &anon_ops;
	vm_initializer *init = page->uninit.init;

	// aux 읽기
	struct uninit_page_args *upargs =
								(struct uninit_page_args *) page->uninit.aux;
	bool is_stack = upargs->is_stack;

	// 옮겨적기
	struct anon_page *anon_page = &page->anon;
	anon_page->is_stack = is_stack;

	if (!init) {
		// stack anon page는 vm_initializer가 없으므로 upargs를 여기서 free
		free(upargs);
	}

	// 메모리 청소
	memset(page->frame->kva, 0, PGSIZE);

	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	read_page_from_swap_disk(page);
	bitmap_set(swap_bitmap, page->anon.swap_pg_no, false); // 스왑 테이블 갱신

	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	// 빈 스왑 페이지 찾기
	page->anon.swap_pg_no = bitmap_scan_and_flip(swap_bitmap, 0, 1, 0);
	write_page_to_swap_disk(page);

	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;
}

// Swap in/out helpers
static void write_page_to_swap_disk(struct page *page) {
	ASSERT(page->frame != NULL);

	size_t sec_no = pg_to_sec(page->anon.swap_pg_no);
	void *addr = page->va;
	for (size_t sec_idx = 0; sec_idx < PGSIZE / DISK_SECTOR_SIZE; sec_idx++) {
		// 페이지 크기만큼 swap disk 섹터에 쓰기
		disk_write(swap_disk, sec_no + sec_idx,
				   addr + sec_idx * DISK_SECTOR_SIZE);
	}
}

static void read_page_from_swap_disk(struct page *page) {
	ASSERT(page->frame != NULL);

	size_t sec_no = pg_to_sec(page->anon.swap_pg_no);
	void *addr = page->va;
	for (size_t sec_idx = 0; sec_idx < PGSIZE / DISK_SECTOR_SIZE; sec_idx++) {
		// 페이지 크기만큼 swap disk 섹터에 쓰기
		disk_read(swap_disk, sec_no + sec_idx,
				   addr + sec_idx * DISK_SECTOR_SIZE);
	}
}