#include "9p.h"
#include <sys/uio.h>
#include <sys/vnode_if.h>

typedef int vnop_t(void *);
__private_extern__ vnop_t **vnode_op_9p = NULL;
static char fsname[MFSNAMELEN] = VFS9PNAME;

enum {
	ATTR_TIMEOUT_US	= 1000000,	/* host files change behind our back */
	L_XATTR_CREATE	= 1,
	L_XATTR_REPLACE	= 2,
};

__private_extern__ void
nlock_9p(node_9p *np, lcktype_9p type)
{
	if (type == NODE_LCK_SHARED)
		lck_rw_lock_shared(np->lck);
	else
		lck_rw_lock_exclusive(np->lck);
	np->lcktype = type;
}

__private_extern__ void
nunlock_9p(node_9p *np)
{
	switch (np->lcktype) {
	case NODE_LCK_SHARED:
		lck_rw_unlock_shared(np->lck);
		break;
	case NODE_LCK_EXCLUSIVE:
		np->lcktype = NODE_LCK_NONE;
		lck_rw_unlock_exclusive(np->lck);
		break;
	case NODE_LCK_NONE:
		break;
	}
}

static uint64_t
now_us(void)
{
	struct timeval tv;

	microuptime(&tv);
	return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* refresh np->attr; caller holds the node lock exclusive */
__private_extern__ int
ngetattr_9p(node_9p *np, int force)
{
	attr_9p a;
	int e;

	if (!force && np->attrtime && now_us() - np->attrtime < ATTR_TIMEOUT_US)
		return 0;
	if ((e = getattr_9p(np->nmp, np->fid, &a)))
		return e;
	/* dirty pages past the host's EOF are not pushed yet */
	if (np->vp && vnode_isreg(np->vp) &&
	    (np->openfid[OFID_WR].fid != NOFID || np->openfid[OFID_RDWR].fid != NOFID) &&
	    (off_t)a.size < ubc_getsize(np->vp))
		a.size = ubc_getsize(np->vp);
	np->attr = a;
	np->attrtime = now_us();
	return 0;
}

static enum vtype
vtype_9p(uint32_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFDIR:	return VDIR;
	case S_IFLNK:	return VLNK;
	case S_IFIFO:	return VFIFO;
	case S_IFSOCK:	return VSOCK;
	case S_IFCHR:	return VCHR;
	case S_IFBLK:	return VBLK;
	default:	return VREG;
	}
}

#define	HASH9P(nmp, k)	(&(nmp)->node[(k) & (nmp)->nodelen])

/*
 * Find or make the vnode for qid. On success the node is returned locked
 * exclusive with an iocount; fid is consumed only when a new node is made.
 */
__private_extern__ int
nget_9p(mount_9p *nmp, fid_9p fid, qid_9p qid, vnode_t dvp, vnode_t *vpp,
	struct componentname *cnp, vfs_context_t ctx)
{
	struct vnode_fsparam fsp;
	struct hnode_9p *nhp;
	node_9p *np;
	uint32_t vid;
	int e, i;

	(void)ctx;
	*vpp = NULL;
	nhp = HASH9P(nmp, qid.path);
loop:
	lck_mtx_lock(nmp->nodelck);
	LIST_FOREACH(np, nhp, next) {
		if (np->qid.path != qid.path)
			continue;
		if (ISSET(np->flags, NODE_INIT)) {
			SET(np->flags, NODE_WAITINIT);
			msleep(np, nmp->nodelck, PINOD|PDROP, "nget_9p_init", NULL);
			goto loop;
		}
		if (ISSET(np->flags, NODE_RECL)) {
			SET(np->flags, NODE_WAITRECL);
			msleep(np, nmp->nodelck, PINOD|PDROP, "nget_9p_reclaim", NULL);
			goto loop;
		}
		vid = vnode_vid(np->vp);
		lck_mtx_unlock(nmp->nodelck);
		if (vnode_getwithvid(np->vp, vid))
			goto loop;
		nlock_9p(np, NODE_LCK_EXCLUSIVE);
		*vpp = np->vp;
		return 0;
	}

	if (fid == NOFID) {
		lck_mtx_unlock(nmp->nodelck);
		return ENOENT;
	}

	np = malloc_9p(sizeof(*np));
	if (np == NULL) {
		lck_mtx_unlock(nmp->nodelck);
		return ENOMEM;
	}
	np->lck = lck_rw_alloc_init(lck_grp_9p, LCK_ATTR_NULL);
	if (np->lck == NULL) {
		lck_mtx_unlock(nmp->nodelck);
		free_9p(np);
		return ENOMEM;
	}
	np->nmp = nmp;
	np->fid = fid;
	np->qid = qid;
	np->id = id_get_9p(nmp, qid.path);
	for (i = 0; i < NOFIDS; i++)
		np->openfid[i].fid = NOFID;
	SET(np->flags, NODE_INIT);
	LIST_INSERT_HEAD(nhp, np, next);
	nlock_9p(np, NODE_LCK_EXCLUSIVE);
	lck_mtx_unlock(nmp->nodelck);

	if ((e = ngetattr_9p(np, 1)))
		goto fail;

	bzero(&fsp, sizeof(fsp));
	fsp.vnfs_mp = nmp->mp;
	fsp.vnfs_vtype = vtype_9p(np->attr.mode);
	fsp.vnfs_str = fsname;
	fsp.vnfs_dvp = dvp;
	fsp.vnfs_fsnode = np;
	fsp.vnfs_vops = vnode_op_9p;
	fsp.vnfs_markroot = nmp->root == NULL;
	fsp.vnfs_marksystem = 0;
	fsp.vnfs_rdev = 0;
	fsp.vnfs_filesize = np->attr.size;
	fsp.vnfs_cnp = cnp;
	fsp.vnfs_flags = VNFS_NOCACHE;

	if ((e = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &fsp, &np->vp)))
		goto fail;
	vnode_addfsref(np->vp);
	vnode_settag(np->vp, VT_OTHER);

	lck_mtx_lock(nmp->nodelck);
	CLR(np->flags, NODE_INIT);
	if (ISSET(np->flags, NODE_WAITINIT)) {
		CLR(np->flags, NODE_WAITINIT);
		wakeup(np);
	}
	lck_mtx_unlock(nmp->nodelck);
	*vpp = np->vp;
	return 0;

fail:
	nunlock_9p(np);
	lck_mtx_lock(nmp->nodelck);
	LIST_REMOVE(np, next);
	CLR(np->flags, NODE_INIT);
	if (ISSET(np->flags, NODE_WAITINIT)) {
		CLR(np->flags, NODE_WAITINIT);
		wakeup(np);
	}
	lck_mtx_unlock(nmp->nodelck);
	lck_rw_free(np->lck, lck_grp_9p);
	free_9p(np);
	return e;
}
#undef HASH9P

