/* This file is a placeholder for storing per-cpu variables in a segment that is not 
   saved and restored per thread, but rather left alone although different on a per-cpu
   basis. */

#ifndef PERCPU_H
#define PERCPU_H

#include <geekos/smp.h>

struct PerCPU_Data {
    int cpu_index;
    struct Kernel_Thread *current_thread;
};

extern struct PerCPU_Data percpu_data[MAX_CPUS];

void Init_PerCPU(int cpu);
int PerCPU_Get_CPU(void);
struct Kernel_Thread *PerCPU_Get_Current(void);

#endif // PERCPU_H