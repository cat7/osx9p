#include "9p.h"

#ifndef VOL_CAP_INT_EXTENDED_ATTR
#define VOL_CAP_INT_EXTENDED_ATTR	0x00004000
#endif

lck_grp_t *lck_grp_9p;

__private_extern__ void*
malloc_9p(size_t n)
{
	void *p;

	MALLOC(p, void*, n, M_TEMP, M_WAITOK|M_ZERO);
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
	if (nmp->lck)
		lck_mtx_free(nmp->lck, lck_grp_9p);
	if (nmp->nodelck)
		lck_mtx_free(nmp->nodelck, lck_grp_9p);
	free_9p(nmp);
}

static int
fsidused_9p(mount_t mp, void *arg)
{
	int32_t *v;

	v = arg;
	if (vfs_statfs(mp)->f_fsid.val[0] == v[0])
		v[1] = 1;
	return VFS_RETURNED;
}

/*
 * The same fsid on every mount of a tag: Carbon on 10.4 caches a volfs
 * volume's fsid by mount point and keeps it after an unmount, and volfs
 * keeps the whole fsid, while the type number can change on a kext reload.
 */
static void
stablefsid_9p(mount_t mp, const char *tag)
{
	int32_t v[2];
	uint32_t h;
	int i;

	for (h = 5381; *tag; tag++)
		h = h * 33 + (u_char)*tag;
	for (i = 0; i < 64; i++, h++) {
		v[0] = 0x7F000000 | (h & 0x00FFFFFF);
		v[1] = 0;
		vfs_iterate(0, fsidused_9p, v);
		if (v[1] == 0) {
			vfs_statfs(mp)->f_fsid.val[0] = v[0];
			vfs_statfs(mp)->f_fsid.val[1] = 0x39500000;
			return;
		}
	}
}

static int
vfs_mount_9p(mount_t mp, vnode_t devvp, user_addr_t data, vfs_context_t ctx)
{
	struct vfsstatfs *sp;
	kauth_cred_t cred;
	args_9p args;
	mount_9p *nmp;
	fid_9p fid;
	qid_9p qid;
	int e;

	(void)devvp;
	fid = NOFID;
	if (vfs_isupdate(mp))
		return ENOTSUP;
	if ((e = copyin(data, &args, sizeof(args))))
		return e;
	args.tag[TAGLEN-1] = 0;
	args.volume[VOLNAMELEN-1] = 0;

	nmp = malloc_9p(sizeof(*nmp));
	if (nmp == NULL)
		return ENOMEM;
	e = ENOMEM;
	nmp->mp = mp;
	nmp->lck = lck_mtx_alloc_init(lck_grp_9p, LCK_ATTR_NULL);
	nmp->nodelck = lck_mtx_alloc_init(lck_grp_9p, LCK_ATTR_NULL);
	nmp->node = hashinit(desiredvnodes, M_TEMP, &nmp->nodelen);
	if (nmp->lck == NULL || nmp->nodelck == NULL || nmp->node == NULL)
		goto error;

	strlcpy_9p(nmp->tag, args.tag, sizeof(nmp->tag));
	strlcpy_9p(nmp->volume, args.volume[0] ? args.volume : args.tag, sizeof(nmp->volume));
	nmp->flags = args.flags;

	cred = vfs_context_ucred(ctx);
	if (IS_VALID_CRED(cred)) {
		nmp->uid = kauth_cred_getuid(cred);
		nmp->gid = kauth_cred_getgid(cred);
	} else {
		nmp->uid = KAUTH_UID_NONE;
		nmp->gid = KAUTH_GID_NONE;
	}

	nmp->chan = v9p_chan_open(nmp->tag);
	if (nmp->chan == NULL) {
		e = ENXIO;
		goto error;
	}
	nmp->msize = v9p_chan_msize(nmp->chan);

	vfs_getnewfsid(mp);
	if (ISSET(nmp->flags, FLAG_VOLFS))
		stablefsid_9p(mp, nmp->tag);
	vfs_setfsprivate(mp, nmp);

	if ((e = version_9p(nmp)))
		goto error;
	if ((e = attach_9p(nmp, &fid, &qid)))
		goto error;
	if ((e = ids_init_9p(nmp, qid.path)))
		goto error;
	if ((e = nget_9p(nmp, fid, qid, NULL, &nmp->root, NULL, ctx)))
		goto error;
	fid = NOFID;	/* owned by the root node now */

	nunlock_9p(NTO9P(nmp->root));
	e = vnode_ref(nmp->root);
	vnode_put(nmp->root);
	if (e)
		goto error;

	if (ISSET(nmp->flags, FLAG_VOLFS))
		vfs_setflags(mp, MNT_DOVOLFS);
	vfs_setauthopaque(mp);
	vfs_clearauthopaqueaccess(mp);

	sp = vfs_statfs(mp);
	strlcpy_9p(sp->f_mntfromname, nmp->tag, MNAMELEN);
	sp->f_bsize = PAGE_SIZE;
	sp->f_iosize = iosize_9p(nmp);
	return 0;

error:
	if (fid != NOFID)
		clunk_9p(nmp, fid);
	freemount_9p(nmp);
	vfs_setfsprivate(mp, NULL);
	return e;
}

