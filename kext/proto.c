#include "9p.h"

enum {
	Tlerror = 6,	Rlerror,
	Tstatfs = 8,	Rstatfs,
	Tlopen = 12,	Rlopen,
	Tlcreate = 14,	Rlcreate,
	Tsymlink = 16,	Rsymlink,
	Treadlink = 22,	Rreadlink,
	Tgetattr = 24,	Rgetattr,
	Tsetattr = 26,	Rsetattr,
	Txattrwalk = 30, Rxattrwalk,
	Txattrcreate = 32, Rxattrcreate,
	Treaddir = 40,	Rreaddir,
	Tfsync = 50,	Rfsync,
	Tmkdir = 72,	Rmkdir,
	Trenameat = 74,	Rrenameat,
	Tunlinkat = 76,	Runlinkat,
	Tversion = 100,	Rversion,
	Tattach = 104,	Rattach,
	Twalk = 110,	Rwalk,
	Tread = 116,	Rread,
	Twrite = 118,	Rwrite,
	Tclunk = 120,	Rclunk,
};

#define HDRSZ		7
#define QIDSZ		13
#define IOHDRSZ		24

typedef struct {
	uint8_t *buf, *p, *end;
	int err;
} msg_9p;

static void
need(msg_9p *m, size_t n)
{
	if (m->p + n > m->end)
		m->err = EMSGSIZE;
}

static void
p8(msg_9p *m, uint8_t v)
{
	need(m, 1);
	if (!m->err)
		*m->p++ = v;
}

static void
p16(msg_9p *m, uint16_t v)
{
	need(m, 2);
	if (!m->err) {
		m->p[0] = v; m->p[1] = v >> 8;
		m->p += 2;
	}
}

static void
p32(msg_9p *m, uint32_t v)
{
	need(m, 4);
	if (!m->err) {
		m->p[0] = v; m->p[1] = v >> 8; m->p[2] = v >> 16; m->p[3] = v >> 24;
		m->p += 4;
	}
}

static void
p64(msg_9p *m, uint64_t v)
{
	p32(m, (uint32_t)v);
	p32(m, (uint32_t)(v >> 32));
}

static void
pstrn(msg_9p *m, const char *s, int n)
{
	p16(m, n);
	need(m, n);
	if (!m->err) {
		bcopy(s, m->p, n);
		m->p += n;
	}
}

static void
pstr(msg_9p *m, const char *s)
{
	pstrn(m, s, strlen(s));
}

static void
pbytes(msg_9p *m, const void *d, uint32_t n)
{
	need(m, n);
	if (!m->err) {
		bcopy(d, m->p, n);
		m->p += n;
	}
}

static uint8_t
g8(msg_9p *m)
{
	need(m, 1);
	return m->err ? 0 : *m->p++;
}

static uint16_t
g16(msg_9p *m)
{
	uint16_t v;

	need(m, 2);
	if (m->err)
		return 0;
	v = m->p[0] | (m->p[1] << 8);
	m->p += 2;
	return v;
}

static uint32_t
g32(msg_9p *m)
{
	uint32_t v;

	need(m, 4);
	if (m->err)
		return 0;
	v = m->p[0] | (m->p[1] << 8) | (m->p[2] << 16) | ((uint32_t)m->p[3] << 24);
	m->p += 4;
	return v;
}

static uint64_t
g64(msg_9p *m)
{
	uint64_t lo;

	lo = g32(m);
	return lo | ((uint64_t)g32(m) << 32);
}

static void
gqid(msg_9p *m, qid_9p *q)
{
	q->type = g8(m);
	q->vers = g32(m);
	q->path = g64(m);
}

/* points into the message, n set to its length */
static char *
gstr(msg_9p *m, int *n)
{
	char *s;

	*n = g16(m);
	need(m, *n);
	if (m->err)
		return NULL;
	s = (char *)m->p;
	m->p += *n;
	return s;
}

