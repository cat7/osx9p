/*
 * mount_9p [-o options] tag dir
 * Mounts the virtio-9p share with mount tag "tag" on dir.
 * Options: volume=NAME (name shown in the Finder), rdonly, nobrowse,
 * noowners (every user owns every file), chatty (log 9P traffic to the
 * system log).
 */
#include <sys/param.h>
#include <sys/mount.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>
#include "../kext/9p.h"

static void
usage(void)
{
	fprintf(stderr, "usage: mount_9p [-o volume=NAME,rdonly,nobrowse,noowners,chatty] tag dir\n");
	exit(EX_USAGE);
}

static void
options(char *s, args_9p *a, int *mntflags)
{
	char *o;

	while ((o = strsep(&s, ",")) != NULL) {
		if (*o == 0)
			continue;
		if (strncmp(o, "volume=", 7) == 0)
			strlcpy(a->volume, o + 7, sizeof(a->volume));
		else if (strcmp(o, "rdonly") == 0 || strcmp(o, "ro") == 0)
			*mntflags |= MNT_RDONLY;
		else if (strcmp(o, "nobrowse") == 0)
			*mntflags |= MNT_DONTBROWSE;
		else if (strcmp(o, "chatty") == 0)
			a->flags |= FLAG_CHATTY9P;
		else if (strcmp(o, "noowners") == 0)
			*mntflags |= MNT_IGNORE_OWNERSHIP;
		else if (strcmp(o, "rw") == 0 || strcmp(o, "nodev") == 0 ||
			 strcmp(o, "nosuid") == 0)
			continue;
		else
			errx(EX_USAGE, "unknown option %s", o);
	}
}

int
main(int argc, char **argv)
{
	char dir[MAXPATHLEN];
	args_9p a;
	int c, mntflags;

	memset(&a, 0, sizeof(a));
	mntflags = MNT_NODEV | MNT_NOSUID;
	while ((c = getopt(argc, argv, "o:")) != -1) {
		switch (c) {
		case 'o':
			options(optarg, &a, &mntflags);
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc != 2)
		usage();
	if (strlen(argv[0]) >= sizeof(a.tag))
		errx(EX_USAGE, "tag too long");
	strlcpy(a.tag, argv[0], sizeof(a.tag));
	if (realpath(argv[1], dir) == NULL)
		err(EX_USAGE, "%s", argv[1]);
	if (mount(VFS9PNAME, dir, mntflags, &a) < 0)
		err(EX_OSERR, "%s on %s", a.tag, dir);
	return 0;
}
