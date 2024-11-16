/*
 * Paging-based user mode implementation
 * Copyright (c) 2001,2003,2004 David H. Hovemeyer <daveho@cs.umd.edu>
 * Copyright (c) 2003,2013,2014 Jeffrey K. Hollingsworth <hollings@cs.umd.edu>
 *
 * All rights reserved.
 *
 * This code may not be resdistributed without the permission of the copyright holders.
 * Any student solutions using any of this code base constitute derviced work and may
 * not be redistributed in any form.  This includes (but is not limited to) posting on
 * public forums or web sites, providing copies to (past, present, or future) students
 * enrolled in similar operating systems courses the University of Maryland's CMSC412 course.
 *
 * $Revision: 1.51 $
 */

#include <geekos/int.h>
#include <geekos/mem.h>
#include <geekos/paging.h>
#include <geekos/malloc.h>
#include <geekos/string.h>
#include <geekos/argblock.h>
#include <geekos/kthread.h>
#include <geekos/range.h>
#include <geekos/vfs.h>
#include <geekos/user.h>
#include <geekos/projects.h>
#include <geekos/smp.h>
#include <geekos/synch.h>
#include <geekos/errno.h>

extern Spin_Lock_t kthreadLock;

int userDebug = 0;
#define Debug(args...) if (userDebug) Print("uservm: " args)

/* ----------------------------------------------------------------------
 * Private functions
 * ---------------------------------------------------------------------- */

// TODO: Add private functions
void *User_To_Kernel(struct User_Context *userContext, ulong_t userPtr) {
    uchar_t *userBase = (uchar_t *) userContext->memory;

    return (void *)(userBase + userPtr);
}

extern struct User_Context *Create_User_Context() {
    struct User_Context *context;
    int index;

    /* Allocate memory for the user context */
    context = (struct User_Context *)Malloc(sizeof(*context));

    if(context == 0)
        goto fail;

    memset(context, 0, sizeof(struct User_Context));

    context->memory = (void*)MEM_USER_START;
    context->size = MEM_USER_END - MEM_USER_START + 1;

    pte_t *userPageDir = Alloc_Page();
    if (userPageDir == 0) Exit(-1);
    memset(userPageDir, 0, PAGE_SIZE);
    context->pageDir = userPageDir;

    memcpy(context->pageDir, Kernel_Page_Dir(), NUM_PAGE_DIR_ENTRIES * sizeof(pde_t) / 2);

    // int pageDirIndex = 0;
    // for (pageDirIndex = 0; pageDirIndex < NUM_PAGE_DIR_ENTRIES / 2; pageDirIndex++) {
    //     if (Kernel_Page_Dir()[pageDirIndex].present == 1) {
    //         context->pageDir[pageDirIndex] = Kernel_Page_Dir()[pageDirIndex];
    //         memcpy(&context->pageDir[pageDirIndex], &Kernel_Page_Dir()[pageDirIndex], sizeof(pde_t));
    //     }
    // }
    // print("pageDirIndex: %d\n", pageDirIndex);
    // for (pageDirIndex = PAGE_DIRECTORY_INDEX(MEM_STACK); pageDirIndex < PAGE_DIRECTORY_INDEX(MEM_END); pageDirIndex++) {
    //     if (Kernel_Page_Dir()[pageDirIndex].present == 1) {
    //         context->pageDir[pageDirIndex] = Kernel_Page_Dir()[pageDirIndex];
    //     }
    // }
    // print("pageDirIndex: %d\n", pageDirIndex);

    int apicDirIndex = PAGE_DIRECTORY_INDEX(MEM_APIC);
    // int apicioDirIndex = PAGE_DIRECTORY_INDEX(MEM_APICIO);

    context->pageDir[apicDirIndex] = Kernel_Page_Dir()[apicDirIndex];
    // context->pageDir[apicioDirIndex] = Kernel_Page_Dir()[apicioDirIndex];

    /* Allocate an LDT descriptor for the user context */
    context->ldtDescriptor = Allocate_Segment_Descriptor();
    if(context->ldtDescriptor == 0)
        goto fail;
    if(userDebug)
        Print("Allocated descriptor %d for LDT\n",
              Get_Descriptor_Index(context->ldtDescriptor));
    Init_LDT_Descriptor(context->ldtDescriptor, context->ldt,
                        NUM_USER_LDT_ENTRIES);
    index = Get_Descriptor_Index(context->ldtDescriptor);
    context->ldtSelector = Selector(KERNEL_PRIVILEGE, true, index);

    /* Initialize code and data segments within the LDT */
    Init_Code_Segment_Descriptor(&context->ldt[0],
                                 MEM_USER_START,
                                 (MEM_USER_END - MEM_USER_START + 1) / PAGE_SIZE, 
                                 USER_PRIVILEGE);
    Init_Data_Segment_Descriptor(&context->ldt[1],
                                 MEM_USER_START,
                                 (MEM_USER_END - MEM_USER_START + 1) / PAGE_SIZE, 
                                 USER_PRIVILEGE);
    context->csSelector = Selector(USER_PRIVILEGE, false, 0);
    context->dsSelector = Selector(USER_PRIVILEGE, false, 1);

    /* Nobody is using this user context yet */
    context->refCount = 0;


    /* Success! */
    return context;

  fail:
    /* We failed; release any allocated memory */
    if(context != 0) {
        Free(context);
    }

    return 0;
}


