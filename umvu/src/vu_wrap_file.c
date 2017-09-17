#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <dirent.h>

#include <linux_32_64.h>
#include <vu_log.h>
#include <r_table.h>
#include <umvu_peekpoke.h>
#include <hashtable.h>
#include <arch_table.h>
#include <syscall_defs.h>
#include <vu_execute.h>
#include <service.h>
#include <path_utils.h>
#include <vu_fs.h>
#include <vu_file_table.h>
#include <vu_fd_table.h>
#include <vu_wrapper_utils.h>

/* open, creat, openat */
void wi_open(struct hashtable_obj_t *ht, struct syscall_descriptor_t *sd) {
	int nested = sd->extra->nested;
	if (ht) {
		/* standard args */
		struct vu_service_t *service = ht_get_service(ht);
		int syscall_number = sd->syscall_number;
		int ret_value;
		/* args */
		int flags;
		int mode;
		void *private = NULL;
		/* local bufs */
		/* fetch args */
		switch (syscall_number) {
			case __NR_open:
				flags = sd->syscall_args[1];
				mode = (flags & O_CREAT) || (flags & O_TMPFILE) ?
					sd->syscall_args[2] : 0;
				break;
			case __NR_creat:
				flags = O_CREAT|O_WRONLY|O_TRUNC;
				mode = sd->syscall_args[1];
				break;
			case __NR_openat:
				flags = sd->syscall_args[2];
				mode = (flags & O_CREAT) || (flags & O_TMPFILE) ?
					sd->syscall_args[3] : 0;
		}
		mode = mode & ~vu_fs_get_umask();
		/* call */
		sd->action = SKIP;
		ret_value = service->module_syscall[__VU_open](sd->extra->path, flags, mode, &private);
		if (ret_value < 0) {
			sd->ret_value = -errno;
			return;
		} else {
			struct fnode_t *fnode;
			if (sd->extra->statbuf.st_mode == 0) /* new file just created */
				service->module_syscall[__VU_lstat](sd->extra->path, &sd->extra->statbuf, 0, ret_value, private);
			fnode = vu_fnode_create(ht, sd->extra->path, &sd->extra->statbuf, flags, ret_value, private); 
			if (nested) {
				/* do not use DOIT_CB_AFTER: open must be real, not further virtualized */
				int fd;
				sd->ret_value = fd = r_open(vu_fnode_get_vpath(fnode), O_CREAT | O_RDWR, 0600);
				if (fd >= 0)
					vu_fd_set_fnode(fd, nested, fnode, flags);
				else
					vu_fnode_close(fnode);
			} else {
				sd->inout = fnode;
				sd->ret_value = ret_value;
				/* change the call to "open(vopen, O_CREAT | O_RDWR, 0600)" */
				sd->syscall_number = __NR_open;
				rewrite_syspath(sd, vu_fnode_get_vpath(fnode));
				sd->syscall_args[1] = O_CREAT | O_RDWR | (flags & O_CLOEXEC);
				sd->syscall_args[2] = 0600;
				sd->action = DOIT_CB_AFTER;
			}
		}
	} else if (!nested) {
		sd->action = DOIT_CB_AFTER;
	}
}

void wo_open(struct hashtable_obj_t *ht, struct syscall_descriptor_t *sd) {
	int fd = sd->orig_ret_value;
	if (ht) {
		struct fnode_t *fnode = sd->inout;
		int fdflags = sd->syscall_args[1] & O_CLOEXEC ? FD_CLOEXEC : 0;
		if (fd >= 0) {
			vu_fd_set_fnode(fd, VU_NOT_NESTED, fnode, fdflags);
		} else {
			vu_fnode_close(fnode);
		}
	} else {
		if (fd >= 0) {
			struct vu_fnode_t *fnode;
			/* are flags/mode needed for non virtualized files? */
			int fdflags;
			switch (sd->syscall_number) {
				case __NR_open: fdflags = sd->syscall_args[1] & O_CLOEXEC ? FD_CLOEXEC : 0;
												break;
				case __NR_openat: fdflags = sd->syscall_args[1] & O_CLOEXEC ? FD_CLOEXEC : 0;
													break;
				default: fdflags = 0;
			}
			fnode = vu_fnode_create(NULL, sd->extra->path, NULL, 0, -1, NULL); 
			vu_fd_set_fnode(fd, VU_NOT_NESTED, fnode, fdflags);
		}
	}
	sd->ret_value = sd->orig_ret_value;
}

