/*
 * virtio-9p PCI transport (virtio 1.0, split ring, one request in flight).
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSByteOrder.h>
#include <libkern/c++/OSArray.h>
#include <kern/clock.h>
#include <sys/errno.h>
#include "virtio9p.h"

#define super IOService

enum {
	CAP_COMMON	= 1,
	CAP_NOTIFY	= 2,
	CAP_ISR		= 3,
	CAP_DEVICE	= 4,

	ST_ACK		= 1,
	ST_DRIVER	= 2,
	ST_DRIVER_OK	= 4,
	ST_FEATURES_OK	= 8,

	/* common config */
	CC_DFSELECT	= 0,
	CC_DF		= 4,
	CC_GFSELECT	= 8,
	CC_GF		= 12,
	CC_STATUS	= 20,
	CC_QSELECT	= 22,
	CC_QSIZE	= 24,
	CC_QENABLE	= 28,
	CC_QNOTIFYOFF	= 30,
	CC_QDESC	= 32,
	CC_QAVAIL	= 40,
	CC_QUSED	= 48,

	VRING_DESC_F_NEXT	= 1,
	VRING_DESC_F_WRITE	= 2,

	QSIZE		= 8,
	RING_BYTES	= 4096,
	USED_OFF	= 2048,
	MSIZE_MAX	= 128 * 1024,
	MSIZE_MIN	= 16 * 1024,
};

struct Region {
	IOMemoryMap	*map;
	volatile uint8_t *base;
};

class org_cat7_virtio9p : public IOService
{
	OSDeclareDefaultStructors(org_cat7_virtio9p)

public:
	virtual bool start(IOService *provider);
	virtual void stop(IOService *provider);
	virtual void free(void);
	virtual bool terminate(IOOptionBits options = 0);

	SInt32		fUsers;		/* open channels; vetoes terminate() */

	int rpc(const void *tx, uint32_t txlen, void *rx, uint32_t rxmax, uint32_t *rxlen);
	uint32_t msize(void) { return fMsize; }
	const char *tag(void) { return fTag; }

private:
	bool mapRegion(Region *r, uint8_t bar, uint32_t off, uint32_t len);
	bool setupQueue(void);
	void reset(void);
	void setIcon(void);
	static bool filter(OSObject *owner, IOFilterInterruptEventSource *src);
	static void handler(OSObject *owner, IOInterruptEventSource *src, int count);

	uint8_t  r8(Region &r, uint32_t o)  { return r.base[o]; }
	uint16_t r16(Region &r, uint32_t o) { return OSReadLittleInt16((void *)r.base, o); }
	uint32_t r32(Region &r, uint32_t o) { return OSReadLittleInt32((void *)r.base, o); }
	void w8(Region &r, uint32_t o, uint8_t v)   { r.base[o] = v; OSSynchronizeIO(); }
	void w16(Region &r, uint32_t o, uint16_t v) { OSWriteLittleInt16((void *)r.base, o, v); OSSynchronizeIO(); }
	void w32(Region &r, uint32_t o, uint32_t v) { OSWriteLittleInt32((void *)r.base, o, v); OSSynchronizeIO(); }
	void w64(Region &r, uint32_t o, uint64_t v) { w32(r, o, (uint32_t)v); w32(r, o + 4, (uint32_t)(v >> 32)); }

	IOPCIDevice	*fPCI;
	Region		fCommon, fNotify, fISR, fDevice;
	uint32_t	fNotifyMult;
	volatile uint8_t *fNotifyAddr;

	IOBufferMemoryDescriptor *fRing, *fTx, *fRx;
	uint8_t		*fRingVA;
	uint64_t	fRingPA, fTxPA, fRxPA;
	uint16_t	fQSize;
	uint16_t	fAvailIdx, fUsedIdx;

	IOLock		*fRPCLock;	/* one request at a time */
	IOLock		*fWaitLock;	/* completion handshake */
	IOWorkLoop	*fWorkLoop;
	IOFilterInterruptEventSource *fIntr;
	volatile uint8_t fISRBits;
	bool		fDead;

