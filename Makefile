COMPILE = gcc -std=gnu11 -Wall -g

all: debug wtfd

wtfd: wtfd.c
	$(COMPILE) -O3 -o $@ $<

debug: wtfd.c
	$(COMPILE) -O0 -o $@ $<

clean:
	rm -f wtfd debug

install: wtfd
	mkdir -p $(DESTDIR)/usr/sbin
	cp wtfd $(DESTDIR)/usr/sbin

dch-release:
	gbp dch --release --git-author

release:
	gbp buildpackage

.PHONY: all clean install
