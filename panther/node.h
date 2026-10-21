/* 10.3 nodes: vnode locks are the filesystem's (lockmgr), all under the kernel funnel */

enum {
	NODE_INIT	= 1<<0,
	NODE_WAITINIT	= 1<<1,
	NODE_HASHED	= 1<<2,
	NODE_GONE	= 1<<3,	/* removed: recycle on inactive */
	NODE_MMAPPED	= 1<<4,
	NODE_BUSY	= 1<<5,	/* open fid table in use */
	NODE_WANT	= 1<<6,
	NODE_AD		= 1<<7,	/* "._name": AppleDouble view of name's attributes */
};

#define ISAD(np)	ISSET((np)->flags, NODE_AD)

/* nget_9p flags */
enum {
	NGET_ROOT	= 1<<0,
	NGET_AD		= 1<<1,
};

#define XATTR_FINFO9P	"com.apple.FinderInfo"
#define XATTR_RSRC9P	"com.apple.ResourceFork"
#define XATTR_MAX9P	(16 << 20)

struct node_9p {
	LIST_ENTRY(node_9p) next;
	mount_9p *nmp;
	struct vnode *vp;
	struct lock__bsd__ lock;

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

	uint8_t *adbuf;		/* AppleDouble image while in use */
	uint32_t adlen, adcap;
	uint8_t adloaded, addirty;
};

struct mount_9p {
	struct mount *mp;
	struct vnode *root;
	v9p_chan *chan;

	uid_t uid;
	gid_t gid;
	uint32_t flags;
	char volume[VOLNAMELEN];
	char tag[TAGLEN];

	uint32_t msize;
	uint32_t nfid;
	uint64_t rootqpath;

	LIST_HEAD(hnode_9p, node_9p) *node;
	u_long nodelen;

	struct idtab_9p *ids;

	/* files renamed lately, whose "._" Carbon deletes after the rename */
	struct {
		uint64_t qpath, when;
	} renamed[8];
	uint32_t nrenamed;
};

/* vnops.c */
__private_extern__ int nget_9p(mount_9p*, fid_9p, qid_9p, int, struct vnode**, struct proc*);
__private_extern__ int ngetattr_9p(node_9p*, int);
__private_extern__ struct vnodeopv_desc vnodeopv_desc_9p;
__private_extern__ int (**vnode_op_9p)(void *);

/* adouble.c */
__private_extern__ int adname_9p(const char*, int);
__private_extern__ int xget_9p(mount_9p*, fid_9p, const char*, void*, uint32_t, uint64_t*);
__private_extern__ int xput_9p(mount_9p*, fid_9p, const char*, const void*, uint32_t);
__private_extern__ int adget_9p(node_9p*, char*, int, int, struct vnode**, struct proc*);
__private_extern__ int adflush_9p(node_9p*);
__private_extern__ int adsize_9p(node_9p*, uint64_t*);
__private_extern__ int adread_9p(node_9p*, struct uio*);
__private_extern__ int adwrite_9p(node_9p*, struct uio*, int);
__private_extern__ int adtruncate_9p(node_9p*, uint64_t);
__private_extern__ int adremove_9p(node_9p*);
__private_extern__ void adfree_9p(node_9p*);
__private_extern__ int admove_9p(node_9p*, fid_9p);
__private_extern__ uint64_t adbase_9p(node_9p*);

/* ids.c */
__private_extern__ int ids_init_9p(mount_9p*, uint64_t);
__private_extern__ void ids_fini_9p(mount_9p*);
__private_extern__ uint32_t id_get_9p(mount_9p*, uint64_t);
__private_extern__ void id_link_9p(mount_9p*, uint32_t, uint32_t, const char*, int);
__private_extern__ void id_forget_9p(mount_9p*, uint32_t);
__private_extern__ uint32_t id_parent_9p(mount_9p*, uint32_t);
__private_extern__ int id_name_9p(mount_9p*, uint32_t, char*, int);
__private_extern__ int id_vget_9p(mount_9p*, uint32_t, struct vnode**, struct proc*, int);

/* lock.c */
__private_extern__ void lock_init_9p(void);
__private_extern__ void lock_fini_9p(void);
__private_extern__ void lock_free_9p(node_9p*);
__private_extern__ int vnop_advlock_9p(struct vop_advlock_args*);

#define MTO9P(m)	((mount_9p*)(m)->mnt_data)
#define NTO9P(vp)	((node_9p*)(vp)->v_data)
