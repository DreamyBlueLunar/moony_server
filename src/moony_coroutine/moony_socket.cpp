#include "../../include/moony_coroutine/moony_coroutine.h"

static uint32_t moony_pollevent_2epoll( short events ) {
	uint32_t e = 0;	
	if(events & POLLIN) 	e |= EPOLLIN;
	if(events & POLLOUT)  e |= EPOLLOUT;
	if(events & POLLHUP) 	e |= EPOLLHUP;
	if(events & POLLERR)	e |= EPOLLERR;
	if(events & POLLRDNORM) e |= EPOLLRDNORM;
	if(events & POLLWRNORM) e |= EPOLLWRNORM;
	return e;
}
static short moony_epollevent_2poll( uint32_t events ) {
	short e = 0;	
	if(events & EPOLLIN) 	e |= POLLIN;
	if(events & EPOLLOUT) e |= POLLOUT;
	if(events & EPOLLHUP) e |= POLLHUP;
	if(events & EPOLLERR) e |= POLLERR;
	if(events & EPOLLRDNORM) e |= POLLRDNORM;
	if(events & EPOLLWRNORM) e |= POLLWRNORM;
	return e;
}

/*
 * moony_poll_inner --> 1. sockfd--> epoll, 2 yield, 3. epoll x sockfd
 * fds : 
 */
static int moony_poll_inner(struct pollfd *fds, nfds_t nfds, int timeout) {

	if (timeout == 0) {
		return poll(fds, nfds, timeout);
	}
	if (timeout < 0) {
		timeout = INT_MAX;
	}

	moony_scheduler *scheduler = moony_get_scheduler();
	if (nullptr == scheduler) {
		printf("scheduler not exit!\n");
		return -1;
	}
	
	moony_coroutine *co = scheduler->curr_thread;

	for (int i = 0;i < nfds;i ++) {
	
		struct epoll_event ev;
		ev.events = moony_pollevent_2epoll(fds[i].events);
		ev.data.fd = fds[i].fd;
		epoll_ctl(scheduler->poller_fd, EPOLL_CTL_ADD, fds[i].fd, &ev);

		co->events = fds[i].events;
		moony_schedule_sched_wait(co, fds[i].fd, fds[i].events, timeout);
	}

	moony_coroutine_yield(co);

	for (int i = 0;i < nfds;i ++) {
	
		struct epoll_event ev;
		ev.events = moony_pollevent_2epoll(fds[i].events);
		ev.data.fd = fds[i].fd;
		epoll_ctl(scheduler->poller_fd, EPOLL_CTL_DEL, fds[i].fd, &ev);

		moony_schedule_desched_wait(fds[i].fd);
	}

	return nfds;
}


int moony_socket(int domain, int type, int protocol) {

	int fd = socket(domain, type, protocol);
	if (fd == -1) {
		printf("Failed to create a new socket\n");
		return -1;
	}
	int ret = fcntl(fd, F_SETFL, O_NONBLOCK);
	if (ret == -1) {
		close(ret);
		return -1;
	}
	int reuse = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));
	
	return fd;
}

//moony_accept
//return failed == -1, success > 0

int moony_accept(int fd, struct sockaddr *addr, socklen_t *len) {
	int sockfd = -1;
	int timeout = 1;
	moony_coroutine *co = moony_get_scheduler()->curr_thread;
	
	while (true) {
		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLIN | POLLERR | POLLHUP;
		moony_poll_inner(&fds, 1, timeout);

		sockfd = accept(fd, addr, len);
		if (sockfd < 0) {
			if (errno == EAGAIN) {
				continue;
			} else if (errno == ECONNABORTED) {
				printf("accept : ECONNABORTED\n");
				
			} else if (errno == EMFILE || errno == ENFILE) {
				printf("accept : EMFILE || ENFILE\n");
			}
			return -1;
		} else {
			break;
		}
	}

	int ret = fcntl(sockfd, F_SETFL, O_NONBLOCK);
	if (ret == -1) {
		close(sockfd);
		return -1;
	}
	int reuse = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));
	
	return sockfd;
}


int moony_connect(int fd, struct sockaddr *name, socklen_t namelen) {

	int ret = 0;

	while (true) {

		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLOUT | POLLERR | POLLHUP;
		moony_poll_inner(&fds, 1, 1);

		ret = connect(fd, name, namelen);
		if (ret == 0) break;

		if (ret == -1 && (errno == EAGAIN ||
			errno == EWOULDBLOCK || 
			errno == EINPROGRESS)) {
			continue;
		} else {
			break;
		}
	}

	return ret;
}

