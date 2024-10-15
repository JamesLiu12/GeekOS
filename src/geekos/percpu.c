#include <geekos/percpu.h>
#include <geekos/kthread.h>

struct PerCPU_Data percpu_data[MAX_CPUS];

void Init_PerCPU(int cpu) {
    if (cpu >= 0 && cpu < MAX_CPUS) {
        percpu_data[cpu].cpu_index = cpu;
        percpu_data[cpu].current_thread = NULL;
    }
}

int PerCPU_Get_CPU(void) {
    int cpu_id;
    asm("movl %%gs:0, %0" : "=r"(cpu_id));
    return cpu_id;
}

struct Kernel_Thread *PerCPU_Get_Current(void) {
    struct Kernel_Thread *kthread;
    asm("movl %%gs:4, %0" 
        : "=r"(kthread) 
    );
    return kthread;
}