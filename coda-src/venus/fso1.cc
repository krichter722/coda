/* BLURB gpl

                           Coda File System
                              Release 6

          Copyright (c) 1987-2003 Carnegie Mellon University
                  Additional copyrights listed below

This  code  is  distributed "AS IS" without warranty of any kind under
the terms of the GNU General Public Licence Version 2, as shown in the
file  LICENSE.  The  technical and financial  contributors to Coda are
listed in the file CREDITS.

                        Additional copyrights
		Copyright (c) 2002-2003 Intel Corporation
#*/


/*
 *
 *    Implementation of the Venus File-System Object (fso) abstraction.
 *
 *    ToDo:
 *       1. Need to allocate meta-data by priority (escpecially in the case of dir pages and modlog entries)
 */


/* Following block is shared with worker.c. */
/* It is needed to ensure that C++ makes up "anonymous types" in the same order.  It sucks! */
#ifdef __cplusplus
extern "C" {
#endif

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <struct.h>
#include <stdlib.h>
#include "coda_string.h"
#include <sys/param.h>
#include <unistd.h>
#include <stdlib.h>
#include <rpc2/rpc2.h>
#include <netdb.h>

#include <math.h>

#include <time.h>
#include <coda.h>

#ifdef __cplusplus
}
#endif

/* interfaces */

/* from vicedep */
#include <venusioctl.h>

/* from venus */
#include "advice.h"
#include "adv_monitor.h"
#include "adv_daemon.h"
#include "comm.h"
#include "fso.h"
#include "local.h"
#include "mariner.h"
#include "user.h"
#include "venus.private.h"
#include "venusrecov.h"
#include "venusvol.h"
#include "vproc.h"
#include "worker.h"
#include "realmdb.h"


static int NullRcRights = 0;
static AcRights NullAcRights = { ALL_UIDS, 0, 0, 0 };


/*  *****  Constructors, Destructors  *****  */

/* Pre-allocation routine. */
/* MUST be called from within transaction! */

void *fsobj::operator new(size_t len, fso_alloc_t fromwhere){
    fsobj *f = 0;

    CODA_ASSERT(fromwhere == FROMHEAP);
    /* Allocate recoverable store for the object. */
    f = (fsobj *)rvmlib_rec_malloc((int)len);
    CODA_ASSERT(f);
    return(f);
}

void *fsobj::operator new(size_t len, fso_alloc_t fromwhere, int AllocPriority){
    fsobj *f = 0;
    int rc = 0;

    CODA_ASSERT(fromwhere == FROMFREELIST); 
    /* Find an existing object that can be reclaimed. */
    rc = FSDB->AllocFso(AllocPriority, &f);
    if (rc == ENOSPC)
      LOG(0, ("fsobj::new returns 0 (fsdb::AllocFso returned ENOSPC)\n"));
//    CODA_ASSERT(f);
    return(f);
}

void *fsobj::operator new(size_t len){
    abort(); /* should never be called */
}

fsobj::fsobj(int i) : cf(i) {

    RVMLIB_REC_OBJECT(*this);
    ix = i;

    /* Put ourselves on the free-list. */
    FSDB->FreeFso(this);
}


/* MUST be called from within transaction! */
fsobj::fsobj(VenusFid *key, char *name) : cf() {
    LOG(10, ("fsobj::fsobj: fid = (%s), comp = %s\n", FID_(key),
	     name == NULL ? "(no name)" : name));

    RVMLIB_REC_OBJECT(*this);
    ResetPersistent();
    fid = *key;
    {
	int len = (name ? (int) strlen(name) : 0) + 1;
	comp = (char *)rvmlib_rec_malloc(len);
	rvmlib_set_range(comp, len);
	if (name) strcpy(comp, name);
	else comp[0] = '\0';
    }
    if (FID_IsVolRoot(&fid))
	mvstat = ROOT;
    ResetTransient();

    Lock(WR);

    /* Insert into hash table. */
    (FSDB->htab).append(&fid, &primary_handle);
}

/* local-repair modification */
/* MUST be called from within transaction! */
/* Caller sets range for whole object! */
void fsobj::ResetPersistent() {
    MagicNumber = FSO_MagicNumber;
    fid = NullFid;
    comp = 0;
    vol = 0;
    state = FsoRunt;
    stat.VnodeType = Invalid;
    stat.LinkCount = (unsigned char)-1;
    stat.Length = 0;
    stat.DataVersion = 0;
    stat.VV = NullVV;
    stat.VV.StoreId.Host = NO_HOST;
    stat.Date = (Date_t)-1;
    stat.Author = ALL_UIDS;
    stat.Owner = ALL_UIDS;
    stat.Mode = (unsigned short)-1;
    ClearAcRights(ALL_UIDS);
    flags.fake = 0;
    flags.owrite = 0;
    flags.dirty = 0;
    flags.local = 0;
    mvstat = NORMAL;
    pfid = NullFid;
    CleanStat.Length = (unsigned long)-1;
    CleanStat.Date = (Date_t)-1;
    data.havedata = 0;
}

/* local-repair modification */
/* Needn't be called from within transaction. */
void fsobj::ResetTransient()
{
    /* Sanity checks. */
    if (MagicNumber != FSO_MagicNumber)
	{ print(logFile); CHOKE("fsobj::ResetTransient: bogus MagicNumber"); }

    /* This is a horrible way of resetting handles! */
    list_head_init(&vol_handle);
    memset((void *)&prio_handle, 0, (int)sizeof(prio_handle));
    memset((void *)&del_handle, 0, (int)sizeof(del_handle));
    memset((void *)&owrite_handle, 0, (int)sizeof(owrite_handle));

    if (HAVEDATA(this) && IsDir()) {
	data.dir->udcfvalid = 0;
	data.dir->udcf = 0;
    }
    ClearRcRights();
    DemoteAcRights(ALL_UIDS);
    flags.ckmtpt = 0;
    flags.fetching = 0;
    flags.random = ::random();

    memset((void *)&u, 0, (int)sizeof(u));

    pfso = 0;
    children = 0;
    child_link.clear();

    priority = -1;
    HoardPri = 0;
    HoardVuid = HOARD_UID;
    hdb_bindings = 0;
    FetchAllowed = HF_DontFetch;
    AskingAllowed = HA_Ask;

    mle_bindings = 0;
    shadow = 0;
    
    /* 
     * sync doesn't need to be initialized. 
     * It's used only for LWP_Wait and LWP_Signal. 
     */
    readers = 0;
    writers = 0;
    openers = 0;
    Writers = 0;
    Execers = 0;
    refcnt = 0;

    lastresolved = 0;

    /* Link to volume, and initialize volume specific members. */
    if ((vol = VDB->Find(MakeVolid(&fid))) == 0) {
	print(logFile);
	CHOKE("fsobj::ResetTransient: couldn't find volume");
    }

    /* Add to volume list */
    list_add(&vol_handle, &vol->fso_list);

    if (IsLocalObj()) {
	/* set valid RC status for local object */
	SetRcRights(RC_DATA | RC_STATUS);
    }
}


/* MUST be called from within transaction! */
fsobj::~fsobj() {
    RVMLIB_REC_OBJECT(*this);

#ifdef	VENUSDEBUG
    /* Sanity check. */
    if (!GCABLE(this))
	{ print(logFile); CHOKE("fsobj::~fsobj: !GCABLE"); }
#endif /* VENUSDEBUG */

    LOG(10, ("fsobj::~fsobj: fid = (%s), comp = %s\n", FID_(&fid), comp));

    /* Reset reference counter for this slot. */
    FSDB->LastRef[ix] = 0;

    /* MLE bindings must already be detached! */
    if (mle_bindings) {
	if (mle_bindings->count() != 0)
	    { print(logFile); CHOKE("fsobj::~fsobj: mle_bindings->count() != 0"); }
	delete mle_bindings;
	mle_bindings = 0;
    }

    /* Detach hdb bindings. */
    if (hdb_bindings) {
	DetachHdbBindings();
	if (hdb_bindings->count() != 0)
	    { print(logFile); CHOKE("fsobj::~fsobj: hdb_bindings->count() != 0"); }
	delete hdb_bindings;
	hdb_bindings = 0;
    }

    /* Detach ourselves from our parent (if necessary). */
    if (pfso != 0) {
	pfso->DetachChild(this);
	pfso = 0;
    }

    /* Detach any children of our own. */
    if (children != 0) {
	dlink *d = 0;
	while ((d = children->first())) {
	    fsobj *cf = strbase(fsobj, d, child_link);

	    /* If this is a FakeDir delete all associated FakeMtPts since they are no longer useful! */
	    if (IsFakeDir())
		cf->Kill();

	    DetachChild(cf);
	    cf->pfso = 0;
	}
	if (children->count() != 0)
	    { print(logFile); CHOKE("fsobj::~fsobj: children->count() != 0"); }
	delete children;
    }

    /* Do mount cleanup. */
    switch(mvstat) {
	case NORMAL:
	    /* Nothing to do. */
	    break;

	case MOUNTPOINT:
	    /* Detach volume root. */
	    {
		fsobj *root_fso = u.root;
		if (root_fso == 0) {
		    print(logFile);
		    CHOKE("fsobj::~fsobj: root_fso = 0");
		}
		if (root_fso->u.mtpoint != this) {
		    print(logFile);
		    root_fso->print(logFile);
		    CHOKE("fsobj::~fsobj: rf->mtpt != mf");
		}
		root_fso->UnmountRoot();
		UncoverMtPt();
	    }
	    break;

	case ROOT:
	    /* Detach mtpt. */
	    if (u.mtpoint != 0) {
		fsobj *mtpt_fso = u.mtpoint;
		if (mtpt_fso->u.root == this)
		    mtpt_fso->UncoverMtPt();
		UnmountRoot();
	    }
	    break;

	default:
	    print(logFile);
	    CHOKE("fsobj::~fsobj: bogus mvstat");
    }

    /* Remove from volume's fso list */
    list_del(&vol_handle);

    /* Unlink from volume. */
    VDB->Put(&vol);

    /* Release data. */
    if (HAVEDATA(this))
	DiscardData();

    /* Remove from the delete queue. */
    if (FSDB->delq->remove(&del_handle) != &del_handle)
	{ print(logFile); CHOKE("fsobj::~fsobj: delq remove"); }

    /* Remove from the table. */
    if ((FSDB->htab).remove(&fid, &primary_handle) != &primary_handle)
	{ print(logFile); CHOKE("fsobj::~fsobj: htab remove"); }

    /* Notify waiters of dead runts. */
    if (!HAVESTATUS(this)) {
	LOG(10, ("fsobj::~fsobj: dead runt = (%s)\n", FID_(&fid)));

	FSDB->matriculation_count++;
	VprocSignal(&FSDB->matriculation_sync);
    }

    /* Return component string to heap. */
    rvmlib_rec_free(comp);
    comp = NULL;
}

void fsobj::operator delete(void *deadobj, size_t len) {

    LOG(10, ("fsobj::operator delete()\n"));

    /* Stick on the free list. */
    FSDB->FreeFso((fsobj *)deadobj);
}