//recv 
// add epoll first
//
ssize_t moony_recv(int fd, void *buf, size_t len, int flags) {
	
	struct pollfd fds;
	fds.fd = fd;
	fds.events = POLLIN | POLLERR | POLLHUP;

	moony_poll_inner(&fds, 1, 1);

	int ret = recv(fd, buf, len, flags);
	if (ret < 0) {
		//if (errno == EAGAIN) return ret;
		if (errno == ECONNRESET) return -1;
		//printf("recv error : %d, ret : %d\n", errno, ret);
		
	}
	return ret;
}


ssize_t moony_send(int fd, const void *buf, size_t len, int flags) {
	
	int sent = 0;

	int ret = send(fd, ((char*)buf)+sent, len-sent, flags);
	if (ret == 0) return ret;
	if (ret > 0) sent += ret;

	while (sent < len) {
		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLOUT | POLLERR | POLLHUP;

		moony_poll_inner(&fds, 1, 1);
		ret = send(fd, ((char*)buf)+sent, len-sent, flags);
		//printf("send --> len : %d\n", ret);
		if (ret <= 0) {			
			break;
		}
		sent += ret;
	}

	if (ret <= 0 && sent == 0) return ret;
	
	return sent;
}


ssize_t moony_sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {


	int sent = 0;

	while (sent < len) {
		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLOUT | POLLERR | POLLHUP;

		moony_poll_inner(&fds, 1, 1);
		int ret = sendto(fd, ((char*)buf)+sent, len-sent, flags, dest_addr, addrlen);
		if (ret <= 0) {
			if (errno == EAGAIN) continue;
			else if (errno == ECONNRESET) {
				return ret;
			}
			printf("send errno : %d, ret : %d\n", errno, ret);
			assert(0);
		}
		sent += ret;
	}
	return sent;
	
}

ssize_t moony_recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen) {

	struct pollfd fds;
	fds.fd = fd;
	fds.events = POLLIN | POLLERR | POLLHUP;

	moony_poll_inner(&fds, 1, 1);

	int ret = recvfrom(fd, buf, len, flags, src_addr, addrlen);
	if (ret < 0) {
		if (errno == EAGAIN) return ret;
		if (errno == ECONNRESET) return 0;
		
		printf("recv error : %d, ret : %d\n", errno, ret);
		assert(0);
	}
	return ret;

}




int moony_close(int fd) {
#if 0
	moony_schedule *sched = moony_get_scheduler();

	moony_coroutine *co = sched->curr_thread;
	if (co) {
		TAILQ_INSERT_TAIL(&moony_get_scheduler()->ready, co, ready_next);
		co->status |= BIT(MOONY_COROUTINE_STATUS_FDEOF);
	}
#endif	
	return close(fd);
}


#ifdef  COROUTINE_HOOK

socket_t socket_f = nullptr;

read_t read_f = nullptr;
recv_t recv_f = nullptr;
recvfrom_t recvfrom_f = nullptr;

write_t write_f = nullptr;
send_t send_f = nullptr;
sendto_t sendto_f = nullptr;

accept_t accept_f = nullptr;
close_t close_f = nullptr;
connect_t connect_f = nullptr;


int init_hook(void) {

	socket_f = (socket_t)dlsym(RTLD_NEXT, "socket");
	
	read_f = (read_t)dlsym(RTLD_NEXT, "read");
	recv_f = (recv_t)dlsym(RTLD_NEXT, "recv");
	recvfrom_f = (recvfrom_t)dlsym(RTLD_NEXT, "recvfrom");

	write_f = (write_t)dlsym(RTLD_NEXT, "write");
	send_f = (send_t)dlsym(RTLD_NEXT, "send");
    sendto_f = (sendto_t)dlsym(RTLD_NEXT, "sendto");

	accept_f = (accept_t)dlsym(RTLD_NEXT, "accept");
	close_f = (close_t)dlsym(RTLD_NEXT, "close");
	connect_f = (connect_t)dlsym(RTLD_NEXT, "connect");

    return 0;
}



int socket(int domain, int type, int protocol) {

	if (!socket_f) init_hook();

	int fd = socket_f(domain, type, protocol);
	if (fd == -1) {
		printf("Failed to create a new socket\n");
		return -1;
	}
	int ret = fcntl(fd, F_SETFL, O_NONBLOCK);
	if (ret == -1) {
		close(ret);
		return -1;
	}
	int reuse = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));
	
	return fd;
}

ssize_t read(int fd, void *buf, size_t count) {

	if (!read_f) init_hook();

	struct pollfd fds;
	fds.fd = fd;
	fds.events = POLLIN | POLLERR | POLLHUP;

	moony_poll_inner(&fds, 1, 1);

	int ret = read_f(fd, buf, count);
	if (ret < 0) {
		//if (errno == EAGAIN) return ret;
		if (errno == ECONNRESET) return -1;
		//printf("recv error : %d, ret : %d\n", errno, ret);
		
	}
	return ret;
}

