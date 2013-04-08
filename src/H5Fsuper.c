/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * Copyright by the Board of Trustees of the University of Illinois.         *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of HDF5.  The full HDF5 copyright notice, including     *
 * terms governing use, modification, and redistribution, is contained in    *
 * the files COPYING and Copyright.html.  COPYING can be found at the root   *
 * of the source code distribution tree; Copyright.html can be found at the  *
 * root level of an installed copy of the electronic HDF5 document set and   *
 * is linked from the top-level documents page.  It can also be found at     *
 * http://hdfgroup.org/HDF5/doc/Copyright.html.  If you do not have          *
 * access to either file, you may request a copy from help@hdfgroup.org.     *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/****************/
/* Module Setup */
/****************/

#define H5F_PACKAGE		/*suppress error about including H5Fpkg	  */

/* Interface initialization */
#define H5_INTERFACE_INIT_FUNC	H5F_init_super_interface


/***********/
/* Headers */
/***********/
#include "H5private.h"		/* Generic Functions                    */
#include "H5ACprivate.h"        /* Metadata cache                       */
#include "H5Eprivate.h"		/* Error handling                       */
#include "H5Fpkg.h"             /* File access                          */
#include "H5FDprivate.h"	/* File drivers                         */
#include "H5Iprivate.h"		/* IDs                                  */
#include "H5MFprivate.h"        /* File memory management               */
#include "H5MMprivate.h"	/* Memory management			*/
#include "H5Pprivate.h"		/* Property lists                       */
#include "H5SMprivate.h"        /* Shared Object Header Messages        */


/****************/
/* Local Macros */
/****************/


/******************/
/* Local Typedefs */
/******************/


/********************/
/* Package Typedefs */
/********************/


/********************/
/* Local Prototypes */
/********************/
static herr_t H5F_super_ext_create(H5F_t *f, hid_t dxpl_id, H5O_loc_t *ext_ptr);


/*********************/
/* Package Variables */
/*********************/


/*****************************/
/* Library Private Variables */
/*****************************/

/* Declare a free list to manage the H5F_super_t struct */
H5FL_DEFINE(H5F_super_t);


/*******************/
/* Local Variables */
/*******************/



/*--------------------------------------------------------------------------
NAME
   H5F_init_super_interface -- Initialize interface-specific information
USAGE
    herr_t H5F_init_super_interface()

RETURNS
    Non-negative on success/Negative on failure
DESCRIPTION
    Initializes any interface-specific data or routines.  (Just calls
    H5F_init() currently).

--------------------------------------------------------------------------*/
static herr_t
H5F_init_super_interface(void)
{
    FUNC_ENTER_NOAPI_NOINIT_NOERR

    FUNC_LEAVE_NOAPI(H5F_init())
} /* H5F_init_super_interface() */


/*-------------------------------------------------------------------------
 * Function:	H5F_locate_signature
 *
 * Purpose:	Finds the HDF5 superblock signature in a file.	The signature
 *		can appear at address 0, or any power of two beginning with
 *		512.
 *
 * Return:	Success:	The absolute format address of the signature.
 *
 *		Failure:	HADDR_UNDEF
 *
 * Programmer:	Robb Matzke
 *		Friday, November  7, 1997
 *
 *-------------------------------------------------------------------------
 */
