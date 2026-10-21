/*
 * getattrlist and setattrlist for 10.3, where each filesystem packs the
 * attributes itself. Carbon uses them (through volfs) for every volume
 * with MNT_DOVOLFS. Finder info lives in the host's com.apple.FinderInfo
 * extended attribute, as on 10.4 and later.
 */
#include "9p.h"
#include <sys/attr.h>

#define FINDERINFO	XATTR_FINFO9P
#define RSRCFORK	XATTR_RSRC9P

#ifndef DIR_MNTSTATUS_MNTPOINT
#define DIR_MNTSTATUS_MNTPOINT	0x00000001
#endif

/* fixed sizes, by bit, in bit order */
static const uint8_t cmnsize[] = {
	8, 4, 8, 4, 4, 8, 8, 8, 4, 8, 8, 8, 8, 8, 32, 4, 4, 4, 4, 4, 8, 4,
};
static const uint8_t volsize[] = {
	4, 4, 8, 8, 8, 8, 8, 4, 4, 4, 4, 4, 8, 8, 4, 8, 8,
	sizeof(vol_capabilities_attr_t),
};
static const uint8_t dirsize[] = { 4, 4, 4 };
static const uint8_t filesize[] = {
	4, 8, 8, 4, 4, 4, 4, 4, 8, 8, 8, sizeof(extentrecord), 8, 8, sizeof(extentrecord),
};

static int
groupsize(attrgroup_t a, const uint8_t *size, int n)
{
	int i, s;

	for (i = s = 0; i < n; i++)
		if (a & (1U << i))
			s += size[i];
	return s;
}

static int
fixedsize(struct attrlist *al)
{
	int s;

	s = groupsize(al->commonattr, cmnsize, nelem(cmnsize));
	s += groupsize(al->volattr, volsize, nelem(volsize));
	if (al->volattr & ATTR_VOL_ATTRIBUTES)
		s += sizeof(vol_attributes_attr_t);
	s += groupsize(al->dirattr, dirsize, nelem(dirsize));
	s += groupsize(al->fileattr, filesize, nelem(filesize));
	return s;
}

typedef struct {
	char *fix;	/* next fixed-size attribute */
	char *var;	/* next variable-length datum */
} pack_9p;

static void
put(pack_9p *pk, const void *v, int n)
{
	bcopy(v, pk->fix, n);
	pk->fix += n;
}

static void
put32(pack_9p *pk, uint32_t v)
{
	put(pk, &v, 4);
}

static void
put64(pack_9p *pk, uint64_t v)
{
	put(pk, &v, 8);
}

static void
putts(pack_9p *pk, struct timespec ts)
{
	put32(pk, ts.tv_sec);
	put32(pk, ts.tv_nsec);
}

static void
putid(pack_9p *pk, uint32_t id)
{
	put32(pk, id);
	put32(pk, 0);
}

/* an attrreference to a copy of s, NUL included, padded to 4 */
static void
putstr(pack_9p *pk, const char *s)
{
	struct attrreference r;
	int n;

	n = strlen(s) + 1;
	r.attr_dataoffset = pk->var - pk->fix;
	r.attr_length = n;
	bcopy(s, pk->var, n);
	pk->var += (n + 3) & ~3;
	put(pk, &r, sizeof(r));
}

static int
isempty(const uint8_t *p, int n)
{
	while (n--)
		if (*p++)
			return 0;
	return 1;
}

/* entries in directory np, "." and ".." left out */
static int
nchildren_9p(node_9p *np, uint32_t *count)
{
	dirent_9p *dp;
	uint32_t nd, iounit, i, c;
	fid_9p fid;
	qid_9p qid;
	int e;

	if ((e = walk_9p(np->nmp, np->fid, NULL, 0, &fid, &qid)))
		return e;
	if ((e = lopen_9p(np->nmp, fid, L_O_RDONLY, &qid, &iounit)) == 0 &&
	    (e = readdirs_9p(np->nmp, fid, &dp, &nd)) == 0) {
		for (i = c = 0; i < nd; i++)
			if (strcmp(dp[i].name, ".") && strcmp(dp[i].name, ".."))
				c++;
		free_9p(dp);
		*count = c;
	}
	clunk_9p(np->nmp, fid);
	return e;
}

