/*
 * 32-bit file IDs in HFS style: the root is 2, its parent 1. A file keeps
 * its host inode number when that fits, so IDs survive a remount. Each ID
 * remembers its parent and name, so vfs_vget can find files by ID alone
 * (volfs, used by Carbon on 10.4).
 */
#include "9p.h"

#define ID_ROOT		2
#define ID_ROOTPARENT	1
#define ID_FIRSTSPARE	0xF0000000U
#define IDHASH		1024

struct id_9p {
	LIST_ENTRY(id_9p) byid, byqid;
	uint32_t id;
	uint32_t parent;
	uint64_t qpath;
	char *name;
};

LIST_HEAD(idhead_9p, id_9p);

struct idtab_9p {
	lck_mtx_t *mtx;
	struct idhead_9p byid[IDHASH];
	struct idhead_9p byqid[IDHASH];
	uint64_t rootqpath;
	uint32_t spare;
};

static struct id_9p *
findid(struct idtab_9p *t, uint32_t id)
{
	struct id_9p *r;

	LIST_FOREACH(r, &t->byid[id % IDHASH], byid)
		if (r->id == id)
			return r;
	return NULL;
}

static struct id_9p *
findqid(struct idtab_9p *t, uint64_t qpath)
{
	struct id_9p *r;

	LIST_FOREACH(r, &t->byqid[qpath % IDHASH], byqid)
		if (r->qpath == qpath)
			return r;
	return NULL;
}

__private_extern__ int
ids_init_9p(mount_9p *nmp, uint64_t rootqpath)
{
	struct idtab_9p *t;
	int i;

	t = malloc_9p(sizeof(*t));
	if (t == NULL)
		return ENOMEM;
	t->mtx = lck_mtx_alloc_init(lck_grp_9p, LCK_ATTR_NULL);
	if (t->mtx == NULL) {
		free_9p(t);
		return ENOMEM;
	}
	for (i = 0; i < IDHASH; i++) {
		LIST_INIT(&t->byid[i]);
		LIST_INIT(&t->byqid[i]);
	}
	t->rootqpath = rootqpath;
	t->spare = ID_FIRSTSPARE;
	nmp->ids = t;
	return 0;
}

__private_extern__ void
ids_fini_9p(mount_9p *nmp)
{
	struct idtab_9p *t;
	struct id_9p *r;
	int i;

	t = nmp->ids;
	if (t == NULL)
		return;
	for (i = 0; i < IDHASH; i++)
		while ((r = LIST_FIRST(&t->byid[i])) != NULL) {
			LIST_REMOVE(r, byid);
			LIST_REMOVE(r, byqid);
			free_9p(r->name);
			free_9p(r);
		}
	lck_mtx_free(t->mtx, lck_grp_9p);
	free_9p(t);
	nmp->ids = NULL;
}

/* the ID for a host object, made on first sight */
__private_extern__ uint32_t
id_get_9p(mount_9p *nmp, uint64_t qpath)
{
	struct idtab_9p *t;
	struct id_9p *r;
	uint32_t id;

	t = nmp->ids;
	if (qpath == t->rootqpath)
		return ID_ROOT;
	lck_mtx_lock(t->mtx);
	r = findqid(t, qpath);
	if (r) {
		id = r->id;
		goto out;
	}
	id = 0;
	if (qpath > ID_ROOT && qpath < ID_FIRSTSPARE && findid(t, (uint32_t)qpath) == NULL)
		id = (uint32_t)qpath;
	while (id == 0) {
		if (++t->spare == 0)
			t->spare = ID_FIRSTSPARE;
		if (findid(t, t->spare) == NULL)
			id = t->spare;
	}
	lck_mtx_unlock(t->mtx);

	r = malloc_9p(sizeof(*r));
	if (r == NULL)
		return id;
	lck_mtx_lock(t->mtx);
	if (findqid(t, qpath)) {
		/* raced */
		id = findqid(t, qpath)->id;
		free_9p(r);
		goto out;
	}
	r->id = id;
	r->qpath = qpath;
	LIST_INSERT_HEAD(&t->byid[id % IDHASH], r, byid);
	LIST_INSERT_HEAD(&t->byqid[qpath % IDHASH], r, byqid);
out:
	lck_mtx_unlock(t->mtx);
	return id;
}

