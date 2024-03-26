#ifndef VM_ANON_H
#define VM_ANON_H
#include "vm/vm.h"
struct page;
enum vm_type;

struct anon_page {
    uint32_t swap_sec_no; // swap disk상의 sector number
    bool is_stack; // 현재 anonymous 페이지가 stack에 속하는지 여부
};

void vm_anon_init (void);
bool anon_initializer (struct page *page, enum vm_type type, void *kva);

#endif
