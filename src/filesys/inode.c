#include "filesys/inode.h"
#include <bitmap.h>
#include <list.h>
#include <debug.h>
#include <round.h>
#include <stdio.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"


/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define DIRECT_CNT 123
#define INDIRECT_CNT 1
#define DBL_INDIRECT_CNT 1
#define SECTOR_CNT (DIRECT_CNT + INDIRECT_CNT + DBL_INDIRECT_CNT)

#define PTRS_PER_SECTOR ((off_t) (BLOCK_SECTOR_SIZE / sizeof (block_sector_t)))
#define INODE_SPAN ((DIRECT_CNT                                              \
                     + PTRS_PER_SECTOR * INDIRECT_CNT                        \
                     + PTRS_PER_SECTOR * PTRS_PER_SECTOR * DBL_INDIRECT_CNT) \
                    * BLOCK_SECTOR_SIZE)

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t sectors[SECTOR_CNT]; /* Sectors. */
    enum inode_type type;               /* FILE_INODE or DIR_NODE. */
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    struct lock lock;

    /* Denying writes. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
  };

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Controls access to open_inodes list. */
static struct lock open_inodes_lock;

static void deallocate_inode (const struct inode *);
static bool allocate_sectors(struct inode_disk *, off_t , off_t );

/* Initializes the inode module. */
void
inode_init (void)
{
  list_init (&open_inodes);
  lock_init (&open_inodes_lock);
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos)
{
  ASSERT (inode != NULL);
  off_t length = inode_length(inode);
  size_t sector_pos = pos / BLOCK_SECTOR_SIZE;
  size_t max_pos = bytes_to_sectors(length);
  struct cache_block * inode_cache = cache_read(inode->sector);
  struct inode_disk *id = inode_cache->data;
  if (sector_pos <= max_pos){
    //gets the position of the sector offset
    if (sector_pos < DIRECT_CNT){
      //Sector is a direct block
      //printf("byte to sector %d %d %d\n",pos,sector_pos,id->sectors[sector_pos]);
      return id->sectors[sector_pos];
    }
    if (sector_pos < (DIRECT_CNT + (PTRS_PER_SECTOR))){
      struct cache_block * indirect_block =  cache_read(id->sectors[DIRECT_CNT]);
      return indirect_block->data[(sector_pos - DIRECT_CNT)];
    }
    else {
      struct cache_block * dbl_indirect_block =  cache_read(id->sectors[DIRECT_CNT+1]);
      size_t dbl_offset = (sector_pos - DIRECT_CNT - PTRS_PER_SECTOR) / PTRS_PER_SECTOR;
      block_sector_t indirect_sector = dbl_indirect_block->data[dbl_offset];
      struct cache_block * indirect_block =  cache_read(indirect_sector);
      return indirect_block->data[(sector_pos - DIRECT_CNT - (dbl_offset*PTRS_PER_SECTOR))];
    }
  }
  else
    return -1;
}



/* Initializes an inode of the given TYPE, writes the new inode
   to sector SECTOR on the file system device, and returns the
   inode thus created.  Returns a null pointer if unsuccessful,
   in which case SECTOR is released in the free map. */
