/*
 * System call handling.
 *
 * Copyright (C) 1997 Massachusetts Institute of Technology
 * See section "MIT License" in the file LICENSES for licensing terms.
 *
 * Derived from the xv6 instructional operating system from MIT.
 * Adapted for PIOS by Bryan Ford at Yale University.
 */

#include <inc/x86.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/trap.h>
#include <inc/syscall.h>

#include <kern/cpu.h>
#include <kern/trap.h>
#include <kern/proc.h>
#include <kern/syscall.h>



static void gcc_noreturn do_ret(trapframe *tf);

// This bit mask defines the eflags bits user code is allowed to set.
#define FL_USER		(FL_CF|FL_PF|FL_AF|FL_ZF|FL_SF|FL_DF|FL_OF)


// During a system call, generate a specific processor trap -
// as if the user code's INT 0x30 instruction had caused it -
// and reflect the trap to the parent process as with other traps.
static void gcc_noreturn
systrap(trapframe *utf, int trapno, int err)
{
	utf->trapno = trapno;
	utf->err = err;
	proc_ret(utf,0);
} 



// Recover from a trap that occurs during a copyin or copyout,
// by aborting the system call and reflecting the trap to the parent process,
// behaving as if the user program's INT instruction had caused the trap.
// This uses the 'recover' pointer in the current cpu struct,
// and invokes systrap() above to blame the trap on the user process.
//
// Notes:
// - Be sure the parent gets the correct trapno, err, and eip values.
// - Be sure to release any spinlocks you were holding during the copyin/out.
//
static void gcc_noreturn
sysrecover(trapframe *ktf, void *recoverdata)
{
	trapframe *utf = (trapframe*)recoverdata;
	cpu *c = cpu_cur();
	assert(c->recover == sysrecover);
	c->recover = NULL;
	systrap(utf, ktf->trapno, ktf->err);
}

// Check a user virtual address block for validity:
// i.e., make sure the complete area specified lies in
// the user address space between VM_USERLO and VM_USERHI.
// If not, abort the syscall by sending a T_GPFLT to the parent,
// again as if the user program's INT instruction was to blame.
//
// Note: Be careful that your arithmetic works correctly
// even if size is very large, e.g., if uva+size wraps around!
//
static void checkva(trapframe *utf, uint32_t uva, size_t size)
{
	if(uva < VM_USERLO || uva >= VM_USERHI || size >= VM_USERHI -uva)
		systrap(utf, T_PGFLT, 0);
}

// Copy data to/from user space,
// using checkva() above to validate the address range
// and using sysrecover() to recover from any traps during the copy.
void usercopy(trapframe *utf, bool copyout, void *kva, uint32_t uva, size_t size)
{
	checkva(utf, uva, size);
	cpu *c = cpu_cur();
	assert(c->recover == NULL);
	c->recover = sysrecover;

	if(copyout)
		memmove((void*)uva, kva, size);
	else
		memmove(kva, (void*)uva, size);

	assert(c->recover == sysrecover);
	c->recover = NULL;
}

static void
do_cputs(trapframe *tf, uint32_t cmd)
{
	// Print the string supplied by the user: pointer in EBX
char buf[CPUTS_MAX+1];
usercopy(tf,0,buf,tf->regs.ebx,CPUTS_MAX);
buf[CPUTS_MAX] = 0;
cprintf("%s",buf);
	trap_return(tf);	// syscall completed
}
static void
do_put(trapframe *tf, uint32_t cmd)
{
	proc *p = proc_cur();
	assert(p->state == PROC_RUN && p->runcpu == cpu_cur());
//cprintf("PUT proc %x eip %x esp %x cmd %x\n", p, tf->eip, tf->esp, cmd);

	spinlock_acquire(&p->lock);

	// Find the named child process; create if it doesn't exist
	uint32_t cn = tf->regs.edx & 0xff;
	proc *cp = p->child[cn];
	if (!cp) {
		cp = proc_alloc(p, cn);
		if (!cp)	// XX handle more gracefully
			panic("sys_put: no memory for child");
	}

	// Synchronize with child if necessary.
	if (cp->state != PROC_STOP)
		proc_wait(p, cp, tf);

	// Since the child is now stopped, it's ours to control;
	// we no longer need our process lock -
	// and we don't want to be holding it if usercopy() below aborts.
	spinlock_release(&p->lock);

	// Put child's general register state
	if (cmd & SYS_REGS) {
		int len = offsetof(procstate, fx);  // just integer regs
		if (cmd & SYS_FPU) len = sizeof(procstate); // whole shebang

		usercopy(tf,0,&cp->sv, tf->regs.ebx, len);
		// Copy user's trapframe into child process
		procstate *cs = (procstate*) tf->regs.ebx;
		memcpy(&cp->sv, cs, len);

		// Make sure process uses user-mode segments and eflag settings
		cp->sv.tf.ds = CPU_GDT_UDATA | 3;
		cp->sv.tf.es = CPU_GDT_UDATA | 3;
		cp->sv.tf.cs = CPU_GDT_UCODE | 3;
		cp->sv.tf.ss = CPU_GDT_UDATA | 3;
		cp->sv.tf.eflags &= FL_USER;
		cp->sv.tf.eflags |= FL_IF;  // enable interrupts
	}
	uint32_t sva = tf->regs.esi;
	uint32_t dva = tf->regs.edi;
	uint32_t size = tf->regs.ecx;
	switch (cmd & SYS_MEMOP) {
		case 0:	// no memory operation
			break;
		case SYS_COPY:
			// validate source region
			if (PTOFF(sva) || PTOFF(size)
					|| sva < VM_USERLO || sva > VM_USERHI
					|| size > VM_USERHI-sva)
				systrap(tf, T_GPFLT, 0);
			// fall thru...
		case SYS_ZERO:
			// validate destination region
			if (PTOFF(dva) || PTOFF(size)
					|| dva < VM_USERLO || dva > VM_USERHI
					|| size > VM_USERHI-dva)
				systrap(tf, T_GPFLT, 0);

			switch (cmd & SYS_MEMOP) {
				case SYS_ZERO:	// zero memory and clear permissions
					pmap_remove(cp->pdir, dva, size);
					break;
				case SYS_COPY:	// copy from local src to dest in child
					pmap_copy(p->pdir, sva, cp->pdir, dva, size);
					break;
			}
			break;
		default:
			systrap(tf, T_GPFLT, 0);
	}

	if (cmd & SYS_PERM) {
		// validate destination region
		if (PGOFF(dva) || PGOFF(size)
				|| dva < VM_USERLO || dva > VM_USERHI
				|| size > VM_USERHI-dva)
			systrap(tf, T_GPFLT, 0);
		if (!pmap_setperm(cp->pdir, dva, size, cmd & SYS_RW))
			panic("pmap_put: no memory to set permissions");
	}

	if (cmd & SYS_SNAP)	// Snapshot child's state
		pmap_copy(cp->pdir, VM_USERLO, cp->rpdir, VM_USERLO,
				VM_USERHI-VM_USERLO);

	// Start the child if requested
	if (cmd & SYS_START)
		proc_ready(cp);

	trap_return(tf);  // syscall completed
}

  static void
