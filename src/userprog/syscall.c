#include "userprog/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "lib/string.h"

static void syscall_handler (struct intr_frame *);
static inline bool get_user (uint8_t *dst, const uint8_t *usrc);
static inline bool put_user (uint8_t *udst, uint8_t byte);
static void copy_in (void *dst_, const void *usrc_, size_t size);
static char *copy_in_string (const char *us);

static void sys_halt(void);
static tid_t sys_exec (const char *file);
static tid_t sys_wait(tid_t tid);
static bool sys_create (const char *file, unsigned initial_size);
static bool sys_remove (const char *file);
static int sys_open(const char *file);
static int sys_filesize(int fd);
static int sys_read (int fd, void *buffer, unsigned int size);
static int sys_write (int fd, void *ks, unsigned int size, void *us);
static void sys_seek(int fd, unsigned int new_pos);
static unsigned int sys_tell(int fd);
static void sys_close(int fd);

static bool sys_chdir(const char *dir);
static bool sys_mkdir(const char *dir);
static bool sys_readdir(int fd, char *name);
static bool sys_isdir(int fd);
static int sys_inumber(int fd);

static struct file_descriptor* find_fd(struct list * file_table, int fd);

void
syscall_init (void)
{
  lock_init(&file_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/* System call handler. */
static void
syscall_handler (struct intr_frame *f)
{
  char *ks;
  unsigned call_nr;
  int args[3];
  memset (args, 0, sizeof args);

  //stores value at address of esp in call number
  copy_in (&call_nr, f->esp, sizeof call_nr);
  //printf("syscall number is %d\n", call_nr);

  switch(call_nr) {
  	case SYS_HALT:
  		sys_halt();
  		break;
  	case SYS_EXIT:
  	  copy_in (args, (uint32_t *) f->esp + 1, sizeof *args);
  		sys_exit(args[0]);
  		break;
  	case SYS_EXEC:
      copy_in (args, (uint32_t *) f->esp + 1, sizeof *args);
      ks = copy_in_string((const char *)args[0]);
  		f->eax = sys_exec(ks);
  		palloc_free_page(ks);
  		break;
  	case SYS_WAIT:
  	  copy_in (args, (uint32_t *) f->esp + 1, sizeof *args);
  		f->eax = sys_wait(args[0]);
  		break;
  	case SYS_CREATE:
  		copy_in (args, (uint32_t *) f->esp + 1, sizeof *args * 2);
  		ks = copy_in_string((const char *)args[0]);
      f->eax = sys_create(ks, args[1]);
      palloc_free_page(ks);
      break;
    case SYS_REMOVE:
      copy_in (args, (uint32_t *) f->esp + 1, sizeof *args);
      ks = copy_in_string((const char *)args[0]);
      f->eax = sys_remove(ks);
      palloc_free_page(ks);
      break;
    case SYS_OPEN:
      copy_in (args, (uint32_t *) f->esp + 1, sizeof *args);
      ks = copy_in_string((const char *)args[0]);
        //include call to copy ufile into kpage with copy_in_string on args[0]
      f->eax = sys_open(ks);
      palloc_free_page(ks);
      break;
    case SYS_FILESIZE:
    	copy_in (args, (uint32_t *) f->esp + 1, sizeof *args);
    	f->eax = sys_filesize(args[0]);
    	break;
    case SYS_READ:
    	copy_in (args, (uint32_t *) f->esp + 1, (sizeof *args) * 3);
    	//include call to copy buffer into kpage with copy_in_string on args[1]
      f->eax = sys_read(args[0], (void *)args[1], args[2]);
      break;
    case SYS_WRITE:
    	copy_in (args, (uint32_t *) f->esp + 1, (sizeof *args) * 3);
    	//include call to copy buffer into kpage with copy_in_string on args[1]
      ks = copy_in_string((const char *)args[1]);
      f->eax = sys_write(args[0], ks, args[2], args[1]);
      palloc_free_page(ks);
      break;
    case SYS_SEEK:
    	copy_in (args, (uint32_t *) f->esp + 1, sizeof *args * 2);
      sys_seek(args[0], args[1]);
      break;
    case SYS_TELL:
    	copy_in (args, (uint32_t *) f->esp + 1, sizeof *args);
  		f->eax = sys_tell(args[0]);
  		break;
    case SYS_CLOSE:
    	copy_in (args, (uint32_t *) f->esp + 1, sizeof *args);
  		sys_close(args[0]);
  		break;
    case SYS_CHDIR:
      copy_in (args, (uint32_t *) f->esp + 1, sizeof *args);
      ks = copy_in_string((const char *)args[0]);
        //include call to copy ufile into kpage with copy_in_string on args[0]
      f->eax = sys_chdir(ks);
      palloc_free_page(ks);
      break;
    case SYS_MKDIR:
      copy_in (args, (uint32_t *) f->esp + 1, sizeof *args);
      ks = copy_in_string((const char *)args[0]);
        //include call to copy ufile into kpage with copy_in_string on args[0]
      f->eax = sys_mkdir(ks);
      palloc_free_page(ks);
      break;
    case SYS_READDIR:
      copy_in(args, (uint32_t *) f->esp + 1, (sizeof *args) * 2);
      ks = copy_in_string((const char *)args[1]);
      f->eax = sys_readdir(args[0], ks);
      palloc_free_page(ks);
      break;
    case SYS_ISDIR:
      copy_in (args, (uint32_t *) f->esp + 1, sizeof *args);
      f->eax = sys_isdir(args[0]);
      break;
    case SYS_INUMBER:
      copy_in (args, (uint32_t *) f->esp + 1, sizeof *args);
      f->eax = sys_inumber(args[0]);
      break;
  }
}

/*
static void
print_fd_table(struct thread *t)
{
  struct list_elem *e;
  struct list *fd_table = &t->file_table;
  struct file_descriptor *fd;
  for (e = list_begin(fd_table); e != list_end(fd_table); e = list_next(e))
  {
    fd = list_entry(e, struct file_descriptor, fd_elem);
    printf("current fd value is %d\n", fd->fd);
  }
}
*/

static bool
sys_chdir(const char *dir)
{
  lock_acquire(&file_lock);
  bool ans = filesys_chdir(dir);
  lock_release(&file_lock);
  return ans;
}

static bool
sys_mkdir(const char *dir)
{
  lock_acquire(&file_lock);
  bool ans = filesys_create(dir,BLOCK_SECTOR_SIZE,DIR_INODE);
  lock_release(&file_lock);
  return ans;
}

static bool
sys_readdir(int fd, char *name)
{
  struct file_descriptor *f = find_fd(&thread_current()->file_table,fd);
  if (f == NULL) return false;
  struct dir *dir = f->dir;
  if (dir == NULL) return false;
  struct inode *inode = dir_get_inode(dir);
  enum inode_type type = inode_get_type(inode);
  lock_acquire(&file_lock);
  bool ans = dir_readdir(dir, name);
  lock_release(&file_lock);
  return ans;
}

static bool
sys_isdir(int fd)
{
  struct file_descriptor *f = find_fd(&thread_current()->file_table,fd);
  if (f == NULL) return false;
  struct dir *dir = f->dir;
  struct inode *inode = file_get_inode(dir);
  enum inode_type type = inode_get_type(inode);
  if (type == DIR_INODE) return true;
  return false;
}

static int
sys_inumber(int fd)
{
  struct file_descriptor *f = find_fd(&thread_current()->file_table,fd);
  if (f == NULL) return -1;
  struct file *file = f->file;
  struct dir *dir = f->dir;
  if (dir == NULL && file == NULL) return false;
  struct inode *inode;
  if (dir == NULL){
    inode = file_get_inode(file);
    return (int)inode_get_inumber(inode);
  }
  if (file == NULL){
    inode = dir_get_inode(dir);
    return (int)inode_get_inumber(inode);
  }
  return -1;
}

static void
sys_halt(void)
{
	shutdown_power_off();
}

void
sys_exit(int status)
{
  struct thread *cur = thread_current();

  if (cur->exit_info != NULL){
    struct child *child_info = cur->exit_info;
    lock_acquire(&child_lock);
    child_info->exit_status = status;
    child_info->exited = true;
    if (child_info->parent_sema != NULL){
      sema_up(child_info->parent_sema);
      //printf("waking up parent\n");
    }
    lock_release(&child_lock);
  }
	printf("%s: exit(%d)\n", cur->name, status);
	thread_exit();
}

/* Exec system call. */
static tid_t
sys_exec (const char *file)
{
  tid_t new_proc;
  new_proc = process_execute(file);
  return new_proc;
}

static tid_t
sys_wait(tid_t tid)
{
  return process_wait(tid);
}

/* Create file system call. */
static bool
sys_create (const char *ufile, unsigned initial_size)
{
  lock_acquire(&file_lock);
  bool ok = filesys_create (ufile, initial_size, FILE_INODE);
  lock_release(&file_lock);
  return ok;
}

/* Deletes the file called file, does not affect any file descriptors*/
static bool
sys_remove (const char *file)
{
  lock_acquire(&file_lock);
	bool ok = filesys_remove(file);
  lock_release(&file_lock);
	return ok;
}

/* Open file by creating file descriptor and adding to process file table */
static int
sys_open(const char *file)
{
  //Create element for file table
  struct file_descriptor *fd = (struct file_descriptor *) malloc(sizeof(struct file_descriptor));
  struct thread * cur = thread_current();
  lock_acquire(&file_lock);
	struct file *f = filesys_open(file);
  lock_release(&file_lock);
  if (f == NULL){
    free(fd);
    return (-1);
  }
  struct inode *inode  = file_get_inode(f);
  if (inode_get_type(inode) == DIR_INODE){
    struct dir *dir = f;
    fd->dir = dir;
    fd->file = NULL;
  } else {
    fd->dir = NULL;
    fd->file = f;
  }
  fd->fd = cur->free_fd;
  cur->free_fd += 1;
  list_push_front(&cur->file_table,&fd->fd_elem);
  return fd->fd;
}

/* Lookup fd in file table and use to get file size */
static int
sys_filesize(int fd)
{
  struct file_descriptor *f = find_fd(&thread_current()->file_table,fd);
  if (f == NULL)
    return (-1);
  lock_acquire(&file_lock);
  int length = (int) file_length(f->file);
  lock_release(&file_lock);
	return length;
}

static bool
verify_buffer(void *buffer, unsigned int size)
{
  char *temp_buffer = buffer;

  for (; size > 0; size--, temp_buffer++){
    if (temp_buffer >= (uint8_t *) PHYS_BASE){
      sys_exit (-1);
    }
  }
}

static int
sys_read(int fd, void *buffer, unsigned int size)
{
  verify_buffer(buffer, size);

  struct file_descriptor *f;
  int retval = size;
  char *temp_buffer = (char *)buffer;

  if (fd != STDIN_FILENO)
  {
    f = find_fd(&thread_current()->file_table,fd);
    if (f == NULL) return -1;
    lock_acquire(&file_lock);
    retval = file_read(f->file, buffer, size);
    lock_release(&file_lock);
  }
  else //fd is STDIN_FILENO
  {
    for (int i = 0; i < size; i++)
    {
      temp_buffer[i] = input_getc();
    }
    return size;
  }
  return retval;
}

/* Write system call. */
/* check if fd is 1 and add eveyrhting to putbuf */
/* If not, look in file table and write to that file */
/* Return bytes actually writen */
static int
sys_write (int fd, void *ks, unsigned int size, void *us)
{
  struct file_descriptor *f;
  int retval = 0;
  if (fd == STDOUT_FILENO){
	  putbuf(ks, size);
    return size;
  }
  else if (fd == STDIN_FILENO)
  {
      return -1;
  }
  else
  {
    f = find_fd(&thread_current()->file_table,fd);
    if (f == NULL) return -1;
    if (f->file == NULL) return -1;
    lock_acquire(&file_lock);
    retval += file_write (f->file, (const void *)us, size);
    lock_release(&file_lock);
  }
  return retval;
}

static void
sys_seek(int fd, unsigned int new_pos)
{
  //search through fd list to change pos to new_pos
  struct file_descriptor *f = find_fd(&thread_current()->file_table,fd);
  if (f == NULL)
    return;
  lock_acquire(&file_lock);
	file_seek(f->file, new_pos);
  lock_release(&file_lock);
  return;
}

static unsigned int
sys_tell(int fd){
  //search through fd list to return the pos value in struct file_descriptor
  struct file_descriptor *f = find_fd(&thread_current()->file_table,fd);
  if (f == NULL)
    return 0;
  lock_acquire(&file_lock);
	unsigned int pos = (unsigned int) file_tell(f->file);
  lock_release(&file_lock);
  return pos;
}

static void
sys_close(int fd)
{
  //remove fd from the current threads file table
  struct file_descriptor *f = find_fd(&thread_current()->file_table,fd);
  if (f == NULL)
    return;
  lock_acquire(&file_lock);
  list_remove(&f->fd_elem);
  lock_release(&file_lock);
  free(f);
  return;
}

//Traverses file table and returns file descriptor with fd number
//Return NULL if not present
static struct file_descriptor *
find_fd(struct list * file_table, int fd){
  struct list_elem *e;
  struct file_descriptor *f;
  for (e = list_begin (file_table); e != list_end (file_table); e = list_next (e)){
    f = list_entry (e, struct file_descriptor, fd_elem);
    if (f->fd == fd)
      return f;
  }
  return NULL;
}

/* Copies a byte from user address USRC to kernel address DST.  USRC must
   be below PHYS_BASE.  Returns true if successful, false if a segfault
   occurred. Unlike the one posted on the p2 website, thsi one takes two
   arguments: dst, and usrc */

static inline bool
get_user (uint8_t *dst, const uint8_t *usrc)
{
  int eax;
  asm ("movl $1f, %%eax; movb %2, %%al; movb %%al, %0; 1:"
       : "=m" (*dst), "=&a" (eax) : "m" (*usrc));
  return eax != 0;
}


/* Writes BYTE to user address UDST.  UDST must be below PHYS_BASE.
   Returns true if successful, false if a segfault occurred. */

static inline bool
put_user (uint8_t *udst, uint8_t byte)
{
  int eax;
  asm ("movl $1f, %%eax; movb %b2, %0; 1:"
       : "=m" (*udst), "=&a" (eax) : "q" (byte));
  return eax != 0;
}



/* Copies SIZE bytes from user address USRC to kernel address DST.  Call
   thread_exit() if any of the user accesses are invalid. */

static void copy_in (void *dst_, const void *usrc_, size_t size) {

  uint8_t *dst = dst_;
  const uint8_t *usrc = usrc_;

  for (; size > 0; size--, dst++, usrc++){
    if (usrc >= (uint8_t *) PHYS_BASE || !get_user (dst, usrc)){
      sys_exit (-1);
    }
  }
}



/* Creates a copy of user string US in kernel memory and returns it as a
   page that must be freed with palloc_free_page().  Truncates the string
   at PGSIZE bytes in size.  Call thread_exit() if any of the user accesses
   are invalid. */
static char *
copy_in_string (const char *us)
{

  char *ks;

  ks = palloc_get_page (0);
  if (ks == NULL)
    sys_exit (-1);

  for (int i = 0; ; i++){
    if (us >= (uint8_t *) PHYS_BASE || !get_user (ks + i, us + i)){
      palloc_free_page(ks);
      sys_exit (-1);
    }
    if (ks[i] == '\0') break;
  }
  return ks;

  // don't forget to call palloc_free_page(..) when you're done
  // with this page, before you return to user from syscall
}
