/*
 *   VUOS: view OS project
 *   Copyright (C) 2017,2018,2019 Renzo Davoli <renzo@cs.unibo.it>,
 *                                Antonio Cardace <anto.cardace@gmail.com>
 *   VirtualSquare team.
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sched.h>
#include <stddef.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>

#include <umvu_tracer.h>
#include <r_table.h>
#include <ptrace_defs.h>
#include <xcommon.h>
#include <vu_log.h>
#include <vu_inheritance.h>
#include <umvu_peekpoke.h>
#define DISABLE_VDSO

static int umvu_trace_legacy(pid_t tracee_tid);
static int umvu_trace_seccomp(pid_t tracee_tid);
static int (*umvu_trace)(pid_t) = umvu_trace_legacy;
static enum __ptrace_request ptrace_next_syscall = PTRACE_SYSCALL;

static int (*libc_pthread_create)();

static int nproc;
static pthread_mutex_t nproc_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t nproc_termination_cond = PTHREAD_COND_INITIALIZER;

static void default_syscall_handler(syscall_state_t state, struct syscall_descriptor_t *sd);
static syscall_handler_t syscall_handler = default_syscall_handler;

/* This BPF filter:
   returns SECCOMP_RET_ALLOW if the syscall is restart_syscall or poll(1, _, _).
   it returns SECCOMP_RET_TRACE otherwise. */
/* All syscalls are forwarded via ptrace to the hypervisor (but
   restart_syscall or the special case of poll to manage the
   hand-off between guardian angels) */
/* The hypervisor:
   * changes the syscall number to -1 to skip the syscall
   * returns PTRACE_CONT if the syscall is real (the kernel must process it)
   * returns PTRACE_SYSCALL if the kernel must process it but post processing is needed
 */

static struct sock_filter seccomp_filter[] = {
  BPF_STMT(BPF_LD+BPF_W+BPF_ABS, offsetof(struct seccomp_data, nr)),

  BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, __NR_restart_syscall, 0, 1),
  BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW),

  BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, __NR_poll, 0, 3),
  BPF_STMT(BPF_LD+BPF_W+BPF_ABS, offsetof(struct seccomp_data, args[0])),
  BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, 1, 0, 1),
  BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW),

  BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_TRACE),
};

static struct sock_fprog seccomp_prog = {
  .filter = seccomp_filter,
  .len = (unsigned short) (sizeof(seccomp_filter)/sizeof(seccomp_filter[0])),
};

#ifdef DISABLE_VDSO

#include <sys/auxv.h>
static void disable_vdso(pid_t tid) {
	struct user_regs_struct regs;
	struct syscall_descriptor_t sys_orig;
	uintptr_t addr;
	uintptr_t string;
	struct auxel {
		uintptr_t key;
		uintptr_t value;
	} auxel;
	P_GETREGS_NODIE(tid, &regs);
	umvu_peek_syscall(&regs, &sys_orig, PEEK_ARGS);
	//printk("disable_vdso %d %llx\n", tid, sys_orig.stack_pointer);
	string = -1;
	/* argv */
	for (addr = sys_orig.stack_pointer; string != 0; addr += sizeof(string)) {
		umvu_peek_data(addr, &string, sizeof(string));
		//printk("env %llx\n", string);
	}
	string = -1;
	/* environ */
	for (; string != 0; addr += sizeof(string)) {
		umvu_peek_data(addr, &string, sizeof(string));
		//printk("env %llx\n", key);
	}
	auxel.key = -1;
	for (; auxel.key != 0; addr += sizeof(auxel)) {
		umvu_peek_data(addr, &auxel, sizeof(auxel));
		//printk("env %llx %llx\n", auxel.key, auxel.value);
		if (auxel.key == AT_SYSINFO_EHDR) {
			auxel.value = 0;
			umvu_poke_data(addr, &auxel, sizeof(auxel));
			break;
		}
	}
}
#else
#define disable_vdso(x)
#endif

static void default_syscall_handler(syscall_state_t state, struct syscall_descriptor_t *sd) {
#if 0
	printf("trace %d - SCNO %d\n", state, sd->syscall_number);
	sd->action = DOIT_CB_AFTER;
	if (state == OUT_SYSCALL)
		sd->ret_value = sd->orig_ret_value;
#endif
}