do_get(trapframe *tf, uint32_t cmd)
{
  proc *p = proc_cur();
  assert(p->state == PROC_RUN && p->runcpu == cpu_cur());
  //cprintf("GET proc %x eip %x esp %x cmd %x\n", p, tf->eip, tf->esp, cmd);

  spinlock_acquire(&p->lock);

  // Find the named child process; DON'T create if it doesn't exist
  uint32_t cn = tf->regs.edx & 0xff;
  proc *cp = p->child[cn];
  if (!cp)
    cp = &proc_null;

  // Synchronize with child if necessary.
  if (cp->state != PROC_STOP)
    proc_wait(p, cp, tf);

  // Since the child is now stopped, it's ours to control;
  // we no longer need our process lock -
  // and we don't want to be holding it if usercopy() below aborts.
  spinlock_release(&p->lock);

  // Get child's general register state
  if (cmd & SYS_REGS) {
    int len = offsetof(procstate, fx);  // just integer regs
    if (cmd & SYS_FPU) len = sizeof(procstate); // whole shebang
usercopy(tf, 1, &cp->sv, tf->regs.ebx, len);
    // Copy child process's trapframe into user space
    procstate *cs = (procstate*) tf->regs.ebx;
    memcpy(cs, &cp->sv, len);
  }
uint32_t sva = tf->regs.esi;
	uint32_t dva = tf->regs.edi;
	uint32_t size = tf->regs.ecx;
	switch (cmd & SYS_MEMOP) {
	case 0:	// no memory operation
		break;
	case SYS_COPY:
	case SYS_MERGE:
		// validate source region
		if (PTOFF(sva) || PTOFF(size)
				|| sva < VM_USERLO || sva > VM_USERHI
				|| size > VM_USERHI-sva)
			systrap(tf, T_GPFLT, 0);
		// fall thru...
	case SYS_ZERO:
		// validate destination region
		if (PTOFF(dva) || PTOFF(size)
				|| dva < VM_USERLO || dva > VM_USERHI
				|| size > VM_USERHI-dva)
			systrap(tf, T_GPFLT, 0);

		switch (cmd & SYS_MEMOP) {
		case SYS_ZERO:	// zero memory and clear permissions
			pmap_remove(p->pdir, dva, size);
			break;
		case SYS_COPY:	// copy from local src to dest in child
			pmap_copy(cp->pdir, sva, p->pdir, dva, size);
			break;
		case SYS_MERGE:	// merge from local src to dest in child
			pmap_merge(cp->rpdir, cp->pdir, sva,
					p->pdir, dva, size);
			break;
		}
		break;
	default:
		systrap(tf, T_GPFLT, 0);
	}

	if (cmd & SYS_PERM) {
		// validate destination region
		if (PGOFF(dva) || PGOFF(size)
				|| dva < VM_USERLO || dva > VM_USERHI
				|| size > VM_USERHI-dva)
			systrap(tf, T_GPFLT, 0);
		if (!pmap_setperm(p->pdir, dva, size, cmd & SYS_RW))
			panic("pmap_get: no memory to set permissions");
	}

	if (cmd & SYS_SNAP)
		systrap(tf, T_GPFLT, 0);	// only valid for PUT
  trap_return(tf);  // syscall completed
}

static void gcc_noreturn
do_ret(trapframe *tf)
{
//cprintf("RET proc %x eip %x esp %x\n", proc_cur(), tf->eip, tf->esp);
	proc_ret(tf, 1);	// Complete syscall insn and return to parent
}


// Common function to handle all system calls -
// decode the system call type and call an appropriate handler function.
// Be sure to handle undefined system calls appropriately.
void
syscall(trapframe *tf)
{
	// EAX register holds system call command/flags
	uint32_t cmd = tf->regs.eax;
	switch (cmd & SYS_TYPE) {
	case SYS_CPUTS:	return do_cputs(tf, cmd);
	case SYS_PUT:	return do_put(tf, cmd);
	case SYS_GET:	return do_get(tf, cmd);
	case SYS_RET:	return do_ret(tf);
	default:	return;		// handle as a regular trap
	}
}

