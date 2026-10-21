/*
 * Vnode operations for the Mac OS X 10.3 VFS: vnodes are locked by the
 * filesystem, references and pathname buffers follow the 4.4BSD rules,
 * and all of it runs under the kernel funnel, which a thread gives up
 * while it sleeps on a 9P reply.
 */
#include "9p.h"

typedef int vop_t(void *);
__private_extern__ vop_t **vnode_op_9p = NULL;

extern int nop_abortop(struct vop_abortop_args *);
extern int nop_lease(struct vop_lease_args *);
extern int err_mknod(struct vop_mknod_args *);
extern int err_link(struct vop_link_args *);
extern int vop_getattrlist_9p(struct vop_getattrlist_args *);
extern int vop_setattrlist_9p(struct vop_setattrlist_args *);

enum {
	ATTR_TIMEOUT_US	= 1000000,	/* host files change behind our back */
};

static uint64_t
now_us(void)
{
	struct timeval tv;

	microuptime(&tv);
	return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static int
haswriter(node_9p *np)
{
	return np->openfid[OFID_WR].fid != NOFID || np->openfid[OFID_RDWR].fid != NOFID;
}

/* refresh np->attr */
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
	if (np->vp && np->vp->v_type == VREG && UBCINFOEXISTS(np->vp) &&
	    haswriter(np) && (off_t)a.size < ubc_getsize(np->vp))
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

/* the open fid table; held only across fid bookkeeping, never across I/O */
static void
nbusy(node_9p *np)
{
	while (ISSET(np->flags, NODE_BUSY)) {
		SET(np->flags, NODE_WANT);
		tsleep(&np->openfid, PINOD, "9pbusy", 0);
	}
	SET(np->flags, NODE_BUSY);
}

static void
nunbusy(node_9p *np)
{
	CLR(np->flags, NODE_BUSY);
	if (ISSET(np->flags, NODE_WANT)) {
		CLR(np->flags, NODE_WANT);
		wakeup(&np->openfid);
	}
}

static void
initdone(node_9p *np)
{
	CLR(np->flags, NODE_INIT);
	if (ISSET(np->flags, NODE_WAITINIT)) {
		CLR(np->flags, NODE_WAITINIT);
		wakeup(np);
	}
}

#define	HASH9P(nmp, k)	(&(nmp)->node[(k) & (nmp)->nodelen])

/*
 * Find or make the vnode for qid, returned locked and referenced.
 * fid is consumed only when a new node is made.
 */
__private_extern__ int
nget_9p(mount_9p *nmp, fid_9p fid, qid_9p qid, int flags, struct vnode **vpp,
	struct proc *p)
{
	struct hnode_9p *nhp;
	node_9p *np, *nnp;
	struct vnode *vp;
	int e, i;

	*vpp = NULL;
	nnp = NULL;
	nhp = HASH9P(nmp, qid.path);
loop:
	LIST_FOREACH(np, nhp, next) {
		if (np->qid.path != qid.path)
			continue;
		if (ISSET(np->flags, NODE_INIT)) {
			SET(np->flags, NODE_WAITINIT);
			tsleep(np, PINOD, "9pinit", 0);
			goto loop;
		}
		vp = np->vp;
		if (vget(vp, LK_EXCLUSIVE, p))
			goto loop;
		free_9p(nnp);
		*vpp = vp;
		return 0;
	}
	if (fid == NOFID) {
		free_9p(nnp);
		return ENOENT;
	}
	if (nnp == NULL) {
		/* the allocation may sleep: look again after it */
		if ((nnp = malloc_9p(sizeof(*nnp))) == NULL)
			return ENOMEM;
		goto loop;
	}

	np = nnp;
	np->nmp = nmp;
	np->fid = fid;
	np->qid = qid;
	np->id = id_get_9p(nmp, qid.path);
	for (i = 0; i < NOFIDS; i++)
		np->openfid[i].fid = NOFID;
	np->flags = NODE_INIT | NODE_HASHED;
	if (flags & NGET_AD)
		np->flags |= NODE_AD;
	lockinit(&np->lock, PINOD, "9pnode", 0, 0);
	lockmgr(&np->lock, LK_EXCLUSIVE, NULL, p);
	LIST_INSERT_HEAD(nhp, np, next);

	if ((e = ngetattr_9p(np, 1)) == 0)
		e = getnewvnode(VT_OTHER, nmp->mp, vnode_op_9p, &vp);
	if (e) {
		LIST_REMOVE(np, next);
		initdone(np);
		lockmgr(&np->lock, LK_RELEASE, NULL, p);
		free_9p(np);
		return e;
	}
	vp->v_data = np;
	vp->v_type = ISAD(np) ? VREG : vtype_9p(np->attr.mode);
	if (flags & NGET_ROOT)
		vp->v_flag |= VROOT;
	np->vp = vp;
	initdone(np);

	if (vp->v_type == VREG && (e = ubc_info_init(vp))) {
		np->fid = NOFID;	/* the caller's still */
		vput(vp);
		return e;
	}
	*vpp = vp;
	return 0;
}
#undef HASH9P

/* walk to dir/name: its vnode, locked and referenced */
static int
nwalkget_9p(node_9p *dnp, char *name, int nname, struct vnode **vpp, struct proc *p)
{
	mount_9p *nmp;
	fid_9p fid, ofid;
	qid_9p qid;
	node_9p *np;
	int e;

	nmp = dnp->nmp;
	if ((e = walk_9p(nmp, dnp->fid, name, nname, &fid, &qid)))
		return e;
	e = nget_9p(nmp, fid, qid, 0, vpp, p);
	if (e == 0) {
		np = NTO9P(*vpp);
		if (np->fid != fid) {
			/* renamed on the host, the old fid names nothing: keep the new one */
			ofid = np->fid;
			np->fid = fid;
			np->attrtime = 0;
			fid = ofid;
		} else
			fid = NOFID;
		if (!(nname == 2 && name[0] == '.' && name[1] == '.'))
			id_link_9p(nmp, np->id, dnp->id, name, nname);
	}
	if (fid != NOFID)
		clunk_9p(nmp, fid);
	return e;
}

/* the pathname buffer is ours to free unless the caller saves it */
static void
freename(struct componentname *cnp)
{
	char *s;

	if ((cnp->cn_flags & (HASBUF | SAVESTART)) == HASBUF) {
		s = cnp->cn_pnbuf;
		cnp->cn_pnbuf = NULL;
		cnp->cn_flags &= ~HASBUF;
		FREE_ZONE(s, cnp->cn_pnlen, M_NAMEI);
	}
}

static int
vop_lookup_9p(struct vop_lookup_args *ap)
{
	struct componentname *cnp;
	struct vnode *dvp, **vpp;
	struct proc *p;
	node_9p *dnp;
	int e, e2, flags, lockparent, islast;

	dvp = ap->a_dvp;
	vpp = ap->a_vpp;
	cnp = ap->a_cnp;
	p = cnp->cn_proc;
	dnp = NTO9P(dvp);
	flags = cnp->cn_flags;
	*vpp = NULL;

	if (dvp->v_type != VDIR)
		return ENOTDIR;
	islast = ISSET(flags, ISLASTCN);
	if (islast && ISSET(dvp->v_mount->mnt_flag, MNT_RDONLY) &&
	    (cnp->cn_nameiop == DELETE || cnp->cn_nameiop == RENAME))
		return EROFS;
	lockparent = ISSET(flags, LOCKPARENT);
	if (islast && cnp->cn_nameiop != LOOKUP)
		cnp->cn_flags |= SAVENAME;

	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		VREF(dvp);
		*vpp = dvp;
		return 0;
	}
	if (ISSET(flags, ISDOTDOT)) {
		VOP_UNLOCK(dvp, 0, p);
		e = nwalkget_9p(dnp, "..", 2, vpp, p);
		if (e || (lockparent && islast)) {
			e2 = vn_lock(dvp, LK_EXCLUSIVE | LK_RETRY, p);
			if (e == 0 && e2) {
				vput(*vpp);
				*vpp = NULL;
				e = e2;
			}
		}
		return e;
	}

	e = nwalkget_9p(dnp, cnp->cn_nameptr, cnp->cn_namelen, vpp, p);
	if (e == ENOENT && adname_9p(cnp->cn_nameptr, cnp->cn_namelen)) {
		e = adget_9p(dnp, cnp->cn_nameptr + 2, cnp->cn_namelen - 2, 0, vpp, p);
		if (e == 0)
			id_link_9p(dnp->nmp, NTO9P(*vpp)->id, dnp->id, cnp->cn_nameptr,
				   cnp->cn_namelen);
	}
	if (e == 0) {
		if (!lockparent || !islast)
			VOP_UNLOCK(dvp, 0, p);
		return 0;
	}
	if (e == ENOENT && islast &&
	    (cnp->cn_nameiop == CREATE || cnp->cn_nameiop == RENAME)) {
		e = EJUSTRETURN;
		if (!lockparent)
			VOP_UNLOCK(dvp, 0, p);
	}
	return e;
}