static int
strdup_9p(const char *s, int n, char **out)
{
	char *d;

	d = malloc_9p(n + 1);
	if (d == NULL)
		return ENOMEM;
	bcopy(s, d, n);
	d[n] = 0;
	*out = d;
	return 0;
}

static fid_9p
nextfid(mount_9p *nmp)
{
	fid_9p f;

	do
		f = OSIncrementAtomic((SInt32 *)&nmp->nfid) + 1;
	while (f == NOFID || f == 0);
	return f;
}

/* Linux errno, as 9P2000.L carries it, to ours */
static int
errno_9p(uint32_t e)
{
	static const struct { uint16_t l, d; } map[] = {
		{ 11, EAGAIN },		{ 35, EDEADLK },	{ 36, ENAMETOOLONG },
		{ 37, ENOLCK },		{ 38, ENOSYS },		{ 39, ENOTEMPTY },
		{ 40, ELOOP },		{ 61, ENOATTR },	{ 75, EOVERFLOW },
		{ 84, EILSEQ },		{ 95, ENOTSUP },	{ 110, ETIMEDOUT },
		{ 116, ESTALE },	{ 122, EDQUOT },
		{ 93, ENOATTR },	{ 96, ENOATTR },	/* untranslated Darwin host ENOATTR, ENODATA */
	};
	unsigned int i;

	if (e == 0)
		return EIO;
	if (e <= 34 && e != 11)
		return e;
	for (i = 0; i < nelem(map); i++)
		if (map[i].l == e)
			return map[i].d;
	return EIO;
}

static int
tinit(msg_9p *m, size_t size, uint8_t type, uint16_t tag)
{
	m->buf = malloc_9p(size);
	if (m->buf == NULL)
		return ENOMEM;
	m->p = m->buf + 4;
	m->end = m->buf + size;
	m->err = 0;
	p8(m, type);
	p16(m, tag);
	return 0;
}

/*
 * Send tx (built by tinit + p*), receive into a fresh buffer of up to
 * rxmax bytes. On success rx->p is past the R-header.
 */
static int
rpc(mount_9p *nmp, msg_9p *tx, msg_9p *rx, uint32_t rxmax)
{
	uint32_t txlen, rxlen;
	uint8_t type, ttype;
	int e;

	rx->buf = NULL;
	if (tx->err) {
		e = tx->err;
		goto out;
	}
	txlen = tx->p - tx->buf;
	tx->p = tx->buf;
	p32(tx, txlen);
	ttype = tx->buf[4];

	if (rxmax > nmp->msize)
		rxmax = nmp->msize;
	if (rxmax < HDRSZ + 4)
		rxmax = HDRSZ + 4;
	rx->buf = malloc_9p(rxmax);
	if (rx->buf == NULL) {
		e = ENOMEM;
		goto out;
	}
	if (ISSET(nmp->flags, FLAG_CHATTY9P))
		printf("9p: -> type %u len %u\n", ttype, txlen);
	e = v9p_chan_rpc(nmp->chan, tx->buf, txlen, rx->buf, rxmax, &rxlen);
	if (e)
		goto out;
	rx->p = rx->buf;
	rx->end = rx->buf + rxlen;
	rx->err = 0;
	g32(rx);
	type = g8(rx);
	g16(rx);
	if (ISSET(nmp->flags, FLAG_CHATTY9P))
		printf("9p: <- type %u len %u\n", type, rxlen);
	if (type == Rlerror)
		e = errno_9p(g32(rx));
	else if (type != ttype + 1)
		e = EPROTO;
out:
	free_9p(tx->buf);
	tx->buf = NULL;
	if (e) {
		free_9p(rx->buf);
		rx->buf = NULL;
	}
	return e;
}

static int
done(msg_9p *rx, int e)
{
	if (!e && rx->err)
		e = EPROTO;
	free_9p(rx->buf);
	return e;
}

__private_extern__ uint32_t
iosize_9p(mount_9p *nmp)
{
	return nmp->msize - IOHDRSZ;
}

