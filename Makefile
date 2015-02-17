SRCS := \
	core.c
OBJS := $(SRCS:%.c=%.o)
DEPS := $(SRCS:%.c=%.d)

CC := gcc
CFLAGS := -Os -g -Wall -std=c99 -D_GNU_SOURCE
LDFLAGS := -lrt

all: coroo.a demo

clean:
	rm -f *.o *.d coroo.a demo

.PHONY: all clean

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.d: %.c
	@gcc -MM -MT $(patsubst %.c,%.o,$<) -MT $@ -MF $@ $<

coroo.a: $(OBJS)
	ar rcs $@ $^

demo: demo.o coroo.a
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

-include $(DEPS)
