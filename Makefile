CFLAGS  ?= -g -O3 -Wall -Wextra -pedantic -Wno-unused-parameter

TARGET   = kuroko
OBJS     = $(patsubst %.c, %.o, $(filter-out src/module_% src/rline.c src/kuroko.c,$(sort $(wildcard src/*.c))))
MODULES  = $(patsubst src/module_%.c, modules/%.so, $(sort $(wildcard src/module_*.c)))
HEADERS  = $(wildcard src/*.h)
TOOLS    = $(patsubst tools/%.c, krk-%, $(sort $(wildcard tools/*.c)))

# These are used by the install target. We call the local kuroko to get the
# version string to use for the final library, so, uh, probably don't
# try to do that in a cross-compile environment...
VERSION  = $(shell ./kuroko --version | sed 's/.* //')
SONAME   = libkuroko-$(VERSION).so
KRKMODS  = $(wildcard modules/*.krk modules/*/*.krk modules/*/*/*.krk)

ifndef KRK_ENABLE_STATIC
  # The normal build configuration is as a shared library or DLL (on Windows)
  CFLAGS  += -fPIC
  ifeq (,$(findstring mingw,$(CC)))
    # We set rpath here mostly so you can run the locally-built interpreter
    # with the correct library; it shouldn't be needed in a real installation.
    LDFLAGS += -Wl,-rpath -Wl,'$$ORIGIN' -L.
    # On POSIX-like platforms, link with libdl and assume -lkuroko gives us
    # our own library.
    LDLIBS  += -ldl -lpthread
  else
    # For Windows, disable format string warnings because gcc will get mad
    # about non-portable Windows format specifiers...
    CFLAGS  += -Wno-format
    # And we need to link this by name with extension because I don't want
    # to actually rename it to kuroko.dll or whatever.
  endif
  all: ${TARGET} ${MODULES} ${TOOLS}
  KUROKO_LIBS = libkuroko.so
else
  # Static builds are a little different...
  CFLAGS +=-DSTATIC_ONLY
  LDFLAGS += -static
  all: ${TARGET}
  KUROKO_LIBS = ${OBJS} -lpthread
endif

ifndef KRK_DISABLE_RLINE
  # Normally, we link the rich line editor into the
  # interpreter (and not the main library!)
  KUROKO_LIBS += src/rline.o
else
  # ... but it can be disabled if you want a more "pure" build,
  # or if you don't have solid support for the escape sequences
  # it requires on your target platform.
  CFLAGS  += -DNO_RLINE
endif

ifndef KRK_DISABLE_DEBUG
  # Disabling debug functions doesn't really do much; it may result in a smaller
  # library when stripped as there's a lot of debug text, but no performance
  # difference has ever been noted from disabling, eg., instruction tracing.
  CFLAGS  += -DDEBUG
endif

ifdef KRK_ENABLE_BUNDLE
  # When bundling, disable shared object modules.
  MODULES =
  # Add the sources from the shared object modules as regular sources.
  KUROKO_LIBS += $(patsubst %.c,%.o,$(sort $(wildcard src/module_*.c)))
  # Enable the build flag so the interpreter binary knows to run startup functions
  CFLAGS += -DBUNDLE_LIBS=1
  # And link anything our core modules would have needed
  LDLIBS += -lm
endif

.PHONY: help

help:
	@echo "Configuration options available:"
	@echo "   KRK_DISABLE_RLINE=1    Do not build with the rich line editing library enabled."
	@echo "   KRK_DISABLE_DEBUG=1    Disable debugging features (might be faster)."
	@echo "   KRK_ENABLE_STATIC=1    Build a single static binary."
	@echo "   KRK_ENABLE_BUNDLE=1    Link C modules directly into the interpreter."
	@echo ""
	@echo "Available tools: ${TOOLS}"

kuroko: src/kuroko.o ${KUROKO_LIBS}
	${CC} ${CFLAGS} ${LDFLAGS} -o $@ src/kuroko.o ${KUROKO_LIBS}

krk-%: tools/%.c ${KUROKO_LIBS}
	${CC} -Itools ${CFLAGS} ${LDFLAGS} -o $@ $< ${KUROKO_LIBS}

libkuroko.so: ${OBJS}
	${CC} ${CFLAGS} ${LDFLAGS} -shared -o $@ ${OBJS} ${LDLIBS}

# Make sure we rebuild things when headers change as we have a lot of
# headers that define build flags...
%.o: ${HEADERS}

# Modules are built as shared objects. We link them with LDLIBS
# as well, but this probably isn't necessary?
modules/%.so: src/module_%.c libkuroko.so
	${CC} ${CFLAGS} ${LDFLAGS} -shared -o $@ $< ${LDLIBS}

# A module can have dependencies that didn't exist in the main lib,
# like how the math library pulls in libm but we kept references
# to that out of the main interpreter.
modules/math.so: src/module_math.c libkuroko.so
	${CC} ${CFLAGS} ${LDFLAGS} -shared -o $@ $< -lm ${LDLIBS}

.PHONY: clean
clean:
	@rm -f ${OBJS} ${TARGET} ${MODULES} libkuroko.so src/*.o kuroko.exe ${TOOLS} $(patsubst %,%.exe,${TOOLS})
	@rm -rf docs/html

tags: $(wildcard src/*.c) $(wildcard src/*.h)
	@ctags --c-kinds=+lx src/*.c src/*.h

# Test targets run against all .krk files in the test/ directory, writing
# stdout to `.expect` files, and then comparing with `git`.
# To update the tests if changes are expected, run `make test` and commit the result.
.PHONY: test stress-test update-tests
test:
	@for i in test/*.krk; do echo $$i; KUROKO_TEST_ENV=1 $(TESTWRAPPER) ./kuroko $$i > $$i.actual; diff $$i.expect $$i.actual || exit 1; rm $$i.actual; done

update-tests:
	@for i in test/*.krk; do echo $$i; KUROKO_TEST_ENV=1 $(TESTWRAPPER) ./kuroko $$i > $$i.expect; done

# You can also set TESTWRAPPER to other things to run the tests in other tools.
stress-test:
	$(MAKE) TESTWRAPPER='valgrind' test

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
install: kuroko libkuroko.so ${HEADERS} $(KRKMODS) $(MODULES)
	$(INSTALL) -d $(DESTDIR)$(includedir)/kuroko
	$(INSTALL) -d $(DESTDIR)$(bindir)
	$(INSTALL) -d $(DESTDIR)$(libdir)
	$(INSTALL) -d $(DESTDIR)$(bindir)/../lib/kuroko
	$(INSTALL) -d $(DESTDIR)$(bindir)/../lib/kuroko/syntax
	$(INSTALL) -d $(DESTDIR)$(bindir)/../lib/kuroko/foo/bar
	$(INSTALL_DATA) ${HEADERS} $(DESTDIR)$(includedir)/kuroko/
	$(INSTALL_PROGRAM) kuroko $(DESTDIR)$(bindir)/kuroko
	$(INSTALL_PROGRAM) libkuroko.so $(DESTDIR)$(libdir)/$(SONAME)
	ln -s -f $(SONAME) $(DESTDIR)$(libdir)/libkuroko.so
	$(INSTALL_DATA) modules/*.krk         $(DESTDIR)$(bindir)/../lib/kuroko/
	$(INSTALL_DATA) modules/foo/*.krk     $(DESTDIR)$(bindir)/../lib/kuroko/foo/
	$(INSTALL_DATA) modules/foo/bar/*.krk $(DESTDIR)$(bindir)/../lib/kuroko/foo/bar/
	$(INSTALL_DATA) modules/syntax/*.krk  $(DESTDIR)$(bindir)/../lib/kuroko/syntax/
	$(INSTALL_PROGRAM) $(MODULES)         $(DESTDIR)$(bindir)/../lib/kuroko/
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
		--license     "ISC" \
		--category    "devel" \
		-d            "libc6 (>= $(LIBCMIN))" \
		--version     $(VERSION) \
		--iteration   0 \
		--directories $(libdir)/kuroko
	rm -r $(DESTDIR)

.PHONY: docs

docs: kuroko
	./kuroko tools/gendoc.krk
	doxygen docs/Doxyfile
