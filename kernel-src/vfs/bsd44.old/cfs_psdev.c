/* 
 * Mach Operating System
 * Copyright (c) 1989 Carnegie-Mellon University
 * All rights reserved.  The CMU software License Agreement specifies
 * the terms and conditions for use and redistribution.
 */

/*
 * This code was written for the Coda file system at Carnegie Mellon
 * University.  Contributers include David Steere, James Kistler, and
 * M. Satyanarayanan.  */

/* ************************************************** */
/* These routines define the psuedo device for communication between
 * Coda's Venus and Minicache in Mach 2.6. They used to be in cfs_subr.c, 
 * but I moved them to make it easier to port the Minicache without 
 * porting coda. -- DCS 10/12/94
 */

/* 
 * Renamed to cfs_psdev: pseudo-device driver.
 */

/*
 * HISTORY
 * cfs_psdev.c,v
 * Revision 1.3  1996/11/13 04:14:20  bnoble
 * Merging BNOBLE_WORK_6_20_96 into main line
 *
 * Revision 1.2.8.1  1996/08/22 14:25:04  bnoble
 * Added a return code from vc_nb_close
 *
 * Revision 1.2  1996/01/02 16:56:58  bnoble
 * Added support for Coda MiniCache and raw inode calls (final commit)
 *
 * Revision 1.1.2.1  1995/12/20 01:57:24  bnoble
 * Added CFS-specific files
 *
 * Revision 1.1  1995/03/14  20:52:15  bnoble
 * Initial revision
 *
 */

/* These routines are the device entry points for Venus. */

extern int cfsnc_initialized;    /* Set if cache has been initialized */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <cfs/cfs.h>
#include <cfs/cnode.h>
#include <cfs/cfsio.h>

struct vnode *cfs_ctlvp = 0;

int cfs_psdev_print_entry = 0;

#ifdef __GNUC__
#define ENTRY    \
    if(cfs_psdev_print_entry) myprintf(("Entered %s\n",__FUNCTION__))
#else
#define ENTRY
#endif 

#ifdef __FreeBSD__
#ifdef DEVFS
#include <sys/devfsext.h>
#endif DEVFS
#include <sys/conf.h>

/* 
 * Using the reserved device number for local use as a free slot. 
 * (see /usr/src/sys/i386/i386/conf.c)
 */
#define VC_DEV_NO      20

/* Type of device methods. */
#define D_OPEN_T    d_open_t
#define D_CLOSE_T   d_close_t
#define D_RDWR_T    d_rdwr_t
#define D_READ_T    d_read_t
#define D_WRITE_T   d_write_t
#define D_IOCTL_T   d_ioctl_t
#define D_SELECT_T  d_select_t

/* D_OPEN_T    vc_nb_open;   is defined in cfs_FreeBSD.h */
D_CLOSE_T   vc_nb_close;
D_READ_T    vc_nb_read;
D_WRITE_T   vc_nb_write;
D_IOCTL_T   vc_nb_ioctl;
D_SELECT_T  vc_nb_select;
void        vcattach __P((void));

static struct cdevsw vccdevsw =
{ 
  vc_nb_open,      vc_nb_close,    vc_nb_read,        vc_nb_write,
  vc_nb_ioctl,     nostop,         nullreset,         nodevtotty,
  vc_nb_select,    nommap,         NULL,              "Coda", NULL, -1 };

static dev_t vccdev;

PSEUDO_SET(vcattach, vc);

int     vcdebug = 1;
#define VCDEBUG if (vcdebug) printf

void
vcattach(void)
{
  /*
   * In case we are an LKM, set up device switch.
   */
  ENTRY;
  if (0 == (vccdev = makedev(VC_DEV_NO, 0)))
    VCDEBUG("makedev returned null\n");
  else 
    VCDEBUG("makedev OK.\n");
    
  cdevsw_add(&vccdev, &vccdevsw, NULL);
  VCDEBUG("cfs: vccdevsw entry installed at %d.\n", major(vccdev));
}
#else  /* __FreeBSD__ */
#define D_OPEN_T    int
#define D_CLOSE_T   int
#define D_RDWR_T    int
#define D_IOCTL_T   int
#define D_SELECT_T  int
#endif

