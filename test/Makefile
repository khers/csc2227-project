SHELL   = /bin/sh
CC      = gcc
AR      = ar
LD      = ld
RANLIB  = ranlib
CFLAGS  = -O2 -D _DEBUG -std=gnu99 -Wall -Wextra -march=native -ggdb3 -I../include

EXECS	= test_client

all: $(EXECS)

test_client : test_client.o ../lib/libkernkv.a
	$(CC) -o $@ $^

-include $(DEP)

%.o : %.c
	$(CC) $(CFLAGS) -MMD -c $< -o $@

.PHONY : clean
clean:
	rm -f *.o *.so *.a $(EXECS)