static int
vfs_unmount_9p(mount_t mp, int mntflags, vfs_context_t ctx)
{
	mount_9p *nmp;
	vnode_t vp;
	int e, flags;

	(void)ctx;
	nmp = MTO9P(mp);
	flags = 0;
	if (ISSET(mntflags, MNT_FORCE))
		SET(flags, FORCECLOSE);

	vp = nmp->root;
	if ((e = vflush(mp, vp, flags)))
		return e;
	if (vnode_isinuse(vp, 1) && !ISSET(flags, FORCECLOSE))
		return EBUSY;

	OSBitOrAtomic(F_UNMOUNTING, (UInt32 *)&nmp->flags);
	clunk_9p(nmp, NTO9P(vp)->fid);
	vnode_rele(vp);
	vflush(mp, NULL, FORCECLOSE);
	vfs_setfsprivate(mp, NULL);
	freemount_9p(nmp);
	return 0;
}

static int
vfs_root_9p(mount_t mp, vnode_t *vpp, vfs_context_t ctx)
{
	vnode_t vp;
	int e;

	(void)ctx;
	vp = MTO9P(mp)->root;
	*vpp = NULL;
	if ((e = vnode_get(vp)))
		return e;
	*vpp = vp;
	return 0;
}

static int
vfs_getattr_9p(mount_t mp, struct vfs_attr *ap, vfs_context_t ctx)
{
	struct vfsstatfs *sp;
	fsstat_9p st;
	mount_9p *nmp;
	uint64_t scale;

	(void)ctx;
	nmp = MTO9P(mp);
	sp = vfs_statfs(mp);
	VFSATTR_RETURN(ap, f_bsize, sp->f_bsize);
	VFSATTR_RETURN(ap, f_iosize, sp->f_iosize);

	if (statfs_9p(nmp, NTO9P(nmp->root)->fid, &st) == 0 && st.bsize) {
		/* report in f_bsize units */
		scale = st.bsize;
		VFSATTR_RETURN(ap, f_blocks, st.blocks * scale / sp->f_bsize);
		VFSATTR_RETURN(ap, f_bfree, st.bfree * scale / sp->f_bsize);
		VFSATTR_RETURN(ap, f_bavail, st.bavail * scale / sp->f_bsize);
		VFSATTR_RETURN(ap, f_bused, (st.blocks - st.bfree) * scale / sp->f_bsize);
		VFSATTR_RETURN(ap, f_files, st.files);
		VFSATTR_RETURN(ap, f_ffree, st.ffree);
	}

	if (VFSATTR_IS_ACTIVE(ap, f_capabilities)) {
		vol_capabilities_attr_t *c = &ap->f_capabilities;

		c->valid[VOL_CAPABILITIES_FORMAT] =
			VOL_CAP_FMT_PERSISTENTOBJECTIDS |
			VOL_CAP_FMT_SYMBOLICLINKS |
			VOL_CAP_FMT_HARDLINKS |
			VOL_CAP_FMT_JOURNAL |
			VOL_CAP_FMT_JOURNAL_ACTIVE |
			VOL_CAP_FMT_NO_ROOT_TIMES |
			VOL_CAP_FMT_SPARSE_FILES |
			VOL_CAP_FMT_ZERO_RUNS |
			VOL_CAP_FMT_CASE_SENSITIVE |
			VOL_CAP_FMT_CASE_PRESERVING |
			VOL_CAP_FMT_FAST_STATFS |
			VOL_CAP_FMT_2TB_FILESIZE;
		c->capabilities[VOL_CAPABILITIES_FORMAT] =
			VOL_CAP_FMT_SYMBOLICLINKS |
			VOL_CAP_FMT_CASE_PRESERVING |
			VOL_CAP_FMT_2TB_FILESIZE;
		c->valid[VOL_CAPABILITIES_INTERFACES] =
			VOL_CAP_INT_SEARCHFS |
			VOL_CAP_INT_ATTRLIST |
			VOL_CAP_INT_NFSEXPORT |
			VOL_CAP_INT_READDIRATTR |
			VOL_CAP_INT_EXCHANGEDATA |
			VOL_CAP_INT_COPYFILE |
			VOL_CAP_INT_ALLOCATE |
			VOL_CAP_INT_VOL_RENAME |
			VOL_CAP_INT_ADVLOCK |
			VOL_CAP_INT_FLOCK |
			VOL_CAP_INT_EXTENDED_SECURITY |
			VOL_CAP_INT_USERACCESS |
			VOL_CAP_INT_EXTENDED_ATTR;
		c->capabilities[VOL_CAPABILITIES_INTERFACES] =
			VOL_CAP_INT_ATTRLIST |
			VOL_CAP_INT_ADVLOCK |
			VOL_CAP_INT_FLOCK |
			VOL_CAP_INT_EXTENDED_ATTR;
		c->valid[VOL_CAPABILITIES_RESERVED1] = 0;
		c->valid[VOL_CAPABILITIES_RESERVED2] = 0;
		c->capabilities[VOL_CAPABILITIES_RESERVED1] = 0;
		c->capabilities[VOL_CAPABILITIES_RESERVED2] = 0;
		VFSATTR_SET_SUPPORTED(ap, f_capabilities);
	}

	if (VFSATTR_IS_ACTIVE(ap, f_attributes)) {
		vol_attributes_attr_t *a = &ap->f_attributes;

		a->validattr.commonattr = a->nativeattr.commonattr =
			ATTR_CMN_NAME |
			ATTR_CMN_DEVID |
			ATTR_CMN_FSID |
			ATTR_CMN_OBJTYPE |
			ATTR_CMN_OBJTAG |
			ATTR_CMN_OBJID |
			ATTR_CMN_OBJPERMANENTID |
			ATTR_CMN_PAROBJID |
			ATTR_CMN_CRTIME |
			ATTR_CMN_MODTIME |
			ATTR_CMN_CHGTIME |
			ATTR_CMN_ACCTIME |
			ATTR_CMN_FNDRINFO |
			ATTR_CMN_OWNERID |
			ATTR_CMN_GRPID |
			ATTR_CMN_ACCESSMASK |
			ATTR_CMN_FLAGS |
			ATTR_CMN_USERACCESS;
		a->validattr.volattr = a->nativeattr.volattr =
			ATTR_VOL_FSTYPE |
			ATTR_VOL_SIGNATURE |
			ATTR_VOL_SIZE |
			ATTR_VOL_SPACEFREE |
			ATTR_VOL_SPACEAVAIL |
			ATTR_VOL_MINALLOCATION |
			ATTR_VOL_ALLOCATIONCLUMP |
			ATTR_VOL_IOBLOCKSIZE |
			ATTR_VOL_MOUNTPOINT |
			ATTR_VOL_NAME |
			ATTR_VOL_MOUNTFLAGS |
			ATTR_VOL_MOUNTEDDEVICE |
			ATTR_VOL_CAPABILITIES |
			ATTR_VOL_ATTRIBUTES |
			ATTR_VOL_INFO;
		a->validattr.dirattr = a->nativeattr.dirattr =
			ATTR_DIR_LINKCOUNT |
			ATTR_DIR_MOUNTSTATUS;
		a->validattr.fileattr = a->nativeattr.fileattr =
			ATTR_FILE_LINKCOUNT |
			ATTR_FILE_TOTALSIZE |
			ATTR_FILE_ALLOCSIZE |
			ATTR_FILE_IOBLOCKSIZE |
			ATTR_FILE_DATALENGTH |
			ATTR_FILE_DATAALLOCSIZE |
			ATTR_FILE_RSRCLENGTH |
			ATTR_FILE_RSRCALLOCSIZE;
		a->validattr.forkattr = a->nativeattr.forkattr = 0;
		VFSATTR_SET_SUPPORTED(ap, f_attributes);
	}

	if (VFSATTR_IS_ACTIVE(ap, f_vol_name)) {
		strlcpy_9p(ap->f_vol_name, nmp->volume, MAXPATHLEN);
		VFSATTR_SET_SUPPORTED(ap, f_vol_name);
	}
	return 0;
}

