CFLAGS = -g -O3 -fPIC -Wall -Wextra -pedantic -Wno-unused-parameter -DDEBUG
LDFLAGS = -L. -Wl,-rpath -Wl,'$$ORIGIN' -Wl,-z,origin
LDLIBS = -lkuroko -ldl
OBJS = $(patsubst %.c, %.o, $(filter-out rline.c,$(filter-out kuroko.c,$(sort $(wildcard *.c)))))
TARGET = kuroko

MODULES=$(patsubst src/%.c, modules/%.so, $(sort $(wildcard src/*.c)))

all: ${TARGET} ${MODULES}

%.o: *.h

modules/%.so: src/%.c
	${CC} ${CFLAGS} -shared -o $@ $<

libkuroko.so: ${OBJS}
	${CC} ${CLFAGS} -shared -o $@ ${OBJS}

builtins.c: builtins.krk
	echo "const char krk_builtinsSrc[] = " > builtins.c
	cat builtins.krk | sed s'/\(.*\)/\"\0\\n\"/' >> builtins.c
	echo ";" >> builtins.c

kuroko: libkuroko.so rline.o

.PHONY: clean
clean:
	@rm -f ${OBJS} ${TARGET} ${MODULES} libkuroko.so rline.o kuroko.o src/*.o

tags: $(wildcard *.c) $(wildcard *.h)
	@ctags --c-kinds=+lx *.c *.h

.PHONY: test

test:
	@for i in test/*.krk; do echo $$i; KUROKO_TEST_ENV=1 ./kuroko $$i > $$i.expect; done
	@git diff test/*.expect
