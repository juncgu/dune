#include <stdio.h>
#include <stdlib.h>

#include "libdune/dune.h"
#include "libdune/cpu-x86.h"

#include <time.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <asm/prctl.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sched.h>
#include <linux/sched.h>
#include <sys/mman.h>
#include <pthread.h>
#include <err.h>
#include <linux/futex.h>
#include <linux/unistd.h>


// #define PAGE_SIZE ( 1<<12 )
#define PAGE_SIZE	4096
static char *page;
static unsigned long page_nr;
static unsigned long page_len;
static int msleep;
static char *data;

static pthread_mutex_t _syscall_mtx;

struct timespec ts[4];

void time_stamp(int i)
{
	// gettimeofday(ts+i, NULL);
	clock_gettime(CLOCK_MONOTONIC, ts + i);
}

void time_print(int start, int end)
{
        // printf("ts[%d]-- ts[%d]: %ld us\n", start, end, (long)(ts[end].tv_sec * 1000000 + ts[end].tv_usec) - (ts[start].tv_sec * 1000000 + ts[start].tv_usec));
        printf("ts[%d]-- ts[%d]: %ld ns\n", start, end, (long)(ts[end].tv_sec - ts[start].tv_sec) * 1000000000 + ts[end].tv_nsec - ts[start].tv_nsec);
}
uint32_t time_cal(int start, int end)
{
        return (uint32_t)((long)(ts[end].tv_sec - ts[start].tv_sec) * 1000000000 + ts[end].tv_nsec - ts[start].tv_nsec);
}

int cmp(const void *a,const void *b)
{
        return(*(uint32_t *)a-*(uint32_t *)b);
}

void usage(const char *argv0)
{
	fprintf(stderr, "usage:%s mem_size msleep\n", argv0);
	exit(1);
}

static void pgflt_handler(uintptr_t addr, uint64_t fec, struct dune_tf *tf)
{
	int ret;
	ptent_t *pte;

	time_stamp(0);
	// pthread_mutex_lock(&_syscall_mtx);
	ret = dune_vm_lookup(pgroot, (void *) addr, CREATE_NORMAL, &pte);
	assert(ret==0);
	long offset = 0;//for single page
	
	*pte = PTE_P | PTE_W | PTE_ADDR(dune_va_to_pa((void *) addr));
	memcpy((void *)addr, data, PAGE_SIZE);
	memcpy(data, (void *)addr, PAGE_SIZE);
	// pthread_mutex_unlock(&_syscall_mtx);
	time_stamp(1);
}

static void read_memory(void)
{
	unsigned long idx;
	unsigned long i;
	unsigned long offset;
	char *tmp_page;
	uint32_t *time_list;
	uint32_t cdfArray[101];
	time_list = (uint32_t *)malloc(page_nr * sizeof(uint32_t));
	memset(time_list, 0x00, page_nr * sizeof(uint32_t));

	if (posix_memalign((void **)&tmp_page, sysconf(_SC_PAGE_SIZE), PAGE_SIZE)){
		perror("Failed to memalign page\n");
		return;
	}
	memset(tmp_page, 0x00, PAGE_SIZE); 

	time_stamp(2);
	for (i = 0; i < page_nr; i++) {
		offset = PAGE_SIZE * i;
		// time_stamp(0);
		memcpy(tmp_page, page + offset, PAGE_SIZE);
		memcpy(page + offset, tmp_page, PAGE_SIZE);
		// time_stamp(1);
		time_list[i] = time_cal(0,1);
		if (msleep != 0){
			usleep(msleep);
		}
	}
	time_stamp(3);
	time_print(2, 3);

	qsort(time_list, page_nr, sizeof(uint32_t), cmp);
	cdfArray[0] = time_list[0];
	for (i = 1; i < 101; i++){
		idx = page_nr * 0.01 * i - 1;
		cdfArray[i] = time_list[idx];
	}
		
	printf("Start of cdf array:\n");
	for (i = 0; i < 101; i++) {
			printf("%u\n", cdfArray[i]);
	}
	printf("End of cdf array.\n");

	free(tmp_page);
	free(time_list);
}

int main(int argc, char *argv[])
{
	
	// int i,n;
	volatile int ret;

	ret = dune_init_and_enter();

	if (ret) {
		printf("failed to initialize dune\n");
		return ret;
	}

	dune_register_pgflt_handler(pgflt_handler);
	

	if (argc != 3){
		usage(argv[0]);
	}

	page_nr = (unsigned long)(atol(argv[1])) * 256 * 1024;
	page_len = PAGE_SIZE*page_nr;
	msleep = atoi(argv[2]);

	data = (char *)malloc(PAGE_SIZE);
	memset(data, 0x90, PAGE_SIZE);

	if (posix_memalign((void **)&page, PAGE_SIZE, page_len)) {
		perror("init page memory failed");
		return -1;
	}
	memset(page, 0x00, page_len);

	pthread_mutex_lock(&_syscall_mtx);
	//unmap physical address
	dune_vm_unmap(pgroot, (void *)page, page_len);
	pthread_mutex_unlock(&_syscall_mtx);

	read_memory();

	free(page);
	free(data);

	return 0;
}