/* 
 * These functions are written for BSD44.  The Mach versions are just
 * wrappers that call these with the right number of arguments 
 */

int
vc_nb_open(dev, flag, mode, p)    
    dev_t        dev;      
    int          flag;     
    int          mode;     
    struct proc *p;             /* BSD44 only */
{
    register struct vcomm *vcp;
    struct ody_mntinfo *op;
    struct cnode       *cp;
    
    ENTRY;

    if (minor(dev) >= NVCFS || minor(dev) < 0)
	return(ENXIO);
    
    if (!cfsnc_initialized)
	cfsnc_init();
    
    if (cfs_ctlvp == 0) {
	ViceFid ctlfid;
	
	ctlfid.Volume = CTL_VOL;
	ctlfid.Vnode = CTL_VNO;
	ctlfid.Unique = CTL_UNI;
	
	
	cp = makecfsnode(&ctlfid, 0, VCHR);
	cfs_ctlvp = CTOV(cp);
    }
    
    vcp = &cfs_mnttbl[minor(dev)].mi_vcomm;
    if (VC_OPEN(vcp))
	return(EBUSY);
    
    /* Make first 4 bytes be zero */
    cfs_mnttbl[minor(dev)].mi_name = (char *)0;
    SELPROC_INIT(vcp->vc_selproc);
    INIT_QUEUE(vcp->vc_requests);
    INIT_QUEUE(vcp->vc_replys);
    MARK_VC_OPEN(vcp);
    
    cfs_mnttbl[minor(dev)].mi_vfschain.vfsp = NULL;
    cfs_mnttbl[minor(dev)].mi_vfschain.rootvp = NULL;
    cfs_mnttbl[minor(dev)].mi_vfschain.next = NULL;

    return(0);
}


int
vc_nb_close (dev, flag, mode, p)    
    dev_t        dev;      
    int          flag;     
    int          mode;     
    struct proc *p;
{
    register struct vcomm *	vcp;
    register struct vmsg *vmp;
    struct ody_mntinfo *op;
    int                 err;
	
    ENTRY;

    if (minor(dev) >= NVCFS || minor(dev) < 0)
	return(ENXIO);
    
    vcp = &cfs_mnttbl[minor(dev)].mi_vcomm;
    
    if (!VC_OPEN(vcp))
	panic("vcclose: not open");
    
    if (cfs_mnttbl[minor(dev)].mi_name) {
	CFS_FREE(cfs_mnttbl[minor(dev)].mi_name,
		 strlen(cfs_mnttbl[minor(dev)].mi_name));
	cfs_mnttbl[minor(dev)].mi_name = 0;
    }
    
    /* prevent future operations on this vfs from succeeding by auto-
     * unmounting any vfs mounted via this device. This frees user or
     * sysadm from having to remember where all mount points are located.
     * Put this before WAKEUPs to avoid queuing new messages between
     * the WAKEUP and the unmount (which can happen if we're unlucky)
     */
    for (op = &cfs_mnttbl[minor(dev)].mi_vfschain; op ; op = op->next) {
	if (op->rootvp) {
	    /* Let unmount know this is for real */
	    VTOC(op->rootvp)->c_flags |= C_DYING;	
	    err = DOUNMOUNT(op->vfsp);
	    if (err)
		myprintf(("Error %d unmounting vfs in vcclose(%d)\n", 
			  err, minor(dev)));
	} else {
	    /* Should only be null if no mount has happened. */
	    if (op != &cfs_mnttbl[minor(dev)].mi_vfschain) 
		myprintf(("Help! assertion failed in vcwrite\n"));
	    
	}
    }
    
    /* Wakeup clients so they can return. */
    for (vmp = (struct vmsg *)GETNEXT(vcp->vc_requests);
	 !EOQ(vmp, vcp->vc_requests);
	 vmp = (struct vmsg *)GETNEXT(vmp->vm_chain))
    {	    
	/* Free signal request messages and don't wakeup cause
	   no one is waiting. */
	if (vmp->vm_opcode == CFS_SIGNAL) {
	    CFS_FREE((caddr_t)vmp->vm_data, (u_int)VC_IN_NO_DATA);
	    CFS_FREE((caddr_t)vmp, (u_int)sizeof(struct vmsg));
	    continue;
	}
	
	WAKEUP(&vmp->vm_sleep);
    }
    
