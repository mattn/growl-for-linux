PKGCONFIG=gtk+-2.0 libcurl sqlite3
CFLAGS=

all : libfeed.so

libfeed.so : feed.o
	gcc -shared `pkg-config --libs ${PKGCONFIG}` -o libfeed.so feed.o

feed.o : feed.c
	gcc `pkg-config --cflags ${PKGCONFIG}` -Wall -c -o feed.o feed.c

clean:
	-rm *.o *.so
