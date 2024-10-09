#include <sys/eventfd.h>
#include "moony_coroutine.h"

int moony_epoller_create(void) {
	return epoll_create(1);
} 

int moony_epoller_wait(struct timespec t) {
    moony_scheduler *scheduler = moony_get_scheduler();
	return epoll_wait(scheduler->poller_fd,
                      scheduler->eventlist,
                      MOONY_MAX_EVENTS,
                      t.tv_sec*1000.0 + t.tv_nsec/1000000.0);
}

int moony_epoller_ev_register_trigger(void) {
    moony_scheduler *scheduler = moony_get_scheduler();

	if (!scheduler->eventfd) {
        scheduler->eventfd = eventfd(0, EFD_NONBLOCK);
		assert(scheduler->eventfd != -1);
	}

	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = scheduler->eventfd;
	int ret = epoll_ctl(scheduler->poller_fd,
                        EPOLL_CTL_ADD,
                        scheduler->eventfd,
                        &ev);

	assert(ret != -1);
    return 0;
}


