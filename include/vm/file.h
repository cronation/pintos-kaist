#ifndef VM_FILE_H
#define VM_FILE_H
#include "filesys/file.h"
#include "vm/vm.h"

#include <list.h> // P3

struct page;
enum vm_type;

struct file_page { // uninit의 aux에 저장되는 file_page_args를 그대로 받아옴
	void *addr; // mmap시 파일에 할당된 가상 주소
	off_t ofs;
	uint32_t page_read_bytes;
	uint32_t page_zero_bytes;
};

void vm_file_init (void);
bool file_backed_initializer (struct page *page, enum vm_type type, void *kva);
void file_backed_write_back(struct page *page, struct file *file); // P3
void *do_mmap(void *addr, size_t length, int writable,
		struct file *file, off_t offset);
void do_munmap (void *va);
#endif
