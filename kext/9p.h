#ifndef NINEP_H
#define NINEP_H

#include <stdint.h>

#define VFS9PNAME	"9p"
#define VERSION9P	"9P2000.L"

#define TAGLEN		64
#define VOLNAMELEN	256

/* mount(2) data; a local-volume fs gets it past the device path pointer */
typedef struct {
#ifndef KERNEL
	char *fspec;
#endif
	char tag[TAGLEN];
	char volume[VOLNAMELEN];
	uint32_t flags;
} args_9p;

enum {
	FLAG_CHATTY9P	= 1<<0,
	FLAG_VOLFS	= 1<<1,	/* file IDs for volfs (Carbon on 10.4) */
};

#ifdef KERNEL
#include <sys/types.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/lock.h>
#include <sys/kauth.h>
#include <sys/unistd.h>
#include <sys/dirent.h>
#include <sys/ubc.h>
#include <sys/malloc.h>
#include <sys/xattr.h>
#include <sys/vnode_if.h>
#include <kern/locks.h>
#include <libkern/OSAtomic.h>
#include <string.h>
#include "virtio9p.h"

#ifndef nelem
#define nelem(x)	(sizeof(x)/sizeof((x)[0]))
#endif

typedef uint32_t fid_9p;
#define NOFID		0xFFFFFFFFU
#define NOTAG		0xFFFF

typedef struct {
	uint8_t type;
	uint32_t vers;
	uint64_t path;
} qid_9p;

#define QTDIR		0x80
#define QTSYMLINK	0x02

/* Rgetattr, the fields we use */
typedef struct {
	qid_9p qid;
	uint32_t mode;
	uint32_t uid;
	uint32_t gid;
	uint64_t nlink;
	uint64_t rdev;
	uint64_t size;
	uint64_t blocks;
	struct timespec atime, mtime, ctime, btime;
} attr_9p;

typedef struct {
	qid_9p qid;
	uint8_t type;
	char *name;
} dirent_9p;

typedef struct {
	uint32_t bsize;
	uint64_t blocks, bfree, bavail, files, ffree;
} fsstat_9p;

/* Tsetattr valid bits */
enum {
	SA_MODE		= 0x001,
	SA_UID		= 0x002,
	SA_GID		= 0x004,
	SA_SIZE		= 0x008,
	SA_ATIME	= 0x010,
	SA_MTIME	= 0x020,
	SA_ATIME_SET	= 0x080,
	SA_MTIME_SET	= 0x100,
};

/* Linux open flags on the wire */
enum {
	L_O_RDONLY	= 0,
	L_O_WRONLY	= 1,
	L_O_RDWR	= 2,
	L_O_CREAT	= 0100,
	L_O_EXCL	= 0200,
	L_O_TRUNC	= 01000,
	L_O_APPEND	= 02000,
	L_O_DIRECTORY	= 0200000,
	L_AT_REMOVEDIR	= 0x200,
};

typedef struct node_9p node_9p;
typedef struct mount_9p mount_9p;

/* node flags */
enum {
	NODE_INIT	= 1<<0,
	NODE_WAITINIT	= 1<<1,
	NODE_RECL	= 1<<2,
	NODE_WAITRECL	= 1<<3,
	NODE_MMAPPED	= 1<<4,
	NODE_RSRC	= 1<<5,	/* resource fork of node "data" */
};

typedef enum {
	NODE_LCK_NONE,
	NODE_LCK_SHARED,
	NODE_LCK_EXCLUSIVE,
} lcktype_9p;

/* open fids by access mode */
enum {
	OFID_RD,
	OFID_WR,
	OFID_RDWR,
	NOFIDS
};

typedef struct {
	fid_9p fid;
	uint32_t ref;
} openfid_9p;

struct node_9p {
	LIST_ENTRY(node_9p) next;
	mount_9p *nmp;
	vnode_t vp;

	lck_rw_t *lck;
	lcktype_9p lcktype;
	uint32_t flags;
	uint32_t iounit;
	fid_9p fid;
	qid_9p qid;
	uint32_t id;
	attr_9p attr;
	uint64_t attrtime;
	openfid_9p openfid[NOFIDS];
	dirent_9p *dirents;
	uint32_t ndirents;
	struct lock_9p *locks;

	node_9p *rsrc;		/* data node: its fork vnode */
	node_9p *data;		/* fork node: its file */
	uint8_t *rbuf;		/* fork node: the fork while in use */
	uint64_t rlen, rcap;
	uint8_t rloaded, rdirty;
};

#define ISRSRC(np)	ISSET((np)->flags, NODE_RSRC)
#define XATTR_MAX	(16 << 20)

enum {
	F_UNMOUNTING	= 1<<8,
};

struct mount_9p {
	lck_mtx_t *lck;
	mount_t mp;
	vnode_t root;
	v9p_chan *chan;

	uid_t uid;
	gid_t gid;
	uint32_t flags;
	char volume[VOLNAMELEN];
	char tag[TAGLEN];

	uint32_t msize;
	uint32_t nfid;

	LIST_HEAD(hnode_9p, node_9p) *node;
	u_long nodelen;
	lck_mtx_t *nodelck;

	struct idtab_9p *ids;
};

