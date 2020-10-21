CFLAGS=$(shell pkg-config --cflags json-c)
LDLIBS=$(shell pkg-config --libs json-c)

.PHONY: clean

all: stfjson

clean:
	rm -f *.o stfjson
