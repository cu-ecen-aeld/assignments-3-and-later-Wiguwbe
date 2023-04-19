
CROSS_COMPILE?=

CC=$(CROSS_COMPILE)gcc


default: aesdsocket
all: aesdsocket

aesdsocket: aesdsocket.o
	$(CC) -o $@ $<

aesdsocket.o: aesdsocket.c

%.o: %.c
	$(CC) -c $^

clean:
	rm -f aesdsocket *.o
