.PHONY: all clean

all: code

code: main.c buddy.c buddy.h utils.h
	gcc -O2 -std=c11 -Wall -Wextra -o code main.c buddy.c

clean:
	rm -f code