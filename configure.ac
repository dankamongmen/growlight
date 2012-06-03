AC_PREREQ([2.61])
AC_INIT([growlight], [0.0.98], [growlight@sprezzatech.com], [growlight], [http://http://dank.qemfd.net/dankwiki/index.php/Growlight])
AC_CONFIG_AUX_DIR([.])
AM_INIT_AUTOMAKE([-Wall -Werror foreign dist-bzip2])
AC_CONFIG_HEADER([src/config.h])
AC_CONFIG_SRCDIR([src/])

## compiler check
CXXFLAGS="-O2 -Wall -W"
AC_PROG_CXX
AC_PROG_CC_C_O
AC_PROG_CC_C99

AX_PTHREAD

AX_PROG_XSLTPROC
if test "$XSLTPROC" = no ; then
	AC_MSG_ERROR([Cannot find xsltproc.])
fi

AX_CHECK_DOCBOOK_XSLT

PKG_CHECK_MODULES([libudev], [libudev], [AC_DEFINE([HAVE_UDEV], [1], [Use UDEV])])
	CFLAGS+=" $libudev_CFLAGS"
	LIBS+=" $libudev_LIBS"

PKG_CHECK_MODULES([libblkid], [blkid], [have_libblkid=yes])
	CFLAGS+=" $libblkid_CFLAGS"
	LIBS+=" $libblkid_LIBS"

## output
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
