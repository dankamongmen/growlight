LANG=C
LC_ALL=c

AM_LIBS=$(PTHREAD_LIBS)
AM_CFLAGS=-O2 -Wall -W -Werror -Wextra -fvisibility=hidden $(PTHREAD_CFLAGS)
AM_LDFLAGS=-Wl,-O2,--no-undefined-version,--enable-new-dtags,--as-needed,--warn-common
AM_CC=$(PTHREAD_CC)

man8_MANS = growlight.8
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
growlight_tty_SOURCES+=src/growlight.c src/libblkid.c src/readline.c \
		       src/mdadm.c src/sysfs.c src/mounts.c src/mmap.c

%.8: $(doc_DIR)/man/man8/%.xml $(doc_DIR)/@PACKAGE_NAME@.ent
	$(XSLTPROC) --path $(doc_DIR) -o $@ --nonet /usr/share/xml/docbook/stylesheet/docbook-xsl/manpages/docbook.xsl $<

$(doc_DIR)/@PACKAGE_NAME@.ent:
	@mkdir -p $(@D)
	echo -e '<!ENTITY dhfirstname "Nick">\n<!ENTITY dhsurname "Black">\n<!ENTITY dhemail "nick.black@sprezzatech.com">\n<!ENTITY dhusername "&dhfirstname; &dhsurname;">\n<!ENTITY dhrelease "$(VERSION)">' > "$@"

clean-local:
	rm -rf $(MANS)