haddr_t
H5F_locate_signature(H5FD_t *file, hid_t dxpl_id)
{
    haddr_t	    addr, eoa;
    uint8_t	    buf[H5F_SIGNATURE_LEN];
    unsigned	    n, maxpow;
    haddr_t         ret_value;       /* Return value */

    FUNC_ENTER_NOAPI_NOINIT

    /* Find the least N such that 2^N is larger than the file size */
    if(HADDR_UNDEF == (addr = H5FD_get_eof(file)) || HADDR_UNDEF == (eoa = H5FD_get_eoa(file, H5FD_MEM_SUPER)))
	HGOTO_ERROR(H5E_IO, H5E_CANTINIT, HADDR_UNDEF, "unable to obtain EOF/EOA value")
    for(maxpow = 0; addr; maxpow++)
        addr >>= 1;
    maxpow = MAX(maxpow, 9);

    /*
     * Search for the file signature at format address zero followed by
     * powers of two larger than 9.
     */
    for(n = 8; n < maxpow; n++) {
	addr = (8 == n) ? 0 : (haddr_t)1 << n;
	if(H5FD_set_eoa(file, H5FD_MEM_SUPER, addr + H5F_SIGNATURE_LEN) < 0)
	    HGOTO_ERROR(H5E_IO, H5E_CANTINIT, HADDR_UNDEF, "unable to set EOA value for file signature")
	if(H5FD_read(file, dxpl_id, H5FD_MEM_SUPER, addr, (size_t)H5F_SIGNATURE_LEN, buf) < 0)
	    HGOTO_ERROR(H5E_IO, H5E_CANTINIT, HADDR_UNDEF, "unable to read file signature")
	if(!HDmemcmp(buf, H5F_SIGNATURE, (size_t)H5F_SIGNATURE_LEN))
            break;
    } /* end for */

    /*
     * If the signature was not found then reset the EOA value and return
     * failure.
     */
    if(n >= maxpow) {
	(void)H5FD_set_eoa(file, H5FD_MEM_SUPER, eoa); /* Ignore return value */
	HGOTO_ERROR(H5E_IO, H5E_CANTINIT, HADDR_UNDEF, "unable to find a valid file signature")
    } /* end if */

    /* Set return value */
    ret_value = addr;

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5F_locate_signature() */


/*-------------------------------------------------------------------------
 * Function:    H5F_super_ext_create
 *
 * Purpose:     Create the superblock extension
 *
 * Return:      Success:        non-negative on success
 *              Failure:        Negative
 *
 * Programmer:  Vailin Choi; Feb 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5F_super_ext_create(H5F_t *f, hid_t dxpl_id, H5O_loc_t *ext_ptr)
{
    herr_t ret_value = SUCCEED;         /* Return value */

    FUNC_ENTER_NOAPI_NOINIT

    /* Sanity check */
    HDassert(f);
    HDassert(f->shared);
    HDassert(f->shared->sblock);
    HDassert(!H5F_addr_defined(f->shared->sblock->ext_addr));
    HDassert(ext_ptr);

    /* Check for older version of superblock format that can't support superblock extensions */
    if(f->shared->sblock->super_vers < HDF5_SUPERBLOCK_VERSION_2)
        HGOTO_ERROR(H5E_FILE, H5E_CANTCREATE, FAIL, "superblock extension not permitted with version %u of superblock", f->shared->sblock->super_vers)
    else if(H5F_addr_defined(f->shared->sblock->ext_addr))
        HGOTO_ERROR(H5E_FILE, H5E_CANTCREATE, FAIL, "superblock extension already exists?!?!")
    else {
        /* The superblock extension isn't actually a group, but the
         * default group creation list should work fine.
         * If we don't supply a size for the object header, HDF5 will
         * allocate H5O_MIN_SIZE by default.  This is currently
         * big enough to hold the biggest possible extension, but should
         * be tuned if more information is added to the superblock
         * extension.
         */
        H5O_loc_reset(ext_ptr);
        if(H5O_create(f, dxpl_id, 0, (size_t)1, H5P_GROUP_CREATE_DEFAULT, ext_ptr) < 0)
            HGOTO_ERROR(H5E_OHDR, H5E_CANTCREATE, FAIL, "unable to create superblock extension")

        /* Record the address of the superblock extension */
        f->shared->sblock->ext_addr = ext_ptr->addr;
    } /* end else */

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5F_super_ext_create() */


/*-------------------------------------------------------------------------
 * Function:    H5F_super_ext_open
 *
 * Purpose:     Open an existing superblock extension
 *
 * Return:      Success:        non-negative on success
 *              Failure:        Negative
 *
 * Programmer:  Vailin Choi; Feb 2009
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5F_super_ext_open(H5F_t *f, haddr_t ext_addr, H5O_loc_t *ext_ptr)
{
    herr_t ret_value = SUCCEED;         /* Return value */

    FUNC_ENTER_NOAPI_NOINIT

    /* Sanity check */
    HDassert(f);
    HDassert(H5F_addr_defined(ext_addr));
    HDassert(ext_ptr);

    /* Set up "fake" object location for superblock extension */
    H5O_loc_reset(ext_ptr);
    ext_ptr->file = f;
    ext_ptr->addr = ext_addr;

    /* Open the superblock extension object header */
    if(H5O_open(ext_ptr) < 0)
	HGOTO_ERROR(H5E_OHDR, H5E_CANTOPENOBJ, FAIL, "unable to open superblock extension")

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5F_super_ext_open() */


/*-------------------------------------------------------------------------
 * Function:   H5F_super_ext_close
 *
 * Purpose:    Close superblock extension
 *
 * Return:     Success:        non-negative on success
 *             Failure:        Negative
 *
 * Programmer:  Vailin Choi; Feb 2009
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5F_super_ext_close(H5F_t *f, H5O_loc_t *ext_ptr, hid_t dxpl_id,
    hbool_t was_created)
{
    herr_t ret_value = SUCCEED;         /* Return value */

    FUNC_ENTER_NOAPI_NOINIT

    /* Sanity check */
    HDassert(f);
    HDassert(ext_ptr);

    /* Check if extension was created */
    if(was_created) {
        /* Increment link count on superblock extension's object header */
        if(H5O_link(ext_ptr, 1, dxpl_id) < 0)
            HGOTO_ERROR(H5E_FILE, H5E_LINKCOUNT, FAIL, "unable to increment hard link count")

        /* Decrement refcount on superblock extension's object header in memory */
        if(H5O_dec_rc_by_loc(ext_ptr, dxpl_id) < 0)
           HDONE_ERROR(H5E_FILE, H5E_CANTDEC, FAIL, "unable to decrement refcount on superblock extension")
    } /* end if */

    /* Twiddle the number of open objects to avoid closing the file. */
    f->nopen_objs++;
    if(H5O_close(ext_ptr) < 0)
        HGOTO_ERROR(H5E_FILE, H5E_CANTCLOSEOBJ, FAIL, "unable to close superblock extension")
    f->nopen_objs--;

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5F_super_ext_close() */


/*-------------------------------------------------------------------------
 * Function:    H5F_super_read
 *
 * Purpose:     Reads the superblock from the file or from the BUF. If
 *              ADDR is a valid address, then it reads it from the file.
 *              If not, then BUF must be non-NULL for it to read from the
 *              BUF.
 *
 * Return:      Success:        SUCCEED
 *              Failure:        FAIL
 *
 * Programmer:  Bill Wendling
 *              wendling@ncsa.uiuc.edu
 *              Sept 12, 2003
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5F_super_read(H5F_t *f, hid_t dxpl_id)
{
    H5F_super_t *       sblock = NULL;      /* superblock structure                         */
    unsigned            sblock_flags = H5AC__NO_FLAGS_SET;       /* flags used in superblock unprotect call      */
    haddr_t             super_addr;         /* Absolute address of superblock */
    H5AC_protect_t      rw;                 /* read/write permissions for file              */
    hbool_t             dirtied = FALSE;    /* Bool for sblock protect call                 */
    herr_t              ret_value = SUCCEED; /* return value                                */

    FUNC_ENTER_NOAPI_TAG(dxpl_id, H5AC__SUPERBLOCK_TAG, FAIL)

    /* Find the superblock */
    if(HADDR_UNDEF == (super_addr = H5F_locate_signature(f->shared->lf, dxpl_id)))
        HGOTO_ERROR(H5E_FILE, H5E_NOTHDF5, FAIL, "unable to find file signature")

    /* Check for userblock present */
    if(H5F_addr_gt(super_addr, 0)) {
        /* Set the base address for the file in the VFD now */
        if(H5FD_set_base_addr(f->shared->lf, super_addr) < 0)
            HGOTO_ERROR(H5E_FILE, H5E_CANTSET, FAIL, "failed to set base address for file driver")
    } /* end if */

    /* Determine file intent for superblock protect */
    if(H5F_INTENT(f) & H5F_ACC_RDWR)
        rw = H5AC_WRITE;
    else
        rw = H5AC_READ;

    /* Look up the superblock */
    if(NULL == (sblock = (H5F_super_t *)H5AC_protect(f, dxpl_id, H5AC_SUPERBLOCK, (haddr_t)0, &dirtied, rw)))
        HGOTO_ERROR(H5E_CACHE, H5E_CANTPROTECT, FAIL, "unable to load superblock")

    /* Mark the superblock dirty if it was modified during loading or VFD indicated to do so */
    if((H5AC_WRITE == rw) && (dirtied || H5F_HAS_FEATURE(f, H5FD_FEAT_DIRTY_SBLK_LOAD)))
        sblock_flags |= H5AC__DIRTIED_FLAG;

    /* Pin the superblock in the cache */
    if(H5AC_pin_protected_entry(sblock) < 0)
        HGOTO_ERROR(H5E_FSPACE, H5E_CANTPIN, FAIL, "unable to pin superblock")

    /* Set the pointer to the pinned superblock */
    f->shared->sblock = sblock;

done:
    /* Release the superblock */
    if(sblock && H5AC_unprotect(f, dxpl_id, H5AC_SUPERBLOCK, (haddr_t)0, sblock, sblock_flags) < 0)
        HDONE_ERROR(H5E_CACHE, H5E_CANTUNPROTECT, FAIL, "unable to close superblock")

    FUNC_LEAVE_NOAPI_TAG(ret_value, FAIL)
} /* end H5F_super_read() */


