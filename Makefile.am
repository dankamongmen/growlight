LANG=C
LC_ALL=c

# FIXME handle these format-truncation warnings
AM_CFLAGS=-Wall -O2 -W -Wshadow -Werror -Wextra -Wno-format-truncation \
	  -fvisibility=hidden \
	  $(PTHREAD_CFLAGS) $(OPENSSL_INCLUDES) $(ZFS_CFLAGS)
AM_LDFLAGS=-Wl,-O2,--no-undefined-version,--enable-new-dtags,--as-needed,--warn-common
AM_CC=$(PTHREAD_CC)

LIBS+=$(PTHREAD_LIBS) $(OPENSSL_LIBS) $(ZFS_LIBS)

dist_man_MANS = growlight-readline.8 growlight-curses.8

doc_DIR=doc
doc_DATA=
doc_DATA+=README

EXTRA_DIST=zfs

dist_doc_DATA=
dist_doc_DATA+=COPYING growlight-readline.xhtml growlight-curses.xhtml

AM_CPPFLAGS=-D_GNU_SOURCE

bin_PROGRAMS=growlight-readline
bin_PROGRAMS+=growlight-curses

common_SOURCES=src/growlight.c src/growlight.h src/mbr.c src/mbr.h \
	src/libblkid.c src/libblkid.h src/apm.c src/apm.h src/ssd.h src/ssd.c \
	src/mdadm.c src/mdadm.h src/sysfs.c src/sysfs.h \
	src/mounts.c src/mounts.h src/mmap.c src/mmap.h src/dmi.c src/dmi.h \
	src/target.c src/target.h src/sg.c src/sg.h src/ptable.c src/ptable.h \
	src/swap.c src/swap.h src/fs.c src/fs.h src/popen.c src/popen.h \
	src/health.c src/health.h src/udev.c src/udev.h src/msdos.c src/msdos.h \
	src/smart.c src/smart.h src/zfs.c src/zfs.h src/secure.c src/secure.h \
	src/gpt.c src/gpt.h src/crc32.c src/crc32.h src/ptypes.c src/ptypes.h \
	src/dm.c src/dm.h src/aggregate.c src/aggregate.h src/crypt.h \
	src/crypt.c src/recipes.h src/recipes.c

growlight_readline_SOURCES=$(common_SOURCES)
growlight_readline_SOURCES+=src/readline.c
growlight_readline_LDADD=$(CURSES_LIBS)

growlight_curses_SOURCES=$(common_SOURCES)
growlight_curses_SOURCES+=src/ncurses.c src/ncurses.h src/ui-aggregate.h src/ui-aggregate.c
growlight_curses_LDADD=$(PANEL_LIBS) $(CURSES_LIBS)

%.8: $(doc_DIR)/man/man8/%.xml $(doc_DIR)/@PACKAGE_NAME@.ent
	$(XSLTPROC) --path $(doc_DIR) -o $@ --nonet /usr/share/xml/docbook/stylesheet/docbook-xsl/manpages/docbook.xsl $<

%.xhtml: $(doc_DIR)/man/man8/%.xml $(doc_DIR)/@PACKAGE_NAME@.ent
	$(XSLTPROC) --path $(doc_DIR) -o $@ --nonet /usr/share/xml/docbook/stylesheet/docbook-xsl/xhtml-1_1/docbook.xsl $<

$(doc_DIR)/@PACKAGE_NAME@.ent:
	@mkdir -p $(@D)
	echo -e '<!ENTITY dhfirstname "Nick">\n<!ENTITY dhsurname "Black">\n<!ENTITY dhemail "nick.black@sprezzatech.com">\n<!ENTITY dhusername "&dhfirstname; &dhsurname;">\n<!ENTITY dhrelease "$(VERSION)">' > "$@"

clean-local:
	rm -rf $(MANS) $(doc_DIR)/@PACKAGE_NAME@.ent

uninstall-local:
	rmdir "$(DESTDIR)$(docdir)"
