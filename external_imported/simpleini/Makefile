help:
	@echo This makefile is just for the test program \(use \"make clean all test\"\)
	@echo Just include the SimpleIni.h header file to use it.

install:
	@echo No install required. Just include the SimpleIni.h header file to use it.

TOPTARGETS := all clean test

SUBDIRS := tests

$(TOPTARGETS): $(SUBDIRS)
$(SUBDIRS):
	$(MAKE) -C $@ $(MAKECMDGOALS)

.PHONY: $(TOPTARGETS) $(SUBDIRS)
