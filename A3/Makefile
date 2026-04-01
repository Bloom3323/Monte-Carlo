CC = gcc
CFLAGS = -Wall -g -std=gnu99
LDFLAGS = -lm

all: montecarlo

montecarlo: main.o worker.o
	$(CC) $(CFLAGS) -o montecarlo main.o worker.o $(LDFLAGS)

main.o: main.c montecarlo.h
	$(CC) $(CFLAGS) -c main.c

worker.o: worker.c montecarlo.h
	$(CC) $(CFLAGS) -c worker.c

clean:
	rm -f *.o montecarlo
