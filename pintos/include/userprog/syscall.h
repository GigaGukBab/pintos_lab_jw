#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

extern struct lock filesys_lock;
void syscall_init(void);
void check_address(void *addr);

#endif /* userprog/syscall.h */
