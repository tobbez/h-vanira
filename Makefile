
.PHONY: clean

all: h-vanira.c
	gcc -Wall -Wextra -ansi -D_POSIX_SOURCE -Os $< -o h-vanira

clean:
	rm -f h-vanira
