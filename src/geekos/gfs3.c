/*
 * GeekOS file system
 * Copyright (c) 2008, David H. Hovemeyer <daveho@cs.umd.edu>, 
 * Neil Spring <nspring@cs.umd.edu>, Aaron Schulman <schulman@cs.umd.edu>
 *
 * All rights reserved.
 *
 * This code may not be resdistributed without the permission of the copyright holders.
 * Any student solutions using any of this code base constitute derviced work and may
 * not be redistributed in any form.  This includes (but is not limited to) posting on
 * public forums or web sites, providing copies to (past, present, or future) students
 * enrolled in similar operating systems courses the University of Maryland's CMSC412 course.
 */


#include <limits.h>
#include <geekos/errno.h>
#include <geekos/kassert.h>
#include <geekos/screen.h>
#include <geekos/malloc.h>
#include <geekos/string.h>
#include <geekos/bitset.h>
#include <geekos/synch.h>
#include <geekos/bufcache.h>
#include <geekos/gfs3.h>
#include <geekos/pfat.h>
#include <geekos/projects.h>

/* ----------------------------------------------------------------------
 * Private data and functions
 * ---------------------------------------------------------------------- */

#define MAX_NAME_LEN 256
#define GFS3_INODES_PER_BLOCK 16

struct gfs3_instance {
    struct FS_Buffer_Cache *cache;
    struct gfs3_superblock *sb;
    struct gfs3_inode *rootInode;
    struct Block_Device *dev;
    uchar_t *bitmap;
};

struct gfs3_file {
    struct gfs3_inode *inode;
    gfs3_inodenum inodenum;
};

void Get_Next_Name(char **path, char *name) {
    int length = 0;
    char *slashAddr = 0;
    char *trimmedPath = &(*path)[1];
    
    slashAddr = strchr(trimmedPath, '/');

    length = slashAddr == 0 ? strlen(trimmedPath) : slashAddr - trimmedPath;

    strncpy(name, trimmedPath, length);

    *path = &trimmedPath[length]; 
}

void *_Malloc(ulong_t size) {
    void *res = Malloc(size); 
    if (res == 0) Exit(ENOMEM);
    // memset(res, 0, size);
    return res;
}

struct gfs3_inode *getInode(struct gfs3_instance *instance, gfs3_inodenum inode_num) {
    struct FS_Buffer_Cache *cache = instance->cache;

    struct FS_Buffer *buffer = NULL;
    struct gfs3_inode *inode = NULL;
    int rc;

    // Locate the block containing the inode
    ulong_t inode_block = inode_num / GFS3_INODES_PER_BLOCK + instance->sb->block_with_inode_zero;
    // Print("inode_block: %d\n", inode_block);
    // Print("block_with_inode_zero: %d\n", instance->sb->block_with_inode_zero);
    ulong_t inode_offset = inode_num % GFS3_INODES_PER_BLOCK;

    // Get the FS buffer for the block

    // Print("inode_offset: %d\n", inode_offset);

    rc = Get_FS_Buffer(cache, inode_block, &buffer);
    if (rc != 0) {
        return NULL;
    }
    // void *buffer = _Malloc(SECTOR_SIZE);
    // Block_Read(instance->dev, inode_block, buffer);

    // Calculate the exact location of the inode in the block
    inode = _Malloc(sizeof(struct gfs3_inode));
    memcpy(inode, (char *)buffer->data + inode_offset * sizeof(struct gfs3_inode), sizeof(struct gfs3_inode));
    // inode = (struct gfs3_inode *)((char *)buffer->data + inode_offset * sizeof(struct gfs3_inode));
    Release_FS_Buffer(cache, buffer);
    return inode;
}

void writeInode(struct gfs3_instance *instance, gfs3_inodenum inode_num, struct gfs3_inode *inode) {
    struct FS_Buffer_Cache *cache = instance->cache;

    struct FS_Buffer *buffer = NULL;
    int rc;

    ulong_t inode_block = inode_num / GFS3_INODES_PER_BLOCK + instance->sb->block_with_inode_zero;
    ulong_t inode_offset = inode_num % GFS3_INODES_PER_BLOCK;

    rc = Get_FS_Buffer(cache, inode_block, &buffer);
    if (rc != 0) {
        return;
    }

    memcpy((char *)buffer->data + inode_offset * sizeof(struct gfs3_inode), inode, sizeof(struct gfs3_inode));
    Modify_FS_Buffer(cache, buffer);
    Release_FS_Buffer(cache, buffer);
}

int getFreeBlockNum(struct gfs3_instance *instance) {
    struct FS_Buffer_Cache *cache = instance->cache;

    // struct FS_Buffer *buffer = NULL;
    // int rc;

    // rc = Get_FS_Buffer(cache, 2, &buffer);
    // if (rc != 0) {
    //     return -1;
    // }
    

    int res = Find_First_Free_Bit(instance->bitmap, SECTOR_SIZE * 8);
    // Release_FS_Buffer(cache, buffer);
    return res;
}

int setBlockUsed(struct gfs3_instance *instance, ulong_t blockNum, bool used) {
    struct FS_Buffer_Cache *cache = instance->cache;
    struct FS_Buffer *buffer = NULL;
    int rc = Get_FS_Buffer(cache, 2, &buffer);
    if (rc != 0) {
        return -1;
    }

    if (used) Set_Bit(instance->bitmap, blockNum);
    else Clear_Bit(instance->bitmap, blockNum);
    buffer->data = instance->bitmap;
    Modify_FS_Buffer(cache, buffer);
    Release_FS_Buffer(cache, buffer);
}

int setBlocksUsed(struct gfs3_instance *instance, ulong_t blockNum, ulong_t numBlocks, bool used) {
    int i;
    for (i = 0; i < numBlocks; i++) {
        setBlockUsed(instance, blockNum + i, used);
    }
}

int findMaxBlocksToAllocate(struct gfs3_instance *instance, int upperLimit) {
    int l = 0, r = upperLimit;
    while (l < r) {
        int mid = (l + r) >> 1;
        if (Find_First_N_Free(instance->bitmap, mid, SECTOR_SIZE * 8) != -1) {
            l = mid + 1;
        } else {
            r = mid - 1;
        }
    }
    return r;
}

int copyBlocks(struct gfs3_instance *instance, ulong_t dstBlocks, ulong_t srcBlocks, ulong_t numBlocks) {
    struct FS_Buffer_Cache *cache = instance->cache;
    int rc;

    int i;
    for (i = 0; i < numBlocks; i++) {
        struct FS_Buffer *srcBuffer = NULL;
        struct FS_Buffer *dstBuffer = NULL;
        rc = Get_FS_Buffer(cache, srcBlocks + i, &srcBuffer);
        if (rc != 0) {
            return -1;
        }
        rc = Get_FS_Buffer(cache, dstBlocks + i, &dstBuffer);
        if (rc != 0) {
            return -1;
        }
        memcpy(dstBuffer->data, srcBuffer->data, SECTOR_SIZE);
        Modify_FS_Buffer(cache, dstBuffer);
        Release_FS_Buffer(cache, srcBuffer);
        Release_FS_Buffer(cache, dstBuffer);
    }
}

int allocateBlocks(struct gfs3_inode *inode, struct gfs3_instance *instance, ulong_t numBlocks) {
    ulong_t blocksAllocated = 0;
    struct gfs3_inode tempInode;
    int i;
    for (i = 0; i < GFS3_EXTENTS && blocksAllocated < numBlocks; i++) tempInode.extents[i].start_block = 0;
    for (i = 0; i < GFS3_EXTENTS && blocksAllocated < numBlocks; i++) {
        int blocksToAllocate = findMaxBlocksToAllocate(instance, numBlocks - blocksAllocated);
        int blockNum = Find_First_N_Free(instance->bitmap, blocksToAllocate, SECTOR_SIZE * 8);
        // if (inode->extents[i].start_block == 0) {
        //     inode->extents[i].start_block = blockNum;
        //     inode->extents[i].length_blocks = blocksToAllocate;
        //     setBlocksUsed(instance, blockNum, blocksToAllocate, 1);
        // } else {
        //     copyBlocks(instance, blockNum, inode->extents[i].start_block, inode->extents[i].length_blocks);
        //     setBlocksUsed(instance, blockNum, blocksToAllocate, 1);
        //     setBlocksUsed(instance, inode->extents[i].start_block, inode->extents[i].length_blocks, 0);
        //     inode->extents[i].start_block = blockNum;
        //     inode->extents[i].length_blocks = blocksToAllocate;
        // }
        tempInode.extents[i].start_block = blockNum;
        tempInode.extents[i].length_blocks = blocksToAllocate;
        setBlocksUsed(instance, blockNum, blocksToAllocate, 1);
        blocksAllocated += blocksToAllocate;
    }
    int newBlockNum = tempInode.extents[0].start_block;
    int newExtentNum;
    for(i = 0; i < GFS3_EXTENTS; i++) {
        if (inode->extents[i].start_block == 0) continue;
        int j;
        for (j = inode->extents[i].start_block; j < inode->extents[i].start_block + inode->extents[i].length_blocks; j++) {
            setBlockUsed(instance, j, 0);
            copyBlocks(instance, newBlockNum, j, 1);
            newBlockNum++;
            if (newBlockNum == tempInode.extents[newExtentNum].start_block + tempInode.extents[newExtentNum].length_blocks) {
                newExtentNum++;
                newBlockNum = tempInode.extents[newExtentNum].start_block;
            }
        }
    }

    for (i = 0; i < GFS3_EXTENTS; i++) {
        inode->extents[i].start_block = tempInode.extents[i].start_block;
        inode->extents[i].length_blocks = tempInode.extents[i].length_blocks;
    }

    if (blocksAllocated < numBlocks) {
        return -1;
    }
    return 0;
}

