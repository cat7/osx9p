/*
 * Mount operations for the Mac OS X 10.3 VFS. A loadable filesystem
 * builds its vnode operations vector itself and enters the type table
 * with vfsconf_add, under the kernel funnel.
 */
#include "9p.h"

extern int vfs_opv_numops;

__private_extern__ void*
malloc_9p(size_t n)
{
	void *p;

	MALLOC(p, void*, n, M_TEMP, M_WAITOK);
	if (p)
		bzero(p, n);
	return p;
}

__private_extern__ void
free_9p(void *p)
{
	if (p == NULL)
		return;
	FREE(p, M_TEMP);
}

static void
strlcpy_9p(char *d, const char *s, size_t n)
{
	size_t l;

	l = strlen(s);
	if (l >= n)
		l = n - 1;
	bcopy(s, d, l);
	d[l] = 0;
}

static void
freemount_9p(mount_9p *nmp)
{
	if (nmp == NULL)
		return;
	if (nmp->chan)
		v9p_chan_close(nmp->chan);
	ids_fini_9p(nmp);
	if (nmp->node)
		FREE(nmp->node, M_TEMP);
	free_9p(nmp);
}

static int
vfs_statfs_9p(struct mount *mp, struct statfs *sbp, struct proc *p)
{
	mount_9p *nmp;
	fsstat_9p st;
	uint64_t bytes, blocks, bfree, bavail;
	int shift;

	nmp = MTO9P(mp);
	shift = PAGE_SHIFT;
	blocks = bfree = bavail = 0;
	if (statfs_9p(nmp, NTO9P(nmp->root)->fid, &st) == 0 && st.bsize) {
		/* statfs counts in longs: grow the block until they fit */
		bytes = st.blocks * st.bsize;
		while ((bytes >> shift) > 0x7FFFFFFFULL)
			shift++;
		blocks = bytes >> shift;
		bfree = (st.bfree * st.bsize) >> shift;
		bavail = (st.bavail * st.bsize) >> shift;
		sbp->f_files = st.files > 0x7FFFFFFFULL ? 0x7FFFFFFF : (long)st.files;
		sbp->f_ffree = st.ffree > 0x7FFFFFFFULL ? 0x7FFFFFFF : (long)st.ffree;
	} else {
		sbp->f_files = 0;
		sbp->f_ffree = 0;
	}
	sbp->f_bsize = 1 << shift;
	sbp->f_iosize = iosize_9p(nmp);
	sbp->f_blocks = blocks;
	sbp->f_bfree = bfree;
	sbp->f_bavail = bavail;
	sbp->f_type = mp->mnt_vfc->vfc_typenum;
	sbp->f_owner = nmp->uid;
	if (sbp != &mp->mnt_stat) {
		sbp->f_fsid = mp->mnt_stat.f_fsid;
		bcopy(mp->mnt_stat.f_mntonname, sbp->f_mntonname, MNAMELEN);
		bcopy(mp->mnt_stat.f_mntfromname, sbp->f_mntfromname, MNAMELEN);
	}
	return 0;
}

/*
 * The same fsid on every mount of a tag: Carbon caches a volfs volume's
 * fsid by mount point, and the type number changes on a kext reload.
 */
static void
stablefsid_9p(struct mount *mp, const char *tag)
{
	fsid_t fsid;
	uint32_t h;
	int i;

	for (h = 5381; *tag; tag++)
		h = h * 33 + (u_char)*tag;
	for (i = 0; i < 64; i++, h++) {
		fsid.val[0] = 0x7F000000 | (h & 0x00FFFFFF);
		fsid.val[1] = 0x39500000;
		if (vfs_getvfs(&fsid) == NULL) {
			mp->mnt_stat.f_fsid = fsid;
			return;
		}
	}
}

static int
vfs_mount_9p(struct mount *mp, char *path, caddr_t data, struct nameidata *ndp,
	     struct proc *p)
{
	struct vnode *vp;
	mount_9p *nmp;
	args_9p args;
	fid_9p fid;
	qid_9p qid;
	size_t len;
	int e;

	fid = NOFID;
	if ((e = copyin(data, (caddr_t)&args, sizeof(args))))
		return e;
	if (ISSET(mp->mnt_flag, MNT_UPDATE)) {
		/* an update only sets or clears MNT_LOCAL */
		if (MTO9P(mp) == NULL)
			return EINVAL;
		if (ISSET(args.flags, FLAG_NOTLOCAL))
			CLR(mp->mnt_flag, MNT_LOCAL);
		else
			SET(mp->mnt_flag, MNT_LOCAL);
		return 0;
	}
	args.tag[TAGLEN-1] = 0;
	args.volume[VOLNAMELEN-1] = 0;

