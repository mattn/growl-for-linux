PKGCONFIG=gtk+-2.0 openssl sqlite3
CFLAGS=

all : gol displays

displays:
	cd display/default && ${MAKE} -f Makefile && cp libdefault.so ..
	cd display/balloon && ${MAKE} -f Makefile && cp libballoon.so ..
	cd display/nico2 && ${MAKE} -f Makefile && cp libnico2.so ..

gol : gol.o
	gcc -g `pkg-config --libs ${PKGCONFIG}` -o gol gol.o

gol.o : gol.c
	gcc -g `pkg-config --cflags ${PKGCONFIG}` -Wall -c -o gol.o gol.c

clean:
	-rm *.o gol
