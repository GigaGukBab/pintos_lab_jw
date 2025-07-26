#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
#define is_fd_readable(fd) (fd >= 2 && fd < 128)
#define is_fd_writable(fd) (fd >= 1 && fd < 128)

void syscall_init(void);
struct file *process_get_file(int fd);
int process_add_file(struct file *file);

#endif /* userprog/syscall.h */
