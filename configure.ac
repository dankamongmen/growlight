AC_PREREQ([2.61])
AC_INIT([growlight], [1.0.6.2], [dankamongmen@gmail.com], [growlight], [https://nick-black.com/dankwiki/index.php/Growlight])
AC_CONFIG_AUX_DIR([.])
AM_INIT_AUTOMAKE([-Wall foreign dist-xz no-dist-gzip std-options subdir-objects nostdinc])
AC_CONFIG_HEADER([src/config.h])
AC_CONFIG_SRCDIR([src/])

## compiler check
AC_PROG_CXX
AC_PROG_CC
AC_PROG_CC_C_O
AM_PROG_CC_C_O
AC_C_INLINE
AC_C_RESTRICT
AC_CHECK_HEADERS([wchar.h])

AX_PTHREAD

AX_PROG_XSLTPROC
if test "x$XSLTPROC" = "x" ; then
	AC_MSG_ERROR([Cannot find xsltproc.])
fi

AC_CHECK_FUNC(epoll_create,[],[AC_MSG_ERROR([Cannot use epoll_create()])])
AC_CHECK_FUNCS_ONCE(epoll_create1)

AX_CHECK_DOCBOOK_XSLT

AX_CHECK_OPENSSL([],[AC_MSG_ERROR([Could not find OpenSSL])])

AC_ARG_WITH([zfs-include],
	[AS_HELP_STRING([--with-zfs-include],
		[Location of the ZFS headers. Defaults to /usr/local/include/libzfs.])],
	[ZFS_CFLAGS="-I$withval"],
	[ZFS_CFLAGS='-I/usr/local/include/libzfs'])
AC_SUBST([ZFS_CFLAGS])

AC_ARG_WITH([zfs-lib],
	[AS_HELP_STRING([--with-zfs-lib], [Location of the ZFS libraries.])],
	[ZFS_LIBS="-L$withval"],
	[ZFS_LIBS=''])
AC_SUBST([ZFS_LIBS])

AC_ARG_ENABLE([zfs],
 [  --enable-zfs               Build support for ZFS ],
 [ case "${enableval}" in
  yes) zfs=true ;;
  no)  zfs=false ;;
  *) AC_MSG_ERROR([bad value ${enableval} for --enable-zfs]) ;;
   esac ],
 [zfs=false])

if test "x$zfs" = "xtrue" ; then
	CFLAGS+=" $ZFS_CFLAGS"
	LFLAGS+=" $ZFS_LIBS"
	AC_CHECK_LIB(zfs,libzfs_init, [have_libzfs=yes],
		[AC_MSG_ERROR([Cannot find libzfs.])])
if test "x$have_libzfs" = xyes; then
	LIBS+=" -lzfs -lnvpair"
	AC_CHECK_HEADER([libzfs.h],[],[AC_MSG_ERROR([Found ZFS library but not headers.])])
	AC_DEFINE([HAVE_LIBZFS], [1], [Use libzfs])
fi
fi

# Use pkg-config where we can, to pick up CFLAGS requirements.
PKG_PROG_PKG_CONFIG

# Dependencies of some (but not all) targets must be explicitly claimed by
# their binaries in Makefile.am.
AX_WITH_CURSES
AX_WITH_CURSES_PANEL
PKG_CHECK_MODULES([libpanel], [panel], [AC_DEFINE([HAVE_PANEL], [1], [Use libpanel from ncursesw])])
if test "x$ax_cv_curses" = xyes; then
	CFLAGS+=" -D_XOPEN_SOURCE_EXTENDED"
fi

AX_LIB_READLINE
AC_CHECK_LIB([readline], [main], [have_readline=yes],
	[AC_MSG_ERROR([Cannot find libreadline.])])

# CUnit (unit testing)
PKG_CHECK_MODULES([CUNIT], [cunit >= 2.1], [have_cunit=yes])

# Remaining dependencies are needed by all binaries.
AC_CHECK_LIB(pciaccess,pci_system_init, [have_pciaccess=yes],
             [AC_MSG_ERROR([Cannot find libpciaccess.])])
        LIBS+=" -lpciaccess"

AC_CHECK_LIB(pci,pci_get_dev, [have_libpci=yes],
	[AC_MSG_ERROR([Cannot find libpci.])])
	LIBS+=" -lpci"

PKG_CHECK_MODULES([libatasmart], [libatasmart], [AC_DEFINE([HAVE_ATASMART], [1], [Use libatasmart])])
	CFLAGS+=" $libatasmart_CFLAGS"
	LIBS+=" $libatasmart_LIBS"

PKG_CHECK_MODULES([libudev], [libudev], [AC_DEFINE([HAVE_UDEV], [1], [Use UDEV])])
	CFLAGS+=" $libudev_CFLAGS"
	LIBS+=" $libudev_LIBS"

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
