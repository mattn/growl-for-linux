PKGCONFIG=gtk+-2.0 libcurl sqlite3
CFLAGS=

all : libballoon.so

libballoon.so : balloon.o
	gcc -shared `pkg-config --libs ${PKGCONFIG}` -o libballoon.so balloon.o

balloon.o : balloon.c
	gcc `pkg-config --cflags ${PKGCONFIG}` -Wall -c -o balloon.o balloon.c

clean:
	-rm *.o libballoon.so
