CFLAGS+=-O3
LDFLAGS+=-lgit2 -ldb

SOURCES:=$(wildcard *.c)
HEADERS:=$(wildcard *.h)

git-replace:	$(SOURCES) $(HEADERS)
	$(CC) -o git-replace $(CFLAGS) $(SOURCES) $(LDFLAGS)
