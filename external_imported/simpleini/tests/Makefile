CC=g++
CFLAGS=-Wall -std=c++11
CPPFLAGS=-Wall -std=c++11
LDFLAGS=-lpthread -lgtest -lgtest_main -lpthread -L/usr/lib -L/usr/lib/x86_64-linux-gnu

OBJS=ts-roundtrip.o ts-snippets.o ts-utf8.o ts-bugfix.o ts-quotes.o ts-noconvert.o

BIN=./tests

all: $(OBJS)
	$(CC) -o $(BIN) $(OBJS) $(LDFLAGS)

clean:
	rm -f core $(OBJS) $(BIN)

test: $(BIN)
	$(BIN)

$(OBJS): ../SimpleIni.h

