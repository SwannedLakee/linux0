// SPDX-License-Identifier: GPL-2.0

#include <linux/context_tracking.h>
#include <linux/entry-common.h>
#include <linux/resume_user_mode.h>
#include <linux/highmem.h>
#include <linux/jump_label.h>
#include <linux/kmsan.h>
#include <linux/livepatch.h>
#include <linux/audit.h>
#include <linux/tick.h>

#include "common.h"

#define CREATE_TRACE_POINTS
#include <trace/events/syscalls.h>

static inline void syscall_enter_audit(struct pt_regs *regs, long syscall)
{
	if (unlikely(audit_context())) {
		unsigned long args[6];

		syscall_get_arguments(current, regs, args);
		audit_syscall_entry(syscall, args[0], args[1], args[2], args[3]);
	}
}

long syscall_trace_enter(struct pt_regs *regs, long syscall,
				unsigned long work)
{
	long ret = 0;

	/*
	 * Handle Syscall User Dispatch.  This must comes first, since
	 * the ABI here can be something that doesn't make sense for
	 * other syscall_work features.
	 */
	if (work & SYSCALL_WORK_SYSCALL_USER_DISPATCH) {
		if (syscall_user_dispatch(regs))
			return -1L;
	}

	/* Handle ptrace */
	if (work & (SYSCALL_WORK_SYSCALL_TRACE | SYSCALL_WORK_SYSCALL_EMU)) {
		ret = ptrace_report_syscall_entry(regs);
		if (ret || (work & SYSCALL_WORK_SYSCALL_EMU))
			return -1L;
	}

	/* Do seccomp after ptrace, to catch any tracer changes. */
	if (work & SYSCALL_WORK_SECCOMP) {
		ret = __secure_computing();
		if (ret == -1L)
			return ret;
	}

	/* Either of the above might have changed the syscall number */
	syscall = syscall_get_nr(current, regs);

	if (unlikely(work & SYSCALL_WORK_SYSCALL_TRACEPOINT)) {
		trace_sys_enter(regs, syscall);
		/*
		 * Probes or BPF hooks in the tracepoint may have changed the
		 * system call number as well.
		 */
		syscall = syscall_get_nr(current, regs);
	}

	syscall_enter_audit(regs, syscall);

	return ret ? : syscall;
}

noinstr void syscall_enter_from_user_mode_prepare(struct pt_regs *regs)
{
	enter_from_user_mode(regs);
	instrumentation_begin();
	local_irq_enable();
	instrumentation_end();
}

/* Workaround to allow gradual conversion of architecture code */
void __weak arch_do_signal_or_restart(struct pt_regs *regs) { }

/**
 * exit_to_user_mode_loop - do any pending work before leaving to user space
 * @regs:	Pointer to pt_regs on entry stack
 * @ti_work:	TIF work flags as read by the caller
 */
__always_inline unsigned long exit_to_user_mode_loop(struct pt_regs *regs,
						     unsigned long ti_work)
{
	/*
	 * Before returning to user space ensure that all pending work
	 * items have been completed.
	 */
	while (ti_work & EXIT_TO_USER_MODE_WORK) {

		local_irq_enable_exit_to_user(ti_work);

		if (ti_work & (_TIF_NEED_RESCHED | _TIF_NEED_RESCHED_LAZY))
			schedule();

		if (ti_work & _TIF_UPROBE)
			uprobe_notify_resume(regs);

		if (ti_work & _TIF_PATCH_PENDING)
			klp_update_patch_state(current);

		if (ti_work & (_TIF_SIGPENDING | _TIF_NOTIFY_SIGNAL))
			arch_do_signal_or_restart(regs);

		if (ti_work & _TIF_NOTIFY_RESUME)
			resume_user_mode_work(regs);

		/* Architecture specific TIF work */
		arch_exit_to_user_mode_work(regs, ti_work);

		/*
		 * Disable interrupts and reevaluate the work flags as they
		 * might have changed while interrupts and preemption was
		 * enabled above.
		 */
		local_irq_disable_exit_to_user();

		/* Check if any of the above work has queued a deferred wakeup */
		tick_nohz_user_enter_prepare();

		ti_work = read_thread_flags();
	}

	/* Return the latest work state for arch_exit_to_user_mode() */
	return ti_work;
}

