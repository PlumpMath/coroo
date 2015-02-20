#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/socket.h>

#define __USE_FORTIFY_LEVEL_OLD __USE_FORTIFY_LEVEL
#undef __USE_FORTIFY_LEVEL
#include <setjmp.h>
#define __USE_FORTIFY_LEVEL __USE_FORTIFY_LEVEL_OLD
#undef __USE_FORTIFY_LEVEL_OLD

#include <valgrind/valgrind.h>

#include "coroo.h"
#include "refcnt.h"

typedef enum {
	STACK_DIRECTION_UNKNOWN,
	STACK_DIRECTION_UP,
	STACK_DIRECTION_DOWN,
} StackDirection;

typedef struct ListElement {
	struct ListElement *prev, *next;
} ListElement;

typedef struct List {
	struct ListElement anchor;
} List;

struct CorooThread {
	ListElement list_elem;
	jmp_buf thread_state;
	struct pollfd *poll_descs;
	nfds_t poll_descs_count;
	int64_t poll_expiration;
	bool poll_acked;
	// initialized by coroo_thread_start
	void *stack_base;
	size_t stack_size; // including guard page
	unsigned int stack_id; // for valgrind
	CorooThreadFunction thread_function;
	void *thread_argument;
};

static const bool use_mmap = true;

static StackDirection stack_direction = STACK_DIRECTION_UNKNOWN;

static CorooThread main_thread;
static CorooThread *current_thread;
static List ready_threads;
static List waiting_threads;
static List dead_threads;

#define list_entry(elem, type, field) \
	((type *)((uintptr_t)elem - offsetof(type, field)))

static void run_next_thread();

static void list_init(List *list) {
	list->anchor.prev = &list->anchor;
	list->anchor.next = &list->anchor;
}

static bool list_empty(List *list) {
	return list->anchor.next == &list->anchor;
}

static ListElement *list_remove(ListElement *elem) {
	elem->prev->next = elem->next;
	elem->next->prev = elem->prev;
	elem->prev = NULL;
	elem->next = NULL;
	return elem;
}

#if 0
static void list_push_front(List *list, ListElement *elem) {
	elem->prev = &list->anchor;
	elem->next = list->anchor.next;
	elem->prev->next = elem;
	elem->next->prev = elem;
}
#endif

static void list_push_back(List *list, ListElement *elem) {
	elem->prev = list->anchor.prev;
	elem->next = &list->anchor;
	elem->prev->next = elem;
	elem->next->prev = elem;
}

static ListElement *list_pop_front(List *list) {
	return list_remove(list->anchor.next);
}

#if 0
static ListElement *list_pop_back(List *list) {
	return list_remove(list->anchor.prev);
}
#endif

static int64_t get_time_millis() {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + (int64_t)now.tv_nsec / 1000000;
}

static void maybe_clobber_pointer(void **ptr) {
	if (stack_direction == STACK_DIRECTION_UNKNOWN) {
		uintptr_t value = (uintptr_t)(*ptr);
		union {
			char buf;
			struct {
				uintptr_t value;
				char end;
			};
		} unzer;
		unzer.value = value;
		unzer.end = '\0';
		strtok(&unzer.buf, "");
		*ptr = (void *)(value | unzer.value);
	}
}

static void determine_stack_direction_actual(void *prev) {
	if (stack_direction != STACK_DIRECTION_UNKNOWN)
		return;
	if ((void *)&prev < prev) {
		stack_direction = STACK_DIRECTION_DOWN;
	} else if ((void *)&prev > prev) {
		stack_direction = STACK_DIRECTION_UP;
	} else {
		fputs("critical error: failed to determine stack direction!\n", stderr);
		abort();
	}
}

static void determine_stack_direction(void *prev) {
	void (*func)(void *) = determine_stack_direction_actual;
	maybe_clobber_pointer((void **)&func);
	func(prev);
}