/* R_OK, W_OK and X_OK as the caller has them */
static uint32_t
useraccess(struct vnode *vp, node_9p *np, struct ucred *cred)
{
	uint32_t m, a;

	if (cred->cr_uid == 0) {
		a = R_OK | W_OK;
		if (vp->v_type == VDIR || (np->attr.mode & (S_IXUSR|S_IXGRP|S_IXOTH)))
			a |= X_OK;
	} else {
		if (ISSET(vp->v_mount->mnt_flag, MNT_UNKNOWNPERMISSIONS) ||
		    cred->cr_uid == np->nmp->uid)
			m = (np->attr.mode >> 6) & 7;
		else
			m = np->attr.mode & 7;
		a = (m & 4 ? R_OK : 0) | (m & 2 ? W_OK : 0) | (m & 1 ? X_OK : 0);
	}
	if (ISSET(vp->v_mount->mnt_flag, MNT_RDONLY))
		a &= ~W_OK;
	return a;
}

static void
packcommon(pack_9p *pk, attrgroup_t a, struct vnode *vp, node_9p *np,
	   struct ucred *cred, int isvol, const uint8_t *finfo)
{
	mount_9p *nmp;
	struct mount *mp;
	char *name;
	uint32_t mode;
	static const struct timespec zerotime;

	nmp = np->nmp;
	mp = vp->v_mount;
	if (a & ATTR_CMN_NAME) {
		name = malloc_9p(MAXPATHLEN);
		if (name && !ISSET(vp->v_flag, VROOT) &&
		    id_name_9p(nmp, np->id, name, MAXPATHLEN) == 0)
			putstr(pk, name);
		else
			putstr(pk, ISSET(vp->v_flag, VROOT) ? nmp->volume : "");
		free_9p(name);
	}
	if (a & ATTR_CMN_DEVID)
		put32(pk, 0);
	if (a & ATTR_CMN_FSID)
		put(pk, &mp->mnt_stat.f_fsid, sizeof(fsid_t));
	if (a & ATTR_CMN_OBJTYPE)
		put32(pk, isvol ? 0 : vp->v_type);
	if (a & ATTR_CMN_OBJTAG)
		put32(pk, VT_OTHER);
	if (a & ATTR_CMN_OBJID)
		putid(pk, isvol ? 0 : np->id);
	if (a & ATTR_CMN_OBJPERMANENTID)
		putid(pk, isvol ? 0 : np->id);
	if (a & ATTR_CMN_PAROBJID)
		putid(pk, isvol ? 0 : id_parent_9p(nmp, np->id));
	if (a & ATTR_CMN_SCRIPT)
		put32(pk, 0);
	if (a & ATTR_CMN_CRTIME)
		putts(pk, np->attr.btime.tv_sec ? np->attr.btime : np->attr.mtime);
	if (a & ATTR_CMN_MODTIME)
		putts(pk, np->attr.mtime);
	if (a & ATTR_CMN_CHGTIME)
		putts(pk, np->attr.ctime);
	if (a & ATTR_CMN_ACCTIME)
		putts(pk, np->attr.atime);
	if (a & ATTR_CMN_BKUPTIME)
		putts(pk, zerotime);
	if (a & ATTR_CMN_FNDRINFO)
		put(pk, finfo, 32);
	if (a & ATTR_CMN_OWNERID)
		put32(pk, ISSET(mp->mnt_flag, MNT_UNKNOWNPERMISSIONS) ? cred->cr_uid : nmp->uid);
	if (a & ATTR_CMN_GRPID)
		put32(pk, ISSET(mp->mnt_flag, MNT_UNKNOWNPERMISSIONS) ? cred->cr_gid : nmp->gid);
	if (a & ATTR_CMN_ACCESSMASK) {
		mode = np->attr.mode;
		if (ISSET(mp->mnt_flag, MNT_UNKNOWNPERMISSIONS))
			mode &= ~(S_ISUID | S_ISGID);
		put32(pk, mode);
	}
	if (a & ATTR_CMN_FLAGS)
		put32(pk, 0);
	if (a & ATTR_CMN_USERACCESS)
		put32(pk, useraccess(vp, np, cred));
}