bool
inode_create (block_sector_t sector, off_t length, enum inode_type type)
{
  struct inode_disk *disk_inode = NULL;
  struct cache_block *b, *data;
  void *data_region;
  bool success = false;
  block_sector_t direct_sector;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);


  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      for (int i=0;i<SECTOR_CNT;i++)
        disk_inode->sectors[i] = -1;
      size_t num_init_sectors = bytes_to_sectors (length);
      //disk_inode->length = length;
      disk_inode->length = length;
      disk_inode->type   = type;
      disk_inode->magic  = INODE_MAGIC;
      success = allocate_sectors(disk_inode, 0, num_init_sectors);
      //create a sectors for the initial size of the file
      /*
      bool allocation = true;
      for (size_t i = 0; i < num_init_sectors; i++)
      {
        if (!free_map_allocate(1, &disk_inode->sectors[i])){
          allocation = false;
        }
      }
      if (allocation)
        {
          //b = cache_lock(sector);
          //zero out memory
          //data_region = cache_zero(b);
          //memcpy(data_region, disk_inode, BLOCK_SECTOR_SIZE);
          block_write(fs_device, sector, disk_inode);
          //cache_unlock(b);
          //printf("4\n");
          success = true;
          //printf("inode create was successful\n");
        }
      */
      block_write(fs_device, sector, disk_inode);
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e))
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector)
        {
          inode_reopen (inode);
          return inode;
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL){
    return NULL;
  }

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init(&inode->lock);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    {
      lock_acquire (&open_inodes_lock);
      inode->open_cnt++;
      lock_release (&open_inodes_lock);
    }
  return inode;
}

/* Returns the type of INODE. */
enum inode_type
inode_get_type (const struct inode *inode)
{
  struct cache_block *b = cache_read(inode->sector);
  struct inode_disk *id = (struct inode_disk *)(b->data);
  enum inode_type type = id->type;
  return type;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode)
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;
  cache_flush();
  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);

      /* Deallocate blocks if removed. */
      if (inode->removed)
        {
          deallocate_inode(inode);
        }

      free (inode);
    }
}

/* Deallocates SECTOR and anything it points to recursively.
   LEVEL is 2 if SECTOR is doubly indirect,
   or 1 if SECTOR is indirect,
   or 0 if SECTOR is a data sector. */
static void
deallocate_recursive (block_sector_t sector, int level)
{
  //this sector points to data, so we can just free this data
  if (level == 0)
  {
    free_map_release(sector, 1);
  } else {
    return;
  }
  //struct cache_block *b = cache_lock(sector);
  //struct inode_disk *id = (inode_disk *)cache_read(b);
  //cache_unlock(b);

  // cache_Read, deallocate_recursive, .....
}

/* Deallocates the blocks allocated for INODE. */
static void
deallocate_inode (const struct inode *inode)
{
  //printf("i am dealloating an inode\n");
  //struct cache_block *b = cache_lock(inode->sector);
  //struct inode_disk *id = (struct inode_disk *)cache_read(b);
  size_t num_sectors = bytes_to_sectors(inode_length(inode));
  struct cache_block *b = cache_read(inode->sector);
  struct inode_disk *id = (struct inode_disk *)(b->data);
  for (unsigned int i = 0; i < num_sectors; i++){
    deallocate_recursive(id->sectors[i], 0);
  }
  //free_map_release(inode->sector, 1);
  //deallocate_recursive(id->sectors[DIRECT_CNT], 1);
  //free doubly indirect
  //deallocate_recursive(id->sectors[DIRECT_CNT + INDIRECT_CNT], 2);

  //cache_unlock(b);
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode)
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Translates SECTOR_IDX into a sequence of block indexes in
   OFFSETS and sets *OFFSET_CNT to the number of offsets. */
static void
calculate_indices (off_t sector_idx, size_t offsets[], size_t *offset_cnt)
{
  /* Handle direct blocks. */
  if (sector_idx < DIRECT_CNT)
  {
    offsets[0] = sector_idx;
    *offset_cnt = 1;
  }

  /* Handle indirect blocks. */
  else if (sector_idx >= DIRECT_CNT && sector_idx < DIRECT_CNT + PTRS_PER_SECTOR * INDIRECT_CNT)
  {
    offsets[0] = DIRECT_CNT;
    offsets[1] = sector_idx - DIRECT_CNT;
    *offset_cnt = 2;
  }

  /* Handle doubly indirect blocks. */
  else
  {
    offsets[0] = DIRECT_CNT + INDIRECT_CNT;
    offsets[1] = (sector_idx - (DIRECT_CNT + PTRS_PER_SECTOR * INDIRECT_CNT)) / PTRS_PER_SECTOR;
    offsets[2] = (sector_idx - (DIRECT_CNT + PTRS_PER_SECTOR * INDIRECT_CNT)) % PTRS_PER_SECTOR;
    *offset_cnt = 3;
  }
}

