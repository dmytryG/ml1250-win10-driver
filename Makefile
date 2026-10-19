# ml1250 - build with any Windows C compiler that can link setupapi/winspool.
#
#   make                 build dist/ml1250.exe
#   make test            build and run the self tests
#   make dist            build everything and lay out dist/ for redistribution
#
# Tested with llvm-mingw clang 22 and with mingw-w64 gcc.  MSVC works too:
#   cl /O2 /Fe:dist\ml1250.exe src\*.c setupapi.lib winspool.lib

CC      ?= clang
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter
LDLIBS  ?= -lsetupapi -lwinspool

SRC     := $(wildcard src/*.c)
LIBSRC  := $(filter-out src/main.c,$(SRC))

.PHONY: all test dist clean

all: dist/ml1250.exe

dist/ml1250.exe: $(SRC) src/ml1250.h
	@mkdir -p dist
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

dist/selftest.exe: tests/selftest.c $(LIBSRC) src/ml1250.h
	@mkdir -p dist
	$(CC) -O1 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -o $@ \
	    tests/selftest.c $(LIBSRC) $(LDLIBS)

dist/infcheck.exe: tests/infcheck.c
	@mkdir -p dist
	$(CC) -O1 -std=c11 -Wall -Wextra -o $@ tests/infcheck.c -lsetupapi

test: dist/selftest.exe dist/infcheck.exe
	./dist/selftest.exe
	./dist/infcheck.exe driver/ml1250.inf
	python tests/gpdcheck.py driver/ml1250.gpd

dist: all dist/selftest.exe dist/infcheck.exe
	@mkdir -p dist/driver
	@cp driver/ml1250.inf driver/ml1250.gpd \
	    driver/Install-ML1250.ps1 driver/Uninstall-ML1250.ps1 \
	    driver/Install.cmd driver/Uninstall.cmd dist/driver/
	@cp README.md dist/
	@echo "dist/ is ready to zip and copy to the target machine"

clean:
	rm -rf dist