    for (vmp = (struct vmsg *)GETNEXT(vcp->vc_replys);
	 !EOQ(vmp, vcp->vc_replys);
	 vmp = (struct vmsg *)GETNEXT(vmp->vm_chain))
    {
	WAKEUP(&vmp->vm_sleep);
    }
    
    MARK_VC_CLOSED(vcp);
    return 0;
}


int 
vc_nb_read(dev, uiop, flag)   
    dev_t        dev;  
    struct uio  *uiop; 
    int          flag;
{
    register struct vcomm *	vcp;
    register struct vmsg *vmp;
    int error = 0;
    
    ENTRY;

    if (minor(dev) >= NVCFS || minor(dev) < 0)
	return(ENXIO);
    
    vcp = &cfs_mnttbl[minor(dev)].mi_vcomm;
    /* Get message at head of request queue. */
    if (EMPTY(vcp->vc_requests))
	return(0);	/* Nothing to read */
    
    vmp = (struct vmsg *)GETNEXT(vcp->vc_requests);
    
    /* Move the input args into userspace */
    UIOMOVE(vmp->vm_data, vmp->vm_inSize, UIO_READ, uiop, error);
    if (error) {
	myprintf(("vcread: error (%d) on uiomove\n", error));
	error = EINVAL;
    }

#ifdef DIAGNOSTIC    
    if (vmp->vm_chain.forw == 0 || vmp->vm_chain.back == 0)
	panic("vc_nb_read: bad chain");
#endif

    REMQUE(vmp->vm_chain);
    
    /* If request was a signal, free up the message and don't
       enqueue it in the reply queue. */
    if (vmp->vm_opcode == CFS_SIGNAL) {
	if (cfsdebug)
	    myprintf(("vcread: signal msg (%d, %d)\n", 
		      vmp->vm_opcode, vmp->vm_unique));
	CFS_FREE((caddr_t)vmp->vm_data, (u_int)VC_IN_NO_DATA);
	CFS_FREE((caddr_t)vmp, (u_int)sizeof(struct vmsg));
	return(error);
    }
    
    vmp->vm_flags |= VM_READ;
    INSQUE(vmp->vm_chain, vcp->vc_replys);
    
    return(error);
}

int
vc_nb_write(dev, uiop, flag)   
    dev_t        dev;  
    struct uio  *uiop; 
    int          flag;
{
    register struct vcomm *	vcp;
    register struct vmsg *vmp;
    struct outputArgs *out;
    u_long seq;
    u_long opcode;
    int buf[2];
    int error = 0;

    ENTRY;

    if (minor(dev) >= NVCFS || minor(dev) < 0)
	return(ENXIO);
    
    vcp = &cfs_mnttbl[minor(dev)].mi_vcomm;
    
    /* Peek at the opcode, unique without transfering the data. */
    UIOMOVE((caddr_t)buf, sizeof(int) * 2, UIO_WRITE, uiop, error);
    if (error) {
	myprintf(("vcwrite: error (%d) on uiomove\n", error));
	return(EINVAL);
    }
    
    opcode = buf[0];
    seq = buf[1];
	
    if (cfsdebug)
	myprintf(("vcwrite got a call for %d.%d\n", opcode, seq));
    
    if (DOWNCALL(opcode)) {
	struct outputArgs pbuf;
	
	/* get the rest of the data. */
	UIOMOVE((caddr_t)&pbuf.result, sizeof(pbuf) - (sizeof(int)*2), 
		UIO_WRITE, uiop, error);
	if (error) {
	    myprintf(("vcwrite: error (%d) on uiomove (Op %d seq %d)\n", 
		      error, opcode, seq));
	    return(EINVAL);
	    }
	
	return handleDownCall(opcode, &pbuf);
    }
    
    /* Look for the message on the (waiting for) reply queue. */
    for (vmp = (struct vmsg *)GETNEXT(vcp->vc_replys);
	 !EOQ(vmp, vcp->vc_replys);
	 vmp = (struct vmsg *)GETNEXT(vmp->vm_chain))
    {
	if (vmp->vm_unique == seq) break;
    }
    
    if (EOQ(vmp, vcp->vc_replys)) {
	if (cfsdebug)
	    myprintf(("vcwrite: msg (%d, %d) not found\n", opcode, seq));
	
	return(ESRCH);
	}
    
    /* Remove the message from the reply queue */
    REMQUE(vmp->vm_chain);
    
    /* move data into response buffer. */
    out = (struct outputArgs *)vmp->vm_data;
    /* Don't need to copy opcode and uniquifier. */
    
    /* get the rest of the data. */
    if (vmp->vm_outSize < uiop->uio_resid) {
	myprintf(("vcwrite: more data than asked for (%d < %d)\n",
		  vmp->vm_outSize, uiop->uio_resid));
	WAKEUP(&vmp->vm_sleep); 	/* Notify caller of the error. */
	return(EINVAL);
    } 
    
    buf[0] = uiop->uio_resid; 	/* Save this value. */
    UIOMOVE((caddr_t) &out->result, vmp->vm_outSize - (sizeof(int) * 2), 
	    UIO_WRITE, uiop, error);
    if (error) {
	myprintf(("vcwrite: error (%d) on uiomove (op %d seq %d)\n", 
		  error, opcode, seq));
	return(EINVAL);
    }
    
    /* I don't think these are used, but just in case. */
    /* XXX - aren't these two already correct? -bnoble */
    out->opcode = opcode;
    out->unique = seq;
    vmp->vm_outSize	= buf[0];	/* Amount of data transferred? */
    vmp->vm_flags |= VM_WRITE;
    WAKEUP(&vmp->vm_sleep);
    
    return(0);
}