/*-------------------------------------------------------------------------
 * Function:    H5F_super_init
 *
 * Purpose:     Allocates the superblock for the file and initializes
 *              information about the superblock in memory.  Writes extension
 *              messages if any are needed.
 *
 * Return:      Success:        SUCCEED
 *              Failure:        FAIL
 *
 * Programmer:  Quincey Koziol
 *              koziol@ncsa.uiuc.edu
 *              Sept 15, 2003
 *
 * Modifications:
 *	Vailin Choi; Dec 2012
 *	Changes due to "page" file space management in support of level 2 page caching
 *
 *	Vailin Choi; Feb 2013
 * 	Create the "fsinfo" message with "mark if unknown" flag.
 *	Changes to handle file space info for paged aggregation.
 *-------------------------------------------------------------------------
 */
herr_t
H5F_super_init(H5F_t *f, hid_t dxpl_id)
{
    H5F_super_t    *sblock = NULL;      /* Superblock cache structure                 */
    hbool_t         sblock_in_cache = FALSE;    /* Whether the superblock has been inserted into the metadata cache */
    H5P_genplist_t *plist;              /* File creation property list                */
    hsize_t         userblock_size;     /* Size of userblock, in bytes                */
    hsize_t         superblock_size;    /* Size of superblock, in bytes               */
    size_t          driver_size;        /* Size of driver info block (bytes)          */
    unsigned super_vers = HDF5_SUPERBLOCK_VERSION_DEF; /* Superblock version for file */
    H5O_loc_t       ext_loc;            /* Superblock extension object location */
    hbool_t         need_ext;           /* Whether the superblock extension is needed */
    hbool_t         ext_created = FALSE; /* Whether the extension has been created */
    H5F_fspace_strategy_t	saved_fs_strategy = f->shared->fs_strategy;	/* The file space handling strategy */
    hbool_t	    saved_fs_persist = f->shared->fs_persist;			/* The file space "persist" status */
    herr_t          ret_value = SUCCEED; /* Return Value                              */

    FUNC_ENTER_NOAPI_TAG(dxpl_id, H5AC__SUPERBLOCK_TAG, FAIL)

    /* Allocate space for the superblock */
    if(NULL == (sblock = H5FL_CALLOC(H5F_super_t)))
        HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, FAIL, "memory allocation failed")

    /* Initialize various address information */
    sblock->base_addr = HADDR_UNDEF;
    sblock->ext_addr = HADDR_UNDEF;
    sblock->driver_addr = HADDR_UNDEF;
    sblock->root_addr = HADDR_UNDEF;

    /* Get the shared file creation property list */
    if(NULL == (plist = (H5P_genplist_t *)H5I_object(f->shared->fcpl_id)))
        HGOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "not a property list")

    /* Initialize sym_leaf_k */
    if(H5P_get(plist, H5F_CRT_SYM_LEAF_NAME, &sblock->sym_leaf_k) < 0)
        HGOTO_ERROR(H5E_PLIST, H5E_CANTGET, FAIL, "can't get byte number for object size")

    /* Initialize btree_k */
    if(H5P_get(plist, H5F_CRT_BTREE_RANK_NAME, &sblock->btree_k[0]) < 0)
        HGOTO_ERROR(H5E_PLIST, H5E_CANTGET, FAIL, "unable to get rank for btree internal nodes")

    if(f->shared->fs_strategy == (H5F_fspace_strategy_t)(-1))	/* A default strategy set by user */
	f->shared->fs_strategy = H5F_FILE_SPACE_STRATEGY_DEF;
    else if(f->shared->latest_format && f->shared->fs_strategy == H5F_FILE_SPACE_STRATEGY_DEF) /* A library default strategy */
	f->shared->fs_strategy = H5F_FSPACE_STRATEGY_PAGE;

    /* The file space strategy is changed */
    if(f->shared->fs_strategy != saved_fs_strategy) {
        H5P_genplist_t *c_plist;              /* Property list */

        if(NULL == (c_plist = (H5P_genplist_t *)H5I_object(f->shared->fcpl_id)))
            HGOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "not property list")
        if(H5P_set(c_plist, H5F_CRT_FILE_SPACE_STRATEGY_NAME, &f->shared->fs_strategy) < 0)
            HGOTO_ERROR(H5E_PLIST, H5E_CANTSET, FAIL, "unable to set file space strategy")
    }

    if(f->shared->fs_persist == (hbool_t)(-1))	/* A default "persist" set by user */
	f->shared->fs_persist = H5F_FREE_SPACE_PERSIST_DEF;
    else if(f->shared->latest_format && f->shared->fs_persist == H5F_FREE_SPACE_PERSIST_DEF) /* A library default "persist" */
	f->shared->fs_persist = TRUE;

    /* The file space "persist" status is changed */
    if(f->shared->fs_persist != saved_fs_persist) {
        H5P_genplist_t *c_plist;              /* Property list */

        if(NULL == (c_plist = (H5P_genplist_t *)H5I_object(f->shared->fcpl_id)))
            HGOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "not property list")
        if(H5P_set(c_plist, H5F_CRT_FREE_SPACE_PERSIST_NAME, &f->shared->fs_persist) < 0)
            HGOTO_ERROR(H5E_PLIST, H5E_CANTSET, FAIL, "unable to set free space persist status")
    }

    /* 
     *	When using the latest version of the format:
     *	-- Bump superblock version
     */
    if(f->shared->latest_format) {
        super_vers = HDF5_SUPERBLOCK_VERSION_LATEST;
    /* Bump superblock version to create superblock extension for SOHM info */
    } else if(f->shared->sohm_nindexes > 0)
        super_vers = HDF5_SUPERBLOCK_VERSION_2;
    /* 
     *	Bump superblock version to create superblock extension for:
     * 	-- non-default file space strategy or 
     * 	-- non-default persisting free-space or 
     *  -- non-default free-space threshold or
     *  -- non-default fsp_size
     */
    else if(!(f->shared->fs_strategy == H5F_FILE_SPACE_STRATEGY_DEF &&
            f->shared->fs_persist == H5F_FREE_SPACE_PERSIST_DEF &&
            f->shared->fs_threshold == H5F_FREE_SPACE_THRESHOLD_DEF &&
	    f->shared->fsp_size == H5F_FILE_SPACE_PAGE_SIZE_DEF))
        super_vers = HDF5_SUPERBLOCK_VERSION_2;
    /* Check for non-default indexed storage B-tree internal 'K' value
     * and set the version # of the superblock to 1 if it is a non-default
     * value.
     */
    else if(sblock->btree_k[H5B_CHUNK_ID] != HDF5_BTREE_CHUNK_IK_DEF)
        super_vers = HDF5_SUPERBLOCK_VERSION_1;

    /* If a newer superblock version is required, set it here */
    if(super_vers != HDF5_SUPERBLOCK_VERSION_DEF) {
        H5P_genplist_t *c_plist;              /* Property list */

        if(NULL == (c_plist = (H5P_genplist_t *)H5I_object(f->shared->fcpl_id)))
            HGOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "not property list")
        if(H5P_set(c_plist, H5F_CRT_SUPER_VERS_NAME, &super_vers) < 0)
            HGOTO_ERROR(H5E_PLIST, H5E_CANTSET, FAIL, "unable to set superblock version")
    } /* end if */

    if(H5FD_set_paged_aggr(f->shared->lf, (hbool_t)H5F_PAGED_AGGR(f)) < 0)
	HGOTO_ERROR(H5E_FILE, H5E_CANTINIT, FAIL, "failed to set paged_aggr status for file driver")

    /*
     * The superblock starts immediately after the user-defined
     * header, which we have already insured is a proper size. The
     * base address is set to the same thing as the superblock for
     * now.
     */
    if(H5P_get(plist, H5F_CRT_USER_BLOCK_NAME, &userblock_size) < 0)
        HGOTO_ERROR(H5E_FILE, H5E_CANTGET, FAIL, "unable to get userblock size")

    /* Sanity check the userblock size vs. the file's allocation alignment */
    if(userblock_size > 0) {
	/* Set up the alignment to use for page or aggr fs */
	hsize_t alignment = H5F_PAGED_AGGR(f) ? f->shared->fsp_size : f->shared->alignment;

        if(userblock_size < alignment)
            HGOTO_ERROR(H5E_FILE, H5E_BADVALUE, FAIL, "userblock size must be > file object alignment")
        if(0 != (userblock_size % alignment))
            HGOTO_ERROR(H5E_FILE, H5E_BADVALUE, FAIL, "userblock size must be an integral multiple of file object alignment")
    } /* end if */

    sblock->base_addr = userblock_size;
    sblock->status_flags = 0;

    /* Reserve space for the userblock */
    if(H5FD_set_eoa(f->shared->lf, H5FD_MEM_SUPER, userblock_size) < 0)
        HGOTO_ERROR(H5E_FILE, H5E_CANTINIT, FAIL, "unable to set EOA value for userblock")
   
    /* Set the base address for the file in the VFD now, after allocating
     *  space for userblock.
     */
    if(H5FD_set_base_addr(f->shared->lf, sblock->base_addr) < 0)
	HGOTO_ERROR(H5E_FILE, H5E_CANTINIT, FAIL, "failed to set base address for file driver")

    /* Save a local copy of the superblock version number */
    sblock->super_vers = super_vers;

    /* Compute the size of the superblock */
    superblock_size = (hsize_t)H5F_SUPERBLOCK_SIZE(super_vers, f);

    /* Compute the size of the driver information block */
    H5_ASSIGN_OVERFLOW(driver_size, H5FD_sb_size(f->shared->lf), hsize_t, size_t);
    if(driver_size > 0) {
        driver_size += H5F_DRVINFOBLOCK_HDR_SIZE;

        /*
         * The file driver information block begins immediately after the
         * superblock. (relative to base address in file)
         */
        sblock->driver_addr = superblock_size;
    } /* end if */

    /*
     * Allocate space for the userblock, superblock & driver info blocks.
     * We do it with one allocation request because the userblock and
     * superblock need to be at the beginning of the file and only the first
     * allocation request is required to return memory at format address zero.
     */
    if(super_vers < HDF5_SUPERBLOCK_VERSION_2)
        superblock_size += driver_size;

    /* Insert superblock into cache, pinned */
    if(H5AC_insert_entry(f, dxpl_id, H5AC_SUPERBLOCK, (haddr_t)0, sblock, H5AC__PIN_ENTRY_FLAG | H5AC__FLUSH_LAST_FLAG | H5AC__FLUSH_COLLECTIVELY_FLAG) < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTINS, FAIL, "can't add superblock to cache")
    sblock_in_cache = TRUE;

    /* Keep a copy of the superblock info */
    f->shared->sblock = sblock;

    /* Allocate space for the superblock */
    if(HADDR_UNDEF == H5MF_alloc(f, H5FD_MEM_SUPER, dxpl_id, superblock_size))
	HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, FAIL, "file allocation failed for superblock")

    /*
     * Determine if we will need a superblock extension
     */

    /* Files with SOHM indices always need the superblock extension */
    if(f->shared->sohm_nindexes > 0) {
        HDassert(super_vers >= HDF5_SUPERBLOCK_VERSION_2);
        need_ext = TRUE;
    } /* end if */
    /* Files with non-default free space settings always need the superblock extension */
    else if(!(f->shared->fs_strategy == H5F_FILE_SPACE_STRATEGY_DEF &&
            f->shared->fs_persist == H5F_FREE_SPACE_PERSIST_DEF &&
            f->shared->fs_threshold == H5F_FREE_SPACE_THRESHOLD_DEF &&
	    f->shared->fsp_size == H5F_FILE_SPACE_PAGE_SIZE_DEF)) {
        HDassert(super_vers >= HDF5_SUPERBLOCK_VERSION_2);
        need_ext = TRUE;
    }
    /* If we're going to use a version of the superblock format which allows
     *  for the superblock extension, check for non-default values to store
     *  in it.
     */
    else if(super_vers >= HDF5_SUPERBLOCK_VERSION_2) {
        /* Check for non-default v1 B-tree 'K' values to store */
        if(sblock->btree_k[H5B_SNODE_ID] != HDF5_BTREE_SNODE_IK_DEF ||
                sblock->btree_k[H5B_CHUNK_ID] != HDF5_BTREE_CHUNK_IK_DEF ||
                sblock->sym_leaf_k != H5F_CRT_SYM_LEAF_DEF)
            need_ext = TRUE;
        /* Check for driver info to store */
        else if(driver_size > 0)
            need_ext = TRUE;
        else
            need_ext = FALSE;
    } /* end if */
    else
        need_ext = FALSE;

    /* Create the superblock extension for "extra" superblock data, if necessary. */
    if(need_ext) {
        /* The superblock extension isn't actually a group, but the
         * default group creation list should work fine.
         * If we don't supply a size for the object header, HDF5 will
         * allocate H5O_MIN_SIZE by default.  This is currently
         * big enough to hold the biggest possible extension, but should
         * be tuned if more information is added to the superblock
         * extension.
         */
	if(H5F_super_ext_create(f, dxpl_id, &ext_loc) < 0)
            HGOTO_ERROR(H5E_FILE, H5E_CANTCREATE, FAIL, "unable to create superblock extension")
        ext_created = TRUE;

        /* Create the Shared Object Header Message table and register it with
         *      the metadata cache, if this file supports shared messages.
         */
        if(f->shared->sohm_nindexes > 0) {
            /* Initialize the shared message code & write the SOHM message to the extension */
            if(H5SM_init(f, plist, &ext_loc, dxpl_id) < 0)
                HGOTO_ERROR(H5E_FILE, H5E_CANTINIT, FAIL, "unable to create SOHM table")
        } /* end if */

        /* Check for non-default v1 B-tree 'K' values to store */
        if(sblock->btree_k[H5B_SNODE_ID] != HDF5_BTREE_SNODE_IK_DEF ||
                sblock->btree_k[H5B_CHUNK_ID] != HDF5_BTREE_CHUNK_IK_DEF ||
                sblock->sym_leaf_k != H5F_CRT_SYM_LEAF_DEF) {
            H5O_btreek_t btreek;        /* v1 B-tree 'K' value message for superblock extension */

            /* Write v1 B-tree 'K' value information to the superblock extension */
            btreek.btree_k[H5B_CHUNK_ID] = sblock->btree_k[H5B_CHUNK_ID];
            btreek.btree_k[H5B_SNODE_ID] = sblock->btree_k[H5B_SNODE_ID];
            btreek.sym_leaf_k = sblock->sym_leaf_k;
            if(H5O_msg_create(&ext_loc, H5O_BTREEK_ID, H5O_MSG_FLAG_CONSTANT | H5O_MSG_FLAG_DONTSHARE, H5O_UPDATE_TIME, &btreek, dxpl_id) < 0)
                HGOTO_ERROR(H5E_FILE, H5E_CANTINIT, FAIL, "unable to update v1 B-tree 'K' value header message")
        } /* end if */

        /* Check for driver info to store */
        if(driver_size > 0) {
            H5O_drvinfo_t drvinfo;      /* Driver info */
            uint8_t dbuf[H5F_MAX_DRVINFOBLOCK_SIZE];  /* Driver info block encoding buffer */

            /* Sanity check */
            HDassert(driver_size <= H5F_MAX_DRVINFOBLOCK_SIZE);

            /* Encode driver-specific data */
            if(H5FD_sb_encode(f->shared->lf, drvinfo.name, dbuf) < 0)
                HGOTO_ERROR(H5E_FILE, H5E_CANTINIT, FAIL, "unable to encode driver information")

            /* Write driver info information to the superblock extension */
            drvinfo.len = driver_size;
            drvinfo.buf = dbuf;
            if(H5O_msg_create(&ext_loc, H5O_DRVINFO_ID, H5O_MSG_FLAG_DONTSHARE, H5O_UPDATE_TIME, &drvinfo, dxpl_id) < 0)
                HGOTO_ERROR(H5E_FILE, H5E_CANTINIT, FAIL, "unable to update driver info header message")
        } /* end if */

        /* Check for non-default free-space info settings */
	if(!(f->shared->fs_strategy == H5F_FILE_SPACE_STRATEGY_DEF &&
            f->shared->fs_persist == H5F_FREE_SPACE_PERSIST_DEF &&
            f->shared->fs_threshold == H5F_FREE_SPACE_THRESHOLD_DEF &&
	    f->shared->fsp_size == H5F_FILE_SPACE_PAGE_SIZE_DEF)) {

            H5O_fsinfo_t fsinfo;	/* Free space manager info message */
	    H5FD_mem_t   type; 		/* Free-space types for iteration */

	    /* Write free-space manager info message to superblock extension object header if needed */
	    fsinfo.version = H5O_FSINFO_VERSION_0;
	    fsinfo.strategy = f->shared->fs_strategy;
	    fsinfo.persist = f->shared->fs_persist;
	    fsinfo.threshold = f->shared->fs_threshold;
	    fsinfo.fsp_size = f->shared->fsp_size;
	    fsinfo.last_small = f->shared->last_small;
	    fsinfo.pgend_meta_thres = f->shared->pgend_meta_thres;

	    for(type = H5FD_MEM_SUPER; type < H5FD_MEM_NTYPES; H5_INC_ENUM(H5FD_mem_t, type))
		fsinfo.fs_addr[type-1] = HADDR_UNDEF;

            if(H5O_msg_create(&ext_loc, H5O_FSINFO_ID, H5O_MSG_FLAG_DONTSHARE|H5O_MSG_FLAG_MARK_IF_UNKNOWN, H5O_UPDATE_TIME, &fsinfo, dxpl_id) < 0)
                HGOTO_ERROR(H5E_FILE, H5E_CANTINIT, FAIL, "unable to update free-space info header message")
	} /* end if */
    } /* end if */

