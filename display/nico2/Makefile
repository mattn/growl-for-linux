PKGCONFIG=gtk+-2.0 libcurl sqlite3
CFLAGS=

all : libnico2.so

libnico2.so : nico2.o
	gcc -shared `pkg-config --libs ${PKGCONFIG}` -o libnico2.so nico2.o

nico2.o : nico2.c
	gcc `pkg-config --cflags ${PKGCONFIG}` -Wall -c -o nico2.o nico2.c

clean:
	-rm *.o *.so
