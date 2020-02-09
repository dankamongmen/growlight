AC_PREREQ([2.61])
AC_INIT([growlight], [1.2.0], [dankamongmen@gmail.com], [growlight], [https://nick-black.com/dankwiki/index.php/Growlight])
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

AC_ARG_ENABLE([zfs],
 [  --enable-zfs            Build support for ZFS ],
 [ case "${enableval}" in
  yes) zfs=true ;;
  no)  zfs=false ;;
  *) AC_MSG_ERROR([bad value ${enableval} for --enable-zfs]) ;;
   esac ],
 [zfs=true])

AC_ARG_ENABLE([notcurses],
 [  --enable-notcurses      Build support for notcurses ],
 [ case "${enableval}" in
  yes) notcurses=true ;;
  no)  notcurses=false ;;
  *) AC_MSG_ERROR([bad value ${enableval} for --enable-notcurses]) ;;
   esac ],
 [notcurses=true])

# Use pkg-config where we can, to pick up CFLAGS requirements.
PKG_PROG_PKG_CONFIG

# Dependencies of some (but not all) targets must be explicitly claimed by
# their binaries in Makefile.am.

if test "x$zfs" = "xtrue" ; then
  PKG_CHECK_MODULES([ZFS], [libzfs], [AC_DEFINE([HAVE_ZFS], [1], [Use libzfs from ZoL])])
  ZFS_CFLAGS+=" $ZFS_CFLAGS"
	ZFS_LFLAGS+=" $ZFS_LIBS"
fi

# FIXME what about the --without-ncurses/--without-ncursesw cases?
# latter shouldn't be supported. former should preempt this, disable build...
AX_WITH_CURSES
if test "x$ax_cv_ncursesw" != xyes; then
  AC_MSG_ERROR([Requires NcursesW (ncurses with wide-character support)])
else
	CFLAGS+=" $CURSES_CFLAGS"
  PKG_CHECK_MODULES([PANEL], [panelw], [AC_DEFINE([HAVE_PANEL], [1], [Use panelw from ncursesw])])
  CFLAGS+=" $PANEL_CFLAGS"
fi

AX_LIB_READLINE
AC_CHECK_LIB([readline], [main], [have_readline=yes],
	[AC_MSG_ERROR([Cannot find libreadline.])])

# CUnit (unit testing)
PKG_CHECK_MODULES([CUNIT], [cunit >= 2.1], [have_cunit=yes])

# Remaining dependencies are needed by all binaries.
AC_CHECK_LIB(pciaccess, pci_system_init, [have_pciaccess=yes],
             [AC_MSG_ERROR([Cannot find libpciaccess.])])
        LIBS+=" -lpciaccess"

AC_CHECK_LIB(pci, pci_get_dev, [have_libpci=yes],
	[AC_MSG_ERROR([Cannot find libpci.])])
	LIBS+=" -lpci"

if test "x$notcurses" = "xtrue" ; then
  PKG_CHECK_MODULES([notcurses], [notcurses >= 1.1.6])
  	notcurses_CFLAGS+=" $notcurses_CFLAGS"
  	notcurses_LIBS+=" $notcurses_LIBS"
fi

PKG_CHECK_MODULES([libatasmart], [libatasmart >= 0.19], [AC_DEFINE([HAVE_ATASMART], [1], [Use libatasmart])])
	CFLAGS+=" $libatasmart_CFLAGS"
	LIBS+=" $libatasmart_LIBS"

PKG_CHECK_MODULES([libudev], [libudev >= 175], [AC_DEFINE([HAVE_UDEV], [1], [Use UDEV])])
	CFLAGS+=" $libudev_CFLAGS"
	LIBS+=" $libudev_LIBS"

PKG_CHECK_MODULES([libblkid], [blkid >= 2.20.1], [have_libblkid=yes])
	CFLAGS+=" $libblkid_CFLAGS"
	LIBS+=" $libblkid_LIBS"

PKG_CHECK_MODULES([libdevmapper], [devmapper >= 1.02.74], [have_libdevmapper=yes])
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
