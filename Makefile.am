LANG=C
LC_ALL=c

AM_LIBS=$(PTHREAD_LIBS)
AM_CFLAGS=-O2 -Wall -W -Werror -Wextra -fvisibility=hidden $(PTHREAD_CFLAGS)
AM_LDFLAGS=-Wl,-O2,--no-undefined-version,--enable-new-dtags,--as-needed,--warn-common
AM_CC=$(PTHREAD_CC)

doc_DATA=
doc_DATA+=README

dist_doc_DATA=
dist_doc_DATA+=COPYING

AM_CPPFLAGS=-D_GNU_SOURCE

bin_PROGRAMS=
bin_PROGRAMS+=growlight

growlight_SOURCES=
growlight_SOURCES+=src/growlight.c src/libblkid.c
