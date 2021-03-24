SHELL := /bin/bash
PATH := $(PWD)/../depot_tools:$(PATH)

all:
	echo 'Nothing to do' && exit 1

build-with-gn:
	gn gen out/Default
	ninja -C out/Default
.PHONY: build-with-gn

build-with-cmake:
	mkdir -p cmakebuild
	cd cmakebuild; cmake ..
	cmake --build cmakebuild --parallel
.PHONY: build-with-cmake

update-with-gclient:
	gclient sync
.PHONY: update-with-gclient

example: build-with-gn
	g++ -g \
		-o example example.cpp \
		-I. -I./third_party/mini_chromium/mini_chromium \
		-std=c++14 \
		-L./out/Default/obj/client -lclient \
		-L./out/Default/obj/util -lutil \
		-L./out/Default/obj/third_party/mini_chromium/mini_chromium/base -lbase \
		-framework Foundation -framework Security -framework CoreText \
		-framework CoreGraphics -framework IOKit -lbsm
.PHONY: example

gen-sentry-patch:
	git format-patch --stdout master...HEAD > getsentry.patch
.PHONY: get-sentry-patch