static void nproc_update(int i) {
	pthread_mutex_lock(&nproc_mutex);
	nproc += i;
	if (nproc == 0)
		pthread_cond_broadcast(&nproc_termination_cond);
	pthread_mutex_unlock(&nproc_mutex);
}

static void wait4termination(void) {
	pthread_mutex_lock(&nproc_mutex);
	while (nproc > 0)
		pthread_cond_wait(&nproc_termination_cond, &nproc_mutex);
	pthread_mutex_unlock(&nproc_mutex);
}

/* struct definitions */
typedef struct tracer_args {
	pid_t tracee_tid;
	struct user_regs_struct regs;
	void *inherited_args[];
} tracer_args;

static void unblock_tracee(pid_t tid, struct user_regs_struct *regs)
{
	P_INTERRUPT(tid, 0L);
	r_wait4(tid, NULL, __WALL, NULL);
	P_SETREGS(tid, regs);
	// P_SYSCALL if legacy, P_CONT if SECCOMP
	PTRACE(ptrace_next_syscall, tid, 0L);
}

static void *spawn_tracer(void *arg)
{
	tracer_args *t_arg = (tracer_args *)arg;
	pid_t tracee_tid = t_arg->tracee_tid;
	umvu_settid(tracee_tid);
	nproc_update(1);
	vu_inheritance_call(INH_START, t_arg->inherited_args, NULL);

	P_SEIZE_NODIE(tracee_tid, PTRACE_STD_OPTS);
	unblock_tracee(tracee_tid, &(t_arg->regs));
	free(t_arg);
	umvu_trace(tracee_tid);
	return NULL;
}

static void block_tracee(pid_t tid, struct user_regs_struct *regs)
{
	struct syscall_descriptor_t sys_orig, sys_modified;
	P_GETREGS_NODIE(tid, regs);
	umvu_peek_syscall(regs, &sys_orig, PEEK_ARGS);
	sys_modified = sys_orig;
	/* change syscall to poll(NULL, 0, -1);
	 * actually it uses poll((struct pollfd *)1, 0, -1):
	 *        the first arg is ignored as the second is zero.
	 *        the first arg is a tag for the BPF program */
	sys_modified.syscall_number = __NR_poll;
	sys_modified.syscall_args[0] = 1;
	sys_modified.syscall_args[1] = 0;
	sys_modified.syscall_args[2] = -1;
	umvu_poke_syscall(regs, &sys_modified, POKE_ARGS);
	P_SETREGS_NODIE(tid, regs);
	sys_orig.prog_counter -= SYSCALL_INSTRUCTION_LEN;
	umvu_poke_syscall(regs, &sys_orig, POKE_ARGS);
}

static void transfer_tracee(pid_t newtid, syscall_arg_t clone_flags)
{
	pthread_t newthread;
	pthread_attr_t thread_attr;
	tracer_args *t_args = (tracer_args *)
		malloc(sizeof(tracer_args) + vu_inheritance_inout_size());
	struct user_regs_struct *regs;

	fatal(t_args);
	regs = &(t_args->regs);
	block_tracee(newtid, regs);
	/*init args for new thread*/
	t_args->tracee_tid = newtid;
	vu_inheritance_call(INH_CLONE, t_args->inherited_args, &clone_flags);
	P_DETACH_NODIE(newtid, 0L);
	pthread_attr_init(&thread_attr);
	pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
	libc_pthread_create(&newthread, &thread_attr, &spawn_tracer, t_args);
	pthread_attr_destroy(&thread_attr);
}

/* umvu_trace code is replicated for performance issues and
	 readability */
