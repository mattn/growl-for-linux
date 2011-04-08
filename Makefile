CFLAGS=

all : gol.exe

console : gol.o
	gcc -o gol.exe \
		-mconsole \
		-Lc:/gtk/lib \
		gol.o \
        -Lc:/gtk/lib -lgtk-win32-2.0 -lgdk-win32-2.0 -latk-1.0 -lgio-2.0 -lgdk_pixbuf-2.0 -lpangowin32-1.0 -lgdi32 -lpangocairo-1.0 -lpango-1.0 -lcairo -lgobject-2.0 -lgmodule-2.0 -lxml2 -lgthread-2.0 -lglib-2.0 -lintl \
		-lcurldll \
		-lshell32

gol.exe : gol.o
	gcc -o gol.exe \
		-Lc:/gtk/lib \
		gol.o \
        -Lc:/gtk/lib -lgtk-win32-2.0 -lgdk-win32-2.0 -latk-1.0 -lgio-2.0 -lgdk_pixbuf-2.0 -lpangowin32-1.0 -lgdi32 -lpangocairo-1.0 -lpango-1.0 -lcairo -lgobject-2.0 -lgmodule-2.0 -lxml2 -lgthread-2.0 -lglib-2.0 -lintl \
		-lcurldll \
		-lshell32

gol.o : gol.c
	gcc -Wall -c $(CFLAGS) -o gol.o \
        -mms-bitfields -Ic:/gtk/include/gtk-2.0 -Ic:/gtk/lib/gtk-2.0/include -Ic:/gtk/include/atk-1.0 -Ic:/gtk/include/cairo -Ic:/gtk/include/pango-1.0 -Ic:/gtk/include/glib-2.0 -Ic:/gtk/lib/glib-2.0/include -Ic:/gtk/include/freetype2 -Ic:/gtk/include -Ic:/gtk/include/libpng14 -Ic:/gtk/include/libxml2 \
		gol.c

clean:
	-rm *.o *.res *.exe