int writeToInode(struct gfs3_inode *inode, void *buf, ulong_t numBytes, struct gfs3_instance *instance, ulong_t startPos, bool overwrite) {
    // Print("writeToInode\n");
    // Print("numBytes: %d\n", numBytes);
    // Print("%d ", ((char *)buf)[0]);
    // Print("n ");
    int frontBlockNum = (startPos / SECTOR_SIZE);
    ulong_t bytesWrite = 0;
    int i;
    int offset = startPos % SECTOR_SIZE;

    // if (((char *)buf)[0] == 41) while(1);
    // Print("%d ", ((char *)buf)[0]);
    for (i = 0; i < GFS3_EXTENTS && bytesWrite < numBytes; i++) {
        // if (i == 0) Print("start_block: %d\n", inode->extents[i].start_block);
        // if (i == 0) Print("length_blocks: %d\n", inode->extents[i].length_blocks);
        // if (!overwrite) {
        //     frontBlockNum = inode->extents[i].length_blocks;
        // }

        if (inode->extents[i].start_block == 0) {
            int blockNum = getFreeBlockNum(instance);
            // Print("f: %d ", blockNum);
            // Print("i: %d\n", i);
            // Print("blockNum: %d\n", blockNum);
            // Print("blockNum: %d\n", blockNum);
            if (blockNum == -1) {
                return bytesWrite;
            }
            setBlockUsed(instance, blockNum, 1);
            inode->extents[i].start_block = blockNum;
            inode->extents[i].length_blocks = 1;
            Print("add new extent: %d\n", i);
        }

        // Print("inode: %d\n", inode);
        // Print("i: %d\n", i);
        // Print("inode->extents[i].start_block: %d\n", inode->extents[i].start_block);

        struct FS_Buffer *buffer = NULL;

        // void *buffer = _Malloc(SECTOR_SIZE);
        int blockNum = inode->extents[i].start_block + frontBlockNum;

        if (frontBlockNum >= inode->extents[i].length_blocks && frontBlockNum != 0) {
            // if (((char*)buf)[0] == -51) {
                // Print("i:%d frontBlockNum:%d length_blocks:%d startPos mod SECTOR_SIZE:%d\n", i, frontBlockNum, inode->extents[i].length_blocks, startPos % SECTOR_SIZE);
            // }
            frontBlockNum -= inode->extents[i].length_blocks; 
            if (frontBlockNum + (startPos % SECTOR_SIZE != 0) > 0 && i == GFS3_EXTENTS - 1) {
                //TODO allocate the space reversely
                int rc = allocateBlocks(inode, instance, frontBlockNum + (startPos % SECTOR_SIZE != 0) + 5);
                if (rc != 0) {
                    return bytesWrite;
                }
                // Print("rc: %d\n", rc);
                return writeToInode(inode, buf, numBytes, instance, startPos, overwrite);
                // blockNum += frontBlockNum;
            }
            else if (startPos % SECTOR_SIZE != 0)  {
                continue;
            }
        }
        // Print("blockNum: %d\n", blockNum);
        // Print("i: %d\n", i);
        // if (((char *)buf)[0] == 98) while(1);
        // frontBlockNum = 0;
        // Block_Read(instance->dev, inode->extents[i].start_block, buffer);
        // Print("i: %d\n", i);
        // Print("inode->extents[i].start_block: %d\n", inode->extents[i].start_block);
        // if ((frontBlockNum == 1 && startPos % SECTOR_SIZE == 0)) blockNum++, frontBlockNum = 0;

        // Print("inode->extents[i].length_blocks: %d\n", inode->extents[i].length_blocks);
        int rc = Get_FS_Buffer(instance->cache, blockNum, &buffer);
        // Print("inode->extents[i].length_blocks: %d\n", inode->extents[i].length_blocks);
        if (inode->extents[i].length_blocks == 0) while(1);
        if (rc != 0) {
            Print("rc: %d\n", rc);
            Print("inode: %d\n", inode);
            Print("inode->extents[i].start_block: %d\n", inode->extents[i].start_block);
            return bytesWrite;
        }

        while(1) {
            if (bytesWrite >= numBytes || blockNum - inode->extents[i].start_block >= inode->extents[i].length_blocks) {
                break;
            }
            // Print("%d ", blockNum);
            ulong_t writeSize = MIN(numBytes - bytesWrite, offset == 0 ? SECTOR_SIZE : SECTOR_SIZE - offset);

            // memcpy((char *)buf + bytesWrite, buffer->data + offset, writeSize);
            memcpy(buffer->data + offset, (char *)buf + bytesWrite, writeSize);
            Modify_FS_Buffer(instance->cache, buffer);

            offset = 0;
            bytesWrite += writeSize;
            blockNum++;
            if (blockNum - inode->extents[i].start_block >= inode->extents[i].length_blocks) break;
            Release_FS_Buffer(instance->cache, buffer);
            buffer = NULL;
            rc = Get_FS_Buffer(instance->cache, blockNum, &buffer);
            if (rc != 0) {
                return bytesWrite;
            }
        }
        Release_FS_Buffer(instance->cache, buffer);
        buffer = NULL;

        // Print("bytesWrite < numBytes: %d\n", bytesWrite < numBytes);
        // Print("frontBlockNum: %d\n", frontBlockNum);
        if (bytesWrite < numBytes) {
            while (1) {
                if (bytesWrite >= numBytes) break;
                // Print("%da ", blockNum);
                // Print("%d ", bytesWrite);
                // Print("inode->extents[i].length_blocks: %d\n", inode->extents[i].length_blocks);
                int rc = Get_FS_Buffer(instance->cache, blockNum, &buffer);
                // Print("inode->extents[i].length_blocks: %d\n", inode->extents[i].length_blocks);
                if (rc != 0) {
                    return bytesWrite;
                }
                bool isBlockFree = !Is_Bit_Set(instance->bitmap, blockNum);
                // Print("isBlockFree: %d\n", isBlockFree);
                // Print("blockNum: %d\n", blockNum);
                // Print("Find_Next_Free: %d\n", Find_First_Free_Bit(instance->bitmap, SECTOR_SIZE * 8));
                if (isBlockFree) {
                    // Print("%d:NB ", ((char *)buf)[0]);
                    // Print("len %d ", inode->extents[i].length_blocks);
                    // Print("sb: %d\n", inode->extents[i].start_block);
                    // Print("blockNum: %d\n", blockNum);
                    // Print("BN %d ", blockNum);
                    setBlockUsed(instance, blockNum, 1);
                    ulong_t writeSize = MIN(numBytes - bytesWrite, SECTOR_SIZE);
                    // Print("writeSize: %d\n", writeSize);
                    memcpy(buffer->data, (char *)buf + bytesWrite, writeSize);
                    Modify_FS_Buffer(instance->cache, buffer);
                    Release_FS_Buffer(instance->cache, buffer);
                    buffer = NULL;
                    inode->extents[i].length_blocks++;
                    blockNum++;
                    bytesWrite += writeSize;
                } else {
                    Print("NF ");
                    Release_FS_Buffer(instance->cache, buffer);
                    buffer = NULL;
                    break;
                }
            }

            if (bytesWrite < numBytes) {
                // TODO Find a larger contiguous chunk of free blocks (with Find_First_N_Free), move all file blocks  to these free blocks and release old blocks
                // while(1);
                int blocksNeeded = (numBytes - bytesWrite + SECTOR_SIZE - 1) / SECTOR_SIZE;
                int freeBlocks = Find_First_N_Free(instance->bitmap, inode->extents[i].length_blocks + blocksNeeded, SECTOR_SIZE * 8);
                // Print("freeBlocks: %d\n", freeBlocks);
                if (freeBlocks != -1) {
                    // Print("blocksNeeded: %d\n", blocksNeeded);
                    int j;
                    for (j = 0; j < inode->extents[i].length_blocks; j++) {
                        struct FS_Buffer *oldBuffer = NULL;
                        // Release_FS_Buffer(instance->cache, buffer);
                        rc = Get_FS_Buffer(instance->cache, inode->extents[i].start_block + j, &oldBuffer);
                        if (rc != 0) {
                            return bytesWrite;
                        }

                        struct FS_Buffer *newBuffer = NULL;
                        rc = Get_FS_Buffer(instance->cache, freeBlocks + j, &newBuffer);
                        if (rc != 0) {
                            Release_FS_Buffer(instance->cache, oldBuffer);
                            buffer = NULL;
                            return bytesWrite;
                        }

                        memcpy(newBuffer->data, oldBuffer->data, SECTOR_SIZE);
                        Modify_FS_Buffer(instance->cache, newBuffer);
                        Release_FS_Buffer(instance->cache, oldBuffer);
                        Release_FS_Buffer(instance->cache, newBuffer);
                        oldBuffer = NULL;
                        newBuffer = NULL;
                        setBlockUsed(instance, freeBlocks + j, 1);
                        setBlockUsed(instance, inode->extents[i].start_block + j, 0);
                    }

                    blockNum = freeBlocks + inode->extents[i].length_blocks;

                    for (j = 0; j < blocksNeeded; j++) {
                        setBlockUsed(instance, blockNum, 1);
                        int rc = Get_FS_Buffer(instance->cache, blockNum, &buffer);
                        if (rc != 0) {
                            return bytesWrite;
                        }
                        ulong_t writeSize = MIN(numBytes - bytesWrite, SECTOR_SIZE);
                        memcpy(buffer->data, (char *)buf + bytesWrite, writeSize);
                        Modify_FS_Buffer(instance->cache, buffer);
                        Release_FS_Buffer(instance->cache, buffer);
                        buffer = NULL;
                        bytesWrite += writeSize;
                        blockNum++;
                    }

                    inode->extents[i].start_block = freeBlocks;
                    inode->extents[i].length_blocks += blocksNeeded;
                }

            }
        }

        // Release_FS_Buffer(instance->cache, buffer);
    }

    if (bytesWrite == 0) {
        // Print("buf[0]: %d\n", ((char *)buf)[0]);
    }
    
    return bytesWrite;
}