static int umvu_trace_legacy(pid_t tracee_tid)
{
	int wstatus, sig_tid;
	syscall_state_t syscall_state = IN_SYSCALL;
	struct user_regs_struct regs;
	struct syscall_descriptor_t syscall_desc = {.action = DOIT, .inout = NULL};
	syscall_arg_t clone_flags;
	//printk("new thread for %d\n", tracee_tid);
	while (1) {
		sig_tid = r_wait4(-1, &wstatus, __WALL | __WNOTHREAD, NULL);
		if (sig_tid == -1) {
			perror("r_wait4 -1");
			vu_inheritance_call(INH_TERMINATE, NULL, NULL);
			nproc_update(-1);
			return -1;
		} else if (WIFSTOPPED(wstatus)) {
			if (WSTOPSIG(wstatus) == SIGTRAP) {
				if (wstatus >> 8 == (SIGTRAP | (PTRACE_EVENT_EXIT << 8))) {
					/* tracee is about to exit */
					unsigned long exit_status;
					P_GETEVENTMSG(sig_tid, &exit_status);
					P_DETACH(sig_tid, 0L);
					vu_inheritance_call(INH_TERMINATE, NULL, NULL);
					nproc_update(-1);
					return exit_status;
				} else if (wstatus >> 8 ==
						(SIGTRAP | (PTRACE_EVENT_EXEC << 8))) {
					/* the tracee is doing execve() */
					/* if a thread which is not the thread group leader performs
					 * an execve() his tid become equal to the thread group leader,
					 * we must update tracee_tid otherwise a execve could be mistaken for
					 * a clone() */
					tracee_tid = sig_tid;
					disable_vdso(tracee_tid);
					vu_inheritance_call(INH_EXEC, NULL, NULL);
					//printf("exec %d\n", tracee_tid);
				}
				else if (wstatus >> 8 == (SIGTRAP | (PTRACE_EVENT_CLONE << 8)) ||
						wstatus >> 8 == (SIGTRAP | (PTRACE_EVENT_VFORK << 8)) ||
						wstatus >> 8 == (SIGTRAP | (PTRACE_EVENT_FORK << 8))) {
					/* the tracee is doing a clone */
					clone_flags = syscall_desc.syscall_args[0];
				}
				P_SYSCALL(sig_tid, 0L);
			} else if (sig_tid != tracee_tid) {
				/*new tracee*/
				if (wstatus >> 16 == PTRACE_EVENT_STOP) {
					P_SYSCALL_NODIE(sig_tid, 0L);
				} else {
					transfer_tracee(sig_tid, clone_flags);
				}
			} else if (WSTOPSIG(wstatus) == (SIGTRAP | 0x80)) {
				/*SYSCALL*/
				if (syscall_state == IN_SYSCALL) {
					syscall_desc.action = DOIT;
					syscall_desc.waiting_pid = 0;
					P_GETREGS(sig_tid, &regs);
					umvu_peek_syscall(&regs, &syscall_desc, PEEK_ARGS);
					syscall_handler(syscall_state, &syscall_desc);
					if (syscall_desc.action & UMVU_BLOCKIT) {
						struct syscall_descriptor_t sys_modified = syscall_desc;
						umvu_block(&sys_modified);
						umvu_poke_syscall(&regs, &sys_modified, POKE_ARGS);
						P_SETREGS(sig_tid, &regs);
					} else {
						if (syscall_desc.action & UMVU_SKIP)
							syscall_desc.syscall_number = __NR_getpid;
						if (umvu_poke_syscall(&regs, &syscall_desc, POKE_ARGS))
							P_SETREGS(sig_tid, &regs);
					}
					P_SYSCALL(sig_tid, 0L);
					if (syscall_desc.action & UMVU_CB_AFTER) {
						syscall_state = DURING_SYSCALL;
						syscall_handler(syscall_state, &syscall_desc);
					}
					syscall_state = OUT_SYSCALL;
				} else { /* OUT_SYSCALL */
					if (syscall_desc.action != DOIT) {
						if (syscall_desc.waiting_pid != 0)
							r_kill(syscall_desc.waiting_pid, SIGKILL);
						P_GETREGS(sig_tid, &regs);
						umvu_peek_syscall(&regs, &syscall_desc, PEEK_RETVALUE);
						if (syscall_desc.action & UMVU_CB_AFTER)
							syscall_handler(syscall_state, &syscall_desc);
						if (syscall_desc.action & UMVU_DO_IT_AGAIN) {
							  syscall_desc.prog_counter -= SYSCALL_INSTRUCTION_LEN;
								umvu_poke_syscall(&regs, &syscall_desc, POKE_ARGS);
								P_SETREGS(sig_tid, &regs);
						}
						else {
							syscall_desc.inout = NULL;
							if (umvu_poke_syscall(&regs, &syscall_desc, POKE_RETVALUE))
								P_SETREGS(sig_tid, &regs);
						}
					}
					syscall_state = IN_SYSCALL;
					syscall_desc.waiting_pid = 0;
					P_SYSCALL(sig_tid, 0L);
				}
			} else {
				/*group-stop or signal injection*/
				P_SYSCALL(sig_tid, WSTOPSIG(wstatus));
			}
		} else {
			//printk("waiting_pid? %d %d\n", sig_tid, syscall_desc.waiting_pid);
			if (sig_tid == syscall_desc.waiting_pid) {
				umvu_unblock();
				syscall_desc.waiting_pid = 0;
			}
		}
	}
}

