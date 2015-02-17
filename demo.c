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

int main(int argc, char *argv[]) {
	coroo_thread_init();
	if (argc < 2) {
		fprintf(stderr, "specify a demo name!\n");
		return EXIT_FAILURE;
	}
	const char *demo_name = argv[1];
	if (strcmp(demo_name, "startthread") == 0) {
		startthread_demo();
	} else {
		fprintf(stderr, "unknown demo name!\n");
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