/* ----------------------------------------------------------------------
 * Public functions
 * ---------------------------------------------------------------------- */

/*
 * Destroy a User_Context object, including all memory
 * and other resources allocated within it.
 */
void Destroy_User_Context(struct User_Context *context) {
    /*
     * Hints:
     * - Free all pages, page tables, and page directory for
     *   the process (interrupts must be disabled while you do this,
     *   otherwise those pages could be stolen by other processes)
     * - Free semaphores, files, and other resources used
     *   by the process
     */
    // TODO_P(PROJECT_VIRTUAL_MEMORY_A,
    //        "Destroy User_Context data structure after process exits");

    KASSERT(context->refCount == 0);

    /* Free the context's LDT descriptor */
    Free_Segment_Descriptor(context->ldtDescriptor);
    
    pde_t *pageDir = context->pageDir;
    int pageDirIndex = PAGE_DIRECTORY_INDEX(MEM_USER_START);
    while (pageDirIndex <= PAGE_DIRECTORY_INDEX(MEM_USER_END)) {
        if (pageDir[pageDirIndex].present == 1) {
            pte_t *pageTable = (pte_t *) (pageDir[pageDirIndex].pageTableBaseAddr << 12);
            int pageTableIndex = 0;

            if (pageDirIndex == PAGE_DIRECTORY_INDEX(MEM_USER_END) && 
                pageTableIndex >= PAGE_TABLE_INDEX(MEM_USER_END)) {
                break;
            }

            while (pageTableIndex < NUM_PAGE_TABLE_ENTRIES) {
                if (pageTable[pageTableIndex].present == 1) {
                    Free_Page((void *) (pageTable[pageTableIndex].pageBaseAddr << 12));
                }
                pageTableIndex++;
            }
            Free_Page(pageTable);
        }
        pageDirIndex++;
    }

    Free_Page(pageDir);

    /* Free the context's memory */
    Free(context);
}

/*
 * Load a user executable into memory by creating a User_Context
 * data structure.
 * Params:
 * exeFileData - a buffer containing the executable to load
 * exeFileLength - number of bytes in exeFileData
 * exeFormat - parsed ELF segment information describing how to
 *   load the executable's text and data segments, and the
 *   code entry point address
 * command - string containing the complete command to be executed:
 *   this should be used to create the argument block for the
 *   process
 * pUserContext - reference to the pointer where the User_Context
 *   should be stored
 *
 * Returns:
 *   0 if successful, or an error code (< 0) if unsuccessful
 */