ssize_t recv(int fd, void *buf, size_t len, int flags) {

	if (!recv_f) init_hook();

	struct pollfd fds;
	fds.fd = fd;
	fds.events = POLLIN | POLLERR | POLLHUP;

	moony_poll_inner(&fds, 1, 1);

	int ret = recv_f(fd, buf, len, flags);
    //printf("--> recv: %s\n", (char*)buf);
	if (ret < 0) {
		//if (errno == EAGAIN) return ret;
		if (errno == ECONNRESET) return -1;
		//printf("recv error : %d, ret : %d\n", errno, ret);
		
	}
	return ret;
}


ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen) {

	if (!recvfrom_f) init_hook();

	struct pollfd fds;
	fds.fd = fd;
	fds.events = POLLIN | POLLERR | POLLHUP;

	moony_poll_inner(&fds, 1, 1);

	int ret = recvfrom_f(fd, buf, len, flags, src_addr, addrlen);
	if (ret < 0) {
		if (errno == EAGAIN) return ret;
		if (errno == ECONNRESET) return 0;
		
		printf("recv error : %d, ret : %d\n", errno, ret);
		assert(0);
	}
	return ret;

}


ssize_t write(int fd, const void *buf, size_t count) {

	if (!write_f) init_hook();

	int sent = 0;

	int ret = write_f(fd, ((char*)buf)+sent, count-sent);
	if (ret == 0) return ret;
	if (ret > 0) sent += ret;

	while (sent < count) {
		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLOUT | POLLERR | POLLHUP;

		moony_poll_inner(&fds, 1, 1);
		ret = write_f(fd, ((char*)buf)+sent, count-sent);
		//printf("send --> len : %d\n", ret);
		if (ret <= 0) {			
			break;
		}
		sent += ret;
	}

	if (ret <= 0 && sent == 0) return ret;
	
	return sent;
}


ssize_t send(int fd, const void *buf, size_t len, int flags) {

	if (!send_f) init_hook();

	int sent = 0;

    //printf("<-- send: %s\n", (char*)buf);
	int ret = send_f(fd, ((char*)buf)+sent, len-sent, flags);
	if (ret == 0) return ret;
	if (ret > 0) sent += ret;

	while (sent < len) {
		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLOUT | POLLERR | POLLHUP;

		moony_poll_inner(&fds, 1, 1);
		ret = send_f(fd, ((char*)buf)+sent, len-sent, flags);
		//printf("send --> len : %d\n", ret);
		if (ret <= 0) {			
			break;
		}
		sent += ret;
	}

	if (ret <= 0 && sent == 0) return ret;
	
	return sent;
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
        const struct sockaddr *dest_addr, socklen_t addrlen) {

	if (!sendto_f) init_hook();

	struct pollfd fds;
	fds.fd = sockfd;
	fds.events = POLLOUT | POLLERR | POLLHUP;

	moony_poll_inner(&fds, 1, 1);

	int ret = sendto_f(sockfd, buf, len, flags, dest_addr, addrlen);
	if (ret < 0) {
		if (errno == EAGAIN) return ret;
		if (errno == ECONNRESET) return 0;
		
		printf("recv error : %d, ret : %d\n", errno, ret);
		assert(0);
	}
	return ret;

}



int accept(int fd, struct sockaddr *addr, socklen_t *len) {

	if (!accept_f) init_hook();

	int sockfd = -1;
	int timeout = 1;
	moony_coroutine *co = moony_get_scheduler()->curr_thread;
	
	while (true) {
		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLIN | POLLERR | POLLHUP;
		moony_poll_inner(&fds, 1, timeout);

		sockfd = accept_f(fd, addr, len);
		if (sockfd < 0) {
			if (errno == EAGAIN) {
				continue;
			} else if (errno == ECONNABORTED) {
				printf("accept : ECONNABORTED\n");
				
			} else if (errno == EMFILE || errno == ENFILE) {
				printf("accept : EMFILE || ENFILE\n");
			}
			return -1;
		} else {
			break;
		}
	}

	int ret = fcntl(sockfd, F_SETFL, O_NONBLOCK);
	if (ret == -1) {
		close(sockfd);
		return -1;
	}
	int reuse = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));
	
	return sockfd;
}

int close(int fd) {

	if (!close_f) init_hook();

	return close_f(fd);
}



int connect(int fd, const struct sockaddr *addr, socklen_t addrlen) {

	if (!connect_f) init_hook();

	int ret = 0;

	while (true) {

		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLOUT | POLLERR | POLLHUP;
		moony_poll_inner(&fds, 1, 1);

		ret = connect_f(fd, addr, addrlen);
		if (ret == 0) break;

		if (ret == -1 && (errno == EAGAIN ||
			errno == EWOULDBLOCK || 
			errno == EINPROGRESS)) {
			continue;
		} else {
			break;
		}
	}

	return ret;
}

#endif
