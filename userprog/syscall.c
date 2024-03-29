#include <debug.h>
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "vm/vm.h"

#define MAX_FILE 128

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

struct file *get_file_from_fd_table (int fd);

void halt(void);
void exit (int status);
tid_t fork (const char *thread_name, struct intr_frame *f);
int exec (const char *file);
int wait (tid_t tid);
bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
int open (const char *file);
int filesize (int fd);
int read (int fd, void *buffer, unsigned length);
int write (int fd, const void *buffer, unsigned length);
void seek (int fd, unsigned position);
unsigned tell (int fd);
void close (int fd);

/* project 3 */
void *mmap(void *addr, size_t length, int writable, int fd, off_t offset);
void munmap(void *addr);


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

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

	lock_init(&file_lock);
}

/* Project2  */
// void check_address(void *addr) {
// 	struct thread *t = thread_current();
// 	if (!is_user_vaddr(addr) || addr == NULL || pml4_get_page(t->pml4 , addr) == NULL) {
// 		exit(-1);
// 	}
// }

/* Project3  */
// void * check_address(void * addr) {
// 	if (!is_user_vaddr(addr) || addr == NULL) {
// 		exit(-1);
// 	}
// }

struct page *check_address(void *addr) {
	if (is_kernel_vaddr(addr) || addr == NULL)
		exit(-1);

	return spt_find_page(&thread_current()->spt, addr);
}

void validate_buffer(void *buffer, size_t size, bool to_write) {

    if (buffer == NULL)
        exit(-1);

    if (buffer <= USER_STACK && buffer >= thread_current()->rsp_stack)
        return;

    void *start_addr = pg_round_down(buffer);
    void *end_addr = pg_round_down(buffer + size);

    ASSERT(start_addr <= end_addr);
    for (void *addr = end_addr; addr >= start_addr; addr -= PGSIZE) {
        // printf("addr: %p\n", addr);
        struct page *pg = check_address(addr);
        if (pg == NULL) {
            exit(-1);
        }
        
        if (pg->writable == false && to_write == true) {
            exit(-1);
        }
    }
}

int add_file_to_fd_table (struct file *file) {
	struct thread *t = thread_current();
	struct file **fdt = t->fd_table;
	int fd = t->fd_idx;
	
	while (t->fd_table[fd] != NULL) {
		if (fd >= FDCOUNT_LIMIT) {
			t->fd_idx = FDCOUNT_LIMIT;
			return -1;
		}
		fd++;
	}

	t->fd_idx = fd;
	fdt[fd] = file;
	return fd;
}

struct file *get_file_from_fd_table (int fd) {
	struct thread *t = thread_current();
	if (fd < 0 || fd >= MAX_FILE) {
		return NULL;
	}
	return t->fd_table[fd];
}

void halt(void) {
	power_off();
}

void exit (int status) {
	thread_current()->exit_status = status;
	printf("%s: exit(%d)\n", thread_name(), thread_current()->exit_status);
	thread_exit();
}

tid_t fork (const char *thread_name, struct intr_frame *f) {
	check_address(thread_name);
	return process_fork(thread_name, f);
}

int exec (const char *file) {
	check_address(file);
	if (process_exec((void *) file) < 0) {
		exit(-1);
	}
}

int wait (tid_t tid) {
	return process_wait (tid);
}

bool create (const char *file, unsigned initial_size) {
	check_address(file);
	lock_acquire(&file_lock);
	bool success = filesys_create(file, initial_size);
	lock_release(&file_lock);
	return success;
}

bool remove (const char *file) {
	lock_acquire(&file_lock);
	check_address(file);
	bool success = filesys_remove(file);
	lock_release(&file_lock);
	return success;
}

int open (const char *file) {
	check_address(file);
	lock_acquire(&file_lock);
	struct file *file_info = filesys_open(file);
	lock_release(&file_lock);

	if (file_info == NULL) {
		return -1;
	}
	int fd = add_file_to_fd_table(file_info);
	if (fd == -1) {
		file_close(file_info);
	}
	return fd;
}

int filesize (int fd) {
	return file_length(get_file_from_fd_table(fd));
}