__private_extern__ int
version_9p(mount_9p *nmp)
{
	msg_9p tx, rx;
	char *v;
	int e, n;

	if ((e = tinit(&tx, 64, Tversion, NOTAG)))
		return e;
	p32(&tx, nmp->msize);
	pstr(&tx, VERSION9P);
	if ((e = rpc(nmp, &tx, &rx, 128)))
		return e;
	n = g32(&rx);
	v = gstr(&rx, &e);
	if (rx.err || v == NULL || e != (int)strlen(VERSION9P) ||
	    strncmp(v, VERSION9P, e) != 0)
		return done(&rx, EPROTONOSUPPORT);
	if (n < nmp->msize)
		nmp->msize = n;
	return done(&rx, 0);
}

__private_extern__ int
attach_9p(mount_9p *nmp, fid_9p *fidp, qid_9p *qidp)
{
	msg_9p tx, rx;
	fid_9p fid;
	int e;

	if ((e = tinit(&tx, 128, Tattach, 1)))
		return e;
	fid = nextfid(nmp);
	p32(&tx, fid);
	p32(&tx, NOFID);
	pstr(&tx, "");
	pstr(&tx, "");
	p32(&tx, nmp->uid);
	if ((e = rpc(nmp, &tx, &rx, 64)))
		return e;
	gqid(&rx, qidp);
	*fidp = fid;
	return done(&rx, 0);
}

__private_extern__ int
walk_9p(mount_9p *nmp, fid_9p fid, char *name, int nname, fid_9p *fidp, qid_9p *qidp)
{
	msg_9p tx, rx;
	fid_9p nfid;
	int e, n;

	if ((e = tinit(&tx, 32 + nname, Twalk, 1)))
		return e;
	nfid = nextfid(nmp);
	p32(&tx, fid);
	p32(&tx, nfid);
	if (name) {
		p16(&tx, 1);
		pstrn(&tx, name, nname);
	} else
		p16(&tx, 0);
	if ((e = rpc(nmp, &tx, &rx, 64)))
		return e;
	n = g16(&rx);
	if (name && n != 1) {
		/* a short walk does not create newfid */
		return done(&rx, ENOENT);
	}
	if (n)
		gqid(&rx, qidp);
	*fidp = nfid;
	return done(&rx, 0);
}

__private_extern__ int
getattr_9p(mount_9p *nmp, fid_9p fid, attr_9p *ap)
{
	msg_9p tx, rx;
	int e;

	if ((e = tinit(&tx, 32, Tgetattr, 1)))
		return e;
	p32(&tx, fid);
	p64(&tx, 0x3fff);	/* basic + btime + gen + data_version */
	if ((e = rpc(nmp, &tx, &rx, 256)))
		return e;
	g64(&rx);		/* valid */
	gqid(&rx, &ap->qid);
	ap->mode = g32(&rx);
	ap->uid = g32(&rx);
	ap->gid = g32(&rx);
	ap->nlink = g64(&rx);
	ap->rdev = g64(&rx);
	ap->size = g64(&rx);
	g64(&rx);		/* blksize */
	ap->blocks = g64(&rx);
	ap->atime.tv_sec = g64(&rx);
	ap->atime.tv_nsec = g64(&rx);
	ap->mtime.tv_sec = g64(&rx);
	ap->mtime.tv_nsec = g64(&rx);
	ap->ctime.tv_sec = g64(&rx);
	ap->ctime.tv_nsec = g64(&rx);
	ap->btime.tv_sec = g64(&rx);
	ap->btime.tv_nsec = g64(&rx);
	return done(&rx, 0);
}

__private_extern__ int
setattr_9p(mount_9p *nmp, fid_9p fid, uint32_t valid, uint32_t mode, uint64_t size,
	   struct timespec *atime, struct timespec *mtime)
{
	msg_9p tx, rx;
	int e;

