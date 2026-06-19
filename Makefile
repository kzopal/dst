# dst - dynamic suckless terminal
# See LICENSE file for copyright and license details.
.POSIX:

include config.mk

SRC = st.c x.c config.c rebuild.c
OBJ = $(SRC:.c=.o)

all: dst

config.h:
	cp config.def.h config.h

.c.o:
	$(CC) $(STCFLAGS) -c $<

st.o: config.h st.h win.h
x.o: arg.h config.h st.h win.h rebuild.h

$(OBJ): config.h config.mk

dst: $(OBJ)
	$(CC) -o $@ $(OBJ) $(STLDFLAGS)

clean:
	rm -f dst $(OBJ) dst-$(VERSION).tar.gz

dist: clean
	mkdir -p dst-$(VERSION)
	cp -R FAQ LEGACY TODO LICENSE Makefile README config.mk\
		config.def.h config.sample st.info dst.1 arg.h st.h win.h rebuild.h $(SRC)\
		dst-$(VERSION)
	tar -cf - dst-$(VERSION) | gzip > dst-$(VERSION).tar.gz
	rm -rf dst-$(VERSION)

install: dst
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f dst $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/dst
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	sed "s/VERSION/$(VERSION)/g" < dst.1 > $(DESTDIR)$(MANPREFIX)/man1/dst.1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/dst.1
	tic -sx st.info
	@echo Please see the README file regarding the terminfo entry of dst.

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/dst
	rm -f $(DESTDIR)$(MANPREFIX)/man1/dst.1

.PHONY: all clean dist install uninstall
