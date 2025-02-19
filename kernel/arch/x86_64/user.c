/**
 * @file  kernel/arch/x86_64/user.c
 * @brief Various assembly snippets for jumping to usermode and back.
 *
 * @copyright
 * This file is part of ToaruOS and is released under the terms
 * of the NCSA / University of Illinois License - see LICENSE.md
 * Copyright (C) 2021 K. Lange
 */
#include <stdint.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/arch/x86_64/regs.h>
#include <kernel/arch/x86_64/mmu.h>
#include <kernel/arch/x86_64/ports.h>

/**
 * @brief Enter userspace.
 *
 * Called by process startup.
 * Does not return.
 *
 * @param entrypoint Address to "return" to in userspace.
 * @param argc       Number of arguments to provide to the new process.
 * @param argv       Argument array to pass to the new process; make sure this is user-accessible!
 * @param envp       Environment strings array
 * @param stack      Userspace stack address.
 */
void arch_enter_user(uintptr_t entrypoint, int argc, char * argv[], char * envp[], uintptr_t stack) {
	struct regs ret;
	ret.cs = 0x18 | 0x03;
	ret.ss = 0x20 | 0x03;
	ret.rip = entrypoint;
	ret.rflags = (1 << 21) | (1 << 9);
	ret.rsp = stack;

	asm volatile (
		"pushq %0\n"
		"pushq %1\n"
		"pushq %2\n"
		"pushq %3\n"
		"pushq %4\n"
		"swapgs\n"
		"iretq"
	: : "m"(ret.ss), "m"(ret.rsp), "m"(ret.rflags), "m"(ret.cs), "m"(ret.rip),
	    "D"(argc), "S"(argv), "d"(envp));
}

/**
 * @brief Enter a userspace signal handler.
 *
 * Similar to @c arch_enter_user but also setups up magic return addresses.
 *
 * Since signal handlers do to take complicated argument arrays, this only
 * supplies a @p signum argument.
 *
 * Does not return.
 *
 * @param entrypoint Userspace address of the signal handler, set by the process.
 * @param signum     Signal number that caused this entry.
 */
void arch_enter_signal_handler(uintptr_t entrypoint, int signum) {
	struct regs ret;
	ret.cs = 0x18 | 0x03;
	ret.ss = 0x20 | 0x03;
	ret.rip = entrypoint;
	ret.rflags = (1 << 21) | (1 << 9);
	ret.rsp = (this_core->current_process->syscall_registers->rsp - 128 - 8) & 0xFFFFFFFFFFFFFFF0; /* ensure considerable alignment */
	*(uintptr_t*)ret.rsp = 0x00000008DEADBEEF; /* arbitrarily chosen stack return sentinel IP */

	asm volatile(
		"pushq %0\n"
		"pushq %1\n"
		"pushq %2\n"
		"pushq %3\n"
		"pushq %4\n"
		"swapgs\n"
		"iretq"
	: : "m"(ret.ss), "m"(ret.rsp), "m"(ret.rflags), "m"(ret.cs), "m"(ret.rip),
	    "D"(signum));
	__builtin_unreachable();
}

/**
 * @brief Return from fork or clone.
 *
 * This is what we inject as the stored rip for a new thread,
 * so that it immediately returns from the system call.
 *
 * This is never called as a function, its address is stored
 * in the thread context of a new @c process_t.
 */
__attribute__((naked))
void arch_resume_user(void) {
	asm volatile (
		"pop %r15\n"
		"pop %r14\n"
		"pop %r13\n"
		"pop %r12\n"
		"pop %r11\n"
		"pop %r10\n"
		"pop %r9\n"
		"pop %r8\n"
		"pop %rbp\n"
		"pop %rdi\n"
		"pop %rsi\n"
		"pop %rdx\n"
		"pop %rcx\n"
		"pop %rbx\n"
		"pop %rax\n"
		"add $16, %rsp\n"
		"swapgs\n"
		"iretq\n"
	);
	__builtin_unreachable();
}

/**
 * @brief Save FPU registers for this thread.
 */
void arch_restore_floating(process_t * proc) {
	asm volatile ("fxrstor (%0)" :: "r"(&proc->thread.fp_regs));
}

/**
 * @brief Restore FPU registers for this thread.
 */
void arch_save_floating(process_t * proc) {
	asm volatile ("fxsave (%0)" :: "r"(&proc->thread.fp_regs));
}

/**
 * @brief Called in a loop by kernel idle tasks.
 *
 * Turns on and waits for interrupts.
 * There is room for improvement here with other power states,
 * but HLT is "good enough" for us.
 */
void arch_pause(void) {
	asm volatile (
		"sti\n"
		"hlt\n"
		"cli\n"
	);
}

extern void lapic_send_ipi(int i, uint32_t val);

/**
 * @brief Prepare for a fatal event by stopping all other cores.
 *
 * Sends an IPI to all other CPUs to tell them to immediately stop.
 * This causes an NMI (isr2), which disables interrupts and loops
 * on a hlt instruction.
 *
 * Ensures that we can then print tracebacks and do other complicated
 * things without having to mess with locks, and without other
 * processors causing further damage in the case of a fatal error.
 */
void arch_fatal_prepare(void) {
	for (int i = 0; i < processor_count; ++i) {
		if (i == this_core->cpu_id) continue;
		lapic_send_ipi(processor_local_data[i].lapic_id, 0x447D);
	}
}

/**
 * @brief Halt all processors, including this one.
 * @see arch_fatal_prepare
 */
void arch_fatal(void) {
	arch_fatal_prepare();
	while (1) {
		asm volatile (
			"cli\n"
			"hlt\n"
		);
	}
}

/**
 * @brief Reboot the computer.
 *
 * This tries to do a "keyboard reset". We clear out the IDT
 * so that we can maybe triple fault, and then we try to use
 * the keyboard reset vector... if that doesn't work,
 * then returning from this and letting anything else happen
 * almost certainly will.
 */
long arch_reboot(void) {
	/* load a null page as an IDT */
	uintptr_t frame = mmu_allocate_a_frame();
	uintptr_t * idt = mmu_map_from_physical(frame << 12);
	memset(idt, 0, 0x1000);
	asm volatile (
		"lidt (%0)"
		: : "r"(idt)
	);
	uint8_t out = 0x02;
	while ((out & 0x02) != 0) {
		out = inportb(0x64);
	}
	outportb(0x64, 0xFE); /* Reset */
	return 0;
}

/* Syscall parameter accessors */
void arch_syscall_return(struct regs * r, long retval) { r->rax = retval; }
long arch_syscall_number(struct regs * r) { return (unsigned long)r->rax; }
long arch_syscall_arg0(struct regs * r) { return r->rbx; }
long arch_syscall_arg1(struct regs * r) { return r->rcx; }
long arch_syscall_arg2(struct regs * r) { return r->rdx; }
long arch_syscall_arg3(struct regs * r) { return r->rsi; }
long arch_syscall_arg4(struct regs * r) { return r->rdi; }
long arch_stack_pointer(struct regs * r) { return r->rsp; }
long arch_user_ip(struct regs * r) { return r->rip; }
