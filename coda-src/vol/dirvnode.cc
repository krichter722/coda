#ifndef _BLURB_
#define _BLURB_
/*

            Coda: an Experimental Distributed File System
                             Release 4.0

          Copyright (c) 1987-1996 Carnegie Mellon University
                         All Rights Reserved

Permission  to  use, copy, modify and distribute this software and its
documentation is hereby granted,  provided  that  both  the  copyright
notice  and  this  permission  notice  appear  in  all  copies  of the
software, derivative works or  modified  versions,  and  any  portions
thereof, and that both notices appear in supporting documentation, and
that credit is given to Carnegie Mellon University  in  all  documents
and publicity pertaining to direct or indirect use of this code or its
derivatives.

CODA IS AN EXPERIMENTAL SOFTWARE SYSTEM AND IS  KNOWN  TO  HAVE  BUGS,
SOME  OF  WHICH MAY HAVE SERIOUS CONSEQUENCES.  CARNEGIE MELLON ALLOWS
FREE USE OF THIS SOFTWARE IN ITS "AS IS" CONDITION.   CARNEGIE  MELLON
DISCLAIMS  ANY  LIABILITY  OF  ANY  KIND  FOR  ANY  DAMAGES WHATSOEVER
RESULTING DIRECTLY OR INDIRECTLY FROM THE USE OF THIS SOFTWARE  OR  OF
ANY DERIVATIVE WORK.

Carnegie  Mellon  encourages  users  of  this  software  to return any
improvements or extensions that  they  make,  and  to  grant  Carnegie
Mellon the rights to redistribute these changes without encumbrance.
*/

#endif /*_BLURB_*/


#ifdef __cplusplus
extern "C" {
#endif __cplusplus

#include <sys/types.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include <lwp.h>
#include <lock.h>
#include <util.h>
#include <rvmlib.h>
#include <codadir.h>
#include <vice.h>
#ifdef __cplusplus
}
#endif __cplusplus


#include "cvnode.h"
#include "volume.h"

/* 
   This file manages the directory handle cache in conjunction 
   with Vnodes.

   The Directory Handle cache is controlled by the Vnode cache. 
   When directory content is needed, VN_SetDirHandle must be called. 
   When the Vnode goes away is calls DC_Drop to eliminate the cache.
   It could call DC_Put but presently the calls VN_SetDirHandle is not
   paired with a corresponding VN_PutDirHandle call. 

   The reference counting done by DC is therefore ignored:
   i.e. VN_SetDirHandle can be called at any time and need not te
   matched by equally many "Puts", when VN_PutDirHandle is called the
   cache entry will go away.

   This would be a good thing to fix, since it would potentially enhance 
   performance.

*/

/* copies DirHandle data  into pages in recoverable storage */
/* Called from within a transaction */
int VN_DCommit(Vnode *vnp)
{   
	PDirInode   pdi = (PDirInode) vnp->disk.inodeNumber;
	PDCEntry    pdce = vnp->dh;

	if (!vnp || (vnp->disk.type != vDirectory) || !pdce) {
		DLog(29, "VN_DCommit: Vnode or dh not allocated/not a directory");
		return 0;
	}

	if (vnp->delete_me) {
		/* directory was deleted */
		DLog(29, "VN_DCommit: deleted directory, vnode = %d",  
		     vnp->vnodeNumber);
		vnp->disk.inodeNumber = 0;
		/* if this vnode was just cloned, there won't be a pdi upon 
		   removal */
		if (pdi)
			DI_Dec(pdi);
	} else if (vnp->changed) {
		/* directory was modified - commit the pages */
		DLog(29, "VN_DCommit: Commiting pages for dir vnode = %d", 
			vnp->vnodeNumber);
		/* copy the VM pages into RVM */
		DI_DhToDi(pdce);
		/* CODA_ASSERT the directory inode now exists... */
		CODA_ASSERT(DC_DC2DI(pdce));
		/* rehash just in case it is new */
		DC_Rehash(pdce);
		vnp->disk.inodeNumber = (long unsigned int) DC_DC2DI(pdce);
	}
	return 0;
}


/* Eliminate the VM Data of the directory */
int VN_DAbort(Vnode *vnp) 
{
	Volume *volume;
    
	if (!vnp || (vnp->disk.type != vDirectory) || !vnp->dh){
		DLog(29, "DAbort: Vnode not allocated, not a directory or no handle");
		return(0);
	}
    
	DH_FreeData(DC_DC2DH(vnp->dh));

	return(0);
}

