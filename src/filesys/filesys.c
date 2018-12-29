#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/cache.h"
#include "threads/thread.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format)
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  cache_init ();
  free_map_init ();
  if (format)
    do_format ();
  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void)
{
  free_map_close ();
  cache_flush ();
}

/* Extracts a file name part from *SRCP into PART,
   and updates *SRCP so that the next call will return the next
   file name part.
   Returns 1 if successful, 0 at end of string, -1 for a too-long
   file name part. */
static int
get_next_part (char part[NAME_MAX+1], const char **srcp)
{
  const char *src = *srcp;
  char *dst = part;

  /* Skip leading slashes.
     If it's all slashes, we're done. */
  while (*src == '/')
    src++;
  if (*src == '\0')
    return 0;

  /* Copy up to NAME_MAX character from SRC to DST.
     Add null terminator. */
  while (*src != '/' && *src != '\0')
    {
      if (dst < part + NAME_MAX)
        *dst++ = *src;
      else
        return -1;
      src++;
    }
  *dst = '\0';

  /* Advance source pointer. */
  *srcp = src;
  return 1;
}

/* Resolves relative or absolute file NAME.
   Returns true if successful, false on failure.
   Stores the directory corresponding to the name into *DIRP,
   and the file name part into BASE_NAME. */
static bool
resolve_name_to_entry (const char *name,
                       struct dir **dirp, char base_name[NAME_MAX + 1])
{
  char part[NAME_MAX + 1];
  struct dir *dir = NULL;
  struct inode *inode;
  int split;
  //printf("resolve name %s\n",thread_current()->name);
  if ((split = get_next_part(part, &name)) < 1){
    if (split == 0){
      *dirp = dir_open_root();
      base_name[0] = '.';
      base_name[1] = '\0';
      return true;
    }
    return false;
  }
  if (!dir_lookup(thread_cwd(), part, &inode)){
    dir = dir_open_root();
    dir_lookup(dir, part, &inode);
  }
  else {
    dir = thread_cwd();
  }
  if (inode == NULL){
    dir_close(dir);
    return false;
  }
  while(true){
    if (inode_get_type(inode) == FILE_INODE) { //pointing to a file, check to see if end of name
      for (int i = 0; i < NAME_MAX + 1; i++)
        base_name[i] = part[i];
      if (get_next_part(part, &name) != 0){
        dir_close(dir);
        return false;
      }
      break;
    }
    split = get_next_part(part, &name);
    if (inode_get_type(inode) == DIR_INODE){
      dir_close(dir); //close previous directory
      dir = dir_open(inode); //open new directory
      if (split == 0){
        *dirp = dir;
        base_name[0] = '.';
        base_name[1] = '\0';
        return true;
      }
      if (!dir_lookup(dir, part, &inode)){ // Take next step
        //Have not reached end file, but no further directories with part name
        get_next_part(base_name, &name);
        return false;
      }
    }
    if (split < 1)
     break;
  }
  if (split == -1){ //if the split failed and did not end return false
    dir_close(dir);
    return false;
  }
  *dirp = dir;
  return true;
}

/* Returns true if name is a valid new directory or file
  */
static bool
new_file_path(const char *name, struct dir **dirp, char base_name[NAME_MAX + 1]){
  char part[NAME_MAX + 1];
  struct inode *inode;
  int split;
  char * tmp = name;
  if (get_next_part(part, &tmp) < 1) // too long or null
    return false;
  if (get_next_part(part, &tmp) == 0){ //Just a local file or directory
    for (int i = 0; i < NAME_MAX + 1; i++)
      base_name[i] = part[i];
    *dirp = thread_cwd();
    return true;
  }
  // Have to ensure it is a legit path up until last name
  // starting from cwd if relative or root if absolute
  get_next_part(part, &name);
  if (!dir_lookup(thread_cwd(), part, &inode)){
    *dirp = dir_open_root();
    if (!dir_lookup(*dirp, part, &inode))
      return false; //couldn't find it in either relative or absolute
  }
  else {
    *dirp = thread_cwd();
  }
  if (inode == NULL) //first part of name doesn't exist
    return false;
  while((split = get_next_part(part, &name)) > 0){
    dir_close(*dirp);
    *dirp = dir_open(inode);
    if (!dir_lookup(*dirp, part, &inode)){
      break;
    }
  }
  if (split == -1)
    return false;
  if (get_next_part(part, &name)!=0){
    return false;
  }
  for (int i = 0; i < NAME_MAX + 1; i++)
    base_name[i] = part[i];
  return true;
}


/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size, enum inode_type type)
{
  // Need to find open sector for inode
  block_sector_t inode_sector = 0;
  struct dir *dir = NULL;
  char base_name[NAME_MAX + 1];
  char * tmp = name;
  if (resolve_name_to_entry(tmp, &dir, base_name)) //File exists already
    return false;
  dir_close(dir);
  dir = NULL;
  if (!new_file_path(name,&dir,base_name))
    return false;
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector,initial_size, type)
                  && dir_add (dir, base_name, inode_sector));
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);
  if (type == DIR_INODE){
    struct dir *new_dir = dir_open( inode_open(inode_sector));
    struct inode *parent_inode = dir_get_inode(dir);
    block_sector_t parent_sector = inode_get_inumber(parent_inode);
    //add self to directory
    if (!dir_add(new_dir, ".", inode_sector)){
      dir_close(new_dir);
      return false;
    }
    //add parent to directory
    if (!dir_add(new_dir, "..", parent_sector)){
      dir_close(new_dir);
      return false;
    }
    //new_dir->pos += 2*sizeof(struct dir_entry);
    dir_close(new_dir);
  }
  dir_close (dir);
  //printf("success? %d\n",success);
  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;
  struct dir *dir = NULL;
  char base_name[NAME_MAX + 1];
  if (!resolve_name_to_entry(name, &dir, base_name)){
    return NULL;
  }
  //printf("name %s was resolved to have base_name %s\n", name, base_name);
  struct inode *inode = NULL;

  if (dir != NULL && !strcmp(base_name, ".")){
    return dir;
  }
  if (dir != NULL)
    dir_lookup (dir, &base_name, &inode);
  dir_close (dir);

  if (inode_get_type(inode) == DIR_INODE){
    return dir_open(inode);
  }

  return file_open (inode);
}

void
update_cwd(struct dir *dir)
{
  struct thread *cur = thread_current();
  cur->cwd = inode_get_inumber(dir->inode);
}

bool
filesys_chdir(const char* name)
{
  struct dir *dir = NULL;
  char base_name[NAME_MAX + 1];
  if (!resolve_name_to_entry(name, &dir, base_name))
    return NULL;
  struct inode *inode = NULL;

  if (dir != NULL && !strcmp(base_name, ".")){
    update_cwd(dir);
    return true;
  }

  return false;
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name)
{
  struct dir *dir = NULL;
  char base_name[NAME_MAX + 1];
  if (!new_file_path(name, &dir, base_name)){
    return false;
  }

  bool success = (dir != NULL && dir_remove (dir, base_name));
  dir_close (dir);

  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, ROOT_DIR_SECTOR))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
