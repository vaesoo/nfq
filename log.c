/*
 * log.c
 *
 * Holger Eitzenberger, 2013.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "nfq.h"


void
die(const char *fmt, ...)
{
	va_list ap;

	flockfile(stderr);
	va_start(ap, fmt);
	if (fmt)
		vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	funlockfile(stderr);

	exit(1);
}

void
xlog(const char *fmt, ...)
{
	va_list ap;

	if (fmt == NULL)
		return;

	flockfile(stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	funlockfile(stdout);
}

void
xlog_verbose(int level, const char *fmt, ...)
{
	va_list ap;

	if (verbose < level)
		return;
	flockfile(stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	funlockfile(stdout);
}

void
xerr(const char *fmt, ...)
{
	va_list ap;

	if (fmt == NULL)
		return;

	flockfile(stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	putchar('\n');
	funlockfile(stderr);
}