/* proto.c */
__private_extern__ int version_9p(mount_9p*);
__private_extern__ int attach_9p(mount_9p*, fid_9p*, qid_9p*);
__private_extern__ int walk_9p(mount_9p*, fid_9p, char*, int, fid_9p*, qid_9p*);
__private_extern__ int getattr_9p(mount_9p*, fid_9p, attr_9p*);
__private_extern__ int setattr_9p(mount_9p*, fid_9p, uint32_t, uint32_t, uint64_t, struct timespec*, struct timespec*);
__private_extern__ int lopen_9p(mount_9p*, fid_9p, uint32_t, qid_9p*, uint32_t*);
__private_extern__ int lcreate_9p(mount_9p*, fid_9p, char*, int, uint32_t, uint32_t, qid_9p*, uint32_t*);
__private_extern__ int mkdir_9p(mount_9p*, fid_9p, char*, int, uint32_t, qid_9p*);
__private_extern__ int symlink_9p(mount_9p*, fid_9p, char*, int, char*, qid_9p*);
__private_extern__ int readlink_9p(mount_9p*, fid_9p, char**);
__private_extern__ int read_9p(mount_9p*, fid_9p, void*, uint32_t, uint64_t, uint32_t*);
__private_extern__ int write_9p(mount_9p*, fid_9p, void*, uint32_t, uint64_t, uint32_t*);
__private_extern__ int clunk_9p(mount_9p*, fid_9p);
__private_extern__ int unlinkat_9p(mount_9p*, fid_9p, char*, int, uint32_t);
__private_extern__ int renameat_9p(mount_9p*, fid_9p, char*, int, fid_9p, char*, int);
__private_extern__ int readdirs_9p(mount_9p*, fid_9p, dirent_9p**, uint32_t*);
__private_extern__ int statfs_9p(mount_9p*, fid_9p, fsstat_9p*);
__private_extern__ int fsync_9p(mount_9p*, fid_9p);
__private_extern__ int xattrwalk_9p(mount_9p*, fid_9p, const char*, fid_9p*, uint64_t*);
__private_extern__ int xattrcreate_9p(mount_9p*, fid_9p, const char*, uint64_t, uint32_t);
__private_extern__ uint32_t iosize_9p(mount_9p*);

/* vfsops.c */
__private_extern__ void* malloc_9p(size_t);
__private_extern__ void free_9p(void*);

/* vnops.c */
__private_extern__ int nget_9p(mount_9p*, fid_9p, qid_9p, vnode_t, vnode_t*, struct componentname*, vfs_context_t);
__private_extern__ int ngetattr_9p(node_9p*, int);
__private_extern__ void nlock_9p(node_9p*, lcktype_9p);
__private_extern__ void nunlock_9p(node_9p*);
__private_extern__ struct vnodeopv_desc vnodeopv_desc_9p;

__private_extern__ lck_grp_t *lck_grp_9p;

/* ids.c */
__private_extern__ int ids_init_9p(mount_9p*, uint64_t);
__private_extern__ void ids_fini_9p(mount_9p*);
__private_extern__ uint32_t id_get_9p(mount_9p*, uint64_t);
__private_extern__ void id_link_9p(mount_9p*, uint32_t, uint32_t, const char*, int);
__private_extern__ void id_forget_9p(mount_9p*, uint32_t);
__private_extern__ uint32_t id_parent_9p(mount_9p*, uint32_t);
__private_extern__ int id_name_9p(mount_9p*, uint32_t, char*, int);
__private_extern__ int id_vget_9p(mount_9p*, uint32_t, vnode_t*, vfs_context_t, int);

/* rsrc.c */
__private_extern__ void rsrc_init_9p(void);
__private_extern__ void rsrc_fini_9p(void);
__private_extern__ int forkspec_9p(struct componentname*, int*);
__private_extern__ int rsrc_vget_9p(node_9p*, vnode_t, vnode_t*);
__private_extern__ void rsrc_sync_9p(node_9p*, int);
__private_extern__ int rsrc_read_9p(node_9p*, uio_t);
__private_extern__ int rsrc_write_9p(node_9p*, uio_t, int);
__private_extern__ int rsrc_size_9p(node_9p*, uint64_t*);
__private_extern__ int rsrc_truncate_9p(node_9p*, uint64_t);
__private_extern__ int rsrc_flush_9p(node_9p*);
__private_extern__ void rsrc_reclaim_9p(node_9p*);

/* vnops.c, shared with rsrc.c */
__private_extern__ int xread_9p(mount_9p*, fid_9p, uint8_t*, uint64_t, uint32_t);
__private_extern__ int xset_9p(node_9p*, const char*, uint8_t*, uint64_t, uint32_t);
__private_extern__ int (**vnode_op_9p)(void *);

/* lock.c */
__private_extern__ void lock_init_9p(void);
__private_extern__ void lock_fini_9p(void);
__private_extern__ void lock_free_9p(node_9p*);
__private_extern__ int vnop_advlock_9p(struct vnop_advlock_args*);

/* virtio9p.cpp */
void v9p_transport_init(void);
void v9p_transport_fini(void);

#define MTO9P(m)	((mount_9p*)vfs_fsprivate(m))
#define NTO9P(vp)	((node_9p*)vnode_fsnode(vp))

#ifdef DEBUG9P
#define DEBUG(f, a...)	printf("9p: %s: " f "\n", __FUNCTION__, ## a)
#else
#define DEBUG(f, a...)
#endif
#endif /* KERNEL */
#endif
