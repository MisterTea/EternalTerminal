all: test

update-test-discovery:
	@perl -ne 'print if s/SENTRY_TEST\(([^)]+)\)/XX(\1)/' tests/unit/*.c | sort | uniq > tests/unit/tests.inc
.PHONY: update-test-discovery

build/Makefile: CMakeLists.txt
	@mkdir -p build
	@cd build; cmake ..

build: build/Makefile
	@cmake --build build --parallel
.PHONY: build

build/sentry_test_unit: build
	@cmake --build build --target sentry_test_unit --parallel

test: update-test-discovery test-integration
.PHONY: test

test-integration: setup-venv
	.venv/bin/pytest tests --verbose
.PHONY: test-integration

test-leaks: update-test-discovery CMakeLists.txt
	@mkdir -p leak-build
	@cd leak-build; cmake \
		-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$(PWD)/leak-build \
		-DSENTRY_BACKEND=none \
		-DWITH_ASAN_OPTION=ON \
		-DCMAKE_C_COMPILER=/usr/local/opt/llvm/bin/clang \
		-DCMAKE_CXX_COMPILER=/usr/local/opt/llvm/bin/clang++ \
		-DCMAKE_LINKER=/usr/local/opt/llvm/bin/clang \
		..
	@cmake --build leak-build --target sentry_test_unit --parallel
	@ASAN_OPTIONS=detect_leaks=1 ./leak-build/sentry_test_unit
.PHONY: test-leaks

clean: build/Makefile
	@$(MAKE) -C build clean
.PHONY: clean

setup: setup-git setup-venv
.PHONY: setup

setup-git: .git/hooks/pre-commit
	git submodule update --init --recursive
.PHONY: setup-git

setup-venv: .venv/bin/python
.PHONY: setup-venv

.git/hooks/pre-commit:
	@cd .git/hooks && ln -sf ../../scripts/git-precommit-hook.sh pre-commit

.venv/bin/python: Makefile tests/requirements.txt
	@rm -rf .venv
	@which virtualenv || sudo pip install virtualenv
	virtualenv -p python3 .venv
	.venv/bin/pip install --upgrade --requirement tests/requirements.txt

format: setup-venv
	@clang-format -i \
		examples/*.c \
		include/*.h \
		src/*.c \
		src/*.h \
		src/*/*.c \
		src/*/*.cpp \
		src/*/*.h \
		tests/unit/*.c \
		tests/unit/*.h
	@.venv/bin/black tests
.PHONY: format

style: setup-venv
	@.venv/bin/python ./scripts/check-clang-format.py -r examples include src tests/unit
	@.venv/bin/black --diff --check tests
.PHONY: style
