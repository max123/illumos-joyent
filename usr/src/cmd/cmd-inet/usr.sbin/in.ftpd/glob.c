/*
 * Copyright (c) 2011 Gary Mills
 *
 * Copyright (c) 1989, 2010, Oracle and/or its affiliates. All rights reserved.
 */

/****************************************************************************    
  Copyright (c) 1999,2000,2001 WU-FTPD Development Group.  
  All rights reserved.
  
  Portions Copyright (c) 1980, 1985, 1988, 1989, 1990, 1991, 1993, 1994 
    The Regents of the University of California.
  Portions Copyright (c) 1993, 1994 Washington University in Saint Louis. 
  Portions Copyright (c) 1996, 1998 Berkeley Software Design, Inc. 
  Portions Copyright (c) 1989 Massachusetts Institute of Technology. 
  Portions Copyright (c) 1998 Sendmail, Inc. 
  Portions Copyright (c) 1983, 1995, 1996, 1997 Eric P.  Allman. 
  Portions Copyright (c) 1997 by Stan Barber. 
  Portions Copyright (c) 1997 by Kent Landfield. 
  Portions Copyright (c) 1991, 1992, 1993, 1994, 1995, 1996, 1997 
    Free Software Foundation, Inc.   
  
  Use and distribution of this software and its source code are governed  
  by the terms and conditions of the WU-FTPD Software License ("LICENSE"). 
  
  If you did not receive a copy of the license, it may be obtained online 
  at http://www.wu-ftpd.org/license.html. 
  
  $Id: glob.c,v 1.14.2.2 2001/11/29 17:01:38 wuftpd Exp $ 
  
****************************************************************************/
/*
 * C-shell glob for random programs.
 */

#include "config.h"

#include <sys/param.h>
#include <sys/stat.h>

#ifdef HAVE_DIRENT_H
#include <dirent.h>
#else
#include <sys/dir.h>
#endif

#include <pwd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto.h"

#define	QUOTE 0200
#define	TRIM 0177
#define	eq(a,b)		(strcmp(a, b)==0)
#define	GAVSIZ		(1024 * 8)
#define	isdir(d)	((d.st_mode & S_IFMT) == S_IFDIR)

#define	GLOB_LIMIT_MALLOC	65536
#define	GLOB_LIMIT_STAT		128
#define	GLOB_LIMIT_READDIR	16384

#define	GLOB_INDEX_MALLOC	0
#define	GLOB_INDEX_STAT		1
#define	GLOB_INDEX_READDIR	2

static char **gargv;		/* Pointer to the (stack) arglist */
static char **agargv;
static size_t agargv_size;
static int gargc;		/* Number args in gargv */
static size_t gnleft;
static short gflag;
static int tglob(register char);

/* 0 = malloc(), 1 = stat(), 2 = readdir() */
static size_t limits[] = { 0, 0, 0 };

/* Prototypes */

static char *strend(register char *);
static void addpath(char);
static void ginit(char **);
static void collect(register char *, boolean_t check_ncargs);
static void acollect(register char *, boolean_t check_ncargs);
static void sort(void);
static void expand(char *, boolean_t check_ncargs);
static void matchdir(char *, boolean_t check_ncargs);
static int execbrc(char *, char *);
static int match(char *, char *, boolean_t check_ncargs);
static int amatch(char *, char *, boolean_t check_ncargs);
static void Gcat(register char *, register char *, boolean_t check_ncargs);
static void rscan(register char **, int (*f) (register char));
static int tglob(register char c);
static int gethdir(char *);

int letter(register char);
int digit(register char);
int any(register int, register char *);
int blklen(register char **);
char **blkcpy(char **, register char **);

char *globerr;
char *home;
extern int errno;

static int globcnt;

char *globchars = "`{[*?";

static char *gpath, *gpathp, *lastgpathp;
static int globbed;
static char *entp;
static char **sortbas;

#ifdef OTHER_PASSWD
#include "getpwnam.h"
extern char _path_passwd[];
#endif

