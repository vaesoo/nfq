# Makefile

LIBNL_CFLAGS := $(shell pkg-config --cflags libnl-route-3.0 libnl-nf-3.0 libnl-cli-3.0 libnl-genl-3.0)
LIBNL_LIBS := $(shell pkg-config --libs libnl-route-3.0 libnl-nf-3.0 libnl-cli-3.0 libnl-genl-3.0)

CC = gcc
CFLAGS = -g -Wall -pthread -D_GNU_SOURCE $(LIBNL_CFLAGS)
LDLIBS = $(LIBNL_LIBS)

LIBNL := $(HOME)/git.repos/libnl/


.PHONY: default all clean distclean TAGS
default: nfq

nfq: nfq.o event.o list.o log.o

clean:
	$(RM) *.o *~

distclean:
	$(RM) *.o *~
	$(RM) nfq TAGS

TAGS:
	etags *.[ch]
