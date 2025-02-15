/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 1988, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2018 Joyent, Inc.
 * Copyright 2020 Oxide Computer Company
 * Copyright 2021 OmniOS Community Edition (OmniOSce) Association.
 */

/*	Copyright (c) 1984, 1986, 1987, 1988, 1989 AT&T	*/

#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/systm.h>
#include <sys/cred.h>
#include <sys/user.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/ucontext.h>
#include <sys/procfs.h>
#include <sys/vnode.h>
#include <sys/acct.h>
#include <sys/var.h>
#include <sys/cmn_err.h>
#include <sys/debug.h>
#include <sys/wait.h>
#include <sys/siginfo.h>
#include <sys/procset.h>
#include <sys/class.h>
#include <sys/file.h>
#include <sys/session.h>
#include <sys/kmem.h>
#include <sys/vtrace.h>
#include <sys/prsystm.h>
#include <sys/ipc.h>
#include <sys/sem_impl.h>
#include <c2/audit.h>
#include <sys/aio_impl.h>
#include <vm/as.h>
#include <sys/poll.h>
#include <sys/door.h>
#include <sys/lwpchan_impl.h>
#include <sys/utrap.h>
#include <sys/task.h>
#include <sys/exacct.h>
#include <sys/cyclic.h>
#include <sys/schedctl.h>
#include <sys/rctl.h>
#include <sys/contract_impl.h>
#include <sys/contract/process_impl.h>
#include <sys/list.h>
#include <sys/dtrace.h>
#include <sys/pool.h>
#include <sys/sdt.h>
#include <sys/corectl.h>
#include <sys/core.h>
#include <sys/brand.h>
#include <sys/libc_kernel.h>

/*
 * convert code/data pair into old style wait status
 */
int
wstat(int code, int data)
{
	int stat = (data & 0377);

	switch (code) {
	case CLD_EXITED:
		stat <<= 8;
		break;
	case CLD_DUMPED:
		stat |= WCOREFLG;
		break;
	case CLD_KILLED:
		break;
	case CLD_TRAPPED:
	case CLD_STOPPED:
		stat <<= 8;
		stat |= WSTOPFLG;
		break;
	case CLD_CONTINUED:
		stat = WCONTFLG;
		break;
	default:
		cmn_err(CE_PANIC, "wstat: bad code");
		/* NOTREACHED */
	}
	return (stat);
}

static char *
exit_reason(char *buf, size_t bufsz, int what, int why)
{
	switch (why) {
	case CLD_EXITED:
		(void) snprintf(buf, bufsz, "exited with status %d", what);
		break;
	case CLD_KILLED:
		(void) snprintf(buf, bufsz, "exited on fatal signal %d", what);
		break;
	case CLD_DUMPED:
		(void) snprintf(buf, bufsz, "core dumped on signal %d", what);
		break;
	default:
		(void) snprintf(buf, bufsz, "encountered unknown error "
		    "(%d, %d)", why, what);
		break;
	}

	return (buf);
}

/*
 * exit system call: pass back caller's arg.
 */
void
rexit(int rval)
{
	exit(CLD_EXITED, rval);
}

/*
 * Called by proc_exit() when a zone's init exits, presumably because
 * it failed.  As long as the given zone is still in the "running"
 * state, we will re-exec() init, but first we need to reset things
 * which are usually inherited across exec() but will break init's
 * assumption that it is being exec()'d from a virgin process.  Most
 * importantly this includes closing all file descriptors (exec only
 * closes those marked close-on-exec) and resetting signals (exec only
 * resets handled signals, and we need to clear any signals which
 * killed init).  Anything else that exec(2) says would be inherited,
 * but would affect the execution of init, needs to be reset.
 */
static int
restart_init(int what, int why)
{
	kthread_t *t = curthread;
	klwp_t *lwp = ttolwp(t);
	proc_t *p = ttoproc(t);
	proc_t *pp = p->p_zone->zone_zsched;
	user_t *up = PTOU(p);

	vnode_t *oldcd, *oldrd;
	int i, err;
	char reason_buf[64];

	/*
	 * Let zone admin (and global zone admin if this is for a non-global
	 * zone) know that init has failed and will be restarted.
	 */
	zcmn_err(p->p_zone->zone_id, CE_WARN,
	    "init(8) %s: restarting automatically",
	    exit_reason(reason_buf, sizeof (reason_buf), what, why));

	if (!INGLOBALZONE(p)) {
		cmn_err(CE_WARN, "init(8) for zone %s (pid %d) %s: "
		    "restarting automatically",
		    p->p_zone->zone_name, p->p_pid, reason_buf);
	}

	/*
	 * Remove any fpollinfo_t's for this (last) thread from our file
	 * descriptors so closeall() can ASSERT() that they're all gone.
	 * Then close all open file descriptors in the process.
	 */
	pollcleanup();
	closeall(P_FINFO(p));

	/*
	 * Grab p_lock and begin clearing miscellaneous global process
	 * state that needs to be reset before we exec the new init(8).
	 */

	mutex_enter(&p->p_lock);
	prbarrier(p);

	p->p_flag &= ~(SKILLED | SEXTKILLED | SEXITING | SDOCORE);
	up->u_cmask = CMASK;

	sigemptyset(&t->t_hold);
	sigemptyset(&t->t_sig);
	sigemptyset(&t->t_extsig);

	sigemptyset(&p->p_sig);
	sigemptyset(&p->p_extsig);

	sigdelq(p, t, 0);
	sigdelq(p, NULL, 0);

	if (p->p_killsqp) {
		siginfofree(p->p_killsqp);
		p->p_killsqp = NULL;
	}

	/*
	 * Reset any signals that are ignored back to the default disposition.
	 * Other u_signal members will be cleared when exec calls sigdefault().
	 */
	for (i = 1; i < NSIG; i++) {
		if (up->u_signal[i - 1] == SIG_IGN) {
			up->u_signal[i - 1] = SIG_DFL;
			sigemptyset(&up->u_sigmask[i - 1]);
		}
	}

	/*
	 * Clear the current signal, any signal info associated with it, and
	 * any signal information from contracts and/or contract templates.
	 */
	lwp->lwp_cursig = 0;
	lwp->lwp_extsig = 0;
	if (lwp->lwp_curinfo != NULL) {
		siginfofree(lwp->lwp_curinfo);
		lwp->lwp_curinfo = NULL;
	}
	lwp_ctmpl_clear(lwp, B_FALSE);

	/*
	 * Reset both the process root directory and the current working
	 * directory to the root of the zone just as we do during boot.
	 */
	VN_HOLD(p->p_zone->zone_rootvp);
	oldrd = up->u_rdir;
	up->u_rdir = p->p_zone->zone_rootvp;

	VN_HOLD(p->p_zone->zone_rootvp);
	oldcd = up->u_cdir;
	up->u_cdir = p->p_zone->zone_rootvp;

	if (up->u_cwd != NULL) {
		refstr_rele(up->u_cwd);
		up->u_cwd = NULL;
	}

	/* Reset security flags */
	mutex_enter(&pp->p_lock);
	p->p_secflags = pp->p_secflags;
	mutex_exit(&pp->p_lock);

	mutex_exit(&p->p_lock);

	if (oldrd != NULL)
		VN_RELE(oldrd);
	if (oldcd != NULL)
		VN_RELE(oldcd);

	/*
	 * It's possible that a zone's init will have become privilege aware
	 * and modified privilege sets; reset them.
	 */
	cred_t *oldcr, *newcr;

	mutex_enter(&p->p_crlock);
	oldcr = p->p_cred;
	mutex_enter(&pp->p_crlock);
	crhold(newcr = p->p_cred = pp->p_cred);
	mutex_exit(&pp->p_crlock);
	mutex_exit(&p->p_crlock);
	crfree(oldcr);
	/* Additional hold for the current thread - expected by crset() */
	crhold(newcr);
	crset(p, newcr);

	/* Free the controlling tty.  (freectty() always assumes curproc.) */
	ASSERT(p == curproc);
	(void) freectty(B_TRUE);

	/*
	 * Now exec() the new init(8) on top of the current process.  If we
	 * succeed, the caller will treat this like a successful system call.
	 * If we fail, we issue messages and the caller will proceed with exit.
	 */
	err = exec_init(p->p_zone->zone_initname, NULL);

	if (err == 0)
		return (0);

	zcmn_err(p->p_zone->zone_id, CE_WARN,
	    "failed to restart init(8) (err=%d): system reboot required", err);

	if (!INGLOBALZONE(p)) {
		cmn_err(CE_WARN, "failed to restart init(8) for zone %s "
		    "(pid %d, err=%d): zoneadm(8) boot required",
		    p->p_zone->zone_name, p->p_pid, err);
	}

	return (-1);
}