/* local-repair modification */
/* MUST NOT be called from within transaction. */
void fsobj::Recover()
{
    /* Validate state. */
    switch(state) {
	case FsoRunt:
	    /* Objects that hadn't matriculated can be safely discarded. */
	    eprint("\t(%s, %s) runt object being discarded...",
		   comp, FID_(&fid));
	    goto Failure;

	case FsoNormal:
	    break;

	case FsoDying:
	    /* Dying objects should shortly be deleted. */
	    FSDB->delq->append(&del_handle);
	    break;

	default:
	    print(logFile);
	    CHOKE("fsobj::Recover: bogus state");
    }

    /* Uncover mount points. */
    if (IsMtPt()) {
	Recov_BeginTrans();

	/* XXX this can probably be removed */
	RVMLIB_REC_OBJECT(stat.VnodeType);
	stat.VnodeType = SymbolicLink;
	/* XXX */

	RVMLIB_REC_OBJECT(mvstat);
	mvstat = NORMAL;
	Recov_EndTrans(MAXFP);
    }

    /* Rebuild priority queue. */
    ComputePriority();

    /* Garbage collect data that was in the process of being fetched. */
    if (flags.fetching) {
	FSO_ASSERT(this, HAVEDATA(this));
	eprint("\t(%s, %s) freeing garbage data contents", comp, FID_(&fid));
	Recov_BeginTrans();
	RVMLIB_REC_OBJECT(flags);
	flags.fetching = 0;

    /* Could we trust the value of 'validdata' which was stored in RVM?
     * there is no real way of telling when exactly we crashed and whether
     * the datablocks hit the disk. The whole recovery seems to make the
     * assumption that this is true, however... I'm hesitant to remove the
     * call to 'DiscardData' here. It is probably ok when venus crashed, but
     * probably not when the OS died. --JH */
	DiscardData();
	Recov_EndTrans(0);
    }

    /* Files that were open for write must be "closed" and discarded. */
    if (flags.owrite) {
	FSO_ASSERT(this, HAVEDATA(this));
	eprint("\t(%s, %s) found owrite object, discarding", comp, FID_(&fid));
	if (IsFile()) {
	    char spoolfile[MAXPATHLEN];
	    int idx = 0;

	    do {
		snprintf(spoolfile,MAXPATHLEN,"%s/%s-%u",SpoolDir,comp,idx++);
	    } while (::access(spoolfile, F_OK) == 0 || errno != ENOENT); 

	    data.file->Copy(spoolfile, NULL, 1);
	    eprint("\t(lost file data backed up to %s)", spoolfile);
	}
	Recov_BeginTrans();
	RVMLIB_REC_OBJECT(flags);
	flags.owrite = 0;
	Recov_EndTrans(0);
	goto Failure;
    }

    /* Get rid of fake objects, and other objects that are not likely to be
     * useful anymore. */
    if (IsFake() && !LRDB->RFM_IsFakeRoot(&fid)) {
	LOG(0, ("fsobj::Recover: (%s) is a fake object\n",
		FID_(&fid)));
	goto Failure;
    }
    if (!IsFake() && !vol->IsReplicated() && !IsLocalObj()) {
	LOG(0, ("fsobj::Recover: (%s) is probably in a backup volume\n",
		FID_(&fid)));
	goto Failure;
    }

    /* Get rid of a former mount-root whose fid is not a volume root and whose
     * pfid is NullFid */
    if (IsNormal() && !FID_IsVolRoot(&fid) && 
	FID_EQ(&pfid, &NullFid) && !IsLocalObj()) {
	LOG(0, ("fsobj::Recover: (%s) is a non-volume root whose pfid is NullFid\n",
		FID_(&fid)));
	goto Failure;
    }

    /* Check the cache file. */
    switch(stat.VnodeType) {
	case File:
	    {

	    if (!HAVEDATA(this) && cf.Length() != 0) {
		eprint("\t(%s, %s) cache file validation failed",
		       comp, FID_(&fid));
		FSDB->FreeBlocks(NBLOCKS(cf.Length()));
		cf.Reset();
	    }
	    }
	    break;

	case Directory:
	case SymbolicLink:
	    /* 
	     * Reclaim cache-file blocks. Since directory contents are stored
	     * in RVM (as are all fsobjs), cache file blocks for directories 
	     * are thrown out at startup because they are the ``Unix format'' 
	     * version of the object.  The stuff in RVM is the ``Vice format'' 
	     * version. 
	     */
	    if (cf.Length() != 0) {
		FSDB->FreeBlocks(NBLOCKS(cf.Length()));
		cf.Reset();
	    }
	    break;

	case Invalid:
	    CHOKE("fsobj::Recover: bogus VnodeType (%d)", stat.VnodeType);
    }

    if (LogLevel >= 1) print(logFile);
    return;

Failure:
    {
	LOG(0, ("fsobj::Recover: invalid fso (%s, %s), attempting to GC...\n",
		comp, FID_(&fid)));
	print(logFile);

        /* Scavenge data for bogus objects. */
        /* Note that any store of this file in the CML must be cancelled (in
         * later step of recovery). */
        {
	    if (HAVEDATA(this)) {
                Recov_BeginTrans();
                /* Normally we can't discard dirty files, but here we just
                 * decided that there is no other way. */
                flags.dirty = 0;
                DiscardData();
                Recov_EndTrans(MAXFP);
	    }
            if (cf.Length()) {
                /* Reclaim cache-file blocks. */
                FSDB->FreeBlocks(NBLOCKS(cf.Length()));
		cf.Reset();
	    }
        }

	/* Kill bogus object. */
	/* Caution: Do NOT GC since linked objects may not be valid yet! */
	{
	    Recov_BeginTrans();
	    Kill();
	    Recov_EndTrans(MAXFP);
	}
    }
}


/*  *****  General Status  *****  */

/* MUST be called from within transaction! */
/* Call with object write-locked. */
void fsobj::Matriculate()
{
    if (HAVESTATUS(this))
	{ print(logFile); CHOKE("fsobj::Matriculate: HAVESTATUS"); }

    LOG(10, ("fsobj::Matriculate: (%s)\n", FID_(&fid)));

    RVMLIB_REC_OBJECT(state);
    state = FsoNormal;

    /* Notify waiters. */
    FSDB->matriculation_count++;
    VprocSignal(&FSDB->matriculation_sync);	/* OK; we are in transaction, but signal is NO yield */
}


/* Need not be called from within transaction. */
/* Call with object write-locked. */
/* CallBack handler calls this with NoLock (to avoid deadlock)! -JJK */
/* >> Demote should not yield or destroy the object << */
void fsobj::Demote(void)
{
    if (!HAVESTATUS(this) || DYING(this)) return;
    //if (IsMtPt() || IsFakeMTLink()) return;
    if (IsFakeMTLink()) return;

    LOG(10, ("fsobj::Demote: fid = (%s)\n", FID_(&fid)));

    ClearRcRights();

    if (IsDir())
	DemoteAcRights(ALL_UIDS);

    DemoteHdbBindings();

    /* Kernel demotion must be severe for non-directories (i.e., purge name- as well as attr-cache) */
    /* because pfid is suspect and the only way to revalidate it is via a cfs_lookup call. -JJK */
    int severely = (!IsDir() || IsFakeDir());
    k_Purge(&fid, severely);
}


/* MUST be called from within transaction! */
/* Call with object write-locked. */
void fsobj::Kill(int TellServers) 
{
	if (DYING(this)) 
		return;

	LOG(10, ("fsobj::Kill: (%s)\n", FID_(&fid)));

	DisableReplacement();
	
	FSDB->delq->append(&del_handle);
	RVMLIB_REC_OBJECT(state);
	state = FsoDying;

	/* Inform advice servers of loss of availability of this object */
	/* NotifyUsersOfKillEvent(hdb_bindings, NBLOCKS(stat.Length)); */
	Demote();
}


/* MUST be called from within transaction! */
void fsobj::GC() {
    delete this;
}


/* MUST NOT be called from within transaction! */
int fsobj::Flush() {
    /* Flush all children first. */
    /* N.B. Recursion here could overflow smallish stacks! */
    if (children != 0) {
	dlist_iterator next(*children);
	dlink *d = next();
	if (d != 0) {
	    do {
		fsobj *cf = strbase(fsobj, d, child_link);
		d = next();
		(void)cf->Flush();
	    } while(d != 0);
	}
    }

    if (!FLUSHABLE(this)) {
	LOG(10, ("fsobj::Flush: (%s) !FLUSHABLE\n", FID_(&fid)));
	Demote();
	return(EMFILE);
    }

    LOG(10, ("fsobj::Flush: flushed (%s)\n", FID_(&fid)));
    Recov_BeginTrans();
    Kill();
    GC();
    Recov_EndTrans(MAXFP);

    return(0);
}


/* MUST be called from within transaction! */
/* Call with object write-locked. */
/* Called as result of {GetAttr, ValidateAttr, GetData, ValidateData}. */

void fsobj::UpdateStatusAndSHA(ViceStatus *vstat, uid_t uid, RPC2_BoundedBS *newsha)
{
    /* Mount points are never updated. */
    if (IsMtPt())
	{ print(logFile); CHOKE("fsobj::UpdateStatusAndSHA: IsMtPt!"); }
    /* Fake objects are never updated. */
    if (IsFake())
	{ print(logFile); CHOKE("fsobj::UpdateStatusAndSHA: IsFake!"); }

    LOG(100, ("fsobj::UpdateStatusAndSHA: (%s), uid = %d\n", FID_(&fid), uid));

    if (HAVESTATUS(this)) {		/* {ValidateAttr} */
      if (!StatusEq(vstat, 0))
	    ReplaceStatusAndSHA(vstat, 0, newsha);
      /* else ValidateAttr was successful for this object, so
	 leave SHA alone */
    }
    else {				/* {GetAttr} */
	Matriculate();
	ReplaceStatusAndSHA(vstat, 0, newsha);
    }

    /* Set access rights and parent (if they differ). */
    if (IsDir())
	SetAcRights(uid, vstat->MyAccess, vstat->AnyAccess);

    SetParent(vstat->vparent, vstat->uparent);
}


/* MUST be called from within transaction! */
/* Call with object write-locked. */
/* Called for mutating operations. */
/* SHA is always cleared by this call */
void fsobj::UpdateStatusAndClearSHA(ViceStatus *vstat, vv_t *UpdateSet, uid_t uid)
{
    /* Mount points are never updated. */
    if (IsMtPt())
	{ print(logFile); CHOKE("fsobj::UpdateStatusAndClearSHA: IsMtPt!"); }
    /* Fake objects are never updated. */
    if (IsFake())
	{ print(logFile); CHOKE("fsobj::UpdateStatusAndClearSHA: IsFake!"); }

    LOG(100, ("fsobj::UpdateStatusAndClearSHA: (%s), uid = %d\n", FID_(&fid), uid));

    /* Install the new status block. */
    if (!StatusEq(vstat, 1))
	/* Ought to Die in this event! */;

    ReplaceStatusAndSHA(vstat, UpdateSet, NULL);

    /* Set access rights and parent (if they differ). */
    /* N.B.  It should be a fatal error if they differ! */
    if (IsDir())
	SetAcRights(uid, vstat->MyAccess, vstat->AnyAccess);

    SetParent(vstat->vparent, vstat->uparent);
}


/* Need not be called from within transaction. */
int fsobj::StatusEq(ViceStatus *vstat, int Mutating)
{
    int eq = 1;
    int log = (Mutating || HAVEDATA(this));

    if (stat.Length != vstat->Length) {
	eq = 0;
	if (log)
	    LOG(0, ("fsobj::StatusEq: (%s), Length %d != %d\n",
		    FID_(&fid), stat.Length, vstat->Length));
    }
    /* DataVersion is a non-replicated value and different replicas may
     * legitimately return different dataversions. On a replicated volume we
     * use the VV, and shouldn't use the DataVersion at all. -JH
     */
    if (!vol->IsReplicated()) {
	if (stat.DataVersion != vstat->DataVersion) {
	    eq = 0;
	    if (log)
		LOG(0, ("fsobj::StatusEq: (%s), DataVersion %d != %d\n",
			FID_(&fid), stat.DataVersion, vstat->DataVersion));
	}
    } else {
	if (!Mutating && VV_Cmp(&stat.VV, &vstat->VV) != VV_EQ) {
	    eq = 0;
	    if (log)
		LOG(0, ("fsobj::StatusEq: (%s), VVs differ\n", FID_(&fid)));
	}
    }
    if (stat.Date != vstat->Date) {
	eq = 0;
	if (log)
	    LOG(0, ("fsobj::StatusEq: (%s), Date %d != %d\n",
		    FID_(&fid), stat.Date, vstat->Date));
    }
    if (stat.Owner != vstat->Owner) {
	eq = 0;
	if (log)
	    LOG(0, ("fsobj::StatusEq: (%s), Owner %d != %d\n",
		    FID_(&fid), stat.Owner, vstat->Owner));
    }
    if (stat.Mode != vstat->Mode) {
	eq = 0;
	if (log)
	    LOG(0, ("fsobj::StatusEq: (%s), Mode %d != %d\n",
		    FID_(&fid), stat.Mode, vstat->Mode));
    }
    if (stat.LinkCount != vstat->LinkCount) {
	eq = 0;
	if (log)
	    LOG(0, ("fsobj::StatusEq: (%s), LinkCount %d != %d\n",
		    FID_(&fid), stat.LinkCount, vstat->LinkCount));
    }
    if (stat.VnodeType != (int)vstat->VnodeType) {
	eq = 0;
	if (log)
	    LOG(0, ("fsobj::StatusEq: (%s), VnodeType %d != %d\n",
		    FID_(&fid), stat.VnodeType, (int)vstat->VnodeType));
    }

    return(eq);
}


