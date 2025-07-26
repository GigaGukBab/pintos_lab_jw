#include "userprog/syscall.h"

#include <stdio.h>
#include <syscall-nr.h>

#include "include/filesys/file.h"
#include "include/filesys/filesys.h"
#include "include/userprog/process.h"
#include "intrinsic.h"
#include "lib/kernel/console.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/loader.h"
#include "threads/thread.h"
#include "userprog/gdt.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);
static bool check_bad_addr();
static int write_handler(int fd, const void *buffer, unsigned size);
static int close_handler(int fd);
struct file *process_get_file(int fd);

static int write_handler(int fd, const void *buffer, unsigned size);

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

// 시스템 초기화 시 시스템 콜 벡터 설정 등의 초기화 작업 수행
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
}

/* The main system call interface */
// 어셈블리 코드(syscall-entry.S)로부터 제어를 넘겨받음
void syscall_handler(struct intr_frame *f UNUSED)
{
    // 유저 스택에서 시스템콜 번호 꺼내기
    int syscall_number = f->R.rax;

    switch (syscall_number)
    {
        case SYS_HALT:
        {
            power_off();
            break;
        }
        case SYS_EXIT:
        {
            struct thread *curr = thread_current();
            curr->exit_status = f->R.rdi;
            thread_exit();
        }
        case SYS_FORK:
        {
            f->R.rax = process_fork(f->R.rdi, f);
            break;
        }
        case SYS_EXEC:
        {
            printf("exec syscall called!!!\n");
            break;
        }
        case SYS_WAIT:
        {
            f->R.rax = process_wait(f->R.rdi);
            break;
        }
        case SYS_CREATE:
        {
            const char *open_filename = (const char *) f->R.rdi;
            int32_t filesize = f->R.rsi;
            struct thread *curr = thread_current();

            if (open_filename == NULL ||
                check_bad_addr(open_filename, curr) == NULL)
            {
                curr->tf.R.rax = -1;
                thread_exit();
            }
            else
            {
                f->R.rax = filesys_create(open_filename, filesize);
                break;
            }
        }
        case SYS_REMOVE:
        {
            printf("remove syscall called!!!\n");
            break;
        }
        case SYS_OPEN:
        {
            struct thread *curr = thread_current();
            if (is_user_vaddr(f->R.rdi))
            {
                const char *open_filename = (const char *) f->R.rdi;
                if (open_filename == NULL ||
                    check_bad_addr(open_filename, curr) == NULL)
                {
                    curr->tf.R.rax = -1;
                    thread_exit();
                }
                else
                {
                    struct file *open_file = filesys_open(open_filename);

                    if (open_file != NULL)
                    {
                        f->R.rax = process_add_file(open_file);
                    }
                    else
                    {
                        f->R.rax = -1;
                    }
                    break;
                }
            }
        }
        case SYS_FILESIZE:
        {
            struct file *current_file = process_get_file(f->R.rdi);
            f->R.rax = file_length(current_file);
            break;
        }
        case SYS_READ:
        {
            struct thread *current_thread = thread_current();

            if (!is_fd_readable(f->R.rdi) ||
                check_bad_addr(f->R.rsi, current_thread) == NULL)
            {
                current_thread->tf.R.rax = -1;
                thread_exit();
            }

            struct file *current_file = process_get_file(f->R.rdi);
            f->R.rax = file_read(current_file, f->R.rsi, f->R.rdx);
            break;
        }
        case SYS_WRITE:
        {
            // 인터럽트 프레임(struct intr_frame *f)을 통해 사용자 프로그램의
            // 레지스터 상태와 시스템 콜 번호 및 인자들을 읽어옴 f->R.rax :
            // 시스템 콜 반환값 (리턴값 저장) f->R.rdi : 첫 번째 인자 (fd)
            // f->R.rsi : 두 번째 인자 (buffer)
            // f->R.rdx : 세 번째 인자 (size)
            f->R.rax = write_handler(f->R.rdi, f->R.rsi, f->R.rdx);
            break;
        }
        case SYS_SEEK:
        {
            printf("seek syscall called!!!\n");
            break;
        }
        case SYS_TELL:
        {
            printf("tell syscall called!!!\n");
            break;
        }
        case SYS_CLOSE:
        {
            // fd_list에서 fd를 가진 file_fd 찾아서
            // 리스트에서 제거하고 file_fd 메모리 해제
            close_handler(f->R.rdi);
            break;
        }
    }
}

static bool check_bad_addr(const char *vaddr, struct thread *t)
{
    return pml4_get_page(t->pml4, vaddr);
}

/* 파일 또는 STDOUT으로 쓰기 */
static int write_handler(int fd, const void *buffer, unsigned size)
{
    struct thread *current_thread = thread_current();

    if (!is_fd_writable(fd) || check_bad_addr(buffer, current_thread) == NULL)
    {
        current_thread->tf.R.rax = -1;
        thread_exit();
    }

    if (!(is_user_vaddr(buffer) && is_user_vaddr(buffer + size)))
    {
        return -1;
    }

    switch (fd)
    {
        case 1: /* fd가 1이면 표준 출력 (파일이 아니라 콘솔로 출력) */
        {
            putbuf(buffer, size);
        }
        default: /* open()으로 연 파일이 할당된 경우 */
        {
            struct file *file = process_get_file(fd);
            if (file == NULL) return -1;
            return file_write(file, buffer, size);
        }
    }
}

static int close_handler(int fd)
{
    struct thread *current_thread = thread_current();

    if (fd < 2 || fd > 127)
    {
        return -1;
    }

    if (current_thread->fdt[fd] == NULL)
    {
        return -1;
    }
    else
    {
        current_thread->fdt[fd] = NULL;
        free(current_thread->fdt[fd]);
        return 0;
    }
}

// 헌재 실행 중인 프로세스의 열린 파일 리스트에서 특정 파일 디스크립터(fd)에
// 해당하는 파일 포인터를 찾아 반환
struct file *process_get_file(int fd)
{
    struct thread *current_thread = thread_current();

    if (current_thread->fdt[fd]->fd_type == FD_TYPE_FILE &&
        current_thread->fdt[fd]->fd_ptr != NULL)
    {
        return current_thread->fdt[fd]->fd_ptr;
    }
    else
    {
        return NULL;
    }
}

// 현재 실행 중인 프로세스의 열린 파일 리스트에 파일 추가
int process_add_file(struct file *file)
{
    if (file == NULL)
    {
        return -1;
    }

    struct thread *current_thread = thread_current();

    current_thread->fdt[current_thread->next_fd] =
        malloc(sizeof(struct uni_file));

    current_thread->fdt[current_thread->next_fd]->fd_type = FD_TYPE_FILE;
    current_thread->fdt[current_thread->next_fd]->fd_ptr = file;

    return current_thread->next_fd++;
}