int deleteRecursively(struct gfs3_instance *instance, struct gfs3_inode *inode, gfs3_inodenum inodenum, struct gfs3_inode *parentInode, char *name, bool recursive) {
    // Print("deleteRecursively\n");
    struct FS_Buffer_Cache *cache = instance->cache;
    struct gfs3_inode *childInode = NULL;
    struct FS_Buffer *buffer = NULL;
    int rc;
    struct gfs3_dirent *dirent = NULL;

    inode->reference_count--;
    if (inode->reference_count > 0) {
        return 0;
    }

    int i;
    // for (i = 0; i < GFS3_EXTENTS; i++) {
    //     if (inode->extents[i].start_block == 0 && inode->type == GFS3_DIRECTORY) continue;

    //     struct FS_Buffer *buffer = NULL;

    //     int rc = Get_FS_Buffer(instance->cache, inode->extents[i].start_block, &buffer);
    //     if (rc != 0) {
    //         return rc;
    //     }

    //     int blockNum = inode->extents[i].start_block;

    //     dirent = (struct gfs3_dirent *)buffer->data;

        // if (inode->type == GFS3_DIRECTORY) {
        //     for (; blockNum - inode->extents[i].start_block < inode->extents[i].length_blocks;) {
        //         Release_FS_Buffer(instance->cache, buffer);
        //         childInode = getInode(instance, dirent->inum);

        //         if (!recursive) return EINVALID;
        //         deleteRecursively(instance, childInode, dirent->inum, inode, dirent->name, 1);
        //         Free(childInode);

        //         if (dirent->entry_length != 0) {
        //             ulong_t jump_length = dirent->entry_length + 4;

        //             dirent = (struct gfs3_dirent *)((char *)dirent + jump_length);
        //         } 
        //         else if (blockNum - inode->extents[i].start_block < inode->extents[i].length_blocks) {
        //             blockNum++;

        //             Release_FS_Buffer(instance->cache, buffer);
        //             int rc = Get_FS_Buffer(instance->cache, blockNum, &buffer);
        //             if (rc != 0) {
        //                 return rc;
        //             }
        //             dirent = (struct gfs3_dirent *)buffer->data;
        //         }
        //     }
        // }
        // Release_FS_Buffer(instance->cache, buffer);

        // // Print("Delete\n");
        // rc = Get_FS_Buffer(instance->cache, inode->extents[i].start_block, &buffer);
        // if (rc != 0) {
        //     return rc;
        // }

        // blockNum = inode->extents[i].start_block;
        // dirent = (struct gfs3_dirent *)buffer->data;

        // for (; blockNum - inode->extents[i].start_block < inode->extents[i].length_blocks;) {
        //     memset(dirent, 0, dirent->entry_length + 4);
        //     Modify_FS_Buffer(instance->cache, buffer);

        //     if (blockNum - inode->extents[i].start_block < inode->extents[i].length_blocks) {
        //         blockNum++;

        //         Release_FS_Buffer(instance->cache, buffer);
        //         rc = Get_FS_Buffer(instance->cache, blockNum, &buffer);
        //         if (rc != 0) {
        //             return rc;
        //         }
        //         dirent = (struct gfs3_dirent *)buffer->data;
        //     }
        // }

    //     Release_FS_Buffer(instance->cache, buffer);
    //     rc = Get_FS_Buffer(instance->cache, inode->extents[i].start_block, &buffer);
    //     if (rc != 0) {
    //         return rc;
    //     }
    //     dirent = (struct gfs3_dirent *)buffer->data;
    //     dirent->name_length = 0;
    //     dirent->inum = 0;
    //     Modify_FS_Buffer(instance->cache, buffer);
    //     Release_FS_Buffer(instance->cache, buffer);
    // }

    // int cnt = 0;
    for (i = 0; i < GFS3_EXTENTS; i++) {
        if (parentInode->extents[i].start_block == 0) continue;

        struct FS_Buffer *buffer = NULL;
        int rc = Get_FS_Buffer(instance->cache, parentInode->extents[i].start_block, &buffer);
        if (rc != 0) {
            return rc;
        }

        int blockNum = parentInode->extents[i].start_block;
        struct gfs3_dirent *dirent = (struct gfs3_dirent *)buffer->data;

        setBlocksUsed(instance, inode->extents[i].start_block, inode->extents[i].length_blocks, 0);
        writeInode(instance, inodenum, inode);

        for (; blockNum - parentInode->extents[i].start_block < parentInode->extents[i].length_blocks;) {
            if (dirent->inum != 0 && strncmp(dirent->name, name, strlen(name)) == 0) {
                // cnt++;
                int entry_length = dirent->entry_length;
                memset(dirent, 0, dirent->entry_length + 4);
                dirent->entry_length = entry_length;
                // Print("dirent->name: %s\n", dirent->name);
                // Print("dirent->inum: %d\n", dirent->inum);
                Modify_FS_Buffer(instance->cache, buffer);
                Release_FS_Buffer(instance->cache, buffer);
                return 0;
            }
            if (dirent->entry_length != 0) {
                ulong_t jump_length = dirent->entry_length + 4;
                dirent = (struct gfs3_dirent *)((char *)dirent + jump_length);
            } else if (blockNum - parentInode->extents[i].start_block < parentInode->extents[i].length_blocks) {
                blockNum++;
                Release_FS_Buffer(instance->cache, buffer);
                rc = Get_FS_Buffer(instance->cache, blockNum, &buffer);
                if (rc != 0) {
                    return rc;
                }
                dirent = (struct gfs3_dirent *)buffer->data;
            }
        }
        Release_FS_Buffer(instance->cache, buffer);
    }
    // Print("cnt: %d\n", cnt);
    return ENOTFOUND;
}

/* ----------------------------------------------------------------------
 * Implementation of VFS operations
 * ---------------------------------------------------------------------- */

/*
 * Get metadata for given file.
 */
static int GFS3_Sync(struct Mount_Point *mountPoint);

