#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <unistd.h>
#include <setjmp.h>
#include <sys/mman.h>

#include "coroo.h"

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
	void *stack_base;
	size_t stack_size; // including guard page
	CorooThreadFunction thread_function;
	void *thread_argument;
};

static StackDirection stack_direction = STACK_DIRECTION_UNKNOWN;

static CorooThread main_thread;
static CorooThread *current_thread;
static List ready_threads;
static List waiting_threads;
static List dead_threads;

#define list_entry(elem, type, field) \
	(type *)((uintptr_t)elem - offsetof(type, field))

static void list_init(List *list) {
	list->anchor.prev = &list->anchor;
	list->anchor.next = &list->anchor;
}

static bool list_empty(List *list) {
	return list->anchor.prev == list->anchor.next;
}

static ListElement *list_remove(ListElement *elem) {
	elem->prev->next = elem->next;
	elem->next->prev = elem->prev;
	return elem;
}

static void list_push_front(List *list, ListElement *elem) {
	elem->prev = &list->anchor;
	elem->next = list->anchor.next;
	elem->prev->next = elem;
	elem->next->prev = elem;
}

static void list_push_back(List *list, ListElement *elem) {
	elem->prev = list->anchor.prev;
	elem->next = &list->anchor;
	elem->prev->next = elem;
	elem->next->prev = elem;
}

static ListElement *list_pop_front(List *list) {
	return list_remove(list->anchor.next);
}

static ListElement *list_pop_back(List *list) {
	return list_remove(list->anchor.prev);
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

static void thread_invoke_function_actual(CorooThread *thread, char *filler) {
	// return control
	if (setjmp(thread->thread_state) == 0)
		longjmp(current_thread->thread_state, 1);
	// run!
	thread->thread_function(thread->thread_argument);
	// must not return
	coroo_thread_exit();
}

static void thread_invoke_function(CorooThread *thread, char *filler) {
	void (*func)(CorooThread *, char *) = thread_invoke_function_actual;
	maybe_clobber_pointer((void **)&func);
	func(thread, filler);
}

static void thread_start_helper_actual(CorooThread *thread, size_t jump) {
	char filler[jump];
	thread_invoke_function(thread, filler);
}

static void thread_start_helper(CorooThread *thread, size_t jump) {
	void (*func)(CorooThread *, size_t) = thread_start_helper_actual;
	maybe_clobber_pointer((void **)&func);
	func(thread, jump);
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

static void run_next_thread() {
	while (!list_empty(&ready_threads)) {
		CorooThread *self = current_thread;
		CorooThread *next = list_entry(
				list_pop_front(&ready_threads),
				CorooThread,
				list_elem);
		// do context switch
		current_thread = next;
		if (setjmp(self->thread_state) == 0)
			longjmp(next->thread_state, 1);
	}
	// no ready threads
	printf("no ready threads!");
	abort();
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
	void *stack_base = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (!stack_base)
		fputs("warning: failed to map memory for stack\n", stderr);
	// make guard page
	void *guard_start = stack_direction == STACK_DIRECTION_DOWN ?
		stack_base :
		(void *)((uintptr_t)stack_base + stack_size - page_size);
	if (mprotect(guard_start, page_size, PROT_NONE) != 0)
		fputs("warning: failed to set guard page\n", stderr);
	// initialize the thread
	CorooThread *thread = malloc(sizeof(CorooThread));
	thread->stack_base = stack_base;
	thread->stack_size = stack_size;
	thread->thread_function = thread_function;
	thread->thread_argument = thread_argument;
	// initialize the thread state
	size_t jump;
	if (stack_direction == STACK_DIRECTION_DOWN)
		jump = (uintptr_t)&thread - ((uintptr_t)stack_base + stack_size - page_size);
	else
		jump = ((uintptr_t)stack_base + page_size) - (uintptr_t)&jump;
	if (setjmp(current_thread->thread_state) == 0)
		thread_start_helper(thread, jump);
	// mark it as ready
	list_push_back(&ready_threads, &thread->list_elem);
	// return the thread
	return thread;
}

void coroo_thread_exit() {
	list_push_back(&dead_threads, &current_thread->list_elem);
	run_next_thread();
}
