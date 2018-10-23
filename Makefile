wtfd: wtfd.c
	gcc -std=gnu11 -Wall -O2 -g -o $@ $<

all: wtfd
clean:
	rm -f wtfd

install: wtfd
	mkdir -p $(DESTDIR)/usr/sbin
	cp wtfd $(DESTDIR)/usr/sbin

.PHONY: all clean install
