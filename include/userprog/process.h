#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H
#define MAX_ARGS 128

#include "threads/thread.h"

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_ UNUSED);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);
static bool install_page (void *upage, void *kpage, bool writable);

struct segment {
    struct file *file;
    off_t ofs;
    size_t page_read_bytes;
};

#endif /* userprog/process.h */