/* close */
void wi_close(struct hashtable_obj_t *ht, struct syscall_descriptor_t *sd) {
	  int nested = sd->extra->nested;
		if (nested) {
			/* do not use DOIT_CB_AFTER: close must be real, not further virtualized */
			if (ht) {
				int fd = sd->syscall_args[0];
				int ret_value = vu_fd_close(fd, VU_NESTED);
				sd->ret_value = ret_value < 0 ? -errno : 0;
				r_close(fd);
				sd->action = SKIP;
			}
		} else {
			sd->action = DOIT_CB_AFTER;
		}
}

void wo_close(struct hashtable_obj_t *ht, struct syscall_descriptor_t *sd) {
	int fd = sd->syscall_args[0];
	int ret_value = sd->orig_ret_value;
	if (ret_value >= 0) 
		vu_fd_close(fd, VU_NOT_NESTED);
	sd->ret_value = sd->orig_ret_value;
}

static int fnode_close_upcall(struct hashtable_obj_t *ht, int sfd, void *private) {
  if (ht) {
    struct vu_service_t *service = ht_get_service(ht);
    return service->module_syscall[__VU_close](sfd, private);
  } else
    return 0;
}

/* read, readv */
void wi_read(struct hashtable_obj_t *ht, struct syscall_descriptor_t *sd) {
	if (ht) {
		int fd = sd->syscall_args[0];
		struct vu_service_t *service = ht_get_service(ht);
		int nested = sd->extra->nested;
		void *private = NULL;
		if (sd->syscall_number == __NR_read) {
			int sfd = vu_fd_get_sfd(fd, &private, nested);
			uintptr_t addr =  sd->syscall_args[1];
			size_t bufsize = sd->syscall_args[2];
			void *buf; 
			ssize_t ret_value;
			vu_alloc_arg(addr, buf, bufsize, nested);
			ret_value = service->module_syscall[__VU_read](sfd, buf, bufsize, private);
			if (ret_value < 0)
				sd->ret_value = -errno;
			else {
				sd->ret_value = ret_value;
				if (ret_value > 0)
					vu_poke_arg(addr, buf, ret_value, nested);
			}
			vu_free_arg(buf, nested);
		} else { // readv
			sd->ret_value = -ENOSYS; // XXX TBD
		}
		sd->action = SKIP;
	}
}

/* write, writev */
void wi_write(struct hashtable_obj_t *ht, struct syscall_descriptor_t *sd) {
	if (ht) {
		int fd = sd->syscall_args[0];
		struct vu_service_t *service = ht_get_service(ht);
		int nested = sd->extra->nested;
		void *private = NULL;
		if (sd->syscall_number == __NR_write) {
			int sfd = vu_fd_get_sfd(fd, &private, nested);
			uintptr_t addr =  sd->syscall_args[1];
			size_t bufsize = sd->syscall_args[2];
			void *buf;
			ssize_t ret_value;
			vu_peek_alloc_arg(addr, buf, bufsize, nested);
			ret_value = service->module_syscall[__VU_write](sfd, buf, bufsize, private);
			vu_free_arg(buf, nested);
			if (ret_value < 0)
				sd->ret_value = -errno;
			else 
				sd->ret_value = ret_value;
		} else { // writev
			sd->ret_value = -ENOSYS; // XXX TBD
		}
		sd->action = SKIP;
	}
}

