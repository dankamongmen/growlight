LANG=C
LC_ALL=c

AM_CFLAGS=-Wall -g -W -Werror -Wextra -fvisibility=hidden \
	$(PTHREAD_CFLAGS) $(OPENSSL_INCLUDES)
AM_LDFLAGS=-Wl,-O2,--no-undefined-version,--enable-new-dtags,--as-needed,--warn-common \
	$(PTHREAD_LIBS) $(OPENSSL_LIBS) $(CURSES_LIB)
AM_CC=$(PTHREAD_CC)

dist_man_MANS = growlight.8

doc_DIR=doc
doc_DATA=
doc_DATA+=README

dist_doc_DATA=
dist_doc_DATA+=COPYING

AM_CPPFLAGS=-D_GNU_SOURCE

bin_PROGRAMS=
bin_PROGRAMS+=growlight-tty

growlight_tty_SOURCES=
growlight_tty_SOURCES+=src/growlight.c src/growlight.h src/mbr.c src/mbr.h \
	src/libblkid.c src/libblkid.h \
	src/mdadm.c src/mdadm.h src/sysfs.c src/sysfs.h \
	src/mounts.c src/mounts.h src/mmap.c src/mmap.h \
	src/target.c src/target.h src/sg.c src/sg.h src/ptable.c src/ptable.h \
	src/swap.c src/swap.h src/fs.c src/fs.h src/popen.c src/popen.h \
	src/health.c src/health.h src/udev.c src/udev.h \
	src/smart.c src/smart.h src/zfs.c src/zfs.h src/secure.c src/secure.h \
	src/readline.c

%.8: $(doc_DIR)/man/man8/%.xml $(doc_DIR)/@PACKAGE_NAME@.ent
	$(XSLTPROC) --path $(doc_DIR) -o $@ --nonet /usr/share/xml/docbook/stylesheet/docbook-xsl/manpages/docbook.xsl $<

%.xhtml: $(doc_DIR)/man/man8/%.xml $(doc_DIR)/@PACKAGE_NAME@.ent
	$(XSLTPROC) --path $(doc_DIR) -o $@ --nonet /usr/share/xml/docbook/stylesheet/docbook-xsl/xhtml-1_1/docbook.xsl $<

$(doc_DIR)/@PACKAGE_NAME@.ent:
	@mkdir -p $(@D)
	echo -e '<!ENTITY dhfirstname "Nick">\n<!ENTITY dhsurname "Black">\n<!ENTITY dhemail "nick.black@sprezzatech.com">\n<!ENTITY dhusername "&dhfirstname; &dhsurname;">\n<!ENTITY dhrelease "$(VERSION)">' > "$@"

clean-local:
	rm -rf $(MANS)
