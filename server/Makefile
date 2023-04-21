
CROSS_COMPILE?=

CC?=$(CROSS_COMPILE)gcc

CFLAGS?=
LDFLAGS?= -lpthread -lrt


default: aesdsocket
all: aesdsocket

aesdsocket: aesdsocket.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

aesdsocket.o: aesdsocket.c

%.o: %.c
	$(CC) $(CFLAGS) -c $^

clean:
	rm -f aesdsocket *.o
