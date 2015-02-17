#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "coroo.h"

void startthread_thread(void *aux) {
	while (true) {
		printf("startthread works!\n");
		sleep(1);
	}
}

void startthread_demo() {
	coroo_thread_start(65536, startthread_thread, NULL);
	coroo_thread_exit();
}

void basicpoll_thread(void *aux) {
	while (true) {
		printf("start poll\n");
		short res = coroo_poll_simple(0, POLLIN, 3000);
		printf("poll complete\n");
		if (res) {
			char buf[512];
			ssize_t cnt = read(0, buf, sizeof(buf));
			write(1, buf, cnt);
		} else {
			printf("timeout!\n");
		}
	}
}

void basicpoll_demo() {
	coroo_thread_start(65536, basicpoll_thread, NULL);
	coroo_thread_exit();
}

void fastyield_thread(void *aux) {
	int id = *(int *)aux;
	while (true) {
		printf("thread %d !\n", id);
		coroo_thread_yield();
	}
}

void fastyield_demo() {
	int ids[4];
	for (int i = 0; i < 4; i++) {
		ids[i] = i;
		coroo_thread_start(65536, fastyield_thread, &ids[i]);
	}
	coroo_thread_exit();
}

void chainstart_thread(void *aux) {
	int id = *(int *)aux;
	printf("thread %d started!\n", id);
	id++;
	coroo_thread_start(65536, chainstart_thread, &id);
	coroo_thread_exit();
}

void chainstart_demo() {
	int id = 1;
	coroo_thread_start(65536, chainstart_thread, &id);
	coroo_thread_exit();
}

int main(int argc, char *argv[]) {
	coroo_thread_init();
	if (argc < 2) {
		fprintf(stderr, "specify a demo name!\n");
		return EXIT_FAILURE;
	}
	const char *demo_name = argv[1];
	if (strcmp(demo_name, "startthread") == 0) {
		startthread_demo();
	} else if (strcmp(demo_name, "basicpoll") == 0) {
		basicpoll_demo();
	} else if (strcmp(demo_name, "fastyield") == 0) {
		fastyield_demo();
	} else if (strcmp(demo_name, "chainstart") == 0) {
		chainstart_demo();
	} else {
		fprintf(stderr, "unknown demo name!\n");
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
