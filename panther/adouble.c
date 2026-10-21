/*
 * Carbon on 10.3 keeps Finder info and resource forks of files on volumes
 * other than HFS in AppleDouble files, "._name" beside "name". Here those
 * names are served from, and stored in, the host's com.apple.FinderInfo
 * and com.apple.ResourceFork attributes of "name", where 10.4 and later,
 * Mac OS 9 and the host itself keep them. A real "._name" on the host is
 * left alone and shown as it is.
 */
#include "9p.h"

#define ADQBIT		(1ULL << 62)	/* an AppleDouble node's key: its file's, flipped */

enum {
	AD_MAGIC	= 0x00051607,
	AD_VERSION	= 0x00020000,
	AD_RSRC		= 2,
	AD_FINFO	= 9,
	AD_HDRLEN	= 26,
	AD_ENTLEN	= 12,
	AD_FINFOOFF	= AD_HDRLEN + 2 * AD_ENTLEN,	/* 0x32 */
	AD_RSRCOFF	= AD_FINFOOFF + 32,		/* 0x52 */
};

static void
w16(uint8_t *p, uint16_t v)
{
	p[0] = v >> 8;
	p[1] = v;
}

static void
w32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static uint16_t
r16(const uint8_t *p)
{
	return p[0] << 8 | p[1];
}

static uint32_t
r32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/* the key of the file an AppleDouble node stands for */
__private_extern__ uint64_t
adbase_9p(node_9p *np)
{
	return np->qid.path ^ ADQBIT;
}

__private_extern__ int
adname_9p(const char *name, int len)
{
	return len > 2 && name[0] == '.' && name[1] == '_';
}

/* the value of attribute name of fid's file, whole; absent reads as none */
__private_extern__ int
xget_9p(mount_9p *nmp, fid_9p fid, const char *name, void *buf, uint32_t len, uint64_t *size)
{
	uint64_t n, off;
	uint32_t got;
	fid_9p xfid;
	int e;

	if (buf)
		bzero(buf, len);
	if (size)
		*size = 0;
	e = xattrwalk_9p(nmp, fid, name, &xfid, &n);
	if (e == ENOATTR)
		return 0;
	if (e)
		return e;
	if (size)
		*size = n;
	for (off = 0; buf && off < MIN(n, (uint64_t)len); off += got) {
		if ((e = read_9p(nmp, xfid, (uint8_t *)buf + off, MIN(MIN(n, len) - off, iosize_9p(nmp)),
				 off, &got)))
			break;
		if (got == 0) {
			e = EIO;
			break;
		}
	}
	clunk_9p(nmp, xfid);
	return e;
}

/* replace attribute name of fid's file with buf[0..len); none removes it */
__private_extern__ int
xput_9p(mount_9p *nmp, fid_9p fid, const char *name, const void *buf, uint32_t len)
{
	uint32_t off, n;
	uint64_t old;
	fid_9p xfid;
	qid_9p qid;
	int e, e2;

	/* a macOS host overwrites a resource fork in place, keeping its tail */
	if (len && strcmp(name, XATTR_RSRC9P) == 0 &&
	    xattrwalk_9p(nmp, fid, name, &xfid, &old) == 0) {
		clunk_9p(nmp, xfid);
		if (old > len && (e = xput_9p(nmp, fid, name, NULL, 0)))
			return e;
	}
	if ((e = walk_9p(nmp, fid, NULL, 0, &xfid, &qid)))
		return e;
	if ((e = xattrcreate_9p(nmp, xfid, name, len, 0))) {
		clunk_9p(nmp, xfid);
		return e;
	}
	for (off = 0; off < len && !e; off += n) {
		e = write_9p(nmp, xfid, (uint8_t *)buf + off, MIN(len - off, iosize_9p(nmp)), off, &n);
		if (!e && n == 0)
			e = EIO;
	}
	/* the server applies the value on clunk */
	e2 = clunk_9p(nmp, xfid);
	if (e == 0)
		e = e2;
	if (len == 0 && e == ENOATTR)
		e = 0;
	return e;
}

static int
iszero(const uint8_t *p, int n)
{
	while (n--)
		if (*p++)
			return 0;
	return 1;
}

static int
adresize(node_9p *np, uint32_t len)
{
	uint8_t *b;
	uint32_t cap;

	if (len > XATTR_MAX9P + AD_RSRCOFF)
		return EFBIG;
	if (len > np->adcap) {
		cap = MAX(len, np->adcap * 2);
		cap = MAX(cap, 512);
		if ((b = malloc_9p(cap)) == NULL)
			return ENOMEM;
		if (np->adbuf) {
			bcopy(np->adbuf, b, np->adlen);
			free_9p(np->adbuf);
		}
		np->adbuf = b;
		np->adcap = cap;
	}
	if (len > np->adlen)
		bzero(np->adbuf + np->adlen, len - np->adlen);
	np->adlen = len;
	return 0;
}

