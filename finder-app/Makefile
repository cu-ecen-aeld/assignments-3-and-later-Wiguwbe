
CROSS_COMPILE?=

CC=$(CROSS_COMPILE)gcc

writer: writer.o
	$(CC) -o $@ $<

writer.o: writer.c

%.o: %.c
	$(CC) -c $^

clean:
	rm -f writer *.o