/* MUST be called from within transaction! */
/* Call with object write-locked. */
void fsobj::ReplaceStatusAndSHA(ViceStatus *vstat, vv_t *UpdateSet, RPC2_BoundedBS *vsha)
{
    RVMLIB_REC_OBJECT(stat);
    RVMLIB_REC_OBJECT(VenusSHA);

    if (vsha && vsha->SeqLen == SHA_DIGEST_LENGTH)
	 memcpy(VenusSHA, vsha->SeqBody, SHA_DIGEST_LENGTH);
    else memset(VenusSHA, 0, SHA_DIGEST_LENGTH);

    /* We're changing the length? 
     * Then the cached data is probably no longer useable! But try to fix up
     * the cachefile so that we can at least give a stale copy. */
    if (HAVEDATA(this) && stat.Length != vstat->Length) {
	LOG(0, ("fsobj::ReplaceStatusAndSHA: (%s), changed stat.length %d->%d\n",
		FID_(&fid), stat.Length, vstat->Length));
	if (IsFile())
	    LocalSetAttr((unsigned long)-1, vstat->Length, (unsigned long)-1,
			 (unsigned long)-1, (unsigned long)-1);
	SetRcRights(RC_STATUS);
    }

    stat.Length = vstat->Length;
    stat.DataVersion = vstat->DataVersion;

    /* nice optimization, but repair is looking at version vectors in not
     * necessarily replicated volumes (although the IsReadWriteRep test should
     * have matched in that case) */
    //if (vol->IsReplicated() || vol->IsReadWriteReplica())
    if (UpdateSet == 0)
	stat.VV = vstat->VV;
    else {
	stat.VV.StoreId = vstat->VV.StoreId;
	AddVVs(&stat.VV, UpdateSet);
    }
    stat.Date = vstat->Date;
    stat.Owner = (uid_t) vstat->Owner;
    stat.Mode = (short) vstat->Mode;
    stat.LinkCount = (unsigned char) vstat->LinkCount;
    if (vstat->VnodeType)
	stat.VnodeType = vstat->VnodeType;
}


int fsobj::CheckRcRights(int rights) {
    return((rights & RcRights) == rights);
}


void fsobj::SetRcRights(int rights)
{
    if (vol->IsBackup() || IsFake())
	return;

    LOG(100, ("fsobj::SetRcRights: (%s), rights = %d\n", FID_(&fid), rights));

    /* There is a problem if the rights are set that we have valid data,
     * but we actually don't have data yet. */
    FSO_ASSERT(this, !(rights & RC_DATA) ||
	             ((rights & RC_DATA) && HAVEALLDATA(this)));
    RcRights = rights;
}


void fsobj::ClearRcRights() {
    LOG(100, ("fsobj::ClearRcRights: (%s), rights = %d\n",
	      FID_(&fid), RcRights));

    RcRights = NullRcRights;
}


int fsobj::IsValid(int rcrights) {
    int haveit = 0;

    if (rcrights & RC_STATUS) 
        haveit = HAVESTATUS(this); /* (state != FsoRunt) */

    if (!haveit && (rcrights & RC_DATA))  /* should work if both set */
        haveit = HAVEALLDATA(this);

    /* If we don't have the object, it definitely is not valid. */
    if (!haveit) return 0;

    /* Replicated objects must be considered valid when we are either
     * disconnected or write-disconnected and the object is dirty. */
    if (vol->IsReplicated()) {
	if (vol->IsDisconnected())		       return 1;
	if (vol->IsWriteDisconnected() && flags.dirty) return 1;
    }

    /* Several other reasons that imply this object is valid */
    if (CheckRcRights(rcrights))    return 1;
    if (IsMtPt() || IsFakeMTLink()) return 1;

    /* Now if we still have the volume callback, we can't lose.
     * also update VCB statistics -- valid due to VCB */
    if (!vol->IsReplicated()) return 0;

    repvol *vp = (repvol *)vol;
    if (vp->HaveCallBack()) {
        vp->VCBHits++;
        return 1;
    }

    /* Final conclusion, the object is not valid */
    return 0;
}


/* Returns {0, EACCES, ENOENT}. */
int fsobj::CheckAcRights(uid_t uid, long rights, int connected)
{
    if (uid != ALL_UIDS) {
	/* Do we have this user's rights in the cache? */
	for (int i = 0; i < CPSIZE; i++) {
	    if (SpecificUser[i].inuse && (!connected || SpecificUser[i].valid)
		&& uid == SpecificUser[i].uid)
		return((rights & SpecificUser[i].rights) ? 0 : EACCES);
	}
    }
    if (uid == ALL_UIDS || !connected) {
	/* Do we have access via System:AnyUser? */
	if (AnyUser.inuse && (!connected || AnyUser.valid))
	    return((rights & AnyUser.rights) ? 0 : EACCES);
    }

    LOG(10, ("fsobj::CheckAcRights: not found, (%s), (%d, %d, %d)\n",
	      FID_(&fid), uid, rights, connected));
    return(ENOENT);
}


/* MUST be called from within transaction! */
void fsobj::SetAcRights(uid_t uid, long my_rights, long any_rights)
{
    LOG(100, ("fsobj::SetAcRights: (%s), uid = %d, my_rights = %d, any_rights = %d\n",
	       FID_(&fid), uid, my_rights, any_rights));

    if (!AnyUser.inuse || AnyUser.rights != any_rights) {
	RVMLIB_REC_OBJECT(AnyUser);
	AnyUser.rights = (unsigned char) any_rights;
	AnyUser.inuse = 1;
    }
    AnyUser.valid = 1;

    /* Don't record my_rights if we're not really authenticated! */
    userent *ue = vol->realm->GetUser(uid);
    int tokensvalid = ue->TokensValid();
    PutUser(&ue);
    if (!tokensvalid) return;

    int i;
    int j = -1;
    int k = -1;
    for (i = 0; i < CPSIZE; i++) {
	if (uid == SpecificUser[i].uid) break;
	if (!SpecificUser[i].inuse) j = i;
	if (!SpecificUser[i].valid) k = i;
    }
    if (i == CPSIZE && j != -1) i = j;
    if (i == CPSIZE && k != -1) i = k;
    if (i == CPSIZE) i = (int) (Vtime() % CPSIZE);

    if (!SpecificUser[i].inuse || SpecificUser[i].uid != uid ||
	SpecificUser[i].rights != my_rights)
    {
	RVMLIB_REC_OBJECT(SpecificUser[i]);
	SpecificUser[i].uid = uid;
	SpecificUser[i].rights = (unsigned char) my_rights;
	SpecificUser[i].inuse = 1;
    }
    SpecificUser[i].valid = 1;
}


/* Need not be called from within transaction. */
void fsobj::DemoteAcRights(uid_t uid)
{
    LOG(100, ("fsobj::DemoteAcRights: (%s), uid = %d\n", FID_(&fid), uid));

    if (uid == ALL_UIDS && AnyUser.valid)
	AnyUser.valid = 0;

    for (int i = 0; i < CPSIZE; i++)
	if ((uid == ALL_UIDS || SpecificUser[i].uid == uid) && SpecificUser[i].valid)
	    SpecificUser[i].valid = 0;
}


/* Need not be called from within transaction. */
void fsobj::PromoteAcRights(uid_t uid)
{
    LOG(100, ("fsobj::PromoteAcRights: (%s), uid = %d\n", FID_(&fid), uid));

    if (uid == ALL_UIDS) {
	AnyUser.valid = 1;

	/* 
	 * if other users who have rights in the cache also have
	 * tokens, promote their rights too. 
	 */
	for (int i = 0; i < CPSIZE; i++)
	    if (SpecificUser[i].inuse && !SpecificUser[i].valid) {
		userent *ue = vol->realm->GetUser(SpecificUser[i].uid);
		int tokensvalid = ue->TokensValid();
		PutUser(&ue);
		if (tokensvalid) SpecificUser[i].valid = 1;
	    }
    } else {
	/* 
	 * Make sure tokens didn't expire for this user while
	 * the RPC was in progress. If we set them anyway, and
	 * he goes disconnected, he may have access to files he 
	 * otherwise wouldn't have because he lost tokens.
	 */
	userent *ue = vol->realm->GetUser(uid);
	int tokensvalid = ue->TokensValid();
	PutUser(&ue);
	if (!tokensvalid) return;

	for (int i = 0; i < CPSIZE; i++)
	    if (SpecificUser[i].uid == uid)
		SpecificUser[i].valid = 1;
    }
}


/* MUST be called from within transaction! */
void fsobj::ClearAcRights(uid_t uid)
{
    LOG(100, ("fsobj::ClearAcRights: (%s), uid = %d\n", FID_(&fid), uid));

    if (uid == ALL_UIDS) {
	RVMLIB_REC_OBJECT(AnyUser);
	AnyUser = NullAcRights;
    }

    for (int i = 0; i < CPSIZE; i++)
	if (uid == ALL_UIDS || SpecificUser[i].uid == uid) {
	    RVMLIB_REC_OBJECT(SpecificUser[i]);
	    SpecificUser[i] = NullAcRights;
	}
}


/* local-repair modification */
/* MUST be called from within transaction (at least if <vnode, unique> != pfid.<Vnode, Unique>)! */
void fsobj::SetParent(VnodeId vnode, Unique_t unique) {
    if (IsRoot() || (vnode == 0 && unique == 0) || LRDB->RFM_IsGlobalRoot(&fid))
	return;

    /* Update pfid if necessary. */
    if (pfid.Vnode != vnode || pfid.Unique != unique) {
	/* Detach from old parent if necessary. */
	if (pfso != 0) {
	    pfso->DetachChild(this);
	    pfso = 0;
	}

	/* Install new parent fid. */
	RVMLIB_REC_OBJECT(pfid);
	pfid = fid;
	pfid.Vnode = vnode;
	pfid.Unique = unique;
    }

    /* Attach to new parent if possible. */
    if (pfso == 0) {
	fsobj *pf = FSDB->Find(&pfid);
	if (pf != 0 && HAVESTATUS(pf) && !GCABLE(pf)) {
	    pfso = pf;
	    pfso->AttachChild(this);
	}
    }
}


/* MUST be called from within transaction! */
void fsobj::MakeDirty() {
    if (DIRTY(this)) return;

    LOG(1, ("fsobj::MakeDirty: (%s)\n", FID_(&fid)));

    /* We must have data here */
    /* Not really, we could have just created this object while disconnected */
    /* CODA_ASSERT(HAVEALLDATA(this)); */

    RVMLIB_REC_OBJECT(flags);
    flags.dirty = 1;
    RVMLIB_REC_OBJECT(CleanStat);
    CleanStat.Length = stat.Length;
    CleanStat.Date = stat.Date;

    DisableReplacement();
}


/* MUST be called from within transaction! */
void fsobj::MakeClean() {
    if (!DIRTY(this)) return;

    LOG(1, ("fsobj::MakeClean: (%s)\n", FID_(&fid)));

    RVMLIB_REC_OBJECT(flags);
    flags.dirty = 0;

    EnableReplacement();
}