/* if level is 0, allocate a direct block within an indirect block at sector_idx
   if level is 1, allocate an indirect block within a doubly indirect block at sector_idx*/
static bool
allocate_indirect(const uint8_t *non_direct, off_t sector_idx, int level)
{
  if (level == 0){ //non direct is an indirect block
    if (!free_map_allocate(1, &non_direct[sector_idx]))
      return false;
    //get the block and clear out information
    struct cache_block *b = cache_read(non_direct[sector_idx]);
    cache_zero(b);
  }
  if (level == 1){ //non direct is a doubly indirect block
    if (!free_map_allocate(1, &non_direct[sector_idx]))
      return false;
  }
}
/* allocates num_sectors for a doubly indirect block, indiecrt must already be allocated */
static bool
allocate_dbl_indirect(const uint8_t *dbl_block, off_t *sector_idx_, off_t *num_sectors_)
{
  off_t sector_idx = *sector_idx_;
  off_t num_sectors = *num_sectors_;
  //const uint8_t *dbl_block = cache_read(id->sectors[DIRECT_CNT + INDIRECT_CNT])->data;
  while(num_sectors > 0){
    //get position within the doubly indirect block
    off_t dbind_pos = (sector_idx - (DIRECT_CNT + PTRS_PER_SECTOR * INDIRECT_CNT)) / PTRS_PER_SECTOR;
    //get position wihtin the indirect block
    off_t indirect_pos = (sector_idx - (DIRECT_CNT + PTRS_PER_SECTOR * INDIRECT_CNT)) % PTRS_PER_SECTOR;
    if (indirect_pos == 0) //this indicates that this is the first entry within the indirect block, so it must not be allocated yet
      if (!allocate_indirect(dbl_block, dbind_pos, 1)) //allocate an indirect block
        return false;
    //alloc sect is the minimum between num_sectors and the number of sectors that can be held by an indirect block
    //makes sure we don't try to allocate more than a single indirect can hold
    off_t alloc_sect = num_sectors < PTRS_PER_SECTOR * INDIRECT_CNT ? num_sectors : PTRS_PER_SECTOR * INDIRECT_CNT;
    const uint8_t ind_block = cache_read(dbl_block[dbind_pos])->data;
    //allocate as many block pointers in ind_block as needed or possible
    for (off_t i = 0; i <= alloc_sect; i++)
      if (!allocate_indirect(ind_block, i, 0))
        return false;
    //decrement num_sectors by the amount of sectors allocated
    num_sectors -= alloc_sect;
    //increment sector_idx by the number of sectors allocated
    sector_idx += alloc_sect;
  }
  *num_sectors_ = num_sectors;
  *sector_idx_ = sector_idx;
  return true;
}

