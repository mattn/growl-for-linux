PKGCONFIG=gtk+-2.0 libcurl sqlite3
CFLAGS=

all : libdefault.so

libdefault.so : default.o
	gcc -shared `pkg-config --libs ${PKGCONFIG}` -o libdefault.so default.o

default.o : default.c
	gcc `pkg-config --cflags ${PKGCONFIG}` -Wall -c -o default.o default.c

clean:
	-rm *.o *.so
