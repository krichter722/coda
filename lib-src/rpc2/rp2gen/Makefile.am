## Process this file with automake to produce Makefile.in

CCLD=$(NATIVECC)
#EXEEXT=
AM_YFLAGS= -d
AM_CPPFLAGS= -I$(top_builddir)/include -I$(top_srcdir)/include -I$(srcdir) -I.

# override any cross compilation target flags
CFLAGS=-Wall
LDFLAGS=
LIBS=

.c.o:
	$(NATIVECC) $(AM_CPPFLAGS) -c $<

bin_PROGRAMS = rp2gen

rp2gen_SOURCES = rp2main.c rp2gen_parser.y rp2gen_lexer.l symtab.c crout.c \
    util.c rp2.h rp2gen_parser.h

CLEANFILES = rp2gen_parser.c rp2gen_parser.h rp2gen_lexer.c
MAINTAINERCLEANFILES = Makefile.in

rp2gen_parser.h: rp2gen_parser.c
rp2gen_lexer.c: rp2gen_parser.h

# some systems don't have stdint.h, the generated lexer.c file won't compile
dist-hook:
	-$(RM) $(distdir)/rp2gen_lexer.c

