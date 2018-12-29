#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);

/* Global lock used by calls to file system to protect synchronization */
struct lock file_lock;

void sys_exit(int status);
#endif /* userprog/syscall.h */