/*
 * If SYSCALL_EMU is set, then the only reason to report is when
 * SINGLESTEP is set (i.e. PTRACE_SYSEMU_SINGLESTEP).  This syscall
 * instruction has been already reported in syscall_enter_from_user_mode().
 */
static inline bool report_single_step(unsigned long work)
{
	if (work & SYSCALL_WORK_SYSCALL_EMU)
		return false;

	return work & SYSCALL_WORK_SYSCALL_EXIT_TRAP;
}

void syscall_exit_work(struct pt_regs *regs, unsigned long work)
{
	bool step;

	/*
	 * If the syscall was rolled back due to syscall user dispatching,
	 * then the tracers below are not invoked for the same reason as
	 * the entry side was not invoked in syscall_trace_enter(): The ABI
	 * of these syscalls is unknown.
	 */
	if (work & SYSCALL_WORK_SYSCALL_USER_DISPATCH) {
		if (unlikely(current->syscall_dispatch.on_dispatch)) {
			current->syscall_dispatch.on_dispatch = false;
			return;
		}
	}

	audit_syscall_exit(regs);

	if (work & SYSCALL_WORK_SYSCALL_TRACEPOINT)
		trace_sys_exit(regs, syscall_get_return_value(current, regs));

	step = report_single_step(work);
	if (step || work & SYSCALL_WORK_SYSCALL_TRACE)
		ptrace_report_syscall_exit(regs, step);
}

noinstr void irqentry_enter_from_user_mode(struct pt_regs *regs)
{
	enter_from_user_mode(regs);
}

noinstr void irqentry_exit_to_user_mode(struct pt_regs *regs)
{
	instrumentation_begin();
	exit_to_user_mode_prepare(regs);
	instrumentation_end();
	exit_to_user_mode();
}

noinstr irqentry_state_t irqentry_enter(struct pt_regs *regs)
{
	irqentry_state_t ret = {
		.exit_rcu = false,
	};

	if (user_mode(regs)) {
		irqentry_enter_from_user_mode(regs);
		return ret;
	}

	/*
	 * If this entry hit the idle task invoke ct_irq_enter() whether
	 * RCU is watching or not.
	 *
	 * Interrupts can nest when the first interrupt invokes softirq
	 * processing on return which enables interrupts.
	 *
	 * Scheduler ticks in the idle task can mark quiescent state and
	 * terminate a grace period, if and only if the timer interrupt is
	 * not nested into another interrupt.
	 *
	 * Checking for rcu_is_watching() here would prevent the nesting
	 * interrupt to invoke ct_irq_enter(). If that nested interrupt is
	 * the tick then rcu_flavor_sched_clock_irq() would wrongfully
	 * assume that it is the first interrupt and eventually claim
	 * quiescent state and end grace periods prematurely.
	 *
	 * Unconditionally invoke ct_irq_enter() so RCU state stays
	 * consistent.
	 *
	 * TINY_RCU does not support EQS, so let the compiler eliminate
	 * this part when enabled.
	 */
	if (!IS_ENABLED(CONFIG_TINY_RCU) && is_idle_task(current)) {
		/*
		 * If RCU is not watching then the same careful
		 * sequence vs. lockdep and tracing is required
		 * as in irqentry_enter_from_user_mode().
		 */
		lockdep_hardirqs_off(CALLER_ADDR0);
		ct_irq_enter();
		instrumentation_begin();
		kmsan_unpoison_entry_regs(regs);
		trace_hardirqs_off_finish();
		instrumentation_end();

		ret.exit_rcu = true;
		return ret;
	}

	/*
	 * If RCU is watching then RCU only wants to check whether it needs
	 * to restart the tick in NOHZ mode. rcu_irq_enter_check_tick()
	 * already contains a warning when RCU is not watching, so no point
	 * in having another one here.
	 */
	lockdep_hardirqs_off(CALLER_ADDR0);
	instrumentation_begin();
	kmsan_unpoison_entry_regs(regs);
	rcu_irq_enter_check_tick();
	trace_hardirqs_off_finish();
	instrumentation_end();

	return ret;
}

