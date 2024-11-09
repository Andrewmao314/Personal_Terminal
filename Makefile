CFLAGS = -g3 -Wall -Wextra -Wconversion -Wcast-qual -Wcast-align
CFLAGS += -Winline -Wfloat-equal -Wnested-externs
CFLAGS += -pedantic -std=gnu99 -Werror -D_GNU_SOURCE
CC = gcc
SOURCES = sh.c jobs.c
PROMPT = -DPROMPT
EXECS = 33sh 33noprompt

.PHONY: all clean

all: $(EXECS)

33sh: $(SOURCES)
	$(CC) $(CFLAGS) $(PROMPT) $(SOURCES) -o $@
33noprompt: $(SOURCES)
	$(CC) $(CFLAGS) $(SOURCES) -o $@
clean:
	#TODO: clean up any executable files that this Makefile has produced
	rm -f $(EXECS)
