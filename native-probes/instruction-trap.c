#define _QNX_SOURCE 1
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef PROBE_MODE
#define PROBE_MODE "unknown"
#endif

static volatile sig_atomic_t trap_count;
static volatile sig_atomic_t trap_code;
static volatile uintptr_t saved_pc;
static volatile uint32_t saved_spsr;
static volatile uintptr_t resume_pc;

extern char linuxemu_fault_instruction[];
extern char linuxemu_resume_instruction[];

static void trap_handler(int sig, siginfo_t *info, void *argument)
{
    ucontext_t *context = argument;

    if (sig != SIGILL || trap_count != 0) {
        _exit(120);
    }

    trap_count = 1;
    trap_code = info != 0 ? info->si_code : 0;
    saved_pc = context->uc_mcontext.cpu.gpr[15];
    saved_spsr = context->uc_mcontext.cpu.spsr;
    context->uc_mcontext.cpu.gpr[15] = (uint32_t)(resume_pc & ~(uintptr_t)1);
}

int main(void)
{
    struct sigaction action;
    uintptr_t fault_pc;
    intptr_t pc_delta;

    alarm(10);
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = trap_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGILL, &action, 0) != 0) {
        perror("sigaction");
        return 1;
    }

    fault_pc = (uintptr_t)linuxemu_fault_instruction;
    resume_pc = (uintptr_t)linuxemu_resume_instruction;
#if defined(PROBE_THUMB)
    __asm__ volatile (
        ".global linuxemu_fault_instruction\n"
        "linuxemu_fault_instruction:\n"
        ".short 0xde00\n"
        ".global linuxemu_resume_instruction\n"
        "linuxemu_resume_instruction:\n");
#else
    __asm__ volatile (
        ".global linuxemu_fault_instruction\n"
        "linuxemu_fault_instruction:\n"
        ".word 0xe7f000f0\n"
        ".global linuxemu_resume_instruction\n"
        "linuxemu_resume_instruction:\n");
#endif
    pc_delta = (intptr_t)saved_pc - (intptr_t)(fault_pc & ~(uintptr_t)1);
    printf("mode=%s trap_count=%d si_code=%d fault=%p saved_pc=%p "
           "resume=%p pc_delta=%ld spsr=0x%08lx thumb_state=%lu\n",
        PROBE_MODE, (int)trap_count, (int)trap_code,
        (void *)fault_pc, (void *)saved_pc, (void *)resume_pc,
        (long)pc_delta, (unsigned long)saved_spsr,
        (unsigned long)((saved_spsr >> 5) & 1));
    printf("instruction_trap=%s\n", trap_count == 1 ? "PASS" : "FAIL");
    return trap_count == 1 ? 0 : 1;
}
