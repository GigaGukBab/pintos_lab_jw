#include "userprog/syscall.h"

#include <stdio.h>
#include <syscall-nr.h>

#include "include/devices/input.h"
#include "include/filesys/file.h"
#include "include/filesys/filesys.h"
#include "include/lib/string.h"
#include "include/lib/user/syscall.h"
#include "include/threads/init.h"
#include "include/userprog/process.h"
#include "intrinsic.h"
#include "lib/kernel/console.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/loader.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "userprog/gdt.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

struct lock filesys_lock;

/* check address for validation
 * - 커널 영역 접근 차단
 * - 미매핑 페이지 차단 */
void check_address(void *vaddr)
{
    struct thread *curr = thread_current();
    if (vaddr == NULL || !is_user_vaddr(vaddr) ||
        pml4_get_page(curr->pml4, vaddr) == NULL)
    {
        exit(-1);
    }
}

/* protect from bad file descriptor number */
void check_fd(int fd)
{
    if (fd < 0 || fd > FDTABLE_SIZE)
    {
        exit(-1);
    }
}

/* 시스템 초기화 시 시스템 콜 벡터 설정 등의 초기화 작업 수행 */
void syscall_init(void)
{
    write_msr(MSR_STAR, ((uint64_t) SEL_UCSEG - 0x10) << 48 |
                            ((uint64_t) SEL_KCSEG) << 32);
    write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

    /* The interrupt service rountine should not serve any interrupts
     * until the syscall_entry swaps the userland stack to the kernel
     * mode stack. Therefore, we masked the FLAG_FL. */
    write_msr(MSR_SYSCALL_MASK,
              FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

    /* initialize filesys lock */
    lock_init(&filesys_lock);
}

/* system power off */
void halt(void)
{
    power_off();
}

/* exit with given status number */
void exit(int status)
{
    struct thread *t = thread_current();
    t->exit_status = status;
    printf("%s: exit(%d)\n", t->name,
           t->exit_status); /* Process Termination Message */

    if (lock_held_by_current_thread(&filesys_lock))
    {
        lock_release(&filesys_lock);
    }

    thread_exit();
}

// pid_t fork(const char *thread_name)
// {
//     struct intr_frame *if_ =
//         pg_round_up(&thread_name) - sizeof(struct intr_frame);
//     return process_fork(thread_name, if_);
// }

/* execute program with given file name */
int exec(const char *file)
{
    check_address(file);

    int result = process_exec(file);
    if (result == -1)
    {
        exit(-1);
    }

    return result;
}

/* wait process until it's finish (exit) */
int wait(pid_t pid)
{
    return process_wait(pid);
}

bool create(const char *file, unsigned initial_size)
{
    bool file_create_result = false;

    check_address(file);

    lock_acquire(&filesys_lock);
    file_create_result = filesys_create(file, initial_size);
    lock_release(&filesys_lock);
    return file_create_result;
}

bool remove(const char *file)
{
    check_address(file);

    bool file_remove_result = false;

    lock_acquire(&filesys_lock);
    file_remove_result = filesys_remove(file);
    lock_release(&filesys_lock);

    return file_remove_result;
}

int open(const char *file)
{
    check_address(file);

    lock_acquire(&filesys_lock);
    struct file *open_file = filesys_open(file);
    lock_release(&filesys_lock);

    if (open_file != NULL)
    {
        return process_add_file(open_file);
    }
    else
    {
        return -1;
    }
}

int filesize(int fd)
{
    check_fd(fd);

    struct uni_file *uni_file = process_get_file(fd);
    if (uni_file == NULL)
    {
        return -1;
    }

    struct file *file = uni_file->ptr;
    if (file == NULL)
    {
        return -1;
    }

    return file_length(file);
}

int read(int fd, void *buffer, unsigned length)
{
    check_address(buffer);
    check_fd(fd);

    int bytes_read;

    if (fd == FD_STDIN)
    {
        /* >> (1) STDIN - 키보드에서 length 바이트만큼 읽기 */
        unsigned i;
        for (i = 0; i < length; i++)
        {
            /* 한 글자씩 채움 */
            ((char *) buffer)[i] = input_getc();
        }

        return length;
    }
    else if (fd == FD_STDOUT)
    {
        /* >> (2) STDOUT - Not allowed to read on Write-Only file */
        return -1;
    }
    else
    {
        /* >> (3): read from file */
        struct uni_file *uni_file = process_get_file(fd);
        if (uni_file == NULL)
        {
            return -1;
        }

        struct file *file = uni_file->ptr;
        if (file == NULL)
        {
            return -1;
        }

        lock_acquire(&filesys_lock);
        bytes_read = file_read(file, buffer, length);
        lock_release(&filesys_lock);

        return bytes_read;
    }
}

int write(int fd, const void *buffer, unsigned size)
{
    check_address(buffer);
    check_fd(fd);

    struct thread *curr = thread_current();

    if (fd == FD_STDIN)
    {
        /* >> (1): writing on Read-Only file is not allowed */
        return -1;
    }
    else if (fd == FD_STDOUT)
    {
        /* >> (2): write on STDOUT */
        putbuf((char *) buffer, size);
        return size;
    }
    else
    {
        /* >> (3): write on file */
        struct uni_file *uni_file = process_get_file(fd);
        if (uni_file == NULL)
        {
            return -1;
        }

        struct file *file = uni_file->ptr;
        if (file == NULL)
        {
            return -1;
        }

        lock_acquire(&filesys_lock);
        int bytes_written = file_write(file, buffer, size);
        lock_release(&filesys_lock);
        return bytes_written;
    }
}

/* fd로 열린 파일에서 읽거나 쓸 다음 바이트를 파일 시작부터 position(바이트
 * 단위) 위치로 설정합니다. (따라서 position이 0이면 파일의 시작을 의미합니다.)
 * 파일의 현재 끝을 넘어서는 seek는 오류가 아닙니다. 이후 read는 0바이트를
 * 반환하여 파일 끝을 나타냅니다. 이후 write는 파일을 확장하며,
 * 쓰이지 않은 간격을 0으로 채웁니다.
 * (그러나 Pintos에서는 프로젝트 4가 완료될 때까지 파일 길이가 고정되어
 * 있으므로, 파일 끝을 넘는 쓰기는 오류를 반환합니다.) 이러한 동작 방식은 파일
 * 시스템에 구현되어 있으므로 system call 구현에서 별도의 작업이 필요하지
 * 않습니다. */
void seek(int fd, unsigned position)
{
    check_fd(fd);

    if (fd == FD_STDIN || fd == FD_STDOUT)
    {
        return;
    }

    struct uni_file *uni_file = process_get_file(fd);
    if (uni_file == NULL)
    {
        return;
    }

    struct file *file = uni_file->ptr;
    if (file == NULL)
    {
        return;
    }

    file_seek(file, position);
}

unsigned tell(int fd)
{
    check_fd(fd);

    if (fd == FD_STDIN || fd == FD_STDOUT)
    {
        return 0;
    }

    struct uni_file *uni_file = process_get_file(fd);
    if (uni_file == NULL)
    {
        return -1;
    }

    struct file *file = uni_file->ptr;
    if (file == NULL)
    {
        return;
    }

    unsigned position = file_tell(file);
    return position;
}

void close(int fd)
{
    check_fd(fd);

    if (fd == FD_STDIN || fd == FD_STDOUT)
    {
        return;
    }

    struct thread *curr = thread_current();
    struct uni_file *uni_file = curr->fd_table[fd];
    if (uni_file == NULL)
    {
        return;
    }

    if (uni_file->type == FD_FILE && uni_file->ptr != NULL)
    {
        lock_acquire(&filesys_lock);
        file_close((struct file *) uni_file->ptr);
        lock_release(&filesys_lock);
    }

    free(uni_file);
    curr->fd_table[fd] = NULL;
}

/* The main system call interface */
/*
 * 어셈블리 코드(syscall-entry.S)로부터 제어를 넘겨받음
 * 인터럽트 프레임(struct intr_frame *f)을 통해 사용자 프로그램의
 * 레지스터 상태와 시스템 콜 번호 및 인자들을 읽어옴
 */
void syscall_handler(struct intr_frame *f UNUSED)
{
    /* 유저 스택에서 시스템콜 번호 꺼내기 */
    int syscall_number = f->R.rax;

    switch (syscall_number)
    {
        case SYS_HALT:
        {
            halt();
            break;
        }
        case SYS_EXIT:
        {
            exit(f->R.rdi);
            break;
        }
        case SYS_FORK:
        {
            f->R.rax = process_fork(f->R.rdi, f);
            break;
        }
        case SYS_EXEC:
        {
            f->R.rax = exec(f->R.rdi);
            break;
        }
        case SYS_WAIT:
        {
            f->R.rax = wait(f->R.rdi);
            break;
        }
        case SYS_CREATE:
        {
            f->R.rax = create(f->R.rdi, f->R.rsi);
            break;
        }
        case SYS_REMOVE:
        {
            f->R.rax = remove(f->R.rdi);
            break;
        }
        case SYS_OPEN:
        {
            f->R.rax = open(f->R.rdi);
            break;
        }
        case SYS_FILESIZE:
        {
            f->R.rax = filesize(f->R.rdi);
            break;
        }
        case SYS_READ:
        {
            f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
            break;
        }
        case SYS_WRITE:
        {
            f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
            break;
        }
        case SYS_SEEK:
        {
            seek(f->R.rdi, f->R.rsi);
            break;
        }
        case SYS_TELL:
        {
            f->R.rax = tell(f->R.rdi);
            break;
        }
        case SYS_CLOSE:
        {
            close(f->R.rdi);
            break;
        }
    }
}