	uint32_t	fMsize;
	char		fTag[256];
};

OSDefineMetaClassAndStructors(org_cat7_virtio9p, IOService)

static OSArray *gDevices;
static IOLock *gDevLock;

extern "C" void
v9p_transport_init(void)
{
	gDevLock = IOLockAlloc();
	gDevices = OSArray::withCapacity(2);
}

extern "C" void
v9p_transport_fini(void)
{
	if (gDevices)
		gDevices->release();
	gDevices = NULL;
	if (gDevLock)
		IOLockFree(gDevLock);
	gDevLock = NULL;
}

bool
org_cat7_virtio9p::mapRegion(Region *r, uint8_t bar, uint32_t off, uint32_t len)
{
	IODeviceMemory *mem;
	uint32_t lo, hi;
	uint64_t pa;

	if (bar > 5)
		return false;
	r->map = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0 + 4 * bar);
	if (r->map && r->map->getLength() >= off + len) {
		r->base = (volatile uint8_t *)r->map->getVirtualAddress() + off;
		return true;
	}
	if (r->map)
		r->map->release();
	r->map = NULL;

	/* The firmware did not describe the BAR; map what it holds. */
	lo = fPCI->configRead32(kIOPCIConfigBaseAddress0 + 4 * bar);
	if (lo & 1)
		return false;
	pa = lo & ~0xfU;
	if ((lo & 6) == 4 && bar < 5) {
		hi = fPCI->configRead32(kIOPCIConfigBaseAddress0 + 4 * (bar + 1));
		pa |= (uint64_t)hi << 32;
	}
	if (pa == 0 || (pa >> 32))
		return false;
	mem = IODeviceMemory::withRange((IOPhysicalAddress)(pa + off), len);
	if (!mem)
		return false;
	r->map = mem->map(kIOMapInhibitCache);
	mem->release();
	if (!r->map)
		return false;
	r->base = (volatile uint8_t *)r->map->getVirtualAddress();
	return true;
}

static IOBufferMemoryDescriptor *
dmaAlloc(uint32_t size, uint64_t *pa, uint8_t **va)
{
	IOBufferMemoryDescriptor *md;
	IOByteCount seglen;

	md = IOBufferMemoryDescriptor::withOptions(kIOMemoryPhysicallyContiguous,
						    size, PAGE_SIZE);
	if (!md)
		return NULL;
	if (md->prepare() != kIOReturnSuccess) {
		md->release();
		return NULL;
	}
	/* the device bypasses the DART: CPU physical, not mapped, addresses */
	*pa = md->getPhysicalSegment64(0, &seglen);
	if (*pa == 0 || seglen < size) {
		md->complete();
		md->release();
		return NULL;
	}
	if (va)
		*va = (uint8_t *)md->getBytesNoCopy();
	bzero(md->getBytesNoCopy(), size);
	return md;
}

static void
dmaFree(IOBufferMemoryDescriptor *md)
{
	if (md) {
		md->complete();
		md->release();
	}
}

void
org_cat7_virtio9p::reset(void)
{
	if (fCommon.base) {
		w8(fCommon, CC_STATUS, 0);
		while (r8(fCommon, CC_STATUS) != 0)
			IOSleep(1);
	}
}

bool
org_cat7_virtio9p::setupQueue(void)
{
	uint16_t qmax, off;

	w16(fCommon, CC_QSELECT, 0);
	qmax = r16(fCommon, CC_QSIZE);
	if (qmax == 0)
		return false;
	fQSize = qmax < QSIZE ? qmax : QSIZE;
	w16(fCommon, CC_QSIZE, fQSize);

	fRing = dmaAlloc(RING_BYTES, &fRingPA, &fRingVA);
	if (!fRing)
		return false;
	w64(fCommon, CC_QDESC, fRingPA);
	w64(fCommon, CC_QAVAIL, fRingPA + 16 * fQSize);
	w64(fCommon, CC_QUSED, fRingPA + USED_OFF);

	off = r16(fCommon, CC_QNOTIFYOFF);
	fNotifyAddr = fNotify.base + (uint32_t)off * fNotifyMult;
	w16(fCommon, CC_QENABLE, 1);
	return true;
}