static void
packvol(pack_9p *pk, attrgroup_t a, struct vnode *vp, node_9p *np)
{
	vol_capabilities_attr_t cap;
	vol_attributes_attr_t va;
	struct mount *mp;
	mount_9p *nmp;
	fsstat_9p st;
	uint64_t objs;

	nmp = np->nmp;
	mp = vp->v_mount;
	if (statfs_9p(nmp, np->fid, &st) || st.bsize == 0)
		bzero(&st, sizeof(st));
	objs = st.files > st.ffree ? st.files - st.ffree : 0;
	if (objs > 0xFFFFFFFFULL)
		objs = 0xFFFFFFFFULL;
	if (a & ATTR_VOL_FSTYPE)
		put32(pk, mp->mnt_vfc->vfc_typenum);
	if (a & ATTR_VOL_SIGNATURE)
		put32(pk, 0x4244);	/* 'BD', as for any volume that is not HFS */
	if (a & ATTR_VOL_SIZE)
		put64(pk, st.blocks * st.bsize);
	if (a & ATTR_VOL_SPACEFREE)
		put64(pk, st.bfree * st.bsize);
	if (a & ATTR_VOL_SPACEAVAIL)
		put64(pk, st.bavail * st.bsize);
	if (a & ATTR_VOL_MINALLOCATION)
		put64(pk, PAGE_SIZE);
	if (a & ATTR_VOL_ALLOCATIONCLUMP)
		put64(pk, iosize_9p(nmp));
	if (a & ATTR_VOL_IOBLOCKSIZE)
		put32(pk, iosize_9p(nmp));
	if (a & ATTR_VOL_OBJCOUNT)
		put32(pk, objs);
	if (a & ATTR_VOL_FILECOUNT)
		put32(pk, objs);
	if (a & ATTR_VOL_DIRCOUNT)
		put32(pk, 0);
	if (a & ATTR_VOL_MAXOBJCOUNT)
		put32(pk, 0xFFFFFFFF);
	if (a & ATTR_VOL_MOUNTPOINT)
		putstr(pk, mp->mnt_stat.f_mntonname);
	if (a & ATTR_VOL_NAME)
		putstr(pk, nmp->volume);
	if (a & ATTR_VOL_MOUNTFLAGS)
		put32(pk, mp->mnt_flag);
	if (a & ATTR_VOL_MOUNTEDDEVICE)
		putstr(pk, mp->mnt_stat.f_mntfromname);
	if (a & ATTR_VOL_ENCODINGSUSED)
		put64(pk, 1);	/* Mac Roman */
	if (a & ATTR_VOL_CAPABILITIES) {
		bzero(&cap, sizeof(cap));
		cap.capabilities[VOL_CAPABILITIES_FORMAT] =
			VOL_CAP_FMT_PERSISTENTOBJECTIDS |
			VOL_CAP_FMT_SYMBOLICLINKS |
			VOL_CAP_FMT_CASE_PRESERVING;
		cap.capabilities[VOL_CAPABILITIES_INTERFACES] =
			VOL_CAP_INT_ATTRLIST |
			VOL_CAP_INT_ADVLOCK |
			VOL_CAP_INT_FLOCK;
		cap.valid[VOL_CAPABILITIES_FORMAT] =
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
			VOL_CAP_FMT_FAST_STATFS;
		cap.valid[VOL_CAPABILITIES_INTERFACES] =
			VOL_CAP_INT_SEARCHFS |
			VOL_CAP_INT_ATTRLIST |
			VOL_CAP_INT_NFSEXPORT |
			VOL_CAP_INT_READDIRATTR |
			VOL_CAP_INT_EXCHANGEDATA |
			VOL_CAP_INT_COPYFILE |
			VOL_CAP_INT_ALLOCATE |
			VOL_CAP_INT_VOL_RENAME |
			VOL_CAP_INT_ADVLOCK |
			VOL_CAP_INT_FLOCK;
		put(pk, &cap, sizeof(cap));
	}
	if (a & ATTR_VOL_ATTRIBUTES) {
		bzero(&va, sizeof(va));
		va.validattr.commonattr = va.nativeattr.commonattr =
			ATTR_CMN_VALIDMASK & ~(ATTR_CMN_NAMEDATTRCOUNT | ATTR_CMN_NAMEDATTRLIST);
		va.validattr.volattr = va.nativeattr.volattr = ATTR_VOL_VALIDMASK;
		va.validattr.dirattr = va.nativeattr.dirattr = ATTR_DIR_VALIDMASK;
		va.validattr.fileattr = va.nativeattr.fileattr =
			ATTR_FILE_VALIDMASK & ~(ATTR_FILE_FILETYPE | ATTR_FILE_FORKCOUNT |
						ATTR_FILE_FORKLIST);
		va.validattr.forkattr = va.nativeattr.forkattr = 0;
		put(pk, &va, sizeof(va));
	}
}

