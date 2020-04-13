SHELL	= /bin/sh
CC	= gcc
AR	= ar
LD	= ld
RANLIB	= ranlib
CFLAGS       = -O2 -D _DEBUG -std=gnu99 -fPIC -Wall -Wextra -march=native -ggdb3 -I../include
LDFLAGS      = -shared

SHARED  = libkernkv.so
STATIC  = libkernkv.a
SRC = $(shell echo *.c)
OBJ = $(SRC:.c=.o)
DEP = $(OBJ:%.o=%.d)

PREFIX = $(DESTDIR)/usr/local
BINDIR = $(PREFIX)/bin

all: $(STATIC) $(SHARED)

$(SHARED): $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $^

$(STATIC): $(OBJ)
	$(AR) rcu $@ $^
	$(RANLIB) $@

-include $(DEP)

%.o : %.c
	$(CC) $(CFLAGS) -MMD -c $< -o $@

.PHONY : clean
clean:
	rm -f *.o *.so *.a