static int GFS3_FStat(struct File *file, struct VFS_File_Stat *stat) {
    // TODO_P(PROJECT_GFS3, "GeekOS filesystem FStat operation");
    // Print("GFS3_FStat\n");
    struct gfs3_file *gfs3File = (struct gfs3_file *)file->fsData;
    struct gfs3_inode *inode = gfs3File->inode;

    stat->isDirectory = (inode->type == GFS3_DIRECTORY);
    stat->size = inode->size;
    return 0;
}

/*
 * Read data from current position in file.
 */
static int GFS3_Read(struct File *file, void *buf, ulong_t numBytes) {
    // TODO_P(PROJECT_GFS3, "GeekOS filesystem read operation");
    // Print("GFS3_Read\n");
    if ((file->mode & O_READ) == 0) {
        return EACCESS;
    }
    ulong_t startPos = file->filePos;
    ulong_t endPos = startPos + numBytes;
    ulong_t bytesRead = 0;

    struct gfs3_file *gfs3File = (struct gfs3_file *)file->fsData;
    struct gfs3_instance *instance = (struct gfs3_instance *)file->mountPoint->fsData;
    struct gfs3_inode *inode = gfs3File->inode;

    int frontBlockNum = startPos / SECTOR_SIZE;
    int offset = startPos % SECTOR_SIZE;

    int i;
    for (i = 0; i < GFS3_EXTENTS && bytesRead < numBytes; i++) {
        if (inode->extents[i].start_block == 0) continue;

        struct FS_Buffer *buffer = NULL;

        // void *buffer = _Malloc(SECTOR_SIZE);
        int blockNum = inode->extents[i].start_block + frontBlockNum;

        if (blockNum - inode->extents[i].start_block >= inode->extents[i].length_blocks) {
            frontBlockNum -= inode->extents[i].length_blocks;
            continue;
        }
        frontBlockNum = 0;
        // Block_Read(instance->dev, inode->extents[i].start_block, buffer);
        int rc = Get_FS_Buffer(instance->cache, blockNum, &buffer);
        if (rc != 0) {
            file->filePos += bytesRead;
            return bytesRead;
        }

        while(1) {
            // Print("bytesRead: %d\n", bytesRead);
            if (bytesRead >= numBytes) {
                break;
            }
            ulong_t readSize = MIN(numBytes - bytesRead, offset == 0 ? SECTOR_SIZE : SECTOR_SIZE - offset);
            memcpy((char *)buf + bytesRead, buffer->data + offset, readSize);
            offset = 0;
            bytesRead += readSize;
            blockNum++;
            if (blockNum - inode->extents[i].start_block >= inode->extents[i].length_blocks) break;
            Release_FS_Buffer(instance->cache, buffer);
            rc = Get_FS_Buffer(instance->cache, blockNum, &buffer);
            if (rc != 0) {
                file->filePos += bytesRead;
                return bytesRead;
            }
        }
        Release_FS_Buffer(instance->cache, buffer);
    }

    file->filePos += bytesRead;
    return bytesRead;
    // return EUNSUPPORTED;
}

/*g
 * Write data to current position in file.
 */
static int GFS3_Write(struct File *file, void *buf, ulong_t numBytes) {
    // TODO_P(PROJECT_GFS3, "GeekOS filesystem write operation");
    if ((file->mode & O_WRITE) == 0) {
        return EACCESS;
    }
    ulong_t startPos = file->filePos;

    struct gfs3_file *gfs3File = (struct gfs3_file *)file->fsData;
    struct gfs3_instance *instance = (struct gfs3_instance *)file->mountPoint->fsData;
    struct gfs3_inode *inode = gfs3File->inode;

    // Print("startPos: %d\n", startPos);
    ulong_t bytesWrite = writeToInode(inode, buf, numBytes, instance, startPos, 1);
    // ulong_t bytesWrite = 100;
    // if (bytesWrite < numBytes) {
    //     Print("%d\n", ((char *)buf)[0]);
    //     Print("bytesWrite: %d ", bytesWrite);
    //     while(1);
    // }
    // else if (bytesWrite > numBytes) {
    //     Print("bytesWrite11: %d ", bytesWrite);
    //     while(1);
    // }
    // Print("\n");
    file->filePos = startPos + bytesWrite;
    inode->size = MAX(inode->size, startPos + bytesWrite);
    writeInode(instance, gfs3File->inodenum, inode);

    return bytesWrite;
    // return EUNSUPPORTED;
}


/*
 * Seek to a position in file; returns 0 on success.
 */
static int GFS3_Seek(struct File *file, ulong_t pos) {
    // TODO_P(PROJECT_GFS3, "GeekOS filesystem seek operation");
    // Print("GFS3_Seek\n");
    // if (pos > file->endPos || pos < 0) {
    //     Print("pos: %d\n", pos);
    //     Print("file->endPos: %d\n", file->endPos);
    //     return EUNSPECIFIED;
    // }
    file->filePos = pos;    
    return 0;
    // return EUNSUPPORTED;
}

/*
 * Close a file.
 */
static int GFS3_Close(struct File *file) {
    // TODO_P(PROJECT_GFS3, "GeekOS filesystem close operation");
    // Print("GFS3_Close\n");
    GFS3_Sync(file->mountPoint);
    struct gfs3_file *gfs3File = (struct gfs3_file *)file->fsData;
    Free(gfs3File);
    return 0;
    // return EUNSUPPORTED;
}

/*static*/ struct File_Ops s_gfs3FileOps = {
    &GFS3_FStat,
    &GFS3_Read,
    &GFS3_Write,
    &GFS3_Seek,
    &GFS3_Close,
    0,                          /* Read_Entry */
};

/*
 * Stat operation for an already open directory.
 */
static int GFS3_FStat_Directory(struct File *dir,
                                struct VFS_File_Stat *stat) {
    /* may be unused. */
    // TODO_P(PROJECT_GFS3, "GeekOS filesystem FStat directory operation");
    // Print("GFS3_FStat_Directory\n");
    struct gfs3_file *gfs3File = (struct gfs3_file *)dir->fsData;
    struct gfs3_inode *inode = gfs3File->inode;

    stat->isDirectory = (inode->type == GFS3_DIRECTORY);
    stat->size = inode->size;
    return 0;
}

/*
 * Directory Close operation.
 */
static int GFS3_Close_Directory(struct File *dir) {
    // TODO_P(PROJECT_GFS3, "GeekOS filesystem Close directory operation");
    // Print("GFS3_Close_Directory\n");
    struct gfs3_file *gfs3File = (struct gfs3_file *)dir->fsData;
    Free(gfs3File);
    return 0;
    // return EUNSUPPORTED;
}

/*
 * Read a directory entry from an open directory.
 */
static int GFS3_Read_Entry(struct File *dir, struct VFS_Dir_Entry *entry) {
    // TODO_P(PROJECT_GFS3, "GeekOS filesystem Read_Entry operation");
    Print("GFS3_Read_Entry\n");
    struct gfs3_file *gfs3File = (struct gfs3_file *)dir->fsData;
    struct gfs3_instance *instance = (struct gfs3_instance *)dir->mountPoint->fsData;
    struct gfs3_inode *inode = gfs3File->inode;

    ulong_t filePos = dir->filePos;
    ulong_t bytesRead = 0;

    int i;
    for (i = 0; i < GFS3_EXTENTS; i++) {
        if (inode->extents[i].start_block == 0) continue;

        struct FS_Buffer *buffer = NULL;
        int rc = Get_FS_Buffer(instance->cache, inode->extents[i].start_block, &buffer);
        if (rc != 0) {
            return rc;
        }
        // void *buffer = _Malloc(SECTOR_SIZE);
        int blockNum = inode->extents[i].start_block;
        // Block_Read(instance->dev, inode->extents[i].start_block, buffer);

        struct gfs3_dirent *dirent = (struct gfs3_dirent *)buffer->data;

        for (; blockNum - inode->extents[i].start_block < inode->extents[i].length_blocks;) {
            struct gfs3_inode *tempInode = getInode(instance, dirent->inum);
            int type = tempInode->type;
            Free(tempInode);

            // Print("dirent->name: %s\n", dirent->name);
            if (dirent->inum != 0) {
                if (bytesRead >= filePos) {
                    strncpy(entry->name, dirent->name, dirent->name_length);
                    entry->name[dirent->name_length] = '\0';
                    entry->stats.isDirectory = type;
                    dir->filePos += dirent->entry_length;
                    Release_FS_Buffer(instance->cache, buffer);
                    return 0;
                }
            }
            if (dirent->entry_length != 0) {
                ulong_t jump_length = dirent->entry_length + 4;
                bytesRead += dirent->entry_length;

                dirent = (struct gfs3_dirent *)((char *)dirent + jump_length);
            } 
            else if (blockNum - inode->extents[i].start_block < inode->extents[i].length_blocks) {
                blockNum++;
                // Block_Read(instance->dev, blockNum, buffer);
                Release_FS_Buffer(instance->cache, buffer);
                int rc = Get_FS_Buffer(instance->cache, blockNum, &buffer);
                if (rc != 0) {
                    return rc;
                }
                dirent = (struct gfs3_dirent *)buffer->data;
            }
        }
        Release_FS_Buffer(instance->cache, buffer);
    }

    return VFS_NO_MORE_DIR_ENTRIES;
    // return EUNSUPPORTED;
}