	if ((e = tinit(&tx, 80, Tsetattr, 1)))
		return e;
	p32(&tx, fid);
	p32(&tx, valid);
	p32(&tx, mode);
	p32(&tx, 0);
	p32(&tx, 0);
	p64(&tx, size);
	p64(&tx, atime ? atime->tv_sec : 0);
	p64(&tx, atime ? atime->tv_nsec : 0);
	p64(&tx, mtime ? mtime->tv_sec : 0);
	p64(&tx, mtime ? mtime->tv_nsec : 0);
	if ((e = rpc(nmp, &tx, &rx, 32)))
		return e;
	return done(&rx, 0);
}

__private_extern__ int
lopen_9p(mount_9p *nmp, fid_9p fid, uint32_t flags, qid_9p *qidp, uint32_t *iounit)
{
	msg_9p tx, rx;
	int e;

	if ((e = tinit(&tx, 32, Tlopen, 1)))
		return e;
	p32(&tx, fid);
	p32(&tx, flags);
	if ((e = rpc(nmp, &tx, &rx, 64)))
		return e;
	gqid(&rx, qidp);
	*iounit = g32(&rx);
	return done(&rx, 0);
}

__private_extern__ int
lcreate_9p(mount_9p *nmp, fid_9p fid, char *name, int nname, uint32_t flags,
	   uint32_t mode, qid_9p *qidp, uint32_t *iounit)
{
	msg_9p tx, rx;
	int e;

	if ((e = tinit(&tx, 32 + nname, Tlcreate, 1)))
		return e;
	p32(&tx, fid);
	pstrn(&tx, name, nname);
	p32(&tx, flags);
	p32(&tx, mode);
	p32(&tx, nmp->gid);
	if ((e = rpc(nmp, &tx, &rx, 64)))
		return e;
	gqid(&rx, qidp);
	*iounit = g32(&rx);
	return done(&rx, 0);
}

__private_extern__ int
mkdir_9p(mount_9p *nmp, fid_9p dfid, char *name, int nname, uint32_t mode, qid_9p *qidp)
{
	msg_9p tx, rx;
	int e;

	if ((e = tinit(&tx, 32 + nname, Tmkdir, 1)))
		return e;
	p32(&tx, dfid);
	pstrn(&tx, name, nname);
	p32(&tx, mode);
	p32(&tx, nmp->gid);
	if ((e = rpc(nmp, &tx, &rx, 64)))
		return e;
	gqid(&rx, qidp);
	return done(&rx, 0);
}

__private_extern__ int
symlink_9p(mount_9p *nmp, fid_9p dfid, char *name, int nname, char *target, qid_9p *qidp)
{
	msg_9p tx, rx;
	int e;

	if ((e = tinit(&tx, 32 + nname + strlen(target), Tsymlink, 1)))
		return e;
	p32(&tx, dfid);
	pstrn(&tx, name, nname);
	pstr(&tx, target);
	p32(&tx, nmp->gid);
	if ((e = rpc(nmp, &tx, &rx, 64)))
		return e;
	gqid(&rx, qidp);
	return done(&rx, 0);
}

__private_extern__ int
readlink_9p(mount_9p *nmp, fid_9p fid, char **target)
{
	msg_9p tx, rx;
	char *s;
	int e, n;

	if ((e = tinit(&tx, 16, Treadlink, 1)))
		return e;
	p32(&tx, fid);
	if ((e = rpc(nmp, &tx, &rx, MAXPATHLEN + 16)))
		return e;
	s = gstr(&rx, &n);
	if (s == NULL)
		return done(&rx, EPROTO);
	e = strdup_9p(s, n, target);
	return done(&rx, e);
}

__private_extern__ int
read_9p(mount_9p *nmp, fid_9p fid, void *buf, uint32_t count, uint64_t off, uint32_t *nread)
{
	msg_9p tx, rx;
	uint32_t n;
	int e;

	*nread = 0;
	if (count > iosize_9p(nmp))
		count = iosize_9p(nmp);
	if ((e = tinit(&tx, 32, Tread, 1)))
		return e;
	p32(&tx, fid);
	p64(&tx, off);
	p32(&tx, count);
	if ((e = rpc(nmp, &tx, &rx, HDRSZ + 4 + count)))
		return e;
	n = g32(&rx);
	if (n > count || rx.p + n > rx.end)
		return done(&rx, EPROTO);
	bcopy(rx.p, buf, n);
	*nread = n;
	return done(&rx, 0);
}

