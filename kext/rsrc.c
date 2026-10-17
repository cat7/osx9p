/*
 * Resource fork vnodes for "file/..namedfork/rsrc" on 10.4, where the
 * filesystem resolves the suffix itself (10.5 does it in the VFS). The
 * fork is the host's com.apple.ResourceFork xattr; 9P sets an xattr in
 * one piece, so the fork is held in memory while it is written and sent
 * on close and fsync.
 */
#include "9p.h"
#include <libkern/OSMalloc.h>

#define RSRC	XATTR_RESOURCEFORK_NAME

/* forks run to megabytes: pageable kernel_map memory, not kalloc */
static OSMallocTag rtag;

__private_extern__ void
rsrc_init_9p(void)
{
	rtag = OSMalloc_Tagalloc(VFS9PNAME ".rsrc", OSMT_PAGEABLE);
}

__private_extern__ void
rsrc_fini_9p(void)
{
	OSMalloc_Tagfree(rtag);
}

static uint8_t *
ralloc(uint64_t n)
{
	return OSMalloc((uint32_t)(n ? n : 1), rtag);
}

static void
rfree(uint8_t *b, uint64_t n)
{
	if (b)
		OSFree(b, (uint32_t)(n ? n : 1), rtag);
}

static const char rsrcspec[] = "/..namedfork/rsrc";
static const char dataspec[] = "/..namedfork/data";

/* length of a fork suffix following the name; *rsrc set for the resource fork */
__private_extern__ int
forkspec_9p(struct componentname *cnp, int *rsrc)
{
	char *s;

	*rsrc = 0;
	s = cnp->cn_nameptr + cnp->cn_namelen;
	if (*s == 0)
		return 0;
	if (strncmp(s, rsrcspec, sizeof(rsrcspec)) == 0) {
		*rsrc = 1;
		return sizeof(rsrcspec) - 1;
	}
	if (strncmp(s, dataspec, sizeof(dataspec)) == 0)
		return sizeof(dataspec) - 1;
	return 0;
}

static int
rsize(node_9p *rp, uint64_t *size)
{
	fid_9p fid;
	int e;

	if (rp->rloaded) {
		*size = rp->rlen;
		return 0;
	}
	e = xattrwalk_9p(rp->nmp, rp->fid, RSRC, &fid, size);
	if (e == ENOATTR) {
		*size = 0;
		return 0;
	}
	if (e == 0)
		clunk_9p(rp->nmp, fid);
	return e;
}

static int
rload(node_9p *rp)
{
	uint64_t size;
	uint8_t *b;
	fid_9p fid;
	int e;

	if (rp->rloaded)
		return 0;
	e = xattrwalk_9p(rp->nmp, rp->fid, RSRC, &fid, &size);
	if (e == ENOATTR) {
		fid = NOFID;
		size = 0;
		e = 0;
	} else if (e)
		return e;
	if (size > XATTR_MAX) {
		clunk_9p(rp->nmp, fid);
		return EFBIG;
	}
	b = ralloc(size);
	if (b == NULL)
		e = ENOMEM;
	else if (size)
		e = xread_9p(rp->nmp, fid, b, 0, size);
	if (fid != NOFID)
		clunk_9p(rp->nmp, fid);
	if (e) {
		rfree(b, size);
		return e;
	}
	rp->rbuf = b;
	rp->rlen = rp->rcap = size;
	rp->rloaded = 1;
	return 0;
}

/* set the fork length, zero-filling growth */
static int
rresize(node_9p *rp, uint64_t len)
{
	uint64_t cap;
	uint8_t *b;

	if (len > XATTR_MAX)
		return EFBIG;
	if (len > rp->rcap) {
		cap = MAX(len, MIN(rp->rcap * 2, (uint64_t)XATTR_MAX));
		cap = MAX(cap, 4096);
		b = ralloc(cap);
		if (b == NULL)
			return ENOMEM;
		bcopy(rp->rbuf, b, rp->rlen);
		rfree(rp->rbuf, rp->rcap);
		rp->rbuf = b;
		rp->rcap = cap;
	}
	if (len > rp->rlen)
		bzero(rp->rbuf + rp->rlen, len - rp->rlen);
	rp->rlen = len;
	rp->rdirty = 1;
	return 0;
}