static int
vfs_sync_9p(mount_t mp, int waitfor, vfs_context_t ctx)
{
	(void)mp;
	(void)waitfor;
	(void)ctx;
	return 0;
}

static int
vfs_vget_9p(mount_t mp, ino64_t ino, vnode_t *vpp, vfs_context_t ctx)
{
	*vpp = NULL;
	if (ino > 0xFFFFFFFFULL)
		return ENOENT;
	return id_vget_9p(MTO9P(mp), (uint32_t)ino, vpp, ctx, 0);
}

static struct vfsops vfsops_9p = {
	.vfs_mount	= vfs_mount_9p,
	.vfs_unmount	= vfs_unmount_9p,
	.vfs_root	= vfs_root_9p,
	.vfs_getattr	= vfs_getattr_9p,
	.vfs_sync	= vfs_sync_9p,
	.vfs_vget	= vfs_vget_9p,
};

static struct vnodeopv_desc *vnodeopv_desc_9p_list[] = {
	&vnodeopv_desc_9p
};

static struct vfs_fsentry vfs_fsentry_9p = {
	.vfe_vfsops	= &vfsops_9p,
	.vfe_vopcnt	= nelem(vnodeopv_desc_9p_list),
	.vfe_opvdescs	= vnodeopv_desc_9p_list,
	.vfe_fsname	= VFS9PNAME,
	.vfe_flags	= VFS_TBLTHREADSAFE | VFS_TBLFSNODELOCK | VFS_TBLNOTYPENUM |
			  VFS_TBLLOCALVOL | VFS_TBL64BITREADY,
};

static vfstable_t vfstable_9p;

__private_extern__ kern_return_t
osx9p_start(kmod_info_t *ki, void *d)
{
	(void)ki;
	(void)d;
	lck_grp_9p = lck_grp_alloc_init(VFS9PNAME, LCK_GRP_ATTR_NULL);
	lock_init_9p();
	v9p_transport_init();
	if (vfs_fsadd(&vfs_fsentry_9p, &vfstable_9p)) {
		v9p_transport_fini();
		lock_fini_9p();
		lck_grp_free(lck_grp_9p);
		return KERN_FAILURE;
	}
	return KERN_SUCCESS;
}

__private_extern__ kern_return_t
osx9p_stop(kmod_info_t *ki, void *d)
{
	(void)ki;
	(void)d;
	if (vfs_fsremove(vfstable_9p))
		return KERN_FAILURE;
	vfstable_9p = NULL;
	v9p_transport_fini();
	lock_fini_9p();
	lck_grp_free(lck_grp_9p);
	return KERN_SUCCESS;
}
