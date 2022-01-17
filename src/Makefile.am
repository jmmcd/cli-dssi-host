bin_PROGRAMS = cli-dssi-host

AM_CFLAGS = -Wall -std=c99 $(DSSI_CFLAGS) $(SNDFILE_CFLAGS) $(ALSA_CFLAGS)
AM_LIBS = $(DSSI_LIBS) $(SNDFILE_LIBS) $(ALSA_LIBS)

cli_dssi_host_SOURCES = cli-dssi-host.c cli-dssi-host.h
cli_dssi_host_LDADD = $(AM_LIBS)

EXTRA_DIST = lts.prs
