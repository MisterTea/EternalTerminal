CXX?=g++
CXXFLAGS+=-Wall -std=c++14 `pkg-config --cflags gtest_main`
LDFLAGS+=`pkg-config --libs gtest_main`

OBJS=ts-roundtrip.o ts-snippets.o ts-utf8.o ts-bugfix.o ts-quotes.o ts-noconvert.o

BIN=./tests

all: $(BIN)

$(BIN): $(OBJS)
	$(CXX) -o $(BIN) $(OBJS) $(LDFLAGS)

clean:
	rm -f core $(OBJS) $(BIN)

test: $(BIN)
	$(BIN)

$(OBJS): ../SimpleIni.h

