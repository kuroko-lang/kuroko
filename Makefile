CFLAGS = -g -O3 -Wall -Wextra -pedantic -Wno-unused-parameter -DDEBUG
LDFLAGS =  -Wl,--export-dynamic
LDLIBS =-ldl
OBJS = $(patsubst %.c, %.o, $(filter-out builtins.o,$(sort $(wildcard *.c)))) builtins.o
TARGET = kuroko

MODULES=$(patsubst src/%.c, modules/%.so, $(sort $(wildcard src/*.c)))

all: ${TARGET} ${MODULES}

modules/%.so: src/%.c
	${CC} ${CFLAGS} -shared -fPIC -o $@ $<

builtins.c: builtins.krk
	echo "const char _builtins_src[] = {\n" > builtins.c
	hexdump -v -e '16/1 "0x%02x,"' -e '"\n"' builtins.krk | sed s'/0x  ,//g' >> builtins.c
	echo "0x00 };" >> builtins.c

kuroko: ${OBJS}

.PHONY: clean
clean:
	@rm -f ${OBJS} ${TARGET} builtins.c ${MODULES}

tags: $(wildcard *.c) $(wildcard *.h)
	@ctags --c-kinds=+lx *.c *.h