/*
 * Release resources.
 * Enter zombie state.
 * Wake up parent and init processes,
 * and dispose of children.
 */
void
exit(int why, int what)
{
	/*
	 * If proc_exit() fails, then some other lwp in the process
	 * got there first.  We just have to call lwp_exit() to allow
	 * the other lwp to finish exiting the process.  Otherwise we're
	 * restarting init, and should return.
	 */
	if (proc_exit(why, what) != 0) {
		mutex_enter(&curproc->p_lock);
		ASSERT(curproc->p_flag & SEXITLWPS);
		lwp_exit();
		/* NOTREACHED */
	}
}

/*
 * Set the SEXITING flag on the process, after making sure /proc does
 * not have it locked.  This is done in more places than proc_exit(),
 * so it is a separate function.
 */
void
proc_is_exiting(proc_t *p)
{
	mutex_enter(&p->p_lock);
	prbarrier(p);
	p->p_flag |= SEXITING;
	mutex_exit(&p->p_lock);
}

/*
 * Return true if zone's init is restarted, false if exit processing should
 * proceeed.
 */
static boolean_t
zone_init_exit(zone_t *z, int why, int what)
{
	proc_t *p = curproc;
	char reason_buf[64];

	/*
	 * Typically we don't let the zone's init exit unless zone_start_init()
	 * failed its exec, or we are shutting down the zone or the machine,
	 * although the various flags handled within this function will control
	 * the behavior.
	 *
	 * Since we are single threaded, we don't need to lock the following
	 * accesses to zone_proc_initpid.
	 */
	if (z->zone_boot_err != 0 ||
	    zone_status_get(z) >= ZONE_IS_SHUTTING_DOWN ||
	    zone_status_get(global_zone) >= ZONE_IS_SHUTTING_DOWN) {
		/*
		 * Clear the zone's init pid and proceed with exit processing.
		 */
		z->zone_proc_initpid = -1;
		return (B_FALSE);
	}

	zcmn_err(p->p_zone->zone_id, CE_WARN, "init(8) %s",
	    exit_reason(reason_buf, sizeof (reason_buf), what, why));

	if (!INGLOBALZONE(p)) {
		cmn_err(CE_WARN, "init(8) for zone %s (pid %d) %s",
		    p->p_zone->zone_name, p->p_pid, reason_buf);
	}

	/*
	 * There are a variety of configuration flags on the zone to control
	 * init exit behavior.
	 *
	 * If the init process should be restarted, the "zone_restart_init"
	 * member will be set.
	 */
	if (!z->zone_restart_init) {
		/*
		 * The zone has been set up to halt when init exits.
		 */
		z->zone_init_status = wstat(why, what);
		(void) zone_kadmin(A_SHUTDOWN, AD_HALT, NULL, zone_kcred());
		z->zone_proc_initpid = -1;
		return (B_FALSE);
	}

	/*
	 * At this point we know we're configured to restart init, but there
	 * are various modifiers to that behavior.
	 */

	if (z->zone_reboot_on_init_exit) {
		/*
		 * Some init programs in branded zones do not tolerate a
		 * restart in the traditional manner; setting
		 * "zone_reboot_on_init_exit" will cause the entire zone to be
		 * rebooted instead.
		 */

		if (z->zone_restart_init_0) {
			/*
			 * Some init programs in branded zones only want to
			 * restart if they exit 0, otherwise the zone should
			 * shutdown. Setting the "zone_restart_init_0" member
			 * controls this behavior.
			 */
			if (why == CLD_EXITED && what == 0) {
				/* Trigger a zone reboot */
				(void) zone_kadmin(A_REBOOT, 0, NULL,
				    zone_kcred());
			} else {
				/* Shutdown instead of reboot */
				(void) zone_kadmin(A_SHUTDOWN, AD_HALT, NULL,
				    zone_kcred());
			}
		} else {
			/* Trigger a zone reboot */
			(void) zone_kadmin(A_REBOOT, 0, NULL, zone_kcred());
		}

		z->zone_init_status = wstat(why, what);
		z->zone_proc_initpid = -1;
		return (B_FALSE);
	}

	if (z->zone_restart_init_0) {
		/*
		 * Some init programs in branded zones only want to restart if
		 * they exit 0, otherwise the zone should shutdown. Setting the
		 * "zone_restart_init_0" member controls this behavior.
		 *
		 * In this case we only restart init if it exited successfully.
		 */
		if (why == CLD_EXITED && what == 0 &&
		    restart_init(what, why) == 0) {
			return (B_TRUE);
		}
	} else {
		/*
		 * No restart modifiers on the zone, attempt to restart init.
		 */
		if (restart_init(what, why) == 0)
			return (B_TRUE);
	}

	/*
	 * The restart failed, or the criteria for a restart are not met;
	 * the zone will shut down.
	 */
	z->zone_init_status = wstat(why, what);
	(void) zone_kadmin(A_SHUTDOWN, AD_HALT, NULL, zone_kcred());
	z->zone_proc_initpid = -1;
	return (B_FALSE);
}

