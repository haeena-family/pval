
CC = gcc
INCLUDE := -I../include/
LDFLAGS := -pthread
CFLAGS := -g -Wall $(INCLUDE)

PROGNAME = dump-one dump-multi look-tcp

all: $(PROGNAME)

.c.o:
	$(CC) $< -o $@

clean:
	rm -rf *.o
	rm -rf $(PROGNAME)