char **ftpglob(register char *v, boolean_t check_ncargs)
{
    char agpath[BUFSIZ];
    char *vv[2];

    if (agargv == NULL) {
	size_t inc = GAVSIZ * sizeof (char *);

	agargv = (char **) malloc(inc);
	limits[GLOB_INDEX_MALLOC] += inc;
	if (agargv == NULL) {
	    fatal("Out of memory");
	}
	agargv_size = GAVSIZ;
    }
    fixpath(v);
    if (v[0] == '\0')
	v = ".";
    else if ((strlen(v) > 1) && (v[strlen(v) - 1] == '/'))
	v[strlen(v) - 1] = '\0';

    vv[0] = v;
    vv[1] = NULL;
    globerr = NULL;
    gflag = 0;
    rscan(vv, tglob);
    if (gflag == 0) {
	vv[0] = strspl(v, "");
	return (copyblk(vv));
    }

    globerr = NULL;
    gpath = agpath;
    gpathp = gpath;
    *gpathp = 0;
    lastgpathp = &gpath[sizeof agpath - 2];
    ginit(agargv);
    globcnt = 0;
    collect(v, check_ncargs);
    if (globcnt == 0 && (gflag & 1)) {
	blkfree(gargv), gargv = 0;
	return (0);
    }
    else
	return (gargv = copyblk(gargv));
}

static void ginit(char **agargv)
{

    agargv[0] = 0;
    gargv = agargv;
    sortbas = agargv;
    gargc = 0;
    gnleft = NCARGS - 4;
}

static void collect(register char *as, boolean_t check_ncargs)
{
    if (eq(as, "{") || eq(as, "{}")) {
	Gcat(as, "", check_ncargs);
	sort();
    }
    else
	acollect(as, check_ncargs);
}

static void acollect(register char *as, boolean_t check_ncargs)
{
    register int ogargc = gargc;

    gpathp = gpath;
    *gpathp = 0;
    globbed = 0;
    expand(as, check_ncargs);
    if (gargc != ogargc)
	sort();
}

static int
argcmp(const void *p1, const void *p2)
{
    char *s1 = *(char **) p1;
    char *s2 = *(char **) p2;

    return (strcmp(s1, s2));
}

static void sort(void)
{
    char **Gvp = &gargv[gargc];

    if (!globerr)
	qsort(sortbas, Gvp - sortbas, sizeof (*sortbas), argcmp);
    sortbas = Gvp;
}

static void expand(char *as, boolean_t check_ncargs)
{
    register char *cs;
    register char *sgpathp, *oldcs;
    struct stat stb;

    if (globerr)
	return;
    sgpathp = gpathp;
    cs = as;
    if (*cs == '~' && gpathp == gpath) {
	addpath('~');
	for (cs++; letter(*cs) || digit(*cs) || *cs == '-';)
	    addpath(*cs++);
	if (!*cs || *cs == '/') {
	    if (gpathp != gpath + 1) {
		*gpathp = 0;
		if (gethdir(gpath + 1))
		    globerr = "Unknown user name after ~";
		/* memmove used as strings overlap */
		(void) memmove(gpath, gpath + 1, strlen(gpath + 1) + 1);
	    }
	    else
		(void) strlcpy(gpath, home, BUFSIZ);
	    gpathp = strend(gpath);
	}
    }
    while (!any(*cs, globchars)) {
	if (*cs == 0) {
	    if (!globbed)
		Gcat(gpath, "", check_ncargs);
	    else if (stat(gpath, &stb) >= 0) {
		Gcat(gpath, "", check_ncargs);
		globcnt++;
	    }
	    if (limits[GLOB_INDEX_STAT]++ >= GLOB_LIMIT_STAT)
	      globerr = "Arguments too long";
	    goto endit;
	}
	addpath(*cs++);
    }
    oldcs = cs;
    while (cs > as && *cs != '/')
	cs--, gpathp--;
    if (*cs == '/')
	cs++, gpathp++;
    *gpathp = 0;
    if (*oldcs == '{') {
	(void) execbrc(cs, ((char *) 0));
	return;
    }
    matchdir(cs, check_ncargs);
  endit:
    gpathp = sgpathp;
    *gpathp = 0;
}

static void matchdir(char *pattern, boolean_t check_ncargs)
{
    struct stat stb;

#ifdef HAVE_DIRENT_H
    register struct dirent *dp;
#else
    register struct direct *dp;
#endif

    DIR *dirp;

    dirp = opendir(*gpath == '\0' ? "." : gpath);
    if (dirp == NULL) {
	if (globbed)
	    return;
	goto patherr2;
    }
#ifdef HAVE_DIRFD
    if (fstat(dirfd(dirp), &stb) < 0)
#else /* HAVE_DIRFD */
    if (fstat(dirp->dd_fd, &stb) < 0)
#endif /* HAVE_DIRFD */
	goto patherr1;
    if (limits[GLOB_INDEX_STAT]++ >= GLOB_LIMIT_STAT) {
      globerr = "Arguments too long";
      return;
    }
    if (!isdir(stb)) {
	errno = ENOTDIR;
	goto patherr1;
    }
    while (!globerr && ((dp = readdir(dirp)) != NULL)) {
	if (dp->d_ino == 0)
	    continue;
	if (limits[GLOB_INDEX_READDIR]++ >= GLOB_LIMIT_READDIR)
	  globerr = "Arguments too long";
	else if (match(dp->d_name, pattern, check_ncargs)) {
	    Gcat(gpath, dp->d_name, check_ncargs);
	    globcnt++;
	}
    }
    closedir(dirp);
    return;

  patherr1:
    closedir(dirp);
  patherr2:
    globerr = "Bad directory components";
}