/*
 * Return value:
 *   1 - exitlwps() failed, call (or continue) lwp_exit()
 *   0 - restarting init.  Return through system call path
 */
int
proc_exit(int why, int what)
{
	kthread_t *t = curthread;
	klwp_t *lwp = ttolwp(t);
	proc_t *p = ttoproc(t);
	zone_t *z = p->p_zone;
	timeout_id_t tmp_id;
	int rv;
	proc_t *q;
	task_t *tk;
	vnode_t *exec_vp, *execdir_vp, *cdir, *rdir;
	sigqueue_t *sqp;
	lwpdir_t *lwpdir;
	uint_t lwpdir_sz;
	tidhash_t *tidhash;
	uint_t tidhash_sz;
	ret_tidhash_t *ret_tidhash;
	refstr_t *cwd;
	hrtime_t hrutime, hrstime;
	int evaporate;

	/*
	 * Stop and discard the process's lwps except for the current one,
	 * unless some other lwp beat us to it.  If exitlwps() fails then
	 * return and the calling lwp will call (or continue in) lwp_exit().
	 */
	proc_is_exiting(p);
	if (exitlwps(0) != 0)
		return (1);

	mutex_enter(&p->p_lock);
	if (p->p_ttime > 0) {
		/*
		 * Account any remaining ticks charged to this process
		 * on its way out.
		 */
		(void) task_cpu_time_incr(p->p_task, p->p_ttime);
		p->p_ttime = 0;
	}
	mutex_exit(&p->p_lock);

	if (p->p_pid == z->zone_proc_initpid) {
		/* If zone's init restarts, we're done here. */
		if (zone_init_exit(z, why, what))
			return (0);
	}

	/*
	 * Delay firing probes (and performing brand cleanup) until after the
	 * zone_proc_initpid check. Cases which result in zone shutdown or
	 * restart via zone_kadmin eventually result in a call back to
	 * proc_exit.
	 */
	DTRACE_PROC(lwp__exit);
	DTRACE_PROC1(exit, int, why);

	/*
	 * Will perform any brand specific proc exit processing. Since this
	 * is always the last lwp, will also perform lwp exit/free and proc
	 * exit. Brand data will be freed when the process is reaped.
	 */
	if (PROC_IS_BRANDED(p)) {
		BROP(p)->b_lwpexit(lwp);
		BROP(p)->b_proc_exit(p);
		/*
		 * To ensure that b_proc_exit has access to brand-specific data
		 * contained by the one remaining lwp, call the freelwp hook as
		 * the last part of this clean-up process.
		 */
		BROP(p)->b_freelwp(lwp);
		lwp_detach_brand_hdlrs(lwp);
	}

	lwp_pcb_exit();

	/*
	 * Allocate a sigqueue now, before we grab locks.
	 * It will be given to sigcld(), below.
	 * Special case:  If we will be making the process disappear
	 * without a trace because it is either:
	 *	* an exiting SSYS process, or
	 *	* a posix_spawn() vfork child who requests it,
	 * we don't bother to allocate a useless sigqueue.
	 */
	evaporate = (p->p_flag & SSYS) || ((p->p_flag & SVFORK) &&
	    why == CLD_EXITED && what == _EVAPORATE);
	if (!evaporate)
		sqp = kmem_zalloc(sizeof (sigqueue_t), KM_SLEEP);

	/*
	 * revoke any doors created by the process.
	 */
	if (p->p_door_list)
		door_exit();

	/*
	 * Release schedctl data structures.
	 */
	if (p->p_pagep)
		schedctl_proc_cleanup();

	/*
	 * make sure all pending kaio has completed.
	 */
	if (p->p_aio)
		aio_cleanup_exit();

	/*
	 * discard the lwpchan cache.
	 */
	if (p->p_lcp != NULL)
		lwpchan_destroy_cache(0);

	/*
	 * Clean up any DTrace helper actions or probes for the process.
	 */
	if (p->p_dtrace_helpers != NULL) {
		ASSERT(dtrace_helpers_cleanup != NULL);
		(*dtrace_helpers_cleanup)(p);
	}

	/*
	 * Clean up any signalfd state for the process.
	 */
	if (p->p_sigfd != NULL) {
		VERIFY(sigfd_exit_helper != NULL);
		(*sigfd_exit_helper)();
	}

	/* untimeout the realtime timers */
	if (p->p_itimer != NULL)
		timer_exit();

	if ((tmp_id = p->p_alarmid) != 0) {
		p->p_alarmid = 0;
		(void) untimeout(tmp_id);
	}

	/*
	 * If we had generated any upanic(2) state, free that now.
	 */
	if (p->p_upanic != NULL) {
		kmem_free(p->p_upanic, PRUPANIC_BUFLEN);
		p->p_upanic = NULL;
	}

	/*
	 * Remove any fpollinfo_t's for this (last) thread from our file
	 * descriptors so closeall() can ASSERT() that they're all gone.
	 */
	pollcleanup();

	if (p->p_rprof_cyclic != CYCLIC_NONE) {
		mutex_enter(&cpu_lock);
		cyclic_remove(p->p_rprof_cyclic);
		mutex_exit(&cpu_lock);
	}

	mutex_enter(&p->p_lock);

	/*
	 * Clean up any DTrace probes associated with this process.
	 */
	if (p->p_dtrace_probes) {
		ASSERT(dtrace_fasttrap_exit_ptr != NULL);
		dtrace_fasttrap_exit_ptr(p);
	}

	while ((tmp_id = p->p_itimerid) != 0) {
		p->p_itimerid = 0;
		mutex_exit(&p->p_lock);
		(void) untimeout(tmp_id);
		mutex_enter(&p->p_lock);
	}

	lwp_cleanup();

	/*
	 * We are about to exit; prevent our resource associations from
	 * being changed.
	 */
	pool_barrier_enter();

	/*
	 * Block the process against /proc now that we have really
	 * acquired p->p_lock (to manipulate p_tlist at least).
	 */
	prbarrier(p);

	sigfillset(&p->p_ignore);
	sigemptyset(&p->p_siginfo);
	sigemptyset(&p->p_sig);
	sigemptyset(&p->p_extsig);
	sigemptyset(&t->t_sig);
	sigemptyset(&t->t_extsig);
	sigemptyset(&p->p_sigmask);
	sigdelq(p, t, 0);
	lwp->lwp_cursig = 0;
	lwp->lwp_extsig = 0;
	p->p_flag &= ~(SKILLED | SEXTKILLED);
	if (lwp->lwp_curinfo) {
		siginfofree(lwp->lwp_curinfo);
		lwp->lwp_curinfo = NULL;
	}

	t->t_proc_flag |= TP_LWPEXIT;
	ASSERT(p->p_lwpcnt == 1 && p->p_zombcnt == 0);
	prlwpexit(t);		/* notify /proc */
	lwp_hash_out(p, t->t_tid);
	prexit(p);

	p->p_lwpcnt = 0;
	p->p_tlist = NULL;
	sigqfree(p);
	term_mstate(t);
	p->p_mterm = gethrtime();

	exec_vp = p->p_exec;
	execdir_vp = p->p_execdir;
	p->p_exec = NULLVP;
	p->p_execdir = NULLVP;
	mutex_exit(&p->p_lock);

	pr_free_watched_pages(p);

	closeall(P_FINFO(p));

	/* Free the controlling tty.  (freectty() always assumes curproc.) */
	ASSERT(p == curproc);
	(void) freectty(B_TRUE);

#if defined(__sparc)
	if (p->p_utraps != NULL)
		utrap_free(p);
#endif
	if (p->p_semacct)			/* IPC semaphore exit */
		semexit(p);
	rv = wstat(why, what);

	acct(rv);
	exacct_commit_proc(p, rv);

	/*
	 * Release any resources associated with C2 auditing
	 */
	if (AU_AUDITING()) {
		/*
		 * audit exit system call
		 */
		audit_exit(why, what);
	}

	/*
	 * Free address space.
	 */
	relvm();

	if (exec_vp) {
		/*
		 * Close this executable which has been opened when the process
		 * was created by getproc().
		 */
		(void) VOP_CLOSE(exec_vp, FREAD, 1, (offset_t)0, CRED(), NULL);
		VN_RELE(exec_vp);
	}
	if (execdir_vp)
		VN_RELE(execdir_vp);

	/*
	 * Release held contracts.
	 */
	contract_exit(p);

	/*
	 * Depart our encapsulating process contract.
	 */
	if ((p->p_flag & SSYS) == 0) {
		ASSERT(p->p_ct_process);
		contract_process_exit(p->p_ct_process, p, rv);
	}

	/*
	 * Remove pool association, and block if requested by pool_do_bind.
	 */
	mutex_enter(&p->p_lock);
	ASSERT(p->p_pool->pool_ref > 0);
	atomic_dec_32(&p->p_pool->pool_ref);
	p->p_pool = pool_default;
	/*
	 * Now that our address space has been freed and all other threads
	 * in this process have exited, set the PEXITED pool flag.  This
	 * tells the pools subsystems to ignore this process if it was
	 * requested to rebind this process to a new pool.
	 */
	p->p_poolflag |= PEXITED;
	pool_barrier_exit();
	mutex_exit(&p->p_lock);

	mutex_enter(&pidlock);

	/*
	 * Delete this process from the newstate list of its parent. We
	 * will put it in the right place in the sigcld in the end.
	 */
	delete_ns(p->p_parent, p);

	/*
	 * Reassign the orphans to the next of kin.
	 * Don't rearrange init's orphanage.
	 */
	if ((q = p->p_orphan) != NULL && p != proc_init) {

		proc_t *nokp = p->p_nextofkin;

		for (;;) {
			q->p_nextofkin = nokp;
			if (q->p_nextorph == NULL)
				break;
			q = q->p_nextorph;
		}
		q->p_nextorph = nokp->p_orphan;
		nokp->p_orphan = p->p_orphan;
		p->p_orphan = NULL;
	}

	/*
	 * Reassign the children to init.
	 * Don't try to assign init's children to init.
	 */
	if ((q = p->p_child) != NULL && p != proc_init) {
		struct proc	*np;
		struct proc	*initp = proc_init;
		pid_t		zone_initpid = 1;
		struct proc	*zoneinitp = NULL;
		boolean_t	setzonetop = B_FALSE;

		if (!INGLOBALZONE(curproc)) {
			zone_initpid = curproc->p_zone->zone_proc_initpid;

			ASSERT(MUTEX_HELD(&pidlock));
			zoneinitp = prfind(zone_initpid);
			if (zoneinitp != NULL) {
				initp = zoneinitp;
			} else {
				zone_initpid = 1;
				setzonetop = B_TRUE;
			}
		}

		pgdetach(p);

		do {
			np = q->p_sibling;
			/*
			 * Delete it from its current parent new state
			 * list and add it to init new state list
			 */
			delete_ns(q->p_parent, q);

			q->p_ppid = zone_initpid;

			q->p_pidflag &= ~(CLDNOSIGCHLD | CLDWAITPID);
			if (setzonetop) {
				mutex_enter(&q->p_lock);
				q->p_flag |= SZONETOP;
				mutex_exit(&q->p_lock);
			}
			q->p_parent = initp;

			/*
			 * Since q will be the first child,
			 * it will not have a previous sibling.
			 */
			q->p_psibling = NULL;
			if (initp->p_child) {
				initp->p_child->p_psibling = q;
			}
			q->p_sibling = initp->p_child;
			initp->p_child = q;
			if (q->p_proc_flag & P_PR_PTRACE) {
				mutex_enter(&q->p_lock);
				sigtoproc(q, NULL, SIGKILL);
				mutex_exit(&q->p_lock);
			}
			/*
			 * sigcld() will add the child to parents
			 * newstate list.
			 */
			if (q->p_stat == SZOMB)
				sigcld(q, NULL);
		} while ((q = np) != NULL);

		p->p_child = NULL;
		ASSERT(p->p_child_ns == NULL);
	}

	TRACE_1(TR_FAC_PROC, TR_PROC_EXIT, "proc_exit: %p", p);

	mutex_enter(&p->p_lock);
	CL_EXIT(curthread); /* tell the scheduler that curthread is exiting */

	/*
	 * Have our task accummulate our resource usage data before they
	 * become contaminated by p_cacct etc., and before we renounce
	 * membership of the task.
	 *
	 * We do this regardless of whether or not task accounting is active.
	 * This is to avoid having nonsense data reported for this task if
	 * task accounting is subsequently enabled. The overhead is minimal;
	 * by this point, this process has accounted for the usage of all its
	 * LWPs. We nonetheless do the work here, and under the protection of
	 * pidlock, so that the movement of the process's usage to the task
	 * happens at the same time as the removal of the process from the
	 * task, from the point of view of exacct_snapshot_task_usage().
	 */
	exacct_update_task_mstate(p);

	hrutime = mstate_aggr_state(p, LMS_USER);
	hrstime = mstate_aggr_state(p, LMS_SYSTEM);
	p->p_utime = (clock_t)NSEC_TO_TICK(hrutime) + p->p_cutime;
	p->p_stime = (clock_t)NSEC_TO_TICK(hrstime) + p->p_cstime;

	p->p_acct[LMS_USER]	+= p->p_cacct[LMS_USER];
	p->p_acct[LMS_SYSTEM]	+= p->p_cacct[LMS_SYSTEM];
	p->p_acct[LMS_TRAP]	+= p->p_cacct[LMS_TRAP];
	p->p_acct[LMS_TFAULT]	+= p->p_cacct[LMS_TFAULT];
	p->p_acct[LMS_DFAULT]	+= p->p_cacct[LMS_DFAULT];
	p->p_acct[LMS_KFAULT]	+= p->p_cacct[LMS_KFAULT];
	p->p_acct[LMS_USER_LOCK] += p->p_cacct[LMS_USER_LOCK];
	p->p_acct[LMS_SLEEP]	+= p->p_cacct[LMS_SLEEP];
	p->p_acct[LMS_WAIT_CPU]	+= p->p_cacct[LMS_WAIT_CPU];
	p->p_acct[LMS_STOPPED]	+= p->p_cacct[LMS_STOPPED];

	p->p_ru.minflt	+= p->p_cru.minflt;
	p->p_ru.majflt	+= p->p_cru.majflt;
	p->p_ru.nswap	+= p->p_cru.nswap;
	p->p_ru.inblock	+= p->p_cru.inblock;
	p->p_ru.oublock	+= p->p_cru.oublock;
	p->p_ru.msgsnd	+= p->p_cru.msgsnd;
	p->p_ru.msgrcv	+= p->p_cru.msgrcv;
	p->p_ru.nsignals += p->p_cru.nsignals;
	p->p_ru.nvcsw	+= p->p_cru.nvcsw;
	p->p_ru.nivcsw	+= p->p_cru.nivcsw;
	p->p_ru.sysc	+= p->p_cru.sysc;
	p->p_ru.ioch	+= p->p_cru.ioch;

	p->p_stat = SZOMB;
	p->p_proc_flag &= ~P_PR_PTRACE;
	p->p_wdata = what;
	p->p_wcode = (char)why;

	cdir = PTOU(p)->u_cdir;
	rdir = PTOU(p)->u_rdir;
	cwd = PTOU(p)->u_cwd;

	ASSERT(cdir != NULL || p->p_parent == &p0);

	/*
	 * Release resource controls, as they are no longer enforceable.
	 */
	rctl_set_free(p->p_rctls);

	/*
	 * Decrement tk_nlwps counter for our task.max-lwps resource control.
	 * An extended accounting record, if that facility is active, is
	 * scheduled to be written.  We cannot give up task and project
	 * membership at this point because that would allow zombies to escape
	 * from the max-processes resource controls.  Zombies stay in their
	 * current task and project until the process table slot is released
	 * in freeproc().
	 */
	tk = p->p_task;

	mutex_enter(&p->p_zone->zone_nlwps_lock);
	tk->tk_nlwps--;
	tk->tk_proj->kpj_nlwps--;
	p->p_zone->zone_nlwps--;
	mutex_exit(&p->p_zone->zone_nlwps_lock);

	/*
	 * Clear the lwp directory and the lwpid hash table
	 * now that /proc can't bother us any more.
	 * We free the memory below, after dropping p->p_lock.
	 */
	lwpdir = p->p_lwpdir;
	lwpdir_sz = p->p_lwpdir_sz;
	tidhash = p->p_tidhash;
	tidhash_sz = p->p_tidhash_sz;
	ret_tidhash = p->p_ret_tidhash;
	p->p_lwpdir = NULL;
	p->p_lwpfree = NULL;
	p->p_lwpdir_sz = 0;
	p->p_tidhash = NULL;
	p->p_tidhash_sz = 0;
	p->p_ret_tidhash = NULL;

	/*
	 * If the process has context ops installed, call the exit routine
	 * on behalf of this last remaining thread. Normally exitpctx() is
	 * called during thread_exit() or lwp_exit(), but because this is the
	 * last thread in the process, we must call it here. By the time
	 * thread_exit() is called (below), the association with the relevant
	 * process has been lost.
	 *
	 * We also free the context here.
	 */
	if (p->p_pctx) {
		kpreempt_disable();
		exitpctx(p);
		kpreempt_enable();

		freepctx(p, 0);
	}

	/*
	 * curthread's proc pointer is changed to point to the 'sched'
	 * process for the corresponding zone, except in the case when
	 * the exiting process is in fact a zsched instance, in which
	 * case the proc pointer is set to p0.  We do so, so that the
	 * process still points at the right zone when we call the VN_RELE()
	 * below.
	 *
	 * This is because curthread's original proc pointer can be freed as
	 * soon as the child sends a SIGCLD to its parent.  We use zsched so
	 * that for user processes, even in the final moments of death, the
	 * process is still associated with its zone.
	 */
	if (p != t->t_procp->p_zone->zone_zsched)
		t->t_procp = t->t_procp->p_zone->zone_zsched;
	else
		t->t_procp = &p0;

	mutex_exit(&p->p_lock);
	if (!evaporate) {
		/*
		 * The brand specific code only happens when the brand has a
		 * function to call in place of sigcld and the parent of the
		 * exiting process is not the global zone init. If the parent
		 * is the global zone init, then the process was reparented,
		 * and we don't want brand code delivering possibly strange
		 * signals to init. Also, init is not branded, so any brand
		 * specific exit data will not be picked up by init anyway.
		 */
		if (PROC_IS_BRANDED(p) &&
		    BROP(p)->b_exit_with_sig != NULL &&
		    p->p_ppid != 1) {
			/*
			 * The code for _fini that could unload the brand_t
			 * blocks until the count of zones using the module
			 * reaches zero. Zones decrement the refcount on their
			 * brands only after all user tasks in that zone have
			 * exited and been waited on. The decrement on the
			 * brand's refcount happen in zone_destroy(). That
			 * depends on zone_shutdown() having been completed.
			 * zone_shutdown() includes a call to zone_empty(),
			 * where the zone waits for itself to reach the state
			 * ZONE_IS_EMPTY. This state is only set in either
			 * zone_shutdown(), when there are no user processes as
			 * the zone enters this function, or in
			 * zone_task_rele(). zone_task_rele() is called from
			 * code triggered by waiting on processes, not by the
			 * processes exiting through proc_exit().  This means
			 * all the branded processes that could exist for a
			 * specific brand_t must exit and get reaped before the
			 * refcount on the brand_t can reach 0. _fini will
			 * never unload the corresponding brand module before
			 * proc_exit finishes execution for all processes
			 * branded with a particular brand_t, which makes the
			 * operation below safe to do. Brands that wish to use
			 * this mechanism must wait in _fini as described
			 * above.
			 */
			BROP(p)->b_exit_with_sig(p, sqp);
		} else {
			p->p_pidflag &= ~CLDPEND;
			sigcld(p, sqp);
		}

	} else {
		/*
		 * Do what sigcld() would do if the disposition
		 * of the SIGCHLD signal were set to be ignored.
		 */
		cv_broadcast(&p->p_srwchan_cv);
		freeproc(p);
	}
	mutex_exit(&pidlock);

	/*
	 * We don't release u_cdir and u_rdir until SZOMB is set.
	 * This protects us against dofusers().
	 */
	if (cdir)
		VN_RELE(cdir);
	if (rdir)
		VN_RELE(rdir);
	if (cwd)
		refstr_rele(cwd);

	/*
	 * task_rele() may ultimately cause the zone to go away (or
	 * may cause the last user process in a zone to go away, which
	 * signals zsched to go away).  So prior to this call, we must
	 * no longer point at zsched.
	 */
	t->t_procp = &p0;

	kmem_free(lwpdir, lwpdir_sz * sizeof (lwpdir_t));
	kmem_free(tidhash, tidhash_sz * sizeof (tidhash_t));
	while (ret_tidhash != NULL) {
		ret_tidhash_t *next = ret_tidhash->rth_next;
		kmem_free(ret_tidhash->rth_tidhash,
		    ret_tidhash->rth_tidhash_sz * sizeof (tidhash_t));
		kmem_free(ret_tidhash, sizeof (*ret_tidhash));
		ret_tidhash = next;
	}

	thread_exit();
	/* NOTREACHED */
}