/* walk to dir/name and return its vnode, unlocked, with an iocount */
static int
nwalkget_9p(node_9p *dnp, char *name, int nname, vnode_t dvp, vnode_t *vpp,
	    struct componentname *cnp, vfs_context_t ctx)
{
	mount_9p *nmp;
	fid_9p fid, ofid;
	qid_9p qid;
	int e;

	nmp = dnp->nmp;
	if ((e = walk_9p(nmp, dnp->fid, name, nname, &fid, &qid)))
		return e;
	e = nget_9p(nmp, fid, qid, dvp, vpp, cnp, ctx);
	if (e == 0 && NTO9P(*vpp)->fid != fid) {
		/* renamed on the host, the old fid names nothing: keep the new one */
		ofid = NTO9P(*vpp)->fid;
		NTO9P(*vpp)->fid = fid;
		NTO9P(*vpp)->attrtime = 0;
		fid = ofid;
	}
	if (e || NTO9P(*vpp)->fid != fid)
		clunk_9p(nmp, fid);
	if (e == 0) {
		if (!(nname == 2 && name[0] == '.' && name[1] == '.'))
			id_link_9p(nmp, NTO9P(*vpp)->id, dnp->id, name, nname);
		nunlock_9p(NTO9P(*vpp));
	}
	return e;
}

static int
vnop_lookup_9p(struct vnop_lookup_args *ap)
{
	struct componentname *cnp;
	vnode_t dvp, *vpp;
	node_9p *dnp;
	int e;

	dvp = ap->a_dvp;
	vpp = ap->a_vpp;
	cnp = ap->a_cnp;
	dnp = NTO9P(dvp);
	*vpp = NULL;

	if (!vnode_isdir(dvp))
		return ENOTDIR;
	if (ISSET(cnp->cn_flags, ISLASTCN) && cnp->cn_nameiop != LOOKUP &&
	    vnode_vfsisrdonly(dvp))
		return EROFS;

	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		if ((e = vnode_get(dvp)))
			return e;
		*vpp = dvp;
		return 0;
	}
	if (ISSET(cnp->cn_flags, ISDOTDOT)) {
		if (vnode_isvroot(dvp))
			return ENOENT;
		nlock_9p(dnp, NODE_LCK_SHARED);
		e = nwalkget_9p(dnp, "..", 2, NULL, vpp, NULL, ap->a_context);
		nunlock_9p(dnp);
		return e;
	}

	nlock_9p(dnp, NODE_LCK_SHARED);
	e = nwalkget_9p(dnp, cnp->cn_nameptr, cnp->cn_namelen, dvp, vpp, cnp, ap->a_context);
	nunlock_9p(dnp);
	if (e == 0 && !ISSET(cnp->cn_flags, ISLASTCN) && vnode_isreg(*vpp)) {
		vnode_t rvp;
		int rsrc, n;

		/* "name/..namedfork/rsrc": 10.4 leaves fork paths to us */
		n = forkspec_9p(cnp, &rsrc);
		if (n) {
			if (cnp->cn_nameiop != LOOKUP && cnp->cn_nameiop != CREATE) {
				vnode_put(*vpp);
				*vpp = NULL;
				return EPERM;
			}
			cnp->cn_consume = n;
			if (rsrc) {
				e = rsrc_vget_9p(NTO9P(*vpp), dvp, &rvp);
				vnode_put(*vpp);
				*vpp = e ? NULL : rvp;
			}
		}
	}
	if (e == ENOENT && ISSET(cnp->cn_flags, ISLASTCN) &&
	    (cnp->cn_nameiop == CREATE || cnp->cn_nameiop == RENAME))
		e = EJUSTRETURN;
	return e;
}

static int
ncreate_9p(vnode_t dvp, vnode_t *vpp, struct componentname *cnp,
	   struct vnode_attr *vap, vfs_context_t ctx, char *target)
{
	mount_9p *nmp;
	node_9p *dnp, *np;
	uint32_t mode, flags, iounit;
	fid_9p ofid;
	qid_9p qid;
	int e;

	dnp = NTO9P(dvp);
	nmp = dnp->nmp;
	*vpp = NULL;
	ofid = NOFID;

	if (vnode_vfsisrdonly(dvp))
		return EROFS;

	mode = vap->va_mode & 07777;
	nlock_9p(dnp, NODE_LCK_EXCLUSIVE);
	switch (vap->va_type) {
	case VREG:
		if ((e = walk_9p(nmp, dnp->fid, NULL, 0, &ofid, &qid)))
			goto out;
		flags = L_O_RDWR | L_O_CREAT;
		if (ISSET(vap->va_vaflags, VA_EXCLUSIVE))
			flags |= L_O_EXCL;
		if ((e = lcreate_9p(nmp, ofid, cnp->cn_nameptr, cnp->cn_namelen,
				    flags, mode | S_IFREG, &qid, &iounit))) {
			clunk_9p(nmp, ofid);
			ofid = NOFID;
			goto out;
		}
		break;
	case VDIR:
		e = mkdir_9p(nmp, dnp->fid, cnp->cn_nameptr, cnp->cn_namelen,
			     mode | S_IFDIR, &qid);
		break;
	case VLNK:
		e = symlink_9p(nmp, dnp->fid, cnp->cn_nameptr, cnp->cn_namelen,
			       target, &qid);
		break;
	default:
		e = ENOTSUP;
		break;
	}
	if (e)
		goto out;

	dnp->attrtime = 0;
	e = nwalkget_9p(dnp, cnp->cn_nameptr, cnp->cn_namelen, dvp, vpp, cnp, ctx);
	if (e == 0 && ofid != NOFID) {
		/* keep the creating handle: it may write a file made read-only */
		np = NTO9P(*vpp);
		nlock_9p(np, NODE_LCK_EXCLUSIVE);
		if (np->openfid[OFID_RDWR].fid == NOFID) {
			np->openfid[OFID_RDWR].fid = ofid;
			np->iounit = iounit;
			ofid = NOFID;
		}
		nunlock_9p(np);
	}
out:
	if (ofid != NOFID)
		clunk_9p(nmp, ofid);
	nunlock_9p(dnp);
	return e;
}

static int
vnop_create_9p(struct vnop_create_args *ap)
{
	return ncreate_9p(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, ap->a_context, NULL);
}

static int
vnop_mkdir_9p(struct vnop_mkdir_args *ap)
{
	return ncreate_9p(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, ap->a_context, NULL);
}

static int
vnop_symlink_9p(struct vnop_symlink_args *ap)
{
	return ncreate_9p(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, ap->a_context, ap->a_target);
}

static int
ofidslot(int fflag)
{
	switch (fflag & (FREAD|FWRITE)) {
	case FREAD|FWRITE:
		return OFID_RDWR;
	case FWRITE:
		return OFID_WR;
	default:
		return OFID_RD;
	}
}

/*
 * An open fid for I/O. Falls back to a temporary one (*tmp set; caller
 * clunks it) for paging after the last close.
 */
