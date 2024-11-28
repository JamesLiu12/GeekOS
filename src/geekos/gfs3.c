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
    inode = (struct gfs3_inode *)((char *)buffer->data + inode_offset * sizeof(struct gfs3_inode));
    Release_FS_Buffer(cache, buffer);
    return inode;
}

/* ----------------------------------------------------------------------
 * Implementation of VFS operations
 * ---------------------------------------------------------------------- */

/*
 * Get metadata for given file.
 */
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
    ulong_t startPos = file->filePos;
    ulong_t endPos = startPos + numBytes;
    ulong_t bytesRead = 0;

    struct gfs3_file *gfs3File = (struct gfs3_file *)file->fsData;
    struct gfs3_instance *instance = (struct gfs3_instance *)file->mountPoint->fsData;
    struct gfs3_inode *inode = gfs3File->inode;

    int frontBlockNum = startPos / SECTOR_SIZE;

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
        int offset = startPos % SECTOR_SIZE;
        // Block_Read(instance->dev, inode->extents[i].start_block, buffer);
        int rc = Get_FS_Buffer(instance->cache, inode->extents[i].start_block, &buffer);
        if (rc != 0) {
            file->filePos += bytesRead;
            return bytesRead;
        }

        for (; blockNum - inode->extents[i].start_block < inode->extents[i].length_blocks;) {
            // Print("bytesRead: %d\n", bytesRead);
            if (bytesRead >= numBytes) {
                break;
            }
            ulong_t readSize = MIN(numBytes - bytesRead, offset == 0 ? SECTOR_SIZE : SECTOR_SIZE - offset);
            memcpy((char *)buf + bytesRead, buffer->data + offset, readSize);
            offset = 0;
            bytesRead += readSize;
        }
        Release_FS_Buffer(instance->cache, buffer);
    }

    file->filePos += bytesRead;
    return bytesRead;
    // return EUNSUPPORTED;
}

/*
 * Write data to current position in file.
 */
static int GFS3_Write(struct File *file, void *buf, ulong_t numBytes) {
    TODO_P(PROJECT_GFS3, "GeekOS filesystem write operation");
    return EUNSUPPORTED;
}


/*
 * Seek to a position in file; returns 0 on success.
 */
static int GFS3_Seek(struct File *file, ulong_t pos) {
    // TODO_P(PROJECT_GFS3, "GeekOS filesystem seek operation");
    // Print("GFS3_Seek\n");
    if (pos > file->endPos || pos < 0) {
        return EUNSPECIFIED;
    }
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
    // Print("GFS3_Read_Entry\n");
    struct gfs3_file *gfs3File = (struct gfs3_file *)dir->fsData;
    struct gfs3_instance *instance = (struct gfs3_instance *)dir->mountPoint->fsData;
    struct gfs3_inode *inode = gfs3File->inode;

    ulong_t filePos = dir->filePos;
    ulong_t bytesRead = 0;

    int i;
    for (i = 0; i < GFS3_EXTENTS; i++) {
        if (inode->extents[i].start_block == 0) continue;

        struct FS_Buffer *buffer = _Malloc(sizeof(struct FS_Buffer));
        int rc = Get_FS_Buffer(instance->cache, inode->extents[i].start_block, &buffer);
        if (rc != 0) {
            return rc;
        }
        // void *buffer = _Malloc(SECTOR_SIZE);
        int blockNum = inode->extents[i].start_block;
        // Block_Read(instance->dev, inode->extents[i].start_block, buffer);

        struct gfs3_dirent *dirent = (struct gfs3_dirent *)buffer->data;

        for (; blockNum - inode->extents[i].start_block < inode->extents[i].length_blocks;) {
            int type = getInode(instance, dirent->inum)->type;

            // Print("dirent->name: %s\n", dirent->name);
            if (dirent->inum != 0) {
                if (bytesRead >= filePos) {
                    strncpy(entry->name, dirent->name, MAX_NAME_LEN);
                    entry->name[MAX_NAME_LEN - 1] = '\0';
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
            else {
                blockNum++;
                // Block_Read(instance->dev, blockNum, buffer);
                Release_FS_Buffer(instance->cache, buffer);
                int rc = Get_FS_Buffer(instance->cache, inode->extents[i].start_block, &buffer);
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
            for (; blockNum - inode->extents[i].start_block < inode->extents[i].length_blocks;) {
                // Print("dirent->name: %s\n", dirent->name);
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
                else {
                    blockNum++;
                    // Block_Read(mountPoint->dev, blockNum, buffer);
                    Release_FS_Buffer(instance->cache, buffer);
                    int rc = Get_FS_Buffer(instance->cache, inode->extents[i].start_block, &buffer);
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

        // Get the inode of the next directory
        inode = getInode(instance, inodenum);

        if (*currentPath == 0) {
            // If at the end of the path, open the file
            if (inode->type != GFS3_FILE) {
                Free(duplicatedPath);
                return ENOTFOUND;
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
    TODO_P(PROJECT_GFS3, "GeekOS filesystem create directory operation");
    return EUNSUPPORTED;
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
                // Print("dirent->name: %s\n", dirent->name);
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
                else {
                    blockNum++;
                    // Block_Read(mountPoint->dev, blockNum, buffer);
                    Release_FS_Buffer(instance->cache, buffer);
                    int rc = Get_FS_Buffer(instance->cache, inode->extents[i].start_block, &buffer);
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
                return ENOMEM;
            }
            *pDir = dir;
        }
    }
    return 0;
    // return EUNSUPPORTED;
}

/*
 * Open a directory named by given path.
 */
static int GFS3_Delete(struct Mount_Point *mountPoint, const char *path,
                       bool recursive) {
    TODO_P(PROJECT_GFS3, "GeekOS filesystem delete operation");
    return EUNSUPPORTED;
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
                // Print("dirent->name_length: %d\n", dirent->name_length);
                // Print("dirent: %d\n", (char *)dirent);
                // Print("dirent->inum: %d\n", dirent->inum);
                if (dirent->inum != 0) {
                    // Print("dirent->name: %s\n", dirent->name);
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
                else {
                    blockNum++;
                    // Block_Read(mountPoint->dev, blockNum, buffer);
                    Release_FS_Buffer(instance->cache, buffer);
                    int rc = Get_FS_Buffer(instance->cache, inode->extents[i].start_block, &buffer);
                    if (rc != 0) {
                        Free(duplicatedPath);
                        return rc;
                    }
                    dirent = (struct gfs3_dirent *)buffer;
                }
            }
            Release_FS_Buffer(instance->cache, buffer);
        }

        if (!found) {
            Free(duplicatedPath);
            return ENOTFOUND;
        }

        inode = getInode(instance, inodenum);

        if (*currentPath == 0) {
            break;
        }
    }

    if (inode == NULL) {
        Free(duplicatedPath);
        return ENOTFOUND;
    }

    stat->isDirectory = (inode->type == GFS3_DIRECTORY);
    stat->size = inode->size;

    Free(duplicatedPath);
    return 0;
    // return EUNSUPPORTED;
}

/*
 * Synchronize the filesystem data with the disk
 * (i.e., flush out all buffered filesystem data).
 */
static int GFS3_Sync(struct Mount_Point *mountPoint) {
    TODO_P(PROJECT_GFS3, "GeekOS filesystem sync operation");
    return EUNSUPPORTED;
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
