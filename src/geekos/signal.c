/*************************************************************************/
/*
 * GeekOS master source distribution and/or project solution
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
/*************************************************************************/
/*
 * Signals
 * $Rev $
 * 
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "COPYING".
 */

#include <geekos/kassert.h>
#include <geekos/defs.h>
#include <geekos/screen.h>
#include <geekos/int.h>
#include <geekos/mem.h>
#include <geekos/symbol.h>
#include <geekos/string.h>
#include <geekos/kthread.h>
#include <geekos/malloc.h>
#include <geekos/user.h>
#include <geekos/signal.h>
#include <geekos/projects.h>
#include <geekos/alarm.h>
#include <geekos/smp.h>
#include <geekos/errno.h>

void initProcess_Signals(struct Process_Signals* signals) {
    signals->handlingSignal = 0;
    signals->pendingSignal = 0;
    signals->returnSignal = NULL;
    signals->sigQueue.head = NULL;
    signals->sigQueue.tail = NULL;
    int i;
    for (i = 1; i <= MAXSIG; i++) {
        signals->handlers[i] = SIG_DFL;
    }
}
int isEmpty(struct Signal_Deque* deque) {
    return (deque->head == NULL);
}
void pushFront(struct Signal_Deque* deque, int signalNum) {
    Print("pushFront\n");
    struct Signal_Node *newNode = (struct Signal_Node*)Malloc(sizeof(struct Signal_Node));
    newNode->signalNum = signalNum;
    newNode->next = deque->head;
    newNode->prev = NULL;

    if (isEmpty(deque)) {
        deque->tail = newNode;
    } else {
        deque->head->prev = newNode;
    }
    deque->head = newNode;
}
void pushBack(struct Signal_Deque* deque, int signalNum) {
    Print("pushBack %d\n", signalNum);
    struct Signal_Node *newNode = (struct Signal_Node*)Malloc(sizeof(struct Signal_Node));
    newNode->signalNum = signalNum;
    newNode->next = NULL;
    newNode->prev = deque->tail;

    if (isEmpty(deque)) {
        deque->head = newNode;
    } else {
        deque->tail->next = newNode;
    }

    deque->tail = newNode;
}
int popFront(struct Signal_Deque* deque) {
    Print("popFront %d\n", deque->head->signalNum);
    if (isEmpty(deque)) {
        Print("Deque is empty, nothing to pop from the front.\n");
        return;
    }

    struct Signal_Node *temp = deque->head;
    int res = temp->signalNum;
    deque->head = deque->head->next;

    if (deque->head == NULL) {
        deque->tail = NULL;
    } else {
        deque->head->prev = NULL;
    }

    Free(temp);
    return res;
}
int popBack(struct Signal_Deque* deque) {
    Print("popBack\n");
    if (isEmpty(deque)) {
        Print("Deque is empty, nothing to pop from the rear.\n");
        while (1);
        return;
    }

    struct Signal_Node *temp = deque->tail;
    int res = temp->signalNum;
    deque->tail = deque->tail->prev;

    if (deque->tail == NULL) {
        deque->head = NULL;
    } else {
        deque->tail->next = NULL;
    }

    Free(temp);
    return res;
}
int getFrontNode(struct Signal_Deque* deque) {
    if (isEmpty(deque)) {
        Print("Deque is empty, no front element.\n");
        return -1;
    }
    return deque->head;
}
int getBackNode(struct Signal_Deque* deque) {
    if (isEmpty(deque)) {
        Print("Deque is empty, no rear element.\n");
        return -1;
    }
    return deque->tail;
}
int getNextNode(struct Signal_Node* node) {
    if (node == NULL) {
        Print("Node is NULL, no next element.\n");
        return -1;
    }
    return node->next;
}


/* Called when signal handling is complete. */
void Complete_Handler(struct Kernel_Thread *kthread,
                      struct Interrupt_State *state) {
    KASSERT(kthread);
    KASSERT(state);
    // TODO_P(PROJECT_SIGNALS,
    //        "Complete_Handler cleans up after a signal handler");

    struct User_Interrupt_State *userState = (struct User_Interrupt_State *) state;

    kthread->userContext->signals.handlingSignal = 0;
    
    // userState->espUser += sizeof(signal_handler);
    userState->espUser += sizeof(int);
    if(!Copy_From_User(state, userState->espUser, sizeof(struct Interrupt_State))) return EUNSPECIFIED;
    
    userState->espUser += sizeof(struct Interrupt_State);
}

int Check_Pending_Signal(struct Kernel_Thread *kthread,
                         struct Interrupt_State *state) {
    KASSERT(kthread);
    KASSERT(state);

    // if (kthread->userContext->signals.pendingSignal == 0) return 0;
    // if (state->cs == KERNEL_CS) return 0;
    // if (kthread->userContext->signals.currentSignal != 0) return 0;
    // return 1;

    if (kthread->userContext->signals.pendingSignal && (state->cs != KERNEL_CS) && !kthread->userContext->signals.handlingSignal) {
        return 1;
    }

    return 0;
}

void Send_Signal(struct Kernel_Thread *kthread, int signum) {
    struct Signal_Node *currSig = getFrontNode(
                                        &kthread->userContext->signals.sigQueue);
                                        
    Print("Sent Signal %d\n", signum);
    
    /* Checks for duplicates in the signal queue */
    while (currSig != 0) {
        if (currSig->signalNum == signum){
            return;
        }
        
        currSig = getNextNode(currSig);
    }

    Print("Recieved Signal %d\n", signum);

    /* Set the thread's signal pending flag */
    kthread->userContext->signals.pendingSignal = 1;
    
    /* If signal is SIGKILL then add to front of queue */
    if (signum == SIGKILL) {
        pushFront(&kthread->userContext->signals.sigQueue, signum);
    }
    /* Else just add to back of queue */
    else {
        pushBack(&kthread->userContext->signals.sigQueue, signum);
    }
}

