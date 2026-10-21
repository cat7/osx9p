/*
 * Advisory locks (fcntl and flock), kept in the guest: the 9P server does
 * not enforce them, so they hold between guest processes only.
 */
#include "9p.h"
#include <sys/proc.h>

struct lock_9p {
	struct lock_9p *next;
	caddr_t id;		/* proc for fcntl locks, file for flock */
	int flags;		/* F_POSIX or F_FLOCK */
	short type;		/* F_RDLCK or F_WRLCK */
	pid_t pid;
	off_t start, end;	/* inclusive; end -1 = to EOF and beyond */
};

#ifdef PANTHER
/* the kernel funnel serialises us until we sleep: a flag is the mutex */
static int lockbusy_9p, lockwant_9p;

static void
lkenter(void)
{
	while (lockbusy_9p) {
		lockwant_9p = 1;
		tsleep(&lockbusy_9p, PLOCK, "9p_lktab", 0);
	}
	lockbusy_9p = 1;
}

static void
lkexit(void)
{
	lockbusy_9p = 0;
	if (lockwant_9p) {
		lockwant_9p = 0;
		wakeup(&lockbusy_9p);
	}
}

static int
lksleep(void *chan)
{
	int e;

	lkexit();
	e = tsleep(chan, PLOCK | PCATCH, "9p_lock", 0);
	lkenter();
	return e;
}

__private_extern__ void
lock_init_9p(void)
{
}

__private_extern__ void
lock_fini_9p(void)
{
}

#define lkpid(id)	(((struct proc *)(id))->p_pid)
#define advlock_args	vop_advlock_args
#else
static lck_mtx_t *lockmtx_9p;

static void
lkenter(void)
{
	lck_mtx_lock(lockmtx_9p);
}

static void
lkexit(void)
{
	lck_mtx_unlock(lockmtx_9p);
}

static int
lksleep(void *chan)
{
	return msleep(chan, lockmtx_9p, PLOCK | PCATCH, "9p_lock", NULL);
}

__private_extern__ void
lock_init_9p(void)
{
	lockmtx_9p = lck_mtx_alloc_init(lck_grp_9p, LCK_ATTR_NULL);
}

__private_extern__ void
lock_fini_9p(void)
{
	if (lockmtx_9p)
		lck_mtx_free(lockmtx_9p, lck_grp_9p);
	lockmtx_9p = NULL;
}

#define lkpid(id)	proc_pid((proc_t)(id))
#define advlock_args	vnop_advlock_args
#endif

static int
overlaps(struct lock_9p *l, off_t start, off_t end)
{
	return (l->end == -1 || l->end >= start) && (end == -1 || l->start <= end);
}

static struct lock_9p *
conflict(node_9p *np, caddr_t id, short type, off_t start, off_t end)
{
	struct lock_9p *l;

	for (l = np->locks; l; l = l->next) {
		if (l->id == id)
			continue;
		if (l->type == F_RDLCK && type == F_RDLCK)
			continue;
		if (overlaps(l, start, end))
			return l;
	}
	return NULL;
}

/* drop id's locks in [start, end], splitting where needed */
static int
release(node_9p *np, caddr_t id, off_t start, off_t end)
{
	struct lock_9p **lp, *l, *n;

	for (lp = &np->locks; (l = *lp) != NULL; ) {
		if (l->id != id || !overlaps(l, start, end)) {
			lp = &l->next;
			continue;
		}
		if (l->start < start && end != -1 && (l->end == -1 || l->end > end)) {
			/* the range is inside l: keep both ends */
			n = malloc_9p(sizeof(*n));
			if (n == NULL)
				return ENOLCK;
			*n = *l;
			n->start = end + 1;
			l->end = start - 1;
			n->next = l->next;
			l->next = n;
			lp = &n->next;
		} else if (l->start < start) {
			l->end = start - 1;
			lp = &l->next;
		} else if (end != -1 && (l->end == -1 || l->end > end)) {
			l->start = end + 1;
			lp = &l->next;
		} else {
			*lp = l->next;
			free_9p(l);
		}
	}
	return 0;
}

static void
wake(node_9p *np)
{
	wakeup(&np->locks);
}

__private_extern__ void
lock_free_9p(node_9p *np)
{
	struct lock_9p *l;

	lkenter();
	while ((l = np->locks) != NULL) {
		np->locks = l->next;
		free_9p(l);
	}
	lkexit();
}

__private_extern__ int
vnop_advlock_9p(struct advlock_args *ap)
{
	struct flock *fl;
	struct lock_9p *l, *n;
	node_9p *np;
	off_t start, end;
	int e;

	np = NTO9P(ap->a_vp);
	fl = ap->a_fl;

	switch (fl->l_whence) {
	case SEEK_SET:
	case SEEK_CUR:	/* the caller has made it absolute */
		start = fl->l_start;
		break;
	case SEEK_END:
#ifdef PANTHER
		start = np->attr.size;
		if (UBCINFOEXISTS(ap->a_vp))
			start = MAX(start, ubc_getsize(ap->a_vp));
		start += fl->l_start;
#else
		nlock_9p(np, NODE_LCK_SHARED);
		start = MAX((off_t)np->attr.size, ubc_getsize(ap->a_vp)) + fl->l_start;
		nunlock_9p(np);
#endif
		break;
	default:
		return EINVAL;
	}
	if (fl->l_len < 0) {
		start += fl->l_len;
		end = start - fl->l_len - 1;
	} else if (fl->l_len == 0)
		end = -1;
	else
		end = start + fl->l_len - 1;
	if (start < 0)
		return EINVAL;

	lkenter();
	e = 0;
	switch (ap->a_op) {
	case F_GETLK:
		l = conflict(np, ap->a_id, fl->l_type, start, end);
		if (l) {
			fl->l_whence = SEEK_SET;
			fl->l_start = l->start;
			fl->l_len = l->end == -1 ? 0 : l->end - l->start + 1;
			fl->l_type = l->type;
			fl->l_pid = l->pid;
		} else
			fl->l_type = F_UNLCK;
		break;

	case F_UNLCK:
		e = release(np, ap->a_id, start, end);
		wake(np);
		break;

	case F_SETLK:
		if (fl->l_type != F_RDLCK && fl->l_type != F_WRLCK) {
			e = EINVAL;
			break;
		}
		while (conflict(np, ap->a_id, fl->l_type, start, end)) {
			if (!ISSET(ap->a_flags, F_WAIT)) {
				e = EAGAIN;
				goto out;
			}
			e = lksleep(&np->locks);
			if (e)
				goto out;
		}
		n = malloc_9p(sizeof(*n));
		if (n == NULL) {
			e = ENOLCK;
			break;
		}
		/* a new lock replaces the owner's own in the range */
		if ((e = release(np, ap->a_id, start, end))) {
			free_9p(n);
			break;
		}
		n->id = ap->a_id;
		n->flags = ap->a_flags & (F_POSIX | F_FLOCK);
		n->type = fl->l_type;
		n->pid = ISSET(ap->a_flags, F_POSIX) ? lkpid(ap->a_id) : -1;
		n->start = start;
		n->end = end;
		n->next = np->locks;
		np->locks = n;
		wake(np);	/* a downgrade may unblock readers */
		break;

	default:
		e = EINVAL;
		break;
	}
out:
	lkexit();
	return e;
}