__private_extern__ int
write_9p(mount_9p *nmp, fid_9p fid, void *buf, uint32_t count, uint64_t off, uint32_t *nwrite)
{
	msg_9p tx, rx;
	int e;

	*nwrite = 0;
	if (count > iosize_9p(nmp))
		count = iosize_9p(nmp);
	if ((e = tinit(&tx, IOHDRSZ + count, Twrite, 1)))
		return e;
	p32(&tx, fid);
	p64(&tx, off);
	p32(&tx, count);
	pbytes(&tx, buf, count);
	if ((e = rpc(nmp, &tx, &rx, 32)))
		return e;
	*nwrite = g32(&rx);
	if (*nwrite > count)
		*nwrite = count;
	return done(&rx, 0);
}

__private_extern__ int
clunk_9p(mount_9p *nmp, fid_9p fid)
{
	msg_9p tx, rx;
	int e;

	if (fid == NOFID)
		return EBADF;
	if ((e = tinit(&tx, 16, Tclunk, 1)))
		return e;
	p32(&tx, fid);
	if ((e = rpc(nmp, &tx, &rx, 32)))
		return e;
	return done(&rx, 0);
}

__private_extern__ int
unlinkat_9p(mount_9p *nmp, fid_9p dfid, char *name, int nname, uint32_t flags)
{
	msg_9p tx, rx;
	int e;

	if ((e = tinit(&tx, 32 + nname, Tunlinkat, 1)))
		return e;
	p32(&tx, dfid);
	pstrn(&tx, name, nname);
	p32(&tx, flags);
	if ((e = rpc(nmp, &tx, &rx, 32)))
		return e;
	return done(&rx, 0);
}

__private_extern__ int
renameat_9p(mount_9p *nmp, fid_9p ofid, char *oname, int noname, fid_9p nfid,
	    char *nname, int nnname)
{
	msg_9p tx, rx;
	int e;

	if ((e = tinit(&tx, 32 + noname + nnname, Trenameat, 1)))
		return e;
	p32(&tx, ofid);
	pstrn(&tx, oname, noname);
	p32(&tx, nfid);
	pstrn(&tx, nname, nnname);
	if ((e = rpc(nmp, &tx, &rx, 32)))
		return e;
	return done(&rx, 0);
}

__private_extern__ int
statfs_9p(mount_9p *nmp, fid_9p fid, fsstat_9p *sp)
{
	msg_9p tx, rx;
	int e;

	if ((e = tinit(&tx, 16, Tstatfs, 1)))
		return e;
	p32(&tx, fid);
	if ((e = rpc(nmp, &tx, &rx, 128)))
		return e;
	g32(&rx);		/* type */
	sp->bsize = g32(&rx);
	sp->blocks = g64(&rx);
	sp->bfree = g64(&rx);
	sp->bavail = g64(&rx);
	sp->files = g64(&rx);
	sp->ffree = g64(&rx);
	return done(&rx, 0);
}

__private_extern__ int
fsync_9p(mount_9p *nmp, fid_9p fid)
{
	msg_9p tx, rx;
	int e;

	if ((e = tinit(&tx, 16, Tfsync, 1)))
		return e;
	p32(&tx, fid);
	p32(&tx, 0);
	if ((e = rpc(nmp, &tx, &rx, 32)))
		return e;
	return done(&rx, 0);
}

/* newfid reads the value (or, for name "", the name list) */
__private_extern__ int
xattrwalk_9p(mount_9p *nmp, fid_9p fid, const char *name, fid_9p *fidp, uint64_t *sizep)
{
	msg_9p tx, rx;
	fid_9p nfid;
	int e;

	if ((e = tinit(&tx, 32 + strlen(name), Txattrwalk, 1)))
		return e;
	nfid = nextfid(nmp);
	p32(&tx, fid);
	p32(&tx, nfid);
	pstr(&tx, name);
	if ((e = rpc(nmp, &tx, &rx, 32)))
		return e;
	*sizep = g64(&rx);
	*fidp = nfid;
	return done(&rx, 0);
}