/*
 * Carbon looks the volume's device up by BSD name (the part of
 * f_mntfromname after the last '/') and takes its IOMediaIcon; without
 * one, a local volume gets the folder icon.  Prefix as BSDNAME9P in 9p.h.
 */
void
org_cat7_virtio9p::setIcon(void)
{
	char name[sizeof(fTag) + 3];
	OSDictionary *d;
	OSString *s;

	bcopy("9p-", name, 3);
	bcopy(fTag, name + 3, strlen(fTag) + 1);
	setProperty("BSD Name", name);
	if ((d = OSDictionary::withCapacity(2)) == NULL)
		return;
	if ((s = OSString::withCString("com.apple.iokit.IOStorageFamily"))) {
		d->setObject("CFBundleIdentifier", s);
		s->release();
	}
	if ((s = OSString::withCString("External.icns"))) {
		d->setObject("IOBundleResourceFile", s);
		s->release();
	}
	setProperty("IOMediaIcon", d);
	d->release();
}

bool
org_cat7_virtio9p::start(IOService *provider)
{
	uint8_t cap, type, bar, st;
	uint32_t off, len, f0, f1, n, i;
	bool have[5] = { false };

	if (!super::start(provider))
		return false;
	fPCI = OSDynamicCast(IOPCIDevice, provider);
	if (!fPCI)
		return false;
	fPCI->retain();
	fPCI->setMemoryEnable(true);
	fPCI->setBusMasterEnable(true);

	if (!(fPCI->configRead16(kIOPCIConfigStatus) & 0x10))
		goto fail;
	for (cap = fPCI->configRead8(kIOPCIConfigCapabilitiesPtr) & 0xfc; cap;
	     cap = fPCI->configRead8(cap + 1) & 0xfc) {
		if (fPCI->configRead8(cap) != 0x09)
			continue;
		type = fPCI->configRead8(cap + 3);
		bar = fPCI->configRead8(cap + 4);
		off = fPCI->configRead32(cap + 8);
		len = fPCI->configRead32(cap + 12);
		if (type < CAP_COMMON || type > CAP_DEVICE || have[type])
			continue;
		Region *r = type == CAP_COMMON ? &fCommon :
			    type == CAP_NOTIFY ? &fNotify :
			    type == CAP_ISR ? &fISR : &fDevice;
		if (!mapRegion(r, bar, off, len)) {
			IOLog("virtio9p: cannot map BAR%u\n", bar);
			continue;
		}
		if (type == CAP_NOTIFY)
			fNotifyMult = fPCI->configRead32(cap + 16);
		have[type] = true;
	}
	if (!have[CAP_COMMON] || !have[CAP_NOTIFY] || !have[CAP_ISR] || !have[CAP_DEVICE]) {
		IOLog("virtio9p: no virtio 1.0 interface\n");
		goto fail;
	}

	reset();
	w8(fCommon, CC_STATUS, ST_ACK);
	w8(fCommon, CC_STATUS, ST_ACK | ST_DRIVER);
	w32(fCommon, CC_DFSELECT, 0);
	f0 = r32(fCommon, CC_DF);
	w32(fCommon, CC_DFSELECT, 1);
	f1 = r32(fCommon, CC_DF);
	if (!(f1 & 1) || !(f0 & 1)) {
		IOLog("virtio9p: device lacks VERSION_1 or MOUNT_TAG\n");
		goto fail;
	}
	w32(fCommon, CC_GFSELECT, 0);
	w32(fCommon, CC_GF, 1);		/* MOUNT_TAG */
	w32(fCommon, CC_GFSELECT, 1);
	w32(fCommon, CC_GF, 1);		/* VERSION_1 */
	w8(fCommon, CC_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
	st = r8(fCommon, CC_STATUS);
	if (!(st & ST_FEATURES_OK))
		goto fail;

	n = r16(fDevice, 0);
	if (n == 0 || n >= sizeof(fTag))
		goto fail;
	for (i = 0; i < n; i++)
		fTag[i] = r8(fDevice, 2 + i);
	fTag[n] = 0;

	if (!setupQueue())
		goto fail;
	for (fMsize = MSIZE_MAX; fMsize >= MSIZE_MIN; fMsize /= 2) {
		fTx = dmaAlloc(fMsize, &fTxPA, NULL);
		fRx = dmaAlloc(fMsize, &fRxPA, NULL);
		if (fTx && fRx)
			break;
		dmaFree(fTx);
		dmaFree(fRx);
		fTx = fRx = NULL;
	}
	if (!fTx)
		goto fail;

	fRPCLock = IOLockAlloc();
	fWaitLock = IOLockAlloc();
	if (!fRPCLock || !fWaitLock)
		goto fail;

	fWorkLoop = getWorkLoop();
	if (fWorkLoop) {
		fWorkLoop->retain();
		fIntr = IOFilterInterruptEventSource::filterInterruptEventSource(this,
				handler, filter, fPCI, 0);
		if (fIntr && fWorkLoop->addEventSource(fIntr) == kIOReturnSuccess)
			fIntr->enable();
		else if (fIntr) {
			fIntr->release();
			fIntr = NULL;
		}
	}
	if (!fIntr)
		IOLog("virtio9p: no interrupt, polling\n");

	w8(fCommon, CC_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);

	setProperty("mount-tag", fTag);
	setIcon();
	IOLockLock(gDevLock);
	gDevices->setObject(this);
	IOLockUnlock(gDevLock);
	registerService();
	IOLog("virtio9p: tag \"%s\", msize %u\n", fTag, (unsigned)fMsize);
	return true;

fail:
	reset();
	super::stop(provider);
	return false;
}

void
org_cat7_virtio9p::stop(IOService *provider)
{
	unsigned int i;

	IOLockLock(gDevLock);
	for (i = 0; i < gDevices->getCount(); i++)
		if (gDevices->getObject(i) == this) {
			gDevices->removeObject(i);
			break;
		}
	IOLockUnlock(gDevLock);

	if (fRPCLock)
		IOLockLock(fRPCLock);
	fDead = true;
	reset();
	if (fRPCLock)
		IOLockUnlock(fRPCLock);
	if (fIntr) {
		fIntr->disable();
		fWorkLoop->removeEventSource(fIntr);
		fIntr->release();
		fIntr = NULL;
	}
	super::stop(provider);
}

bool
org_cat7_virtio9p::terminate(IOOptionBits options)
{
	if (fUsers > 0)
		return false;
	return super::terminate(options);
}

void
org_cat7_virtio9p::free(void)
{
	Region *rs[] = { &fCommon, &fNotify, &fISR, &fDevice };
	unsigned int i;

	for (i = 0; i < 4; i++)
		if (rs[i]->map)
			rs[i]->map->release();
	dmaFree(fRing);
	dmaFree(fTx);
	dmaFree(fRx);
	if (fWorkLoop)
		fWorkLoop->release();
	if (fRPCLock)
		IOLockFree(fRPCLock);
	if (fWaitLock)
		IOLockFree(fWaitLock);
	if (fPCI)
		fPCI->release();
	super::free();
}

bool
org_cat7_virtio9p::filter(OSObject *owner, IOFilterInterruptEventSource *src)
{
	org_cat7_virtio9p *me = (org_cat7_virtio9p *)owner;
	uint8_t isr;

	(void)src;
	isr = me->fISR.base[0];		/* reading clears it */
	if (!isr)
		return false;
	me->fISRBits |= isr;
	return true;
}

void
org_cat7_virtio9p::handler(OSObject *owner, IOInterruptEventSource *src, int count)
{
	org_cat7_virtio9p *me = (org_cat7_virtio9p *)owner;

	(void)src;
	(void)count;
	me->fISRBits = 0;
	IOLockLock(me->fWaitLock);
	IOLockWakeup(me->fWaitLock, (void *)&me->fUsedIdx, false);
	IOLockUnlock(me->fWaitLock);
}

int
org_cat7_virtio9p::rpc(const void *tx, uint32_t txlen, void *rx, uint32_t rxmax, uint32_t *rxlen)
{
	uint8_t *desc, *avail, *used;
	AbsoluteTime deadline;
	uint32_t len, slot;
	int e;

	if (txlen > fMsize)
		return EMSGSIZE;

	IOLockLock(fRPCLock);
	if (fDead) {
		IOLockUnlock(fRPCLock);
		return ENXIO;
	}
	bcopy(tx, fTx->getBytesNoCopy(), txlen);

	desc = fRingVA;
	avail = fRingVA + 16 * fQSize;
	used = fRingVA + USED_OFF;

	OSWriteLittleInt64(desc, 0, fTxPA);
	OSWriteLittleInt32(desc, 8, txlen);
	OSWriteLittleInt16(desc, 12, VRING_DESC_F_NEXT);
	OSWriteLittleInt16(desc, 14, 1);
	OSWriteLittleInt64(desc, 16, fRxPA);
	OSWriteLittleInt32(desc, 24, fMsize);
	OSWriteLittleInt16(desc, 28, VRING_DESC_F_WRITE);
	OSWriteLittleInt16(desc, 30, 0);

	OSWriteLittleInt16(avail, 4 + 2 * (fAvailIdx % fQSize), 0);
	__asm__ volatile("sync" ::: "memory");
	fAvailIdx++;
	OSWriteLittleInt16(avail, 2, fAvailIdx);
	__asm__ volatile("sync" ::: "memory");
	OSWriteLittleInt16((void *)fNotifyAddr, 0, 0);
	OSSynchronizeIO();

	IOLockLock(fWaitLock);
	while (OSReadLittleInt16(used, 2) == fUsedIdx) {
#ifdef PANTHER
		clock_interval_to_deadline(5, kMillisecondScale, &deadline);
#else
		clock_interval_to_deadline(5, kMillisecondScale, (uint64_t *)&deadline);
#endif
		IOLockSleepDeadline(fWaitLock, (void *)&fUsedIdx,
				    deadline, THREAD_UNINT);
	}
	IOLockUnlock(fWaitLock);
	__asm__ volatile("sync" ::: "memory");

	slot = fUsedIdx % fQSize;
	len = OSReadLittleInt32(used, 4 + 8 * slot + 4);
	fUsedIdx++;

	e = 0;
	if (len < 7 || len > fMsize)
		e = EIO;
	else {
		if (len > rxmax)
			len = rxmax;
		bcopy(fRx->getBytesNoCopy(), rx, len);
		*rxlen = len;
	}
	IOLockUnlock(fRPCLock);
	return e;
}

extern "C" v9p_chan *
v9p_chan_open(const char *tag)
{
	org_cat7_virtio9p *d, *found;
	unsigned int i;

	found = NULL;
	IOLockLock(gDevLock);
	for (i = 0; i < gDevices->getCount(); i++) {
		d = (org_cat7_virtio9p *)gDevices->getObject(i);
		if (strcmp(d->tag(), tag) == 0) {
			d->retain();
			OSIncrementAtomic(&d->fUsers);
			found = d;
			break;
		}
	}
	IOLockUnlock(gDevLock);
	return (v9p_chan *)found;
}

extern "C" void
v9p_chan_close(v9p_chan *ch)
{
	org_cat7_virtio9p *d = (org_cat7_virtio9p *)ch;

	if (d) {
		OSDecrementAtomic(&d->fUsers);
		d->release();
	}
}

extern "C" uint32_t
v9p_chan_msize(v9p_chan *ch)
{
	return ((org_cat7_virtio9p *)ch)->msize();
}

extern "C" int
v9p_chan_rpc(v9p_chan *ch, const void *tx, uint32_t txlen, void *rx, uint32_t rxmax, uint32_t *rxlen)
{
	return ((org_cat7_virtio9p *)ch)->rpc(tx, txlen, rx, rxmax, rxlen);
}