static int
iofid_9p(node_9p *np, int write, fid_9p *fidp, int *tmp)
{
	fid_9p fid;
	qid_9p qid;
	uint32_t iounit;
	int e, slot;

	*tmp = 0;
	slot = write ? OFID_WR : OFID_RD;
	if (np->openfid[slot].fid != NOFID) {
		*fidp = np->openfid[slot].fid;
		return 0;
	}
	if (np->openfid[OFID_RDWR].fid != NOFID) {
		*fidp = np->openfid[OFID_RDWR].fid;
		return 0;
	}
	if ((e = walk_9p(np->nmp, np->fid, NULL, 0, &fid, &qid)))
		return e;
	if ((e = lopen_9p(np->nmp, fid, write ? L_O_WRONLY : L_O_RDONLY, &qid, &iounit))) {
		clunk_9p(np->nmp, fid);
		return e;
	}
	*fidp = fid;
	*tmp = 1;
	return 0;
}

static int
vnop_open_9p(struct vnop_open_args *ap)
{
	openfid_9p *op;
	node_9p *np;
	vnode_t vp;
	fid_9p fid;
	qid_9p qid;
	uint32_t iounit, flags;
	uint64_t osize;
	struct timespec omtime;
	int e, slot;

	vp = ap->a_vp;
	np = NTO9P(vp);
	if (ISRSRC(np))
		return 0;
	slot = ofidslot(ap->a_mode);
	switch (slot) {
	case OFID_RDWR:	flags = L_O_RDWR; break;
	case OFID_WR:	flags = L_O_WRONLY; break;
	default:	flags = L_O_RDONLY; break;
	}

	nlock_9p(np, NODE_LCK_EXCLUSIVE);
	op = &np->openfid[slot];
	if (op->fid == NOFID && slot != OFID_RDWR &&
	    np->openfid[OFID_RDWR].fid != NOFID && np->openfid[OFID_RDWR].ref == 0) {
		/* the handle vnop_create kept */
		op = &np->openfid[OFID_RDWR];
	}
	if (op->fid == NOFID) {
		if ((e = walk_9p(np->nmp, np->fid, NULL, 0, &fid, &qid)))
			goto out;
		if ((e = lopen_9p(np->nmp, fid, flags, &qid, &iounit))) {
			clunk_9p(np->nmp, fid);
			goto out;
		}
		np->iounit = iounit;
		op->fid = fid;
	}
	op->ref++;

	/* drop cached pages if the host changed the file */
	if (vnode_isreg(vp)) {
		/* our own dirty pages first, so the host copy is current */
		ubc_sync_range(vp, 0, ubc_getsize(vp), UBC_PUSHDIRTY|UBC_SYNC);
		osize = np->attr.size;
		omtime = np->attr.mtime;
		if (ngetattr_9p(np, 1) == 0 &&
		    (np->attr.size != osize || np->attr.mtime.tv_sec != omtime.tv_sec ||
		     np->attr.mtime.tv_nsec != omtime.tv_nsec)) {
			ubc_sync_range(vp, 0, ubc_getsize(vp), UBC_INVALIDATE);
			ubc_setsize(vp, np->attr.size);
		}
	}
	e = 0;
out:
	nunlock_9p(np);
	return e;
}

static int
vnop_close_9p(struct vnop_close_args *ap)
{
	openfid_9p *op;
	node_9p *np;
	vnode_t vp;
	int i, slot;

	vp = ap->a_vp;
	np = NTO9P(vp);
	if (ISRSRC(np))
		return rsrc_flush_9p(np);
	slot = ofidslot(ap->a_fflag);

	nlock_9p(np, NODE_LCK_EXCLUSIVE);
	op = &np->openfid[slot];
	if (op->ref == 0 && np->openfid[OFID_RDWR].ref)
		op = &np->openfid[OFID_RDWR];
	if (op->ref == 0) {
		nunlock_9p(np);
		return 0;
	}
	if (--op->ref == 0) {
		for (i = 0; i < NOFIDS; i++)
			if (np->openfid[i].ref)
				break;
		if ((i == NOFIDS || op != &np->openfid[OFID_RD]) && vnode_isreg(vp)) {
			/* write back while the handle is still open */
			nunlock_9p(np);
			ubc_sync_range(vp, 0, ubc_getsize(vp), UBC_PUSHDIRTY|UBC_SYNC);
			nlock_9p(np, NODE_LCK_EXCLUSIVE);
		}
		if (op->ref == 0 && op->fid != NOFID) {
			clunk_9p(np->nmp, op->fid);
			op->fid = NOFID;
		}
	}
	nunlock_9p(np);
	return 0;
}

static int
vnop_getattr_9p(struct vnop_getattr_args *ap)
{
	struct vnode_attr *vap;
	node_9p *np;
	mount_9p *nmp;
	static const struct timespec nobackup;
	uint64_t rsize;
	int e;

	np = NTO9P(ap->a_vp);
	nmp = np->nmp;
	vap = ap->a_vap;
	rsize = 0;
	if (ISRSRC(np) && (e = rsrc_size_9p(np, &rsize)))
		return e;

	nlock_9p(np, NODE_LCK_EXCLUSIVE);
	if ((e = ngetattr_9p(np, 0)))
		goto out;
	VATTR_RETURN(vap, va_type, vtype_9p(np->attr.mode));
	VATTR_RETURN(vap, va_mode, np->attr.mode & 07777);
	VATTR_RETURN(vap, va_nlink, np->attr.nlink);
	VATTR_RETURN(vap, va_uid, nmp->uid);
	VATTR_RETURN(vap, va_gid, nmp->gid);
	VATTR_RETURN(vap, va_rdev, 0);
	if (ISRSRC(np)) {
		VATTR_RETURN(vap, va_data_size, rsize);
		VATTR_RETURN(vap, va_total_size, rsize);
		VATTR_RETURN(vap, va_data_alloc, rsize);
		VATTR_RETURN(vap, va_total_alloc, rsize);
	} else {
		VATTR_RETURN(vap, va_data_size, np->attr.size);
		VATTR_RETURN(vap, va_total_size, np->attr.size);
		VATTR_RETURN(vap, va_data_alloc, np->attr.blocks * 512);
		VATTR_RETURN(vap, va_total_alloc, np->attr.blocks * 512);
	}
	VATTR_RETURN(vap, va_iosize, iosize_9p(nmp));
	VATTR_RETURN(vap, va_flags, 0);
	VATTR_RETURN(vap, va_access_time, np->attr.atime);
	VATTR_RETURN(vap, va_modify_time, np->attr.mtime);
	VATTR_RETURN(vap, va_change_time, np->attr.ctime);
	if (np->attr.btime.tv_sec)
		VATTR_RETURN(vap, va_create_time, np->attr.btime);
	else
		VATTR_RETURN(vap, va_create_time, np->attr.mtime);
	VATTR_RETURN(vap, va_backup_time, nobackup);
	VATTR_RETURN(vap, va_fileid, np->id);
	VATTR_RETURN(vap, va_linkid, np->id);
	VATTR_RETURN(vap, va_parentid, id_parent_9p(nmp, np->id));
	/* the host may have renamed it since the vnode got its name */
	if (VATTR_IS_ACTIVE(vap, va_name) && vap->va_name &&
	    id_name_9p(nmp, np->id, vap->va_name, MAXPATHLEN) == 0)
		VATTR_SET_SUPPORTED(vap, va_name);
	VATTR_RETURN(vap, va_fsid, vfs_statfs(nmp->mp)->f_fsid.val[0]);
	VATTR_RETURN(vap, va_filerev, np->qid.vers);
	VATTR_RETURN(vap, va_gen, 0);
	VATTR_RETURN(vap, va_encoding, 0x7E);	/* kTextEncodingMacUnicode */
out:
	nunlock_9p(np);
	return e;
}

