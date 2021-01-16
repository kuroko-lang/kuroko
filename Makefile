CFLAGS  ?= -g -O3 -Wall -Wextra -pedantic -Wno-unused-parameter

TARGET   = kuroko
OBJS     = $(patsubst %.c, %.o, $(filter-out rline.c,$(filter-out kuroko.c,$(sort $(wildcard *.c)))))
MODULES  = $(patsubst src/%.c, modules/%.so, $(sort $(wildcard src/*.c)))

ifndef KRK_ENABLE_STATIC
CFLAGS  += -fPIC
LDFLAGS += -L. -Wl,-rpath -Wl,'$$ORIGIN' -Wl,-z,origin
LDLIBS  += -lkuroko -ldl
all: ${TARGET} ${MODULES}
KUROKO_LIBS = libkuroko.so
else
CFLAGS +=-DSTATIC_ONLY
LDFLAGS += -static
all: ${TARGET}
OBJS += kuroko.o
KUROKO_LIBS = ${OBJS}
endif

ifndef KRK_DISABLE_RLINE
KUROKO_LIBS += rline.o
else
CFLAGS  += -DNO_RLINE
endif

ifndef KRK_DISABLE_DEBUG
CFLAGS  += -DDEBUG
endif

ifdef KRK_ENABLE_BUNDLE
MODULES =
KUROKO_LIBS += $(patsubst %.c,%.o,$(sort $(wildcard src/*.c)))
CFLAGS += -DBUNDLE_LIBS=1
endif

.PHONY: help

help:
	@echo "Configuration options available:"
	@echo "   KRK_DISABLE_RLINE=1    Do not build with the rich line editing library enabled."
	@echo "   KRK_DISABLE_DEBUG=1    Disable debugging features (might be faster)."
	@echo "   KRK_ENABLE_STATIC=1    Build a single static binary."
	@echo "   KRK_ENABLE_BUNDLE=1    Link C modules directly into the interpreter."

kuroko: ${KUROKO_LIBS}

%.o: *.h

modules/%.so: src/%.c
	${CC} ${CFLAGS} -shared -o $@ $<

modules/math.so: src/math.c
	${CC} ${CFLAGS} -shared -o $@ $< -lm

libkuroko.so: ${OBJS}
	${CC} ${CLFAGS} -shared -o $@ ${OBJS}

builtins.c: builtins.krk
	echo "const char krk_builtinsSrc[] = " > builtins.c
	cat builtins.krk | sed s'/\(.*\)/\"\0\\n\"/' >> builtins.c
	echo ";" >> builtins.c

.PHONY: clean
clean:
	@rm -f ${OBJS} ${TARGET} ${MODULES} libkuroko.so rline.o kuroko.o src/*.o

tags: $(wildcard *.c) $(wildcard *.h)
	@ctags --c-kinds=+lx *.c *.h

.PHONY: test

test:
	@for i in test/*.krk; do echo $$i; KUROKO_TEST_ENV=1 ./kuroko $$i > $$i.expect; done
	@git diff --exit-code test/*.expect

stress-test:
	@for i in test/*.krk; do echo $$i; KUROKO_TEST_ENV=1 valgrind ./kuroko -g $$i > $$i.expect; done
	@git diff --exit-code test/*.expect
