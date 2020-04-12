SHELL = /bin/sh
CC    = gcc
CFLAGS       = -std=gnu99 -fPIC -pedantic -Wall -Wextra -march=native -ggdb3 -I../include
DEBUGFLAGS   = -O0 -D _DEBUG
RELEASEFLAGS = -O2 -D NDEBUG -combine -fwhole-program
LDFLAGS      = -shared

TARGET  = libkernkv.so
SRC = $(shell echo *.c)
OBJ = $(SRC:.c=.o)
DEP = $(OBJ:%.o=%.d)

PREFIX = $(DESTDIR)/usr/local
BINDIR = $(PREFIX)/bin

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(DEBUGFLAGS) $(LDFLAGS) -o $(TARGET) $(OBJ)

-include $(DEP)

%.o : %.c
	# The -MMD flags additionaly creates a .d file with
	# the same name as the .o file.
	$(CC) $(CFLAGS) -MMD -c $< -o $@

.PHONY : clean
clean:
	rm -f *.o *.so *.a