static int
rflush(node_9p *rp)
{
	int e;

	if (!rp->rdirty)
		return 0;
	if (rp->rlen == 0) {
		e = xset_9p(rp, RSRC, NULL, 0, 0);
		if (e == ENOATTR)
			e = 0;
	} else
		e = xset_9p(rp, RSRC, rp->rbuf, rp->rlen, 0);
	/* the file was deleted; its fork goes with it */
	if (e == ENOENT)
		e = 0;
	if (e == 0)
		rp->rdirty = 0;
	return e;
}

static void
rdrop(node_9p *rp)
{
	rfree(rp->rbuf, rp->rcap);
	rp->rbuf = NULL;
	rp->rlen = rp->rcap = 0;
	rp->rloaded = 0;
}

/*
 * Before the data node's ResourceFork xattr is used directly: send pending
 * fork writes, and forget the copy if the xattr is about to change.
 */
__private_extern__ void
rsrc_sync_9p(node_9p *np, int drop)
{
	mount_9p *nmp;
	node_9p *rp;
	vnode_t vp;
	uint32_t vid;

	nmp = np->nmp;
	lck_mtx_lock(nmp->nodelck);
	rp = np->rsrc;
	if (rp == NULL || ISSET(rp->flags, NODE_INIT | NODE_RECL)) {
		lck_mtx_unlock(nmp->nodelck);
		return;
	}
	vp = rp->vp;
	vid = vnode_vid(vp);
	lck_mtx_unlock(nmp->nodelck);
	if (vnode_getwithvid(vp, vid))
		return;
	nlock_9p(rp, NODE_LCK_EXCLUSIVE);
	rflush(rp);
	if (drop && !rp->rdirty)
		rdrop(rp);
	nunlock_9p(rp);
	vnode_put(vp);
}

/* the resource fork vnode of data node np, with an iocount */
__private_extern__ int
rsrc_vget_9p(node_9p *np, vnode_t dvp, vnode_t *vpp)
{
	struct vnode_fsparam fsp;
	mount_9p *nmp;
	node_9p *rp;
	uint64_t size;
	uint32_t vid;
	vnode_t vp;
	qid_9p qid;
	int e, i;

	nmp = np->nmp;
	*vpp = NULL;
again:
	lck_mtx_lock(nmp->nodelck);
	rp = np->rsrc;
	if (rp) {
		if (ISSET(rp->flags, NODE_INIT | NODE_RECL)) {
			SET(rp->flags, ISSET(rp->flags, NODE_INIT) ? NODE_WAITINIT : NODE_WAITRECL);
			msleep(rp, nmp->nodelck, PINOD | PDROP, "9p_rsrc", NULL);
			goto again;
		}
		vp = rp->vp;
		vid = vnode_vid(vp);
		lck_mtx_unlock(nmp->nodelck);
		if (vnode_getwithvid(vp, vid))
			goto again;
		*vpp = vp;
		return 0;
	}

	rp = malloc_9p(sizeof(*rp));
	if (rp == NULL) {
		lck_mtx_unlock(nmp->nodelck);
		return ENOMEM;
	}
	rp->lck = lck_rw_alloc_init(lck_grp_9p, LCK_ATTR_NULL);
	if (rp->lck == NULL) {
		lck_mtx_unlock(nmp->nodelck);
		free_9p(rp);
		return ENOMEM;
	}
	rp->nmp = nmp;
	rp->flags = NODE_INIT | NODE_RSRC;
	rp->qid = np->qid;
	rp->id = np->id;
	rp->attr = np->attr;
	rp->data = np;
	rp->fid = NOFID;
	for (i = 0; i < NOFIDS; i++)
		rp->openfid[i].fid = NOFID;
	np->rsrc = rp;
	lck_mtx_unlock(nmp->nodelck);

	nlock_9p(np, NODE_LCK_SHARED);
	e = walk_9p(nmp, np->fid, NULL, 0, &rp->fid, &qid);
	nunlock_9p(np);
	if (e == 0)
		e = rsize(rp, &size);
	if (e == 0) {
		bzero(&fsp, sizeof(fsp));
		fsp.vnfs_mp = nmp->mp;
		fsp.vnfs_vtype = VREG;
		fsp.vnfs_str = VFS9PNAME;
		fsp.vnfs_dvp = dvp;
		fsp.vnfs_fsnode = rp;
		fsp.vnfs_vops = vnode_op_9p;
		fsp.vnfs_filesize = size;
		fsp.vnfs_flags = VNFS_NOCACHE | VNFS_CANTCACHE;
		e = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &fsp, &rp->vp);
	}

	lck_mtx_lock(nmp->nodelck);
	if (e)
		np->rsrc = NULL;
	if (ISSET(rp->flags, NODE_WAITINIT))
		wakeup(rp);
	CLR(rp->flags, NODE_INIT | NODE_WAITINIT);
	lck_mtx_unlock(nmp->nodelck);
	if (e) {
		if (rp->fid != NOFID)
			clunk_9p(nmp, rp->fid);
		lck_rw_free(rp->lck, lck_grp_9p);
		free_9p(rp);
		return e;
	}
	vnode_addfsref(rp->vp);
	vnode_settag(rp->vp, VT_OTHER);
	vnode_setnocache(rp->vp);
	*vpp = rp->vp;
	return 0;
}