/*static*/ struct File_Ops s_gfs3DirOps = {
    &GFS3_FStat_Directory,
    0,                          /* Read */
    0,                          /* Write */
    0,                          /* Seek */
    &GFS3_Close_Directory,
    &GFS3_Read_Entry,
};



/*
 * Open a file named by given path.
 */
static int GFS3_Open(struct Mount_Point *mountPoint, const char *path,
                     int mode, struct File **pFile) {
    // Print("GFS3_Open\n");
    // TODO_P(PROJECT_GFS3, "GeekOS filesystem open operation");

    struct gfs3_instance *instance = (struct gfs3_instance *)mountPoint->fsData;
    int blockNum = instance->sb->block_with_inode_zero;
    gfs3_inodenum inodenum = GFS3_INUM_ROOT;
    struct gfs3_inode *inode = getInode(instance, GFS3_INUM_ROOT);
    struct gfs3_inode *directoryInode = NULL;
    gfs3_inodenum directoryInodenum = GFS3_INUM_ROOT;

    char name[MAX_NAME_LEN];

    char *currentPath = (char *)Malloc(strlen(path) + 1);
    if (currentPath == NULL) {
        return ENOMEM;
    }
    char *duplicatedPath = currentPath;
    // Print("path: %s\n", path);
    strcpy(currentPath, path);
    // Print("currentPath: %s\n", currentPath);

    /* Iterate through the path until reaching the end */
    while (*currentPath != 0) {
        ulong_t direntStartPos = 0;
        ulong_t previousStartPos = 0;
        memset(name, '\0', MAX_NAME_LEN); // Reset name to empty
        Get_Next_Name(&currentPath, name);

        // If not at the end of the path, check if the current inode is a directory
        if (*currentPath != 0 && inode->type != GFS3_DIRECTORY) {
            Free(duplicatedPath);
            return ENOTDIR;
        }

        bool found = false;

        // If the current inode is a directory, get the inode number of the next directory
        // entry in the current directory
        struct gfs3_dirent *dirent = NULL;

        int i;
        for (i = 0; i < GFS3_EXTENTS; i++) {
            if (inode->extents[i].start_block == 0) continue;

            struct FS_Buffer *buffer = NULL;

            int rc = Get_FS_Buffer(instance->cache, inode->extents[i].start_block, &buffer);
            if (rc != 0) {
                Free(duplicatedPath);
                return rc;
            }
            // void *buffer = _Malloc(SECTOR_SIZE);
            int blockNum = inode->extents[i].start_block;
            // Block_Read(mountPoint->dev, inode->extents[i].start_block, buffer);

            // void *startAddr = buffer;
            dirent = (struct gfs3_dirent *)buffer->data;

            // Print("blockNum - inode->extents[i].start_block: %d\n", blockNum - inode->extents[i].start_block);
            // Print("inode->extents[i].length_blocks: %d\n", inode->extents[i].length_blocks);
            for (; blockNum - inode->extents[i].start_block < inode->extents[i].length_blocks;) {
                // Print("dirent->name: %s\n", dirent->name);
                // Print("name: %s\n", name);
                // Print("dirent->name_length: %d\n", dirent->name_length);
                // Print("dirent: %d\n", (char *)dirent);
                // Print("dirent->inum: %d\n", dirent->inum);
                if (dirent->inum != 0) {
                    direntStartPos += dirent->entry_length + 4;
                    // Print("%s %d ", dirent->name,direntStartPos);
                    previousStartPos = direntStartPos;
                    if (strncmp(dirent->name, name, strlen(name)) == 0 && dirent->name_length == strlen(name)) {
                        // Print("found\n");
                        inodenum = dirent->inum;
                        found = true;
                        break;
                    }
                }
                // ulong_t jump_length = (6 + dirent->name_length + 3) / 4 * 4;
                if (dirent->entry_length != 0) {
                    ulong_t jump_length = dirent->entry_length + 4;

                    dirent = (struct gfs3_dirent *)((char *)dirent + jump_length);
                } 
                else if (blockNum - inode->extents[i].start_block < inode->extents[i].length_blocks) {
                    direntStartPos = 0;
                    int j;
                    for (j = 0; j < i; j++) direntStartPos += inode->extents[i].length_blocks * SECTOR_SIZE;
                    direntStartPos += (blockNum - inode->extents[i].start_block + 1) * SECTOR_SIZE;
                    blockNum++;
                    // Block_Read(mountPoint->dev, blockNum, buffer);
                    Release_FS_Buffer(instance->cache, buffer);
                    int rc = Get_FS_Buffer(instance->cache, blockNum, &buffer);
                    if (rc != 0) {
                        Free(duplicatedPath);
                        return rc;
                    }
                    dirent = (struct gfs3_dirent *)buffer->data;
                }
            }
            
            Release_FS_Buffer(instance->cache, buffer);
        }
        
        direntStartPos = previousStartPos;

        if (!found && *currentPath != 0) {
            Free(duplicatedPath);
            return ENOTFOUND;
        }

        // Get the inode of the next directory
        if (directoryInode != NULL) {
            Free(directoryInode);
            directoryInode = NULL;
        }
        directoryInodenum = inodenum;
        directoryInode = inode;
        // Print("inodenum: %d\n", inodenum);
        inode = getInode(instance, inodenum);

        if (*currentPath == 0) {
            // If at the end of the path, open the file
            bool isFile = inode->type == GFS3_FILE;

            if (!found || !isFile) {
                if (mode & O_CREATE) {
                    // Find a free inode
                    Print("Create file\n");
                    struct gfs3_inode *newInode = NULL;
                    gfs3_inodenum newInodeNum;
                    for (newInodeNum = 3; newInodeNum < instance->sb->number_of_inodes; newInodeNum++) {
                        newInode = getInode(instance, newInodeNum);
                        if (newInode->reference_count == 0) {
                            break;
                        }
                        newInode = NULL;
                    }

                    if (newInode == NULL) {
                        Free(duplicatedPath);
                        return ENOMEM;
                    }

                    // Set up the parameters for the new file
                    newInode->type = GFS3_FILE;
                    newInode->size = 0;
                    newInode->reference_count = 1;
                    memset(newInode->extents, 0, sizeof(newInode->extents));
                    // Print("newInode: %d\n", newInode);
                    // Print("%d\n", newInode->extents[0].start_block);
                    // Print("%d\n", newInode->extents[1].start_block);
                    // Print("%d\n", newInode->extents[2].start_block);


                    ulong_t newDirentLength = (6 + strlen(name) + 3) / 4 * 4;
                    struct gfs3_dirent *newDirent = _Malloc(newDirentLength);
                    newDirent->inum = newInodeNum;
                    newDirent->entry_length = newDirentLength;
                    newDirent->name_length = strlen(name);
                    // Print("name: %s\n", name);
                    strcpy(newDirent->name, name);

                    if (direntStartPos / SECTOR_SIZE < (direntStartPos + newDirentLength) / SECTOR_SIZE) {
                        direntStartPos += SECTOR_SIZE - (direntStartPos % SECTOR_SIZE);
                    }
                    // Print("startPos: %d\n", direntStartPos);
                    ulong_t bytesWrite = writeToInode(directoryInode, newDirent, newDirentLength, instance, direntStartPos, 1);
                    directoryInode->size += bytesWrite;
                    Print("directoryInode->size: %d\n", directoryInode->size);
                    // Print("inode->size: %d\n", inode->size);
                    // Print("bytesWrite: %d\n", bytesWrite);
                    // Print("directoryInodeNum: %d\n", directoryInodenum);
                    writeInode(instance, directoryInodenum, directoryInode);
                    // Print("inode: %d\n", inode);
                    // Print("directoryInode: %d\n", directoryInode);
                    writeInode(instance, newInodeNum, newInode);

                    // struct gfs3_inode *tempInode = getInode(instance, newInodeNum);
                    // Print("tempInode->type: %d\n", tempInode->type);
                    // Free(tempInode);
                    // Print("newInode->type: %d\n", newInode->type);

                    // Print("bytesWrite: %d\n", bytesWrite);
                    // Print("direntBlock: %d\n", direntStartPos / SECTOR_SIZE + directoryInode);
                    if (directoryInode != NULL) Free(directoryInode);
                    // setBlockUsed(instance, 329, 1);
                    if (bytesWrite != newDirentLength) {
                        Free(duplicatedPath);
                        return ENOMEM;
                    }
                    Free(inode);
                    inode = newInode;
                    inodenum = newInodeNum;
                    
                } else {
                    Free(duplicatedPath);
                    return ENOTFOUND;
                }
            }

            // Open the file
            struct File *file = Allocate_File(&s_gfs3FileOps, 0, inode->size, inode, mode, mountPoint);

            struct gfs3_file *gfs3File = _Malloc(sizeof(struct gfs3_file));
            gfs3File->inode = inode;
            gfs3File->inodenum = inodenum;

            file->fsData = gfs3File;

            if (file == NULL) {
                Free(duplicatedPath);
                return ENOMEM;
            }
            *pFile = file;

        }
    }
    Free(duplicatedPath);
    return 0;
    // return EUNSUPPORTED;
}

