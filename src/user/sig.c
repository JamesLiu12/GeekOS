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

#include <conio.h>
#include <process.h>
#include <sched.h>
#include <sema.h>
#include <string.h>
#include <fileio.h>
#include <signal.h>
#include <geekos/errno.h>

int global = 0;                 /* remnant of basic fork testing */
int sigpiped = 0;               /* flag to be set when the handler is invoked */
char bigbuf[2048];              /* spot in which to read data */

/* invoked when sigpipe is delivered. */
void sig_handler(int signal) {
    Print("sig_handler\n");
}

int main() {
    Signal(sig_handler, SIGPIPE);
    int child_pid = Fork();
    return 0;
}
