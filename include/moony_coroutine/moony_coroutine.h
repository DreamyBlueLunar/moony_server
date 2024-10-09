#ifndef MOONY_COROUTINE_H_
#define MOONY_COROUTINE_H_

#include <dlfcn.h>

#define _USE_UCONTEXT

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <climits>
#include <cassert>
#include <cinttypes>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <netinet/tcp.h>

#ifdef _USE_UCONTEXT
#include <ucontext.h>
#endif

#include <sys/epoll.h>
#include <sys/poll.h>

#include <cerrno>

#include "moony_queue.h"
#include "moony_red_black_tree.h"

#define MOONY_MAX_EVENTS		(1024 * 1024)
#define MOONY_MAX_STACKSIZE	    (128 * 1024) // {http: 16*1024, tcp: 4*1024}

#define BIT(x)	 				(1 << (x))
#define CLEARBIT(x) 			~(1 << (x))

#define CANCEL_FD_WAIT_UINT64	1

typedef void (*coroutine_entry)(void *);

typedef enum {
	MOONY_COROUTINE_STATUS_WAIT_READ,
	MOONY_COROUTINE_STATUS_WAIT_WRITE,
	MOONY_COROUTINE_STATUS_NEW,
	MOONY_COROUTINE_STATUS_READY,
    MOONY_COROUTINE_STATUS_EXITED,
    MOONY_COROUTINE_STATUS_BUSY,
    MOONY_COROUTINE_STATUS_SLEEPING,
    MOONY_COROUTINE_STATUS_EXPIRED,
    MOONY_COROUTINE_STATUS_FDEOF,
    MOONY_COROUTINE_STATUS_DETACH,
    MOONY_COROUTINE_STATUS_CANCELLED,
    MOONY_COROUTINE_STATUS_PENDING_RUNCOMPUTE,
    MOONY_COROUTINE_STATUS_RUNCOMPUTE,
    MOONY_COROUTINE_STATUS_WAIT_IO_READ,
    MOONY_COROUTINE_STATUS_WAIT_IO_WRITE,
    MOONY_COROUTINE_STATUS_WAIT_MULTI
} moony_coroutine_status;

typedef enum {
    MOONY_COROUTINE_COMPUTE_BUSY,
    MOONY_COROUTINE_COMPUTE_FREE
} moony_coroutine_compute_status;

typedef enum {
    MOONY_COROUTINE_EV_READ,
    MOONY_COROUTINE_EV_WRITE
} moony_coroutine_event;

LIST_HEAD(_moony_coroutine_link, _moony_coroutine);
TAILQ_HEAD(_moony_coroutine_queue, _moony_coroutine);

RB_HEAD(_moony_coroutine_rbtree_sleep, _moony_coroutine);
RB_HEAD(_moony_coroutine_rbtree_wait, _moony_coroutine);

typedef struct _moony_coroutine_link moony_coroutine_link;
typedef struct _moony_coroutine_queue moony_coroutine_queue;

typedef struct _moony_coroutine_rbtree_sleep moony_coroutine_rbtree_sleep;
typedef struct _moony_coroutine_rbtree_wait moony_coroutine_rbtree_wait;


#ifndef _USE_UCONTEXT
typedef struct _moony_cpu_ctx {
	void *esp; //
	void *ebp;
	void *eip;
	void *edi;
	void *esi;
	void *ebx;
	void *r1;
	void *r2;
	void *r3;
	void *r4;
	void *r5;
} moony_cpu_ctx;
#endif

///
typedef struct _moony_scheduler {
	uint64_t birth;
#ifdef _USE_UCONTEXT
	ucontext_t ctx;
#else
	moony_cpu_ctx ctx;
#endif
	void *stack;
	size_t stack_size;
	int spawned_coroutines;
	uint64_t default_timeout;
	struct _moony_coroutine *curr_thread;
	int page_size;

	int poller_fd;
	int eventfd;
	struct epoll_event eventlist[MOONY_MAX_EVENTS];
	int nevents;

	int num_new_events;
	pthread_mutex_t defer_mutex;

	moony_coroutine_queue ready;
    moony_coroutine_queue defer;

    moony_coroutine_link busy;

    moony_coroutine_rbtree_sleep sleeping;
    moony_coroutine_rbtree_wait waiting;

	//private 

} moony_scheduler;

typedef struct _moony_coroutine {

	//private
	
#ifdef _USE_UCONTEXT
	ucontext_t ctx;
#else
	moony_cpu_ctx ctx;
#endif
	coroutine_entry func;
	void *arg;
	void *data;
	size_t stack_size;
	size_t last_stack_size;
	
	int status;
	moony_scheduler *sched;

	uint64_t birth;
	uint64_t id;
#if CANCEL_FD_WAIT_UINT64
	int fd;
	unsigned short events;  //POLL_EVENT
#else
	int64_t fd_wait;
#endif
	char funcname[64];
	struct _moony_coroutine *co_join;

	void **co_exit_ptr;
	void *stack;
	void *ebp;
	uint32_t ops;
	uint64_t sleep_usecs;

	RB_ENTRY(_moony_coroutine) sleep_node;
	RB_ENTRY(_moony_coroutine) wait_node;

	LIST_ENTRY(_moony_coroutine) busy_next;

	TAILQ_ENTRY(_moony_coroutine) ready_next;
	TAILQ_ENTRY(_moony_coroutine) defer_next;
	TAILQ_ENTRY(_moony_coroutine) cond_next;

	TAILQ_ENTRY(_moony_coroutine) io_next;
	TAILQ_ENTRY(_moony_coroutine) compute_next;

	struct {
		void *buf;
		size_t nbytes;
		int fd;
		int ret;
		int err;
	} io;

	struct _moony_coroutine_compute_sched *compute_sched;
	int ready_fds;
	struct pollfd *pfds;
	nfds_t nfds;
} moony_coroutine;