int Load_User_Program(char *exeFileData, ulong_t exeFileLength,
                      struct Exe_Format *exeFormat, const char *command,
                      struct User_Context **pUserContext) {
    /*
     * Hints:
     * - This will be similar to the same function in userseg.c
     * - Determine space requirements for code, data, argument block,
     *   and stack
     * - Allocate pages for above, map them into user address
     *   space (allocating page directory and page tables as needed)
     * - Fill in initial stack pointer, argument block address,
     *   and code entry point fields in User_Context
     */
    // TODO_P(PROJECT_VIRTUAL_MEMORY_A,
    //        "Load user program into address space");
    int i;
    unsigned numArgs;
    ulong_t argBlockSize;
    ulong_t size, argBlockAddr;
    struct User_Context *userContext = 0;

    /* Create User_Context */
    userContext = Create_User_Context();
    if(userContext == 0)
        return -1;
    
    pte_t *originalPageDir = Get_PDBR();
    Set_PDBR(userContext->pageDir);

    /* Load segment data into memory */
    for(i = 0; i < exeFormat->numSegments; ++i) {
        struct Exe_Segment *segment = &exeFormat->segmentList[i];
        ulong_t copied = 0;
        ulong_t virtualAddress = (segment->startAddress) + MEM_USER_START;
        ulong_t offset = virtualAddress % PAGE_SIZE;
        ulong_t toBeCopied = segment->lengthInFile;

        if (PAGE_ADDR(segment->startAddress) == 0) continue;

        while(toBeCopied > 0) {
            
            void *pageAddr = Get_Page_Addr(virtualAddress, userContext->pageDir);
            int size = toBeCopied > (PAGE_SIZE - offset) ? PAGE_SIZE - offset : toBeCopied;
            memcpy(pageAddr + offset, exeFileData + segment->offsetInFile + copied, size);
            Make_Page_Pageable(virtualAddress, userContext->pageDir);

            if ((segment->protFlags & VM_WRITE) == 0) {
                Make_Page_ReadOnly(virtualAddress, userContext->pageDir);
            }
            
            copied += size;
            toBeCopied -= size;
            offset = 0;
            virtualAddress += size;
        }
    }

    Get_Argument_Block_Size(command, &numArgs, &argBlockSize);

    void *pageAddr = Get_Page_Addr(MEM_STACK - argBlockSize, userContext->pageDir);
    Make_Page_Pageable(MEM_STACK - argBlockSize, userContext->pageDir);

    /* Format argument block */
    Format_Argument_Block(pageAddr + PAGE_SIZE - argBlockSize, numArgs,
                          MEM_STACK - argBlockSize - MEM_USER_START, 
                          command);

    pageAddr = Get_Page_Addr(MEM_STACK - argBlockSize - PAGE_SIZE, userContext->pageDir);
    Make_Page_Pageable(MEM_STACK - argBlockSize - PAGE_SIZE, userContext->pageDir);

    pageAddr = Get_Page_Addr(MEM_STACK - argBlockSize - PAGE_SIZE * 2, userContext->pageDir);
    Make_Page_Pageable(MEM_STACK - argBlockSize - PAGE_SIZE * 2, userContext->pageDir);

    /* Fill in code entry point */
    userContext->entryAddr = exeFormat->entryAddr;

    /*
     * Fill in addresses of argument block and stack
     * (They happen to be the same)
     */
    userContext->argBlockAddr = MEM_STACK - argBlockSize - MEM_USER_START;
    userContext->stackPointerAddr = MEM_STACK - argBlockSize - MEM_USER_START;

    userContext->stackLimit = MEM_STACK - PAGE_SIZE * 3;

    *pUserContext = userContext;

    Set_PDBR(originalPageDir);

    return 0;
}