static bool
allocate_sectors(struct inode_disk *id, off_t existing_sectors, off_t new_sectors)
{
  struct cache_block *b;
  off_t sector_idx;
  const uint8_t *non_direct; //data in cache is an array of fixed size uint8_t

  while (new_sectors>0){
    if (existing_sectors < DIRECT_CNT){
      if (!free_map_allocate(1, &id->sectors[existing_sectors]))
        return false;
    }
    else if (existing_sectors >= DIRECT_CNT && existing_sectors < DIRECT_CNT + (PTRS_PER_SECTOR * INDIRECT_CNT)){
      if ((sector_idx = existing_sectors - DIRECT_CNT) == 0){ //then this is the first block that could not be held by a direct slot
        // Allocate the indirect block
        if (!free_map_allocate(1, &id->sectors[DIRECT_CNT]))
          return false;
      }
      b = cache_read(id->sectors[DIRECT_CNT]);
      non_direct = b->data;
      if (!free_map_allocate(1, &non_direct[sector_idx]))
        return false;
    }
    else {
      if (existing_sectors - (DIRECT_CNT + (PTRS_PER_SECTOR * INDIRECT_CNT)) == 0){ //this is the first block that couldn't be held by either direct or indirect
        if (!free_map_allocate(1, &id->sectors[DIRECT_CNT + INDIRECT_CNT]))
          return false;
      }
      non_direct = cache_read(id->sectors[DIRECT_CNT + INDIRECT_CNT])->data;
      //allocates when number of blocks has gotten too large for indirect and direct, updates exisiting_sectors and new_sectors
      if (!allocate_dbl_indirect(non_direct, &existing_sectors, &new_sectors))
        return false;
      return true;
    }
    existing_sectors++;
    new_sectors--;
  }
  return true;
}
/*
static bool
allocate_sectors(struct inode_disk * id, off_t existing_sectors, off_t new_sectors) {
  //Consider case where making a file bigger than 8MB
  struct cache_block *b;
  struct inode_disk *indirect;
  //Go through and make sectors
  printf("Allocating sectors %d %d\n",new_sectors, existing_sectors);
  while (new_sectors>0){
    if (existing_sectors < DIRECT_CNT){
      if (!free_map_allocate(1, &id->sectors[existing_sectors]))
        return false;
    }
    else if (existing_sectors >= DIRECT_CNT && existing_sectors < DIRECT_CNT + PTRS_PER_SECTOR * INDIRECT_CNT){
      if (id->sectors[DIRECT_CNT] == -1){
        // Allocate the indirect block
        if (!free_map_allocate(1, &id->sectors[DIRECT_CNT]))
          return false;
      }
      b = cache_read(id->sectors[DIRECT_CNT]);
      indirect = (struct inode_disk *)(b->data);
      if (!free_map_allocate(1, &indirect->sectors[existing_sectors-DIRECT_CNT]))
        return false;
    }
    else{
      if (id->sectors[DIRECT_CNT+1] == -1){
        // Allocate the Double indirect block
        if (!free_map_allocate(1, &id->sectors[DIRECT_CNT+1]))
          return false;
        b = cache_read(id->sectors[DIRECT_CNT+1]);
        for(int i=0;i<PTRS_PER_SECTOR;i++)
          b->data[i] = -1;
      }
      b = cache_read(id->sectors[DIRECT_CNT+1]);
      off_t indirect_block = (existing_sectors - (DIRECT_CNT + PTRS_PER_SECTOR * INDIRECT_CNT)) / PTRS_PER_SECTOR;
      if (b->data[indirect_block] == -1){
        // Allocate the indirect block
        if (!free_map_allocate(1, &b->data[indirect_block]))
          return false;
      }
      b = cache_read(b->data[indirect_block]);
      if (!free_map_allocate(1, &b->data[(existing_sectors - (DIRECT_CNT + PTRS_PER_SECTOR * INDIRECT_CNT)) % PTRS_PER_SECTOR]))
        return false;
    }
    existing_sectors++;
    new_sectors--;
  }
}
*/
/* Retrieves the data block for the given byte OFFSET in INODE,
   setting *DATA_BLOCK to the block.
   Returns true if successful, false on failure.
   If ALLOCATE is false, then missing blocks will be successful
   with *DATA_BLOCk set to a null pointer.
   If ALLOCATE is true, then missing blocks will be allocated.
   The block returned will be locked, normally non-exclusively,
   but a newly allocated block will have an exclusive lock. */
