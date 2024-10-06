/*************************************************************************/
/*
 * GeekOS master source distribution and/or project solution
 * Copyright (c) 2005 Michael Hicks <mwh@cs.umd.edu>
 * Copyright (c) 2014 Jeffrey K. Hollingsworth <hollings@cs.umd.edu>
 *
 * All rights reserved.
 *
 * This code may not be resdistributed without the permission of the copyright holders.
 * Any student solutions using any of this code base constitute derviced work and may
 * not be redistributed in any form.  This includes (but is not limited to) posting on
 * public forums or web sites, providing copies to (past, present, or future) students
 * enrolled in similar operating systems courses the University of Maryland's CMSC412 course.
 */
/*************************************************************************/

#ifndef GEEKOS_SIGNAL_H
#define GEEKOS_SIGNAL_H

/* Signal numbers */
#define SIGKILL  1              /* can't be handled by users */
#define SIGUSR1  2
#define SIGUSR2  3
#define SIGCHLD  4
#define SIGALARM  5
#define SIGPIPE  6

/* The largest signal number supported */
#define MAXSIG   6

/* Macro to determine whether a number is a valid signal number */
#define IS_SIGNUM(n) (((n) > 0) && ((n) <= MAXSIG))

/* Definition of a signal handler */
typedef void (*signal_handler) (int);

struct Signal_Node {
    int signalNum;
    struct Signal_Node *next;
    struct Signal_Node *prev;
};

struct Signal_Deque {
    struct Signal_Node *head;
    struct Signal_Node *tail;
};

struct Process_Signals {
    signal_handler handlers[MAXSIG + 1];
    int handlingSignal;
    signal_handler returnSignal;
    int pendingSignal;
    // int saved_gs;
    // int saved_fs;
    // int saved_es;
    // int saved_ds;
    // int saved_ebp;
    // int saved_edi;
    // int saved_esi;
    // int saved_edx;
    // int saved_ecx;
    // int saved_ebx;
    // int saved_eax;
    // int saved_eip;
    // int saved_cs;
    // int saved_eflags;
    struct Signal_Deque sigQueue;
};

void initProcess_Signals(struct Process_Signals* signals);
int isEmpty(struct Signal_Deque* deque);
void pushFront(struct Signal_Deque* deque, int data);
void pushBack(struct Signal_Deque* deque, int data);
int popFront(struct Signal_Deque* deque);
int popBack(struct Signal_Deque* deque);

void Send_Signal(struct Kernel_Thread *kthread, int signum);


/* Default handlers */
#define SIG_DFL  (signal_handler)1
#define SIG_IGN  (signal_handler)2

#ifdef GEEKOS

struct Interrupt_State;
struct Kernel_Thread;

int Check_Pending_Signal(struct Kernel_Thread *kthread,
                         struct Interrupt_State *esp);
void Complete_Handler(struct Kernel_Thread *kthread,
                      struct Interrupt_State *esp);

#endif

#endif