static int umvu_trace_seccomp(pid_t tracee_tid)
{
	int wstatus, sig_tid;
	struct user_regs_struct regs;
	struct syscall_descriptor_t syscall_desc = {.action = DOIT, .inout = NULL};
	syscall_arg_t clone_flags;
	//printk("new seccomp thread for %d\n", tracee_tid);
	while (1) {
		sig_tid = r_wait4(-1, &wstatus, __WALL | __WNOTHREAD, NULL);
		if (sig_tid == -1) {
			perror("r_wait4 -1");
			vu_inheritance_call(INH_TERMINATE, NULL, NULL);
			nproc_update(-1);
			return -1;
		} else if (WIFSTOPPED(wstatus)) {
			if (WSTOPSIG(wstatus) == SIGTRAP) {
				if (wstatus >> 8 == (SIGTRAP | (PTRACE_EVENT_SECCOMP << 8))) {
					if (sig_tid != tracee_tid) {
						transfer_tracee(sig_tid, clone_flags);
					} else {
						syscall_desc.action = DOIT;
						syscall_desc.waiting_pid = 0;
						P_GETREGS(sig_tid, &regs);
						umvu_peek_syscall(&regs, &syscall_desc, PEEK_ARGS);
						syscall_handler(IN_SYSCALL, &syscall_desc);
						if (syscall_desc.action & UMVU_BLOCKIT) {
							struct syscall_descriptor_t sys_modified = syscall_desc;
							umvu_block(&sys_modified);
							umvu_poke_syscall(&regs, &sys_modified, POKE_ARGS);
							P_SETREGS(sig_tid, &regs);
						} else if (syscall_desc.action & UMVU_SKIP) {
							umvu_poke_syscall(&regs, &syscall_desc, SKIP_SETRETVALUE);
							P_SETREGS(sig_tid, &regs);
						} else {
							if (umvu_poke_syscall(&regs, &syscall_desc, POKE_ARGS))
								P_SETREGS(sig_tid, &regs);
						}
						if (syscall_desc.action & UMVU_CB_AFTER) {
							P_SYSCALL(sig_tid, 0L);
							syscall_handler(DURING_SYSCALL, &syscall_desc);
						} else
							P_CONT(sig_tid, 0L);
					}
				} else if (wstatus >> 8 == (SIGTRAP | (PTRACE_EVENT_EXIT << 8))) {
					/* tracee is about to exit */
					unsigned long exit_status;
					P_GETEVENTMSG(sig_tid, &exit_status);
					P_DETACH(sig_tid, 0L);
					vu_inheritance_call(INH_TERMINATE, NULL, NULL);
					nproc_update(-1);
					return exit_status;
				} else if (wstatus >> 8 ==
						(SIGTRAP | (PTRACE_EVENT_EXEC << 8))) {
					/* the tracee is doing execve() */
					/* if a thread which is not the thread group leader performs
					 * an execve() his tid become equal to the thread group leader,
					 * we must update tracee_tid otherwise a execve could be mistaken for
					 * a clone() */
					tracee_tid = sig_tid;
					disable_vdso(tracee_tid);
					vu_inheritance_call(INH_EXEC, NULL, NULL);
					P_CONT(sig_tid, 0L);
				} else if (wstatus >> 8 == (SIGTRAP | (PTRACE_EVENT_CLONE << 8)) ||
						wstatus >> 8 == (SIGTRAP | (PTRACE_EVENT_VFORK << 8)) ||
						wstatus >> 8 == (SIGTRAP | (PTRACE_EVENT_FORK << 8))) {
					/* the tracee is doing a clone */
					clone_flags = syscall_desc.syscall_args[0];
					P_CONT(sig_tid, 0L);
				} else
					P_CONT(sig_tid, 0L);
				/*
				 * PTRACE_O_SYSGOOD option was set by the tracer, so when
				 * the syscall-exit-stop event occurs, WSTOPSIG(wstatus) will
				 * be equal to (SIGTRAP | 0x80) to be able to tell it apart
				 * from the syscall-enter-stop event o normal traps
				 * */
			} else if (WSTOPSIG(wstatus) == (SIGTRAP | 0x80)) {
				if (syscall_desc.waiting_pid != 0)
					r_kill(syscall_desc.waiting_pid, SIGKILL);
				P_GETREGS(sig_tid, &regs);
				umvu_peek_syscall(&regs, &syscall_desc, PEEK_RETVALUE);
				if (syscall_desc.action & UMVU_CB_AFTER)
					syscall_handler(OUT_SYSCALL, &syscall_desc);
				if (syscall_desc.action & UMVU_DO_IT_AGAIN) {
					syscall_desc.prog_counter -= SYSCALL_INSTRUCTION_LEN;
					umvu_poke_syscall(&regs, &syscall_desc, POKE_ARGS);
					P_SETREGS(sig_tid, &regs);
				} else {
					/* If the SC's return value has been modified by the wrap
					 * function, update the corresponding register
					 * */
					if (umvu_poke_syscall(&regs, &syscall_desc, POKE_RETVALUE))
						P_SETREGS(sig_tid, &regs);
					syscall_desc.inout = NULL;
				}
				syscall_desc.waiting_pid = 0;
				P_CONT(sig_tid, 0L);
			} else {
				/*group-stop or signal injection*/
				P_CONT(sig_tid, WSTOPSIG(wstatus));
			}
		} else {
			//printk("waiting_pid? %d %d\n", sig_tid, syscall_desc.waiting_pid);
			if (sig_tid == syscall_desc.waiting_pid) {
				umvu_unblock();
				syscall_desc.waiting_pid = 0;
			}
		}
	}
}