/* fid becomes a write handle for the value; it is set on clunk */
__private_extern__ int
xattrcreate_9p(mount_9p *nmp, fid_9p fid, const char *name, uint64_t size, uint32_t flags)
{
	msg_9p tx, rx;
	int e;

	if ((e = tinit(&tx, 32 + strlen(name), Txattrcreate, 1)))
		return e;
	p32(&tx, fid);
	pstr(&tx, name);
	p64(&tx, size);
	p32(&tx, flags);
	if ((e = rpc(nmp, &tx, &rx, 32)))
		return e;
	return done(&rx, 0);
}

/* the whole directory, "." and ".." left out */
__private_extern__ int
readdirs_9p(mount_9p *nmp, fid_9p fid, dirent_9p **dp, uint32_t *ndp)
{
	msg_9p tx, rx, raw;
	uint8_t *all, *nall;
	uint32_t n, total, cap, nd, i;
	uint64_t off;
	dirent_9p *d;
	char *names, *s;
	qid_9p q;
	int e, len;

	*dp = NULL;
	*ndp = 0;
	all = NULL;
	total = cap = 0;
	off = 0;
	for (;;) {
		if ((e = tinit(&tx, 32, Treaddir, 1)))
			goto out;
		p32(&tx, fid);
		p64(&tx, off);
		p32(&tx, iosize_9p(nmp));
		if ((e = rpc(nmp, &tx, &rx, nmp->msize)))
			goto out;
		n = g32(&rx);
		if (rx.err || rx.p + n > rx.end) {
			e = done(&rx, EPROTO);
			goto out;
		}
		if (n == 0) {
			done(&rx, 0);
			break;
		}
		if (total + n > cap) {
			cap = (total + n) * 2;
			nall = malloc_9p(cap);
			if (nall == NULL) {
				e = done(&rx, ENOMEM);
				goto out;
			}
			if (all)
				bcopy(all, nall, total);
			free_9p(all);
			all = nall;
		}
		bcopy(rx.p, all + total, n);
		total += n;
		/* next offset is the last entry's */
		raw.buf = raw.p = rx.p;
		raw.end = rx.p + n;
		raw.err = 0;
		while (raw.p < raw.end && !raw.err) {
			gqid(&raw, &q);
			off = g64(&raw);
			g8(&raw);
			gstr(&raw, &len);
		}
		e = done(&rx, raw.err ? EPROTO : 0);
		if (e)
			goto out;
	}

	/* count, then unpack */
	nd = 0;
	raw.p = all;
	raw.end = all + total;
	raw.err = 0;
	while (raw.p < raw.end && !raw.err) {
		gqid(&raw, &q);
		g64(&raw);
		g8(&raw);
		s = gstr(&raw, &len);
		if (s && !(len == 1 && s[0] == '.') && !(len == 2 && s[0] == '.' && s[1] == '.'))
			nd++;
	}
	if (raw.err) {
		e = EPROTO;
		goto out;
	}
	d = malloc_9p(nd * sizeof(*d) + total + nd);
	if (d == NULL) {
		e = ENOMEM;
		goto out;
	}
	names = (char *)(d + nd);
	raw.p = all;
	i = 0;
	while (raw.p < raw.end && i < nd) {
		gqid(&raw, &q);
		g64(&raw);
		d[i].type = g8(&raw);
		s = gstr(&raw, &len);
		if ((len == 1 && s[0] == '.') || (len == 2 && s[0] == '.' && s[1] == '.'))
			continue;
		d[i].qid = q;
		d[i].name = names;
		bcopy(s, names, len);
		names[len] = 0;
		names += len + 1;
		i++;
	}
	*dp = d;
	*ndp = nd;
	e = 0;
out:
	free_9p(all);
	return e;
}