int read(int fd, void *buffer, unsigned length) {
    validate_buffer(buffer, length, true);
    int bytesRead = 0;
    if (fd == 0) {
        for (int i = 0; i < length; i++) {
            char c = input_getc();
            ((char *)buffer)[i] = c;
            bytesRead++;

            if (c == '\n') break;
        }
    } else if (fd == 1) {
        return -1;
    } else {
        struct file *f = get_file_from_fd_table(fd);
        if (f == NULL) {
            return -1;
        }
        lock_acquire(&file_lock);
        bytesRead = file_read(f, buffer, length);
        lock_release(&file_lock);
    }
    return bytesRead;
}

int write (int fd, const void *buffer, unsigned length) {
	// check_address(buffer);
	validate_buffer(buffer, length, false);
	int bytesRead = 0;

	if (fd == 0) {
		return -1;
	} 
	else if (fd == 1) {
		putbuf(buffer, length);
		return length;
	} 
	else {
		struct file *f = get_file_from_fd_table(fd);
		if (f == NULL) {
			return -1;
		}
		lock_acquire(&file_lock);
		bytesRead = file_write(f, buffer, length);
		lock_release(&file_lock);
	}
	return bytesRead;
}

void seek (int fd, unsigned position) {
	struct file *f = get_file_from_fd_table(fd);
	if (f == NULL) {
		return;
	}
	file_seek(f, position);
}

unsigned tell (int fd) {
	struct file *f = get_file_from_fd_table(fd);
	if (f == NULL) {
		return -1;
	}
	return file_tell(f);
}

void close (int fd) {
	struct thread *t = thread_current();
	struct file **fdt = t->fd_table;
	if (fd < 0 || fd >= MAX_FILE) {
		return;
	}
	if (fdt[fd] == NULL) {
		return;
	}
	file_close(fdt[fd]);
	fdt[fd] = NULL;
}

void *mmap(void *addr, size_t length, int writable, int fd, off_t offset)
{
    if (!addr || addr != pg_round_down(addr))
        return NULL;

    if (offset != pg_round_down(offset))
        return NULL;

    if (!is_user_vaddr(addr) || !is_user_vaddr(addr + length))
        return NULL;

    if (spt_find_page(&thread_current()->spt, addr))
        return NULL;

    struct file *f = process_get_file(fd);

    if (f == NULL)
        return NULL;

    if (file_length(f) == 0 || (int)length <= 0)
        return NULL;

    return do_mmap(addr, length, writable, f, offset); 
}

void munmap(void *addr) {
	do_munmap(addr);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
	#ifdef VM
		thread_current()->rsp_stack = f->rsp;
	#endif
	switch (f->R.rax) {
		case SYS_HALT:
			halt();
			break;
		case SYS_EXIT:
			exit(f->R.rdi);
			break;
		case SYS_FORK:
			lock_acquire(&file_lock);
			f->R.rax = fork(f->R.rdi, f);
			lock_release(&file_lock);
			break;
		case SYS_EXEC:
			if (exec(f->R.rdi) < 0) {
				exit(-1);
			}
			break;
		case SYS_WAIT:
			f->R.rax = wait(f->R.rdi);
			break;
		case SYS_CREATE:
			f->R.rax = create(f->R.rdi, f->R.rsi);
			break;
		case SYS_REMOVE:
			f->R.rax = remove(f->R.rdi);
			break;
		case SYS_OPEN:
			f->R.rax = open(f->R.rdi);
			break;
		case SYS_FILESIZE:
			f->R.rax = filesize(f->R.rdi);
			break;
		case SYS_READ:
			f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		case SYS_WRITE:
			f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		case SYS_SEEK:
			seek(f->R.rdi, f->R.rsi);
			break;
		case SYS_TELL:
			f->R.rax = tell(f->R.rdi);
			break;
		case SYS_CLOSE:
			close(f->R.rdi);
			break;
		case SYS_MMAP:
			f->R.rax = mmap(f->R.rdi, f->R.rsi, f->R.rdx, f->R.r10, f->R.r8);
			break;
		case SYS_MUNMAP:
			munmap(f->R.rdi);
			break;
		default:
			exit(-1);
	}
}