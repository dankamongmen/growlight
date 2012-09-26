AC_PREREQ([2.61])
AC_INIT([growlight], [1.0.2-rc6], [sprezzos-dev@sprezzatech.com], [growlight], [http://http://nick-black.com/dankwiki/index.php/Growlight])
AC_CONFIG_AUX_DIR([.])
AM_INIT_AUTOMAKE([-Wall -Werror foreign dist-xz std-options subdir-objects nostdinc color-tests])
AC_CONFIG_HEADER([src/config.h])
AC_CONFIG_SRCDIR([src/])

## compiler check
CXXFLAGS="-O2 -Wall -W"
AC_PROG_CXX
AC_PROG_CC
AC_PROG_CC_C_O
AC_PROG_CC_C99
AC_C_INLINE
AC_C_RESTRICT
AC_CHECK_HEADERS([wchar.h])

AX_PTHREAD

AX_PROG_XSLTPROC
if test "$XSLTPROC" = no ; then
	AC_MSG_ERROR([Cannot find xsltproc.])
fi

AC_CHECK_FUNC(epoll_create1,[],[AC_MSG_ERROR([Cannot use epoll_create1()])])

AX_CHECK_DOCBOOK_XSLT

AX_CHECK_OPENSSL([],[AC_MSG_ERROR([Could not find OpenSSL])])

AC_ARG_ENABLE([zfs],
 [  --enable-zfs               Build support for ZFS ],
 [ case "${enableval}" in
  yes) zfs=true ;;
  no)  zfs=false ;;
  *) AC_MSG_ERROR([bad value ${enableval} for --enable-zfs]) ;;
   esac ],
 [zfs=false])

if test "x$zfs" = "xtrue" ; then
	AC_CHECK_LIB(zfs,libzfs_init, [have_libzfs=yes],
		[AC_MSG_ERROR([Cannot find libzfs.])])
if test "x$have_libzfs" = xyes; then
	LIBS+=" -lzfs -lnvpair"
	AC_CHECK_HEADER([libzfs.h],[],[AC_MSG_ERROR([Found ZFS library but not headers.])])
	AC_DEFINE([HAVE_LIBZFS], [1], [Use libzfs])
fi
fi

AX_WITH_CURSES
AX_WITH_CURSES_PANEL
if test "x$ax_cv_curses" = xyes; then
	CFLAGS+=" -D_XOPEN_SOURCE_EXTENDED"
fi

AC_CHECK_LIB(pciaccess,pci_system_init, [have_pciaccess=yes],
             [AC_MSG_ERROR([Cannot find libpciaccess.])])
        LIBS+=" -lpciaccess"

AC_CHECK_LIB(pci,pci_get_dev, [have_libpci=yes],
	[AC_MSG_ERROR([Cannot find libpci.])])
	LIBS+=" -lpci"

PKG_PROG_PKG_CONFIG

PKG_CHECK_MODULES([libatasmart], [libatasmart], [AC_DEFINE([HAVE_ATASMART], [1], [Use libatasmart])])
	CFLAGS+=" $libatasmart_CFLAGS"
	LIBS+=" $libatasmart_LIBS"

PKG_CHECK_MODULES([libudev], [libudev], [AC_DEFINE([HAVE_UDEV], [1], [Use UDEV])])
	CFLAGS+=" $libudev_CFLAGS"
	LIBS+=" $libudev_LIBS"

AX_LIB_READLINE
AC_CHECK_LIB([readline], [main], [have_readline=yes],
	[AC_MSG_ERROR([Cannot find libreadline.])])

PKG_CHECK_MODULES([libblkid], [blkid], [have_libblkid=yes])
	CFLAGS+=" $libblkid_CFLAGS"
	LIBS+=" $libblkid_LIBS"

PKG_CHECK_MODULES([libdevmapper], [devmapper], [have_libdevmapper=yes])
	CFLAGS+=" $libdevmapper_CFLAGS"
	LIBS+=" $libdevmapper_LIBS"

PKG_CHECK_MODULES([libcryptsetup], [libcryptsetup], [have_libcryptsetup=yes])
	CFLAGS+=" $libcryptsetup_CFLAGS"
	LIBS+=" $libcryptsetup_LIBS"

## output
AC_SUBST([CFLAGS])
AC_SUBST([CONFIGURED_UIS])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