bool Validate_User_Memory(struct User_Context * userContext,
                          ulong_t userAddr, ulong_t bufSize,
                          int for_writing) {
    ulong_t avail;
    for_writing = for_writing;  /* avoid warning */
    
    if (userAddr >= MEM_USER_END - MEM_USER_START) return false;

    avail = (MEM_USER_END - MEM_USER_START) - userAddr;
    if(bufSize > avail)
        return false;

    return true;
}

/*
 * Copy data from user buffer into kernel buffer.
 * Returns true if successful, false otherwise.
 */
bool Copy_From_User(void *destInKernel, ulong_t srcInUser,
                    ulong_t numBytes) {
    /*
     * Hints:
     * - Make sure that user page is part of a valid region
     *   of memory
     * - Remember that you need to add 0x80000000 to user addresses
     *   to convert them to kernel addresses, because of how the
     *   user code and data segments are defined
     * - User pages may need to be paged in from disk before being accessed.
     * - Before you touch (read or write) any data in a user
     *   page, **disable the PAGE_PAGEABLE bit**.
     *
     * Be very careful with race conditions in reading a page from disk.
     * Kernel code must always assume that if the struct Page for
     * a page of memory has the PAGE_PAGEABLE bit set,
     * IT CAN BE STOLEN AT ANY TIME.  The only exception is if
     * interrupts are disabled; because no other process can run,
     * the page is guaranteed not to be stolen.
     */
    // TODO_P(PROJECT_VIRTUAL_MEMORY_A, "Copy user data to kernel buffer");
    struct User_Context *current = CURRENT_THREAD->userContext;

    if(!Validate_User_Memory(current, srcInUser, numBytes, VUM_READING))
        return false;
    pte_t *origPageDir = Get_PDBR();
    Set_PDBR(current->pageDir);
    Get_Page_Addr(srcInUser + MEM_USER_START, current->pageDir);
    memcpy(destInKernel, User_To_Kernel(current, srcInUser), numBytes);
    Make_Page_Pageable(srcInUser + MEM_USER_START, current->pageDir);
    Set_PDBR(origPageDir);

    return true;
}

/*
 * Copy data from kernel buffer into user buffer.
 * Returns true if successful, false otherwise.
 */
bool Copy_To_User(ulong_t destInUser, const void *srcInKernel,
                  ulong_t numBytes) {
    /*
     * Hints:
     * - Same as for Copy_From_User()
     * - Also, make sure the memory is mapped into the user
     *   address space with write permission enabled
     */
    // TODO_P(PROJECT_VIRTUAL_MEMORY_A, "Copy kernel data to user buffer");
    struct User_Context *current = CURRENT_THREAD->userContext;

    if(!Validate_User_Memory(current, destInUser, numBytes, VUM_WRITING))
        return false;
    pte_t *origPageDir = Get_PDBR();
    Set_PDBR(current->pageDir);
    Get_Page_Addr(destInUser + MEM_USER_START, current->pageDir);
    memcpy(User_To_Kernel(current, destInUser), srcInKernel, numBytes);
    Make_Page_Pageable(destInUser + MEM_USER_START, current->pageDir);
    Set_PDBR(origPageDir);

    return true;
}


/*
 * Switch to user address space.
 */
void Switch_To_Address_Space(struct User_Context *userContext) {
    /*
     * - If you are still using an LDT to define your user code and data
     *   segments, switch to the process's LDT
     * - 
     */
    // TODO_P(PROJECT_VIRTUAL_MEMORY_A,
    //        "Switch_To_Address_Space() using paging");
    ushort_t ldtSelector;

    /* Eager check to ensure that the new address space
       has either memory (userseg) or a page directory 
       (uservm) and is not likely to abort */
    KASSERT(userContext->memory || userContext->pageDir);

    /* Switch to the LDT of the new user context */
    ldtSelector = userContext->ldtSelector;
    __asm__ __volatile__("lldt %0"::"a"(ldtSelector)
        );

    Set_PDBR(userContext->pageDir);
}