static void thread_start_helper_actual(size_t jump) {
	union {
		char filler[jump];
		void *clobbered;
	} filler;
	maybe_clobber_pointer(&filler.clobbered);
	current_thread->thread_function(current_thread->thread_argument);
	maybe_clobber_pointer(&filler.clobbered);
	coroo_thread_exit();
	maybe_clobber_pointer(&filler.clobbered);
}

static void thread_start_helper(size_t jump) {
	void (*func)(size_t) = thread_start_helper_actual;
	maybe_clobber_pointer((void **)&func);
	func(jump);
}

static size_t get_page_size() {
	static size_t cached = 0;
	if (cached == 0) {
		cached = sysconf(_SC_PAGESIZE);
		if (cached & (cached - 1)) {
			fputs("critical error: page size not power of two!\n", stderr);
			abort();
		}
	}
	return cached;
}

static void wait_for_events() {
	// count number of descriptors
	nfds_t nfds = 0;
	ListElement *e, *anchor = &waiting_threads.anchor;
	for (e = anchor->next; e != anchor; e = e->next) {
		CorooThread *t = list_entry(e, CorooThread, list_elem);
		nfds += t->poll_descs_count;
	}
	// allocate descriptor table
	CorooThread **threads = malloc(nfds * sizeof(*threads));
	struct pollfd **originals = malloc(nfds * sizeof(*originals));
	struct pollfd *effectives = malloc(nfds * sizeof(*effectives));
	// populate tables
	int64_t now = get_time_millis();
	int64_t timeout = -1;
	nfds_t i = 0;
	for (e = anchor->next; e != anchor; e = e->next) {
		CorooThread *t = list_entry(e, CorooThread, list_elem);
		for (nfds_t j = 0; j < t->poll_descs_count; j++, i++) {
			struct pollfd *desc = &t->poll_descs[j];
			threads[i] = t;
			originals[i] = desc;
			effectives[i] = *desc;
			if (t->poll_expiration >= 0) {
				int64_t remaining = t->poll_expiration - now;
				if (remaining <= 0)
					timeout = 0;
				else
					timeout = (remaining < timeout || timeout < 0) ? remaining : timeout;
			}
		}
	}
	// do the poll
	poll(effectives, nfds, timeout);
	now = get_time_millis();
	// dispatch results
	for (i = 0; i < nfds; i++) {
		CorooThread *t = threads[i];
		struct pollfd *desc = &effectives[i];
		bool ack = desc->revents ||
			(t->poll_expiration >= 0 && now >= t->poll_expiration);
		originals[i]->revents = desc->revents;
		if (ack && !t->poll_acked) {
			list_remove(&t->list_elem);
			list_push_back(&ready_threads, &t->list_elem);
			t->poll_acked = true;
		}
	}
	// clean up
	free(threads);
	free(originals);
	free(effectives);
}

static void reap_dead_threads() {
	while (!list_empty(&dead_threads)) {
		CorooThread *t = list_entry(
				list_pop_front(&dead_threads),
				CorooThread,
				list_elem);
		if (t != &main_thread) {
			VALGRIND_STACK_DEREGISTER(t->stack_id);
			if (use_mmap)
				munmap(t->stack_base, t->stack_size);
			else
				free(t->stack_base);
			free(t);
		}
	}
}

static void run_next_thread() {
	while (list_empty(&ready_threads))
		wait_for_events();
	// something's ready to run!
	CorooThread *next_thread = list_entry(
			list_pop_front(&ready_threads),
			CorooThread,
			list_elem);
	if (current_thread == next_thread)
		return;
	// do context switch
	if (setjmp(current_thread->thread_state) == 0) {
		current_thread = next_thread;
		longjmp(next_thread->thread_state, 1);
	}
	// clean up
	reap_dead_threads();
}

