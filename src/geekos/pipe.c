/*
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
 */
#include <geekos/pipe.h>
#include <geekos/malloc.h>
#include <geekos/string.h>
#include <geekos/errno.h>
#include <geekos/projects.h>
#include <geekos/int.h>
#include <geekos/smp.h>
#include <geekos/signal.h>
#include <geekos/user.h>
#include <signal.h>


const struct File_Ops Pipe_Read_Ops =
    { NULL, Pipe_Read, NULL, NULL, Pipe_Close, NULL };
const struct File_Ops Pipe_Write_Ops =
    { NULL, NULL, Pipe_Write, NULL, Pipe_Close, NULL };

int Pipe_Create(struct File **read_file, struct File **write_file) {
    struct Pipe *pipe = Malloc(sizeof(struct Pipe));
    if (!pipe) return ENOMEM;

    pipe->buffer = Malloc(PIPE_BUFFER_CAPACITY);
    if (!pipe->buffer) return ENOMEM;

    pipe->read_pos = 0;
    pipe->write_pos = 0;
    pipe->reader = 1;
    pipe->writer = 1;
    pipe->buffer_size = 0;

    *read_file = Allocate_File(&Pipe_Read_Ops, 0, 0, pipe, 0, 0);
    *write_file = Allocate_File(&Pipe_Write_Ops, 0, 0, pipe, 0, 0);
    (*read_file)->refCount = 1;
    (*write_file)->refCount = 1;

    return 0;
}

int Pipe_Read(struct File *f, void *buf, ulong_t numBytes) {
    struct Pipe *pipe = (struct Pipe*)f->fsData;

    if (pipe->read_pos == pipe->write_pos) return pipe->writer ? EWOULDBLOCK: 0;
    
    ulong_t numBytes_copied = MIN(pipe->buffer_size, numBytes);

    memcpy(buf, pipe->buffer + pipe->read_pos, numBytes_copied);

    pipe->read_pos += numBytes_copied;
    pipe->buffer_size -= numBytes_copied;

    return numBytes_copied;
}

int Pipe_Write(struct File *f, void *buf, ulong_t numBytes) {
    // Print("Pipe_Write\n");
    ulong_t p;
    struct Pipe *pipe = (struct Pipe*)f->fsData;
    char* dst = (char *)pipe->buffer;
    char* src = (char *)buf;

    if (pipe->reader == 0) {
        // Print("Sending SIGPIPE %d\n", SIGPIPE);
        Send_Signal(CURRENT_THREAD, 6);
        return EPIPE;
    }
    if (numBytes + pipe->buffer_size > PIPE_BUFFER_CAPACITY) {
        return ENOMEM;
    }


    // Write to Pipe buffer
    for (p = 0; p < numBytes; p++) {
        dst[(pipe->write_pos + p) % PIPE_BUFFER_CAPACITY] = src[p];
    }

    pipe->buffer_size += numBytes;
    pipe->write_pos = (pipe->write_pos + numBytes) % PIPE_BUFFER_CAPACITY;
    return numBytes;
}

int Pipe_Close(struct File *f) {
    struct Pipe *pipe = f->fsData;

    if (!pipe->reader || !pipe->writer) return 0;
    
    if (f->refCount == 0) {
        if (f->ops->Read) {
            pipe->reader--;
            // if (!pipe->reader) Free(pipe->buffer);
        }
        else if (f->ops->Write) {
            pipe->writer--;
        }
    }
    
    // if (!pipe->reader && !pipe->writer) Free(pipe);
    return 0;
}