/* make dir/name; the new vnode comes back locked */
static int
ncreate_9p(struct vnode *dvp, struct vnode **vpp, struct componentname *cnp,
	   enum vtype type, struct vattr *vap, char *target)
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
	iounit = 0;

	if (ISSET(dvp->v_mount->mnt_flag, MNT_RDONLY))
		return EROFS;
	mode = vap->va_mode & 07777;
	switch (type) {
	case VREG:
		if ((e = walk_9p(nmp, dnp->fid, NULL, 0, &ofid, &qid)))
			return e;
		flags = L_O_RDWR | L_O_CREAT;
		if (ISSET(vap->va_vaflags, VA_EXCLUSIVE))
			flags |= L_O_EXCL;
		if ((e = lcreate_9p(nmp, ofid, cnp->cn_nameptr, cnp->cn_namelen,
				    flags, mode | S_IFREG, &qid, &iounit))) {
			clunk_9p(nmp, ofid);
			return e;
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
		e = EOPNOTSUPP;
		break;
	}
	if (e)
		return e;

	dnp->attrtime = 0;
	e = nwalkget_9p(dnp, cnp->cn_nameptr, cnp->cn_namelen, vpp, cnp->cn_proc);
	if (e == 0 && ofid != NOFID) {
		/* keep the creating handle: it may write a file made read-only */
		np = NTO9P(*vpp);
		nbusy(np);
		if (np->openfid[OFID_RDWR].fid == NOFID) {
			np->openfid[OFID_RDWR].fid = ofid;
			np->iounit = iounit;
			ofid = NOFID;
		}
		nunbusy(np);
	}
	if (ofid != NOFID)
		clunk_9p(nmp, ofid);
	return e;
}

static int
vop_create_9p(struct vop_create_args *ap)
{
	struct componentname *cnp;
	int e;

	/* "._name" of an existing name: its AppleDouble view */
	cnp = ap->a_cnp;
	if (adname_9p(cnp->cn_nameptr, cnp->cn_namelen) &&
	    !ISSET(ap->a_dvp->v_mount->mnt_flag, MNT_RDONLY) &&
	    (e = adget_9p(NTO9P(ap->a_dvp), cnp->cn_nameptr + 2, cnp->cn_namelen - 2, 1,
			  ap->a_vpp, cnp->cn_proc)) != ENOENT) {
		if (e == 0)
			id_link_9p(NTO9P(ap->a_dvp)->nmp, NTO9P(*ap->a_vpp)->id,
				   NTO9P(ap->a_dvp)->id, cnp->cn_nameptr, cnp->cn_namelen);
		vput(ap->a_dvp);
		freename(cnp);
		return e;
	}

	e = ncreate_9p(ap->a_dvp, ap->a_vpp, ap->a_cnp, VREG, ap->a_vap, NULL);
	vput(ap->a_dvp);
	freename(ap->a_cnp);
	return e;
}

static int
vop_mkdir_9p(struct vop_mkdir_args *ap)
{
	int e;

	e = ncreate_9p(ap->a_dvp, ap->a_vpp, ap->a_cnp, VDIR, ap->a_vap, NULL);
	vput(ap->a_dvp);
	freename(ap->a_cnp);
	return e;
}