/* the AppleDouble image of the file's Finder info and resource fork */
static int
adload(node_9p *np)
{
	uint64_t rlen;
	uint8_t *b;
	int e;

	if (np->adloaded)
		return 0;
	if ((e = xget_9p(np->nmp, np->fid, XATTR_RSRC9P, NULL, 0, &rlen)))
		return e;
	if (rlen > XATTR_MAX9P)
		return EFBIG;
	if ((e = adresize(np, AD_RSRCOFF + (uint32_t)rlen)))
		return e;
	b = np->adbuf;
	w32(b, AD_MAGIC);
	w32(b + 4, AD_VERSION);
	bcopy("Mac OS X        ", b + 8, 16);
	w16(b + 24, 2);
	w32(b + AD_HDRLEN, AD_FINFO);
	w32(b + AD_HDRLEN + 4, AD_FINFOOFF);
	w32(b + AD_HDRLEN + 8, 32);
	w32(b + AD_HDRLEN + AD_ENTLEN, AD_RSRC);
	w32(b + AD_HDRLEN + AD_ENTLEN + 4, AD_RSRCOFF);
	w32(b + AD_HDRLEN + AD_ENTLEN + 8, (uint32_t)rlen);
	if ((e = xget_9p(np->nmp, np->fid, XATTR_FINFO9P, b + AD_FINFOOFF, 32, NULL)) ||
	    (rlen && (e = xget_9p(np->nmp, np->fid, XATTR_RSRC9P, b + AD_RSRCOFF,
				  (uint32_t)rlen, NULL)))) {
		np->adlen = 0;
		return e;
	}
	np->adloaded = 1;
	np->addirty = 0;
	return 0;
}

/* store what the image says; one that is not AppleDouble leaves things be */
__private_extern__ int
adflush_9p(node_9p *np)
{
	uint8_t finfo[32], *b;
	uint32_t i, n, id, off, len, roff, rlen;
	int e, havefinfo;

	if (!np->adloaded || !np->addirty)
		return 0;
	b = np->adbuf;
	bzero(finfo, sizeof(finfo));
	roff = rlen = 0;
	havefinfo = 0;
	if (np->adlen == 0) {
		/* emptied: no Finder info, no fork */
	} else if (np->adlen < AD_HDRLEN || r32(b) != AD_MAGIC) {
		np->addirty = 0;
		return 0;
	} else {
		n = r16(b + 24);
		for (i = 0; i < n && AD_HDRLEN + (i + 1) * AD_ENTLEN <= np->adlen; i++) {
			id = r32(b + AD_HDRLEN + i * AD_ENTLEN);
			off = r32(b + AD_HDRLEN + i * AD_ENTLEN + 4);
			len = r32(b + AD_HDRLEN + i * AD_ENTLEN + 8);
			if (off > np->adlen || len > np->adlen - off)
				continue;
			if (id == AD_FINFO) {
				bcopy(b + off, finfo, MIN(len, 32));
				havefinfo = 1;
			} else if (id == AD_RSRC) {
				roff = off;
				rlen = len;
			}
		}
	}
	e = xput_9p(np->nmp, np->fid, XATTR_FINFO9P, finfo,
		    havefinfo && !iszero(finfo, 32) ? 32 : 0);
	if (e == 0)
		e = xput_9p(np->nmp, np->fid, XATTR_RSRC9P, b + roff, rlen);
	/* the file went away under it: nothing to keep */
	if (e == ENOENT)
		e = 0;
	if (e == 0)
		np->addirty = 0;
	return e;
}

/*
 * The AppleDouble node for "._base" in dnp, locked and referenced: one
 * there already, or, if the file has Finder info or a fork (or create is
 * set), a new one.
 */
__private_extern__ int
adget_9p(node_9p *dnp, char *base, int nbase, int create, struct vnode **vpp,
	 struct proc *p)
{
	mount_9p *nmp;
	uint8_t finfo[32];
	uint64_t rlen;
	node_9p *np;
	fid_9p fid;
	qid_9p qid;
	int e;