static int execbrc(char *p, char *s)
{
    char restbuf[BUFSIZ + 2];
    char *restbufend = &restbuf[sizeof(restbuf)];
    register char *pe, *pm, *pl;
    int brclev = 0;
    char *lm, savec, *sgpathp;

    for (lm = restbuf; *p != '{'; *lm++ = *p++) {
	if (lm >= restbufend)
	    return (0);
    }
    for (pe = ++p; *pe; pe++) {
	switch (*pe) {

	case '{':
	    brclev++;
	    continue;

	case '}':
	    if (brclev == 0)
		goto pend;
	    brclev--;
	    continue;

	case '[':
	    for (pe++; *pe && *pe != ']'; pe++)
		continue;
	    if (!*pe) {
		globerr = "Missing ]";
		return (0);
	    }
	    continue;
	}
    }
  pend:
    if (brclev || !*pe) {
	globerr = "Missing }";
	return (0);
    }
    for (pl = pm = p; pm <= pe; pm++) {
	switch (*pm & (QUOTE | TRIM)) {

	case '{':
	    brclev++;
	    continue;

	case '}':
	    if (brclev) {
		brclev--;
		continue;
	    }
	    goto doit;

	case ',' | QUOTE:
	case ',':
	    if (brclev)
		continue;
	  doit:
	    savec = *pm;
	    *pm = 0;
	    if (lm + strlen(pl) + strlen(pe + 1) >= restbufend)
		return (0);
	    (void) strlcpy(lm, pl, restbufend - lm);
	    (void) strlcat(restbuf, pe + 1, sizeof(restbuf));
	    *pm = savec;
	    if (s == 0) {
		sgpathp = gpathp;
		expand(restbuf, B_TRUE);
		gpathp = sgpathp;
		*gpathp = 0;
	    }
	    else if (amatch(s, restbuf, B_TRUE))
		return (1);
	    sort();
	    pl = pm + 1;
	    continue;

	case '[':
	    for (pm++; *pm && *pm != ']'; pm++)
		continue;
	    if (!*pm) {
		globerr = "Missing ]";
		return (0);
	    }
	    continue;
	}
    }
    return (0);
}

static int match(char *s, char *p, boolean_t check_ncargs)
{
    register int c;
    register char *sentp;
    char sglobbed = globbed;

    if (*s == '.' && *p != '.')
	return (0);
    sentp = entp;
    entp = s;
    c = amatch(s, p, check_ncargs);
    entp = sentp;
    globbed = sglobbed;
    return (c);
}

static int amatch(char *s, char *p, boolean_t check_ncargs)
{
    register int scc;
    int ok, lc;
    char *sgpathp;
    struct stat stb;
    int c, cc;

    globbed = 1;
    for (;;) {
	scc = *s++ & TRIM;
	switch (c = *p++) {

	case '{':
	    return (execbrc(p - 1, s - 1));

	case '[':
	    ok = 0;
	    lc = 077777;
	    while ((cc = *p++)) {
		if (cc == ']') {
		    if (ok)
			break;
		    return (0);
		}
		if (cc == '-') {
		    if (lc <= scc && scc <= *p++)
			ok++;
		}
		else if (scc == (lc = cc))
		    ok++;
	    }
	    if (cc == 0) {
		globerr = "Missing ]";
		return (0);
	    }
	    continue;

	case '*':
	    if (!*p)
		return (1);
	    if (*p == '/') {
		p++;
		goto slash;
	    } else if (*p == '*') {
		s--;
		continue;
	    }
	    s--;
	    do {
		if (amatch(s, p, check_ncargs))
		    return (1);
	    } while (*s++);
	    return (0);

	case 0:
	    return (scc == 0);

	default:
	    if (c != scc)
		return (0);
	    continue;

	case '?':
	    if (scc == 0)
		return (0);
	    continue;

	case '/':
	    if (scc)
		return (0);
	  slash:
	    s = entp;
	    sgpathp = gpathp;
	    while (*s)
		addpath(*s++);
	    addpath('/');
	    if (stat(gpath, &stb) == 0 && isdir(stb))
		if (*p == 0) {
		    Gcat(gpath, "", check_ncargs);
		    globcnt++;
		}
		else
		    expand(p, check_ncargs);
	    if (limits[GLOB_INDEX_STAT]++ >= GLOB_LIMIT_STAT)
	      globerr = "Arguments too long";
	    gpathp = sgpathp;
	    *gpathp = 0;
	    return (0);
	}
    }
}