done:
    /* Close superblock extension, if it was created */
    if(ext_created && H5F_super_ext_close(f, &ext_loc, dxpl_id, ext_created) < 0)
        HDONE_ERROR(H5E_FILE, H5E_CANTRELEASE, FAIL, "unable to close file's superblock extension")

    /* Cleanup on failure */
    if(ret_value < 0) {
        /* Check if the superblock has been allocated yet */
        if(sblock) {
            /* Check if we've cached it already */
            if(sblock_in_cache) {
                /* Unpin superblock in cache */
                if(H5AC_unpin_entry(sblock) < 0)
                    HDONE_ERROR(H5E_FILE, H5E_CANTUNPIN, FAIL, "unable to unpin superblock")

                /* Evict the superblock from the cache */
                if(H5AC_expunge_entry(f, dxpl_id, H5AC_SUPERBLOCK, (haddr_t)0, H5AC__NO_FLAGS_SET) < 0)
                    HDONE_ERROR(H5E_FILE, H5E_CANTEXPUNGE, FAIL, "unable to expunge superblock")
            } /* end if */
            else
                /* Free superblock */
                if(H5F_super_free(sblock) < 0)
                    HDONE_ERROR(H5E_FILE, H5E_CANTFREE, FAIL, "unable to destroy superblock")

            /* Reset variables in file structure */
            f->shared->sblock = NULL;
        } /* end if */
    } /* end if */

    FUNC_LEAVE_NOAPI_TAG(ret_value, FAIL)
} /* end H5F_super_init() */