	nmp = malloc_9p(sizeof(*nmp));
	if (nmp == NULL)
		return ENOMEM;
	nmp->mp = mp;
	nmp->node = hashinit(desiredvnodes, M_TEMP, &nmp->nodelen);
	if (nmp->node == NULL) {
		e = ENOMEM;
		goto error;
	}
	strlcpy_9p(nmp->tag, args.tag, sizeof(nmp->tag));
	strlcpy_9p(nmp->volume, args.volume[0] ? args.volume : args.tag, sizeof(nmp->volume));
	nmp->flags = args.flags;
	nmp->uid = p->p_ucred->cr_uid;
	nmp->gid = p->p_ucred->cr_gid;

	nmp->chan = v9p_chan_open(nmp->tag);
	if (nmp->chan == NULL) {
		e = ENXIO;
		goto error;
	}
	nmp->msize = v9p_chan_msize(nmp->chan);
	mp->mnt_data = (qaddr_t)nmp;
	vfs_getnewfsid(mp);
	if (ISSET(nmp->flags, FLAG_VOLFS))
		stablefsid_9p(mp, nmp->tag);

	if ((e = version_9p(nmp)))
		goto error;
	if ((e = attach_9p(nmp, &fid, &qid)))
		goto error;
	nmp->rootqpath = qid.path;
	if ((e = ids_init_9p(nmp, qid.path)))
		goto error;
	if ((e = nget_9p(nmp, fid, qid, 1, &vp, p)))
		goto error;
	fid = NOFID;	/* owned by the root node now */
	nmp->root = vp;
	VOP_UNLOCK(vp, 0, p);	/* the mount keeps the reference */

	(void)copyinstr(path, mp->mnt_stat.f_mntonname, MNAMELEN - 1, &len);
	bzero(mp->mnt_stat.f_mntonname + len, MNAMELEN - len);
	bzero(mp->mnt_stat.f_mntfromname, MNAMELEN);
	/* Carbon takes the part after the last '/' as the BSD name */
	strlcpy_9p(mp->mnt_stat.f_mntfromname, MNTFROM9P, MNAMELEN);
	strlcpy_9p(mp->mnt_stat.f_mntfromname + sizeof(MNTFROM9P) - 1, nmp->tag,
		   MNAMELEN - sizeof(MNTFROM9P) + 1);
	if (ISSET(nmp->flags, FLAG_VOLFS))
		SET(mp->mnt_flag, MNT_DOVOLFS);
	if (ISSET(nmp->flags, FLAG_NOTLOCAL))
		CLR(mp->mnt_flag, MNT_LOCAL);
	else
		SET(mp->mnt_flag, MNT_LOCAL);
	(void)vfs_statfs_9p(mp, &mp->mnt_stat, p);
	return 0;

error:
	if (fid != NOFID)
		clunk_9p(nmp, fid);
	mp->mnt_data = NULL;
	freemount_9p(nmp);
	return e;
}

static int
vfs_start_9p(struct mount *mp, int flags, struct proc *p)
{
	return 0;
}

static int
vfs_unmount_9p(struct mount *mp, int mntflags, struct proc *p)
{
	struct vnode *vp;
	mount_9p *nmp;
	int e, flags;

	nmp = MTO9P(mp);
	vp = nmp->root;
	flags = 0;
	if (ISSET(mntflags, MNT_FORCE))
		flags |= FORCECLOSE;
	if ((e = vflush(mp, vp, flags)))
		return e;
	if (vp->v_usecount > 1 && !ISSET(flags, FORCECLOSE))
		return EBUSY;
	/* the root's reclaim clunks its fid while the channel is open */
	vrele(vp);
	vgone(vp);
	mp->mnt_data = NULL;
	freemount_9p(nmp);
	return 0;
}

static int
vfs_root_9p(struct mount *mp, struct vnode **vpp)
{
	struct vnode *vp;
	int e;

	vp = MTO9P(mp)->root;
	while ((e = vget(vp, LK_EXCLUSIVE, current_proc())))
		if (e != ENOENT)
			return e;
	*vpp = vp;
	return 0;
}

static int
vfs_quotactl_9p(struct mount *mp, int cmds, uid_t uid, caddr_t arg, struct proc *p)
{
	return EOPNOTSUPP;
}

static int
vfs_sync_9p(struct mount *mp, int waitfor, struct ucred *cred, struct proc *p)
{
	return 0;
}