static int
vnop_setattr_9p(struct vnop_setattr_args *ap)
{
	struct vnode_attr *vap;
	struct timespec *at, *mt;
	uint32_t valid, mode;
	uint64_t size;
	node_9p *np;
	vnode_t vp;
	int e;

	vp = ap->a_vp;
	vap = ap->a_vap;
	np = NTO9P(vp);
	if (vnode_vfsisrdonly(vp))
		return EROFS;
	if (ISRSRC(np)) {
		/* only the fork length is the fork's own */
		if (VATTR_IS_ACTIVE(vap, va_data_size) &&
		    (e = rsrc_truncate_9p(np, vap->va_data_size)))
			return e;
		vap->va_supported |= vap->va_active;
		return 0;
	}

	valid = 0;
	mode = 0;
	size = 0;
	at = mt = NULL;
	if (VATTR_IS_ACTIVE(vap, va_data_size)) {
		if (vnode_isdir(vp))
			return EISDIR;
		valid |= SA_SIZE;
		size = vap->va_data_size;
	}
	if (VATTR_IS_ACTIVE(vap, va_mode)) {
		valid |= SA_MODE;
		mode = vap->va_mode & 07777;
	}
	if (VATTR_IS_ACTIVE(vap, va_access_time)) {
		valid |= SA_ATIME | SA_ATIME_SET;
		at = &vap->va_access_time;
	}
	if (VATTR_IS_ACTIVE(vap, va_modify_time)) {
		valid |= SA_MTIME | SA_MTIME_SET;
		mt = &vap->va_modify_time;
	}

	nlock_9p(np, NODE_LCK_EXCLUSIVE);
	e = 0;
	if (valid) {
		/* drop pages past the new EOF before the host truncates */
		if (ISSET(valid, SA_SIZE))
			ubc_setsize(vp, size);
		e = setattr_9p(np->nmp, np->fid, valid, mode, size, at, mt);
		np->attrtime = 0;
		if (e == 0 && ISSET(valid, SA_SIZE))
			np->attr.size = size;
	}
	nunlock_9p(np);
	if (e)
		return e;

	VATTR_SET_SUPPORTED(vap, va_data_size);
	VATTR_SET_SUPPORTED(vap, va_mode);
	VATTR_SET_SUPPORTED(vap, va_access_time);
	VATTR_SET_SUPPORTED(vap, va_modify_time);
	/* ownership and flags are the host's business */
	VATTR_SET_SUPPORTED(vap, va_uid);
	VATTR_SET_SUPPORTED(vap, va_gid);
	VATTR_SET_SUPPORTED(vap, va_flags);
	VATTR_SET_SUPPORTED(vap, va_create_time);
	VATTR_SET_SUPPORTED(vap, va_change_time);
	VATTR_SET_SUPPORTED(vap, va_backup_time);
	VATTR_SET_SUPPORTED(vap, va_encoding);
	return 0;
}

static int
nread_9p(node_9p *np, uio_t uio)
{
	uint32_t n, l, sz;
	fid_9p fid;
	char *p;
	int e, tmp;

	if ((e = iofid_9p(np, 0, &fid, &tmp)))
		return e;
	sz = iosize_9p(np->nmp);
	p = malloc_9p(sz);
	if (p == NULL) {
		e = ENOMEM;
		goto out;
	}
	while (uio_resid(uio) > 0) {
		n = MIN(uio_resid(uio), sz);
		if ((e = read_9p(np->nmp, fid, p, n, uio_offset(uio), &l)) || l == 0)
			break;
		if ((e = uiomove(p, l, uio)))
			break;
	}
	free_9p(p);
out:
	if (tmp)
		clunk_9p(np->nmp, fid);
	return e;
}

static int
nwrite_9p(node_9p *np, uio_t uio)
{
	user_ssize_t resid;
	uint32_t l, n, sz;
	fid_9p fid;
	off_t off;
	char *p;
	int e, tmp;

	if ((e = iofid_9p(np, 1, &fid, &tmp)))
		return e;
	sz = iosize_9p(np->nmp);
	p = malloc_9p(sz);
	if (p == NULL) {
		e = ENOMEM;
		goto out;
	}
	while (uio_resid(uio) > 0) {
		off = uio_offset(uio);
		resid = uio_resid(uio);
		n = MIN(resid, sz);
		if ((e = uiomove(p, n, uio)))
			break;
		if ((e = write_9p(np->nmp, fid, p, n, off, &l)))
			break;
		if (l == 0) {
			e = EIO;
			break;
		}
		uio_setoffset(uio, off + l);
		uio_setresid(uio, resid - l);
	}
	free_9p(p);
out:
	if (tmp)
		clunk_9p(np->nmp, fid);
	return e;
}

static int
vnop_read_9p(struct vnop_read_args *ap)
{
	node_9p *np;
	vnode_t vp;
	uio_t uio;
	int e;

	vp = ap->a_vp;
	uio = ap->a_uio;
	np = NTO9P(vp);
	if (ISRSRC(np))
		return rsrc_read_9p(np, uio);
	if (vnode_isdir(vp))
		return EISDIR;
	if (uio_offset(uio) < 0)
		return EINVAL;
	if (uio_resid(uio) == 0)
		return 0;

	nlock_9p(np, NODE_LCK_SHARED);
	if (!vnode_isreg(vp) || vnode_isnocache(vp) || ISSET(ap->a_ioflag, IO_NOCACHE)) {
		ubc_sync_range(vp, uio_offset(uio), uio_offset(uio) + uio_resid(uio), UBC_PUSHDIRTY|UBC_SYNC|UBC_INVALIDATE);
		e = nread_9p(np, uio);
	} else
		e = cluster_read(vp, uio, np->attr.size, ap->a_ioflag);
	nunlock_9p(np);
	return e;
}