/*
 * Format siginfo structure for wait system calls.
 */
void
winfo(proc_t *pp, k_siginfo_t *ip, int waitflag)
{
	ASSERT(MUTEX_HELD(&pidlock));

	bzero(ip, sizeof (k_siginfo_t));
	ip->si_signo = SIGCLD;
	ip->si_code = pp->p_wcode;
	ip->si_pid = pp->p_pid;
	ip->si_ctid = PRCTID(pp);
	ip->si_zoneid = pp->p_zone->zone_id;
	ip->si_status = pp->p_wdata;
	ip->si_stime = pp->p_stime;
	ip->si_utime = pp->p_utime;

	if (waitflag) {
		pp->p_wcode = 0;
		pp->p_wdata = 0;
		pp->p_pidflag &= ~CLDPEND;
	}
}

/*
 * Wait system call.
 * Search for a terminated (zombie) child,
 * finally lay it to rest, and collect its status.
 * Look also for stopped children,
 * and pass back status from them.
 */
int
waitid(idtype_t idtype, id_t id, k_siginfo_t *ip, int options)
{
	proc_t *cp, *pp;
	int waitflag = !(options & WNOWAIT);
	boolean_t have_brand_helper = B_FALSE;

	/*
	 * Obsolete flag, defined here only for binary compatibility
	 * with old statically linked executables.  Delete this when
	 * we no longer care about these old and broken applications.
	 */
#define	_WNOCHLD	0400
	options &= ~_WNOCHLD;

	if (options == 0 || (options & ~WOPTMASK))
		return (EINVAL);

	switch (idtype) {
	case P_PID:
	case P_PGID:
		if (id < 0 || id >= maxpid)
			return (EINVAL);
		/* FALLTHROUGH */
	case P_ALL:
		break;
	default:
		return (EINVAL);
	}

	pp = ttoproc(curthread);

	/*
	 * Anytime you are looking for a process, you take pidlock to prevent
	 * things from changing as you look.
	 */
	mutex_enter(&pidlock);

	/*
	 * if we are only looking for exited processes and child_ns list
	 * is empty no reason to look at all children.
	 */
	if (idtype == P_ALL &&
	    (options & ~WNOWAIT) == (WNOHANG | WEXITED) &&
	    pp->p_child_ns == NULL) {
		if (pp->p_child) {
			mutex_exit(&pidlock);
			bzero(ip, sizeof (k_siginfo_t));
			return (0);
		}
		mutex_exit(&pidlock);
		return (ECHILD);
	}

	if (PROC_IS_BRANDED(pp) && BROP(pp)->b_waitid_helper != NULL) {
		have_brand_helper = B_TRUE;
	}

	while (pp->p_child != NULL || have_brand_helper) {
		boolean_t brand_wants_wait = B_FALSE;
		int proc_gone = 0;
		int found = 0;

		/*
		 * Give the brand a chance to return synthetic results from
		 * this waitid() call before we do the real thing.
		 */
		if (have_brand_helper) {
			int ret;

			if (BROP(pp)->b_waitid_helper(idtype, id, ip, options,
			    &brand_wants_wait, &ret) == 0) {
				mutex_exit(&pidlock);
				return (ret);
			}

			if (pp->p_child == NULL) {
				goto no_real_children;
			}
		}

		/*
		 * Look for interesting children in the newstate list.
		 */
		VERIFY(pp->p_child != NULL);
		for (cp = pp->p_child_ns; cp != NULL; cp = cp->p_sibling_ns) {
			if (idtype != P_PID && (cp->p_pidflag & CLDWAITPID))
				continue;
			if (idtype == P_PID && id != cp->p_pid)
				continue;
			if (idtype == P_PGID && id != cp->p_pgrp)
				continue;
			if (PROC_IS_BRANDED(pp)) {
				if (BROP(pp)->b_wait_filter != NULL &&
				    BROP(pp)->b_wait_filter(pp, cp) == B_FALSE)
					continue;
			}

			switch (cp->p_wcode) {

			case CLD_TRAPPED:
			case CLD_STOPPED:
			case CLD_CONTINUED:
				cmn_err(CE_PANIC,
				    "waitid: wrong state %d on the p_newstate"
				    " list", cp->p_wcode);
				break;

			case CLD_EXITED:
			case CLD_DUMPED:
			case CLD_KILLED:
				if (!(options & WEXITED)) {
					/*
					 * Count how many are already gone
					 * for good.
					 */
					proc_gone++;
					break;
				}
				if (!waitflag) {
					winfo(cp, ip, 0);
				} else {
					winfo(cp, ip, 1);
					freeproc(cp);
				}
				mutex_exit(&pidlock);
				if (waitflag) {		/* accept SIGCLD */
					sigcld_delete(ip);
					sigcld_repost();
				}
				return (0);
			}

			if (idtype == P_PID)
				break;
		}

		/*
		 * Wow! None of the threads on the p_sibling_ns list were
		 * interesting threads. Check all the kids!
		 */
		for (cp = pp->p_child; cp != NULL; cp = cp->p_sibling) {
			if (idtype == P_PID && id != cp->p_pid)
				continue;
			if (idtype == P_PGID && id != cp->p_pgrp)
				continue;
			if (PROC_IS_BRANDED(pp)) {
				if (BROP(pp)->b_wait_filter != NULL &&
				    BROP(pp)->b_wait_filter(pp, cp) == B_FALSE)
					continue;
			}

			switch (cp->p_wcode) {
			case CLD_TRAPPED:
				if (!(options & WTRAPPED))
					break;
				winfo(cp, ip, waitflag);
				mutex_exit(&pidlock);
				if (waitflag) {		/* accept SIGCLD */
					sigcld_delete(ip);
					sigcld_repost();
				}
				return (0);

			case CLD_STOPPED:
				if (!(options & WSTOPPED))
					break;
				/* Is it still stopped? */
				mutex_enter(&cp->p_lock);
				if (!jobstopped(cp)) {
					mutex_exit(&cp->p_lock);
					break;
				}
				mutex_exit(&cp->p_lock);
				winfo(cp, ip, waitflag);
				mutex_exit(&pidlock);
				if (waitflag) {		/* accept SIGCLD */
					sigcld_delete(ip);
					sigcld_repost();
				}
				return (0);

			case CLD_CONTINUED:
				if (!(options & WCONTINUED))
					break;
				winfo(cp, ip, waitflag);
				mutex_exit(&pidlock);
				if (waitflag) {		/* accept SIGCLD */
					sigcld_delete(ip);
					sigcld_repost();
				}
				return (0);

			case CLD_EXITED:
			case CLD_DUMPED:
			case CLD_KILLED:
				if (idtype != P_PID &&
				    (cp->p_pidflag & CLDWAITPID))
					continue;
				/*
				 * Don't complain if a process was found in
				 * the first loop but we broke out of the loop
				 * because of the arguments passed to us.
				 */
				if (proc_gone == 0) {
					cmn_err(CE_PANIC,
					    "waitid: wrong state on the"
					    " p_child list");
				} else {
					break;
				}
			}

			found++;

			if (idtype == P_PID)
				break;
		}

no_real_children:
		/*
		 * If we found no interesting processes at all,
		 * break out and return ECHILD.
		 */
		if (!brand_wants_wait && (found + proc_gone == 0))
			break;

		if (options & WNOHANG) {
			mutex_exit(&pidlock);
			bzero(ip, sizeof (k_siginfo_t));
			/*
			 * We should set ip->si_signo = SIGCLD,
			 * but there is an SVVS test that expects
			 * ip->si_signo to be zero in this case.
			 */
			return (0);
		}

		/*
		 * If we found no processes of interest that could
		 * change state while we wait, we don't wait at all.
		 * Get out with ECHILD according to SVID.
		 */
		if (!brand_wants_wait && (found == proc_gone))
			break;

		if (!cv_wait_sig_swap(&pp->p_cv, &pidlock)) {
			mutex_exit(&pidlock);
			return (EINTR);
		}
	}
	mutex_exit(&pidlock);
	return (ECHILD);
}