/*  *****  Mount State  *****  */
/* local-repair modification */
/* MUST NOT be called from within transaction! */
/* Call with object write-locked. */
int fsobj::TryToCover(VenusFid *inc_fid, uid_t uid)
{
    if (!HAVEALLDATA(this))
	{ print(logFile); CHOKE("fsobj::TryToCover: called without data"); }

    LOG(10, ("fsobj::TryToCover: fid = (%s)\n", FID_(&fid)));

    int code = 0;

    /* Don't cover mount points in backup volumes! */
    if (!IsLocalObj() && vol->IsBackup())
	return(ENOENT); /* ELOOP? */

    /* Check for bogosities. */
    int len = (int) stat.Length;
    if (len < 2) {
	eprint("TryToCover: bogus link length");
	return(EINVAL);
    }
    char type = data.symlink[0];
    switch(type) {
	case '#':
	case '@':
	    break;

	default:
	    eprint("TryToCover: bogus mount point type (%c)", type);
	    return(EINVAL);
    }

    /* Look up the volume that is to be mounted on us. */

    /* Turn volume name into a proper string. */
    data.symlink[len-1] = '\0';

    volent *tvol = 0;
    if (IsFake()) {
	Volid vid;
	char *realmname, tmp;
	Realm *r = vol->realm;
	int n;

	n = sscanf(data.symlink, "@%lx.%*x.%*x@%c", &vid.Volume, &tmp);
	if (n < 1) {
	    print(logFile);
	    CHOKE("fsobj::TryToCover: couldn't get volume id");
	}

	r->GetRef();

	if (n == 2) {
	    /* strrchr should succeed now because sscanf succeeded. */
	    realmname = strrchr(data.symlink, '@')+1;

	    r->PutRef();
	    r = REALMDB->GetRealm(realmname);
	}
	vid.Realm = r->Id();

	code = VDB->Get(&tvol, &vid);

	r->PutRef();
    }
    else
	code = VDB->Get(&tvol, vol->realm, &data.symlink[1], this);

    if (code != 0) {
	LOG(100, ("fsobj::TryToCover: vdb::Get(%s) failed (%d)\n", data.symlink, code));
	return(code);
    }

    /* Don't allow a volume to be mounted inside itself! */
    /* but only when its mount root is the global-root-obj of a local subtree */
    if (fid.Realm == tvol->GetRealmId() && fid.Volume == tvol->GetVolumeId() &&
	!LRDB->RFM_IsGlobalChild(&fid)) {
	eprint("TryToCover(%s): recursive mount!", data.symlink);
	VDB->Put(&tvol);
	return(ELOOP);
    }

    /* Only allow cross-realm mountpoints to or from the local realm. */
    if (vol->GetRealmId() != tvol->GetRealmId()) {
	if (vol->GetRealmId() != LocalRealm->Id() &&
	    tvol->GetRealmId() != LocalRealm->Id()) {
	    VDB->Put(&tvol);
	    return ELOOP;
	}
    }

    /* Get volume root. */
    fsobj *rf = 0;
    VenusFid root_fid;
    root_fid.Realm = tvol->GetRealmId();
    root_fid.Volume = tvol->vid;
    if (IsFake()) {
	if (sscanf(data.symlink, "@%*x.%lx.%lx", &root_fid.Vnode, &root_fid.Unique) != 2)
	    { print(logFile); CHOKE("fsobj::TryToCover: couldn't get <tvolid, tunique>"); }
    }
    else {
	    FID_MakeRoot(MakeViceFid(&root_fid));
    }
    code = FSDB->Get(&rf, &root_fid, uid, RC_STATUS, comp);
    if (code != 0) {
	LOG(100, ("fsobj::TryToCover: Get root (%s) failed (%d)\n",
		  FID_(&root_fid), code));

	VDB->Put(&tvol);
	if (code == EINCONS && inc_fid != 0) *inc_fid = root_fid;
	return(code);
    }
    rf->PromoteLock();

    /* If root is currently mounted, uncover the mount point and unmount. */
    Recov_BeginTrans();
    fsobj *mf = rf->u.mtpoint;
    if (mf != 0) {
	    if (mf == this) {
		    eprint("TryToCover: re-mounting (%s) on (%s)", tvol->name, comp);
		    UncoverMtPt();
	    } else {
		    if (mf->u.root != rf)
			    { mf->print(logFile); rf->print(logFile); CHOKE("TryToCover: mf->root != rf"); }
		    mf->UncoverMtPt();
	    }
	    rf->UnmountRoot();
    }
    Recov_EndTrans(MAXFP);

    /* Do the mount magic. */
    Recov_BeginTrans();
    if (IsFake() && !rf->IsRoot()) {
	    RVMLIB_REC_OBJECT(rf->mvstat);
	    RVMLIB_REC_OBJECT(rf->u.mtpoint);
	    rf->mvstat = ROOT;
	    rf->u.mtpoint = 0;
    }
    rf->MountRoot(this);
    CoverMtPt(rf);
    Recov_EndTrans(MAXFP);

    FSDB->Put(&rf);
    VDB->Put(&tvol);
    return(0);
}


/* MUST be called from within transaction! */
/* Call with object write-locked. */
void fsobj::CoverMtPt(fsobj *root_fso) {
    if (!IsNormal())
	{ print(logFile); CHOKE("fsobj::CoverMtPt: mvstat != NORMAL"); }
    if (!data.symlink)
	{ print(logFile); CHOKE("fsobj::CoverMtPt: no data.symlink!"); }

    LOG(10, ("fsobj::CoverMtPt: fid = (%s), rootfid = (%s)\n",
	     FID_(&fid), FID_(&root_fso->fid)));

    RVMLIB_REC_OBJECT(*this);

    /* Exit old state (NORMAL). */
    k_Purge(&fid, 1);

    /* Enter new state (MOUNTPOINT). */
    mvstat = MOUNTPOINT;
    u.root = root_fso;
    DisableReplacement();
}


/* MUST be called from within transaction! */
/* Call with object write-locked. */
void fsobj::UncoverMtPt() {
    if (!IsMtPt()) 
	{ print(logFile); CHOKE("fsobj::UncoverMtPt: mvstat != MOUNTPOINT"); }
    if (!u.root)
	{ print(logFile); CHOKE("fsobj::UncoverMtPt: no u.root!"); }

    LOG(10, ("fsobj::UncoverMtPt: fid = (%s), rootfid = (%s)\n",
	      FID_(&fid), FID_(&u.root->fid)));

    RVMLIB_REC_OBJECT(*this);

    /* Exit old state (MOUNTPOINT). */
    u.root = 0;
    k_Purge(&fid, 1);			/* I don't think this is necessary. */
    k_Purge(&pfid, 1);			/* This IS necessary. */

    /* Enter new state (NORMAL). */
    mvstat = NORMAL;
    EnableReplacement();
}


/* MUST be called from within transaction! */
/* Call with object write-locked. */
void fsobj::MountRoot(fsobj *mtpt_fso) {
    if (!IsRoot())
	{ print(logFile); CHOKE("fsobj::MountRoot: mvstat != ROOT"); }
    if (u.mtpoint)
	{ print(logFile); CHOKE("fsobj::MountRoot: u.mtpoint exists!"); }

    LOG(10, ("fsobj::MountRoot: fid = %s, mtptfid = %s\n",
	     FID_(&fid), FID_(&mtpt_fso->fid)));

    RVMLIB_REC_OBJECT(*this);

    /* Exit old state (ROOT, without link). */
    k_Purge(&fid);

    /* Enter new state (ROOT, with link). */
    u.mtpoint = mtpt_fso;
}


/* MUST be called from within transaction! */
/* Call with object write-locked. */
void fsobj::UnmountRoot() {
    if (!IsRoot()) 
	{ print(logFile); CHOKE("fsobj::UnmountRoot: mvstat != ROOT"); }
    if (!u.mtpoint)
	{ print(logFile); CHOKE("fsobj::UnmountRoot: no u.mtpoint!"); }

    LOG(10, ("fsobj::UnmountRoot: fid = (%s), mtptfid = (%s)\n",
	      FID_(&fid), FID_(&u.mtpoint->fid)));

    RVMLIB_REC_OBJECT(*this);

    /* Exit old state (ROOT, with link). */
    u.mtpoint = 0;
    k_Purge(&fid);

    /* Enter new state (ROOT, without link). */
    if (!FID_IsVolRoot(&fid)) {
	mvstat = NORMAL;	    /* couldn't be mount point, could it? */       
	/* this object could be the global root of a local/global subtree */
	if (FID_EQ(&pfid, &NullFid) && !IsLocalObj()) {
	    LOG(0, ("fsobj::UnmountRoot: (%s) a previous mtroot without pfid, kill it\n",
		    FID_(&fid)));
	    Kill();
	}
    }
}


/*  *****  Child/Parent Linkage  *****  */

/* Need not be called from within transaction. */
void fsobj::AttachChild(fsobj *child) {
    if (!IsDir())
	{ print(logFile); child->print(logFile); CHOKE("fsobj::AttachChild: not dir"); }

    LOG(100, ("fsobj::AttachChild: (%s), (%s)\n",
	       FID_(&fid), FID_(&child->fid)));

    DisableReplacement();

    if (child->child_link.is_linked())
	{ print(logFile); child->print(logFile); CHOKE("fsobj::AttachChild: bad child"); }
    if (children == 0)
	children = new dlist;
    children->prepend(&child->child_link);

    DemoteHdbBindings();	    /* in case an expansion would now be satisfied! */
}


/* Need not be called from within transaction. */
void fsobj::DetachChild(fsobj *child) {
    if (!IsDir())
	{ print(logFile); child->print(logFile); CHOKE("fsobj::DetachChild: not dir"); }

    LOG(100, ("fsobj::DetachChild: (%s), (%s)\n",
	       FID_(&fid), FID_(&child->fid)));

    DemoteHdbBindings();	    /* in case an expansion would no longer be satisfied! */

    if (child->pfso != this || !child->child_link.is_linked() ||
	 children == 0 || children->count() == 0)
	{ print(logFile); child->print(logFile); CHOKE("fsobj::DetachChild: bad child"); }
    if (children->remove(&child->child_link) != &child->child_link)
	{ print(logFile); child->print(logFile); CHOKE("fsobj::DetachChild: remove failed"); }

    EnableReplacement();
}


/*  *****  Priority State  *****  */

/* Need not be called from within transaction. */
void fsobj::Reference() {
    LOG(100, ("fsobj::Reference: (%s), old = %d, new = %d\n",
	       FID_(&fid), FSDB->LastRef[ix], FSDB->RefCounter));

    FSDB->LastRef[ix] = FSDB->RefCounter++;
}

/* local-repair modification */
/* Need not be called from within transaction. */
void fsobj::ComputePriority(int Force) {
    LOG(1000, ("fsobj::ComputePriority: (%s)\n", FID_(&fid)));

    if (IsLocalObj()) {
	LOG(1000, ("fsobj::ComputePriority: local object\n"));
	return;
    }
    FSDB->Recomputes++;

    int new_priority = 0;
    {
	/* Short-term priority (spri) is a function of how recently object was used. */
	/* Define "spread" to be the difference between the most recent */
	/* reference to any object and the most recent reference to this object. */
	/* Let "rank" be a function which scales "spread" over 0 to FSO_MAX_SPRI - 1. */
	/* Then, spri(f) :: FSO_MAX_SPRI - rank(spread(f)) */
	int spri = 0;
	int LastRef = (int) FSDB->LastRef[ix];
	if (LastRef > 0) {
	    int spread = (int) FSDB->RefCounter - LastRef - 1;
	    int rank = spread;
	    {
		/* "rank" depends upon FSO_MAX_SPRI, fsdb::MaxFiles, and a scaling factor. */
		static int initialized = 0;
		static int RightShift;
		if (!initialized) {
#define	log2(x)\
    (ffs(binaryfloor((x) + (x) - 1) - 1))
		    int LOG_MAXFILES = log2(FSDB->MaxFiles);
		    int	LOG_SSF = log2(FSDB->ssf);
		    int LOG_MAX_SPRI = log2(FSO_MAX_SPRI);
		    RightShift = (LOG_MAXFILES + LOG_SSF - LOG_MAX_SPRI);
		    initialized = 1;
		}
		if (RightShift > 0) rank = spread >> RightShift;
		else if (RightShift < 0) rank = spread << (-RightShift);
		if (rank >= FSO_MAX_SPRI) rank = FSO_MAX_SPRI - 1;
	    }
	    spri = FSO_MAX_SPRI - rank;
	}

	/* Medium-term priority (mpri) is just the current Hoard priority. */
	int mpri = HoardPri;

	new_priority = FSDB->MakePri(spri, mpri);
    }

    /* Force is only set by RecomputePriorities when called by the Hoard
     * daemon (once every 10 minutes). By forcefully taking all FSO's off
     * the priority queue and requeueing them the random seed is perturbed
     * to avoid cache pollution by unreferenced low priority objects which
     * happen to have a high random seed */
    if (Force || priority == -1 || new_priority != priority) {
	FSDB->Reorders++;		    /* transient value; punt set_range */

	DisableReplacement();		    /* remove... */
	priority = new_priority;	    /* update key... */
	EnableReplacement();		    /* reinsert... */
    }
}