/*
 * Create a directory named by given path.
 */
static int GFS3_Create_Directory(struct Mount_Point *mountPoint,
                                 const char *path) {
    // TODO_P(PROJECT_GFS3, "GeekOS filesystem create directory operation");
    struct gfs3_instance *instance = (struct gfs3_instance *)mountPoint->fsData;
    int blockNum = instance->sb->block_with_inode_zero;
    gfs3_inodenum inodenum = GFS3_INUM_ROOT;
    struct gfs3_inode *inode = getInode(instance, GFS3_INUM_ROOT);
    struct gfs3_inode *directoryInode = NULL;
    gfs3_inodenum directoryInodenum = GFS3_INUM_ROOT;

    char name[MAX_NAME_LEN];

    char *currentPath = (char *)Malloc(strlen(path) + 1);
    if (currentPath == NULL) {
        return ENOMEM;
    }
    char *duplicatedPath = currentPath;
    // Print("path: %s\n", path);
    strcpy(currentPath, path);
    

    /* Iterate through the path until reaching the end */
    while (*currentPath != 0) {
        ulong_t direntStartPos = 0;
        ulong_t previousStartPos = 0;
        memset(name, '\0', MAX_NAME_LEN); // Reset name to empty
        Get_Next_Name(&currentPath, name);

        // If not at the end of the path, check if the current inode is a directory
        if (inode->type != GFS3_DIRECTORY) {
            return ENOTDIR;
        }

        bool found = false;

        // If the current inode is a directory, get the inode number of the next directory
        // entry in the current directory
        struct gfs3_dirent *dirent = NULL;

        int i;
        for (i = 0; i < GFS3_EXTENTS; i++) {
            if (inode->extents[i].start_block == 0) continue;

            struct FS_Buffer *buffer = NULL;

            int rc = Get_FS_Buffer(instance->cache, inode->extents[i].start_block, &buffer);
            if (rc != 0) {
                Free(duplicatedPath);
                return rc;
            }
            // void *buffer = _Malloc(SECTOR_SIZE);
            int blockNum = inode->extents[i].start_block;
            // Block_Read(mountPoint->dev, inode->extents[i].start_block, buffer);

            // void *startAddr = buffer;
            dirent = (struct gfs3_dirent *)buffer->data;

            // Print("blockNum - inode->extents[i].start_block: %d\n", blockNum - inode->extents[i].start_block);
            for (; blockNum - inode->extents[i].start_block < inode->extents[i].length_blocks;) {
                // Print("dirent->name: %s\n", dirent->name);
                // Print("dirent->name_length: %d\n", dirent->name_length);
                // Print("dirent: %d\n", (char *)dirent);
                // Print("dirent->inum: %d\n", dirent->inum);
                if (dirent->inum != 0) {
                    direntStartPos += dirent->entry_length + 4;
                    previousStartPos = direntStartPos;
                    if (strncmp(dirent->name, name, strlen(name)) == 0 && dirent->name_length == strlen(name)) {
                        // Print("found\n");
                        // Print("dirent->name: %s\n", dirent->name);
                        // Print("dirent->inum: %d\n", dirent->inum);
                        directoryInodenum = inodenum;
                        inodenum = dirent->inum;
                        found = true;
                        break;
                    }
                }
                // ulong_t jump_length = (6 + dirent->name_length + 3) / 4 * 4;
                if (dirent->entry_length != 0) {
                    ulong_t jump_length = dirent->entry_length + 4;

                    dirent = (struct gfs3_dirent *)((char *)dirent + jump_length);
                } 
                else if (blockNum - inode->extents[i].start_block < inode->extents[i].length_blocks) {
                    direntStartPos = 0;
                    int j;
                    for (j = 0; j < i; j++) direntStartPos += inode->extents[i].length_blocks * SECTOR_SIZE;
                    direntStartPos += (blockNum - inode->extents[i].start_block + 1) * SECTOR_SIZE;
                    blockNum++;
                    // Block_Read(mountPoint->dev, blockNum, buffer);
                    Release_FS_Buffer(instance->cache, buffer);
                    int rc = Get_FS_Buffer(instance->cache, blockNum, &buffer);
                    if (rc != 0) {
                        Free(duplicatedPath);
                        return rc;
                    }
                    dirent = (struct gfs3_dirent *)buffer->data;
                }
            }
            Release_FS_Buffer(instance->cache, buffer);
        }

        direntStartPos = previousStartPos;

        if (found && *currentPath == 0) {
            return EEXIST;
        }
        else if (!found) {
            if (*currentPath != 0) {
                return ENOTFOUND;
            }
        }

        if (directoryInode != NULL) Free(directoryInode);
        directoryInode = inode;
        // Print("inodenum: %d\n", inodenum);
        inode = getInode(instance, inodenum);

        if (*currentPath == 0) {
            // Find a free inode
            Print("Create directory\n");
            struct gfs3_inode *newInode = NULL;
            gfs3_inodenum newInodeNum = 0;
            for (newInodeNum = 3; newInodeNum < instance->sb->number_of_inodes; newInodeNum++) {
                newInode = getInode(instance, newInodeNum);
                if (newInode->reference_count == 0) {
                    break;
                }
                newInode = NULL;
            }

            if (newInode == NULL) {
                Free(duplicatedPath);
                return ENOMEM;
            }

            // Set up the parameters for the new directory
            newInode->type = GFS3_DIRECTORY;
            newInode->size = 0;
            newInode->reference_count = 1;
            memset(newInode->extents, 0, sizeof(newInode->extents));

            // Create a new directory entry for the new directory
            ulong_t newDirentLength = (6 + strlen(name) + 3) / 4 * 4;
            struct gfs3_dirent *newDirent = _Malloc(newDirentLength);
            newDirent->inum = newInodeNum;
            newDirent->entry_length = newDirentLength;
            newDirent->name_length = strlen(name);
            strcpy(newDirent->name, name);

            // Write the new directory entry to the parent directory
            ulong_t bytesWrite = writeToInode(inode, newDirent, newDirentLength, instance, direntStartPos, 1);
            inode->size += bytesWrite;
            // writeInode(instance, directoryInodenum, directoryInode);
            writeInode(instance, inodenum, inode);
            writeInode(instance, newInodeNum, newInode);
            // if (directoryInode != NULL) Free(directoryInode);
            // Free(inode);
            
            // struct gfs3_inode *tempInode = getInode(instance, newInodeNum);
            // Print("tempInode->type: %d\n", tempInode->type);
            // Free(tempInode);

            if (bytesWrite != newDirentLength) {
                Free(duplicatedPath);
                return ENOMEM;
            }
            Free(duplicatedPath);
            return 0;
        }
    }
    Free(duplicatedPath);
    return -1;
    // return EUNSUPPORTED;
}

/*
 * Open a directory named by given path.
 */
