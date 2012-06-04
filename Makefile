VERSION = 1.0
#DEBUG = 1

CC = gcc
AS = as

PREFIX ?= /usr/local

COMMONFLAGS = -Wall -std=gnu99
CFLAGS = $(COMMONFLAGS) -arch i386 -arch x86_64
ifdef DEBUG
CFLAGS += -ggdb -DDEBUG
endif
CFLAGS += -DLIBINSTALLPATH=$(PREFIX)/libexec
CFLAGS += -DVERSION=$(VERSION)
DYLIBFLAGS = -lc -dynamiclib -init _libfakeroot_init
ifndef DEBUG
DYLIBFLAGS += -Wl,-exported_symbol -Wl,_libfakeroot_init -Wl,-x
endif

TARGETS = fakeroot libfakeroot.dylib

default: $(TARGETS)
clean:
	rm -f *.o $(TARGETS)

fakeroot: fakeroot.o
fakeroot-client: fakeroot-client.o communicate.o
fakeroot.o: fakeroot.c communicate.h
communicate.o: communicate.c communicate.h
libfakeroot.dylib: libfakeroot.o sysenter.o intercept.o communicate.o
	gcc $(CFLAGS) $(LDFLAGS) $(DYLIBFLAGS) $+ -o $@
sysenter.o: sysenter-32.o sysenter-64.o
	lipo -create $+ -output $@
sysenter-32.o: sysenter-32.s
	$(AS) -arch i386 $+ -o $@
sysenter-64.o: sysenter-64.s
	$(AS) -arch x86_64 $+ -o $@

install:
	install -d -m755 $(DESTDIR)$(PREFIX)/bin
	install -d -m755 $(DESTDIR)$(PREFIX)/libexec
	install -m755 fakeroot $(DESTDIR)$(PREFIX)/bin
	install -m755 libfakeroot.dylib $(DESTDIR)$(PREFIX)/libexec