#if 0
void Print_IS(struct Interrupt_State *esp) {
    void **p;
    Print("esp=%x:\n", (unsigned int)esp);
    Print("  gs=%x\n", (unsigned int)esp->gs);
    Print("  fs=%x\n", (unsigned int)esp->fs);
    Print("  es=%x\n", (unsigned int)esp->es);
    Print("  ds=%x\n", (unsigned int)esp->ds);
    Print("  ebp=%x\n", (unsigned int)esp->ebp);
    Print("  edi=%x\n", (unsigned int)esp->edi);
    Print("  esi=%x\n", (unsigned int)esp->esi);
    Print("  edx=%x\n", (unsigned int)esp->edx);
    Print("  ecx=%x\n", (unsigned int)esp->ecx);
    Print("  ebx=%x\n", (unsigned int)esp->ebx);
    Print("  eax=%x\n", (unsigned int)esp->eax);
    Print("  intNum=%x\n", (unsigned int)esp->intNum);
    Print("  errorCode=%x\n", (unsigned int)esp->errorCode);
    Print("  eip=%x\n", (unsigned int)esp->eip);
    Print("  cs=%x\n", (unsigned int)esp->cs);
    Print("  eflags=%x\n", (unsigned int)esp->eflags);
    p = (void **)(((struct Interrupt_State *)esp) + 1);
    Print("esp+n=%x\n", (unsigned int)p);
    Print("esp+n[0]=%x\n", (unsigned int)p[0]);
    Print("esp+n[1]=%x\n", (unsigned int)p[1]);
}

void dump_stack(unsigned int *esp, unsigned int ofs) {
    int i;
    Print("Setup_Frame: Stack dump\n");
    for(i = 0; i < 25; i++) {
        Print("[%x]: %x\n", (unsigned int)&esp[i] - ofs, esp[i]);
    }
}
#endif

void Setup_Frame(struct Kernel_Thread *kthread,
                 struct Interrupt_State *state) {
    KASSERT(kthread);
    KASSERT(state);

    // int signalNum = kthread->userContext->signals.pendingSignal;
    // void *signalHandler = kthread->userContext->signals.handlers[signalNum];

    // if (signalHandler == SIG_IGN) {
    //     return;
    // }

    // if (signalHandler == SIG_DFL) {
    //     if (signalNum == SIGKILL) {
    //         Exit(kthread);
    //     }
    //     return;
    // }

    // uint_t *userStack = (uint_t *)kthread->esp;

    // *(--userStack) = state->eflags;
    // *(--userStack) = state->cs;
    // *(--userStack) = state->eip;

    // *(--userStack) = state->errorCode;

    // *(--userStack) = state->intNum;

    // *(--userStack) = state->eax;
    // *(--userStack) = state->ebx;
    // *(--userStack) = state->ecx;
    // *(--userStack) = state->edx;
    // *(--userStack) = state->esi;
    // *(--userStack) = state->edi;
    // *(--userStack) = state->ebp;

    // *(--userStack) = state->ds;
    // *(--userStack) = state->es;
    // *(--userStack) = state->fs;
    // *(--userStack) = state->gs;

    // *(--userStack) = signalNum;
    
    // *(--userStack) = (uint_t)kthread->userContext->signals.returnSignal;

    // kthread->esp = (uint_t)userStack;

    // state->eip = (uint_t)signalHandler;

    Print("Setup_Frame\n");	

    kthread->userContext->signals.handlingSignal = 1;
    
    struct User_Interrupt_State *user = (struct User_Interrupt_State *) state;
    int signum = popFront(&kthread->userContext->signals.sigQueue);
    signal_handler sigHand = kthread->userContext->signals.handlers[signum];
    
    /* Checks if there are any more signals in the signal_queue */
    if (isEmpty(&kthread->userContext->signals.sigQueue))
        kthread->userContext->signals.pendingSignal = 0;

    /* Return if signal_handler is SIG_IGN */
    if (sigHand == SIG_IGN) {
        kthread->userContext->signals.handlingSignal = 0;
        return;
    } 
    /* Handle if signal_handler is SIG_DFL */ 
    else if (sigHand == SIG_DFL) {
        kthread->userContext->signals.handlingSignal = 0;
        if (signum == SIGCHLD) { 
            return;
        }
        
        Print("Terminated %d\n", CURRENT_THREAD->pid);
        Exit(0);
    } 
    /* Else set up the user and kernel stack */ 
    else {
        /* Push interrupt_state onto user stack */
        user->espUser -= sizeof(struct Interrupt_State); 
        if(!Copy_To_User(user->espUser, state, sizeof(struct Interrupt_State)))
            return EUNSPECIFIED;
         
        /* Push the signal number onto user stack */
        user->espUser -= sizeof(int); 
        if(!Copy_To_User(user->espUser, &signum, sizeof(int)))
            return EUNSPECIFIED;
           
        /* Push the signal trampoline onto user stack */
        user->espUser -= sizeof(signal_handler);
        if(!Copy_To_User(user->espUser, &kthread->userContext->signals.returnSignal, sizeof(signal_handler)))
            return EUNSPECIFIED;
        
        /* Change the kernel stack */
        state->eip = sigHand;
    }
}
