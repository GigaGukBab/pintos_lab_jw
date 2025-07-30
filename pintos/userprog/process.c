#include "userprog/process.h"

#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "include/lib/string.h"
#include "include/threads/mmu.h"
#include "include/userprog/syscall.h"
#include "intrinsic.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/gdt.h"
#include "userprog/tss.h"
#ifdef VM
#include "vm/vm.h"
#endif

#define MAX_ARGS 32

static void process_cleanup(void);
static bool load(const char *file_name, struct intr_frame *if_, char **argv,
                 int argc);
static void initd(void *f_name);
static void __do_fork(void *);

/* General process initializer for initd and other process. */
static void process_init(void)
{
    struct thread *current = thread_current();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t process_create_initd(const char *file_name)
{
    char *fn_copy;
    char *unused_ptr;
    tid_t tid;

    /* Make a copy of FILE_NAME.
     * Otherwise there's a race between the caller and load(). */
    fn_copy = palloc_get_page(0);
    if (fn_copy == NULL) return TID_ERROR;
    strlcpy(fn_copy, file_name, PGSIZE);

    /* Create a new thread to execute FILE_NAME. */
    file_name = strtok_r(file_name, " ", &unused_ptr);
    memset(unused_ptr + 1, 0, (strlen(unused_ptr + 1)));
    tid = thread_create(file_name, PRI_DEFAULT, initd, fn_copy);
    if (tid == TID_ERROR) palloc_free_page(fn_copy);
    return tid;
}

/* A thread function that launches first user process. */
static void initd(void *f_name)
{
#ifdef VM
    supplemental_page_table_init(&thread_current()->spt);
#endif

    process_init();

    if (process_exec(f_name) < 0) PANIC("Fail to launch initd\n");
    NOT_REACHED();
}

/* initialize fd_table. reserves stdin, stdout */
void process_init_fdt(struct thread *t)
{
    t->fd_table[0] = malloc(sizeof(struct uni_file));
    t->fd_table[1] = malloc(sizeof(struct uni_file));

    t->fd_table[0]->type = FD_STDIN;
    t->fd_table[0]->ptr = NULL;

    t->fd_table[1]->type = FD_STDOUT;
    t->fd_table[1]->ptr = NULL;
}

/* 현재 프로세스를 `name`으로 복제합니다.
 * 복제된 프로세스의 스레드 ID를 반환하며,
 * 스레드 생성에 실패할 경우 TID_ERROR를 반환합니다. */
tid_t process_fork(const char *name, struct intr_frame *if_)
{
    struct thread *curr = thread_current();

    memcpy(&curr->parent_if, if_, sizeof(struct intr_frame));

    /* PID: 복제된 자식 프로세스의 TID */
    tid_t pid = thread_create(name, PRI_DEFAULT, __do_fork, curr);
    if (pid == TID_ERROR)
    {
        return TID_ERROR;
    }

    struct thread *child = process_get_child(pid);

    /* 부모 프로세스는 자식 프로세스의 fork 과정을 기다린다. */
    sema_down(&child->fork_sema);

    /* 자식 프로세스의 fork 과정이 어떤 결과로든 끝나면 이쪽이 실행된다.
     * 이때 어떤 동작이 실패하여 자식의 종료 코드가 -1이면 부모가 실행하는
     * process_fork 과정의 반환값은 TID_ERROR(-1)을 반환하도록 한다.  */
    if (child->exit_status == TID_ERROR)
    {
        return TID_ERROR;
    }

    return pid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool duplicate_pte(uint64_t *pte, void *va, void *aux)
{
    struct thread *current = thread_current();
    struct thread *parent = (struct thread *) aux;
    void *parent_page;
    void *newpage;
    bool writable;

    /* 1. TODO: If the parent_page is kernel page, then return immediately. */
    if (is_kernel_vaddr(va))
    {
        return true;
    }

    /* 2. Resolve VA from the parent's page map level 4. */
    parent_page = pml4_get_page(parent->pml4, va);
    if (parent_page == NULL)
    {
        return false;
    }

    /* 3. TODO: Allocate new PAL_USER page for the child and set result to
     *    TODO: NEWPAGE. */
    newpage = palloc_get_page(PAL_USER | PAL_ZERO);
    if (newpage == NULL)
    {
        return false;
    }

    /* 4. TODO: Duplicate parent's page to the new page and
     *    TODO: check whether parent's page is writable or not (set WRITABLE
     *    TODO: according to the result). */

    memcpy(newpage, parent_page, PGSIZE);

    writable = is_writable(pte);

    /* 5. Add new page to child's page table at address VA with WRITABLE
     *    permission. */
    if (!pml4_set_page(current->pml4, va, newpage, writable))
    {
        /* 6. TODO: if fail to insert page, do error handling. */
        palloc_free_page(newpage);
        return false;
    }

    return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void __do_fork(void *aux)
{
    /* intr frame for jump to userland context */
    struct intr_frame if_;
    struct thread *parent = (struct thread *) aux;
    struct thread *current = thread_current();
    struct intr_frame *parent_if = &parent->parent_if;
    bool succ = true;

    /* 1. Read the cpu context to local stack. */
    memcpy(&if_, parent_if, sizeof(struct intr_frame));
    if_.R.rax = 0; /* 자식 프로세스의 return 값은 항상 0이다. */

    /* 2. Duplicate PT */
    current->pml4 = pml4_create();
    if (current->pml4 == NULL) goto error;

    process_activate(current);
#ifdef VM
    supplemental_page_table_init(&current->spt);
    if (!supplemental_page_table_copy(&current->spt, &parent->spt)) goto error;
#else
    if (!pml4_for_each(parent->pml4, duplicate_pte, parent)) goto error;
#endif

    /* duplciate parent's fd to child.
     * uni_file이 NULL이 아닐때만 복사합니다.
     * fils_duplicate에 실패하면 fork가 실패합니다. */
    lock_acquire(&filesys_lock);
    for (int fd = 2; fd < FDTABLE_SIZE; fd++)
    {
        if (parent->fd_table[fd] != NULL)
        {
            struct file *file = parent->fd_table[fd]->ptr;
            if (file != NULL)
            {
                struct file *dup_file = file_duplicate(file);
                if (dup_file == NULL)
                {
                    succ = false;
                    break;
                }
                current->fd_table[fd] = malloc(sizeof(struct uni_file));
                current->fd_table[fd]->type = FD_FILE;
                current->fd_table[fd]->ptr = dup_file;
            }
        }
    }
    lock_release(&filesys_lock);
    current->next_fd = parent->next_fd;

    process_init();

    /* 부모 프로세스 -> 프로세스로 복제 과정을 성공적으로 마쳤다면, */
    if (succ)
    {
        /* 자식 프로세스는 부모 프로세스에게 성공적으로 복사 과정을 마쳤다는
         * 것을 알려주면 된다.*/
        sema_up(&current->fork_sema);
        /* Finally, switch to the newly created process. */
        do_iret(&if_);
    }
error:
    sema_up(&current->fork_sema);
    thread_exit();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int process_exec(void *f_name)
{
    char *fn_copy = f_name;
    char *argv[MAX_ARGS], *token, *save_ptr;
    int argc = 0;
    bool success;
    if (is_user_vaddr(f_name))
    {
        /* copy userland address to kernel area */
        fn_copy = palloc_get_page(PAL_ZERO);
        if (fn_copy == NULL)
        {
            return -1;
        }
        strlcpy(fn_copy, f_name, PGSIZE);
    }
    else
    {
        /* use straightforward if f_name is already kernel addr */
        fn_copy = f_name;
    }

    for (token = strtok_r(fn_copy, " ", &save_ptr); token != NULL;
         token = strtok_r(NULL, " ", &save_ptr))
    {
        argv[argc++] = token;
    }

    // argv[0]은 실행할 파일명, arg[1...]은 인자들
    // load(argv[0], &_if) 호출
    // setup_stack에서 argv 배열을 스택에 올림

    /* We cannot use the intr_frame in the thread structure.
     * This is because when current thread rescheduled,
     * it stores the execution information to the member. */
    struct intr_frame _if;
    _if.ds = _if.es = _if.ss = SEL_UDSEG;
    _if.cs = SEL_UCSEG;
    _if.eflags = FLAG_IF | FLAG_MBS;

    /* We first kill the current context */
    process_cleanup();

    /* And then load the binary */
    success = load(fn_copy, &_if, argv, argc);

    /* If load failed, quit. */
    palloc_free_page(fn_copy);
    if (!success)
    {
        return -1;
    }

    /* Start switched process. */
    do_iret(&_if);
    NOT_REACHED();
}

/* 해당 file descriptor 번호로 fdt에서 파일을 찾아 반환. */
struct uni_file *process_get_file(int fd)
{
    struct thread *curr = thread_current();

    if (curr->fd_table[fd] == NULL)
    {
        return NULL;
    }
    return curr->fd_table[fd];
}

/* 현재 실행 중인 프로세스의 열린 파일 리스트에 파일 추가
 * 파일 디스크립터 handle 번호 반환. */
int process_add_file(struct file *file)
{
    if (file == NULL)
    {
        return -1;
    }

    struct thread *curr = thread_current();

    curr->fd_table[curr->next_fd] = malloc(sizeof(struct uni_file));

    curr->fd_table[curr->next_fd]->type = FD_FILE;
    curr->fd_table[curr->next_fd]->ptr = file;

    return curr->next_fd++;
}

/* fork로부터 반환된 child의 tid로 현재 부모 프로세스의 자식 리스트에서
 * 해당 자식 프로세스가 등록되어있는지 확인한다.
 * 자식이 있으면 해당 자식 스레드 구조체를 반환하고, 없으면 NULL을 반환한다. */
struct thread *process_get_child(tid_t tid)
{
    struct thread *cur = thread_current();
    struct list *child_list = &cur->child_list;

    for (struct list_elem *e = list_begin(child_list);
         e != list_end(child_list); e = list_next(e))
    {
        struct thread *t = list_entry(e, struct thread, child_elem);
        if (t->tid == tid) return t;
    }
    return NULL;
}

/* 스레드 TID가 종료될 때까지 대기하고, 종료 상태(exit status)를 반환합니다.
 * 만약 커널에 의해 종료되었을 경우(즉 예외로 인해 강제 종료된 경우), -1을
 * 반환합니다. TID가 유효하지 않거나 호출 프로세스의 자식이 아니거나, 이미 해당
 * TID에 대해 process_wait()가 성공적으로 호출된 적이 있다면, 대기 없이 즉시
 * -1을 반환합니다. */
int process_wait(tid_t child_tid)
{
    struct thread *child = process_get_child(child_tid);

    /* 현재 부모의 직계 자식이 아닌 경우 즉시 -1 반환 */
    if (child == NULL)
    {
        return -1;
    }

    /* 이미 해당 TID에 대해 wait를 호출한 적이 있다면 즉시 -1 반환*/
    if (child->wait_called)
    {
        return -1;
    }
    child->wait_called = true;

    /* 자식 프로세스의 실행이 끝날 때 까지 대기 */
    sema_down(&child->wait_sema);

    /* 자식 프로세스가 wait sema에 대해 sema up을 하게 된다는 것은 자식의
     * 실행이 모두 끝났다는 것을 의미한다. 그러므로 부모의 자식 리스트에서 현재
     * 자식을 제거한다. */
    list_remove(&child->child_elem);

    /* 부모는 자식의 종료 과정을 확인하고 다시 exit_sema를 올려서 자식의 종료
     * 과정을 정상적으로 처리할 수 있도록 한다.*/
    sema_up(&child->exit_sema);

    /* 자식의 종료 코드를 반환한다. */
    return child->exit_status;
}

/* Exit the process. This function is called by thread_exit (). */
void process_exit(void)
{
    struct thread *curr = thread_current();

    /* 자식 프로세스가 종료되었음을 부모에게 알림.
     * 부모는 process_wait()에서 wait_sema를 기다리고 있다. */
    sema_up(&curr->wait_sema);

    /* close execution file here */
    if (curr->running_file != NULL)
    {
        lock_acquire(&filesys_lock);
        file_close(curr->running_file);
        curr->running_file = NULL;
        lock_release(&filesys_lock);
    }

    /* malloc으로 할당했던 모든 파일디스크립터 정리 */
    for (int i = 0; i < FDTABLE_SIZE; i++)
    {
        if (curr->fd_table[i] != NULL)
        {
            struct uni_file *uni_file = curr->fd_table[i];

            lock_acquire(&filesys_lock);
            file_close((struct file *) uni_file->ptr);
            lock_release(&filesys_lock);

            free(uni_file);
            curr->fd_table[i] = NULL;
        }
    }

    /* 이후 부모가 자식의 종료 상태를 알 수 있도록 exit_sema를 활용해
     * 자식은 다시 잠든다. */
    sema_down(&curr->exit_sema);

    /* 부모가 exit_sema로 깨웠을 때 마무리 작업을 통해 프로세스를 정리한다. */
    process_cleanup();
}

/* Free the current process's resources. */
static void process_cleanup(void)
{
    struct thread *curr = thread_current();

#ifdef VM
    supplemental_page_table_kill(&curr->spt);
#endif

    uint64_t *pml4;
    /* Destroy the current process's page directory and switch back
     * to the kernel-only page directory. */
    pml4 = curr->pml4;
    if (pml4 != NULL)
    {
        /* Correct ordering here is crucial.  We must set
         * cur->pagedir to NULL before switching page directories,
         * so that a timer interrupt can't switch back to the
         * process page directory.  We must activate the base page
         * directory before destroying the process's page
         * directory, or our active page directory will be one
         * that's been freed (and cleared). */
        curr->pml4 = NULL;
        pml4_activate(NULL);
        pml4_destroy(pml4);
    }
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void process_activate(struct thread *next)
{
    /* Activate thread's page tables. */
    pml4_activate(next->pml4);

    /* Set thread's kernel stack for use in processing interrupts. */
    tss_update(next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr
{
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct ELF64_PHDR
{
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack(struct intr_frame *if_, char **argv, int argc);
static bool validate_segment(const struct Phdr *, struct file *);
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
                         uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool load(const char *file_name, struct intr_frame *if_, char **argv,
                 int argc)
{
    struct thread *t = thread_current();
    struct ELF ehdr;
    struct file *file = NULL;
    off_t file_ofs;
    bool success = false;
    int i;
    char *unused_ptr;

    /* Allocate and activate page directory. */
    t->pml4 = pml4_create();
    if (t->pml4 == NULL) goto done;
    process_activate(thread_current());

    /* Open executable file. */

    lock_acquire(&filesys_lock);
    file = filesys_open(file_name);
    lock_release(&filesys_lock);

    if (file == NULL)
    {
        printf("load: %s: open failed\n", file_name);
        goto done;
    }

    lock_acquire(&filesys_lock);
    /* Read and verify executable header. */
    if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
        memcmp(ehdr.e_ident, "\177ELF\2\1\1", 7) || ehdr.e_type != 2 ||
        ehdr.e_machine != 0x3E  // amd64
        || ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Phdr) ||
        ehdr.e_phnum > 1024)
    {
        printf("load: %s: error loading executable\n", file_name);
        goto done;
    }
    lock_release(&filesys_lock);

    /* Read program headers. */
    lock_acquire(&filesys_lock);
    file_ofs = ehdr.e_phoff;
    for (i = 0; i < ehdr.e_phnum; i++)
    {
        struct Phdr phdr;

        if (file_ofs < 0 || file_ofs > file_length(file)) goto done;
        file_seek(file, file_ofs);

        if (file_read(file, &phdr, sizeof phdr) != sizeof phdr) goto done;
        file_ofs += sizeof phdr;
        switch (phdr.p_type)
        {
            case PT_NULL:
            case PT_NOTE:
            case PT_PHDR:
            case PT_STACK:
            default:
                /* Ignore this segment. */
                break;
            case PT_DYNAMIC:
            case PT_INTERP:
            case PT_SHLIB:
                goto done;
            case PT_LOAD:
                if (validate_segment(&phdr, file))
                {
                    bool writable = (phdr.p_flags & PF_W) != 0;
                    uint64_t file_page = phdr.p_offset & ~PGMASK;
                    uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
                    uint64_t page_offset = phdr.p_vaddr & PGMASK;
                    uint32_t read_bytes, zero_bytes;
                    if (phdr.p_filesz > 0)
                    {
                        /* Normal segment.
                         * Read initial part from disk and zero the rest. */
                        read_bytes = page_offset + phdr.p_filesz;
                        zero_bytes =
                            (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) -
                             read_bytes);
                    }
                    else
                    {
                        /* Entirely zero.
                         * Don't read anything from disk. */
                        read_bytes = 0;
                        zero_bytes =
                            ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
                    }
                    if (!load_segment(file, file_page, (void *) mem_page,
                                      read_bytes, zero_bytes, writable))
                        goto done;
                }
                else
                    goto done;
                break;
        }
    }
    lock_release(&filesys_lock);

    /* deny write on executing file */
    lock_acquire(&filesys_lock);
    file_deny_write(file);
    lock_release(&filesys_lock);

    /* Set up stack. */
    if (!setup_stack(if_, argv, argc)) goto done;

    /* Start address. */
    if_->rip = ehdr.e_entry;

    success = true;

done:
    /* We arrive here whether the load is successful or not. */
    /* save current execution file info on thread struct. */
    t->running_file = file;
    return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Phdr *phdr, struct file *file)
{
    /* p_offset and p_vaddr must have the same page offset. */
    if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) return false;

    /* p_offset must point within FILE. */
    if (phdr->p_offset > (uint64_t) file_length(file)) return false;

    /* p_memsz must be at least as big as p_filesz. */
    if (phdr->p_memsz < phdr->p_filesz) return false;

    /* The segment must not be empty. */
    if (phdr->p_memsz == 0) return false;

    /* The virtual memory region must both start and end within the
             user address space range. */
    if (!is_user_vaddr((void *) phdr->p_vaddr)) return false;
    if (!is_user_vaddr((void *) (phdr->p_vaddr + phdr->p_memsz))) return false;

    /* The region cannot "wrap around" across the kernel virtual
             address space. */
    if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr) return false;

    /* Disallow mapping page 0.
             Not only is it a bad idea to map page 0, but if we allowed
             it then user code that passed a null pointer to system calls
             could quite likely panic the kernel by way of null pointer
             assertions in memcpy(), etc. */
    if (phdr->p_vaddr < PGSIZE) return false;

    /* It's okay. */
    return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page(void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
                         uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable)
{
    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(ofs % PGSIZE == 0);

    file_seek(file, ofs);
    while (read_bytes > 0 || zero_bytes > 0)
    {
        /* Do calculate how to fill this page.
         * We will read PAGE_READ_BYTES bytes from FILE
         * and zero the final PAGE_ZERO_BYTES bytes. */
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        /* Get a page of memory. */
        uint8_t *kpage = palloc_get_page(PAL_USER);
        if (kpage == NULL) return false;

        /* Load this page. */
        if (file_read(file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
            palloc_free_page(kpage);
            return false;
        }
        memset(kpage + page_read_bytes, 0, page_zero_bytes);

        /* Add the page to the process's address space. */
        if (!install_page(upage, kpage, writable))
        {
            printf("fail\n");
            palloc_free_page(kpage);
            return false;
        }

        /* Advance. */
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
    }
    return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool setup_stack(struct intr_frame *if_, char **argv, int argc)
{
    uint8_t *kpage;
    bool success = false;

    kpage = palloc_get_page(PAL_USER | PAL_ZERO);
    if (kpage != NULL)
    {
        success = install_page(((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
        if (success)
        {
            void *curr_rsp = USER_STACK;
            int arg_size = 0;  // arg 총 크기 (0 초기화 필수)
            void *arg_address[MAX_ARGS];

            for (int i = argc - 1; i >= 0; i--)
            {
                curr_rsp -= strlen(argv[i]) + 1;
                arg_size += strlen(argv[i]) + 1;
                arg_address[i] = curr_rsp;
                memcpy(curr_rsp, argv[i], strlen(argv[i]) + 1);
            }

            // 8바이트 정렬 패딩
            if (((8 - (arg_size % 8)) % 8) != 0)
            {
                char align_padding = (8 - (arg_size % 8)) % 8;

                // arg_size += align_padding;
                curr_rsp -= align_padding;
                memset(curr_rsp, 0, sizeof(align_padding));
            }

            // 마지막 arg_address에 NULL 포인터 추가
            curr_rsp -= 8;
            memset(curr_rsp, 0x00, sizeof(void *));

            // arg_address를 가리키는 포인터 추가
            for (int i = argc - 1; i >= 0; i--)
            {
                curr_rsp -= 8;
                memcpy(curr_rsp, &arg_address[i], sizeof(void *));
            }

            // fake return address
            curr_rsp -= 8;
            memset(curr_rsp, 0, sizeof(void *));

            if_->rsp = curr_rsp;
            if_->R.rdi = argc;
            if_->R.rsi = curr_rsp + 8;
        }
        else
            palloc_free_page(kpage);
    }
    return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool install_page(void *upage, void *kpage, bool writable)
{
    struct thread *t = thread_current();

    /* Verify that there's not already a page at that virtual
     * address, then map our page there. */
    return (pml4_get_page(t->pml4, upage) == NULL &&
            pml4_set_page(t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool lazy_load_segment(struct page *page, void *aux)
{
    /* TODO: Load the segment from the file */
    /* TODO: This called when the first page fault occurs on address VA. */
    /* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
                         uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable)
{
    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(ofs % PGSIZE == 0);

    while (read_bytes > 0 || zero_bytes > 0)
    {
        /* Do calculate how to fill this page.
         * We will read PAGE_READ_BYTES bytes from FILE
         * and zero the final PAGE_ZERO_BYTES bytes. */
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        /* TODO: Set up aux to pass information to the lazy_load_segment. */
        void *aux = NULL;
        if (!vm_alloc_page_with_initializer(VM_ANON, upage, writable,
                                            lazy_load_segment, aux))
            return false;

        /* Advance. */
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
    }
    return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool setup_stack(struct intr_frame *if_)
{
    bool success = false;
    void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

    /* TODO: Map the stack on stack_bottom and claim the page immediately.
     * TODO: If success, set the rsp accordingly.
     * TODO: You should mark the page is stack. */
    /* TODO: Your code goes here */

    return success;
}
#endif /* VM */