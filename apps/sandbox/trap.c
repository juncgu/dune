/*
 * trap.c - handles system calls, page faults, and other traps
 */

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/syscall.h>
#include <asm/prctl.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <utime.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "sandbox.h"
#include "boxer.h"

static boxer_syscall_cb _syscall_monitor;

static void
pgflt_handler(uintptr_t addr, uint64_t fec, struct dune_tf *tf)
{
	int ret;
	ptent_t *pte;
	bool was_user = (tf->cs & 0x3);

	if (was_user) {
		printf("sandbox: got unexpected G3 page fault at addr %lx, fec %lx\n",
		       addr, fec);
		dune_dump_trap_frame(tf);
		dune_ret_from_user(-EFAULT);
	} else {
		ret = dune_vm_lookup(pgroot, (void *) addr, CREATE_NORMAL, &pte);
		assert(!ret);
		*pte = PTE_P | PTE_W | PTE_ADDR(dune_va_to_pa((void *) addr));
	}
}

static int check_extent(const void *ptr, size_t len)
{
	if (!mem_ref_is_safe(ptr, len)) {
		printf("sandbox: mem ref with addr %p, len %lx is out of range\n",
		       ptr, len);
		return -EFAULT;
	}

	return 0;
}

static int check_string(const void *ptr)
{
	void *pos;
	size_t maxlen;

	if ((uintptr_t) ptr < APP_MAX_ELF_VADDR)
		maxlen = APP_MAX_ELF_VADDR - ((uintptr_t) ptr);
	else if ((uintptr_t) ptr >= mmap_base) {
		if ((uintptr_t) ptr >= mmap_base + APP_MMAP_LEN)
			goto fault;
		maxlen = mmap_base + APP_MMAP_LEN - ((uintptr_t) ptr);
	} else
		goto fault;

	pos = memchr(ptr, 0, maxlen);
	if (!pos)
		goto fault;

	return 0;

fault:
	printf("str ref addr %p is out of range\n", ptr);
	return -EFAULT;
}

static inline long get_err(long ret)
{
	if (ret < 0)
		return -errno;
	else
		return ret;
}

void boxer_register_syscall_monitor(boxer_syscall_cb cb)
{
	_syscall_monitor = cb;
}

static int check_iovec(struct iovec *iov, int num)
{
	if (check_extent(iov, sizeof(*iov) * num))
		return -EFAULT;

	while (num--) {
		if (check_extent(iov->iov_base, iov->iov_len))
			return -EFAULT;

		iov++;
	}

	return 0;
}