static int GFS3_Open_Directory(struct Mount_Point *mountPoint,
                               const char *path, struct File **pDir) {
    // TODO_P(PROJECT_GFS3, "GeekOS filesystem open directory operation");
    // Print("GFS3_Open_Directory\n");
    struct gfs3_instance *instance = (struct gfs3_instance *)mountPoint->fsData;
    int blockNum = instance->sb->block_with_inode_zero;
    gfs3_inodenum inodenum = GFS3_INUM_ROOT;
    struct gfs3_inode *inode = getInode(instance, GFS3_INUM_ROOT);

    char name[MAX_NAME_LEN];

    char *currentPath = (char *)Malloc(strlen(path) + 1);
    if (currentPath == NULL) {
        return ENOMEM;
    }
    char *duplicatedPath = currentPath;
    // Print("path: %s\n", path);
    strcpy(currentPath, path);
    if (strlen(currentPath) == 1) {
        Print("open root\n");
        struct File *dir = Allocate_File(&s_gfs3DirOps, 0, inode->size, inode, O_READ, mountPoint);
        struct gfs3_file *gfs3File = _Malloc(sizeof(struct gfs3_file));
        gfs3File->inode = inode;
        gfs3File->inodenum = inodenum;

        dir->fsData = gfs3File;

        if (dir == NULL) {
            Free(duplicatedPath);
            return ENOMEM;
        }
        *pDir = dir;
        Free(duplicatedPath);
        return 0;
    }

    /* Iterate through the path until reaching the end */
    while (*currentPath != 0) {
        memset(name, '\0', MAX_NAME_LEN); // Reset name to empty
        Get_Next_Name(&currentPath, name);

        // If not at the end of the path, check if the current inode is a directory
        if (inode->type != GFS3_DIRECTORY) {
            return ENOTDIR;
        }

        bool found = false;

        // If the current inode is a directory, get the inode number of the next directory
        // entry in the current directory
        struct gfs3_dirent *dirent = NULL;

        int i;
        for (i = 0; i < GFS3_EXTENTS; i++) {
            if (inode->extents[i].start_block == 0) continue;

            struct FS_Buffer *buffer = NULL;

            int rc = Get_FS_Buffer(instance->cache, inode->extents[i].start_block, &buffer);
            if (rc != 0) {
                Free(duplicatedPath);
                return rc;
            }
            // void *buffer = _Malloc(SECTOR_SIZE);
            int blockNum = inode->extents[i].start_block;
            // Block_Read(mountPoint->dev, inode->extents[i].start_block, buffer);

            // void *startAddr = buffer;
            dirent = (struct gfs3_dirent *)buffer->data;

            // Print("blockNum - inode->extents[i].start_block: %d\n", blockNum - inode->extents[i].start_block);
            for (; blockNum - inode->extents[i].start_block < inode->extents[i].length_blocks;) {
                Print("dirent->name: %s\n", dirent->name);
                Print("name: %s\n", name);
                // Print("dirent->name_length: %d\n", dirent->name_length);
                // Print("dirent: %d\n", (char *)dirent);
                // Print("dirent->inum: %d\n", dirent->inum);
                if (dirent->inum != 0) {
                    if (strncmp(dirent->name, name, strlen(name)) == 0 && dirent->name_length == strlen(name)) {
                        // Print("found\n");
                        inodenum = dirent->inum;
                        found = true;
                        break;
                    }
                }
                // ulong_t jump_length = (6 + dirent->name_length + 3) / 4 * 4;
                if (dirent->entry_length != 0) {
                    ulong_t jump_length = dirent->entry_length + 4;

                    dirent = (struct gfs3_dirent *)((char *)dirent + jump_length);
                } 
                else if (blockNum - inode->extents[i].start_block < inode->extents[i].length_blocks) {
                    blockNum++;
                    // Block_Read(mountPoint->dev, blockNum, buffer);
                    Release_FS_Buffer(instance->cache, buffer);
                    int rc = Get_FS_Buffer(instance->cache, blockNum, &buffer);
                    if (rc != 0) {
                        Free(duplicatedPath);
                        return rc;
                    }
                    dirent = (struct gfs3_dirent *)buffer->data;
                }
            }
            Release_FS_Buffer(instance->cache, buffer);
        }

        if (!found) {
            return ENOTFOUND;
        }

        // Get the inode of the next directory
        Free(inode);
        inode = getInode(instance, inodenum);
        if (*currentPath == 0) {
            // If at the end of the path, open the directory
            if (inode->type != GFS3_DIRECTORY) {
                return ENOTFOUND;
            }

            // Open the directory
            struct File *dir = Allocate_File(&s_gfs3DirOps, 0, inode->size, inode, O_READ, mountPoint);

            struct gfs3_file *gfs3File = _Malloc(sizeof(struct gfs3_file));
            gfs3File->inode = inode;
            gfs3File->inodenum = inodenum;

            dir->fsData = gfs3File;

            if (dir == NULL) {
                Free(duplicatedPath);
                return ENOMEM;
            }
            *pDir = dir;
        }
    }
    Free(duplicatedPath);
    return 0;
    // return EUNSUPPORTED;
}

/*
 * Open a directory named by given path.
 */
static int GFS3_Delete(struct Mount_Point *mountPoint, const char *path,
                       bool recursive) {
    // TODO_P(PROJECT_GFS3, "GeekOS filesystem delete operation");
    struct gfs3_instance *instance = (struct gfs3_instance *)mountPoint->fsData;
    struct gfs3_inode *inode = getInode(instance, GFS3_INUM_ROOT);
    gfs3_inodenum inodenum = GFS3_INUM_ROOT, parentInodeNum = GFS3_INUM_ROOT;
    int direntSize = 0;
    struct gfs3_inode *parentInode = NULL;

    char name[MAX_NAME_LEN];
    char *currentPath = (char *)Malloc(strlen(path) + 1);
    if (currentPath == NULL) {
        return ENOMEM;
    }
    char *duplicatedPath = currentPath;
    strcpy(currentPath, path);

    while (*currentPath != 0) {
        memset(name, '\0', MAX_NAME_LEN);
        Get_Next_Name(&currentPath, name);

        if (*currentPath != 0 && inode->type != GFS3_DIRECTORY) {
            Free(duplicatedPath);
            return ENOTDIR;
        }

        bool found = false;
        struct gfs3_dirent *dirent = NULL;

        int i;
        for (i = 0; i < GFS3_EXTENTS; i++) {
            if (inode->extents[i].start_block == 0) continue;

            struct FS_Buffer *buffer = NULL;
            int rc = Get_FS_Buffer(instance->cache, inode->extents[i].start_block, &buffer);
            if (rc != 0) {
                Free(duplicatedPath);
                return rc;
            }

            int blockNum = inode->extents[i].start_block;
            dirent = (struct gfs3_dirent *)buffer->data;

            for (; blockNum - inode->extents[i].start_block < inode->extents[i].length_blocks;) {
                if (dirent->inum != 0) {
                    if (strncmp(dirent->name, name, strlen(name)) == 0 && dirent->name_length == strlen(name)) {
                        parentInodeNum = inodenum;
                        inodenum = dirent->inum;
                        direntSize = dirent->entry_length;
                        found = true;
                        break;
                    }
                }
                if (dirent->entry_length != 0) {
                    ulong_t jump_length = dirent->entry_length + 4;
                    dirent = (struct gfs3_dirent *)((char *)dirent + jump_length);
                } else if (blockNum - inode->extents[i].start_block < inode->extents[i].length_blocks) {
                    blockNum++;
                    Release_FS_Buffer(instance->cache, buffer);
                    int rc = Get_FS_Buffer(instance->cache, blockNum, &buffer);
                    if (rc != 0) {
                        Free(duplicatedPath);
                        return rc;
                    }
                    dirent = (struct gfs3_dirent *)buffer->data;
                }
            }
            Release_FS_Buffer(instance->cache, buffer);
        }

        if (!found) {
            Free(duplicatedPath);
            Print("ENOTFOUND\n");
            return ENOTFOUND;
        }

        if (parentInode != NULL) Free(parentInode);
        parentInode = inode;
        inode = getInode(instance, inodenum);
    }

    if (inode == NULL) {
        Free(duplicatedPath);
        return ENOTFOUND;
    }

    if (inode->type == GFS3_DIRECTORY) {
        // Print("inode->type: %d\n", inode->type);
        // Print("recursive: %d\n", recursive);
        Print("inode->size: %d\n", inode->size);
        if (inode->size > 0) {
            Free(duplicatedPath);
            return EUNSPECIFIED;
        }
        int rc = deleteRecursively(instance, inode, inodenum, parentInode, name, 0);
        parentInode->size -= direntSize;
        writeInode(instance, parentInodeNum, parentInode);
        Free(duplicatedPath);
        return rc;
    }

    // if (inode->type == GFS3_DIRECTORY && recursive) {
    //     deleteRecursively(instance, inode);
    // }
    Print("delete file\n");
    int rc = deleteRecursively(instance, inode, inodenum, parentInode, name, 1);
    parentInode->size -= direntSize;
    Print("parentInode->size: %d\n", parentInode->size);
    writeInode(instance, parentInodeNum, parentInode);
    Print("inodenum: %d\n", inodenum);
    if (parentInode != NULL) Free(parentInode);
    Free(inode);

    Free(duplicatedPath);
    Print("file deleted\n");
    // Print("name: %s\n", name);
    return rc;
    // return EUNSUPPORTED;
}