int
waitsys(idtype_t idtype, id_t id, siginfo_t *infop, int options)
{
	int error;
	k_siginfo_t info;

	if (error = waitid(idtype, id, &info, options))
		return (set_errno(error));
	if (copyout(&info, infop, sizeof (k_siginfo_t)))
		return (set_errno(EFAULT));
	return (0);
}

#ifdef _SYSCALL32_IMPL

int
waitsys32(idtype_t idtype, id_t id, siginfo_t *infop, int options)
{
	int error;
	k_siginfo_t info;
	siginfo32_t info32;

	if (error = waitid(idtype, id, &info, options))
		return (set_errno(error));
	siginfo_kto32(&info, &info32);
	if (copyout(&info32, infop, sizeof (info32)))
		return (set_errno(EFAULT));
	return (0);
}

#endif	/* _SYSCALL32_IMPL */

void
proc_detach(proc_t *p)
{
	proc_t *q;

	ASSERT(MUTEX_HELD(&pidlock));

	q = p->p_parent;
	ASSERT(q != NULL);

	/*
	 * Take it off the newstate list of its parent
	 */
	delete_ns(q, p);

	if (q->p_child == p) {
		q->p_child = p->p_sibling;
		/*
		 * If the parent has no children, it better not
		 * have any with new states either!
		 */
		ASSERT(q->p_child ? 1 : q->p_child_ns == NULL);
	}

	if (p->p_sibling) {
		p->p_sibling->p_psibling = p->p_psibling;
	}

	if (p->p_psibling) {
		p->p_psibling->p_sibling = p->p_sibling;
	}
}

