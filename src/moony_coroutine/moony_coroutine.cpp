#include "../../include/moony_coroutine/moony_coroutine.h"

pthread_key_t global_sched_key;
static pthread_once_t sched_key_once = PTHREAD_ONCE_INIT;

#ifdef _USE_UCONTEXT

static void
_save_stack(moony_coroutine *co) {
	char* top = (char*)co->sched->stack + co->sched->stack_size;
	char dummy = 0;
	assert(top - &dummy <= MOONY_MAX_STACKSIZE);
	if (co->stack_size < top - &dummy) {
		co->stack = realloc(co->stack, top - &dummy);
		assert(co->stack != nullptr);
	}
	co->stack_size = top - &dummy;
	memcpy(co->stack, &dummy, co->stack_size);
}

static void
_load_stack(moony_coroutine *co) {
	memcpy((char*)co->sched->stack + co->sched->stack_size - co->stack_size, co->stack, co->stack_size);
}

static void _exec(void *lt) {
	moony_coroutine *co = (moony_coroutine*)lt;
	co->func(co->arg);
	co->status |= (BIT(MOONY_COROUTINE_STATUS_EXITED) | BIT(MOONY_COROUTINE_STATUS_FDEOF) | BIT(MOONY_COROUTINE_STATUS_DETACH));
	moony_coroutine_yield(co);
}

#else

int _switch(moony_cpu_ctx *new_ctx, moony_cpu_ctx *cur_ctx);

#ifdef __i386__
__asm__ (
"    .text                                  \n"
"    .p2align 2,,3                          \n"
".globl _switch                             \n"
"_switch:                                   \n"
"__switch:                                  \n"
"movl 8(%esp), %edx      # fs->%edx         \n"
"movl %esp, 0(%edx)      # save esp         \n"
"movl %ebp, 4(%edx)      # save ebp         \n"
"movl (%esp), %eax       # save eip         \n"
"movl %eax, 8(%edx)                         \n"
"movl %ebx, 12(%edx)     # save ebx,esi,edi \n"
"movl %esi, 16(%edx)                        \n"
"movl %edi, 20(%edx)                        \n"
"movl 4(%esp), %edx      # ts->%edx         \n"
"movl 20(%edx), %edi     # restore ebx,esi,edi      \n"
"movl 16(%edx), %esi                                \n"
"movl 12(%edx), %ebx                                \n"
"movl 0(%edx), %esp      # restore esp              \n"
"movl 4(%edx), %ebp      # restore ebp              \n"
"movl 8(%edx), %eax      # restore eip              \n"
"movl %eax, (%esp)                                  \n"
"ret                                                \n"
);
#elif defined(__x86_64__)

__asm__ (
"    .text                                  \n"
"       .p2align 4,,15                                   \n"
".globl _switch                                          \n"
".globl __switch                                         \n"
"_switch:                                                \n"
"__switch:                                               \n"
"       movq %rsp, 0(%rsi)      # save stack_pointer     \n"
"       movq %rbp, 8(%rsi)      # save frame_pointer     \n"
"       movq (%rsp), %rax       # save insn_pointer      \n"
"       movq %rax, 16(%rsi)                              \n"
"       movq %rbx, 24(%rsi)     # save rbx,r12-r15       \n"
"       movq %r12, 32(%rsi)                              \n"
"       movq %r13, 40(%rsi)                              \n"
"       movq %r14, 48(%rsi)                              \n"
"       movq %r15, 56(%rsi)                              \n"
"       movq 56(%rdi), %r15                              \n"
"       movq 48(%rdi), %r14                              \n"
"       movq 40(%rdi), %r13     # restore rbx,r12-r15    \n"
"       movq 32(%rdi), %r12                              \n"
"       movq 24(%rdi), %rbx                              \n"
"       movq 8(%rdi), %rbp      # restore frame_pointer  \n"
"       movq 0(%rdi), %rsp      # restore stack_pointer  \n"
"       movq 16(%rdi), %rax     # restore insn_pointer   \n"
"       movq %rax, (%rsp)                                \n"
"       ret                                              \n"
);
#endif


static void _exec(void *lt) {
#if defined(__lvm__) && defined(__x86_64__)
	__asm__("movq 16(%%rbp), %[lt]" : [lt] "=r" (lt));
#endif

	moony_coroutine *co = (moony_coroutine*)lt;
	co->func(co->arg);
	co->status |= (BIT(moony_COROUTINE_STATUS_EXITED) | BIT(MOONY_COROUTINE_STATUS_FDEOF) | BIT(moony_COROUTINE_STATUS_DETACH));
#if 1
	moony_coroutine_yield(co);
#else
	co->ops = 0;
	_switch(&co->sched->ctx, &co->ctx);
#endif
}

static inline void moony_coroutine_madvise(moony_coroutine *co) {

	size_t current_stack = (co->stack + co->stack_size) - co->ctx.esp;
	assert(current_stack <= co->stack_size);

	if (current_stack < co->last_stack_size &&
		co->last_stack_size > co->sched->page_size) {
		size_t tmp = current_stack + (-current_stack & (co->sched->page_size - 1));
		assert(madvise(co->stack, co->stack_size-tmp, MADV_DONTNEED) == 0);
	}
	co->last_stack_size = current_stack;
}

#endif

extern int moony_schedule_create(int stack_size);