/* 
   this hashes the Directory Handle and copies inode pages into the DH. 
*/
PDirHandle VN_SetDirHandle(struct Vnode *vn)
{
	PDCEntry pdce = NULL;

	/* three cases:
	   - new not previously seen 
	   - not new, already in RVM
            - new, not yet in RVM, still on the new_list
	*/

	if ( vn->disk.inodeNumber == 0 && vn->dh == 0 ) {  
		pdce = DC_New();
		SLog(0, "VN_GetDirHandle NEW Vnode %#x Uniq %#x cnt %d\n",
		     vn->vnodeNumber, vn->disk.uniquifier, DC_Count(pdce));
		vn->dh = pdce;
		vn->dh_refc = 1;
	} else if ( vn->disk.inodeNumber ) {
		pdce = DC_Get((PDirInode)vn->disk.inodeNumber);
		SLog(0, "VN_GetDirHandle for Vnode %#x Uniq" 
		     " %#x cnt %d, vn_cnt %d\n",
		     vn->vnodeNumber, vn->disk.uniquifier, 
		     DC_Count(pdce), vn->dh_refc);
		vn->dh = pdce;
		vn->dh_refc++;
	} else {
		pdce = vn->dh;
		DC_SetCount(pdce, DC_Count(pdce) + 1);
		vn->dh_refc++;
		SLog(0, "VN_GetDirHandle NEW-seen Vnode %#x Uniq %#x" 
		     "cnt %d, vn_ct %d\n",
		     vn->vnodeNumber, vn->disk.uniquifier, 
		     DC_Count(pdce), vn->dh_refc);
	}

	CODA_ASSERT( (vn->dh_refc>0) && (DC_Count(pdce)>=vn->dh_refc) );
	return DC_DC2DH(pdce);
}

/*
  VN_PutDirHandle: the Vnode is going away, clear the DC entry
 */
void VN_PutDirHandle(struct Vnode *vn)
{

	CODA_ASSERT(vn->dh);

	if (vn->dh) {
		SLog(0, "VN_PutDirHandle: Vn %x Uniq %x: cnt %d, vn_cnt %d\n",
		     vn->vnodeNumber, vn->disk.uniquifier, 
		     DC_Count(vn->dh)-1, vn->dh_refc-1);
		DC_Put(vn->dh);
		vn->dh_refc--;
		CODA_ASSERT(DC_Count(vn->dh) >= 0);
		if ( vn->dh_refc == 0 )
			vn->dh = 0;
	}
}

/* Drop DirHandle */
void VN_DropDirHandle(struct Vnode *vn)
{
	if (vn->dh) {
		SLog(0, "VN_DropDirHandle for Vnode %x Unique %x: cnt %d, vn_cnt %d\n",
		     vn->vnodeNumber, vn->disk.uniquifier, DC_Count(vn->dh), vn->dh_refc);
		DC_Drop(vn->dh);
	}
	vn->dh_refc = 0;
	vn->dh = NULL;
}

/*
   - directories: set the disk.inode field to 0 and 
     create a dcentry with the _old_ contents. 
     NOTES
         - afterwards the vptr->dh  will have VM data, 
	 but no RVM data.
	 - the refcounts of the vnode and the cache entries
	 are split appropriately.

*/
void VN_CopyOnWrite(struct Vnode *vn)
{
	PDCEntry pdce;
	PDCEntry oldpdce;
	PDirHeader pdirh;
	PDirHandle pdh;
	int others_count;
	
	CODA_ASSERT(vn->disk.inodeNumber != 0);
	/* pin it */
	pdh = VN_SetDirHandle(vn);
	CODA_ASSERT(pdh);
	oldpdce = DC_DH2DC(pdh);
	others_count = DC_Count(oldpdce) - vn->dh_refc;

	/* no one else has a reference to this directory - 
	   merely prepare for cloning the RVM directory inode */
	if ( ! others_count ) {
		DC_SetCowpdi(oldpdce, (PDirInode)vn->disk.inodeNumber);
		DC_SetDirty(oldpdce, 1);
	}

	/* other vnodes have a reference to this directory -
	   prepare for cloning the RVM directory inode _and_
	   clone the VM directory cache entry now */
	if ( others_count ) {
		pdce= DC_New();
		CODA_ASSERT(pdce);

		/* get pointer to VM data */
		pdirh = (PDirHeader) malloc(DH_Length(pdh));
		CODA_ASSERT(pdirh);
		memcpy(pdirh, DH_Data(pdh), DH_Length(pdh));
		
		/* set up the copied directory */
		vn->dh = pdce;
		DC_SetDirh(pdce, pdirh);
		DC_SetCowpdi(pdce, (PDirInode)vn->disk.inodeNumber);
		DC_SetDirty(pdce, 1);
		DC_SetCount(pdce, vn->dh_refc);

		/* subtract our references to the oldpdce */
		DC_SetCount(oldpdce, others_count);
	}

	vn->disk.inodeNumber = 0;
	vn->disk.cloned = 0;
 	VN_PutDirHandle(vn);
	
	SLog(0, "VN_CopyOnWrite: New other_count: %d dh_refc %d", 
	     others_count, vn->dh_refc);

}