void coroo_thread_init() {
	// only run once
	if (stack_direction != STACK_DIRECTION_UNKNOWN)
		return;
	// determine stack direction
	int dummy;
	determine_stack_direction(&dummy);
	// initialize lists
	list_init(&ready_threads);
	list_init(&waiting_threads);
	list_init(&dead_threads);
	// initialize main thread
	memset(&main_thread, 0, sizeof(main_thread));
	current_thread = &main_thread;
}

CorooThread *coroo_thread_start(size_t stack_size,
		CorooThreadFunction thread_function,
		void *thread_argument) {
	// round up the stack size
	size_t page_size = get_page_size();
	size_t page_mask = ~(page_size - 1);
	stack_size = (stack_size + page_size - 1) & page_mask;
	stack_size += page_size * 2; // guard page and margin page
	// try to map in stack pages
	void *stack_base;
	if (use_mmap)
		stack_base = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	else
		stack_base = malloc(stack_size);
	if (!stack_base)
		fprintf(stderr, "warning: failed to map memory for stack: %m\n");
	// make guard page
	if (use_mmap) {
		void *guard_start = stack_direction == STACK_DIRECTION_DOWN ?
			stack_base :
			(void *)((uintptr_t)stack_base + stack_size - page_size);
		if (mprotect(guard_start, page_size, PROT_NONE) < 0)
			fprintf(stderr, "failed to set guard page: %m\n");
	}
	// initialize the thread
	CorooThread *thread = malloc(sizeof(*thread));
	thread->stack_base = stack_base;
	thread->stack_size = stack_size;
	thread->stack_id = VALGRIND_STACK_REGISTER(stack_base,
			(void *)((uintptr_t)stack_base + stack_size));
	thread->thread_function = thread_function;
	thread->thread_argument = thread_argument;
	// initialize the thread state
	size_t jump;
	if (stack_direction == STACK_DIRECTION_DOWN)
		jump = (uintptr_t)&thread - ((uintptr_t)stack_base + stack_size - page_size);
	else
		jump = ((uintptr_t)stack_base + page_size) - (uintptr_t)&jump;
	list_push_back(&ready_threads, &current_thread->list_elem);
	if (setjmp(current_thread->thread_state) == 0) {
		current_thread = thread;
		thread_start_helper(jump);
	}
	// return the thread
	return thread;
}

void coroo_thread_yield() {
	list_push_back(&ready_threads, &current_thread->list_elem);
	run_next_thread();
}

void coroo_thread_exit() {
	list_push_back(&dead_threads, &current_thread->list_elem);
	run_next_thread();
}

short coroo_poll_simple(int fd, short events, int64_t timeout) {
	struct pollfd desc;
	desc.fd = fd;
	desc.events = events;
	desc.revents = 0;
	coroo_poll(&desc, 1, timeout);
	return desc.revents;
}

void coroo_poll(struct pollfd *fds, nfds_t nfds, int64_t timeout) {
	current_thread->poll_descs = fds;
	current_thread->poll_descs_count = nfds;
	current_thread->poll_acked = false;
	if (timeout > 0)
		current_thread->poll_expiration = get_time_millis() + timeout;
	else
		current_thread->poll_expiration = timeout;
	list_push_back(&waiting_threads, &current_thread->list_elem);
	run_next_thread();
}

struct CorooBufIO {
	size_t buffer_size;
	struct pollfd *poll_desc;
	char *incoming; // circular buffer
	size_t incoming_start;
	size_t incoming_length;
	char *outgoing; // circular buffer
	size_t outgoing_start;
	size_t outgoing_length;
};

static void bufio_destructor(void *ptr) {
	CorooBufIO *bio = ptr;
	free(bio->incoming);
	free(bio->outgoing);
}

static void bufio_reflag(CorooBufIO *bio) {
	if (bio->incoming_length == 0)
		bio->incoming_start = 0;
	if (bio->outgoing_length == 0)
		bio->outgoing_start = 0;
	struct pollfd *pd = bio->poll_desc;
	if (bio->incoming_length < bio->buffer_size)
		pd->events |= POLLIN;
	else
		pd->events &= ~POLLIN;
	if (bio->outgoing_length > 0)
		pd->events |= POLLOUT;
	else
		pd->events &= ~POLLOUT;
}

