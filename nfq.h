/*
 * nfq.h
 *
 * Holger Eitzenberger <holger@eitzenberger.org>, 2013.
 */
#ifndef NFQ_H
#define NFQ_H

#define NFQ_VERSION		"0.1"

#define __UNUSED		__attribute__((unused))
#define __COLD			__attribute__((cold))
#define __PRINTF(idx, first) __attribute__((format (printf, (idx), (first))))

#ifdef DEBUG
#define pr_debug(fmt, ...)		printf((fmt), ## __VA_ARGS__)
#else
#define pr_debug(fmt, ...)
#endif /* DEBUG */

#define BUG()			abort()

void die(const char *fmt, ...) __COLD __PRINTF(1, 2);
void xerr(const char *fmt, ...) __COLD __PRINTF(1, 2);
void xlog(const char *fmt, ...);
void xlog_verbose(int level, const char *fmt, ...);

extern int verbose;

#endif /* NFQ_H */