/*
 * Remove zombie children from the process table.
 */
void
freeproc(proc_t *p)
{
	proc_t *q;
	task_t *tk;

	ASSERT(p->p_stat == SZOMB);
	ASSERT(p->p_tlist == NULL);
	ASSERT(MUTEX_HELD(&pidlock));

	sigdelq(p, NULL, 0);
	if (p->p_killsqp) {
		siginfofree(p->p_killsqp);
		p->p_killsqp = NULL;
	}

	/* Clear any remaining brand data */
	if (PROC_IS_BRANDED(p)) {
		brand_clearbrand(p, B_FALSE);
	}


	prfree(p);	/* inform /proc */

	/*
	 * Don't free the init processes.
	 * Other dying processes will access it.
	 */
	if (p == proc_init)
		return;


	/*
	 * We wait until now to free the cred structure because a
	 * zombie process's credentials may be examined by /proc.
	 * No cred locking needed because there are no threads at this point.
	 */
	upcount_dec(crgetruid(p->p_cred), crgetzoneid(p->p_cred));
	crfree(p->p_cred);
	if (p->p_corefile != NULL) {
		corectl_path_rele(p->p_corefile);
		p->p_corefile = NULL;
	}
	if (p->p_content != NULL) {
		corectl_content_rele(p->p_content);
		p->p_content = NULL;
	}

	if (p->p_nextofkin && !((p->p_nextofkin->p_flag & SNOWAIT) ||
	    (PTOU(p->p_nextofkin)->u_signal[SIGCLD - 1] == SIG_IGN))) {
		/*
		 * This should still do the right thing since p_utime/stime
		 * get set to the correct value on process exit, so it
		 * should get properly updated
		 */
		p->p_nextofkin->p_cutime += p->p_utime;
		p->p_nextofkin->p_cstime += p->p_stime;

		p->p_nextofkin->p_cacct[LMS_USER] += p->p_acct[LMS_USER];
		p->p_nextofkin->p_cacct[LMS_SYSTEM] += p->p_acct[LMS_SYSTEM];
		p->p_nextofkin->p_cacct[LMS_TRAP] += p->p_acct[LMS_TRAP];
		p->p_nextofkin->p_cacct[LMS_TFAULT] += p->p_acct[LMS_TFAULT];
		p->p_nextofkin->p_cacct[LMS_DFAULT] += p->p_acct[LMS_DFAULT];
		p->p_nextofkin->p_cacct[LMS_KFAULT] += p->p_acct[LMS_KFAULT];
		p->p_nextofkin->p_cacct[LMS_USER_LOCK]
		    += p->p_acct[LMS_USER_LOCK];
		p->p_nextofkin->p_cacct[LMS_SLEEP] += p->p_acct[LMS_SLEEP];
		p->p_nextofkin->p_cacct[LMS_WAIT_CPU]
		    += p->p_acct[LMS_WAIT_CPU];
		p->p_nextofkin->p_cacct[LMS_STOPPED] += p->p_acct[LMS_STOPPED];

		p->p_nextofkin->p_cru.minflt	+= p->p_ru.minflt;
		p->p_nextofkin->p_cru.majflt	+= p->p_ru.majflt;
		p->p_nextofkin->p_cru.nswap	+= p->p_ru.nswap;
		p->p_nextofkin->p_cru.inblock	+= p->p_ru.inblock;
		p->p_nextofkin->p_cru.oublock	+= p->p_ru.oublock;
		p->p_nextofkin->p_cru.msgsnd	+= p->p_ru.msgsnd;
		p->p_nextofkin->p_cru.msgrcv	+= p->p_ru.msgrcv;
		p->p_nextofkin->p_cru.nsignals	+= p->p_ru.nsignals;
		p->p_nextofkin->p_cru.nvcsw	+= p->p_ru.nvcsw;
		p->p_nextofkin->p_cru.nivcsw	+= p->p_ru.nivcsw;
		p->p_nextofkin->p_cru.sysc	+= p->p_ru.sysc;
		p->p_nextofkin->p_cru.ioch	+= p->p_ru.ioch;

	}

	q = p->p_nextofkin;
	if (q && q->p_orphan == p)
		q->p_orphan = p->p_nextorph;
	else if (q) {
		for (q = q->p_orphan; q; q = q->p_nextorph)
			if (q->p_nextorph == p)
				break;
		ASSERT(q && q->p_nextorph == p);
		q->p_nextorph = p->p_nextorph;
	}

	/*
	 * The process table slot is being freed, so it is now safe to give up
	 * task and project membership.
	 */
	mutex_enter(&p->p_lock);
	tk = p->p_task;
	task_detach(p);
	mutex_exit(&p->p_lock);

	proc_detach(p);
	pid_exit(p, tk);	/* frees pid and proc structure */

	task_rele(tk);
}

/*
 * Delete process "child" from the newstate list of process "parent"
 */
void
delete_ns(proc_t *parent, proc_t *child)
{
	proc_t **ns;

	ASSERT(MUTEX_HELD(&pidlock));
	ASSERT(child->p_parent == parent);
	for (ns = &parent->p_child_ns; *ns != NULL; ns = &(*ns)->p_sibling_ns) {
		if (*ns == child) {

			ASSERT((*ns)->p_parent == parent);

			*ns = child->p_sibling_ns;
			child->p_sibling_ns = NULL;
			return;
		}
	}
}

/*
 * Add process "child" to the new state list of process "parent"
 */
void
add_ns(proc_t *parent, proc_t *child)
{
	ASSERT(child->p_sibling_ns == NULL);
	child->p_sibling_ns = parent->p_child_ns;
	parent->p_child_ns = child;
}