/*
 * Get metadata (size, permissions, etc.) of file named by given path.
 */
static int GFS3_Stat(struct Mount_Point *mountPoint, const char *path,
                     struct VFS_File_Stat *stat) {
    // TODO_P(PROJECT_GFS3, "GeekOS filesystem stat operation");
    // Print("GFS3_Stat\n");
    struct gfs3_instance *instance = (struct gfs3_instance *)mountPoint->fsData;
    struct gfs3_inode *inode = getInode(instance, GFS3_INUM_ROOT);
    gfs3_inodenum inodenum = GFS3_INUM_ROOT;

    char name[MAX_NAME_LEN];
    
    char *currentPath = (char *)Malloc(strlen(path) + 1);
    if (currentPath == NULL) {
        return ENOMEM;
    }
    char *duplicatedPath = currentPath;
    strcpy(currentPath, path);

    while (*currentPath != 0) {
        memset(name, '\0', MAX_NAME_LEN);
        Get_Next_Name(&currentPath, name);

        if (*currentPath != 0 && inode->type != GFS3_DIRECTORY) {
            Free(duplicatedPath);
            return ENOTDIR;
        }

        bool found = false;
        struct gfs3_dirent *dirent = NULL;

        int i;
        for (i = 0; i < GFS3_EXTENTS; i++) {
            if (inode->extents[i].start_block == 0) continue;

            struct FS_Buffer *buffer = NULL;
            int rc = Get_FS_Buffer(instance->cache, inode->extents[i].start_block, &buffer);
            if (rc != 0) {
                return rc;
            }
            // void *buffer = _Malloc(SECTOR_SIZE);
            int blockNum = inode->extents[i].start_block;
            // Block_Read(mountPoint->dev, inode->extents[i].start_block, buffer);

            dirent = (struct gfs3_dirent *)((char *)buffer->data);

            for (; blockNum - inode->extents[i].start_block < inode->extents[i].length_blocks;) {
                // Print("dirent->name: %s\n", dirent->name);
                // Print("blockNum: %d\n", blockNum);
                // Print("inodenum: %d\n", inodenum);
                // Print("dirent->name_length: %d\n", dirent->name_length);
                // Print("dirent: %d\n", (char *)dirent);
                // Print("dirent->inum: %d\n", dirent->inum);
                if (dirent->inum != 0) {
                    // Print("dirent->name: %s\n", dirent->name);
                    // Print("name: %s\n", name);
                    // Print("dirent->inum: %d\n", dirent->inum);
                    if (strncmp(dirent->name, name, strlen(name)) == 0 && dirent->name_length == strlen(name)) {
                        // Print("found\n");
                        inodenum = dirent->inum;
                        found = true;
                        break;
                    }
                }
                // ulong_t jump_length = (6 + dirent->name_length + 3) / 4 * 4;
                if (dirent->entry_length != 0) {
                    ulong_t jump_length = dirent->entry_length + 4;

                    dirent = (struct gfs3_dirent *)((char *)dirent + jump_length);
                } 
                else if (blockNum - inode->extents[i].start_block < inode->extents[i].length_blocks) {
                    blockNum++;
                    // Block_Read(mountPoint->dev, blockNum, buffer);
                    Release_FS_Buffer(instance->cache, buffer);
                    int rc = Get_FS_Buffer(instance->cache, blockNum, &buffer);
                    if (rc != 0) {
                        Free(duplicatedPath);
                        return rc;
                    }
                    dirent = (struct gfs3_dirent *)buffer->data;
                }
            }
            Release_FS_Buffer(instance->cache, buffer);
        }

        if (!found) {
            Free(duplicatedPath);
            return ENOTFOUND;
        }

        Free(inode);
        inode = getInode(instance, inodenum);

        if (*currentPath == 0) {
            break;
        }
    }

    if (inode == NULL) {
        Free(duplicatedPath);
        return ENOTFOUND;
    }

    // Print("inode->size: %d\n", inode->size);
    stat->isDirectory = (inode->type == GFS3_DIRECTORY);
    stat->size = inode->size;

    Free(inode);
    Free(duplicatedPath);
    return 0;
    // return EUNSUPPORTED;
}

/*
 * Synchronize the filesystem data with the disk
 * (i.e., flush out all buffered filesystem data).
 */
static int GFS3_Sync(struct Mount_Point *mountPoint) {
    // TODO_P(PROJECT_GFS3, "GeekOS filesystem sync operation");
    return Sync_FS_Buffer_Cache(((struct gfs3_instance *)mountPoint->fsData)->cache);
    // return EUNSUPPORTED;
}

static int GFS3_Disk_Properties(struct Mount_Point *mountPoint,
                                unsigned int *block_size,
                                unsigned int *blocks_in_disk) {
    // TODO_P(PROJECT_GFS3,
    //        "GeekOS filesystem infomation operation; set variables.");
    // Print("GFS3_Disk_Properties\n");
    struct gfs3_instance *instance = (struct gfs3_instance *)mountPoint->fsData;
    *block_size = instance->sb->blocks_per_disk;
    *blocks_in_disk = instance->sb->number_of_inodes;
    return 0;
    // return EUNSUPPORTED;
}

/*static*/ struct Mount_Point_Ops s_gfs3MountPointOps = {
    &GFS3_Open,
    &GFS3_Create_Directory,
    &GFS3_Open_Directory,
    &GFS3_Stat,
    &GFS3_Sync,
    &GFS3_Delete,
    0,                          /* Rename  */
    0,                          /* Link  */
    0,                          /* SymLink  */
    0,                          /* setuid  */
    0,                          /* acl  */
    &GFS3_Disk_Properties,
};

static int GFS3_Format(struct Block_Device *blockDev
                       __attribute__ ((unused))) {
    TODO_P(PROJECT_GFS3,
           "DO NOT IMPLEMENT: There is no format operation for GFS3");
    return EUNSUPPORTED;
}

static int GFS3_Mount(struct Mount_Point *mountPoint) {
    // TODO_P(PROJECT_GFS3, "GeekOS filesystem mount operation");
    // Print("GFS3_Mount\n");
    struct gfs3_superblock *sb = _Malloc(sizeof(struct gfs3_superblock));
    void *buffer = _Malloc(SECTOR_SIZE);

    Block_Read(mountPoint->dev, 0, buffer);


    memcpy(sb, (char *)buffer + PFAT_BOOT_RECORD_OFFSET, sizeof(struct gfs3_superblock));

    if (sb->gfs3_magic != GFS3_MAGIC) {
        return EINVALIDFS;
    }

    mountPoint->ops = &s_gfs3MountPointOps; 

    struct gfs3_instance *instance = _Malloc(sizeof(struct gfs3_instance));
    instance->sb = sb;
    instance->cache = Create_FS_Buffer_Cache(mountPoint->dev, SECTOR_SIZE);
    instance->dev = mountPoint->dev;
    instance->rootInode = getInode(instance, GFS3_INUM_ROOT);
    mountPoint->fsData = instance;

    struct gfs3_inode *inode = getInode(instance, 2);
    instance->bitmap = (uchar_t *) Malloc(SECTOR_SIZE);
    instance->bitmap = Create_Bit_Set(SECTOR_SIZE * 8);

    struct FS_Buffer *fsBuffer = NULL;

    int rc = Get_FS_Buffer(instance->cache, inode->extents->start_block, &fsBuffer);
    if (rc != 0) {
        return rc;
    }

    memcpy(instance->bitmap, fsBuffer->data, SECTOR_SIZE);
    Release_FS_Buffer(instance->cache, fsBuffer);
    int i;
    // for (i = 0; i <= instance->sb->number_of_inodes / GFS3_INODES_PER_BLOCK; i++) {
    //     setBlockUsed(instance, i, 1);
    //     Print("i: %d\n", i);
    // }
    Free(inode);

    return 0;
    // return EUNSUPPORTED;
}


static struct Filesystem_Ops s_gfs3FilesystemOps = {
    &GFS3_Format,
    &GFS3_Mount,
};

/* ----------------------------------------------------------------------
 * Public functions
 * ---------------------------------------------------------------------- */

void Init_GFS3(void) {
    Register_Filesystem("gfs3", &s_gfs3FilesystemOps);
}