/*-------------------------------------------------------------------------
 * Function:    H5F_super_dirty
 *
 * Purpose:     Mark the file's superblock dirty
 *
 * Return:      Success:        non-negative on success
 *              Failure:        Negative
 *
 * Programmer:  Quincey Koziol
 *              August 14, 2009
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5F_super_dirty(H5F_t *f)
{
    herr_t ret_value = SUCCEED;         /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* Sanity check */
    HDassert(f);
    HDassert(f->shared);
    HDassert(f->shared->sblock);

    /* Mark superblock dirty in cache, so change to EOA will get encoded */
    if(H5AC_mark_entry_dirty(f->shared->sblock) < 0)
        HGOTO_ERROR(H5E_FILE, H5E_CANTMARKDIRTY, FAIL, "unable to mark superblock as dirty")

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5F_super_dirty() */


/*-------------------------------------------------------------------------
 * Function:    H5F_super_free
 *
 * Purpose:     Destroyer the file's superblock
 *
 * Return:      Success:        non-negative on success
 *              Failure:        Negative
 *
 * Programmer:  Quincey Koziol
 *              April 1, 2010
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5F_super_free(H5F_super_t *sblock)
{
    FUNC_ENTER_NOAPI_NOINIT_NOERR

    /* Sanity check */
    HDassert(sblock);

    /* Free root group symbol table entry, if any */
    sblock->root_ent = (H5G_entry_t *)H5MM_xfree(sblock->root_ent);

    /* Free superblock */
    sblock = (H5F_super_t *)H5FL_FREE(H5F_super_t, sblock);

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* H5F_super_free() */