/* by file ID, for volfs */
static int
vfs_vget_9p(struct mount *mp, void *ino, struct vnode **vpp)
{
	*vpp = NULL;
	return id_vget_9p(MTO9P(mp), *(uint32_t *)ino, vpp, current_proc(), 0);
}

static int
vfs_fhtovp_9p(struct mount *mp, struct fid *fhp, struct mbuf *nam,
	      struct vnode **vpp, int *exflagsp, struct ucred **credanonp)
{
	return EOPNOTSUPP;
}

static int
vfs_vptofh_9p(struct vnode *vp, struct fid *fhp)
{
	return EOPNOTSUPP;
}

static int
vfs_init_9p(struct vfsconf *vfsp)
{
	return 0;
}

static int
vfs_sysctl_9p(int *name, u_int namelen, void *oldp, size_t *oldlenp,
	      void *newp, size_t newlen, struct proc *p)
{
	return EOPNOTSUPP;
}

static struct vfsops vfsops_9p = {
	vfs_mount_9p,
	vfs_start_9p,
	vfs_unmount_9p,
	vfs_root_9p,
	vfs_quotactl_9p,
	vfs_statfs_9p,
	vfs_sync_9p,
	vfs_vget_9p,
	vfs_fhtovp_9p,
	vfs_vptofh_9p,
	vfs_init_9p,
	vfs_sysctl_9p,
};

/* fill the vnode operations vector, defaults where we have no entry */
static int
opvinit_9p(void)
{
	struct vnodeopv_entry_desc *e;
	int (**v)(void *);
	int i;

	MALLOC(v, int (**)(void *), vfs_opv_numops * sizeof(*v), M_TEMP, M_WAITOK);
	if (v == NULL)
		return ENOMEM;
	bzero(v, vfs_opv_numops * sizeof(*v));
	for (e = vnodeopv_desc_9p.opv_desc_ops; e->opve_op; e++) {
		if (e->opve_op->vdesc_offset == 0 &&
		    e->opve_op->vdesc_offset != VOFFSET(vop_default)) {
			FREE(v, M_TEMP);
			return EINVAL;
		}
		v[e->opve_op->vdesc_offset] = e->opve_impl;
	}
	for (i = 0; i < vfs_opv_numops; i++)
		if (v[i] == NULL)
			v[i] = v[VOFFSET(vop_default)];
	*vnodeopv_desc_9p.opv_desc_vector_p = v;
	return 0;
}

static struct vfsconf *
findvfsconf_9p(void)
{
	struct vfsconf *vfsp;

	for (vfsp = vfsconf; vfsp; vfsp = vfsp->vfc_next)
		if (strcmp(vfsp->vfc_name, VFS9PNAME) == 0)
			return vfsp;
	return NULL;
}

__private_extern__ kern_return_t
osx9p_start(kmod_info_t *ki, void *d)
{
	struct vfsconf vfc;
	boolean_t funnel;
	kern_return_t r;

	funnel = thread_funnel_set(kernel_flock, TRUE);
	r = KERN_FAILURE;
	if (findvfsconf_9p() || opvinit_9p())
		goto out;
	lock_init_9p();
	v9p_transport_init();
	bzero(&vfc, sizeof(vfc));
	vfc.vfc_vfsops = &vfsops_9p;
	strncpy(vfc.vfc_name, VFS9PNAME, MFSNAMELEN);
	vfc.vfc_typenum = maxvfsconf++;
	if (vfsconf_add(&vfc)) {
		v9p_transport_fini();
		lock_fini_9p();
		FREE(vnode_op_9p, M_TEMP);
		vnode_op_9p = NULL;
		goto out;
	}
	r = KERN_SUCCESS;
out:
	thread_funnel_set(kernel_flock, funnel);
	return r;
}

__private_extern__ kern_return_t
osx9p_stop(kmod_info_t *ki, void *d)
{
	struct vfsconf *vfsp;
	boolean_t funnel;
	kern_return_t r;

	funnel = thread_funnel_set(kernel_flock, TRUE);
	r = KERN_FAILURE;
	vfsp = findvfsconf_9p();
	if (vfsp && vfsp->vfc_refcount > 0)
		goto out;
	if (vfsp && vfsconf_del(VFS9PNAME))
		goto out;
	v9p_transport_fini();
	lock_fini_9p();
	if (vnode_op_9p)
		FREE(vnode_op_9p, M_TEMP);
	vnode_op_9p = NULL;
	r = KERN_SUCCESS;
out:
	thread_funnel_set(kernel_flock, funnel);
	return r;
}
