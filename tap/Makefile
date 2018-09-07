
CC = gcc
INCLUDE := -I../include/
LDFLAGS := -pthread
CFLAGS := -g -Wall $(INCLUDE)

PROGNAME = tap_trial tun_trial

all: $(PROGNAME)

.c.o:
	$(CC) $< -o $@

clean:
	rm -rf *.o
	rm -rf $(PROGNAME)