static int
vnop_write_9p(struct vnop_write_args *ap)
{
	user_ssize_t resid;
	off_t eof, zh, zt, off, osize;
	node_9p *np;
	vnode_t vp;
	uio_t uio;
	int e, flag;

	vp = ap->a_vp;
	uio = ap->a_uio;
	np = NTO9P(vp);
	if (ISRSRC(np))
		return rsrc_write_9p(np, uio, ap->a_ioflag);
	if (vnode_isdir(vp))
		return EISDIR;
	if (uio_offset(uio) < 0)
		return EINVAL;
	resid = uio_resid(uio);
	if (resid == 0)
		return 0;

	flag = ap->a_ioflag;
	nlock_9p(np, NODE_LCK_EXCLUSIVE);
	if (ISSET(flag, IO_APPEND)) {
		ngetattr_9p(np, 1);
		uio_setoffset(uio, MAX((off_t)np->attr.size, ubc_getsize(vp)));
	}
	off = uio_offset(uio);
	osize = MAX((off_t)np->attr.size, ubc_getsize(vp));

	if (vnode_isnocache(vp) || ISSET(flag, IO_NOCACHE)) {
		ubc_sync_range(vp, off, off + resid, UBC_PUSHDIRTY|UBC_SYNC|UBC_INVALIDATE);
		e = nwrite_9p(np, uio);
		if (e == 0 && off + resid > osize) {
			np->attr.size = off + resid;
			ubc_setsize(vp, np->attr.size);
		}
	} else {
		zh = zt = 0;
		eof = MAX(osize, off + resid);
		if (eof > osize) {
			if (off > osize) {
				zh = osize;
				SET(flag, IO_HEADZEROFILL);
			}
			zt = (eof + (PAGE_SIZE_64 - 1)) & ~PAGE_MASK_64;
			if (zt > eof) {
				zt = eof;
				SET(flag, IO_TAILZEROFILL);
			}
		}
		e = cluster_write(vp, uio, osize, eof, zh, zt, flag);
		if (e == 0 && eof > osize) {
			np->attr.size = eof;
			ubc_setsize(vp, eof);
		}
	}
	np->attrtime = 0;
	nunlock_9p(np);
	return e;
}

static int
vnop_select_9p(struct vnop_select_args *ap)
{
	(void)ap;
	return 1;
}

static int
vnop_revoke_9p(struct vnop_revoke_args *ap)
{
	return vn_revoke(ap->a_vp, ap->a_flags, ap->a_context);
}

static int
vnop_mmap_9p(struct vnop_mmap_args *ap)
{
	node_9p *np;

	np = NTO9P(ap->a_vp);
	if (ISRSRC(np))
		return ENOTSUP;
	nlock_9p(np, NODE_LCK_EXCLUSIVE);
	SET(np->flags, NODE_MMAPPED);
	nunlock_9p(np);
	return 0;
}

static int
vnop_mnomap_9p(struct vnop_mnomap_args *ap)
{
	node_9p *np;

	np = NTO9P(ap->a_vp);
	nlock_9p(np, NODE_LCK_EXCLUSIVE);
	CLR(np->flags, NODE_MMAPPED);
	nunlock_9p(np);
	return 0;
}

static int
vnop_fsync_9p(struct vnop_fsync_args *ap)
{
	node_9p *np;
	vnode_t vp;
	fid_9p fid;
	int e, tmp;

	vp = ap->a_vp;
	if (!vnode_isreg(vp))
		return 0;
	np = NTO9P(vp);
	if (ISRSRC(np))
		return rsrc_flush_9p(np);
	ubc_sync_range(vp, 0, ubc_getsize(vp), UBC_PUSHDIRTY|UBC_SYNC);
	nlock_9p(np, NODE_LCK_SHARED);
	e = 0;
	if (np->openfid[OFID_WR].fid != NOFID || np->openfid[OFID_RDWR].fid != NOFID) {
		if ((e = iofid_9p(np, 1, &fid, &tmp)) == 0) {
			e = fsync_9p(np->nmp, fid);
			if (tmp)
				clunk_9p(np->nmp, fid);
		}
	}
	nunlock_9p(np);
	return e;
}

static int
nremove_9p(vnode_t dvp, vnode_t vp, struct componentname *cnp, uint32_t flags)
{
	node_9p *dnp;
	int e;

	dnp = NTO9P(dvp);
	if (dvp == vp)
		return EINVAL;
	nlock_9p(dnp, NODE_LCK_EXCLUSIVE);
	e = unlinkat_9p(dnp->nmp, dnp->fid, cnp->cn_nameptr, cnp->cn_namelen, flags);
	dnp->attrtime = 0;
	nunlock_9p(dnp);
	if (e == 0) {
		id_forget_9p(dnp->nmp, NTO9P(vp)->id);
		cache_purge(vp);
		vnode_recycle(vp);
	}
	return e;
}

static int
vnop_remove_9p(struct vnop_remove_args *ap)
{
	if (ISSET(ap->a_flags, VNODE_REMOVE_NODELETEBUSY) && vnode_isinuse(ap->a_vp, 0))
		return EBUSY;
	return nremove_9p(ap->a_dvp, ap->a_vp, ap->a_cnp, 0);
}

static int
vnop_rmdir_9p(struct vnop_rmdir_args *ap)
{
	return nremove_9p(ap->a_dvp, ap->a_vp, ap->a_cnp, L_AT_REMOVEDIR);
}

static int
vnop_rename_9p(struct vnop_rename_args *ap)
{
	node_9p *fdnp, *tdnp;
	int e;

	fdnp = NTO9P(ap->a_fdvp);
	tdnp = NTO9P(ap->a_tdvp);
	if (vnode_mount(ap->a_fdvp) != vnode_mount(ap->a_tdvp))
		return EXDEV;
	e = renameat_9p(fdnp->nmp, fdnp->fid, ap->a_fcnp->cn_nameptr, ap->a_fcnp->cn_namelen,
			tdnp->fid, ap->a_tcnp->cn_nameptr, ap->a_tcnp->cn_namelen);
	if (e)
		return e;
	fdnp->attrtime = 0;
	tdnp->attrtime = 0;
	if (ap->a_tvp)
		id_forget_9p(tdnp->nmp, NTO9P(ap->a_tvp)->id);
	id_link_9p(tdnp->nmp, NTO9P(ap->a_fvp)->id, tdnp->id,
		   ap->a_tcnp->cn_nameptr, ap->a_tcnp->cn_namelen);
	cache_purge(ap->a_fvp);
	cache_purge(ap->a_fdvp);
	if (ap->a_tvp) {
		cache_purge(ap->a_tvp);
		vnode_recycle(ap->a_tvp);
	}
	if (ap->a_fdvp != ap->a_tdvp)
		cache_purge(ap->a_tdvp);
	return 0;
}

#define DIRENT32_LEN(namlen) \
	((offsetof(struct dirent, d_name) + (namlen) + 1 + 3) & ~3)