/* record where id lives */
__private_extern__ void
id_link_9p(mount_9p *nmp, uint32_t id, uint32_t parent, const char *name, int nname)
{
	struct idtab_9p *t;
	struct id_9p *r;
	char *s;

	t = nmp->ids;
	if (id == ID_ROOT)
		return;
	s = malloc_9p(nname + 1);
	if (s == NULL)
		return;
	bcopy(name, s, nname);
	s[nname] = 0;
	lck_mtx_lock(t->mtx);
	r = findid(t, id);
	if (r) {
		r->parent = parent;
		free_9p(r->name);
		r->name = s;
		s = NULL;
	}
	lck_mtx_unlock(t->mtx);
	free_9p(s);
}

__private_extern__ void
id_forget_9p(mount_9p *nmp, uint32_t id)
{
	struct idtab_9p *t;
	struct id_9p *r;

	t = nmp->ids;
	lck_mtx_lock(t->mtx);
	r = findid(t, id);
	if (r) {
		LIST_REMOVE(r, byid);
		LIST_REMOVE(r, byqid);
	}
	lck_mtx_unlock(t->mtx);
	if (r) {
		free_9p(r->name);
		free_9p(r);
	}
}

__private_extern__ uint32_t
id_parent_9p(mount_9p *nmp, uint32_t id)
{
	struct idtab_9p *t;
	struct id_9p *r;
	uint32_t p;

	if (id == ID_ROOT)
		return ID_ROOTPARENT;
	t = nmp->ids;
	lck_mtx_lock(t->mtx);
	r = findid(t, id);
	p = r && r->parent ? r->parent : ID_ROOT;
	lck_mtx_unlock(t->mtx);
	return p;
}

/* parent, name (caller frees) and host object of id */
static int
id_where(mount_9p *nmp, uint32_t id, uint32_t *parent, char **name, uint64_t *qpath)
{
	struct idtab_9p *t;
	struct id_9p *r;
	char *s;
	int n;

	t = nmp->ids;
	lck_mtx_lock(t->mtx);
	r = findid(t, id);
	if (r == NULL || r->name == NULL) {
		lck_mtx_unlock(t->mtx);
		return ENOENT;
	}
	*parent = r->parent;
	*qpath = r->qpath;
	n = strlen(r->name);
	lck_mtx_unlock(t->mtx);

	s = malloc_9p(n + 1);
	if (s == NULL)
		return ENOMEM;
	lck_mtx_lock(t->mtx);
	r = findid(t, id);
	if (r == NULL || r->name == NULL || (int)strlen(r->name) != n) {
		lck_mtx_unlock(t->mtx);
		free_9p(s);
		return ENOENT;
	}
	bcopy(r->name, s, n + 1);
	lck_mtx_unlock(t->mtx);
	*name = s;
	return 0;
}

/* the vnode for id, with an iocount, unlocked */
__private_extern__ int
id_vget_9p(mount_9p *nmp, uint32_t id, vnode_t *vpp, vfs_context_t ctx, int depth)
{
	uint32_t parent;
	uint64_t qpath;
	node_9p *dnp;
	vnode_t dvp, vp;
	fid_9p fid;
	qid_9p qid;
	char *name;
	int e;

	*vpp = NULL;
	if (id == ID_ROOT || id == ID_ROOTPARENT) {
		if ((e = vnode_get(nmp->root)))
			return e;
		*vpp = nmp->root;
		return 0;
	}
	if (depth > 256)
		return ELOOP;
	if ((e = id_where(nmp, id, &parent, &name, &qpath)))
		return e;

	/* cached? */
	bzero(&qid, sizeof(qid));
	qid.path = qpath;
	if (nget_9p(nmp, NOFID, qid, NULL, &vp, NULL, ctx) == 0) {
		nunlock_9p(NTO9P(vp));
		free_9p(name);
		*vpp = vp;
		return 0;
	}

	if ((e = id_vget_9p(nmp, parent, &dvp, ctx, depth + 1))) {
		free_9p(name);
		return e;
	}
	dnp = NTO9P(dvp);
	nlock_9p(dnp, NODE_LCK_SHARED);
	e = walk_9p(nmp, dnp->fid, name, strlen(name), &fid, &qid);
	nunlock_9p(dnp);
	if (e == 0 && qid.path != qpath) {
		/* replaced on the host */
		clunk_9p(nmp, fid);
		e = ENOENT;
	}
	if (e == 0) {
		e = nget_9p(nmp, fid, qid, dvp, &vp, NULL, ctx);
		if (e || NTO9P(vp)->fid != fid)
			clunk_9p(nmp, fid);
		if (e == 0) {
			nunlock_9p(NTO9P(vp));
			*vpp = vp;
		}
	}
	vnode_put(dvp);
	free_9p(name);
	return e;
}