void moony_coroutine_free(moony_coroutine *co) {
	if (co == nullptr) return ;
	co->sched->spawned_coroutines --;
#if 1
	if (co->stack) {
		free(co->stack);
		co->stack = nullptr;
	}
#endif
	if (co) {
		free(co);
	}

}

static void moony_coroutine_init(moony_coroutine *co) {

#ifdef _USE_UCONTEXT
	getcontext(&co->ctx);
	co->ctx.uc_stack.ss_sp = co->sched->stack;
	co->ctx.uc_stack.ss_size = co->sched->stack_size;
	co->ctx.uc_link = &co->sched->ctx;
	// printf("TAG21\n");
	makecontext(&co->ctx, (void (*)(void)) _exec, 1, (void*)co);
	// printf("TAG22\n");
#else
	void **stack = (void **)(co->stack + co->stack_size);

	stack[-3] = nullptr;
	stack[-2] = (void *)co;

	co->ctx.esp = (void*)stack - (4 * sizeof(void*));
	co->ctx.ebp = (void*)stack - (3 * sizeof(void*));
	co->ctx.eip = (void*)_exec;
#endif
	co->status = BIT(MOONY_COROUTINE_STATUS_READY);
}

void moony_coroutine_yield(moony_coroutine *co) {
	co->ops = 0;
#ifdef _USE_UCONTEXT
	if ((co->status & BIT(MOONY_COROUTINE_STATUS_EXITED)) == 0) {
		_save_stack(co);
	}
	swapcontext(&co->ctx, &co->sched->ctx);
#else
	_switch(&co->sched->ctx, &co->ctx);
#endif
}

int moony_coroutine_resume(moony_coroutine *co) {
	
	if (co->status & BIT(MOONY_COROUTINE_STATUS_NEW)) {
		moony_coroutine_init(co);
	} 
#ifdef _USE_UCONTEXT	
	else {
		_load_stack(co);
	}
#endif
	moony_scheduler *scheduler = moony_get_scheduler();
    scheduler->curr_thread = co;
#ifdef _USE_UCONTEXT
	swapcontext(&scheduler->ctx, &co->ctx);
#else
	_switch(&co->ctx, &co->sched->ctx);
	moony_coroutine_madvise(co);
#endif
    scheduler->curr_thread = nullptr;

#if 1
	if (co->status & BIT(MOONY_COROUTINE_STATUS_EXITED)) {
		if (co->status & BIT(MOONY_COROUTINE_STATUS_DETACH)) {
			moony_coroutine_free(co);
		}
		return -1;
	} 
#endif
	return 0;
}

void moony_coroutine_renice(moony_coroutine *co) {
	co->ops ++;
#if 1
	if (co->ops < 5) return ;
#endif
	TAILQ_INSERT_TAIL(&moony_get_scheduler()->ready, co, ready_next);
	moony_coroutine_yield(co);
}


void moony_coroutine_sleep(uint64_t msecs) {
	moony_coroutine *co = moony_get_scheduler()->curr_thread;

	if (msecs == 0) {
		TAILQ_INSERT_TAIL(&co->sched->ready, co, ready_next);
		moony_coroutine_yield(co);
	} else {
		moony_schedule_sched_sleepdown(co, msecs);
	}
}

void moony_coroutine_detach(void) {
	moony_coroutine *co = moony_get_scheduler()->curr_thread;
	co->status |= BIT(MOONY_COROUTINE_STATUS_DETACH);
}

static void moony_coroutine_sched_key_destructor(void *data) {
	free(data);
}

static void moony_coroutine_sched_key_creator(void) {
	assert(pthread_key_create(&global_sched_key, moony_coroutine_sched_key_destructor) == 0);
	assert(pthread_setspecific(global_sched_key, nullptr) == 0);
	
	return ;
}


// coroutine --> 
// create 
//
int moony_coroutine_create(moony_coroutine **new_co, coroutine_entry func, void *arg) {

	assert(pthread_once(&sched_key_once, moony_coroutine_sched_key_creator) == 0);
	moony_scheduler *scheduler = moony_get_scheduler();

	if (scheduler == nullptr) {
		moony_schedule_create(0);

        scheduler = moony_get_scheduler();
		if (scheduler == nullptr) {
			printf("Failed to create scheduler\n");
			return -1;
		}
	}

	moony_coroutine* co = (moony_coroutine*)calloc(1, sizeof(moony_coroutine));
	if (co == nullptr) {
		printf("Failed to allocate memory for new coroutine\n");
		return -2;
	}

#ifdef _USE_UCONTEXT
	co->stack = nullptr;
	co->stack_size = 0;
#else
	int ret = posix_memalign(&co->stack, getpagesize(), sched->stack_size);
	if (ret) {
		printf("Failed to allocate stack for new coroutine\n");
		free(co);
		return -3;
	}
	co->stack_size = sched->stack_size;
#endif
	co->sched = scheduler;
	co->status = BIT(MOONY_COROUTINE_STATUS_NEW); //
	co->id = scheduler->spawned_coroutines ++;
	co->func = func;
#if CANCEL_FD_WAIT_UINT64
	co->fd = -1;
	co->events = 0;
#else
	co->fd_wait = -1;
#endif
	co->arg = arg;
	co->birth = moony_coroutine_usec_now();
	*new_co = co;

	TAILQ_INSERT_TAIL(&co->sched->ready, co, ready_next);

	return 0;
}