/*-------------------------------------------------------------------------
 * Function:    H5F_super_size
 *
 * Purpose:     Get storage size of the superblock and superblock extension
 *
 * Return:      Success:        non-negative on success
 *              Failure:        Negative
 *
 * Programmer:  Vailin Choi
 *              July 11, 2007
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5F_super_size(H5F_t *f, hid_t dxpl_id, hsize_t *super_size, hsize_t *super_ext_size)
{
    herr_t ret_value = SUCCEED;         /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* Sanity check */
    HDassert(f);
    HDassert(f->shared);
    HDassert(f->shared->sblock);

    /* Set the superblock size */
    if(super_size)
	*super_size = (hsize_t)H5F_SUPERBLOCK_SIZE(f->shared->sblock->super_vers, f);

    /* Set the superblock extension size */
    if(super_ext_size) {
        if(H5F_addr_defined(f->shared->sblock->ext_addr)) {
            H5O_loc_t ext_loc;          /* "Object location" for superblock extension */
            H5O_hdr_info_t hdr_info;    /* Object info for superblock extension */

            /* Set up "fake" object location for superblock extension */
            H5O_loc_reset(&ext_loc);
            ext_loc.file = f;
            ext_loc.addr = f->shared->sblock->ext_addr;

            /* Get object header info for superblock extension */
            if(H5O_get_hdr_info(&ext_loc, dxpl_id, &hdr_info) < 0)
                HGOTO_ERROR(H5E_FILE, H5E_CANTGET, FAIL, "unable to retrieve superblock extension info")

            /* Set the superblock extension size */
            *super_ext_size = hdr_info.space.total;
        } /* end if */
        else
            /* Set the superblock extension size to zero */
            *super_ext_size = (hsize_t)0;
    } /* end if */

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5F_super_size() */


