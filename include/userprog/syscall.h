#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);

void syscall_terminate(void); // P2
void print_if(void *if_, char *desc); //////////// DEBUG

#endif /* userprog/syscall.h */
