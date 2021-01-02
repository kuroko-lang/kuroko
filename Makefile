CFLAGS = -g -O3 -fPIC -Wall -Wextra -pedantic -Wno-unused-parameter -DDEBUG
LDFLAGS = -L. -Wl,-rpath -Wl,.
LDLIBS = -lkuroko -ldl
OBJS = $(patsubst %.c, %.o, $(filter-out kuroko.c,$(sort $(wildcard *.c))))
TARGET = kuroko

MODULES=$(patsubst src/%.c, modules/%.so, $(sort $(wildcard src/*.c)))

all: ${TARGET} ${MODULES}

modules/%.so: src/%.c
	${CC} ${CFLAGS} -shared -fPIC -o $@ $<

libkuroko.so: ${OBJS}
	${CC} ${CLFAGS} -shared -fPIC -o $@ ${OBJS}

builtins.c: builtins.krk
	echo "const char _builtins_src[] = {\n" > builtins.c
	hexdump -v -e '16/1 "0x%02x,"' -e '"\n"' builtins.krk | sed s'/0x  ,//g' >> builtins.c
	echo "0x00 };" >> builtins.c

kuroko: libkuroko.so

.PHONY: clean
clean:
	@rm -f ${OBJS} ${TARGET} ${MODULES} libkuroko.so

tags: $(wildcard *.c) $(wildcard *.h)
	@ctags --c-kinds=+lx *.c *.h