__private_extern__ int
rsrc_read_9p(node_9p *rp, uio_t uio)
{
	off_t off;
	uint64_t n;
	int e;

	nlock_9p(rp, NODE_LCK_EXCLUSIVE);
	if ((e = rload(rp)))
		goto out;
	off = uio_offset(uio);
	if (off < 0) {
		e = EINVAL;
		goto out;
	}
	if ((uint64_t)off >= rp->rlen)
		goto out;
	n = MIN((uint64_t)uio_resid(uio), rp->rlen - off);
	e = uiomove((caddr_t)rp->rbuf + off, n, uio);
out:
	nunlock_9p(rp);
	return e;
}

__private_extern__ int
rsrc_write_9p(node_9p *rp, uio_t uio, int ioflag)
{
	off_t off;
	uint64_t n;
	int e;

	nlock_9p(rp, NODE_LCK_EXCLUSIVE);
	if ((e = rload(rp)))
		goto out;
	if (ISSET(ioflag, IO_APPEND))
		uio_setoffset(uio, rp->rlen);
	off = uio_offset(uio);
	n = uio_resid(uio);
	if (off < 0) {
		e = EINVAL;
		goto out;
	}
	if (off + n > rp->rlen && (e = rresize(rp, off + n)))
		goto out;
	e = uiomove((caddr_t)rp->rbuf + off, n, uio);
	rp->rdirty = 1;
	if (ISSET(ioflag, IO_SYNC) && e == 0)
		e = rflush(rp);
out:
	nunlock_9p(rp);
	return e;
}

__private_extern__ int
rsrc_size_9p(node_9p *rp, uint64_t *size)
{
	int e;

	nlock_9p(rp, NODE_LCK_EXCLUSIVE);
	e = rsize(rp, size);
	nunlock_9p(rp);
	return e;
}

__private_extern__ int
rsrc_truncate_9p(node_9p *rp, uint64_t len)
{
	int e;

	nlock_9p(rp, NODE_LCK_EXCLUSIVE);
	if ((e = rload(rp)) == 0)
		e = rresize(rp, len);
	nunlock_9p(rp);
	return e;
}

__private_extern__ int
rsrc_flush_9p(node_9p *rp)
{
	int e;

	nlock_9p(rp, NODE_LCK_EXCLUSIVE);
	e = rflush(rp);
	nunlock_9p(rp);
	return e;
}

__private_extern__ void
rsrc_reclaim_9p(node_9p *rp)
{
	mount_9p *nmp;

	nmp = rp->nmp;
	if (rflush(rp))
		printf("9p: resource fork of id %u lost\n", rp->id);
	rdrop(rp);
	lck_mtx_lock(nmp->nodelck);
	if (rp->data)
		rp->data->rsrc = NULL;
	lck_mtx_unlock(nmp->nodelck);
}