/* getdents64, getdents */
void wi_getdents64(struct hashtable_obj_t *ht, struct syscall_descriptor_t *sd) {
	if (ht) {
		int fd = sd->syscall_args[0];
		struct vu_service_t *service = ht_get_service(ht);
		int nested = sd->extra->nested;
		void *private = NULL;
		int sfd = vu_fd_get_sfd(fd, &private, nested);
		uintptr_t addr =  sd->syscall_args[1];
		unsigned int bufsize = sd->syscall_args[2];
		void *buf;
		int ret_value;
		vu_alloc_arg(addr, buf, bufsize, nested);
		ret_value = service->module_syscall[__VU_getdents64](sfd, buf, bufsize, private);
		if (ret_value < 0)
			sd->ret_value = -errno;
		else {
			if (sd->syscall_number == __NR_getdents)
				dirent64_to_dirent(buf, ret_value);
			vu_poke_arg(addr, buf, ret_value, nested);
			sd->ret_value = ret_value;
		}
		vu_free_arg(buf, nested);
		sd->action = SKIP;
	}
}

/* dup, dup2, dup3 */
void wi_dup3(struct hashtable_obj_t *ht, struct syscall_descriptor_t *sd) {
	int nested = sd->extra->nested;
	if (nested) {
		int fd = sd->syscall_args[0];
		int newfd;
		int flags;
		switch (sd->syscall_number) {
			case __NR_dup: newfd = r_dup(fd);
										 break;
			case __NR_dup2: newfd = sd->syscall_args[1];
											newfd = r_dup2(fd, newfd);
											break;
			case __NR_dup3: newfd = sd->syscall_args[1];
											flags = sd->syscall_args[2];
											newfd = r_dup3(fd, newfd, flags);
		}
		if (newfd < 0)
			sd->ret_value = -errno;
		else {
			if (newfd != fd)
				vu_fd_dup(newfd, VU_NESTED, fd, flags);
			sd->ret_value = newfd;
		}
	} else {
		sd->action = DOIT_CB_AFTER;
	}
}

void wo_dup3(struct hashtable_obj_t *ht, struct syscall_descriptor_t *sd) {
	int newfd = sd->orig_ret_value;
	int fd = sd->syscall_args[0];
	if (newfd >= 0 && fd != newfd) { //dup2 does nothing if fd == newfd
		int flags = 0;
		if (sd->syscall_number == __NR_dup3)
			flags = sd->syscall_args[2];
		vu_fd_dup(newfd, VU_NOT_NESTED, fd, flags);
	}
	sd->ret_value = newfd;
}

/* umask */
/* umask always succeeds. just copy the value */
void wi_umask(struct hashtable_obj_t *ht, struct syscall_descriptor_t *sd) {
	int nested = sd->extra->nested;
	if (!nested) {
		int umask = sd->syscall_args[0];
		vu_fs_set_umask(umask);
	}
}

/* lseek */
void wi_lseek(struct hashtable_obj_t *ht, struct syscall_descriptor_t *sd) {
	if (ht) {
		/* standard args */
		struct vu_service_t *service = ht_get_service(ht);
		int nested = sd->extra->nested;
		off_t ret_value;
		/* args */
		int fd = sd->syscall_args[0];
		void *private = NULL;
    int sfd = vu_fd_get_sfd(fd, &private, nested);
		off_t offset = sd->syscall_args[1];
		int whence = sd->syscall_args[2];
		/* call */
		sd->action = SKIP;
		ret_value = service->module_syscall[__VU_lseek](sfd, offset, whence, private);
		if (ret_value < 0) 
			sd->ret_value = -errno;
		else
			sd->ret_value = ret_value;
	}
}

__attribute__((constructor))
  static void init(void) {
    vu_fnode_set_close_upcall(fnode_close_upcall);
  }

