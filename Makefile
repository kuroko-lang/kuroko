CFLAGS = -g -O3 -Wall -Wextra -pedantic -Wno-unused-parameter -DDEBUG
OBJS = $(patsubst %.c, %.o, $(filter-out builtins.o,$(sort $(wildcard *.c)))) builtins.o
TARGET = kuroko

all: ${TARGET}

builtins.c: builtins.krk
	echo "const char _builtins_src[] = {\n" > builtins.c
	hexdump -v -e '16/1 "0x%02x,"' -e '"\n"' builtins.krk | sed s'/0x  ,//g' >> builtins.c
	echo "};" >> builtins.c

kuroko: ${OBJS}

.PHONY: clean
clean:
	@rm -f ${OBJS} ${TARGET} builtins.c

tags: $(wildcard *.c) $(wildcard *.h)
	@ctags --c-kinds=+lx *.c *.h
