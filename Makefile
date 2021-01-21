CFLAGS  ?= -g -O3 -Wall -Wextra -pedantic -Wno-unused-parameter

TARGET   = kuroko
OBJS     = $(patsubst %.c, %.o, $(filter-out src/module_% src/rline.c src/kuroko.c,$(sort $(wildcard src/*.c))))
MODULES  = $(patsubst src/module_%.c, modules/%.so, $(sort $(wildcard src/module_*.c)))
HEADERS  = $(wildcard src/*.h)

ifndef KRK_ENABLE_STATIC
CFLAGS  += -fPIC
ifeq (,$(findstring mingw,$(CC)))
LDFLAGS += -Wl,-rpath -Wl,'$$ORIGIN' -L.
LDLIBS  += -ldl -lkuroko
else
CFLAGS  += -Wno-format
LDLIBS  += libkuroko.so
endif
all: ${TARGET} ${MODULES}
KUROKO_LIBS = libkuroko.so
else
CFLAGS +=-DSTATIC_ONLY
LDFLAGS += -static
all: ${TARGET}
OBJS += src/kuroko.o
KUROKO_LIBS = ${OBJS}
endif

ifndef KRK_DISABLE_RLINE
KUROKO_LIBS += src/rline.o
else
CFLAGS  += -DNO_RLINE
endif

ifndef KRK_DISABLE_DEBUG
CFLAGS  += -DDEBUG
endif

ifdef KRK_ENABLE_BUNDLE
MODULES =
KUROKO_LIBS += $(patsubst %.c,%.o,$(sort $(wildcard src/module_*.c)))
CFLAGS += -DBUNDLE_LIBS=1
LDLIBS += -lm
endif

.PHONY: help

help:
	@echo "Configuration options available:"
	@echo "   KRK_DISABLE_RLINE=1    Do not build with the rich line editing library enabled."
	@echo "   KRK_DISABLE_DEBUG=1    Disable debugging features (might be faster)."
	@echo "   KRK_ENABLE_STATIC=1    Build a single static binary."
	@echo "   KRK_ENABLE_BUNDLE=1    Link C modules directly into the interpreter."

kuroko: src/kuroko.o ${KUROKO_LIBS}
	${CC} ${CFLAGS} ${LDFLAGS} -o $@ src/kuroko.o ${KUROKO_LIBS} ${LDLIBS}

%.o: ${HEADERS}

modules/%.so: src/module_%.c libkuroko.so
	${CC} ${CFLAGS} ${LDFLAGS} -shared -o $@ $< ${LDLIBS}

modules/math.so: src/module_math.c libkuroko.so
	${CC} ${CFLAGS} ${LDFLAGS} -shared -o $@ $< -lm ${LDLIBS}

libkuroko.so: ${OBJS}
	${CC} ${CFLAGS} ${LDFLAGS} -shared -o $@ ${OBJS}

src/builtins.c: src/builtins.krk
	echo "const char krk_builtinsSrc[] = " > $@
	cat $< | sed s'/\(.*\)/\"\0\\n\"/' >> $@
	echo ";" >> $@

.PHONY: clean
clean:
	@rm -f ${OBJS} ${TARGET} ${MODULES} libkuroko.so src/*.o kuroko.exe

tags: $(wildcard src/*.c) $(wildcard src/*.h)
	@ctags --c-kinds=+lx src/*.c src/*.h

.PHONY: test

test:
	@for i in test/*.krk; do echo $$i; KUROKO_TEST_ENV=1 ./kuroko $$i > $$i.expect; done
	@git diff --exit-code test/*.expect

stress-test:
	@for i in test/*.krk; do echo $$i; KUROKO_TEST_ENV=1 valgrind ./kuroko -g $$i > $$i.expect; done
	@git diff --exit-code test/*.expect