static size_t bufio_wrap(CorooBufIO *bio, size_t sz) {
	if (sz >= bio->buffer_size)
		return sz - bio->buffer_size;
	else
		return sz;
}

CorooBufIO *coroo_bufio_create(size_t buffer_size, struct pollfd *poll_desc) {
	CorooBufIO *bio = refcnt_create(sizeof(*bio), bufio_destructor);
	*bio = (CorooBufIO){};
	bio->buffer_size = buffer_size;
	bio->poll_desc = poll_desc;
	bio->incoming = malloc(buffer_size);
	bio->outgoing = malloc(buffer_size);
	bufio_reflag(bio);
	return bio;
}

short coroo_bufio_update(CorooBufIO *bio) {
	struct pollfd *pd = bio->poll_desc;
	if (pd->revents & (POLLHUP | POLLERR))
		return pd->revents & (POLLHUP | POLLERR);
	if ((pd->revents & POLLIN) && bio->incoming_length < bio->buffer_size) {
		pd->revents &= ~POLLIN;
		size_t start = bufio_wrap(bio, bio->incoming_start + bio->incoming_length);
		size_t end = bio->incoming_start ? bio->incoming_start : bio->buffer_size;
		ssize_t cnt, cnt2;
		if (start < end) {
			cnt = recv(pd->fd, bio->incoming + start, end - start, 0);
		} else {
			cnt = recv(pd->fd, bio->incoming + start, bio->buffer_size - start, 0);
			cnt2 = recv(pd->fd, bio->incoming, end, 0);
			if (cnt2 > 0)
				cnt += cnt2;
		}
		bio->incoming_length += cnt;
		if (cnt == 0)
			return POLLHUP;
	}
	if ((pd->revents & POLLOUT) && bio->outgoing_length > 0) {
		pd->revents &= ~POLLOUT;
		size_t start = bio->outgoing_start;
		size_t end = bufio_wrap(bio, bio->outgoing_start + bio->outgoing_length);
		ssize_t cnt, cnt2;
		if (start < end) {
			cnt = send(pd->fd, bio->outgoing + start, end - start, 0);
		} else {
			cnt = send(pd->fd, bio->outgoing + start, bio->buffer_size - start, 0);
			cnt2 = send(pd->fd, bio->outgoing, end, 0);
			if (cnt2 > 0)
				cnt += cnt2;
		}
		bio->outgoing_start += cnt;
		bio->outgoing_length -= cnt;
		bio->outgoing_start = bufio_wrap(bio, bio->outgoing_start);
		if (cnt == 0)
			return POLLHUP;
	}
	bufio_reflag(bio);
	return 0;
}

void coroo_bufio_tunnel(CorooBufIO *dst, CorooBufIO *src) {
	size_t dst_available = dst->buffer_size - dst->outgoing_length;
	size_t src_available = src->incoming_length;
	size_t full_size = src_available < dst_available ?
		src_available : dst_available;
	while (full_size > 0) {
		size_t dst_ptr = bufio_wrap(dst, dst->outgoing_start + dst->outgoing_length);
		size_t src_ptr = src->incoming_start;
		size_t dst_chunk = dst->buffer_size - dst_ptr;
		size_t src_chunk = src->buffer_size - src_ptr;
		size_t max_chunk = src_chunk < dst_chunk ? src_chunk : dst_chunk;
		size_t chunk_size = max_chunk < full_size ? max_chunk : full_size;
		memcpy(dst->outgoing + dst_ptr, src->incoming + src_ptr, chunk_size);
		dst->outgoing_length += chunk_size;
		src->incoming_start += chunk_size;
		src->incoming_length -= chunk_size;
		src->incoming_start = bufio_wrap(src, src->incoming_start);
		full_size -= chunk_size;
	}
	bufio_reflag(dst);
	bufio_reflag(src);
}