static int
vnop_readdir_9p(struct vnop_readdir_args *ap)
{
	struct dirent de;
	dirent_9p *dp;
	node_9p *np;
	vnode_t vp;
	fid_9p fid;
	off_t off;
	uio_t uio;
	uint32_t i, nd, nlen;
	int e, tmp;

	vp = ap->a_vp;
	uio = ap->a_uio;
	np = NTO9P(vp);
	if (!vnode_isdir(vp))
		return ENOTDIR;
	if (ap->a_flags)
		return EINVAL;	/* no extended or seek-offset entries */
	off = uio_offset(uio);
	if (off < 0)
		return EINVAL;
	if (ap->a_eofflag)
		*ap->a_eofflag = 0;
	if (uio_resid(uio) == 0)
		return 0;

	nlock_9p(np, NODE_LCK_EXCLUSIVE);
	e = 0;
	if (off == 0 || np->dirents == NULL) {
		free_9p(np->dirents);
		np->dirents = NULL;
		np->ndirents = 0;
		if ((e = iofid_9p(np, 0, &fid, &tmp)))
			goto out;
		if (!tmp) {
			/* a shared fid has a position; use a fresh one */
			qid_9p qid;
			uint32_t iounit;

			if ((e = walk_9p(np->nmp, np->fid, NULL, 0, &fid, &qid)))
				goto out;
			if ((e = lopen_9p(np->nmp, fid, L_O_RDONLY, &qid, &iounit))) {
				clunk_9p(np->nmp, fid);
				goto out;
			}
		}
		e = readdirs_9p(np->nmp, fid, &np->dirents, &np->ndirents);
		clunk_9p(np->nmp, fid);
		if (e)
			goto out;
	}

	dp = np->dirents;
	nd = np->ndirents;
	for (i = off; i < nd; i++) {
		bzero(&de, sizeof(de));
		de.d_fileno = id_get_9p(np->nmp, dp[i].qid.path);
		de.d_type = dp[i].type;
		nlen = strlen(dp[i].name);
		de.d_namlen = MIN(nlen, sizeof(de.d_name) - 1);
		bcopy(dp[i].name, de.d_name, de.d_namlen);
		de.d_reclen = DIRENT32_LEN(de.d_namlen);
		id_link_9p(np->nmp, de.d_fileno, np->id, de.d_name, de.d_namlen);
		if (uio_resid(uio) < de.d_reclen)
			break;
		if ((e = uiomove((caddr_t)&de, de.d_reclen, uio)))
			goto out;
	}
	uio_setoffset(uio, i);
	if (ap->a_numdirent)
		*ap->a_numdirent = i - off;
	if (i >= nd) {
		if (ap->a_eofflag)
			*ap->a_eofflag = 1;
		free_9p(np->dirents);
		np->dirents = NULL;
		np->ndirents = 0;
	}
out:
	nunlock_9p(np);
	return e;
}

static int
vnop_readlink_9p(struct vnop_readlink_args *ap)
{
	node_9p *np;
	char *s;
	int e;

	np = NTO9P(ap->a_vp);
	nlock_9p(np, NODE_LCK_SHARED);
	e = readlink_9p(np->nmp, np->fid, &s);
	nunlock_9p(np);
	if (e)
		return e;
	e = uiomove(s, strlen(s), ap->a_uio);
	free_9p(s);
	return e;
}

static int
vnop_reclaim_9p(struct vnop_reclaim_args *ap)
{
	mount_9p *nmp;
	node_9p *np;
	vnode_t vp;
	int i;

	vp = ap->a_vp;
	np = NTO9P(vp);
	nmp = np->nmp;

	lck_mtx_lock(nmp->nodelck);
	SET(np->flags, NODE_RECL);
	if (!ISRSRC(np))
		LIST_REMOVE(np, next);
	if (np->rsrc)
		np->rsrc->data = NULL;
	lck_mtx_unlock(nmp->nodelck);
	if (ISRSRC(np))
		rsrc_reclaim_9p(np);

	vnode_removefsref(vp);
	vnode_clearfsnode(vp);
	cache_purge(vp);

	for (i = 0; i < NOFIDS; i++)
		if (np->openfid[i].fid != NOFID)
			clunk_9p(nmp, np->openfid[i].fid);
	/* the root fid is clunked by vfs_unmount_9p */
	if (!ISSET(nmp->flags, F_UNMOUNTING))
		clunk_9p(nmp, np->fid);

	lck_mtx_lock(nmp->nodelck);
	CLR(np->flags, NODE_RECL);
	if (ISSET(np->flags, NODE_WAITRECL)) {
		CLR(np->flags, NODE_WAITRECL);
		wakeup(np);
	}
	lck_mtx_unlock(nmp->nodelck);
	lck_rw_free(np->lck, lck_grp_9p);
	lock_free_9p(np);
	free_9p(np->dirents);
	free_9p(np);
	return 0;
}

static int
vnop_pathconf_9p(struct vnop_pathconf_args *ap)
{
	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*ap->a_retval = 1;
		return 0;
	case _PC_NAME_MAX:
		*ap->a_retval = NAME_MAX;
		return 0;
	case _PC_PATH_MAX:
		*ap->a_retval = PATH_MAX;
		return 0;
	case _PC_PIPE_BUF:
		*ap->a_retval = PIPE_BUF;
		return 0;
	case _PC_CHOWN_RESTRICTED:
		*ap->a_retval = 200112;
		return 0;
	case _PC_NO_TRUNC:
		*ap->a_retval = 200112;
		return 0;
	case _PC_CASE_SENSITIVE:
		*ap->a_retval = 0;
		return 0;
	case _PC_CASE_PRESERVING:
		*ap->a_retval = 1;
		return 0;
	default:
		return EINVAL;
	}
}

static int
vnop_pagein_9p(struct vnop_pagein_args *ap)
{
	node_9p *np;

	np = NTO9P(ap->a_vp);
	return cluster_pagein(ap->a_vp, ap->a_pl, ap->a_pl_offset, ap->a_f_offset,
			      ap->a_size, MAX((off_t)np->attr.size, ubc_getsize(ap->a_vp)),
			      ap->a_flags);
}

static int
vnop_pageout_9p(struct vnop_pageout_args *ap)
{
	node_9p *np;

	if (vnode_vfsisrdonly(ap->a_vp))
		return EROFS;
	np = NTO9P(ap->a_vp);
	return cluster_pageout(ap->a_vp, ap->a_pl, ap->a_pl_offset, ap->a_f_offset,
			       ap->a_size, ubc_getsize(ap->a_vp), ap->a_flags);
}

static int
vnop_blktooff_9p(struct vnop_blktooff_args *ap)
{
	*ap->a_offset = (off_t)ap->a_lblkno * PAGE_SIZE;
	return 0;
}

static int
vnop_offtoblk_9p(struct vnop_offtoblk_args *ap)
{
	*ap->a_lblkno = ap->a_offset / PAGE_SIZE;
	return 0;
}

static int
vnop_blockmap_9p(struct vnop_blockmap_args *ap)
{
	if (ap->a_run)
		*ap->a_run = ap->a_size;
	if (ap->a_bpn)
		*ap->a_bpn = ap->a_foffset / PAGE_SIZE;
	if (ap->a_poff)
		*(int32_t *)ap->a_poff = 0;
	return 0;
}

