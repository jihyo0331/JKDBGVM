#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-misc-i386.h"
#include "qapi/qapi-types-misc-i386.h"
#include "hw/core/cpu.h"
#include "target/i386/cpu.h"

void cpu_synchronize_state(CPUState *cpu);

static X86Regs *fill_regs_from_env(CPUX86State *env)
{
    X86Regs *r = g_new0(X86Regs, 1);

    r->rax = env->regs[R_EAX];
    r->rbx = env->regs[R_EBX];
    r->rcx = env->regs[R_ECX];
    r->rdx = env->regs[R_EDX];
    r->rsi = env->regs[R_ESI];
    r->rdi = env->regs[R_EDI];
    r->rbp = env->regs[R_EBP];
    r->rsp = env->regs[R_ESP];
    r->r8  = env->regs[8];
    r->r9  = env->regs[9];
    r->r10 = env->regs[10];
    r->r11 = env->regs[11];
    r->r12 = env->regs[12];
    r->r13 = env->regs[13];
    r->r14 = env->regs[14];
    r->r15 = env->regs[15];

    r->rip    = (uint64_t)env->eip;
    r->rflags = (uint64_t)env->eflags;

    r->cs = env->segs[R_CS].selector & 0xFFFF;
    r->ds = env->segs[R_DS].selector & 0xFFFF;
    r->es = env->segs[R_ES].selector & 0xFFFF;
    r->fs = env->segs[R_FS].selector & 0xFFFF;
    r->gs = env->segs[R_GS].selector & 0xFFFF;
    r->ss = env->segs[R_SS].selector & 0xFFFF;

    r->cr0 = env->cr[0];
    r->cr2 = env->cr[2];
    r->cr3 = env->cr[3];
    r->cr4 = env->cr[4];
    r->cr8 = 0;

    r->dr0 = env->dr[0];
    r->dr1 = env->dr[1];
    r->dr2 = env->dr[2];
    r->dr3 = env->dr[3];
    r->dr6 = env->dr[6];
    r->dr7 = env->dr[7];

    return r;
}

X86Regs *qmp_x86_info_register(Error **errp)
{
    CPUState *cs = current_cpu;
    if (!cs) {
        CPU_FOREACH(cs) { break; }
    }
    if (!cs) {
        error_setg(errp, "no CPU available");
        return NULL;
    }

    cpu_synchronize_state(cs);

    X86CPU *xcpu = X86_CPU(cs);
    return fill_regs_from_env(&xcpu->env);
}