static bool
get_data_block (struct inode *inode, off_t offset, bool allocate,
                struct cache_block **data_block)
{
  struct cache_block *b = cache_read(inode->sector);
  struct inode_disk *id = (struct inode_disk *)(b->data);
  // 1 because we treated index as a zero indexed array
  off_t sector_idx = offset / BLOCK_SECTOR_SIZE;
  // existing sectors IS number of existing sectors
  off_t existing_sectors = bytes_to_sectors(id->length);
  off_t new_sectors = sector_idx - existing_sectors + 1;
  //printf("sector_idx: %d, exisiting_sectors: %d, offset: %d\n", sector_idx, existing_sectors, offset);
  if (new_sectors > 0){
    if (!allocate){
      *data_block = NULL;
      return true;
    }
    else{
      if (!allocate_sectors(id,existing_sectors,new_sectors))
        return false;
      id->length = offset;
    }
  }
  size_t offsets[3];
  size_t offset_cnt;
  calculate_indices(sector_idx, offsets, &offset_cnt);
  //for now only deal with direct blocks
  if (offset_cnt == 1)
    *data_block = cache_read(id->sectors[offsets[0]]);
  if (offset_cnt == 2){
    *data_block = cache_read(id->sectors[offsets[0]]);
    *data_block = cache_read((*data_block)->data[offsets[1]]);
  }
  if (offset_cnt == 3){
    *data_block = cache_read(id->sectors[offsets[0]]);
    *data_block = cache_read((*data_block)->data[offsets[1]]);
    *data_block = cache_read((*data_block)->data[offsets[2]]);
  }
  return true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset)
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  struct cache_block * read_block;
  int i;
  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */
      //block_sector_t sector_idx = byte_to_sector(inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;


      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      //int min_left = inode_left < sector_left ? inode_left : sector_left;
      int min_left = sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0 || !get_data_block (inode, offset, false, &read_block))
        break;

      //read_block = cache_read(sector_idx);
      //remeber that allocate is false, so *read_block will always be NULL
      //in the even of read that is past the point of the file
      //we always just have them read 0 into the buffer, check the provided inode_read_at
      //it displays this behavior
      if (read_block == NULL)
        memset (buffer + bytes_read, 0, chunk_size);
      else
      {
        i = sector_ofs;
        for (int n = chunk_size; n>0; n--)
        {
          //printf("read %c\n",read_block->data[i]);
          buffer[bytes_read] = read_block->data[i];
          bytes_read++;
          i++;
        }
      }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
    }
  return bytes_read;
}

/* Extends INODE to be at least LENGTH bytes long. */
static void
extend_file (struct inode *inode, off_t length)
{
  struct cache_block * inode_cache = cache_read(inode->sector);
  struct inode_disk *id = inode_cache->data;
  if (length > id->length)
    id->length = length;
  //printf("inode length %d\n",id->length);
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset)
{
  off_t bytes_written = 0;
  const uint8_t *buffer = buffer_;
  struct cache_block * write_block;
  int i;

  if (inode->deny_write_cnt)
    return 0;
  //printf("writing %s\n",buffer);

  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */
      //block_sector_t sector_idx = byte_to_sector(inode, offset);
      //printf("sector is %d\n", sector_idx);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      //int min_left = inode_left < sector_left ? inode_left : sector_left;
      int min_left = sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0 || !get_data_block (inode, offset, true, &write_block))
        break;

      i = sector_ofs;
      for (int n = chunk_size; n>0; n--)
      {
        write_block->data[i] = buffer[bytes_written];
        bytes_written++;
        i++;
      }
      write_block->dirty=true;

      //printf("\t%s\n",write_block->data );

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
    }
  //printf("offset %d bytes written %d\n",offset,bytes_written);
  extend_file(inode, offset);
  return bytes_written;
}
/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode)
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode)
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  struct cache_block * inode_cache = cache_read(inode->sector);
  struct inode_disk *id = inode_cache->data;
  return id->length;
}

/* Returns the number of openers. */
int
inode_open_cnt (const struct inode *inode)
{
  int open_cnt;

  lock_acquire (&open_inodes_lock);
  open_cnt = inode->open_cnt;
  lock_release (&open_inodes_lock);

  return open_cnt;
}


/* Locks INODE. */
void
inode_lock (struct inode *inode)
{
  lock_acquire (&inode->lock);
}

/* Releases INODE's lock. */
void
inode_unlock (struct inode *inode)
{
  lock_release (&inode->lock);
}