int
vc_nb_ioctl(dev, cmd, addr, flag, p) 
    dev_t         dev;       
    int           cmd;       
    caddr_t       addr;      
    int           flag;      
    struct proc  *p;
{
    ENTRY;

    switch(cmd) {
    case CFSRESIZE: {
	struct cfs_resize *data = (struct cfs_resize *)addr;
	return(cfsnc_resize(data->hashsize, data->heapsize));
	break;
    }
    case CFSSTATS:
	if (cfsnc_use) {
	    cfsnc_gather_stats();
	    return(0);
	} else {
	    return(ENODEV);
	}
	break;
    case CFSPRINT:
	if (cfsnc_use) {
	    print_cfsnc();
	    return(0);
	} else {
	    return(ENODEV);
	}
	break;
    case ODYBIND:
	/* Bind a name to our device. Used to allow more than one kind of FS */
	if (cfs_mnttbl[minor(dev)].mi_name) {
	    if (cfsdebug)
		myprintf(("ODYBIND: dev %d already has name %s\n", minor(dev),
			  cfs_mnttbl[minor(dev)].mi_name));
	    return(EBUSY);	/* Some name already used. */
	} else {
	    struct ody_bind *data = (struct ody_bind *)addr;
	    char *name;
	    
	    CFS_ALLOC(name, char *, (data->size));
	    copyin(data->name, name, data->size);
	    
	    if (cfsdebug)
		myprintf(("ODYBIND: binding %s to dev %d\n", 
			  name, minor(dev)));
	    
	    cfs_mnttbl[minor(dev)].mi_name = name;
	    return(0);
	}
	break;
    default :
	return(EINVAL);
	break;
    }
}

int
vc_nb_select(dev, flag, p)         
    dev_t         dev;    
    int           flag;   
    struct proc  *p;
{
    register struct vcomm *	vcp;
    
    ENTRY;
    
    if (minor(dev) >= NVCFS || minor(dev) < 0)
	return(ENXIO);
    
    vcp = &cfs_mnttbl[minor(dev)].mi_vcomm;
    
    if (flag != FREAD)
	return(0);
    
    if (!EMPTY(vcp->vc_requests))
	return(1);
    
    SELRECORD(vcp->vc_selproc);
    
    return(0);
}

#ifdef __FreeBSD__

#include <sys/queue.h>
#include <sys/mount.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/ufsmount.h>

struct mount *
devtomp (dev_t dev)
{
  struct mount *mp;

  for (mp = mountlist.cqh_first; mp != NULL; mp = mp->mnt_list.cqe_next)
    if (VT_UFS == (mp->mnt_vnodelist.lh_first)->v_tag) {
      if (dev == VFSTOUFS(mp)->um_dev) return mp;
    }
  return (struct mount*)0;
}

#endif /* __FreeBSD__ */
