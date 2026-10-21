/*
 * mount_9p [-o options] tag dir
 * Mounts the virtio-9p share with mount tag "tag" on dir.
 * Options: volume=NAME (name shown in the Finder), rdonly, nobrowse,
 * owners (show host ownership; by default every user owns every file),
 * chatty (log 9P traffic to the system log), volfs/novolfs (file IDs for
 * Carbon; on by default on 10.4, where Carbon needs them).
 */
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <AvailabilityMacros.h>
#include <CoreFoundation/CoreFoundation.h>
#if MAC_OS_X_VERSION_MAX_ALLOWED < 1040
/* the 10.3 SDK: DiskArbitration is private there */
#define kDADiskOptionMountAutomaticNoDefer	0x00000020
#define DLOPEN_NO_WARN
#else
#include <DiskArbitration/DiskArbitration.h>
#endif
#include <dlfcn.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>
#include "../kext/9p.h"

#ifndef MNT_IGNORE_OWNERSHIP
#define MNT_IGNORE_OWNERSHIP	MNT_UNKNOWNPERMISSIONS
#endif

static void
usage(void)
{
	fprintf(stderr, "usage: mount_9p [-o volume=NAME,rdonly,nobrowse,owners,[no]volfs,chatty] tag dir\n");
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
		else if (strcmp(o, "volfs") == 0)
			a->flags |= FLAG_VOLFS;
		else if (strcmp(o, "novolfs") == 0)
			a->flags &= ~FLAG_VOLFS;
		else if (strcmp(o, "noowners") == 0)
			*mntflags |= MNT_IGNORE_OWNERSHIP;
		else if (strcmp(o, "owners") == 0)
			*mntflags &= ~MNT_IGNORE_OWNERSHIP;
		else if (strcmp(o, "rw") == 0 || strcmp(o, "nodev") == 0 ||
			 strcmp(o, "nosuid") == 0)
			continue;
		else
			errx(EX_USAGE, "unknown option %s", o);
	}
}

/* Darwin major version: 7 is Mac OS X 10.3, 8 is 10.4 */
static int
darwin(void)
{
	struct utsname u;

	if (uname(&u) < 0)
		return 0;
	return atoi(u.release);
}

/*
 * diskarbitrationd on 10.3 and 10.4 knows a volume without a device only after a
 * refresh; Carbon ejects every volume through it.  Without NoDefer it
 * unmounts the volume when no one is logged in, as at startup.
 * DiskManagement takes a local volume there for a device and hangs, so
 * the volume is mounted non-local, refreshed, then updated to local.
 */
static void
darefresh(const char *dir)
{
	void *(*sessioncreate)(CFAllocatorRef);
	void *(*fromvolumepath)(CFAllocatorRef, void *, CFURLRef);
	int (*refresh)(void *);
	int (*setoptions)(void *, UInt32, Boolean);
	void *h, *session, *disk;
	CFURLRef url;

	h = dlopen("/System/Library/Frameworks/DiskArbitration.framework/DiskArbitration", RTLD_LAZY);
	if (h == NULL)	/* 10.3 */
		h = dlopen("/System/Library/PrivateFrameworks/DiskArbitration.framework/DiskArbitration", RTLD_LAZY);
	if (h == NULL)
		return;
	sessioncreate = dlsym(h, "DASessionCreate");
	fromvolumepath = dlsym(h, "DADiskCreateFromVolumePath");
	refresh = dlsym(h, "_DADiskRefresh");
	setoptions = dlsym(h, "DADiskSetOptions");
	if (sessioncreate == NULL || fromvolumepath == NULL || refresh == NULL ||
	    setoptions == NULL)
		return;
	session = sessioncreate(kCFAllocatorDefault);
	if (session == NULL)
		return;
	url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
	    (const UInt8 *)dir, strlen(dir), true);
	if (url != NULL) {
		disk = fromvolumepath(kCFAllocatorDefault, session, url);
		if (disk != NULL) {
			if (refresh(disk) == 0)
				setoptions(disk, kDADiskOptionMountAutomaticNoDefer, true);
			CFRelease(disk);
		}
		CFRelease(url);
	}
	CFRelease(session);
}

/* someone has logged in at the console */
static int
loggedin(void)
{
	struct stat st;

	return stat("/dev/console", &st) == 0 && st.st_uid != 0;
}

int
main(int argc, char **argv)
{
	char dir[MAXPATHLEN];
	args_9p a;
	int c, da, mntflags;

	memset(&a, 0, sizeof(a));
	mntflags = MNT_NODEV | MNT_NOSUID | MNT_IGNORE_OWNERSHIP;
	switch (darwin()) {
	case 7:
		/*
		 * Carbon on 10.3 stays with AppleDouble, and the Finder lists a
		 * volfs volume only until it relaunches.  diskarbitrationd there
		 * unmounts a refreshed volume while no one is logged in; the
		 * Finder finds the volume when it starts anyway.
		 */
		da = loggedin();
		if (da)
			a.flags |= FLAG_NOTLOCAL;
		break;
	case 8:
		da = 1;
		a.flags |= FLAG_VOLFS | FLAG_NOTLOCAL;
		break;
	default:
		da = 0;
		break;
	}
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
	if (da) {
		darefresh(dir);
		a.flags &= ~FLAG_NOTLOCAL;
		if (mount(VFS9PNAME, dir, mntflags | MNT_UPDATE, &a) < 0)
			warn("%s on %s", a.tag, dir);
	}
	return 0;
}
