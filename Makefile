CFLAGS  ?= -g -O2 -Wall -Wextra -pedantic -Wno-unused-parameter

CFLAGS += -Isrc
LDFLAGS += -L.

TARGET   = kuroko
OBJS     = $(patsubst %.c, %.o, $(filter-out src/kuroko.c,$(sort $(wildcard src/*.c))))
SOOBJS   = $(patsubst %.o, %.lo, $(OBJS))
MODULES  = $(patsubst src/modules/module_%.c, modules/%.so, $(sort $(wildcard src/modules/module_*.c)))
HEADERS  = $(wildcard src/kuroko/*.h)
TOOLS    = $(patsubst tools/%.c, krk-%, $(sort $(wildcard tools/*.c)))
GENMODS  = modules/codecs/sbencs.krk modules/codecs/dbdata.krk
BIN_OBJS = libkuroko.a

# These are used by the install target. We call the local kuroko to get the
# version string to use for the final library, so, uh, probably don't
# try to do that in a cross-compile environment...
VERSION  = $(shell ./kuroko --version | sed 's/.* //')
SONAME   = libkuroko-$(VERSION).so
KRKMODS  = $(wildcard modules/*.krk modules/*/*.krk modules/*/*/*.krk)

all: ${TARGET} ${MODULES} ${TOOLS} ${GENMODS}

ifneq ($(shell tools/can-floor-without-libm.sh "$(CC) $(CFLAGS)"),yes)
  LDLIBS += -lm
endif

ifeq (,$(findstring mingw,$(CC)))
  CFLAGS  += -pthread
  LDLIBS  += -ldl -lpthread
  BIN_FLAGS = -rdynamic
  LIBRARY = libkuroko.so
  ifeq (Darwin,$(shell uname -s))
    MODLIBS += -undefined dynamic_lookup -DKRK_MEDIOCRE_TLS
  else
    ${TOOLS}: LDFLAGS += '-Wl,-rpath,$$ORIGIN'
  endif
else
  CFLAGS  += -Wno-format -static-libgcc -pthread
  ${SOOBJS}: CFLAGS += -DKRKINLIB
  BIN_OBJS =
  LIBRARY = libkuroko.dll
  kuroko: LDLIBS = -lkuroko
  kuroko: ${LIBRARY}
  MODLIBS += -lkuroko
  modules/socket.so: MODLIBS += -lws2_32
endif

ifdef KRK_DISABLE_DOCS
  CFLAGS += -DKRK_NO_DOCUMENTATION -Wno-unused-value
endif

ifndef KRK_DISABLE_RLINE
  BIN_OBJS += src/vendor/rline.o
else
  CFLAGS  += -DNO_RLINE
endif

ifdef KRK_DISABLE_DEBUG
  CFLAGS  += -DKRK_DISABLE_DEBUG
endif

ifdef KRK_DISABLE_THREADS
  CFLAGS += -DKRK_DISABLE_THREADS
endif

ifdef KRK_NO_DISASSEMBLY
  CFLAGS += -DKRK_NO_DISASSEMBLY=1
endif

ifdef KRK_NO_TRACING
  CFLAGS += -DKRK_NO_TRACING=1
endif

ifdef KRK_NO_STRESS_GC
  CFLAGS += -DKRK_NO_STRESS_GC=1
endif

ifdef KRK_NO_FLOAT
  CFLAGS += -DKRK_NO_FLOAT=1
endif

ifdef KRK_HEAP_TAG_BYTE
  CFLAGS += -DKRK_HEAP_TAG_BYTE=${KRK_HEAP_TAG_BYTE}
endif

.PHONY: help

help:
	@echo "Configuration options available:"
	@echo "   KRK_NO_...             Compile without support for debugging features..."
	@echo "      DISASSEMBLY=1          Do not enable disassembly at compile time."
	@echo "      TRACING=1              Do not enable runtime tracing."
	@echo "      STRESS_GC=1            Do not enable eager GC stress testing."
	@echo "   KRK_DISABLE_THREADS=1  Disable threads on platforms that otherwise support them."
	@echo "   KRK_DISABLE_RLINE=1    Do not build with the rich line editing library enabled."
	@echo "   KRK_DISABLE_DEBUG=1    Disable debugging features (might be faster)."
	@echo "   KRK_DISABLE_DOCS=1     Do not include docstrings for builtins."
	@echo ""
	@echo "Available tools: ${TOOLS}"

kuroko: src/kuroko.c ${BIN_OBJS} ${HEADERS}
	${CC} ${CFLAGS} ${LDFLAGS} ${BIN_FLAGS} -o $@ $< ${BIN_OBJS} ${LDLIBS}

krk-%: tools/%.c ${LIBRARY} ${HEADERS}
	${CC} -Itools ${CFLAGS} ${LDFLAGS} -o $@ $< -lkuroko

libkuroko.so: ${SOOBJS} ${HEADERS}
	${CC} ${CFLAGS} ${LDFLAGS} -fPIC -shared -o $@ ${SOOBJS} ${LDLIBS}

WINLIBS= -l:libwinpthread.a
libkuroko.dll: ${SOOBJS} ${HEADERS}
	${CC} ${CFLAGS} ${LDFLAGS} -fPIC -shared -o $@ ${SOOBJS} ${WINLIBS} -Wl,--export-all-symbols,--out-implib,libkuroko.a

libkuroko.a: ${OBJS}
	${AR} ${ARFLAGS} $@ ${OBJS}


src/chunk.o: src/opcodes.h
src/compiler.o: src/opcodes.h
src/debug.o: src/opcodes.h
src/value.o: src/opcodes.h
src/vm.o: src/opcodes.h
src/exceptions.o: src/opcodes.h


%.o: %.c ${HEADERS}
	${CC} ${CFLAGS} -c -o $@ $<

%.lo: %.c ${HEADERS}
	${CC} ${CFLAGS} -fPIC -c -o $@ $<

modules/math.so: MODLIBS += -lm
modules/%.so: src/modules/module_%.c ${LIBRARY}
	${CC} ${CFLAGS} ${LDFLAGS} -fPIC -shared -o $@ $< ${LDLIBS} ${MODLIBS}

modules/codecs/sbencs.krk: tools/codectools/gen_sbencs.krk tools/codectools/encodings.json tools/codectools/indexes.json | kuroko
	./kuroko tools/codectools/gen_sbencs.krk

modules/codecs/dbdata.krk: tools/codectools/gen_dbdata.krk tools/codectools/encodings.json tools/codectools/indexes.json | kuroko
	./kuroko tools/codectools/gen_dbdata.krk

.PHONY: clean
clean:
	-rm -f ${OBJS} ${SOOBJS} ${TARGET} ${MODULES}
	-rm -f libkuroko.so libkuroko.a libkuroko.dll *.so.debug
	-rm -f src/*.o src/*.lo src/vendor/*.o
	-rm -f kuroko.exe ${TOOLS} $(patsubst %,%.exe,${TOOLS})
	-rm -rf docs/html *.dSYM modules/*.dSYM

tags: $(wildcard src/*.c) $(wildcard src/*.h)
	@ctags --c-kinds=+lx src/*.c src/*.h  src/kuroko/*.h src/vendor/*.h

# Test targets run against all .krk files in the test/ directory, writing
# stdout to `.expect` files, and then comparing with `git`.
# To update the tests if changes are expected, run `make test` and commit the result.
.PHONY: test stress-test update-tests bench
test:
	@for i in test/*.krk; do echo $$i; KUROKO_TEST_ENV=1 $(TESTWRAPPER) ./kuroko $$i > $$i.actual; diff $$i.expect $$i.actual || exit 1; rm $$i.actual; done

update-tests:
	@for i in test/*.krk; do echo $$i; KUROKO_TEST_ENV=1 $(TESTWRAPPER) ./kuroko $$i > $$i.expect; done

# You can also set TESTWRAPPER to other things to run the tests in other tools.
stress-test:
	$(MAKE) TESTWRAPPER='valgrind' test

bench:
	@echo "Kuroko: ($$(./kuroko --version))"
	@for i in bench/*.krk; do ./kuroko "$$i"; done
	@echo "Kuroko ($$(kuroko --version)):"
	@for i in bench/*.krk; do kuroko "$$i"; done
	@echo "CPython ($$(python3 --version))"
	@for i in bench/*.py; do python3 "$$i"; done
	@echo "Micropython ($$(micropython -c 'import sys; print(sys.version)'))"
	@for i in bench/*.py; do micropython -X heapsize=128M "$$i"; done

# Really should be up to you to set, not us...
multiarch   ?= $(shell gcc -print-multiarch)
prefix      ?= /usr/local
exec_prefix ?= $(prefix)
includedir  ?= $(prefix)/include
bindir      ?= $(exec_prefix)/bin
ifeq (/usr,$(prefix))
libdir      ?= $(exec_prefix)/lib/$(multiarch)
else
libdir      ?= $(exec_prefix)/lib
endif
INSTALL = install
INSTALL_PROGRAM=$(INSTALL)
INSTALL_DATA=$(INSTALL) -m 644

.PHONY: install
install: all libkuroko.so ${HEADERS} $(KRKMODS) $(MODULES)
	@echo "Creating directories..."
	$(INSTALL) -d $(DESTDIR)$(includedir)/kuroko
	$(INSTALL) -d $(DESTDIR)$(bindir)
	$(INSTALL) -d $(DESTDIR)$(libdir)
	$(INSTALL) -d $(DESTDIR)$(bindir)/../lib/kuroko
	$(INSTALL) -d $(DESTDIR)$(bindir)/../lib/kuroko/syntax
	$(INSTALL) -d $(DESTDIR)$(bindir)/../lib/kuroko/foo/bar
	$(INSTALL) -d $(DESTDIR)$(bindir)/../lib/kuroko/codecs
	@echo "Installing programs..."
	$(INSTALL_PROGRAM) kuroko $(DESTDIR)$(bindir)/kuroko
	$(INSTALL_PROGRAM) $(TOOLS) $(DESTDIR)$(bindir)/
	@echo "Installing libraries..."
	$(INSTALL_PROGRAM) libkuroko.so $(DESTDIR)$(libdir)/$(SONAME)
	ln -s -f $(SONAME) $(DESTDIR)$(libdir)/libkuroko.so
	$(INSTALL_DATA) libkuroko.a $(DESTDIR)$(libdir)/
	@echo "Installing source modules..."
	$(INSTALL_DATA) modules/*.krk         $(DESTDIR)$(bindir)/../lib/kuroko/
	$(INSTALL_DATA) modules/foo/*.krk     $(DESTDIR)$(bindir)/../lib/kuroko/foo/
	$(INSTALL_DATA) modules/foo/bar/*.krk $(DESTDIR)$(bindir)/../lib/kuroko/foo/bar/
	$(INSTALL_DATA) modules/syntax/*.krk  $(DESTDIR)$(bindir)/../lib/kuroko/syntax/
	$(INSTALL_DATA) modules/codecs/*.krk  $(DESTDIR)$(bindir)/../lib/kuroko/codecs/
	$(INSTALL_PROGRAM) $(MODULES)         $(DESTDIR)$(bindir)/../lib/kuroko/
	@echo "Installing headers..."
	$(INSTALL_DATA) ${HEADERS} $(DESTDIR)$(includedir)/kuroko/
	@echo "You may need to run 'ldconfig'."

install-strip: all
	$(MAKE) INSTALL_PROGRAM='$(INSTALL_PROGRAM) -s' install

LIBCMIN = $(shell readelf -a libkuroko.so kuroko krk-* modules/*.so | grep GLIBC_ | grep Version | sed s"/.*GLIBC_//" | sed s"/  .*//" | sort --version-sort | tail -1)

# The deb target piggybacks off the install target, creating a temporary DESTDIR
# to install into with 'prefix' as /usr, packages that with fpm, and removes DESTDIR
.PHONY: deb
deb: kuroko libkuroko.so
	$(eval DESTDIR := $(shell mktemp -d))
	$(MAKE) prefix=/usr DESTDIR='$(DESTDIR)' install-strip
	fpm -s dir -C $(DESTDIR) -t deb \
		-n            "kuroko" \
		-m            "K. Lange <klange@toaruos.org>" \
		--description "Bytecode-compiled interpreted dynamic programming language." \
		--url         "https://kuroko-lang.github.io/" \
		--license     "MIT" \
		--category    "devel" \
		-d            "libc6 (>= $(LIBCMIN))" \
		--version     $(shell ./kuroko tools/deb-ver.krk) \
		--directories $(libdir)/kuroko
	rm -r $(DESTDIR)

.PHONY: docs
docs: kuroko
	./kuroko tools/gendoc.krk
	doxygen docs/Doxyfile

.PHONY: deploy-docs
deploy-docs: docs
	cp -r docs/html/* ../kuroko-lang.github.io/docs/

.PHONY: cloc
cloc:
	cloc --read-lang-def docs/cloc.txt --vcs git
