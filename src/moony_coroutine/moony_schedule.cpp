#include "moony_coroutine.h"

#define FD_KEY(f,e) (((int64_t)(f) << (sizeof(int32_t) * 8)) | e)
#define FD_EVENT(f) ((int32_t)(f))
#define FD_ONLY(f) ((f) >> ((sizeof(int32_t) * 8)))


static inline int moony_coroutine_sleep_cmp(moony_coroutine *co1, moony_coroutine *co2) {
	if (co1->sleep_usecs < co2->sleep_usecs) {
		return -1;
	}
	if (co1->sleep_usecs == co2->sleep_usecs) {
		return 0;
	}
	return 1;
}

static inline int moony_coroutine_wait_cmp(moony_coroutine *co1, moony_coroutine *co2) {
#if CANCEL_FD_WAIT_UINT64
	if (co1->fd < co2->fd) return -1;
	else if (co1->fd == co2->fd) return 0;
	else return 1;
#else
	if (co1->fd_wait < co2->fd_wait) {
		return -1;
	}
	if (co1->fd_wait == co2->fd_wait) {
		return 0;
	}
#endif
	return 1;
}


RB_GENERATE(_moony_coroutine_rbtree_sleep, _moony_coroutine, sleep_node, moony_coroutine_sleep_cmp);
RB_GENERATE(_moony_coroutine_rbtree_wait, _moony_coroutine, wait_node, moony_coroutine_wait_cmp);



void moony_schedule_sched_sleepdown(moony_coroutine *co, uint64_t msecs) {
	uint64_t usecs = msecs * 1000u;

	moony_coroutine *co_tmp = RB_FIND(_moony_coroutine_rbtree_sleep, &co->sched->sleeping, co);
	if (co_tmp != nullptr) {
		RB_REMOVE(_moony_coroutine_rbtree_sleep, &co->sched->sleeping, co_tmp);
	}

	co->sleep_usecs = moony_coroutine_diff_usecs(co->sched->birth, moony_coroutine_usec_now()) + usecs;

	while (msecs) {
		co_tmp = RB_INSERT(_moony_coroutine_rbtree_sleep, &co->sched->sleeping, co);
		if (co_tmp) {
			printf("1111 sleep_usecs %" PRIu64 "\n", co->sleep_usecs);
			co->sleep_usecs ++;
			continue;
		}
		co->status |= BIT(MOONY_COROUTINE_STATUS_SLEEPING);
		break;
	}

	//yield
}

void moony_schedule_desched_sleepdown(moony_coroutine *co) {
	if (co->status & BIT(MOONY_COROUTINE_STATUS_SLEEPING)) {
		RB_REMOVE(_moony_coroutine_rbtree_sleep, &co->sched->sleeping, co);

		co->status &= CLEARBIT(MOONY_COROUTINE_STATUS_SLEEPING);
		co->status |= BIT(MOONY_COROUTINE_STATUS_READY);
		co->status &= CLEARBIT(MOONY_COROUTINE_STATUS_EXPIRED);
	}
}

moony_coroutine *moony_schedule_search_wait(int fd) {
	moony_coroutine find_it = {0};
	find_it.fd = fd;

	moony_scheduler *scheduler = moony_get_scheduler();
	
	moony_coroutine *co = RB_FIND(_moony_coroutine_rbtree_wait, &scheduler->waiting, &find_it);
	co->status = 0;

	return co;
}

moony_coroutine* moony_schedule_desched_wait(int fd) {
	
	moony_coroutine find_it = {0};
	find_it.fd = fd;

	moony_scheduler *scheduler = moony_get_scheduler();
	
	moony_coroutine *co = RB_FIND(_moony_coroutine_rbtree_wait, &scheduler->waiting, &find_it);
	if (co != nullptr) {
		RB_REMOVE(_moony_coroutine_rbtree_wait, &co->sched->waiting, co);
	}
	co->status = 0;
	moony_schedule_desched_sleepdown(co);
	
	return co;
}

void moony_schedule_sched_wait(moony_coroutine *co, int fd, unsigned short events, uint64_t timeout) {
	
	if (co->status & BIT(MOONY_COROUTINE_STATUS_WAIT_READ) ||
		co->status & BIT(MOONY_COROUTINE_STATUS_WAIT_WRITE)) {
		printf("Unexpected event. lt id %" PRIu64 " fd %" PRId32 " already in %" PRId32 " state\n",
            co->id, co->fd, co->status);
		assert(0);
	}

	if (events & POLLIN) {
		co->status |= MOONY_COROUTINE_STATUS_WAIT_READ;
	} else if (events & POLLOUT) {
		co->status |= MOONY_COROUTINE_STATUS_WAIT_WRITE;
	} else {
		printf("events : %d\n", events);
		assert(0);
	}

	co->fd = fd;
	co->events = events;
	moony_coroutine *co_tmp = RB_INSERT(_moony_coroutine_rbtree_wait, &co->sched->waiting, co);

//	assert(co_tmp == nullptr);

	//printf("timeout --> %"PRIu64"\n", timeout);
	if (timeout == 1) return ; //Error

	moony_schedule_sched_sleepdown(co, timeout);
	
}

void moony_schedule_cancel_wait(moony_coroutine *co) {
	RB_REMOVE(_moony_coroutine_rbtree_wait, &co->sched->waiting, co);
}

void moony_schedule_free(moony_scheduler *scheduler) {
	if (scheduler->poller_fd > 0) {
		close(scheduler->poller_fd);
	}
	if (scheduler->eventfd > 0) {
		close(scheduler->eventfd);
	}
	if (scheduler->stack != nullptr) {
		free(scheduler->stack);
	}
	
	free(scheduler);

	assert(pthread_setspecific(global_sched_key, nullptr) == 0);
}

