/* Mac OS X 10.3 kernel headers (pre-KPI) */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/ucred.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/namei.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/lock.h>
#include <sys/lockf.h>
#include <sys/uio.h>
#include <sys/unistd.h>
#include <sys/dirent.h>
#include <sys/ubc.h>
#include <sys/malloc.h>
#include <sys/errno.h>
#include <sys/vnode_if.h>
#include <libkern/OSAtomic.h>
#include <string.h>

#ifndef EPROTO
#define EPROTO		EIO
#endif