static void
packdir(pack_9p *pk, attrgroup_t a, struct vnode *vp, node_9p *np)
{
	uint32_t n;

	if (a & ATTR_DIR_LINKCOUNT)
		put32(pk, np->attr.nlink);
	if (a & ATTR_DIR_ENTRYCOUNT) {
		/* the host's link count need not count entries */
		n = 0;
		nchildren_9p(np, &n);
		put32(pk, n);
	}
	if (a & ATTR_DIR_MOUNTSTATUS)
		put32(pk, vp->v_mountedhere ? DIR_MNTSTATUS_MNTPOINT : 0);
}

static void
packfile(pack_9p *pk, attrgroup_t a, node_9p *np, uint64_t rlen)
{
	static const extentrecord noextents;
	uint64_t ralloc;

	ralloc = (rlen + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
	if (a & ATTR_FILE_LINKCOUNT)
		put32(pk, np->attr.nlink);
	if (a & ATTR_FILE_TOTALSIZE)
		put64(pk, np->attr.size + rlen);
	if (a & ATTR_FILE_ALLOCSIZE)
		put64(pk, np->attr.blocks * 512 + ralloc);
	if (a & ATTR_FILE_IOBLOCKSIZE)
		put32(pk, iosize_9p(np->nmp));
	if (a & ATTR_FILE_CLUMPSIZE)
		put32(pk, iosize_9p(np->nmp));
	if (a & ATTR_FILE_DEVTYPE)
		put32(pk, 0);
	if (a & ATTR_FILE_DATALENGTH)
		put64(pk, np->attr.size);
	if (a & ATTR_FILE_DATAALLOCSIZE)
		put64(pk, np->attr.blocks * 512);
	if (a & ATTR_FILE_DATAEXTENTS)
		put(pk, &noextents, sizeof(noextents));
	if (a & ATTR_FILE_RSRCLENGTH)
		put64(pk, rlen);
	if (a & ATTR_FILE_RSRCALLOCSIZE)
		put64(pk, ralloc);
	if (a & ATTR_FILE_RSRCEXTENTS)
		put(pk, &noextents, sizeof(noextents));
}

__private_extern__ int
vop_getattrlist_9p(struct vop_getattrlist_args *ap)
{
	struct attrlist *al;
	struct vnode *vp;
	node_9p *np;
	pack_9p pk;
	uint8_t finfo[32];
	uint64_t rlen;
	char *buf;
	int e, fixed, size, isvol;

	vp = ap->a_vp;
	al = ap->a_alist;
	np = NTO9P(vp);
	if (al->bitmapcount != ATTR_BIT_MAP_COUNT ||
	    (al->commonattr & ~ATTR_CMN_VALIDMASK) ||
	    (al->volattr & ~ATTR_VOL_VALIDMASK) ||
	    (al->dirattr & ~ATTR_DIR_VALIDMASK) ||
	    (al->fileattr & ~ATTR_FILE_VALIDMASK))
		return EINVAL;
	/* volume information is asked of the root, alone */
	isvol = al->volattr != 0;
	if (isvol && (!(al->volattr & ATTR_VOL_INFO) || al->dirattr || al->fileattr ||
	    !ISSET(vp->v_flag, VROOT)))
		return EINVAL;
	if ((al->commonattr & (ATTR_CMN_NAMEDATTRCOUNT | ATTR_CMN_NAMEDATTRLIST)) ||
	    (al->fileattr & (ATTR_FILE_FILETYPE | ATTR_FILE_FORKCOUNT | ATTR_FILE_FORKLIST)))
		return EINVAL;
	if ((e = ngetattr_9p(np, 0)))
		return e;

	bzero(finfo, sizeof(finfo));
	if ((al->commonattr & ATTR_CMN_FNDRINFO) && !isvol &&
	    (e = xget_9p(np->nmp, np->fid, FINDERINFO, finfo, sizeof(finfo), NULL)))
		return e;
	rlen = 0;
	if ((al->fileattr & (ATTR_FILE_TOTALSIZE | ATTR_FILE_ALLOCSIZE |
	    ATTR_FILE_RSRCLENGTH | ATTR_FILE_RSRCALLOCSIZE)) && vp->v_type == VREG &&
	    (e = xget_9p(np->nmp, np->fid, RSRCFORK, NULL, 0, &rlen)))
		return e;

	fixed = fixedsize(al);
	size = sizeof(uint32_t) + fixed;
	if (al->commonattr & ATTR_CMN_NAME)
		size += MAXPATHLEN;
	if (al->volattr & ATTR_VOL_MOUNTPOINT)
		size += MNAMELEN + 4;
	if (al->volattr & ATTR_VOL_NAME)
		size += VOLNAMELEN + 4;
	if (al->volattr & ATTR_VOL_MOUNTEDDEVICE)
		size += MNAMELEN + 4;
	if ((buf = malloc_9p(size)) == NULL)
		return ENOMEM;
	pk.fix = buf + sizeof(uint32_t);
	pk.var = pk.fix + fixed;

	packcommon(&pk, al->commonattr, vp, np, ap->a_cred, isvol, finfo);
	if (isvol)
		packvol(&pk, al->volattr, vp, np);
	else if (vp->v_type == VDIR)
		packdir(&pk, al->dirattr, vp, np);
	else
		packfile(&pk, al->fileattr, np, rlen);

	size = MIN(pk.var - buf, ap->a_uio->uio_resid);
	*(uint32_t *)buf = size;
	e = uiomove(buf, size, ap->a_uio);
	free_9p(buf);
	return e;
}

static void
get(char **p, void *v, int n)
{
	if (v)
		bcopy(*p, v, n);
	*p += n;
}

__private_extern__ int
vop_setattrlist_9p(struct vop_setattrlist_args *ap)
{
	struct timespec at, mt;
	struct attrreference r;
	struct attrlist *al;
	struct vnode *vp;
	node_9p *np;
	attrgroup_t a;
	uint32_t valid, mode, v;
	uint8_t finfo[32];
	int e, size, setfinfo;
	char *buf, *p, *ref;

	vp = ap->a_vp;
	al = ap->a_alist;
	np = NTO9P(vp);
	if (ISSET(vp->v_mount->mnt_flag, MNT_RDONLY))
		return EROFS;
	if (al->bitmapcount != ATTR_BIT_MAP_COUNT ||
	    (al->commonattr & ~ATTR_CMN_SETMASK) ||
	    (al->volattr & ~ATTR_VOL_SETMASK) ||
	    (al->dirattr & ~ATTR_DIR_SETMASK) ||
	    (al->fileattr & ~ATTR_FILE_SETMASK))
		return EINVAL;
	if (al->volattr && (!(al->volattr & ATTR_VOL_INFO) ||
	    (al->commonattr & ~ATTR_CMN_VOLSETMASK) || !ISSET(vp->v_flag, VROOT)))
		return EINVAL;
	size = ap->a_uio->uio_resid;
	if (size < fixedsize(al) || size > 4 * PAGE_SIZE)
		return EINVAL;
	if ((buf = malloc_9p(size + 1)) == NULL)
		return ENOMEM;
	if ((e = uiomove(buf, size, ap->a_uio)))
		goto out;

	p = buf;
	a = al->commonattr;
	valid = 0;
	mode = 0;
	setfinfo = 0;
	get(&p, NULL, a & ATTR_CMN_SCRIPT ? 4 : 0);
	get(&p, NULL, a & ATTR_CMN_CRTIME ? 8 : 0);	/* the host keeps its own */
	if (a & ATTR_CMN_MODTIME) {
		get(&p, &v, 4);
		mt.tv_sec = v;
		get(&p, &v, 4);
		mt.tv_nsec = v;
		valid |= SA_MTIME | SA_MTIME_SET;
	}
	get(&p, NULL, a & ATTR_CMN_CHGTIME ? 8 : 0);
	if (a & ATTR_CMN_ACCTIME) {
		get(&p, &v, 4);
		at.tv_sec = v;
		get(&p, &v, 4);
		at.tv_nsec = v;
		valid |= SA_ATIME | SA_ATIME_SET;
	}
	get(&p, NULL, a & ATTR_CMN_BKUPTIME ? 8 : 0);
	if (a & ATTR_CMN_FNDRINFO) {
		get(&p, finfo, 32);
		setfinfo = 1;
	}
	/* ownership and flags are the host's business */
	get(&p, NULL, a & ATTR_CMN_OWNERID ? 4 : 0);
	get(&p, NULL, a & ATTR_CMN_GRPID ? 4 : 0);
	if (a & ATTR_CMN_ACCESSMASK) {
		get(&p, &v, 4);
		if (v != (uint32_t)VNOVAL) {
			mode = v & 07777;
			valid |= SA_MODE;
		}
	}
	get(&p, NULL, a & ATTR_CMN_FLAGS ? 4 : 0);

	if (al->volattr & ATTR_VOL_NAME) {
		/* the name shown for the volume; the host share keeps its own */
		ref = p;
		get(&p, &r, sizeof(r));
		if (r.attr_dataoffset < 0 || r.attr_length > VOLNAMELEN ||
		    ref + r.attr_dataoffset + r.attr_length > buf + size) {
			e = EINVAL;
			goto out;
		}
		buf[size] = 0;
		bcopy(ref + r.attr_dataoffset, np->nmp->volume, r.attr_length);
		np->nmp->volume[VOLNAMELEN - 1] = 0;
	}

	e = 0;
	if (setfinfo)
		e = xput_9p(np->nmp, np->fid, FINDERINFO, finfo, isempty(finfo, 32) ? 0 : 32);
	if (e == 0 && valid) {
		e = setattr_9p(np->nmp, np->fid, valid, mode, 0,
			       valid & SA_ATIME ? &at : NULL,
			       valid & SA_MTIME ? &mt : NULL);
		np->attrtime = 0;
	}
out:
	free_9p(buf);
	return e;
}