/*-------------------------------------------------------------------------
 * Function:    H5F_super_ext_write_msg()
 *
 * Purpose:     Write the message with ID to the superblock extension
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Vailin Choi; Feb 2009
 *
 * Modifications:
 *	Vailin Choi; Feb 2013
 *	Create the message with the specified "mesg_flags".
 *-------------------------------------------------------------------------
 */
herr_t
H5F_super_ext_write_msg(H5F_t *f, hid_t dxpl_id, void *mesg, unsigned id, unsigned mesg_flags, hbool_t may_create)
{
    hbool_t     ext_created = FALSE;   /* Whether superblock extension was created */
    hbool_t     ext_opened = FALSE;    /* Whether superblock extension was opened */
    H5O_loc_t 	ext_loc; 	/* "Object location" for superblock extension */
    htri_t 	status;       	/* Indicate whether the message exists or not */
    herr_t 	ret_value = SUCCEED;         /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* Sanity checks */
    HDassert(f);
    HDassert(f->shared);
    HDassert(f->shared->sblock);

    /* Open/create the superblock extension object header */
    if(H5F_addr_defined(f->shared->sblock->ext_addr)) {
	if(H5F_super_ext_open(f, f->shared->sblock->ext_addr, &ext_loc) < 0)
	    HGOTO_ERROR(H5E_FILE, H5E_CANTOPENOBJ, FAIL, "unable to open file's superblock extension")
    } /* end if */
    else {
        HDassert(may_create);
	if(H5F_super_ext_create(f, dxpl_id, &ext_loc) < 0)
	    HGOTO_ERROR(H5E_FILE, H5E_CANTCREATE, FAIL, "unable to create file's superblock extension")
        ext_created = TRUE;
    } /* end else */
    HDassert(H5F_addr_defined(ext_loc.addr));
    ext_opened = TRUE;

    /* Check if message with ID does not exist in the object header */
    if((status = H5O_msg_exists(&ext_loc, id, dxpl_id)) < 0)
	HGOTO_ERROR(H5E_OHDR, H5E_CANTGET, FAIL, "unable to check object header for message or message exists")

    /* Check for creating vs. writing */
    if(may_create) {
	if(status)
	    HGOTO_ERROR(H5E_OHDR, H5E_CANTGET, FAIL, "Message should not exist")

	/* Create the message with ID in the superblock extension */
	if(H5O_msg_create(&ext_loc, id, mesg_flags, H5O_UPDATE_TIME, mesg, dxpl_id) < 0)
	    HGOTO_ERROR(H5E_OHDR, H5E_CANTGET, FAIL, "unable to create the message in object header")
    } /* end if */
    else {
	if(!status)
	    HGOTO_ERROR(H5E_OHDR, H5E_CANTGET, FAIL, "Message should exist")

	/* Update the message with ID in the superblock extension */
	if(H5O_msg_write(&ext_loc, id, mesg_flags, H5O_UPDATE_TIME, mesg, dxpl_id) < 0)
	    HGOTO_ERROR(H5E_OHDR, H5E_CANTGET, FAIL, "unable to write the message in object header")
    } /* end else */

done:
    /* Close the superblock extension, if it was opened */
    if(ext_opened && H5F_super_ext_close(f, &ext_loc, dxpl_id, ext_created) < 0)
        HDONE_ERROR(H5E_FILE, H5E_CANTRELEASE, FAIL, "unable to close file's superblock extension")

    /* Mark superblock dirty in cache, if superblock extension was created */
    if(ext_created && H5AC_mark_entry_dirty(f->shared->sblock) < 0)
        HDONE_ERROR(H5E_FILE, H5E_CANTMARKDIRTY, FAIL, "unable to mark superblock as dirty")

    FUNC_LEAVE_NOAPI(ret_value)
} /* H5F_super_ext_write_msg() */


