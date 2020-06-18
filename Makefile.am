LANG=C
LC_ALL=c

AM_CFLAGS=-std=gnu11 -Wall -g -ggdb -W -Wshadow -Wextra -fvisibility=hidden \
    -D_FORTIFY_SOURCE=2 \
	  $(PTHREAD_CFLAGS) $(OPENSSL_INCLUDES) $(ZFS_CFLAGS)
AM_LDFLAGS=-Wl,-O2,--no-undefined-version,--enable-new-dtags,--as-needed,--warn-common
AM_CC=$(PTHREAD_CC)

LIBS+=$(PTHREAD_LIBS) $(OPENSSL_LIBS) $(ZFS_LIBS)

# FIXME can we not define these with addsuffix or something
dist_man_MANS = growlight-readline.8 growlight.8
dist_xhtml_XHTML = growlight-readline.xhtml growlight.xhtml

doc_DIR=doc
doc_DATA=
doc_DATA+=README.md

EXTRA_DIST=zfs

dist_doc_DATA=
dist_doc_DATA+=COPYING growlight-readline.xhtml growlight.xhtml

AM_CPPFLAGS=-D_GNU_SOURCE

bin_PROGRAMS=growlight-readline
bin_PROGRAMS+=growlight

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
	src/crypt.c src/recipes.h src/recipes.c src/nvme.h src/nvme.c \
	src/stats.h src/stats.c

growlight_readline_SOURCES=$(common_SOURCES)
growlight_readline_SOURCES+=src/readline.c
growlight_readline_LDADD=@notcurses_LIBS@

growlight_SOURCES=$(common_SOURCES)
growlight_SOURCES+=src/notcurses.c src/notcurses.h src/notui-aggregate.h src/notui-aggregate.c
growlight_LDADD=@notcurses_LIBS@

XSLTARGS=--nonet /usr/share/xml/docbook/stylesheet/docbook-xsl
#XSLTARGS=http://docbook.sourceforge.net/release/xsl/current

$(dist_man_MANS): %.8: $(doc_DIR)/man/man8/%.xml $(doc_DIR)/@PACKAGE_NAME@.ent
	$(XSLTPROC) --path $(doc_DIR) -o $@ $(XSLTARGS)/manpages/docbook.xsl $<

$(dist_xhtml_XHTML): %.xhtml: $(doc_DIR)/man/man8/%.xml $(doc_DIR)/@PACKAGE_NAME@.ent
	$(XSLTPROC) --path $(doc_DIR) -o $@ $(XSLTARGS)/xhtml-1_1/docbook.xsl $<

$(doc_DIR)/@PACKAGE_NAME@.ent:
	@mkdir -p $(@D)
	echo -e '<!ENTITY dhfirstname "Nick">\n<!ENTITY dhsurname "Black">\n<!ENTITY dhemail "nickblack@linux.com">\n<!ENTITY dhusername "&dhfirstname; &dhsurname;">\n<!ENTITY dhrelease "$(VERSION)">' > "$@"

clean-local:
	rm -rf $(MANS) $(doc_DIR)/@PACKAGE_NAME@.ent

uninstall-local:
	rmdir "$(DESTDIR)$(docdir)"