/* local-repair modification */
/* Need not be called from within transaction. */
void fsobj::EnableReplacement() {
#ifdef	VENUSDEBUG
    /* Sanity checks. */
/*
    if (DYING(this)) {
	if (*((dlink **)&del_handle) == 0)
	    { print(logFile); CHOKE("fsobj::EnableReplacement: dying && del_handle = 0"); }
	return;
    }
*/
#endif /* VENUSDEBUG */

    /* Already replaceable? */
    if (REPLACEABLE(this))
	return;

    /* Are ALL conditions for replaceability met? */
    if (DYING(this) || !HAVESTATUS(this) || DIRTY(this) ||
	 READING(this) || WRITING(this) || (children && children->count() > 0) ||
	 IsMtPt() || (IsSymLink() && hdb_bindings && hdb_bindings->count() > 0))
	return;

    /* Sanity check. */
    if (priority == -1 && !IsLocalObj())
	eprint("EnableReplacement(%s): priority unset", FID_(&fid));

    LOG(1000,("fsobj::EnableReplacement: (%s), priority = [%d (%d) %d %d]\n",
	      FID_(&fid), priority, flags.random, HoardPri, FSDB->LastRef[ix]));

#ifdef	VENUSDEBUG
    if (LogLevel >= 10000)
	FSDB->prioq->print(logFile);
#endif

    FSDB->prioq->insert(&prio_handle);

#ifdef	VENUSDEBUG
    if (LogLevel >= 10000 && !(FSDB->prioq->IsOrdered()))
	{ print(logFile); FSDB->prioq->print(logFile); CHOKE("fsobj::EnableReplacement: !IsOrdered after insert"); }
#endif
}


/* Need not be called from within transaction. */
void fsobj::DisableReplacement() {
#ifdef	VENUSDEBUG
    /* Sanity checks. */
/*
    if (DYING(this)) {
	if (*((dlink **)&del_handle) == 0)
	    { print(logFile); CHOKE("fsobj::DisableReplacement: dying && del_handle = 0"); }
	return;
    }
*/
#endif

    /* Already not replaceable? */
    if (!REPLACEABLE(this))
	return;

    LOG(1000,("fsobj::DisableReplacement: (%s), priority = [%d (%d) %d %d]\n",
	      FID_(&fid), priority, flags.random, HoardPri, FSDB->LastRef[ix]));

#ifdef	VENUSDEBUG
    if (LogLevel >= 10000)
	FSDB->prioq->print(logFile);
#endif

    if (FSDB->prioq->remove(&prio_handle) != &prio_handle)
	{ print(logFile); CHOKE("fsobj::DisableReplacement: prioq remove"); }

#ifdef	VENUSDEBUG
    if (LogLevel >= 10000 && !(FSDB->prioq->IsOrdered()))
	{ print(logFile); FSDB->prioq->print(logFile); CHOKE("fsobj::DisableReplacement: !IsOrdered after remove"); }
#endif
}


binding *CheckForDuplicates(dlist *hdb_bindings_list, void *binder)
{
    /* If the list is empty, this can't be a duplicate */
    if (hdb_bindings_list == NULL)
        return(0);

    /* Look for this binder */
    dlist_iterator next(*hdb_bindings_list);
    dlink *d;
    while ((d = next())) {
	binding *b = strbase(binding, d, bindee_handle);
	if (b->binder == binder) {
	  /* Found it! */
	  return(b);
	}
    }

    /* If we had found it, we wouldn't have gotten here! */
    return(NULL);
}

/* Need not be called from within transaction. */
void fsobj::AttachHdbBinding(binding *b)
{
    binding *dup;

    /* Sanity checks. */
    if (b->bindee != 0) {
	print(logFile);
	b->print(logFile);
	CHOKE("fsobj::AttachHdbBinding: bindee != 0");
    }

    /* Check for duplicates */
    if ((dup = CheckForDuplicates(hdb_bindings, b->binder))) {
	LOG(100, ("This is a duplicate binding...skip it.\n"));
        return;
    }

    if (LogLevel >= 1000) {
	dprint("fsobj::AttachHdbBinding:\n");
	print(logFile);
    }

    /* Attach ourselves to the binding. */
    if (!hdb_bindings)
	hdb_bindings = new dlist;
    hdb_bindings->insert(&b->bindee_handle);
    b->bindee = this;
    b->IncrRefCount();

    if (LogLevel >= 10) {
      dprint("fsobj::AttachHdbBinding:\n");
      print(logFile);
      b->print(logFile);
    }

    if (IsSymLink())
	DisableReplacement();

    /* Recompute our priority if necessary. */
    namectxt *nc = (namectxt *)b->binder;
    if (nc->priority > HoardPri) {
	HoardPri = nc->priority;
	HoardVuid = nc->uid;
	ComputePriority();
    }
}


/* Need not be called from within transaction. */
void fsobj::DemoteHdbBindings() {
    if (hdb_bindings == 0) return;

    dlist_iterator next(*hdb_bindings);
    dlink *d;
    while ((d = next())) {
	binding *b = strbase(binding, d, bindee_handle);
	DemoteHdbBinding(b);
    }
}


/* Need not be called from within transaction. */
void fsobj::DemoteHdbBinding(binding *b) {
    /* Sanity checks. */
    if (b->bindee != this) {
	print(logFile);
	if (b != 0) b->print(logFile);
	CHOKE("fsobj::DemoteHdbBinding: bindee != this");
    }
    if (LogLevel >= 1000) {
	dprint("fsobj::DemoteHdbBinding:\n");
	print(logFile);
	b->print(logFile);
    }

    /* Update the state of the binder. */
    namectxt *nc = (namectxt *)b->binder;
    nc->Demote();
}


/* Need not be called from within transaction. */
void fsobj::DetachHdbBindings() {
    if (hdb_bindings == 0) return;

    dlink *d;
    while ((d = hdb_bindings->first())) {
	binding *b = strbase(binding, d, bindee_handle);
	DetachHdbBinding(b, 1);
    }
}


/* Need not be called from within transaction. */
void fsobj::DetachHdbBinding(binding *b, int DemoteNameCtxt) {
  struct timeval StartTV;
  struct timeval EndTV;
  float elapsed;

    /* Sanity checks. */
    if (b->bindee != this) {
	print(logFile);
	if (b != 0) b->print(logFile);
	CHOKE("fsobj::DetachHdbBinding: bindee != this");
    }
    if (LogLevel >= 1000) {
	dprint("fsobj::DetachHdbBinding:\n");
	print(logFile);
	b->print(logFile);
    }

    /* Detach ourselves from the binding. */
    if (hdb_bindings->remove(&b->bindee_handle) != &b->bindee_handle)
	{ print(logFile); b->print(logFile); CHOKE("fsobj::DetachHdbBinding: bindee remove"); }
    b->bindee = 0;
    b->DecrRefCount();
    if (IsSymLink() && hdb_bindings->count() == 0)
	EnableReplacement();

    /* Update the state of the binder. */
    namectxt *nc = (namectxt *)b->binder;
    if (DemoteNameCtxt)
	nc->Demote();

    /* Recompute our priority if necessary. */
    if (nc->priority == HoardPri) {
	int new_HoardPri = 0;
	uid_t new_HoardVuid = HOARD_UID;
    gettimeofday(&StartTV, 0);
    LOG(10, ("Detach: hdb_binding list contains %d namectxts\n", hdb_bindings->count()));
	dlist_iterator next(*hdb_bindings);
	dlink *d;
	while ((d = next())) {
	    binding *b = strbase(binding, d, bindee_handle);
	    namectxt *nc = (namectxt *)b->binder;
	    if (nc->priority > new_HoardPri) {
		new_HoardPri = nc->priority;
		new_HoardVuid = nc->uid;
	    }
	}
    gettimeofday(&EndTV, 0);
    elapsed = SubTimes(&EndTV, &StartTV);
    LOG(10, ("fsobj::DetachHdbBinding: recompute, elapsed= %3.1f\n", elapsed));

	if (new_HoardPri < HoardPri) {
	    HoardPri = new_HoardPri;
	    HoardVuid = new_HoardVuid;
	    ComputePriority();
	}
    }
}

/*
 * This routine attempts to automatically decide whether or not the hoard
 * daemon should fetch an object.  There are three valid return values:
 *	-1: the object should definitely NOT be fetched
 *	 0: the routine cannot automatically determine the fate of this 
 *	    object; the user should be given the option
 *	 1: the object should definitely be fetched
 *
 * As a first guess to the "real" function, we will use 
 * 		ALPHA + (BETA * e^(priority/GAMMA)).
 * An object's priority ranges as high as 100,000 (use formula in Jay's thesis 
 * but instead use 75 for the value of alpha and 25 for the value of 1-alpha).
 * This is (presumably) to keep all of the priorities integers.
 * 
 */
int fsobj::PredetermineFetchState(int estimatedCost, int hoard_priority) {
    double acceptableCost;
    double x;

    if (estimatedCost == -1)
        return(0);

    /* Scale it up correctly... from range 1-1000 to 1-100000 */
    hoard_priority = hoard_priority * 100;

    LOG(100, ("fsobj::PredetermineFetchState(%d)\n",estimatedCost));
    LOG(100, ("PATIENCE_ALPHA = %d, PATIENCE_BETA = %d; PATIENCE_GAMMA = %d\n", 
	      PATIENCE_ALPHA, PATIENCE_BETA, PATIENCE_GAMMA));
    LOG(100, ("priority = %d; HoardPri = %d, hoard_priority = %d\n",
	      priority, HoardPri, hoard_priority));

    x = (double)hoard_priority / (double)PATIENCE_GAMMA;
    acceptableCost = (double)PATIENCE_ALPHA + ((double)PATIENCE_BETA * exp(x));

    if ((hoard_priority == 100000) || (estimatedCost < acceptableCost)) {
        LOG(100, ("fsobj::PredetermineFetchState returns 1 (definitely fetch) \n"));
        return(1);
    }
    else {
	LOG(100, ("fsobj::PredetermineFetchState returns 0 (ask the user) \n"));
        return(0);
    }
}

CacheMissAdvice fsobj::ReadDisconnectedCacheMiss(vproc *vp, uid_t uid)
{
    char pathname[MAXPATHLEN];
    CacheMissAdvice advice;

    LOG(100, ("E fsobj::ReadDisconnectedCacheMiss\n"));

    /* If advice not enabled, simply return */
    if (!SkkEnabled) {
        LOG(100, ("ADVSKK STATS:  RDCM Advice NOT enabled.\n"));
        return(FetchFromServers);
    }

    /* Check that:                                                     *
     *     (a) the request did NOT originate from the Hoard Daemon     *
     *     (b) the request did NOT originate from that AdviceMonitor,  *
     * and (c) the user is running an AdviceMonitor,                   */
    CODA_ASSERT(vp != NULL);
    if (vp->type == VPT_HDBDaemon) {
	LOG(100, ("ADVSKK STATS:  RDCM Advice inappropriate.\n"));
        return(FetchFromServers);
    }
    if (adv_mon.skkPgid(vp->u.u_pgid)) {
        LOG(100, ("ADVSKK STATS:  RDCM Advice inappropriate.\n"));
        return(FetchFromServers);
    }
    if (!(adv_mon.ConnValid())) {
        LOG(100, ("ADVSKK STATS:  RDCM Advice NOT valid. (uid = %d)\n", uid));
        return(FetchFromServers);
    }

    GetPath(pathname, 1);

    LOG(100, ("Requesting ReadDisconnected CacheMiss Advice for path=%s, pid=%d...\n", pathname, vp->u.u_pid));
    advice = adv_mon.ReadDisconnectedAdvice(&fid, pathname, vp->u.u_pgid);
    return(advice);
}

