COMPILE = gcc -std=gnu11 -Wall -g

all: debug wtfd waffle-baked

wtfd: wtfd.c
	$(COMPILE) -O3 -o $@ $<

debug: wtfd.c
	$(COMPILE) -O0 -o $@ $<

waffle-baked: waffle-baked.c
	$(COMPILE) -O2 -o $@ $<

clean:
	rm -f wtfd debug waffle-baked

install: wtfd
	mkdir -p $(DESTDIR)/usr/sbin
	cp wtfd $(DESTDIR)/usr/sbin

.PHONY: all clean install