static int
vnop_strategy_9p(struct vnop_strategy_args *ap)
{
	struct buf *bp;
	node_9p *np;
	caddr_t addr;
	uio_t uio;
	off_t off, eof;
	uint32_t count;
	int e, flags;

	bp = ap->a_bp;
	np = NTO9P(buf_vnode(bp));
	flags = buf_flags(bp);
	uio = NULL;
	addr = NULL;

	if ((e = buf_map(bp, &addr)))
		goto out;
	uio = uio_create(1, (off_t)buf_blkno(bp) * PAGE_SIZE, UIO_SYSSPACE,
			 ISSET(flags, B_READ) ? UIO_READ : UIO_WRITE);
	if (uio == NULL) {
		e = ENOMEM;
		goto out;
	}
	count = buf_count(bp);
	if (!ISSET(flags, B_READ)) {
		/* the last page is only partly file */
		eof = ubc_getsize(buf_vnode(bp));
		off = (off_t)buf_blkno(bp) * PAGE_SIZE;
		if (off >= eof)
			count = 0;
		else if (off + count > eof)
			count = eof - off;
	}
	uio_addiov(uio, CAST_USER_ADDR_T(addr), count);
	if (ISSET(flags, B_READ)) {
		if ((e = nread_9p(np, uio)))
			goto out;
		/* zero the rest of the page past EOF */
		if (uio_resid(uio) > 0) {
			bzero(addr + buf_count(bp) - uio_resid(uio), uio_resid(uio));
			uio_update(uio, uio_resid(uio));
		}
	} else if (count)
		e = nwrite_9p(np, uio);
	buf_setresid(bp, uio_resid(uio));
out:
	if (uio)
		uio_free(uio);
	if (addr)
		buf_unmap(bp);
	buf_seterror(bp, e);
	buf_biodone(bp);
	return e;
}

static int
vnop_bwrite_9p(struct vnop_bwrite_args *ap)
{
	return buf_bwrite(ap->a_bp);
}

static int
isrsrc(const char *name)
{
	return strcmp(name, XATTR_RESOURCEFORK_NAME) == 0;
}

/* read the whole xattr fid into buf (len bytes) */
__private_extern__ int
xread_9p(mount_9p *nmp, fid_9p fid, uint8_t *buf, uint64_t off, uint32_t len)
{
	uint32_t n;
	int e;

	while (len) {
		if ((e = read_9p(nmp, fid, buf, len, off, &n)))
			return e;
		if (n == 0)
			return EIO;
		buf += n;
		off += n;
		len -= n;
	}
	return 0;
}

static int
vnop_getxattr_9p(struct vnop_getxattr_args *ap)
{
	mount_9p *nmp;
	node_9p *np;
	uint64_t size, off;
	uint32_t n;
	fid_9p fid;
	uint8_t *buf;
	uio_t uio;
	int e;

	np = NTO9P(ap->a_vp);
	nmp = np->nmp;
	uio = ap->a_uio;
	if (ISRSRC(np))
		return ENOTSUP;
	if (isrsrc(ap->a_name))
		rsrc_sync_9p(np, 0);

	nlock_9p(np, NODE_LCK_SHARED);
	e = xattrwalk_9p(nmp, np->fid, ap->a_name, &fid, &size);
	nunlock_9p(np);
	if (e)
		return e;

	if (uio == NULL) {
		*ap->a_size = size;
		goto out;
	}
	off = isrsrc(ap->a_name) ? uio_offset(uio) : 0;
	if (!isrsrc(ap->a_name) && (uint64_t)uio_resid(uio) < size) {
		e = ERANGE;
		goto out;
	}
	if (off >= size)
		goto out;
	n = MIN((uint64_t)uio_resid(uio), size - off);
	buf = malloc_9p(n);
	if (buf == NULL) {
		e = ENOMEM;
		goto out;
	}
	if ((e = xread_9p(nmp, fid, buf, off, n)) == 0)
		e = uiomove((caddr_t)buf, n, uio);
	free_9p(buf);
	if (ap->a_size)
		*ap->a_size = n;
out:
	clunk_9p(nmp, fid);
	return e;
}

/* replace the value of name with buf[0..len) */
__private_extern__ int
xset_9p(node_9p *np, const char *name, uint8_t *buf, uint64_t len, uint32_t flags)
{
	mount_9p *nmp;
	uint32_t n;
	uint64_t off;
	fid_9p fid;
	qid_9p qid;
	int e, e2;

	nmp = np->nmp;
	/* a macOS host overwrites a resource fork in place, keeping its tail */
	if (len && isrsrc(name) &&
	    xattrwalk_9p(nmp, np->fid, name, &fid, &off) == 0) {
		clunk_9p(nmp, fid);
		if (off > len) {
			if ((e = xset_9p(np, name, NULL, 0, 0)))
				return e;
			flags &= ~L_XATTR_REPLACE;
		}
	}
	if ((e = walk_9p(nmp, np->fid, NULL, 0, &fid, &qid)))
		return e;
	if ((e = xattrcreate_9p(nmp, fid, name, len, flags))) {
		clunk_9p(nmp, fid);
		return e;
	}
	for (off = 0; off < len && !e; off += n) {
		e = write_9p(nmp, fid, buf + off, MIN(len - off, iosize_9p(nmp)), off, &n);
		if (!e && n == 0)
			e = EIO;
	}
	/* the server applies the value on clunk */
	e2 = clunk_9p(nmp, fid);
	return e ? e : e2;
}

static int
vnop_setxattr_9p(struct vnop_setxattr_args *ap)
{
	mount_9p *nmp;
	node_9p *np;
	uint64_t size, off, old, len;
	uint32_t flags;
	uint8_t *buf;
	fid_9p fid;
	uio_t uio;
	int e;

	np = NTO9P(ap->a_vp);
	nmp = np->nmp;
	uio = ap->a_uio;
	if (ISRSRC(np))
		return ENOTSUP;
	if (isrsrc(ap->a_name))
		rsrc_sync_9p(np, 1);
	if (vnode_vfsisrdonly(ap->a_vp))
		return EROFS;

	flags = 0;
	if (ISSET(ap->a_options, XATTR_CREATE))
		flags |= L_XATTR_CREATE;
	if (ISSET(ap->a_options, XATTR_REPLACE))
		flags |= L_XATTR_REPLACE;

	size = uio_resid(uio);
	off = isrsrc(ap->a_name) ? uio_offset(uio) : 0;
	if (off + size > XATTR_MAX)
		return E2BIG;

	nlock_9p(np, NODE_LCK_EXCLUSIVE);
	old = 0;
	len = off + size;
	if (off) {
		/* a positioned resource fork write keeps the rest of the fork */
		e = xattrwalk_9p(nmp, np->fid, ap->a_name, &fid, &old);
		if (e && e != ENOATTR)
			goto out;
		if (e)
			old = 0;
		else
			clunk_9p(nmp, fid);
		len = MAX(old, off + size);
	}
	buf = malloc_9p(len ? len : 1);
	if (buf == NULL) {
		e = ENOMEM;
		goto out;
	}
	e = 0;
	if (off && old) {
		if ((e = xattrwalk_9p(nmp, np->fid, ap->a_name, &fid, &old)) == 0) {
			e = xread_9p(nmp, fid, buf, 0, MIN(old, len));
			clunk_9p(nmp, fid);
		}
	}
	if (e == 0)
		e = uiomove((caddr_t)buf + off, size, uio);
	if (e == 0)
		e = xset_9p(np, ap->a_name, buf, len, off ? 0 : flags);
	free_9p(buf);
out:
	nunlock_9p(np);
	return e;
}

