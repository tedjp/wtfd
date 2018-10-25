COMPILE = gcc -std=gnu11 -Wall -g -fmax-errors=5

all: debug proxymoron

proxymoron: proxymoron.c
	$(COMPILE) -O3 -o $@ $<

debug: proxymoron.c
	$(COMPILE) -O0 -o $@ $<

clean:
	rm -f proxymoron debug

install: proxymoron
	mkdir -p $(DESTDIR)/usr/sbin
	cp $< $(DESTDIR)/usr/sbin

.PHONY: all clean install
