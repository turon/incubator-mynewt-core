#include <stddef.h>
#include <assert.h>
#include <string.h>
#include "ffs/ffs.h"
#include "ffs_priv.h"

/**
 * Reads a data block header from flash.
 *
 * @param out_disk_block        On success, the block header is writteh here.
 * @param area_idx              The index of the area to read from.
 * @param area_offset           The offset within the area to read from.
 *
 * @return                      0 on success; nonzero on failure.
 */
int
ffs_block_read_disk(struct ffs_disk_block *out_disk_block, uint8_t area_idx,
                    uint32_t area_offset)
{
    int rc;

    rc = ffs_flash_read(area_idx, area_offset, out_disk_block,
                        sizeof *out_disk_block);
    if (rc != 0) {
        return rc;
    }
    if (out_disk_block->fdb_magic != FFS_BLOCK_MAGIC) {
        return FFS_EUNEXP;
    }

    return 0;
}

/**
 * Writes the specified data block to a suitable location in flash.
 *
 * @param out_area_idx          On success, contains the index of the area
 *                                  written to.
 * @param out_area_offset       On success, contains the offset within the area
 *                                  written to.
 * @param disk_block            Points to the disk block to write.
 * @param data                  The contents of the data block.
 *
 * @return                      0 on success; nonzero on failure.
 */
int
ffs_block_write_disk(uint8_t *out_area_idx, uint32_t *out_area_offset,
                     const struct ffs_disk_block *disk_block,
                     const void *data)
{
    uint32_t offset;
    uint8_t area_idx;
    int rc;

    rc = ffs_misc_reserve_space(&area_idx, &offset,
                                sizeof *disk_block + disk_block->fdb_data_len);
    if (rc != 0) {
        return rc;
    }

    rc = ffs_flash_write(area_idx, offset, disk_block, sizeof *disk_block);
    if (rc != 0) {
        return rc;
    }

    if (disk_block->fdb_data_len > 0) {
        rc = ffs_flash_write(area_idx, offset + sizeof *disk_block,
                             data, disk_block->fdb_data_len);
        if (rc != 0) {
            return rc;
        }
    }

    *out_area_idx = area_idx;
    *out_area_offset = offset;

    return 0;
}

static void
ffs_block_from_disk_no_ptrs(struct ffs_block *out_block,
                            const struct ffs_disk_block *disk_block,
                            uint8_t area_idx, uint32_t area_offset)
{
    out_block->fb_id = disk_block->fdb_id;
    out_block->fb_seq = disk_block->fdb_seq;
    out_block->fb_flash_loc = ffs_flash_loc(area_idx, area_offset);
    out_block->fb_inode_entry = NULL;
    out_block->fb_prev = NULL;
    out_block->fb_data_len = disk_block->fdb_data_len;
}

static int
ffs_block_from_disk(struct ffs_block *out_block,
                    const struct ffs_disk_block *disk_block,
                    uint8_t area_idx, uint32_t area_offset)
{
    ffs_block_from_disk_no_ptrs(out_block, disk_block, area_idx, area_offset);

    out_block->fb_inode_entry = ffs_hash_find_inode(disk_block->fdb_inode_id);
    if (out_block->fb_inode_entry == NULL) {
        return FFS_ECORRUPT;
    }

    if (disk_block->fdb_prev_id != FFS_ID_NONE) {
        out_block->fb_prev = ffs_hash_find_block(disk_block->fdb_prev_id);
        if (out_block->fb_prev == NULL) {
            return FFS_ECORRUPT;
        }
    }

    return 0;
}

/**
 * Constructs a disk-representation of the specified data block.
 *
 * @param block                 The source block to convert.
 * @param out_disk_block        The disk block to write to.
 */
void
ffs_block_to_disk(const struct ffs_block *block,
                  struct ffs_disk_block *out_disk_block)
{
    assert(block->fb_inode_entry != NULL);

    out_disk_block->fdb_magic = FFS_BLOCK_MAGIC;
    out_disk_block->fdb_id = block->fb_id;
    out_disk_block->fdb_seq = block->fb_seq;
    out_disk_block->fdb_inode_id = block->fb_inode_entry->fi_hash_entry.fhe_id;
    if (block->fb_prev == NULL) {
        out_disk_block->fdb_prev_id = FFS_ID_NONE;
    } else {
        out_disk_block->fdb_prev_id = block->fb_prev->fhe_id;
    }
    out_disk_block->fdb_data_len = block->fb_data_len;
}

/**
 * Deletes the specified block entry from the ffs RAM representation.
 *
 * @param block_entry           The block entry to delete.
 *
 * @return                      0 on success; nonzero on failure.
 */
int
ffs_block_delete_from_ram(struct ffs_hash_entry *block_entry)
{
    struct ffs_block block;
    int rc;

    rc = ffs_block_from_hash_entry(&block, block_entry);
    if (rc != 0) {
        return rc;
    }

    assert(block.fb_inode_entry != NULL);
    if (block.fb_inode_entry->fi_last_block == block_entry) {
        block.fb_inode_entry->fi_last_block = block.fb_prev;
    }

    ffs_hash_remove(block_entry);
    ffs_hash_entry_free(block_entry);

    return 0;
}

/**
 * Constructs a full data block representation from the specified minimal
 * block entry.  However, the resultant block's pointers are set to null,
 * rather than populated via hash table lookups.  This behavior is useful when
 * the RAM representation has not been fully constructed yet.
 *
 * @param out_block             On success, this gets populated with the data
 *                                  block information.
 * @param block_entry           The source block entry to convert.
 *
 * @return                      0 on success; nonzero on failure.
 */
int
ffs_block_from_hash_entry_no_ptrs(struct ffs_block *out_block,
                                  const struct ffs_hash_entry *block_entry)
{
    struct ffs_disk_block disk_block;
    uint32_t area_offset;
    uint8_t area_idx;
    int rc;

    assert(ffs_hash_id_is_block(block_entry->fhe_id));

    ffs_flash_loc_expand(&area_idx, &area_offset, block_entry->fhe_flash_loc);
    rc = ffs_block_read_disk(&disk_block, area_idx, area_offset);
    if (rc != 0) {
        return rc;
    }

    ffs_block_from_disk_no_ptrs(out_block, &disk_block, area_idx, area_offset);

    return 0;
}

/**
 * Constructs a full data block representation from the specified minimal block
 * entry.  The resultant block's pointers are populated via hash table lookups.
 *
 * @param out_block             On success, this gets populated with the data
 *                                  block information.
 * @param block_entry           The source block entry to convert.
 *
 * @return                      0 on success; nonzero on failure.
 */
int
ffs_block_from_hash_entry(struct ffs_block *out_block,
                          const struct ffs_hash_entry *block_entry)
{
    struct ffs_disk_block disk_block;
    uint32_t area_offset;
    uint8_t area_idx;
    int rc;

    assert(ffs_hash_id_is_block(block_entry->fhe_id));

    ffs_flash_loc_expand(&area_idx, &area_offset, block_entry->fhe_flash_loc);
    rc = ffs_block_read_disk(&disk_block, area_idx, area_offset);
    if (rc != 0) {
        return rc;
    }

    rc = ffs_block_from_disk(out_block, &disk_block, area_idx, area_offset);
    if (rc != 0) {
        return rc;
    }

    return 0;
}