static int
vnop_removexattr_9p(struct vnop_removexattr_args *ap)
{
	node_9p *np;
	int e;

	np = NTO9P(ap->a_vp);
	if (ISRSRC(np))
		return ENOTSUP;
	if (vnode_vfsisrdonly(ap->a_vp))
		return EROFS;
	if (isrsrc(ap->a_name))
		rsrc_sync_9p(np, 1);
	nlock_9p(np, NODE_LCK_EXCLUSIVE);
	e = xset_9p(np, ap->a_name, NULL, 0, 0);
	nunlock_9p(np);
	return e;
}

/* servers name the Apple attributes user.com.apple.*; drop the prefix */
static uint64_t
xnames_9p(uint8_t *buf, uint64_t size)
{
	static const char pfx[] = "user.";
	uint64_t i, o, n;
	char *p;

	for (i = o = 0; i < size; i += n + 1) {
		p = (char *)buf + i;
		for (n = 0; i + n < size && p[n]; n++)
			;
		if (i + n >= size)
			break;
		if (strncmp(p, pfx, sizeof(pfx) - 1) == 0 &&
		    (strcmp(p + sizeof(pfx) - 1, XATTR_FINDERINFO_NAME) == 0 ||
		    strcmp(p + sizeof(pfx) - 1, XATTR_RESOURCEFORK_NAME) == 0)) {
			memmove(buf + o, p + sizeof(pfx) - 1, n - (sizeof(pfx) - 1) + 1);
			o += n - (sizeof(pfx) - 1) + 1;
		} else {
			memmove(buf + o, p, n + 1);
			o += n + 1;
		}
	}
	return o;
}

static int
vnop_listxattr_9p(struct vnop_listxattr_args *ap)
{
	mount_9p *nmp;
	node_9p *np;
	uint64_t size;
	uint8_t *buf;
	fid_9p fid;
	int e;

	np = NTO9P(ap->a_vp);
	nmp = np->nmp;
	if (ISRSRC(np))
		return ENOTSUP;
	rsrc_sync_9p(np, 0);
	nlock_9p(np, NODE_LCK_SHARED);
	e = xattrwalk_9p(nmp, np->fid, "", &fid, &size);
	nunlock_9p(np);
	if (e)
		return e;
	buf = NULL;
	if (size) {
		buf = malloc_9p(size);
		if (buf == NULL)
			e = ENOMEM;
		else if ((e = xread_9p(nmp, fid, buf, 0, size)) == 0)
			size = xnames_9p(buf, size);
	}
	clunk_9p(nmp, fid);
	if (e == 0) {
		if (ap->a_uio == NULL)
			*ap->a_size = size;
		else if ((uint64_t)uio_resid(ap->a_uio) < size)
			e = ERANGE;
		else if (size)
			e = uiomove((caddr_t)buf, size, ap->a_uio);
	}
	if (buf)
		free_9p(buf);
	return e;
}

static struct vnodeopv_entry_desc vnode_entry_desc_9p[] = {
	{ &vnop_default_desc,		(vnop_t *)vn_default_error },
	{ &vnop_lookup_desc,		(vnop_t *)vnop_lookup_9p },
	{ &vnop_create_desc,		(vnop_t *)vnop_create_9p },
	{ &vnop_open_desc,		(vnop_t *)vnop_open_9p },
	{ &vnop_close_desc,		(vnop_t *)vnop_close_9p },
	{ &vnop_getattr_desc,		(vnop_t *)vnop_getattr_9p },
	{ &vnop_setattr_desc,		(vnop_t *)vnop_setattr_9p },
	{ &vnop_read_desc,		(vnop_t *)vnop_read_9p },
	{ &vnop_write_desc,		(vnop_t *)vnop_write_9p },
	{ &vnop_select_desc,		(vnop_t *)vnop_select_9p },
	{ &vnop_revoke_desc,		(vnop_t *)vnop_revoke_9p },
	{ &vnop_mmap_desc,		(vnop_t *)vnop_mmap_9p },
	{ &vnop_mnomap_desc,		(vnop_t *)vnop_mnomap_9p },
	{ &vnop_fsync_desc,		(vnop_t *)vnop_fsync_9p },
	{ &vnop_remove_desc,		(vnop_t *)vnop_remove_9p },
	{ &vnop_rename_desc,		(vnop_t *)vnop_rename_9p },
	{ &vnop_mkdir_desc,		(vnop_t *)vnop_mkdir_9p },
	{ &vnop_rmdir_desc,		(vnop_t *)vnop_rmdir_9p },
	{ &vnop_symlink_desc,		(vnop_t *)vnop_symlink_9p },
	{ &vnop_readdir_desc,		(vnop_t *)vnop_readdir_9p },
	{ &vnop_readlink_desc,		(vnop_t *)vnop_readlink_9p },
	{ &vnop_reclaim_desc,		(vnop_t *)vnop_reclaim_9p },
	{ &vnop_advlock_desc,		(vnop_t *)vnop_advlock_9p },
	{ &vnop_pathconf_desc,		(vnop_t *)vnop_pathconf_9p },
	{ &vnop_pagein_desc,		(vnop_t *)vnop_pagein_9p },
	{ &vnop_pageout_desc,		(vnop_t *)vnop_pageout_9p },
	{ &vnop_getxattr_desc,		(vnop_t *)vnop_getxattr_9p },
	{ &vnop_setxattr_desc,		(vnop_t *)vnop_setxattr_9p },
	{ &vnop_removexattr_desc,	(vnop_t *)vnop_removexattr_9p },
	{ &vnop_listxattr_desc,		(vnop_t *)vnop_listxattr_9p },
	{ &vnop_blktooff_desc,		(vnop_t *)vnop_blktooff_9p },
	{ &vnop_offtoblk_desc,		(vnop_t *)vnop_offtoblk_9p },
	{ &vnop_blockmap_desc,		(vnop_t *)vnop_blockmap_9p },
	{ &vnop_strategy_desc,		(vnop_t *)vnop_strategy_9p },
	{ &vnop_bwrite_desc,		(vnop_t *)vnop_bwrite_9p },
	{ NULL, NULL }
};

__private_extern__ struct vnodeopv_desc vnodeopv_desc_9p = {
	&vnode_op_9p,
	vnode_entry_desc_9p
};
