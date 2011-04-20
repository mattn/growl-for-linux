#!/bin/sh

libtoolize --copy --force --install
autoreconf --force --install
./configure
make dist-bzip2
make distclean

rm -rf INSTALL aclocal.m4 autom4te.cache/ compile config.{sub,guess} configure depcomp install-sh ltmain.sh  m4/ missing
find . -name Makefile.in -exec rm {} +