CacheMissAdvice fsobj::WeaklyConnectedCacheMiss(vproc *vp, uid_t uid)
{
    char pathname[MAXPATHLEN];
    CacheMissAdvice advice;
    unsigned long CurrentBandwidth;

    LOG(100, ("E fsobj::WeaklyConnectedCacheMiss\n"));

    /* If advice not enabled, simply return */
    if (!SkkEnabled) {
        LOG(100, ("ADVSKK STATS:  WCCM Advice NOT enabled.\n"));
        return(FetchFromServers);
    }

    /* Check that:                                                     *
     *     (a) the request did NOT originate from the Hoard Daemon     *
     *     (b) the request did NOT originate from that AdviceMonitor,  *
     * and (c) the user is running an AdviceMonitor,                   */
    CODA_ASSERT(vp != NULL);
    if (vp->type == VPT_HDBDaemon) {
	LOG(100, ("ADVSKK STATS:  WCCM Advice inappropriate.\n"));
        return(FetchFromServers);
    }
    if (adv_mon.skkPgid(vp->u.u_pgid)) {
        LOG(100, ("ADVSKK STATS:  WCCM Advice inappropriate.\n"));
        return(FetchFromServers);
    }
    if (!(adv_mon.ConnValid())) {
        LOG(100, ("ADVSKK STATS:  WCCM Advice NOT valid. (uid = %d)\n", uid));
        return(FetchFromServers);
    }

    GetPath(pathname, 1);

    LOG(100, ("Requesting WeaklyConnected CacheMiss Advice for path=%s, pid=%d...\n", 
	      pathname, vp->u.u_pid));
    vol->GetBandwidth(&CurrentBandwidth);
    advice = adv_mon.WeaklyConnectedAdvice(&fid, pathname, vp->u.u_pid,
					   stat.Length, CurrentBandwidth,
					   cf.Name());
    return(advice);
}

/*  *****  MLE Linkage  *****  */

/* MUST be called from within transaction! */
void fsobj::AttachMleBinding(binding *b) {
    /* Sanity checks. */
    if (b->bindee != 0) {
	print(logFile);
	b->print(logFile);
	CHOKE("fsobj::AttachMleBinding: bindee != 0");
    }
    if (LogLevel >= 1000) {
	dprint("fsobj::AttachMleBinding:\n");
	print(logFile);
	b->print(logFile);
    }

    /* Attach ourselves to the binding. */
    if (mle_bindings == 0)
	mle_bindings = new dlist;
    mle_bindings->append(&b->bindee_handle);
    b->bindee = this;
    b->IncrRefCount();

    /* Set our "dirty" flag if this is the first binding. (i.e. this fso has an mle) */
    if (mle_bindings->count() == 1) {
	MakeDirty();
    }
    else {
	FSO_ASSERT(this, DIRTY(this));
    }
}


/* MUST be called from within transaction! */
void fsobj::DetachMleBinding(binding *b) {
    /* Sanity checks. */
    if (b->bindee != this) {
	print(logFile);
	if (b != 0) b->print(logFile);
	CHOKE("fsobj::DetachMleBinding: bindee != this");
    }
    if (LogLevel >= 1000) {
	dprint("fsobj::DetachMleBinding:\n");
	print(logFile);
	b->print(logFile);
    }
    FSO_ASSERT(this, DIRTY(this));
    FSO_ASSERT(this, mle_bindings != 0);

    /* Detach ourselves from the binding. */
    if (mle_bindings->remove(&b->bindee_handle) != &b->bindee_handle)
	{ print(logFile); b->print(logFile); CHOKE("fsobj::DetachMleBinding: bindee remove"); }
    b->bindee = 0;
    b->DecrRefCount();

    /* Clear our "dirty" flag if this was the last binding. */
    if (mle_bindings->count() == 0) {
	MakeClean();
    }
}


#ifdef REMOVE_THIS
/* MUST NOT be called from within transaction! */
void fsobj::CancelStores()
{
    if (!DIRTY(this))
	{ print(logFile); CHOKE("fsobj::CancelStores: !DIRTY"); }

    CODA_ASSERT(vol->IsReplicated());
    ((repvol *)vol)->CancelStores(&fid);
}
#endif


/*  *****  Data Contents  *****  */

/* MUST be called from within transaction! */
/* Call with object write-locked. */
/* If there are readers of this file, they will have it change from underneath them! */
void fsobj::DiscardData() {
    if (!HAVEDATA(this))
	{ print(logFile); CHOKE("fsobj::DiscardData: !HAVEDATA"); }
    if (WRITING(this) || EXECUTING(this))
	{ print(logFile); CHOKE("fsobj::DiscardData: WRITING || EXECUTING"); }

    LOG(10, ("fsobj::DiscardData: (%s)\n", FID_(&fid)));

    CODA_ASSERT(!DIRTY(this));

    RVMLIB_REC_OBJECT(data);
    switch(stat.VnodeType) {
	case File:
	    {
	    /* stat.Length() might have been changed, only data.file->Length()
	     * can be trusted */
	    FSDB->FreeBlocks(NBLOCKS(data.file->Length()));
	    data.file->Truncate(0);
	    data.file = 0;
	    }
	    break;

	case Directory:
	    {
	    /* Mount points MUST be unmounted before their data can be discarded! */
	    FSO_ASSERT(this, !IsMtPt());

	    /* Return cache-file blocks associated with Unix-format directory. */
	    if (data.dir->udcf) {
		FSDB->FreeBlocks(NBLOCKS(data.dir->udcf->Length()));
		data.dir->udcf->Truncate(0);
		data.dir->udcf = 0;
		data.dir->udcfvalid = 0;
	    }

	    /* Get rid of RVM data. */
	    DH_FreeData(&data.dir->dh);
	    rvmlib_rec_free(data.dir);
	    data.dir = 0;

	    break;
	    }
	case SymbolicLink:
	    {
	    /* Get rid of RVM data. */
	    rvmlib_rec_free(data.symlink);
	    data.symlink = 0;
	    }
	    break;

	case Invalid:
	    CHOKE("fsobj::DiscardData: bogus VnodeType (%d)", stat.VnodeType);
    }
}


/*  *****  Fake Object Management  *****  */

/* local-repair modification */
/* MUST NOT be called from within transaction! */
/* Transform a fresh fsobj into a fake directory or MTLink. */
/* Call with object write-locked. */
/* returns 0 if successful, ENOENT if the parent cannot
   be found. */
int fsobj::Fakeify()
{
    VenusFid fakefid;
    LOG(1, ("fsobj::Fakeify: %s, (%s)\n", comp, FID_(&fid)));

    fsobj *pf = 0;
    if (!IsRoot()) {
	if (fid.Volume == FakeRootVolumeId)
	    pf = FSDB->Find(&rootfid);

	else {
	    /* Laboriously scan database to find our parent! */
	    struct dllist_head *p;
	    list_for_each(p, vol->fso_list) {
		fsobj *pf = list_entry_plusplus(p, fsobj, vol_handle);

		if (!pf->IsDir() || pf->IsMtPt()) continue;
		if (!HAVEALLDATA(pf)) continue;
		if (!pf->dir_IsParent(&fid)) continue;

		/* Found! */
		break;
	    }
	    if (!pf) {
		LOG(0, ("fsobj::Fakeify: %s, (%s), parent not found\n",
			comp, FID_(&fid)));
		return(ENOENT);
	    }
	}
    }

    // Either (pf == 0 and this is the volume root) OR (pf != 0 and it isn't)

    Recov_BeginTrans();
    RVMLIB_REC_OBJECT(*this);

    /* Initialize status. */
    stat.DataVersion = 1;
    stat.Owner = V_UID;
    stat.Date = Vtime();
    
    if (pf) {
	pfid = pf->fid;
	pfso = pf;
	pf->AttachChild(this);
    }

    fakefid.Realm  = LocalRealm->Id();
    fakefid.Volume = FakeRootVolumeId;
    fakefid.Vnode  = 1;
    fakefid.Unique = 1;

    /* Are we creating objects for the fake root volume? */
    if (FID_VolEQ(&fid, &fakefid)) {
	if (FID_EQ(&fid, &fakefid)) { /* actual root directory? */
	    struct dllist_head *p;

	    stat.Mode = 0555;
	    stat.LinkCount = 2;
	    stat.VnodeType = Directory;
	    /* Access rights are not needed, this whole volume is readonly! */

	    /* Create the target directory. */
	    dir_MakeDir();
	    list_for_each(p, REALMDB->realms) {
		Realm *realm = list_entry_plusplus(p, Realm, realms);
		if (!realm->rootservers) continue;

		fakefid.Vnode = 0xfffffffc;
		fakefid.Unique = realm->Id();

		dir_Create(realm->Name(), &fakefid);
		LOG(20, ("fsobj::Fakeify: created fake codaroot entry for %s\n",
			 realm->Name()));
	    }
	    LOG(10, ("fsobj::Fakeify: created fake codaroot directory\n"));
	} else {
	    stat.Mode = 0644;
	    stat.LinkCount = 1;
	    stat.VnodeType = SymbolicLink;

	    /* "#@RRRRRRRRR." */
	    stat.Length = strlen(comp) + 3;
	    data.symlink = (char *)rvmlib_rec_malloc(stat.Length+1);
	    rvmlib_set_range(data.symlink, stat.Length+1);
	    sprintf(data.symlink, "#@%s.", comp);

	    UpdateCacheStats(&FSDB->FileDataStats, CREATE, BLOCKS(this));

	    LOG(10, ("fsobj::Fakeify: created realm mountlink %s\n",
		    data.symlink));
	}
	flags.local = 1;
	goto done;
    }

    fakefid.Volume = FakeRepairVolumeId;
    /* Are we creating objects in the fake repair volume? */
    if (FID_VolEQ(&fid, &fakefid)) {
	if (!FID_IsFakeRoot(MakeViceFid(&fid))) {
	    struct in_addr volumehosts[VSG_MEMBERS];
	    VolumeId volumeids[VSG_MEMBERS];
	    volent *pvol = u.mtpoint->vol;
	    repvol *vp = (repvol *)pvol;

	    CODA_ASSERT(pvol->IsReplicated());

	    stat.Mode = 0555;
	    stat.LinkCount = 2;
	    stat.VnodeType = Directory;
	    /* Access rights are not needed, this whole volume is readonly! */

	    /* Create the target directory. */
	    dir_MakeDir();

	    fakefid.Vnode = 0xfffffffc;

	    /* testing 1..2..3.. trying to show the local copy as well */
	    fakefid.Unique = vp->GetVolumeId();
	    dir_Create("localhost", &fakefid);

	    LOG(1, ("fsobj::Fakeify: new entry (localhost, %s)\n", FID_(&fakefid)));

	    /* Make entries for each of the rw-replicas. */
	    vp->GetHosts(volumehosts);
	    vp->GetVids(volumeids);
	    for (int i = 0; i < VSG_MEMBERS; i++) {
		if (!volumehosts[i].s_addr) continue;
		srvent *s = FindServer(&volumehosts[i]);

		fakefid.Unique = volumeids[i];
		dir_Create(s->name, &fakefid);

		LOG(1, ("fsobj::Fakeify: new entry (%s, %s)\n", s->name, FID_(&fakefid)));
	    }
	} else {
	    /* get the actual object we're mounted on */
	    fsobj *real_obj = pfso->u.mtpoint;
	    ViceFid LinkFid;
	    const char *realmname;

	    CODA_ASSERT(real_obj->vol->IsReplicated());

	    stat.Mode = 0644;
	    stat.LinkCount = 1;
	    stat.VnodeType = SymbolicLink;

	    LinkFid.Volume = fid.Unique;
	    LinkFid.Vnode  = real_obj->fid.Vnode;
	    LinkFid.Unique = real_obj->fid.Unique;
	    /* should really be vp->volrep[i]->realm->Name(), but
	     * cross-realm replication should probably not be attempted
	     * anyways, with authentication issues and all -JH */
	    realmname = real_obj->vol->realm->Name();

	    /* Write out the link contents. */
	    /* "@XXXXXXXX.YYYYYYYY.ZZZZZZZZ@RRRRRRRRR." */
	    stat.Length = 29 + strlen(realmname);
	    data.symlink = (char *)rvmlib_rec_malloc(stat.Length+1);
	    rvmlib_set_range(data.symlink, stat.Length+1);
	    sprintf(data.symlink, "@%08lx.%08lx.%08lx@%s.",
		    LinkFid.Volume, LinkFid.Vnode, LinkFid.Unique, realmname);

	    LOG(0, ("fsobj::Fakeify: making %s a symlink %s\n",
		    FID_(&fid), data.symlink));

	    UpdateCacheStats(&FSDB->FileDataStats, CREATE, BLOCKS(this));
	}
	flags.local = 1;
	goto done;
    }

    /* XXX I eventually want to end up removing the rest of this function -JH */
    LOG(0, ("fsobj::Fakeify: going into the old code\n"));

    if (FID_IsFakeRoot(MakeViceFid(&fid))) {		/* Fake MTLink */
	ViceFid LinkFid;
	const char *realmname;

	stat.Mode = 0644;
	stat.LinkCount = 1;
	stat.VnodeType = SymbolicLink;

	flags.fake = 1;

	/* local-repair modification */
	if (STREQ(comp, "local")) {
	    /* the first special case, fake link for a local object */
	    LOG(100,("fsobj::Fakeify: fake link for a local object %s\n",
		     FID_(&fid)));
	    LOG(100,("fsobj::Fakeify: parent fid for the fake link is %s\n",
		     FID_(&pfid)));
	    flags.local = 1;
	    VenusFid *Fid = LRDB->RFM_LookupLocalRoot(&pfid);

	    LinkFid = *MakeViceFid(Fid);
	    realmname = LocalRealm->Name();
	}
	else if (STREQ(comp, "global")) {
	    /* the second specical case, fake link for a global object */
	    LOG(100, ("fsobj::Fakeify: fake link for a global object %s\n",
		      FID_(&fid)));
	    LOG(100, ("fsobj::Fakeify: parent fid for the fake link is %s\n",
		      FID_(&pfid)));
	    flags.local = 1;
	    VenusFid *Fid = LRDB->RFM_LookupGlobalRoot(&pfid);
	    FSO_ASSERT(this, Fid && Fid->Realm == vol->realm->Id());

	    LinkFid = *MakeViceFid(Fid);
	    realmname = vol->realm->Name();
	} else {
	    CODA_ASSERT(vol->IsReplicated());
	    repvol *vp = (repvol *)vol;
	    struct in_addr host;
	    int i;

	    /* the normal fake link */
	    /* get the volumeid corresponding to the server name */
	    for (i = 0; i < VSG_MEMBERS; i++) {
		if (!vp->volreps[i]) continue;
		vp->volreps[i]->Host(&host);

		srvent *s = FindServer(&host);
		if (s && s->name && STREQ(s->name, comp))
		    break;
	    }
	    if (i == VSG_MEMBERS) // server not found 
		CHOKE("fsobj::fakeify couldn't find the server for %s\n",
		      comp);

	    LinkFid.Volume = vp->volreps[i]->GetVolumeId();
	    LinkFid.Vnode  = pfid.Vnode;
	    LinkFid.Unique = pfid.Unique;
	    realmname = vp->volreps[i]->realm->Name();
	}

	/* Write out the link contents. */
	/* "@XXXXXXXX.YYYYYYYY.ZZZZZZZZ@RRRRRRRRR." */
	stat.Length = 29 + strlen(realmname);
	data.symlink = (char *)rvmlib_rec_malloc(stat.Length+1);
	rvmlib_set_range(data.symlink, stat.Length+1);
	sprintf(data.symlink, "@%08lx.%08lx.%08lx@%s.",
		LinkFid.Volume, LinkFid.Vnode, LinkFid.Unique, realmname);

	LOG(0, ("fsobj::Fakeify: making %s a symlink %s\n",
		  FID_(&fid), data.symlink));

	UpdateCacheStats(&FSDB->FileDataStats, CREATE, BLOCKS(this));
    } else {				/* Fake Directory */
	stat.Mode = 0555;
	stat.LinkCount = 2;
	stat.VnodeType = Directory;
	/* Access rights are not needed, this whole volume is readonly! */

	/* Create the target directory. */
	dir_MakeDir();

	repvol *vp = (repvol *)vol;
	struct in_addr volumehosts[VSG_MEMBERS];

	flags.fake = 1;

	/* Make entries for each of the rw-replicas. */
	vp->GetHosts(volumehosts);
	for (int i = 0; i < VSG_MEMBERS; i++) {
	    if (!volumehosts[i].s_addr) continue;
	    srvent *s = FindServer(&volumehosts[i]);
	    char Name[CODA_MAXNAMLEN+1], *name;

	    if (s && s->name)
		name = s->name;
	    else
		name = inet_ntoa(volumehosts[i]);

	    snprintf(Name, CODA_MAXNAMLEN, "%s", name);
	    Name[CODA_MAXNAMLEN] = '\0';

	    VenusFid FakeFid = vp->GenerateFakeFid();
	    LOG(1, ("fsobj::Fakeify: new entry (%s, %s)\n",
		    Name, FID_(&FakeFid)));
	    dir_Create(Name, &FakeFid);
	}
    }

done:
    /* notify blocked threads that the fso is ready. */
    Matriculate();
    Recov_EndTrans(CMFP);

    return(0);
}


