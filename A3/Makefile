# Makefile for montecarlo project

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS = -lm

SRCS = main.c worker.c simulate.c protocol.c
OBJS = $(SRCS:.c=.o)
TARGET = montecarlo

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

main.o: main.c montecarlo.h
	$(CC) $(CFLAGS) -c main.c

worker.o: worker.c montecarlo.h
	$(CC) $(CFLAGS) -c worker.c

simulate.o: simulate.c montecarlo.h
	$(CC) $(CFLAGS) -c simulate.c

protocol.o: protocol.c montecarlo.h
	$(CC) $(CFLAGS) -c protocol.c

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
