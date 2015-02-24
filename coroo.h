#include <poll.h>
#include <sys/types.h>

typedef struct CorooThread CorooThread;
typedef void (*CorooThreadFunction)(void *);

void coroo_thread_init();
CorooThread *coroo_thread_start(size_t stack_size,
		CorooThreadFunction thread_function,
		void *thread_argument);
void coroo_thread_yield();
void coroo_thread_exit();

short coroo_poll_simple(int fd, short events, int64_t timeout);
void coroo_poll(struct pollfd *fds, nfds_t nfds, int64_t timeout);

ssize_t coroo_read(int fd, void *buf, size_t count);
ssize_t coroo_write(int fd, const void *buf, size_t count);
ssize_t coroo_readall(int fd, void *buf, size_t count);
ssize_t coroo_writeall(int fd, const void *buf, size_t count);