/*  *****  Local Synchronization  *****  */

void fsobj::Lock(LockLevel level) {
    LOG(1000, ("fsobj::Lock: (%s) level = %s\n",
	       FID_(&fid), ((level == RD)?"RD":"WR")));

    if (level != RD && level != WR)
	{ print(logFile); CHOKE("fsobj::Lock: bogus lock level %d", level); }

    FSO_HOLD(this);
    while (level == RD ? (writers > 0) : (writers > 0 || readers > 0))
    {
	LOG(0, ("WAITING(%s): level = %s, readers = %d, writers = %d\n",
		FID_(&fid), lvlstr(level), readers, writers));
	START_TIMING();
	VprocWait(&fso_sync);
	END_TIMING();
	LOG(0, ("WAIT OVER, elapsed = %3.1f\n", elapsed));
    }
    level == RD ? (readers++) : (writers++);
}


void fsobj::PromoteLock() {
    FSO_HOLD(this);
    UnLock(RD);
    Lock(WR);
    FSO_RELE(this);
}


void fsobj::DemoteLock() {
    FSO_HOLD(this);
    UnLock(WR);
    Lock(RD);
    FSO_RELE(this);
}


void fsobj::UnLock(LockLevel level) {
    LOG(1000, ("fsobj::UnLock: (%s) level = %s\n",
	       FID_(&fid), ((level == RD)?"RD":"WR")));

    if (level != RD && level != WR)
	{ print(logFile); CHOKE("fsobj::UnLock: bogus lock level %d", level); }

    if (refcnt <= 0)
	{ print(logFile); CHOKE("fsobj::UnLock: refcnt <= 0"); }
    (level == RD) ? (readers--) : (writers--);
    if (readers < 0 || writers < 0)
	{ print(logFile); CHOKE("fsobj::UnLock: readers = %d, writers = %d", readers, writers); }
    if (level == RD ? (readers == 0) : (writers == 0))
	VprocSignal(&fso_sync);
    FSO_RELE(this);
}


/*  *****  Miscellaneous Utility Routines  *****  */

void fsobj::GetVattr(struct coda_vattr *vap) {
    /* Most attributes are derived from the VenusStat structure. */
    vap->va_type = FTTOVT(stat.VnodeType);
    vap->va_mode = stat.Mode;

    if (S_ISREG(stat.Mode)) /* strip the setuid bits on files! */
        vap->va_mode &= ~(S_ISUID | S_ISGID);

    vap->va_uid = (uid_t) stat.Owner;
    vap->va_gid = V_GID;

    vap->va_fileid = (IsRoot() && u.mtpoint && !IsVenusRoot())
		       ? FidToNodeid(&u.mtpoint->fid)
		       : FidToNodeid(&fid);

    vap->va_nlink = stat.LinkCount;
    vap->va_blocksize = V_BLKSIZE;
    vap->va_rdev = 1;

    /* If the object is currently open for writing we must physically 
       stat it to get its size and time info. */
    if (WRITING(this)) {
	struct stat tstat;
	cf.Stat(&tstat);

	vap->va_size = tstat.st_size;
	vap->va_mtime.tv_sec = tstat.st_mtime;
	vap->va_mtime.tv_nsec = 0;
    }
    else
    {
	vap->va_size = (u_quad_t) stat.Length;
	vap->va_mtime.tv_sec = (time_t)stat.Date;
	vap->va_mtime.tv_nsec = 0;
    }

    /* Convert size of file to bytes of storage after getting size! */
    vap->va_bytes = NBLOCKS_BYTES(vap->va_size);

    /* We don't keep track of atime/ctime, so keep them identical to mtime */
    vap->va_atime = vap->va_mtime;
    vap->va_ctime = vap->va_mtime;

    VPROC_printvattr(vap);
}


void fsobj::ReturnEarly() {
    /* Only mutations on replicated objects can return early. */
    if (!vol->IsReplicated()) return;

    /* Only makes sense to return early to user requests. */
    vproc *v = VprocSelf();
    if (v->type != VPT_Worker) return;

    /* Oh man is this ugly. Why is this here and not in worker? -- DCS */
    /* Assumption: the opcode and unique fields of the w->msg->msg_ent are already filled in */
    worker *w = (worker *)v;
    switch (w->opcode) {
	union outputArgs *out;
	case CODA_CREATE:
	case CODA_MKDIR:
	    {	/* create and mkdir use exactly the same sized output structure */
	    if (w->msg == 0) CHOKE("fsobj::ReturnEarly: w->msg == 0");

	    out = (union outputArgs *)w->msg->msg_buf;
	    out->coda_create.oh.result = 0;
	    out->coda_create.Fid = *VenusToKernelFid(&fid);
	    DemoteLock();
	    GetVattr(&out->coda_create.attr);
	    PromoteLock();
	    w->Return(w->msg, sizeof (struct coda_create_out));
	    break;
	    }

	case CODA_CLOSE:
	    {
	    /* Don't return early here if we already did so in a callback handler! */
	    if (!FID_EQ(&w->StoreFid, &NullFid))
		w->Return(0);
	    break;
	    }

	case CODA_IOCTL:
	    {
	    /* Huh. IOCTL in the kernel thinks there may be return data. Assume not. */
	    out = (union outputArgs *)w->msg->msg_buf;
	    out->coda_ioctl.len = 0; 
	    out->coda_ioctl.oh.result = 0;
	    w->Return(w->msg, sizeof (struct coda_ioctl_out));
	    break;
	    }

	case CODA_LINK:
	case CODA_REMOVE:
	case CODA_RENAME:
	case CODA_RMDIR:
	case CODA_SETATTR:
	case CODA_SYMLINK:
	    w->Return(0);
	    break;

	default:
	    CHOKE("fsobj::ReturnEarly: bogus opcode (%d)", w->opcode);
    }
}


/* Need not be called from within transaction! */
void fsobj::GetPath(char *buf, int scope)
{
    if (IsRoot()) {
	if (scope == PATH_VOLUME) {
	    buf[0] = '\0';
	    return;
	}

	if (scope == PATH_REALM &&
	    (!u.mtpoint || vol->GetRealmId() != u.mtpoint->vol->GetRealmId()))
	{
	    buf[0] = '\0';
	    return;
	}

	if (IsVenusRoot()) {
	    strcpy(buf, venusRoot);
	    return;
	}

	if (!u.mtpoint) {
	    strcpy(buf, "???");
	    return;
	}

	u.mtpoint->GetPath(buf, scope);
	return;
    }

    if (!pfso && !FID_EQ(&pfid, &NullFid)) {
	fsobj *pf = FSDB->Find(&pfid);
	if (pf != 0 && HAVESTATUS(pf) && !GCABLE(pf)) {
	    pfso = pf;
	    pfso->AttachChild(this);
	}
    }

    if (pfso)
	pfso->GetPath(buf, scope);
    else
	strcpy(buf, "???");

    strcat(buf, "/");
    strcat(buf, comp);
}