typedef struct _moony_coroutine_compute_sched {
#ifdef _USE_UCONTEXT
	ucontext_t ctx;
#else
	moony_cpu_ctx ctx;
#endif
	moony_coroutine_queue coroutines;

	moony_coroutine* curr_coroutine;

	pthread_mutex_t run_mutex;
	pthread_cond_t run_cond;

	pthread_mutex_t co_mutex;
	LIST_ENTRY(_moony_coroutine_compute_sched) compute_next;
	
	moony_coroutine_compute_status compute_status;
} moony_coroutine_compute_sched;

extern pthread_key_t global_sched_key;
static inline moony_scheduler* moony_get_scheduler(void) {
	return (moony_scheduler*)pthread_getspecific(global_sched_key);
}

static inline uint64_t moony_coroutine_diff_usecs(uint64_t t1, uint64_t t2) {
	return t2-t1;
}

static inline uint64_t moony_coroutine_usec_now(void) {
	struct timeval t1 = {0, 0};
	gettimeofday(&t1, nullptr);

	return t1.tv_sec * 1000000 + t1.tv_usec;
}



int moony_epoller_create(void);


void moony_schedule_cancel_event(moony_coroutine *co);
void moony_schedule_sched_event(moony_coroutine *co, int fd, moony_coroutine_event e, uint64_t timeout);

void moony_schedule_desched_sleepdown(moony_coroutine *co);
void moony_schedule_sched_sleepdown(moony_coroutine *co, uint64_t msecs);

moony_coroutine* moony_schedule_desched_wait(int fd);
void moony_schedule_sched_wait(moony_coroutine *co, int fd, unsigned short events, uint64_t timeout);

void moony_schedule_run(void);

int moony_epoller_ev_register_trigger(void);
int moony_epoller_wait(struct timespec t);
int moony_coroutine_resume(moony_coroutine *co);
void moony_coroutine_free(moony_coroutine *co);
int moony_coroutine_create(moony_coroutine **new_co, coroutine_entry func, void *arg);
void moony_coroutine_yield(moony_coroutine *co);

void moony_coroutine_sleep(uint64_t msecs);

int moony_socket(int domain, int type, int protocol);
int moony_accept(int fd, struct sockaddr *addr, socklen_t *len);
ssize_t moony_recv(int fd, void *buf, size_t len, int flags);
ssize_t moony_send(int fd, const void *buf, size_t len, int flags);
int moony_close(int fd);
int moony_poll(struct pollfd *fds, nfds_t nfds, int timeout);
int moony_connect(int fd, struct sockaddr *name, socklen_t namelen);

ssize_t moony_sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t moony_recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen);


#define COROUTINE_HOOK 

#ifdef  COROUTINE_HOOK


typedef int (*socket_t)(int domain, int type, int protocol);
extern socket_t socket_f;

typedef int(*connect_t)(int, const struct sockaddr *, socklen_t);
extern connect_t connect_f;

typedef ssize_t(*read_t)(int, void *, size_t);
extern read_t read_f;


typedef ssize_t(*recv_t)(int sockfd, void *buf, size_t len, int flags);
extern recv_t recv_f;

typedef ssize_t(*recvfrom_t)(int sockfd, void *buf, size_t len, int flags,
        struct sockaddr *src_addr, socklen_t *addrlen);
extern recvfrom_t recvfrom_f;

typedef ssize_t(*write_t)(int, const void *, size_t);
extern write_t write_f;

typedef ssize_t(*send_t)(int sockfd, const void *buf, size_t len, int flags);
extern send_t send_f;

typedef ssize_t(*sendto_t)(int sockfd, const void *buf, size_t len, int flags,
        const struct sockaddr *dest_addr, socklen_t addrlen);
extern sendto_t sendto_f;

typedef int(*accept_t)(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
extern accept_t accept_f;

// new-syscall
typedef int(*close_t)(int);
extern close_t close_f;


int init_hook(void);


/*

typedef int(*fcntl_t)(int __fd, int __cmd, ...);
extern fcntl_t fcntl_f;

typedef int (*getsockopt_t)(int sockfd, int level, int optname,
        void *optval, socklen_t *optlen);
extern getsockopt_t getsockopt_f;

typedef int (*setsockopt_t)(int sockfd, int level, int optname,
        const void *optval, socklen_t optlen);
extern setsockopt_t setsockopt_f;

*/

#endif



#endif