static void Gcat(register char *s1, register char *s2, boolean_t check_ncargs)
{
    register size_t len = strlen(s1) + strlen(s2) + 1;

    if (globerr)
	return;
    if ((check_ncargs) && ((len + sizeof (char *)) >= gnleft)) {
	globerr = "Arguments too long";
	return;
    }
    if (len > MAXPATHLEN) {
	globerr = "Pathname too long";
	return;
    }
    if (gargc >= agargv_size - 1) {
	char **tmp;
	size_t inc = GAVSIZ * sizeof (char *);

	tmp = (char **)realloc(agargv,
			       inc + (agargv_size * sizeof (char *)));
	limits[GLOB_INDEX_MALLOC] += inc;
	if ((tmp == NULL) ||
	    (limits[GLOB_INDEX_MALLOC] >= GLOB_LIMIT_MALLOC)) {
	    fatal("Out of memory");
	} else {
	    agargv = tmp;
	    agargv_size += GAVSIZ;
	}
	gargv = agargv;
	sortbas = agargv;
    }
    gargc++;
    if (check_ncargs)
	gnleft -= len + sizeof (char *);
    gargv[gargc] = 0;
    gargv[gargc - 1] = strspl(s1, s2);
}

static void addpath(char c)
{

    if (gpathp >= lastgpathp)
	globerr = "Pathname too long";
    else {
	*gpathp++ = c;
	*gpathp = 0;
    }
}

static void rscan(register char **t, int (*f) (register char))
{
    register char *p, c;

    while ((p = *t++)) {
	if (*p == '~')
	    gflag |= 2;
	else if (eq(p, "{") || eq(p, "{}"))
	    continue;
	while ((c = *p++))
	    (*f) (c);
    }
}
static int tglob(register char c)
{
    if (any(c, globchars))
	gflag |= c == '{' ? 2 : 1;
    return (c);
}

int letter(register char c)
{
    return (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z'))
	    || (c == '_'));
}

int digit(register char c)
{
    return (c >= '0' && c <= '9');
}

int any(register int c, register char *s)
{
    while (*s)
	if (*s++ == c)
	    return (1);
    return (0);
}

int blklen(register char **av)
{
    register int i = 0;

    while (*av++)
	i++;
    return (i);
}

char **blkcpy(char **oav, register char **bv)
{
    register char **av = oav;

    while ((*av++ = *bv++))
	continue;
    return (oav);
}

void blkfree(char **av0)
{
    register char **av = av0;

    if (av) {
	while (*av)
	    free(*av++);
    }
}

char *strspl(register char *cp, register char *dp)
{
    int bufsize = strlen(cp) + strlen(dp) + 1;
    char *ep = malloc(bufsize);

    limits[GLOB_INDEX_MALLOC] += bufsize;
    if ((ep == NULL) || (limits[GLOB_INDEX_MALLOC] >= GLOB_LIMIT_MALLOC))
	fatal("Out of memory");
    (void) strlcpy(ep, cp, bufsize);
    (void) strlcat(ep, dp, bufsize);
    return (ep);
}

char **copyblk(register char **v)
{
    size_t inc = (unsigned) ((blklen(v) + 1) * sizeof(char **));
    char **nv = (char **) malloc(inc);

    limits[GLOB_INDEX_MALLOC] += inc;
    if ((nv == NULL) || (limits[GLOB_INDEX_MALLOC] >= GLOB_LIMIT_MALLOC))
	fatal("Out of memory");

    return (blkcpy(nv, v));
}

static char *strend(register char *cp)
{
    while (*cp)
	cp++;
    return (cp);
}
/*
 * Extract a home directory from the password file
 * The argument points to a buffer where the name of the
 * user whose home directory is sought is currently.
 * We write the home directory of the user back there.
 */
static int gethdir(char *home)
{
#ifdef OTHER_PASSWD
    register struct passwd *pp = bero_getpwnam(home, _path_passwd);
#else
    register struct passwd *pp = getpwnam(home);
#endif
    register char *root = NULL;
    if (!pp || home + strlen(pp->pw_dir) >= lastgpathp)
	return (1);
    root = strstr(pp->pw_dir, "/./");
    (void) strlcpy(home, root ? (root + 2) : pp->pw_dir, lastgpathp - home);

    return (0);
}