int fsobj::MakeShadow()
{
    int err = 0;
    /*
     * Create a shadow, using a name both distinctive and that will
     * be garbage collected at startup.
     */
    if (!shadow) shadow = new CacheFile(-(ix+1));
    else	 shadow->IncRef();

    if (!shadow) return -1;

    /* As we only call MakeShadow during the freezing, and there is only one
     * reintegration at a time, we can sync the shadow copy with the lastest
     * version of the real file. -JH */
    /* As an optimization (to avoid blocking the reintegration too much) we
     * might want to do this only when we just created the shadow file or when
     * there are no writers to the real container file... Maybe later. -JH */
    Lock(RD);
    err = cf.Copy(shadow);
    UnLock(RD);

    return(err);
}


void fsobj::RemoveShadow()
{
    if (shadow->DecRef() == 0)
    {
	delete shadow;
	shadow = 0;
    }
}


/* Only call this on directory objects (or mount points)! */
/* Locking is irrelevant, but this routine MUST NOT yield! */
void fsobj::CacheReport(int fd, int level) {
    FSO_ASSERT(this, IsDir());

    /* Indirect over mount points. */
    if (IsMtPt()) {
	u.root->CacheReport(fd, level);
	return;
    }

    /* Report [slots, blocks] for this directory and its children. */
    int slots = 0;
    int blocks = 0;
    if (children != 0) {
	/* N.B. Recursion here could overflow smallish stacks! */
	dlist_iterator next(*children);
	dlink *d;
	while ((d = next())) {
	    fsobj *cf = strbase(fsobj, d, child_link);

	    slots++;
	    blocks += NBLOCKS(cf->cf.Length());
	}
    }
    fdprint(fd, "[ %3d  %5d ]      ", slots, blocks);
    for (int i = 0; i < level; i++) fdprint(fd, "   ");
    fdprint(fd, "%s\n", comp);

    /* Report child directories. */
    if (children != 0) {
	/* N.B. Recursion here could overflow smallish stacks! */
	dlist_iterator next(*children);
	dlink *d;
	while ((d = next())) {
	    fsobj *cf = strbase(fsobj, d, child_link);

	    if (cf->IsDir())
		cf->CacheReport(fd, level + 1);
	}
    }
}

/* 
 * This is a simple-minded routine that estimates the cost of fetching an
 * object.  It assumes that the fsobj has a reasonable estimate as to the 
 * size of the object stored in stat.Length.
 *
 * The routine takes one argument -- whether the status block (type == 0) 
 * or the actual data (type == 1) is being fetched.  The default is data.
 */
int fsobj::EstimatedFetchCost(int type)
{
    unsigned long bw;	/* bandwidth, in bytes/sec */

    LOG(100, ("E fsobj::EstimatedFetchCost(%d)\n", type));

    vol->GetBandwidth(&bw);

    LOG(100, ("stat.Length = %d; Bandwidth = %d\n", stat.Length, bw));
    LOG(100, ("EstimatedFetchCost = %d\n", (int)stat.Length/bw));

    return( (int)stat.Length/bw ); 
}

void fsobj::RecordReplacement(int status, int data)
{
#if 0
    char mountpath[MAXPATHLEN];
    char path[MAXPATHLEN];

    if (!SkkEnabled) return;

    LOG(10, ("RecordReplacement(%d,%d)\n", status, data));

    CODA_ASSERT(vol != NULL);
    vol->GetMountPath(mountpath, 0);
    GetPath(path, 1);    
    NotifyUserOfReplacement(&fid, path, status, (data ? 1 : 0));
#endif
}

/* local-repair modification */
void fsobj::print(int fdes) {
    /* < address, fid, comp, vol > */
    fdprint(fdes, "%#08x : fid = (%s), comp = %s, vol = %x\n",
	     (long)this, FID_(&fid), comp, vol);

    /* < FsoState, VenusStat, Replica Control Rights, Access Rights, flags > */
    fdprint(fdes, "\tstate = %s, stat = { %d, %d, %d, %d, %#o, %d, %s }, rc rights = %d\n",
	     PrintFsoState(state), stat.Length, stat.DataVersion,
	     stat.Date, stat.Owner, stat.Mode, stat.LinkCount,
	     PrintVnodeType(stat.VnodeType), RcRights);
    fdprint(fdes, "\tVV = {[");
    for (int i = 0; i < VSG_MEMBERS; i++)
	fdprint(fdes, " %d", (&(stat.VV.Versions.Site0))[i]);
    fdprint(fdes, " ] [ %#x %d ] [ %#x ]}\n",
	     stat.VV.StoreId.Host, stat.VV.StoreId.Uniquifier, stat.VV.Flags);
    if (IsDir()) {
	fdprint(fdes, "\tac rights = { [%x %x%x]",
		AnyUser.rights, AnyUser.inuse, AnyUser.valid);
	for (int i = 0; i < CPSIZE; i++)
	    fdprint(fdes, " [%d %x %x%x]",
		    SpecificUser[i].uid, SpecificUser[i].rights,
		    SpecificUser[i].inuse, SpecificUser[i].valid);
	fdprint(fdes, " }\n");
    }
    fdprint(fdes, "\tvoltype = [%d %d %d], fake = %d, fetching = %d local = %d\n",
	     vol->IsBackup(), vol->IsReplicated(), vol->IsReadWriteReplica(),
	     flags.fake, flags.fetching, flags.local);
    fdprint(fdes, "\trep = %d, data = %d, owrite = %d, dirty = %d, shadow = %d\n",
	     REPLACEABLE(this), HAVEDATA(this), flags.owrite, flags.dirty,
	     shadow != 0);

    /* < mvstat [rootfid | mtptfid] > */
    fdprint(fdes, "\tmvstat = %s", PrintMvStat(mvstat));
    if (IsMtPt())
	fdprint(fdes, ", root = (%s)", FID_(&u.root->fid));
    if (IsRoot() && u.mtpoint)
	fdprint(fdes, ", mtpoint = (%s)", FID_(&u.mtpoint->fid));
    fdprint(fdes, "\n");

    /* < parent_fid, pfso, child count > */
    fdprint(fdes, "\tparent = (%s, %x), children = %d\n",
	     FID_(&pfid), pfso, (children ? children->count() : 0));

    /* < priority, HoardPri, HoardVuid, hdb_bindings, LastRef > */
    fdprint(fdes, "\tpriority = %d (%d), hoard = [%d, %d, %d], lastref = %d\n",
	     priority, flags.random, HoardPri, HoardVuid,
	     (hdb_bindings ? hdb_bindings->count() : 0), FSDB->LastRef[ix]);
    if (hdb_bindings) {
      dlist_iterator next(*hdb_bindings);
      dlink *d;
      while ((d = next())) {
	binding *b = strbase(binding, d, bindee_handle);
	namectxt *nc = (namectxt *)b->binder;
	if (nc != NULL) 
	  nc->print(fdes, this);
      }

    }

    /* < mle_bindings, CleanStat > */
    fdprint(fdes, "\tmle_bindings = (%x, %d), cleanstat = [%d, %d]\n",
	     mle_bindings, (mle_bindings ? mle_bindings->count() : 0),
	     CleanStat.Length, CleanStat.Date);

    /* < cachefile, [directory | symlink] contents > */
    fdprint(fdes, "\tcachefile = ");
    cf.print(fdes);
    if (IsDir() && !IsMtPt()) {
	if (data.dir == 0) {
	    fdprint(fdes, "\tdirectory = 0\n");
	}
	else {
	    int pagecount = -1;
	    fdprint(fdes, "\tdirectory = %x, udcf = [%x, %d]\n",
		    data.dir, data.dir->udcf, data.dir->udcfvalid);
	    fdprint(fdes, "\tpages = %d, malloc bitmap = [ ", pagecount);
	    fdprint(fdes, "data at %p ", DH_Data(&(data.dir->dh)));
	    fdprint(fdes, "]\n");
	}
    }
    if (IsSymLink() || IsMtPt()) {
	fdprint(fdes, "\tlink contents: %s\n",
		data.symlink ? data.symlink : "N/A");
    }

    /* < references, openers > */
    fdprint(fdes, "\trefs = [%d %d %d], openers = [%d %d %d]",
	     readers, writers, refcnt,
	     (openers - Writers - Execers), Writers, Execers);
    fdprint(fdes, "\tlastresolved = %u\n", lastresolved);
}

void fsobj::ListCache(FILE *fp, int long_format, unsigned int valid)
{
  /* list in long format, if long_format == 1;
     list fsobjs
          such as valid (valid == 1), non-valid (valid == 2) or all (valid == 3) */

  char path[MAXPATHLEN];
  GetPath(path, 0);		/* Get relative pathname. */    

  switch (valid) {
  case 1: /* only valid */
    if ( DATAVALID(this) && STATUSVALID(this) )
      if (!long_format)
	ListCacheShort(fp);
      else
	ListCacheLong(fp);
    break;
  case 2: /* only non-valid */
    if ( !DATAVALID(this) || !STATUSVALID(this) )
      if (!long_format)
	ListCacheShort(fp);
      else
	ListCacheLong(fp);
    break;
  case 3: /* all */
  default:
      if (!long_format)
	ListCacheShort(fp);
      else
	ListCacheLong(fp);
  }
}

void fsobj::ListCacheShort(FILE* fp)
{
  char path[MAXPATHLEN];
  GetPath(path, 0);		/* Get relative pathname. */
  char valid = ((DATAVALID(this) && STATUSVALID(this) && !DIRTY(this)) ? ' ' : '*');

  fprintf(fp, "%c %s\n", valid, path);
  fflush(fp);
}

void fsobj::ListCacheLong(FILE* fp)
{
  char path[MAXPATHLEN];
  GetPath(path, 0);		/* Get relative pathname. */    
  char valid = ((DATAVALID(this) && STATUSVALID(this) && !DIRTY(this)) ? ' ' : '*');
  char type = ( IsDir() ? 'd' : ( IsSymLink() ? 's' : 'f') );
  char linktype = ' ';
  if ( type != 'f' )
    linktype = (IsMtPt() ? 'm' :
		(IsMTLink() ? 'l' :
		 (IsRoot() ? '/' :
		  (IsVenusRoot() ? 'v': ' '))));

  fprintf(fp, "%c %c%c %s  %s\n", valid, type, linktype, path, FID_(&fid));	
  fflush(fp);
}


/* *****  Iterator  ***** */

fso_iterator::fso_iterator(LockLevel level, const VenusFid *key) : rec_ohashtab_iterator(FSDB->htab, key) {
    clevel = level;
    cvol = 0;
}

/* Returns entry locked as specified. */
fsobj *fso_iterator::operator()() {
    for (;;) {
	rec_olink *o = rec_ohashtab_iterator::operator()();
	if (!o) return(0);

	fsobj *f = strbase(fsobj, o, primary_handle);
	if (cvol == 0 || cvol == f->vol) {
	    if (clevel != NL) f->Lock(clevel);
	    return(f);
	}
    }
}

void fsobj::GetOperationState(int *conn, int *tid)
{
    if (HOARDING(this)) {
	*conn = 1;
	*tid = 0;
	return;
    }
    if (EMULATING(this)) {
	*conn = 0;
	*tid = -1;
	return;
    }

    OBJ_ASSERT(this, LOGGING(this));
    /* 
     * check to see if the object is within the global portion of a subtree
     * that is currently being repaired. (only when a repair session is on)
     */
    *tid = -1;
    int repair_mutation = 0;
    if (LRDB->repair_root_fid != NULL) {
	fsobj *cfo = this;
	while (cfo != NULL) {
	    if (cfo->IsLocalObj())
	      break;
	    if (cfo->IsRoot())
	      cfo = cfo->u.mtpoint->pfso;
	    else
	      cfo = cfo->pfso;
	}
	if (cfo && FID_EQ(&(cfo->pfid), LRDB->repair_root_fid) ||
            FID_EQ(&(cfo->pfid), LRDB->RFM_FakeRootToParent(LRDB->repair_root_fid)))
	    repair_mutation = 1;
    }
    if (repair_mutation) {
	*tid = LRDB->repair_session_tid;
	if (LRDB->repair_session_mode == REP_SCRATCH_MODE) {
	    *conn = 0;
	} else {
	    *conn = 1;
	}
    } else {
	*conn = 0;
    }
}
