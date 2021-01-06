CFLAGS = -g -O3 -fPIC -Wall -Wextra -pedantic -Wno-unused-parameter -DDEBUG
LDFLAGS = -L. -Wl,-rpath -Wl,.
LDLIBS = -lkuroko -ldl
OBJS = $(patsubst %.c, %.o, $(filter-out rline.c,$(filter-out kuroko.c,$(sort $(wildcard *.c)))))
TARGET = kuroko

MODULES=$(patsubst src/%.c, modules/%.so, $(sort $(wildcard src/*.c)))

all: ${TARGET} ${MODULES}

modules/%.so: src/%.c
	${CC} ${CFLAGS} -shared -fPIC -o $@ $<

libkuroko.so: ${OBJS}
	${CC} ${CLFAGS} -shared -fPIC -o $@ ${OBJS}

builtins.c: builtins.krk
	echo "const char _builtins_src[] = " > builtins.c
	cat builtins.krk | sed s'/\(.*\)/\"\0\\n\"/' >> builtins.c
	echo ";" >> builtins.c

kuroko: libkuroko.so rline.o

.PHONY: clean
clean:
	@rm -f ${OBJS} ${TARGET} ${MODULES} libkuroko.so rline.o

tags: $(wildcard *.c) $(wildcard *.h)
	@ctags --c-kinds=+lx *.c *.h

.PHONY: test

test:
	@for i in test/*.krk; do echo $$i; ./kuroko $$i > $$i.expect; done
	@git diff test/*.expect