void raw_irqentry_exit_cond_resched(void)
{
	if (!preempt_count()) {
		/* Sanity check RCU and thread stack */
		rcu_irq_exit_check_preempt();
		if (IS_ENABLED(CONFIG_DEBUG_ENTRY))
			WARN_ON_ONCE(!on_thread_stack());
		if (need_resched())
			preempt_schedule_irq();
	}
}
#ifdef CONFIG_PREEMPT_DYNAMIC
#if defined(CONFIG_HAVE_PREEMPT_DYNAMIC_CALL)
DEFINE_STATIC_CALL(irqentry_exit_cond_resched, raw_irqentry_exit_cond_resched);
#elif defined(CONFIG_HAVE_PREEMPT_DYNAMIC_KEY)
DEFINE_STATIC_KEY_TRUE(sk_dynamic_irqentry_exit_cond_resched);
void dynamic_irqentry_exit_cond_resched(void)
{
	if (!static_branch_unlikely(&sk_dynamic_irqentry_exit_cond_resched))
		return;
	raw_irqentry_exit_cond_resched();
}
#endif
#endif

noinstr void irqentry_exit(struct pt_regs *regs, irqentry_state_t state)
{
	lockdep_assert_irqs_disabled();

	/* Check whether this returns to user mode */
	if (user_mode(regs)) {
		irqentry_exit_to_user_mode(regs);
	} else if (!regs_irqs_disabled(regs)) {
		/*
		 * If RCU was not watching on entry this needs to be done
		 * carefully and needs the same ordering of lockdep/tracing
		 * and RCU as the return to user mode path.
		 */
		if (state.exit_rcu) {
			instrumentation_begin();
			/* Tell the tracer that IRET will enable interrupts */
			trace_hardirqs_on_prepare();
			lockdep_hardirqs_on_prepare();
			instrumentation_end();
			ct_irq_exit();
			lockdep_hardirqs_on(CALLER_ADDR0);
			return;
		}

		instrumentation_begin();
		if (IS_ENABLED(CONFIG_PREEMPTION))
			irqentry_exit_cond_resched();

		/* Covers both tracing and lockdep */
		trace_hardirqs_on();
		instrumentation_end();
	} else {
		/*
		 * IRQ flags state is correct already. Just tell RCU if it
		 * was not watching on entry.
		 */
		if (state.exit_rcu)
			ct_irq_exit();
	}
}

irqentry_state_t noinstr irqentry_nmi_enter(struct pt_regs *regs)
{
	irqentry_state_t irq_state;

	irq_state.lockdep = lockdep_hardirqs_enabled();

	__nmi_enter();
	lockdep_hardirqs_off(CALLER_ADDR0);
	lockdep_hardirq_enter();
	ct_nmi_enter();

	instrumentation_begin();
	kmsan_unpoison_entry_regs(regs);
	trace_hardirqs_off_finish();
	ftrace_nmi_enter();
	instrumentation_end();

	return irq_state;
}

void noinstr irqentry_nmi_exit(struct pt_regs *regs, irqentry_state_t irq_state)
{
	instrumentation_begin();
	ftrace_nmi_exit();
	if (irq_state.lockdep) {
		trace_hardirqs_on_prepare();
		lockdep_hardirqs_on_prepare();
	}
	instrumentation_end();

	ct_nmi_exit();
	lockdep_hardirq_exit();
	if (irq_state.lockdep)
		lockdep_hardirqs_on(CALLER_ADDR0);
	__nmi_exit();
}