static int syscall_check_params(struct dune_tf *tf)
{
	void *ptr = NULL;
	uint64_t len = 0;
	char *str = NULL;
	int err = 0;

	switch (tf->rax) {
	case SYS_uname:
		ptr = (void*) ARG0(tf);
		len = sizeof(struct utsname);
		break;

	case SYS_arch_prctl:
		if (ARG0(tf) == ARCH_GET_FS) {
			ptr = (void*) ARG1(tf);
			len = sizeof(unsigned long);
		}
		break;

	case SYS_access:
	case SYS_open:
	case SYS_unlink:
		str = (char*) ARG0(tf);
		break;

	case SYS_openat:
		str = (char*) ARG1(tf);
		break;

	case SYS_getdents:
	case SYS_read:
	case SYS_write:
	case SYS_connect:
	case SYS_bind:
		ptr = (void*) ARG1(tf);
		len = ARG2(tf);
		break;

	case SYS_writev:
	case SYS_readv:
		err = check_iovec((struct iovec*) ARG1(tf), ARG2(tf));
		break;

	case SYS_stat:
	case SYS_lstat:
		str = (char*) ARG0(tf);
	case SYS_fstat:
		ptr = (void*) ARG1(tf);
		len = sizeof(struct stat);
		break;

	case SYS_statfs:
		str = (char*) ARG0(tf);
		ptr = (void*) ARG1(tf);
		len = sizeof(struct statfs);
		break;

	case SYS_time:
		ptr = (void*) ARG0(tf);
		len = sizeof(time_t);
		break;

	case SYS_epoll_ctl:
		ptr = (void*) ARG3(tf);
		len = sizeof(struct epoll_event);
		break;

	case SYS_epoll_wait:
		ptr = (void*) ARG1(tf);
		len = ARG2(tf) * sizeof(struct epoll_event);
		break;

	case SYS_setsockopt:
		ptr = (void*) ARG3(tf);
		len = ARG4(tf);
		break;

	case SYS_accept:
		if (ARG2(tf)) {
			socklen_t *l = (socklen_t*) ARG2(tf);

			if (check_extent(l, sizeof(*l)))
				err = -EFAULT;
			else {
				len = *l;
				ptr = (void*) ARG1(tf);
				assert(ptr);
			}
		}
		break;

	case SYS_fcntl:
		switch (ARG1(tf)) {
		case F_DUPFD:
		case F_DUPFD_CLOEXEC:
		case F_GETFD:
		case F_SETFD:
		case F_GETFL:
		case F_SETFL:
		case F_GETOWN:
		case F_SETOWN:
			break;

		default:
			err = -EFAULT;
			break;
		}
		break;

	case SYS_ioctl:
		/* XXX unreliable */
		if (_IOC_DIR(ARG1(tf))) {
			ptr = (void*) ARG2(tf);
			len = _IOC_SIZE(ARG1(tf));
		}
		break;

	case SYS_setgroups:
		ptr = (void*) ARG1(tf);
		len = sizeof(gid_t) * ARG0(tf);
		break;

	case SYS_rt_sigaction:
		ptr = (void*) ARG1(tf);
		len = sizeof(struct sigaction);

		if (ARG2(tf)) {
			if (check_extent((void*) ARG2(tf), len)) {
				tf->rax = -EFAULT;
				return -1;
			}
		}
		break;

	/* umm_ checks for correctness */
	case SYS_brk:
		break;

	case SYS_mprotect:
	case SYS_munmap:
	case SYS_mmap:
//		ptr = (void*) ARG0(tf);
//		len = ARG1(tf);
		break;

	case SYS_getcwd:
		ptr = (void*) ARG0(tf);
		len = ARG1(tf);
		break;

	case SYS_getrlimit:
		ptr = (void*) ARG1(tf);
		len = sizeof(struct rlimit);
		break;

	case SYS_sendfile:
		ptr = (void*) ARG2(tf);
		len = sizeof(off_t);
		break;

	case SYS_getuid:
	case SYS_setuid:
	case SYS_getgid:
	case SYS_setgid:
	case SYS_epoll_create:
	case SYS_dup2:
	case SYS_getpid:
	case SYS_socket:
	case SYS_shutdown:
	case SYS_listen:
	case SYS_lseek:
		break;

	/* XXX - doesn't belong here */
	case SYS_close:
		if (ARG0(tf) < 3) {
			tf->rax = 0;
			return -1;
		}
		break;
#if 0
	default:
		{
			static FILE *_out;

			if (!_out)
				_out = fopen("/tmp/syscall.log", "w");

			fprintf(_out, "Syscall %d\n", tf->rax);
			fflush(_out);
		}
		break;
#endif
	}

	if (ptr != NULL && len != 0 && check_extent(ptr, len))
		err = -EFAULT;

	if (str != NULL && check_string(str))
		err = -EFAULT;

	if (err) {
		tf->rax = err;
		return -1;
	}

	return 0;
}

static int syscall_allow(struct dune_tf *tf)
{
	if (!_syscall_monitor) {
		tf->rax = -EPERM;
		return 0;
	}

	return _syscall_monitor(tf);
}

static void syscall_do(struct dune_tf *tf)
{
	switch (tf->rax) {
	case SYS_arch_prctl:
		switch (ARG0(tf)) {
		case ARCH_GET_FS:
			*((unsigned long*) ARG1(tf)) = dune_get_user_fs();
			tf->rax = 0;
			break;

		case ARCH_SET_FS:
			dune_set_user_fs(ARG1(tf));
			tf->rax = 0;
			break;

		default:
			tf->rax = -EINVAL;
			break;
		}
		break;

	case SYS_brk:
		tf->rax = umm_brk((unsigned long) ARG0(tf));
		break;

	case SYS_mmap:
		tf->rax = (unsigned long) umm_mmap((void *) ARG0(tf),
			(size_t) ARG1(tf), (int) ARG2(tf), (int) ARG3(tf),
			(int) ARG4(tf), (off_t) ARG5(tf));
		break;

	case SYS_mprotect:
		tf->rax = umm_mprotect((void *) ARG0(tf),
				       (size_t) ARG1(tf),
				       ARG2(tf));
		break;

	case SYS_munmap:
		tf->rax = umm_munmap((void *) ARG0(tf), (size_t) ARG1(tf)); 
		break;

	case SYS_exit_group:
	case SYS_exit:
		dune_ret_from_user(ARG0(tf));
		break;

	default:
		dune_passthrough_syscall(tf);
		break;
	}
}

static void syscall_handler(struct dune_tf *tf)
{
	if (syscall_check_params(tf) == -1)
		return;

	if (!syscall_allow(tf))
		return;

	syscall_do(tf);
}

int trap_init(void)
{
	dune_register_pgflt_handler(pgflt_handler);
	dune_register_syscall_handler(&syscall_handler);

	return 0;
}
