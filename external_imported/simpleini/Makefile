# This makefile is just to build the automatic test harness
# To use SimpleIni, just include SimpleIni.h header file 

PREFIX?=	/usr/local

TOPTARGETS := all clean test

SUBDIRS := tests

$(TOPTARGETS): $(SUBDIRS)
$(SUBDIRS):
	$(MAKE) -C $@ $(MAKECMDGOALS)

.PHONY: $(TOPTARGETS) $(SUBDIRS)

install:
	mkdir -p $(DESTDIR)$(PREFIX)/include/
	install -C -m 644 SimpleIni.h $(DESTDIR)$(PREFIX)/include/
