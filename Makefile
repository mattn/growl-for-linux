PKGCONFIG=gtk+-2.0 openssl sqlite3
CFLAGS=

all : gol

gol : gol.o
	gcc `pkg-config --libs ${PKGCONFIG}` -o gol gol.o

gol.o : gol.c
	gcc `pkg-config --cflags ${PKGCONFIG}` -Wall -c -o gol.o gol.c

clean:
	-rm *.o gol