int moony_schedule_create(int stack_size) {

	int sched_stack_size = stack_size ? stack_size : MOONY_MAX_STACKSIZE;

	moony_scheduler *scheduler = (moony_scheduler*)calloc(1, sizeof(moony_scheduler));
	if (scheduler == nullptr) {
		printf("Failed to initialize scheduler\n");
		return -1;
	}

	assert(pthread_setspecific(global_sched_key, scheduler) == 0);

    scheduler->poller_fd = moony_epoller_create();
	if (scheduler->poller_fd == -1) {
		printf("Failed to initialize epoller\n");
		moony_schedule_free(scheduler);
		return -2;
	}

	moony_epoller_ev_register_trigger();

    scheduler->stack_size = sched_stack_size;
    scheduler->page_size = getpagesize();

#ifdef _USE_UCONTEXT
	int ret = posix_memalign(&scheduler->stack, scheduler->page_size, scheduler->stack_size);
	assert(ret == 0);
#else
	sched->stack = nullptr;
	bzero(&sched->ctx, sizeof(moony_cpu_ctx));
#endif

    scheduler->spawned_coroutines = 0;
    scheduler->default_timeout = 3000000u;

	RB_INIT(&scheduler->sleeping);
	RB_INIT(&scheduler->waiting);

    scheduler->birth = moony_coroutine_usec_now();

	TAILQ_INIT(&scheduler->ready);
	TAILQ_INIT(&scheduler->defer);
	LIST_INIT(&scheduler->busy);

    return 0;
}


static moony_coroutine *moony_schedule_expired(moony_scheduler *scheduler) {
	
	uint64_t t_diff_usecs = moony_coroutine_diff_usecs(scheduler->birth, moony_coroutine_usec_now());
	moony_coroutine *co = RB_MIN(_moony_coroutine_rbtree_sleep, &scheduler->sleeping);
	if (co == nullptr) return nullptr;
	
	if (co->sleep_usecs <= t_diff_usecs) {
		RB_REMOVE(_moony_coroutine_rbtree_sleep, &co->sched->sleeping, co);
		return co;
	}
	return nullptr;
}

static inline int moony_schedule_isdone(moony_scheduler *scheduler) {
	return (RB_EMPTY(&scheduler->waiting) &&
		LIST_EMPTY(&scheduler->busy) &&
		RB_EMPTY(&scheduler->sleeping) &&
		TAILQ_EMPTY(&scheduler->ready));
}

static uint64_t moony_schedule_min_timeout(moony_scheduler *scheduler) {
	uint64_t t_diff_usecs = moony_coroutine_diff_usecs(scheduler->birth, moony_coroutine_usec_now());
	uint64_t min = scheduler->default_timeout;

	moony_coroutine *co = RB_MIN(_moony_coroutine_rbtree_sleep, &scheduler->sleeping);
	if (!co) return min;

	min = co->sleep_usecs;
	if (min > t_diff_usecs) {
		return min - t_diff_usecs;
	}

	return 0;
} 

static int moony_schedule_epoll(moony_scheduler *scheduler) {

    scheduler->num_new_events = 0;

	struct timespec t = {0, 0};
	uint64_t usecs = moony_schedule_min_timeout(scheduler);
	if (usecs && TAILQ_EMPTY(&scheduler->ready)) {
		t.tv_sec = usecs / 1000000u;
		if (t.tv_sec != 0) {
			t.tv_nsec = (usecs % 1000u) * 1000u;
		} else {
			t.tv_nsec = usecs * 1000u;
		}
	} else {
		return 0;
	}

	int nready = 0;
	while (true) {
		nready = moony_epoller_wait(t);
		if (nready == -1) {
			if (errno == EINTR) continue;
			else assert(0);
		}
		break;
	}

    scheduler->nevents = 0;
    scheduler->num_new_events = nready;

	return 0;
}

void moony_schedule_run(void) {

	moony_scheduler *scheduler = moony_get_scheduler();
	if (scheduler == nullptr) return ;

	while (!moony_schedule_isdone(scheduler)) {

		// 1. expired --> sleep rbtree
		moony_coroutine *expired = nullptr;
		while ((expired = moony_schedule_expired(scheduler)) != nullptr) {
			moony_coroutine_resume(expired);
		}
		// 2. ready queue
		moony_coroutine *last_co_ready = TAILQ_LAST(&scheduler->ready, _moony_coroutine_queue);
		while (!TAILQ_EMPTY(&scheduler->ready)) {
			moony_coroutine *co = TAILQ_FIRST(&scheduler->ready);
			TAILQ_REMOVE(&co->sched->ready, co, ready_next);

			if (co->status & BIT(MOONY_COROUTINE_STATUS_FDEOF)) {
				moony_coroutine_free(co);
				break;
			}

			moony_coroutine_resume(co);
			if (co == last_co_ready) break;
		}

		// 3. wait rbtree
		moony_schedule_epoll(scheduler);
		while (scheduler->num_new_events) {
			int idx = --scheduler->num_new_events;
			struct epoll_event *ev = scheduler->eventlist+idx;
			
			int fd = ev->data.fd;
			int is_eof = ev->events & EPOLLHUP;
			if (is_eof) errno = ECONNRESET;

			moony_coroutine *co = moony_schedule_search_wait(fd);
			if (co != nullptr) {
				if (is_eof) {
					co->status |= BIT(MOONY_COROUTINE_STATUS_FDEOF);
				}
				moony_coroutine_resume(co);
			}

			is_eof = 0;
		}
	}

	moony_schedule_free(scheduler);
	
	return ;
}