/*-------------------------------------------------------------------------
 * Function:    H5F_super_ext_remove_msg
 *
 * Purpose:     Remove the message with ID from the superblock extension
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Vailin Choi; Feb 2009
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5F_super_ext_remove_msg(H5F_t *f, hid_t dxpl_id, unsigned id)
{
    H5O_loc_t 	ext_loc; 	/* "Object location" for superblock extension */
    hbool_t     ext_opened = FALSE;     /* Whether the superblock extension was opened */
    int		null_count = 0;	/* # of null messages */
    htri_t      status;         /* Indicate whether the message exists or not */
    herr_t 	ret_value = SUCCEED;         /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* Make sure that the superblock extension object header exists */
    HDassert(H5F_addr_defined(f->shared->sblock->ext_addr));

    /* Open superblock extension object header */
    if(H5F_super_ext_open(f, f->shared->sblock->ext_addr, &ext_loc) < 0)
	HGOTO_ERROR(H5E_FILE, H5E_CANTRELEASE, FAIL, "error in starting file's superblock extension")
    ext_opened = TRUE;

    /* Check if message with ID exists in the object header */
    if((status = H5O_msg_exists(&ext_loc, id, dxpl_id)) < 0)
	HGOTO_ERROR(H5E_OHDR, H5E_CANTGET, FAIL, "unable to check object header for message")
    else if(status) { /* message exists */
	H5O_hdr_info_t 	hdr_info;       /* Object header info for superblock extension */

	/* Remove the message */
	if(H5O_msg_remove(&ext_loc, id, H5O_ALL, TRUE, dxpl_id) < 0)
	    HGOTO_ERROR(H5E_OHDR, H5E_CANTDELETE, FAIL, "unable to delete free-space manager info message")

	/* Get info for the superblock extension's object header */
	if(H5O_get_hdr_info(&ext_loc, dxpl_id, &hdr_info) < 0)
	    HGOTO_ERROR(H5E_OHDR, H5E_CANTGET, FAIL, "unable to retrieve superblock extension info")

	/* If the object header is an empty base chunk, remove superblock extension */
	if(hdr_info.nchunks == 1) {
	    if((null_count = H5O_msg_count(&ext_loc, H5O_NULL_ID, dxpl_id)) < 0)
		HGOTO_ERROR(H5E_SYM, H5E_CANTCOUNT, FAIL, "unable to count messages")
	    else if((unsigned)null_count == hdr_info.nmesgs) {
		HDassert(H5F_addr_defined(ext_loc.addr));
		if(H5O_delete(f, dxpl_id, ext_loc.addr) < 0)
		    HGOTO_ERROR(H5E_SYM, H5E_CANTCOUNT, FAIL, "unable to count messages")
		f->shared->sblock->ext_addr = HADDR_UNDEF;
	    } /* end if */
	} /* end if */
    } /* end if */

done:
    /* Close superblock extension object header, if opened */
    if(ext_opened && H5F_super_ext_close(f, &ext_loc, dxpl_id, FALSE) < 0)
       HDONE_ERROR(H5E_FILE, H5E_CANTRELEASE, FAIL, "unable to close file's superblock extension")

    FUNC_LEAVE_NOAPI(ret_value)
} /* H5F_super_ext_remove_msg() */