	nmp = dnp->nmp;
	*vpp = NULL;
	if ((e = walk_9p(nmp, dnp->fid, base, nbase, &fid, &qid)))
		return e;
	qid.path ^= ADQBIT;
	qid.type = 0;
	if (nget_9p(nmp, NOFID, qid, 0, vpp, p) == 0) {
		clunk_9p(nmp, fid);
		return 0;
	}
	if (!create) {
		rlen = 0;
		e = xget_9p(nmp, fid, XATTR_FINFO9P, finfo, sizeof(finfo), NULL);
		if (e == 0 && iszero(finfo, sizeof(finfo)))
			e = xget_9p(nmp, fid, XATTR_RSRC9P, NULL, 0, &rlen);
		if (e == 0 && iszero(finfo, sizeof(finfo)) && rlen == 0)
			e = ENOENT;
		if (e) {
			clunk_9p(nmp, fid);
			return e;
		}
	}
	if ((e = nget_9p(nmp, fid, qid, NGET_AD, vpp, p))) {
		clunk_9p(nmp, fid);
		return e;
	}
	np = NTO9P(*vpp);
	if (np->fid != fid)
		clunk_9p(nmp, fid);
	if (create && (e = adload(np))) {
		vput(*vpp);
		*vpp = NULL;
	}
	return e;
}

__private_extern__ int
adsize_9p(node_9p *np, uint64_t *size)
{
	int e;

	if ((e = adload(np)))
		return e;
	*size = np->adlen;
	return 0;
}

__private_extern__ int
adread_9p(node_9p *np, struct uio *uio)
{
	off_t off;
	int e;

	if ((e = adload(np)))
		return e;
	off = uio->uio_offset;
	if (off < 0)
		return EINVAL;
	if (off >= np->adlen)
		return 0;
	return uiomove((caddr_t)np->adbuf + off,
		       MIN((off_t)uio->uio_resid, np->adlen - off), uio);
}

__private_extern__ int
adwrite_9p(node_9p *np, struct uio *uio, int ioflag)
{
	off_t off, end;
	int e;

	if ((e = adload(np)))
		return e;
	if (ISSET(ioflag, IO_APPEND))
		uio->uio_offset = np->adlen;
	off = uio->uio_offset;
	if (off < 0)
		return EINVAL;
	end = off + uio->uio_resid;
	if (end > np->adlen && (e = adresize(np, end)))
		return e;
	if ((e = uiomove((caddr_t)np->adbuf + off, uio->uio_resid, uio)))
		return e;
	np->addirty = 1;
	/* Finder info now: Carbon may keep the file open; a fork waits for close */
	if (ISSET(ioflag, IO_SYNC) || off < AD_RSRCOFF)
		e = adflush_9p(np);
	return e;
}

__private_extern__ int
adtruncate_9p(node_9p *np, uint64_t len)
{
	int e;

	if ((e = adload(np)))
		return e;
	if ((e = adresize(np, len)))
		return e;
	np->addirty = 1;
	return 0;
}

/* removing "._name" drops the file's Finder info and fork */
__private_extern__ int
adremove_9p(node_9p *np)
{
	int e;

	e = xput_9p(np->nmp, np->fid, XATTR_FINFO9P, NULL, 0);
	if (e == 0)
		e = xput_9p(np->nmp, np->fid, XATTR_RSRC9P, NULL, 0);
	if (e == ENOENT)
		e = 0;
	np->adlen = 0;
	np->adloaded = 0;
	np->addirty = 0;
	return e;
}

__private_extern__ void
adfree_9p(node_9p *np)
{
	if (adflush_9p(np))
		printf("9p: Finder info or fork of id %u lost\n", np->id);
	free_9p(np->adbuf);
	np->adbuf = NULL;
	np->adlen = np->adcap = 0;
	np->adloaded = 0;
}

/* "._a" renamed to "._b": a's Finder info and fork move to b's file */
__private_extern__ int
admove_9p(node_9p *from, fid_9p tofid)
{
	mount_9p *nmp;
	uint8_t finfo[32], *r;
	uint64_t rlen;
	int e;

	nmp = from->nmp;
	if ((e = adflush_9p(from)))
		return e;
	if ((e = xget_9p(nmp, from->fid, XATTR_FINFO9P, finfo, sizeof(finfo), NULL)) ||
	    (e = xget_9p(nmp, from->fid, XATTR_RSRC9P, NULL, 0, &rlen)))
		return e;
	if (rlen > XATTR_MAX9P)
		return EFBIG;
	r = malloc_9p(rlen ? rlen : 1);
	if (r == NULL)
		return ENOMEM;
	if ((e = xget_9p(nmp, from->fid, XATTR_RSRC9P, r, rlen, NULL)) == 0 &&
	    (e = xput_9p(nmp, tofid, XATTR_FINFO9P, finfo, iszero(finfo, 32) ? 0 : 32)) == 0 &&
	    (e = xput_9p(nmp, tofid, XATTR_RSRC9P, r, rlen)) == 0)
		e = adremove_9p(from);
	free_9p(r);
	return e;
}
