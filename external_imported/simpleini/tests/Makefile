CC=g++
CFLAGS=-Wall -std=c++11
CPPFLAGS=-Wall -std=c++11
LDFLAGS=-lpthread -lgtest -lgtest_main -lpthread -L/usr/lib

OBJS=ts-roundtrip.o ts-snippets.o ts-utf8.o ts-bugfix.o

BIN=./tests

all: $(OBJS)
	$(CC) -o $(BIN) $(OBJS) $(LDFLAGS)

clean:
	rm -f core $(OBJS) $(BIN)

test: $(BIN)
	$(BIN)

$(OBJS): ../SimpleIni.h

