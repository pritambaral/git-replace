CFLAGS+=-O3
LDFLAGS+=-lgit2 -ldb -lpcrecpp

OBJ = main git strreplace/strreplace
OBJS = $(addsuffix .o,$(OBJ))

all: git-replace

main.o: git.h strreplace/strreplace.o

strreplace/strreplace.o: strreplace/strreplace.hh strreplace/strreplace.cc
	make -C strreplace

git.o: strreplace/strreplace.o

main.o: main.c
	$(CC) -c main.c $(CFLAGS)

git.o: git.c
	$(CC) -c git.c $(CFLAGS)

git-replace:	$(OBJS)
	$(CXX) -o git-replace $(OBJS) $(LDFLAGS)

clean:
	rm *.o
	make -C strreplace/ clean
