CFLAGS=

all : gol

gol : gol.o
	gcc `pkg-config --libs gtk+-2.0 openssl sqlite3 libcurl` -o gol gol.o

gol.o : gol.c
	gcc `pkg-config --cflags gtk+-2.0 openssl sqlite3 libcurl` -Wall -c -o gol.o gol.c

clean:
	-rm *.o gol