/* the caller takes the new link unreferenced */
static int
vop_symlink_9p(struct vop_symlink_args *ap)
{
	int e;

	/* symlink(2) leaves va_type unset */
	e = ncreate_9p(ap->a_dvp, ap->a_vpp, ap->a_cnp, VLNK, ap->a_vap, ap->a_target);
	if (e == 0)
		vput(*ap->a_vpp);
	vput(ap->a_dvp);
	freename(ap->a_cnp);
	return e;
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
 * A fid for I/O: an open one, referenced until iodone_9p, or for paging
 * after the last close a temporary one (*opp NULL).
 */
static int
iofid_9p(node_9p *np, int write, fid_9p *fidp, openfid_9p **opp)
{
	openfid_9p *op;
	fid_9p fid;
	qid_9p qid;
	uint32_t iounit;
	int e;

	nbusy(np);
	op = &np->openfid[write ? OFID_WR : OFID_RD];
	if (op->fid == NOFID)
		op = &np->openfid[OFID_RDWR];
	if (op->fid != NOFID) {
		op->ref++;
		*fidp = op->fid;
		*opp = op;
		nunbusy(np);
		return 0;
	}
	nunbusy(np);
	*opp = NULL;
	if ((e = walk_9p(np->nmp, np->fid, NULL, 0, &fid, &qid)))
		return e;
	if ((e = lopen_9p(np->nmp, fid, write ? L_O_WRONLY : L_O_RDONLY, &qid, &iounit))) {
		clunk_9p(np->nmp, fid);
		return e;
	}
	*fidp = fid;
	return 0;
}

/* drop an open fid reference, clunking the fid with the last one */
static void
ofidrele_9p(node_9p *np, openfid_9p *op)
{
	fid_9p fid;

	nbusy(np);
	fid = NOFID;
	if (op->ref && --op->ref == 0) {
		fid = op->fid;
		op->fid = NOFID;
	}
	nunbusy(np);
	if (fid != NOFID)
		clunk_9p(np->nmp, fid);
}

static void
iodone_9p(node_9p *np, fid_9p fid, openfid_9p *op)
{
	if (op)
		ofidrele_9p(np, op);
	else
		clunk_9p(np->nmp, fid);
}

/* drop cached pages if the host changed the file */
static void
revalidate_9p(node_9p *np)
{
	struct vnode *vp;
	struct timespec omtime;
	uint64_t osize;

	vp = np->vp;
	if (vp->v_type != VREG || !UBCINFOEXISTS(vp))
		return;
	if (ISSET(np->flags, NODE_MMAPPED))
		ubc_pushdirty(vp);
	osize = np->attr.size;
	omtime = np->attr.mtime;
	if (ngetattr_9p(np, 1) == 0 &&
	    (np->attr.size != osize || np->attr.mtime.tv_sec != omtime.tv_sec ||
	     np->attr.mtime.tv_nsec != omtime.tv_nsec)) {
		ubc_invalidate(vp, 0, (size_t)ubc_getsize(vp));
		ubc_setsize(vp, (off_t)np->attr.size);
	}
}

static int
vop_open_9p(struct vop_open_args *ap)
{
	openfid_9p *op;
	node_9p *np;
	fid_9p fid;
	qid_9p qid;
	uint32_t iounit, flags;
	int e, slot;

	np = NTO9P(ap->a_vp);
	if (ISAD(np))
		return 0;
	slot = ofidslot(ap->a_mode);
	switch (slot) {
	case OFID_RDWR:	flags = L_O_RDWR; break;
	case OFID_WR:	flags = L_O_WRONLY; break;
	default:	flags = L_O_RDONLY; break;
	}

	nbusy(np);
	op = &np->openfid[slot];
	if (op->fid == NOFID && slot != OFID_RDWR &&
	    np->openfid[OFID_RDWR].fid != NOFID && np->openfid[OFID_RDWR].ref == 0) {
		/* the handle vop_create kept */
		op = &np->openfid[OFID_RDWR];
	}
	if (op->fid == NOFID) {
		if ((e = walk_9p(np->nmp, np->fid, NULL, 0, &fid, &qid))) {
			nunbusy(np);
			return e;
		}
		if ((e = lopen_9p(np->nmp, fid, flags, &qid, &iounit))) {
			clunk_9p(np->nmp, fid);
			nunbusy(np);
			return e;
		}
		np->iounit = iounit;
		op->fid = fid;
	}
	op->ref++;
	nunbusy(np);
	revalidate_9p(np);
	return 0;
}

static int
vop_close_9p(struct vop_close_args *ap)
{
	openfid_9p *op;
	struct vnode *vp;
	node_9p *np;
	int slot;

	vp = ap->a_vp;
	np = NTO9P(vp);
	if (ISAD(np))
		return adflush_9p(np);
	slot = ofidslot(ap->a_fflag);
	op = &np->openfid[slot];
	if (op->ref == 0 && np->openfid[OFID_RDWR].ref)
		op = &np->openfid[OFID_RDWR];
	if (op->ref == 0)
		return 0;
	/* push mapped writes while a writable handle is still open */
	if (op->ref == 1 && op != &np->openfid[OFID_RD] && vp->v_type == VREG &&
	    UBCINFOEXISTS(vp) && ISSET(np->flags, NODE_MMAPPED))
		ubc_pushdirty(vp);
	ofidrele_9p(np, op);
	return 0;
}

static int
vop_access_9p(struct vop_access_args *ap)
{
	struct vnode *vp;
	struct ucred *cred;
	node_9p *np;
	mode_t mode, bits;
	int e;

	vp = ap->a_vp;
	np = NTO9P(vp);
	cred = ap->a_cred;
	mode = ap->a_mode;
	if (ISSET(mode, VWRITE) && ISSET(vp->v_mount->mnt_flag, MNT_RDONLY) &&
	    (vp->v_type == VREG || vp->v_type == VDIR || vp->v_type == VLNK))
		return EROFS;
	if ((e = ngetattr_9p(np, 0)))
		return e;
	/* the host enforces its own permissions */
	if (cred->cr_uid == 0) {
		if (ISSET(mode, VEXEC) && vp->v_type != VDIR &&
		    (np->attr.mode & (S_IXUSR|S_IXGRP|S_IXOTH)) == 0)
			return EACCES;
		return 0;
	}
	if (ISSET(vp->v_mount->mnt_flag, MNT_UNKNOWNPERMISSIONS) ||
	    cred->cr_uid == np->nmp->uid)
		bits = np->attr.mode & S_IRWXU;
	else
		bits = (np->attr.mode & S_IRWXO) << 6;
	return (mode & ~bits) & (VREAD|VWRITE|VEXEC) ? EACCES : 0;
}

static int
vop_getattr_9p(struct vop_getattr_args *ap)
{
	struct vnode *vp;
	struct vattr *vap;
	struct ucred *cred;
	mount_9p *nmp;
	node_9p *np;
	int e;

	vp = ap->a_vp;
	vap = ap->a_vap;
	cred = ap->a_cred;
	np = NTO9P(vp);
	nmp = np->nmp;
	if ((e = ngetattr_9p(np, 0)))
		return e;
	vattr_null(vap);
	vap->va_type = vp->v_type;
	vap->va_mode = np->attr.mode & 07777;
	vap->va_nlink = np->attr.nlink > 0x7FFF ? 0x7FFF : np->attr.nlink;
	if (ISSET(vp->v_mount->mnt_flag, MNT_UNKNOWNPERMISSIONS) && cred != NOCRED) {
		/* every user owns every file */
		vap->va_uid = cred->cr_uid;
		vap->va_gid = cred->cr_gid;
	} else {
		vap->va_uid = nmp->uid;
		vap->va_gid = nmp->gid;
	}
	vap->va_fsid = vp->v_mount->mnt_stat.f_fsid.val[0];
	vap->va_fileid = np->id;
	vap->va_size = np->attr.size;
	vap->va_blocksize = iosize_9p(nmp);
	vap->va_atime = np->attr.atime;
	vap->va_mtime = np->attr.mtime;
	vap->va_ctime = np->attr.ctime;
	vap->va_gen = 0;
	vap->va_flags = 0;
	vap->va_rdev = 0;
	vap->va_bytes = np->attr.blocks * 512;
	vap->va_filerev = np->qid.vers;
	vap->va_vaflags = 0;
	if (ISAD(np)) {
		uint64_t size;

		if ((e = adsize_9p(np, &size)))
			return e;
		vap->va_size = vap->va_bytes = size;
		vap->va_mode &= 0666;
		vap->va_nlink = 1;
	}
	return 0;
}

static int
vop_setattr_9p(struct vop_setattr_args *ap)
{
	struct timespec *at, *mt;
	struct vattr *vap;
	struct vnode *vp;
	uint32_t valid, mode;
	uint64_t size;
	node_9p *np;
	int e;

	vp = ap->a_vp;
	vap = ap->a_vap;
	np = NTO9P(vp);
	if (ISAD(np)) {
		/* only the length is the AppleDouble file's own */
		if (vap->va_size == VNOVAL)
			return 0;
		if (ISSET(vp->v_mount->mnt_flag, MNT_RDONLY))
			return EROFS;
		if ((e = adtruncate_9p(np, vap->va_size)) == 0 && UBCINFOEXISTS(vp))
			ubc_setsize(vp, (off_t)vap->va_size);
		return e;
	}
	if (vap->va_type != VNON || vap->va_nlink != VNOVAL ||
	    vap->va_fsid != VNOVAL || vap->va_fileid != VNOVAL ||
	    vap->va_blocksize != VNOVAL || vap->va_rdev != VNOVAL ||
	    (int)vap->va_bytes != VNOVAL || vap->va_gen != VNOVAL)
		return EINVAL;

	valid = 0;
	mode = 0;
	size = 0;
	at = mt = NULL;
	if (vap->va_size != VNOVAL) {
		if (vp->v_type == VDIR)
			return EISDIR;
		if (vp->v_type == VREG) {
			valid |= SA_SIZE;
			size = vap->va_size;
		}
	}
	if (vap->va_mode != (mode_t)VNOVAL) {
		valid |= SA_MODE;
		mode = vap->va_mode & 07777;
	}
	if (vap->va_atime.tv_sec != VNOVAL) {
		valid |= SA_ATIME | SA_ATIME_SET;
		at = &vap->va_atime;
	}
	if (vap->va_mtime.tv_sec != VNOVAL) {
		valid |= SA_MTIME | SA_MTIME_SET;
		mt = &vap->va_mtime;
	}
	/* ownership and flags are the host's business */
	if (valid == 0)
		return 0;
	if (ISSET(vp->v_mount->mnt_flag, MNT_RDONLY))
		return EROFS;

	/* drop pages past the new EOF before the host truncates */
	if (ISSET(valid, SA_SIZE) && UBCINFOEXISTS(vp))
		ubc_setsize(vp, (off_t)size);
	e = setattr_9p(np->nmp, np->fid, valid, mode, size, at, mt);
	np->attrtime = 0;
	if (e == 0 && ISSET(valid, SA_SIZE))
		np->attr.size = size;
	return e;
}

static int
nread_9p(node_9p *np, struct uio *uio)
{
	openfid_9p *op;
	uint32_t n, l, sz;
	fid_9p fid;
	char *p;
	int e;

	if ((e = iofid_9p(np, 0, &fid, &op)))
		return e;
	sz = iosize_9p(np->nmp);
	p = malloc_9p(sz);
	if (p == NULL) {
		e = ENOMEM;
		goto out;
	}
	while (uio->uio_resid > 0) {
		n = MIN((uint32_t)uio->uio_resid, sz);
		if ((e = read_9p(np->nmp, fid, p, n, uio->uio_offset, &l)) || l == 0)
			break;
		if ((e = uiomove(p, l, uio)))
			break;
	}
	free_9p(p);
out:
	iodone_9p(np, fid, op);
	return e;
}

/* write all of buf at off */
static int
wall_9p(mount_9p *nmp, fid_9p fid, char *buf, uint32_t n, uint64_t off)
{
	uint32_t l;
	int e;

	while (n) {
		if ((e = write_9p(nmp, fid, buf, n, off, &l)))
			return e;
		if (l == 0)
			return EIO;
		buf += l;
		off += l;
		n -= l;
	}
	return 0;
}

static int
nwrite_9p(node_9p *np, struct uio *uio)
{
	openfid_9p *op;
	uint32_t n, sz;
	fid_9p fid;
	off_t off;
	char *p;
	int e;

	if ((e = iofid_9p(np, 1, &fid, &op)))
		return e;
	sz = iosize_9p(np->nmp);
	p = malloc_9p(sz);
	if (p == NULL) {
		e = ENOMEM;
		goto out;
	}
	while (uio->uio_resid > 0) {
		off = uio->uio_offset;
		n = MIN((uint32_t)uio->uio_resid, sz);
		if ((e = uiomove(p, n, uio)))
			break;
		if ((e = wall_9p(np->nmp, fid, p, n, off))) {
			/* report what reached the host */
			uio->uio_resid += uio->uio_offset - off;
			uio->uio_offset = off;
			break;
		}
	}
	free_9p(p);
out:
	iodone_9p(np, fid, op);
	return e;
}

static int
vop_read_9p(struct vop_read_args *ap)
{
	struct vnode *vp;
	struct uio *uio;
	node_9p *np;

	vp = ap->a_vp;
	uio = ap->a_uio;
	np = NTO9P(vp);
	if (ISAD(np))
		return adread_9p(np, uio);
	if (vp->v_type == VDIR)
		return EISDIR;
	if (vp->v_type != VREG)
		return EOPNOTSUPP;
	if (uio->uio_offset < 0)
		return EINVAL;
	if (uio->uio_resid == 0)
		return 0;
	/* mapped pages are newer than the host */
	if (ISSET(np->flags, NODE_MMAPPED) && UBCINFOEXISTS(vp))
		ubc_pushdirty_range(vp, uio->uio_offset, (off_t)uio->uio_resid);
	return nread_9p(np, uio);
}

static int
vop_write_9p(struct vop_write_args *ap)
{
	struct vnode *vp;
	struct uio *uio;
	node_9p *np;
	off_t off, end;
	int e, resid;

	vp = ap->a_vp;
	uio = ap->a_uio;
	np = NTO9P(vp);
	if (ISSET(vp->v_mount->mnt_flag, MNT_RDONLY))
		return EROFS;
	if (ISAD(np)) {
		if ((e = adwrite_9p(np, uio, ap->a_ioflag)) == 0 && UBCINFOEXISTS(vp))
			ubc_setsize(vp, (off_t)np->adlen);
		return e;
	}
	if (vp->v_type == VDIR)
		return EISDIR;
	if (vp->v_type != VREG)
		return EOPNOTSUPP;
	if (ISSET(ap->a_ioflag, IO_APPEND)) {
		ngetattr_9p(np, 1);
		uio->uio_offset = np->attr.size;
		if (UBCINFOEXISTS(vp) && ubc_getsize(vp) > uio->uio_offset)
			uio->uio_offset = ubc_getsize(vp);
	}
	if (uio->uio_offset < 0)
		return EINVAL;
	resid = uio->uio_resid;
	if (resid == 0)
		return 0;
	off = uio->uio_offset;
	if (ISSET(np->flags, NODE_MMAPPED) && UBCINFOEXISTS(vp))
		ubc_pushdirty_range(vp, off, (off_t)resid);

	e = nwrite_9p(np, uio);

	if (uio->uio_resid < resid) {
		end = off + (resid - uio->uio_resid);
		if (end > (off_t)np->attr.size) {
			np->attr.size = end;
			if (UBCINFOEXISTS(vp))
				ubc_setsize(vp, end);
		}
		/* mapped copies of what we wrote are stale */
		if (ISSET(np->flags, NODE_MMAPPED) && UBCINFOEXISTS(vp))
			ubc_invalidate(vp, off, (size_t)(end - off));
		e = 0;
	}
	np->attrtime = 0;
	return e;
}

static int
vop_ioctl_9p(struct vop_ioctl_args *ap)
{
	return ENOTTY;
}

static int
vop_select_9p(struct vop_select_args *ap)
{
	return 1;
}

static int
vop_mmap_9p(struct vop_mmap_args *ap)
{
	if (ISAD(NTO9P(ap->a_vp)))
		return EPERM;
	SET(NTO9P(ap->a_vp)->flags, NODE_MMAPPED);
	return 0;
}

static int
vop_fsync_9p(struct vop_fsync_args *ap)
{
	openfid_9p *op;
	struct vnode *vp;
	node_9p *np;
	fid_9p fid;
	int e;

	vp = ap->a_vp;
	if (vp->v_type != VREG)
		return 0;
	np = NTO9P(vp);
	if (ISAD(np))
		return adflush_9p(np);
	if (UBCINFOEXISTS(vp))
		ubc_pushdirty(vp);
	if (!haswriter(np))
		return 0;
	if ((e = iofid_9p(np, 1, &fid, &op)))
		return e;
	e = fsync_9p(np->nmp, fid);
	iodone_9p(np, fid, op);
	return e;
}

static int
vop_seek_9p(struct vop_seek_args *ap)
{
	return 0;
}

enum {
	RENAMED_US	= 10000000,	/* how long a rename explains a "._" delete */
};

static void
renamed_9p(mount_9p *nmp, uint64_t qpath)
{
	uint32_t i;

	i = nmp->nrenamed++ % nelem(nmp->renamed);
	nmp->renamed[i].qpath = qpath;
	nmp->renamed[i].when = now_us();
}

/*
 * Carbon renames "x" to "y", then "._x" to "._y", which fails as "x" is
 * gone, and so deletes "._y": the Finder info and fork that "y" carried
 * along are not stale, and stay.
 */
static int
justrenamed_9p(mount_9p *nmp, uint64_t qpath)
{
	uint64_t t;
	uint32_t i;

	t = now_us();
	for (i = 0; i < nelem(nmp->renamed); i++)
		if (nmp->renamed[i].qpath == qpath && nmp->renamed[i].when &&
		    t - nmp->renamed[i].when < RENAMED_US)
			return 1;
	return 0;
}

/* the host removed it: keep it out of the hash, recycle when unused */
static void
ngone_9p(node_9p *np)
{
	id_forget_9p(np->nmp, np->id);
	if (ISSET(np->flags, NODE_HASHED)) {
		LIST_REMOVE(np, next);
		CLR(np->flags, NODE_HASHED);
	}
	SET(np->flags, NODE_GONE);
	cache_purge(np->vp);
}

static int
vop_remove_9p(struct vop_remove_args *ap)
{
	struct vnode *dvp, *vp;
	struct componentname *cnp;
	node_9p *dnp;
	int e;

	dvp = ap->a_dvp;
	vp = ap->a_vp;
	cnp = ap->a_cnp;
	dnp = NTO9P(dvp);
	if (dvp == vp)
		e = EINVAL;
	else if (ISSET(cnp->cn_flags, NODELETEBUSY) &&
		 (UBCISVALID(vp) ? ubc_isinuse(vp, 1) : vp->v_usecount > 1))
		e = EBUSY;	/* Carbon delete semantics */
	else if (ISAD(NTO9P(vp))) {
		if (justrenamed_9p(dnp->nmp, adbase_9p(NTO9P(vp))))
			e = 0;
		else
			e = adremove_9p(NTO9P(vp));
		if (e == 0)
			ngone_9p(NTO9P(vp));
	} else {
		e = unlinkat_9p(dnp->nmp, dnp->fid, cnp->cn_nameptr, cnp->cn_namelen, 0);
		dnp->attrtime = 0;
		if (e == 0)
			ngone_9p(NTO9P(vp));
	}
	if (dvp == vp)
		vrele(vp);
	else {
		/* dropping the pager's reference may recycle vp: unlock first */
		VOP_UNLOCK(vp, 0, cnp->cn_proc);
		if (e == 0)
			ubc_uncache(vp);
		vrele(vp);
	}
	vput(dvp);
	freename(cnp);
	return e;
}

static int
vop_rmdir_9p(struct vop_rmdir_args *ap)
{
	struct vnode *dvp, *vp;
	struct componentname *cnp;
	node_9p *dnp;
	int e;

	dvp = ap->a_dvp;
	vp = ap->a_vp;
	cnp = ap->a_cnp;
	dnp = NTO9P(dvp);
	if (dvp == vp)
		e = EINVAL;
	else {
		e = unlinkat_9p(dnp->nmp, dnp->fid, cnp->cn_nameptr, cnp->cn_namelen,
				L_AT_REMOVEDIR);
		dnp->attrtime = 0;
		if (e == 0) {
			ngone_9p(NTO9P(vp));
			cache_purge(dvp);
		}
	}
	if (dvp == vp)
		vrele(vp);
	else
		vput(vp);
	vput(dvp);
	freename(cnp);
	return e;
}

/*
 * A rename touching an AppleDouble name: "._a" to "._b" moves a's Finder
 * info and fork to b; anything else would split them from their file.
 */
static int
adrename_9p(struct vnode *fvp, node_9p *tdnp, struct vnode *tvp, struct componentname *tcnp)
{
	node_9p *fnp;
	fid_9p fid;
	qid_9p qid;
	int e;

	fnp = NTO9P(fvp);
	if (!ISAD(fnp) || !adname_9p(tcnp->cn_nameptr, tcnp->cn_namelen))
		return EINVAL;
	if (tvp == fvp)
		return 0;
	if (tvp && ISAD(NTO9P(tvp))) {
		/* its image is about to be stale */
		adfree_9p(NTO9P(tvp));
		return admove_9p(fnp, NTO9P(tvp)->fid);
	}
	if (tvp)
		return EINVAL;
	if ((e = walk_9p(tdnp->nmp, tdnp->fid, tcnp->cn_nameptr + 2, tcnp->cn_namelen - 2,
			 &fid, &qid)))
		return e;
	e = admove_9p(fnp, fid);
	clunk_9p(tdnp->nmp, fid);
	return e;
}

static int
vop_rename_9p(struct vop_rename_args *ap)
{
	struct vnode *fdvp, *fvp, *tdvp, *tvp;
	node_9p *fdnp, *tdnp;
	int e;

	fdvp = ap->a_fdvp;
	fvp = ap->a_fvp;
	tdvp = ap->a_tdvp;
	tvp = ap->a_tvp;
	fdnp = NTO9P(fdvp);
	tdnp = NTO9P(tdvp);

	if (fvp->v_mount != tdvp->v_mount || (tvp && fvp->v_mount != tvp->v_mount))
		e = EXDEV;
	else if (tvp && fvp->v_type == VDIR && tvp->v_type != VDIR)
		e = ENOTDIR;
	else if (tvp && fvp->v_type != VDIR && tvp->v_type == VDIR)
		e = EISDIR;
	else if (ISAD(NTO9P(fvp)) || (tvp && ISAD(NTO9P(tvp))))
		e = adrename_9p(fvp, tdnp, tvp, ap->a_tcnp);
	else {
		/* a case-only rename finds the source as the target */
		e = renameat_9p(fdnp->nmp, fdnp->fid, ap->a_fcnp->cn_nameptr,
				ap->a_fcnp->cn_namelen, tdnp->fid,
				ap->a_tcnp->cn_nameptr, ap->a_tcnp->cn_namelen);
	}
	if (e == 0) {
		fdnp->attrtime = 0;
		tdnp->attrtime = 0;
		cache_purge(fvp);
		cache_purge(fdvp);
		if (fdvp != tdvp)
			cache_purge(tdvp);
		if (tvp && tvp != fvp)
			ngone_9p(NTO9P(tvp));
		if (ISAD(NTO9P(fvp)))
			ngone_9p(NTO9P(fvp));	/* its file's Finder info moved on */
		else {
			id_link_9p(tdnp->nmp, NTO9P(fvp)->id, tdnp->id,
				   ap->a_tcnp->cn_nameptr, ap->a_tcnp->cn_namelen);
			renamed_9p(tdnp->nmp, NTO9P(fvp)->qid.path);
		}
	}

	if (tdvp == tvp)
		vrele(tdvp);
	else
		vput(tdvp);
	if (tvp) {
		/* as in remove: unlock before the pager lets go of a replaced file */
		VOP_UNLOCK(tvp, 0, ap->a_tcnp->cn_proc);
		if (e == 0 && tvp != fvp)
			ubc_uncache(tvp);
		vrele(tvp);
	}
	vrele(fdvp);
	vrele(fvp);
	return e;
}

#define DIRENT_LEN(namlen) \
	((offsetof(struct dirent, d_name) + (namlen) + 1 + 3) & ~3)

static int
vop_readdir_9p(struct vop_readdir_args *ap)
{
	struct dirent de;
	struct vnode *vp;
	struct uio *uio;
	dirent_9p *dp;
	node_9p *np;
	fid_9p fid;
	qid_9p qid;
	off_t off;
	uint32_t i, nd, nlen, iounit;
	int e;

	vp = ap->a_vp;
	uio = ap->a_uio;
	np = NTO9P(vp);
	if (vp->v_type != VDIR)
		return ENOTDIR;
	if (ap->a_ncookies)
		*ap->a_ncookies = 0;
	off = uio->uio_offset;
	if (off < 0)
		return EINVAL;
	if (ap->a_eofflag)
		*ap->a_eofflag = 0;
	if (uio->uio_resid == 0)
		return 0;

	e = 0;
	if (off == 0 || np->dirents == NULL) {
		free_9p(np->dirents);
		np->dirents = NULL;
		np->ndirents = 0;
		/* a fid of our own: an open one has a position */
		if ((e = walk_9p(np->nmp, np->fid, NULL, 0, &fid, &qid)))
			return e;
		if ((e = lopen_9p(np->nmp, fid, L_O_RDONLY, &qid, &iounit)) == 0)
			e = readdirs_9p(np->nmp, fid, &np->dirents, &np->ndirents);
		clunk_9p(np->nmp, fid);
		if (e)
			return e;
	}

	/* entries 0 and 1 are "." and "..", which the server leaves out */
	dp = np->dirents;
	nd = np->ndirents + 2;
	for (i = off; i < nd; i++) {
		bzero(&de, sizeof(de));
		if (i < 2) {
			de.d_fileno = i == 0 ? np->id : id_parent_9p(np->nmp, np->id);
			de.d_type = DT_DIR;
			de.d_namlen = i + 1;
			bcopy("..", de.d_name, de.d_namlen);
		} else {
			de.d_fileno = id_get_9p(np->nmp, dp[i - 2].qid.path);
			de.d_type = dp[i - 2].type;
			nlen = strlen(dp[i - 2].name);
			de.d_namlen = MIN(nlen, sizeof(de.d_name) - 1);
			bcopy(dp[i - 2].name, de.d_name, de.d_namlen);
			id_link_9p(np->nmp, de.d_fileno, np->id, de.d_name, de.d_namlen);
		}
		de.d_reclen = DIRENT_LEN(de.d_namlen);
		if (uio->uio_resid < de.d_reclen)
			break;
		if ((e = uiomove((caddr_t)&de, de.d_reclen, uio)))
			return e;
	}
	uio->uio_offset = i;
	if (i >= nd) {
		if (ap->a_eofflag)
			*ap->a_eofflag = 1;
		free_9p(np->dirents);
		np->dirents = NULL;
		np->ndirents = 0;
	}
	return 0;
}

static int
vop_readlink_9p(struct vop_readlink_args *ap)
{
	node_9p *np;
	char *s;
	int e;

	np = NTO9P(ap->a_vp);
	if ((e = readlink_9p(np->nmp, np->fid, &s)))
		return e;
	e = uiomove(s, strlen(s), ap->a_uio);
	free_9p(s);
	return e;
}

static int
vop_inactive_9p(struct vop_inactive_args *ap)
{
	struct vnode *vp;
	node_9p *np;

	vp = ap->a_vp;
	np = NTO9P(vp);
	if (np && ISAD(np))
		adflush_9p(np);
	VOP_UNLOCK(vp, 0, ap->a_p);
	if (np && ISSET(np->flags, NODE_GONE))
		vrecycle(vp, NULL, ap->a_p);
	return 0;
}

static int
vop_reclaim_9p(struct vop_reclaim_args *ap)
{
	struct vnode *vp;
	mount_9p *nmp;
	node_9p *np;
	int i;

	vp = ap->a_vp;
	np = NTO9P(vp);
	if (np == NULL)
		return 0;
	nmp = np->nmp;
	if (ISSET(np->flags, NODE_HASHED)) {
		LIST_REMOVE(np, next);
		CLR(np->flags, NODE_HASHED);
	}
	cache_purge(vp);
	vp->v_data = NULL;
	if (ISAD(np))
		adfree_9p(np);

	for (i = 0; i < NOFIDS; i++)
		if (np->openfid[i].fid != NOFID)
			clunk_9p(nmp, np->openfid[i].fid);
	if (np->fid != NOFID)
		clunk_9p(nmp, np->fid);
	lock_free_9p(np);
	free_9p(np->dirents);
	free_9p(np);
	return 0;
}

static int
vop_lock_9p(struct vop_lock_args *ap)
{
	struct vnode *vp;

	vp = ap->a_vp;
	if (vp->v_tag == VT_NON || vp->v_data == NULL)
		return ENOENT;
	return lockmgr(&NTO9P(vp)->lock, ap->a_flags, &vp->v_interlock, ap->a_p);
}

static int
vop_unlock_9p(struct vop_unlock_args *ap)
{
	struct vnode *vp;

	vp = ap->a_vp;
	return lockmgr(&NTO9P(vp)->lock, ap->a_flags | LK_RELEASE, &vp->v_interlock, ap->a_p);
}

static int
vop_islocked_9p(struct vop_islocked_args *ap)
{
	return lockstatus(&NTO9P(ap->a_vp)->lock);
}

static int
vop_print_9p(struct vop_print_args *ap)
{
	node_9p *np;

	np = NTO9P(ap->a_vp);
	printf("9p: qid %llx fid %u\n", np->qid.path, np->fid);
	return 0;
}

static int
vop_pathconf_9p(struct vop_pathconf_args *ap)
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
	case _PC_NO_TRUNC:
		*ap->a_retval = 1;
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
vop_pagein_9p(struct vop_pagein_args *ap)
{
	struct vnode *vp;
	openfid_9p *op;
	node_9p *np;
	vm_offset_t ioaddr;
	uint32_t n, l, sz;
	off_t foff, eof;
	size_t size, done;
	fid_9p fid;
	upl_t pl;
	int e, commit;

	vp = ap->a_vp;
	np = NTO9P(vp);
	pl = ap->a_pl;
	size = ap->a_size;
	foff = ap->a_f_offset;
	commit = !ISSET(ap->a_flags, UPL_NOCOMMIT);

	eof = ubc_getsize(vp);
	if ((off_t)np->attr.size > eof)
		eof = np->attr.size;
	if (ISAD(np) || UBCINVALID(vp) || (int)size <= 0 || foff < 0 || foff >= eof ||
	    (foff & PAGE_MASK_64)) {
		if (commit)
			ubc_upl_abort_range(pl, ap->a_pl_offset, size,
					    UPL_ABORT_ERROR | UPL_ABORT_FREE_ON_EMPTY);
		return EINVAL;
	}
	if ((e = iofid_9p(np, 0, &fid, &op))) {
		if (commit)
			ubc_upl_abort_range(pl, ap->a_pl_offset, size,
					    UPL_ABORT_ERROR | UPL_ABORT_FREE_ON_EMPTY);
		return e;
	}

	ubc_upl_map(pl, &ioaddr);
	ioaddr += ap->a_pl_offset;
	sz = iosize_9p(np->nmp);
	for (done = 0; done < size; done += n) {
		n = MIN(size - done, sz);
		l = 0;
		if (foff + done < eof &&
		    (e = read_9p(np->nmp, fid, (void *)(ioaddr + done), n, foff + done, &l)))
			break;
		/* zero past EOF */
		if (l < n)
			bzero((void *)(ioaddr + done + l), n - l);
	}
	ubc_upl_unmap(pl);
	iodone_9p(np, fid, op);

	if (commit) {
		if (e)
			ubc_upl_abort_range(pl, ap->a_pl_offset, size,
					    UPL_ABORT_ERROR | UPL_ABORT_FREE_ON_EMPTY);
		else
			ubc_upl_commit_range(pl, ap->a_pl_offset, size,
					     UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
	}
	return e;
}

static int
vop_pageout_9p(struct vop_pageout_args *ap)
{
	struct vnode *vp;
	openfid_9p *op;
	node_9p *np;
	vm_offset_t ioaddr;
	off_t foff, eof;
	size_t size, xsize;
	fid_9p fid;
	upl_t pl;
	int e, commit;

	vp = ap->a_vp;
	np = NTO9P(vp);
	pl = ap->a_pl;
	size = ap->a_size;
	foff = ap->a_f_offset;
	commit = !ISSET(ap->a_flags, UPL_NOCOMMIT);

	if (ISSET(vp->v_mount->mnt_flag, MNT_RDONLY)) {
		if (commit)
			ubc_upl_abort_range(pl, ap->a_pl_offset, size, UPL_ABORT_FREE_ON_EMPTY);
		return EROFS;
	}
	eof = ubc_getsize(vp);
	if (ISAD(np) || UBCINVALID(vp) || (int)size <= 0 || foff < 0 || foff >= eof ||
	    (foff & PAGE_MASK_64)) {
		if (commit)
			ubc_upl_abort_range(pl, ap->a_pl_offset, size, UPL_ABORT_FREE_ON_EMPTY);
		return EINVAL;
	}
	xsize = size;
	if (foff + (off_t)xsize > eof)
		xsize = eof - foff;

	if ((e = iofid_9p(np, 1, &fid, &op))) {
		if (commit)
			ubc_upl_abort_range(pl, ap->a_pl_offset, size, UPL_ABORT_FREE_ON_EMPTY);
		return e;
	}
	ubc_upl_map(pl, &ioaddr);
	ioaddr += ap->a_pl_offset;
	{
		uint32_t n, sz = iosize_9p(np->nmp);
		size_t done;

		for (done = 0; done < xsize && !e; done += n) {
			n = MIN(xsize - done, sz);
			e = wall_9p(np->nmp, fid, (char *)(ioaddr + done), n, foff + done);
		}
	}
	ubc_upl_unmap(pl);
	iodone_9p(np, fid, op);

	if (e == 0 && foff + (off_t)xsize > (off_t)np->attr.size)
		np->attr.size = foff + xsize;
	np->attrtime = 0;
	if (commit) {
		if (e)
			ubc_upl_abort_range(pl, ap->a_pl_offset, size, UPL_ABORT_FREE_ON_EMPTY);
		else
			ubc_upl_commit_range(pl, ap->a_pl_offset, size,
					     UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
	}
	return e;
}

static int
vop_blktooff_9p(struct vop_blktooff_args *ap)
{
	*ap->a_offset = (off_t)ap->a_lblkno << PAGE_SHIFT;
	return 0;
}

static int
vop_offtoblk_9p(struct vop_offtoblk_args *ap)
{
	*ap->a_lblkno = ap->a_offset >> PAGE_SHIFT;
	return 0;
}

static struct vnodeopv_entry_desc vnode_entry_desc_9p[] = {
	{ &vop_default_desc,	(vop_t *)vn_default_error },
	{ &vop_lookup_desc,	(vop_t *)vop_lookup_9p },
	{ &vop_create_desc,	(vop_t *)vop_create_9p },
	{ &vop_mknod_desc,	(vop_t *)err_mknod },
	{ &vop_open_desc,	(vop_t *)vop_open_9p },
	{ &vop_close_desc,	(vop_t *)vop_close_9p },
	{ &vop_access_desc,	(vop_t *)vop_access_9p },
	{ &vop_getattr_desc,	(vop_t *)vop_getattr_9p },
	{ &vop_setattr_desc,	(vop_t *)vop_setattr_9p },
	{ &vop_getattrlist_desc, (vop_t *)vop_getattrlist_9p },
	{ &vop_setattrlist_desc, (vop_t *)vop_setattrlist_9p },
	{ &vop_read_desc,	(vop_t *)vop_read_9p },
	{ &vop_write_desc,	(vop_t *)vop_write_9p },
	{ &vop_lease_desc,	(vop_t *)nop_lease },
	{ &vop_ioctl_desc,	(vop_t *)vop_ioctl_9p },
	{ &vop_select_desc,	(vop_t *)vop_select_9p },
	{ &vop_revoke_desc,	(vop_t *)vop_revoke },
	{ &vop_mmap_desc,	(vop_t *)vop_mmap_9p },
	{ &vop_fsync_desc,	(vop_t *)vop_fsync_9p },
	{ &vop_seek_desc,	(vop_t *)vop_seek_9p },
	{ &vop_remove_desc,	(vop_t *)vop_remove_9p },
	{ &vop_link_desc,	(vop_t *)err_link },
	{ &vop_rename_desc,	(vop_t *)vop_rename_9p },
	{ &vop_mkdir_desc,	(vop_t *)vop_mkdir_9p },
	{ &vop_rmdir_desc,	(vop_t *)vop_rmdir_9p },
	{ &vop_symlink_desc,	(vop_t *)vop_symlink_9p },
	{ &vop_readdir_desc,	(vop_t *)vop_readdir_9p },
	{ &vop_readlink_desc,	(vop_t *)vop_readlink_9p },
	{ &vop_abortop_desc,	(vop_t *)nop_abortop },
	{ &vop_inactive_desc,	(vop_t *)vop_inactive_9p },
	{ &vop_reclaim_desc,	(vop_t *)vop_reclaim_9p },
	{ &vop_lock_desc,	(vop_t *)vop_lock_9p },
	{ &vop_unlock_desc,	(vop_t *)vop_unlock_9p },
	{ &vop_islocked_desc,	(vop_t *)vop_islocked_9p },
	{ &vop_print_desc,	(vop_t *)vop_print_9p },
	{ &vop_pathconf_desc,	(vop_t *)vop_pathconf_9p },
	{ &vop_advlock_desc,	(vop_t *)vnop_advlock_9p },
	{ &vop_pagein_desc,	(vop_t *)vop_pagein_9p },
	{ &vop_pageout_desc,	(vop_t *)vop_pageout_9p },
	{ &vop_blktooff_desc,	(vop_t *)vop_blktooff_9p },
	{ &vop_offtoblk_desc,	(vop_t *)vop_offtoblk_9p },
	{ NULL, NULL }
};

__private_extern__ struct vnodeopv_desc vnodeopv_desc_9p = {
	&vnode_op_9p,
	vnode_entry_desc_9p
};