int umvu_tracepid(pid_t childpid, syscall_handler_t syscall_handler_arg, int main) {
	int wstatus;
	nproc_update(1);
	P_SEIZE(childpid, PTRACE_STD_OPTS);
	// P_SYSCALL if legacy, P_CONT if SECCOMP
	PTRACE(ptrace_next_syscall, childpid, 0L);
	if (syscall_handler_arg != NULL)
		syscall_handler = syscall_handler_arg;
	umvu_settid(childpid);
	wstatus = umvu_trace(childpid);
	if (main)
		wait4termination();
	return wstatus;
}

int umvu_tracer_fork(int seccomp) {
	pid_t childpid;

	childpid = r_fork();
	switch (childpid) {
		case 0:
			/* child */
			raise(SIGSTOP);
			if (seccomp) {
				if (r_prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == -1) {
					perror("prctl(PR_SET_NO_NEW_PRIVS)");
					return -1;
				}
				if (r_seccomp(SECCOMP_SET_MODE_FILTER, 0, &seccomp_prog) == -1) {
					perror("when setting seccomp filter");
					return -1;
				}
			}
			return 0;
		default:
			/*parent*/
			if (seccomp) {
				umvu_trace = umvu_trace_seccomp;
				ptrace_next_syscall = PTRACE_CONT;
			}
			r_wait4(-1, NULL, WUNTRACED, NULL);
			return childpid;
		case -1:
			return -1;
	}
}

int umvu_tracer_test_seccomp(void) {
  pid_t childpid;
  int status;
  struct sock_filter filter[] = {
    BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW),
  };
  struct sock_fprog prog = {
    .filter = filter,
    .len = (unsigned short) (sizeof(filter)/sizeof(filter[0])),
  };

  childpid = r_fork();
  switch (childpid) {
  case 0:
    /*child*/
    if (r_prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == -1)
      exit(errno);
    if (r_seccomp(SECCOMP_SET_MODE_FILTER, 0, &prog) == -1)
      exit(errno);
    exit(0);
  default:
    r_wait4(childpid, &status, 0, NULL);
    if (WEXITSTATUS(status) != 0) {
      errno = WEXITSTATUS(status);
      return -1;
    } else
      return 0;
  case -1:
    return -1;
  }
}

__attribute__((constructor))
	static void init(void) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
		libc_pthread_create = dlsym (RTLD_NEXT, "pthread_create");
#pragma GCC diagnostic pop
	}

