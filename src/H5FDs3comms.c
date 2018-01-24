/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * Copyright by the Board of Trustees of the University of Illinois.         *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of HDF5.  The full HDF5 copyright notice, including     *
 * terms governing use, modification, and redistribution, is contained in    *
 * the COPYING file, which can be found at the root of the source code       *
 * distribution tree, or in https://support.hdfgroup.org/ftp/HDF5/releases.  *
 * If you do not have access to either file, you may request a copy from     *
 * help@hdfgroup.org.                                                        *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */



/*****************************************************************************
 * Source for S3 Communications module
 *
 * ***NOT A FILE DRIVER***
 *
 * Provide functions and structures required for interfacing with Amazon
 * Simple Storage Service (S3). 
 *
 * Provide S3 object access as if it were a local file.
 *
 * Connect to remote host, send and receive HTTP requests and responses
 * as part of the AWS REST API, authenticating requests as appropriate.
 *
 * Programmer: Jacob Smith
 *             2017-11-30
 *
 *****************************************************************************/



/****************/
/* Module Setup */
/****************/

/***********/
/* Headers */
/***********/

#include "H5private.h"   /* generic functions */
#include "H5Eprivate.h"  /* error handling    */
#include "H5MMprivate.h" /* memory management */
#include "H5FDs3comms.h" /* S3 Communications */

/****************/
/* Local Macros */
/****************/

/* toggle function debugging:
 * iff 1 (1L), prints function names as they are called
 */
#define S3COMMS_DEBUG 0

/* maniuplate verbosity of CURL output
 * 0 -> no explicit curl output
 * 1 -> on error, print failure info to stderr
 * 2 -> in addition to above, print information for all performs
 *      sets all curl handles with CURLOPT_VERBOSE
 */
#define S3COMMS_CURL_VERBOSITY 0

/* size to allocate for "bytes=<first_byte>[-<last_byte>]" HTTP Range value
 */
#define MAX_RANGE_BYTES_STR_LEN 128

/******************/
/* Local Typedefs */
/******************/

/********************/
/* Local Structures */
/********************/

/* struct s3r_datastruct
 * Structure passed to curl write callback
 * pointer to data region and record of bytes written (offset)
 */
struct s3r_datastruct {
    char   *data;
    size_t  size;
};

/********************/
/* Local Prototypes */
/********************/

size_t curlwritecallback(char   *ptr,
                         size_t  size,
                         size_t  nmemb,
                         void   *userdata);

/*********************/
/* Package Variables */
/*********************/

/*****************************/
/* Library Private Variables */
/*****************************/

/*******************/
/* Local Variables */
/*******************/

/*************/
/* Functions */
/*************/


/*----------------------------------------------------------------------------
 *
 * Function: curlwritecallback()
 *
 * Purpose:
 *
 *     Function called by CURL to write received data.
 *
 *     Writes bytes to `userdata`.
 *
 *     Internally manages number of bytes processed.
 *
 * Return:
 *
 *     - Number of bytes processed.
 *         - Should equal number of bytes passed to callback.
 *         - Failure will result in curl error: CURLE_WRITE_ERROR.
 *
 * Programmer: Jacob Smith
 *             2017-08-17
 *
 * Changes: None.
 *
 *----------------------------------------------------------------------------
 */
size_t
curlwritecallback(char   *ptr,
                  size_t  size,
                  size_t  nmemb,
                  void   *userdata)
{
    struct s3r_datastruct *sds     = (struct s3r_datastruct *)userdata;
    size_t                 product = (size * nmemb);
    size_t                 written = 0;

    if (size > 0) {
        HDmemcpy(&(sds->data[sds->size]), ptr, product);
        sds->size += product;
        written = product;
    }

    return written;

} /* curlwritecallback */


/*----------------------------------------------------------------------------
 *
 * Function: H5FD_s3comms_hrb_node_set()
 *
 * Purpose:
 *
 *     Create, insert, modify, and remove elements in a field node list.
 *
 *     `name` cannot be null; will return FAIL and list will be unaltered.
 *
 *     Entries are accessed via the lowercase representation of their name:
 *     "Host", "host", and "hOSt" would all access the same node,
 *     but name's case is relevant in HTTP request output.
 *
 *     List pointer `L` must always point to either of :
 *     - header node with lowest alphabetical order (by lowername)
 *     - NULL, if list is empty
 *
 *    Types of operations:
 *
 *    - CREATE
 *        - If `L` is NULL and `name` and `value` are not NULL,
 *          a new node is created at `L`, starting a list.
 *    - MODIFY
 *        - If a node is found with a matching lowercase name and `value`
 *          is not NULL, the existing name, value, and cat values are released
 *          and replaced with the new data.
 *        - No modifications are made to the list pointers.
 *    - REMOVE
 *        - If `value` is NULL, will attempt to remove node with matching 
 *          lowercase name.
 *        - If no match found, returns FAIL and list is not modified.
 *        - When removing a node, all its resources is released.
 *        - If removing the last node in the list, list pointer is set to NULL.
 *    - INSERT
 *        - If no nodes exists with matching lowercase name and `value`
 *          is not NULL, a new node is created, inserted into list
 *          alphabetically by lowercase name.
 *
 * Return:
 *
 *     - SUCCESS: `SUCCEED`
 *         - List was successfully modified
 *     - FAILURE: `FAIL`
 *         - Unable to perform operation
 *             - Foridden (attempting to remove absent node, e.g.)
 *             - Internal error
 *
 * Programmer: Jacob Smith
 *             2017-09-22
 *
 * Changes: 
 *
 *     - Change return value to herr_t
 *     - Change list pointer to pointer-to-pointer-to-node
 *     - Change to use singly-linked list (from twin doubly-linked lists)
 *       with modification to hrb_node_t
 *     --- Jake Smith 2017-01-17
 *
 *----------------------------------------------------------------------------
 */
herr_t
H5FD_s3comms_hrb_node_set(hrb_node_t **L,
                          const char  *name,
                          const char  *value)
{
    size_t      i          = 0;
    hbool_t     is_looking = TRUE;
    char       *nvcat      = NULL;
    char       *lowername  = NULL;
    char       *namecpy    = NULL;
    size_t      namelen    = 0;
    hrb_node_t *new_node   = NULL;
    hrb_node_t *ptr        = NULL;
    char       *valuecpy   = NULL;
    herr_t      ret_value  = SUCCEED;



    FUNC_ENTER_NOAPI_NOINIT

#if S3COMMS_DEBUG
    HDfprintf(stdout, "called H5FD_s3comms_hrb_node_set.\n");
#endif

    if (name == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "unable to operate on null name.\n");
    }
    namelen = strlen(name);

    /***********************
     * PREPARE ALL STRINGS *
     **********************/

    /* if value supplied, copy name, value, and concatenated "name: value"
     * if NULL, we will be removing (if anything)
     */
    if (value != NULL) {
        size_t valuelen   = strlen(value);
        size_t catlen     = namelen + valuelen + 2; /* strlen(": ") -> +2 */
        int    sprint_ret = 0;

        namecpy = (char *)H5MM_malloc(sizeof(char) * (namelen + 1));
        if (namecpy == NULL) {
            HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, FAIL, 
                        "cannot make space for name copy.\n");
        }
        HDmemcpy(namecpy, name, namelen + 1);

        valuecpy = (char *)H5MM_malloc(sizeof(char) * (valuelen + 1));
        if (valuecpy == NULL) {
            HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, FAIL, 
                        "cannot make space for value copy.\n");
        }
        HDmemcpy(valuecpy, value, valuelen + 1);

        nvcat = (char *)H5MM_malloc(sizeof(char) * (catlen + 1));
        if (nvcat == NULL) {
            HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, FAIL, 
                        "cannot make space for concatenated string.\n");
        }
        sprint_ret = snprintf(nvcat, (catlen + 1), "%s: %s", name, value);
        HDassert( sprint_ret > 0 );
        HDassert( catlen == (size_t)sprint_ret );
    }

    /* copy and lowercase name
     */
    lowername = (char *)H5MM_malloc(sizeof(char) * (namelen + 1));
    if (lowername == NULL) {
        HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, FAIL, 
                    "cannot make space for lowercase name copy.\n");
    }
    for (i = 0; i < namelen; i++) {
        lowername[i] = (char)tolower((int)name[i]);
    }
    lowername[namelen] = 0;

    /* create new_node, should we need it 
     */
    new_node = (hrb_node_t *)H5MM_malloc(sizeof(hrb_node_t));
    if (new_node == NULL) {
        HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, FAIL, 
                    "cannot make space for new set.\n");
    }

    new_node->magic     = S3COMMS_HRB_NODE_MAGIC;
    new_node->name      = NULL;
    new_node->value     = NULL;
    new_node->cat       = NULL;
    new_node->lowername = NULL;
    new_node->next      = NULL;

    /************************************
     * INSERT, MODIFY, OR REMOVE A NODE *
     ************************************/

    if (*L == NULL)  {
        if (value == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "trying to remove node from empty list");
        } else {
            /*******************
             * CREATE NEW LIST *
             *******************/

            new_node->cat       = nvcat;
            new_node->name      = namecpy;
            new_node->lowername = lowername;
            new_node->value     = valuecpy;
    
            *L = new_node;
            goto done; /* bypass further seeking */
        }
    }

    HDassert( (*L) != NULL );
    HDassert( (*L)->magic == S3COMMS_HRB_NODE_MAGIC );
    ptr = (*L);

    /* Check whether to modify/remove first node in list
     */
    if (strcmp(lowername, ptr->lowername) == 0) {

        if (value == NULL) {
            /***************
             * REMOVE HEAD *
             ***************/

            *L = ptr->next;

            H5MM_xfree(ptr->cat);
            H5MM_xfree(ptr->lowername);
            H5MM_xfree(ptr->name);
            H5MM_xfree(ptr->value);
            HDassert( ptr->magic == S3COMMS_HRB_NODE_MAGIC );
            ptr->magic = (unsigned long)(~S3COMMS_HRB_NODE_MAGIC);
            H5MM_xfree(ptr);

            H5MM_xfree(lowername); lowername = NULL;
            H5MM_xfree(namecpy);   namecpy   = NULL;
            H5MM_xfree(new_node);  new_node  = NULL;
            H5MM_xfree(nvcat);     nvcat     = NULL;
            H5MM_xfree(valuecpy);  valuecpy  = NULL;
        } else {
            /***************
             * MODIFY HEAD *
             ***************/

            H5MM_xfree(ptr->cat);
            H5MM_xfree(ptr->name);
            H5MM_xfree(ptr->value);

            ptr->name = namecpy;
            ptr->value = valuecpy;
            ptr->cat = nvcat;

            H5MM_xfree(lowername); lowername = NULL;
            H5MM_xfree(new_node);  new_node  = NULL;
        }
        is_looking = FALSE;
    } else if (strcmp(lowername, ptr->lowername) < 0) {

        if (value == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "trying to remove a node 'before' head");
        } else {
            /*******************
             * INSERT NEW HEAD *
             *******************/

            new_node->name      = namecpy;
            new_node->value     = valuecpy;
            new_node->lowername = lowername;
            new_node->cat       = nvcat;
            new_node->next      = ptr;
            *L = new_node;

            is_looking = FALSE;
        }
    }

    while (is_looking) {
        if (ptr->next == NULL) {

            if (value == NULL) {
                HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                            "trying to remove absent node");
            } else {
                /*******************
                 * APPEND NEW NODE *
                 *******************/

                HDassert( strcmp(lowername, ptr->lowername) > 0 );
                new_node->name      = namecpy;
                new_node->value     = valuecpy;
                new_node->lowername = lowername;
                new_node->cat       = nvcat;
                ptr->next = new_node;
            }
            is_looking = FALSE;
        } else if (strcmp(lowername, ptr->next->lowername) < 0) {
 
            if (value == NULL) {
                HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                            "trying to remove absent node");
            } else {
                /*******************
                 * INSERT NEW NODE *
                 *******************/

                HDassert( strcmp(lowername, ptr->lowername) > 0 );
                new_node->name      = namecpy;
                new_node->value     = valuecpy;
                new_node->lowername = lowername;
                new_node->cat       = nvcat;
                new_node->next      = ptr->next;
                ptr->next = new_node;
            }
            is_looking = FALSE;
        } else if (strcmp(lowername, ptr->next->lowername) == 0) {

            if (value == NULL) {
                /*****************
                 * REMOVE A NODE *
                 *****************/

                hrb_node_t *tmp = ptr->next;
                ptr->next = tmp->next;

                H5MM_xfree(tmp->cat);
                H5MM_xfree(tmp->lowername);
                H5MM_xfree(tmp->name);
                H5MM_xfree(tmp->value);
                HDassert( tmp->magic == S3COMMS_HRB_NODE_MAGIC );
                tmp->magic = (unsigned long)(~S3COMMS_HRB_NODE_MAGIC);
                H5MM_xfree(tmp);

                H5MM_xfree(lowername); lowername = NULL;
                H5MM_xfree(namecpy);   namecpy   = NULL;
                H5MM_xfree(new_node);  new_node  = NULL;
                H5MM_xfree(nvcat);     nvcat     = NULL;
                H5MM_xfree(valuecpy);  valuecpy  = NULL;
            } else {
                /*****************
                 * MODIFY A NODE *
                 *****************/

                ptr = ptr->next;
                H5MM_xfree(ptr->name);
                H5MM_xfree(ptr->value);
                H5MM_xfree(ptr->cat);

                HDassert( new_node->magic == S3COMMS_HRB_NODE_MAGIC );
                new_node->magic = (unsigned long)(~S3COMMS_HRB_NODE_MAGIC);
                H5MM_xfree(new_node);  new_node  = NULL;
                H5MM_xfree(lowername); lowername = NULL;

                ptr->name = namecpy;
                ptr->value = valuecpy;
                ptr->cat = nvcat;
            }
            is_looking = FALSE;
        } else {
            /****************
             * KEEP LOOKING *
             ****************/

             ptr = ptr->next;
        }
    }

done:
    if (ret_value == FAIL) {
        /* clean up 
         */
        if (nvcat     != NULL) H5MM_xfree(nvcat);
        if (namecpy   != NULL) H5MM_xfree(namecpy);
        if (lowername != NULL) H5MM_xfree(lowername);
        if (valuecpy  != NULL) H5MM_xfree(valuecpy);
        if (new_node  != NULL) {
            HDassert( new_node->magic == S3COMMS_HRB_NODE_MAGIC );
            new_node->magic = (unsigned long)(~S3COMMS_HRB_NODE_MAGIC);
            H5MM_xfree(new_node);
        }
    }

    FUNC_LEAVE_NOAPI(ret_value);

} /* H5FD_s3comms_hrb_node_set */ 



/*----------------------------------------------------------------------------
 *
 * Function: H5FD_s3comms_hrb_destroy()
 *
 * Purpose:
 *
 *    Destroy and free resources _directly_ associated with an HTTP Buffer. 
 *
 *    Takes a pointer to pointer to the buffer structure.
 *    This allows for the pointer itself to be NULLed from within the call.
 *
 *    If buffer or buffer pointer is NULL, there is no effect.
 *
 *    Headers list at `first_header` is not touched.
 *
 *    - Programmer should re-use or destroy `first_header` 
 *      (hrb_node_t *) pointer as suits their purposes.
 *    - Recommend fetching prior to destroy()
 *      e.g., `reuse_node = hrb_to_die->first_header; destroy(hrb_to_die);`
 *      or maintaining an external reference.
 *    - Destroy node/list as appropriate separately
 *        - `H5FD_s3comms_hrb_node_destroy(&node);
 *    - Failure to account for this will result in a memory leak.
 *
 * Return: 
 *
 *     - SUCCESS: `SUCCEED`
 *         - if `buf` is NULL or `*buf` is NULL, no effect
 *     - FAILURE: `FAIL`
 *         - `buf->magic != S3COMMS_HRB_MAGIC`
 *
 * Programmer: Jacob Smith
 *             2017-07-21
 *
 * Changes:
 *
 *     - Conditional free() of `hrb_node_t` pointer properties based on
 *       `which_free` property.
 *     --- Jacob Smith 2017-08-08
 *
 *     - Integrate with HDF5.
 *     - Returns herr_t instead of nothing.
 *     --- Jacob Smith 2017-09-21
 *
 *     - Change argument to from *buf to **buf, to null pointer within call
 *     --- Jacob Smith 2017-20-05
 *
 *----------------------------------------------------------------------------
 */
herr_t
H5FD_s3comms_hrb_destroy(hrb_t **_buf)
{
    hrb_t  *buf       = NULL;
    herr_t  ret_value = SUCCEED;



    FUNC_ENTER_NOAPI_NOINIT

#if S3COMMS_DEBUG
    HDfprintf(stdout, "called H5FD_s3comms_hrb_destroy.\n");
#endif

    if (_buf != NULL && *_buf != NULL) {
        buf = *_buf;
        HDassert( buf->magic == S3COMMS_HRB_MAGIC );
        if (buf->magic != S3COMMS_HRB_MAGIC) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "pointer's magic does not match.\n");
        }

        H5MM_xfree(buf->verb);
        H5MM_xfree(buf->version);
        H5MM_xfree(buf->resource);
        buf->magic = (unsigned long)(~S3COMMS_HRB_MAGIC);
        H5MM_xfree(buf);
        *_buf = NULL;
    }

done:
    FUNC_LEAVE_NOAPI(ret_value)

} /* H5FD_s3comms_hrb_destroy */


/*----------------------------------------------------------------------------
 *
 * Function: H5FD_s3comms_hrb_init_request()
 *
 * Purpose:
 *
 *     Create a new HTTP Request Buffer, to build an HTTP request,
 *     element by element (header-by-header, plus optional body).
 *
 *     All non-null arguments should be null-terminated strings.
 *
 *     If `verb` is NULL, defaults to "GET".
 *     If `http_version` is NULL, defaults to "HTTP/1.1".
 *
 *     `resource` cannot be NULL; should be string beginning with slash
 *     character ('/').
 *
 *     All strings are copied into the structure, making them safe from 
 *     modification in source strings.    
 *
 * Return:
 *
 *     - SUCCESS: pointer to new `hrb_t`
 *     - FAILURE: `NULL`
 *
 * Programmer: Jacob Smith
 *             2017-07-21
 *
 * Changes:
 *
 *     - Update struct membership for newer 'generic' `hrb_t` format.
 *     --- Jacob Smith, 2017-07-24
 *
 *     - Rename from `hrb_new()` to `hrb_request()`
 *     --- Jacob Smith, 2017-07-25
 *
 *     - Integrate with HDF5.
 *     - Rename from 'hrb_request()` to `H5FD_s3comms_hrb_init_request()`.
 *     - Remove `host` from input paramters.
 *         - Host, as with all other fields, must now be added through the
 *           add-field functions.
 *     - Add `version` (HTTP version string, e.g. "HTTP/1.1") to parameters.
 *     --- Jacob Smith 2017-09-20
 *
 *     - Update to use linked-list `hrb_node_t` headers in structure.
 *     --- Jacob Smith 2017-10-05
 *
 *----------------------------------------------------------------------------
 */
hrb_t *
H5FD_s3comms_hrb_init_request(const char *_verb,
                              const char *_resource,
                              const char *_http_version)
{
    hrb_t  *request   = NULL;
    char   *res       = NULL;
    size_t  reslen    = 0;
    hrb_t  *ret_value = NULL;
    char   *verb      = NULL;
    size_t  verblen   = 0;
    char   *vrsn      = NULL;
    size_t  vrsnlen   = 0;



    FUNC_ENTER_NOAPI_NOINIT

#if S3COMMS_DEBUG
    HDfprintf(stdout, "called H5FD_s3comms_hrb_init_request.\n");
#endif

    if (_resource == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL,
                    "resource string cannot be null.\n");
    }

    /* populate valid NULLs with defaults
     */
    if (_verb == NULL) {
        _verb = "GET";
    }
    if (_http_version == NULL) {
        _http_version = "HTTP/1.1";
    }

    /* malloc space for and prepare structure
     */
    request = (hrb_t *)H5MM_malloc(sizeof(hrb_t));
    if (request == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL,
                    "no space for request structure");
    }
    request->magic        = S3COMMS_HRB_MAGIC;
    request->body         = NULL;
    request->body_len     = 0;
    request->first_header = NULL;



    /* malloc and copy strings for the structure
     */
    if (_resource[0] == '/') {
        reslen = strlen(_resource) + 1;
        res = (char *)H5MM_malloc(sizeof(char) * reslen);
        if (res == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_CANTALLOC, NULL,
                        "no space for resource string");
        }
        strncpy(res, _resource, reslen);
    } else {
        int sprint_ret = 0;
        reslen = strlen(_resource) + 2;
        res = (char *)H5MM_malloc(sizeof(char) * reslen);
        if (res == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_CANTALLOC, NULL,
                        "no space for resource string");
        }
        sprint_ret = snprintf(res, reslen, "/%s", _resource);
        HDassert( sprint_ret > 0 );
        HDassert( (reslen - 1) == (size_t)sprint_ret );
    } /* start resource string with '/' */

    verblen = strlen(_verb) + 1;
    verb = (char *)H5MM_malloc(sizeof(char) * verblen);
    if (verb == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL,
                    "no space for verb string");
    }
    strncpy(verb, _verb, verblen);

    vrsnlen = strlen(_http_version) + 1;
    vrsn = (char *)H5MM_malloc(sizeof(char) * vrsnlen);
    if (vrsn == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL,
                    "no space for http-version string");
    }
    strncpy(vrsn, _http_version, vrsnlen);



    /* place new copies into structure
     */
    request->resource = res;
    request->verb     = verb;
    request->version  = vrsn;

    ret_value = request;

done:

    /* if there is an error, clean up after ourselves
     */
    if (ret_value == NULL) {
        if (request != NULL) {
            H5MM_xfree(request);
        }
        if (vrsn) { H5MM_xfree(vrsn); }
        if (verb) { H5MM_xfree(verb); }
        if (res)  { H5MM_xfree(res);  }
    }

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5FD_s3comms_hrb_init_request */



/****************************************************************************
 * S3R FUNCTIONS 
 ****************************************************************************/


/*----------------------------------------------------------------------------
 *
 * Function: H5FD_s3comms_s3r_close()
 *
 * Purpose:
 *
 *     Close communications through given S3 Request Handle (`s3r_t`)
 *     and clean up associated resources.
 *
 * Return:
 *
 *     - SUCCESS: `SUCCEED`
 *     - FAILURE: `FAIL`
 *         - `handle == NULL`
 *         - `handle->magic != S3COMMS_S3R_MAGIC`
 *
 *
 * Programmer: Jacob Smith
 *             2017-08-31
 *
 * Changes:
 *
 *    - Remove all messiness related to the now-gone "setopt" utility
 *      as it no longer exists in the handle.
 *    - Return type to `void`.
 *    --- Jacob Smith 2017-09-01
 *
 *    - Incorporate into HDF environment.
 *    - Rename from `s3r_close()` to `H5FD_s3comms_s3r_close()`.
 *    --- Jacob Smith 2017-10-06
 *
 *    - Change separate host, resource, port info to `parsed_url_t` struct ptr.
 *    --- Jacob Smith 2017-11-01
 *
 *----------------------------------------------------------------------------
 */
herr_t
H5FD_s3comms_s3r_close(s3r_t *handle)
{
    herr_t ret_value = SUCCEED;



    FUNC_ENTER_NOAPI_NOINIT

#if S3COMMS_DEBUG
    HDfprintf(stdout, "called H5FD_s3comms_s3r_close.\n");
#endif

    if (handle == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "handle cannot be null.\n");
    }
    if (handle->magic != S3COMMS_S3R_MAGIC) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "handle has invalid magic.\n");
    }

    curl_easy_cleanup(handle->curlhandle);
    H5MM_xfree(handle->secret_id);
    H5MM_xfree(handle->region);
    H5MM_xfree(handle->signing_key);
    HDassert( handle->httpverb != NULL );
    H5MM_xfree(handle->httpverb);
    HDassert( SUCCEED == H5FD_s3comms_free_purl(handle->purl) );
    H5MM_xfree(handle);

done:
    FUNC_LEAVE_NOAPI(ret_value)

} /* H5FD_s3comms_s3r_close */


/*----------------------------------------------------------------------------
 *
 * Function: H5FD_s3comms_s3r_getsize()
 *
 * Purpose:
 *
 *    Get the number of bytes of handle's target resource.
 *
 *    Sets handle and curlhandle with to enact an HTTP HEAD request on file,
 *    and parses received headers to extract "Content-Length" from response
 *    headers, storing file size at `handle->filesize`.
 *
 *    Critical step in opening (initiating) an `s3r_t` handle.
 *
 *    Wraps `s3r_read()`.
 *    Sets curlhandle to write headers to a temporary buffer (using extant 
 *    write callback) and provides no buffer for body.
 *
 *    Upon exit, unsets HTTP HEAD settings from curl handle.
 *
 * Return:
 *
 *     - SUCCESS: `SUCCEED`
 *     - FAILURE: `FAIL`
 *
 * Programmer: Jacob Smith
 *             2017-08-23
 *
 * Changes:
 *
 *     - Update to revised `s3r_t` format and life cycle.
 *     --- Jacob Smith 2017-09-01
 *
 *     - Conditional change to static header buffer and structure.
 *     --- Jacob Smith 2017-09-05
 *
 *     - Incorporate into HDF environment.
 *     - Rename from `s3r_getsize()` to `H5FD_s3comms_s3r_getsize()`.
 *     --- Jacob Smith 2017-10-06
 *
 *----------------------------------------------------------------------------
 */
herr_t
H5FD_s3comms_s3r_getsize(s3r_t *handle)
{
    unsigned long int      content_length = 0;
    CURL                  *curlh          = NULL;
    char                  *end            = NULL;
    char                   headerresponse[CURL_MAX_HTTP_HEADER];
    herr_t                 ret_value      = SUCCEED;
    struct s3r_datastruct  sds            = { headerresponse, 0 };
    char                  *start          = NULL;



    FUNC_ENTER_NOAPI_NOINIT

#if S3COMMS_DEBUG
    HDfprintf(stdout, "called H5FD_s3comms_s3r_getsize.\n");
#endif

    if (handle == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "handle cannot be null.\n");
    }
    if (handle->magic != S3COMMS_S3R_MAGIC) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "handle has invalid magic.\n");
    }
    if (handle->curlhandle == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "handle has bad (null) curlhandle.\n")
    }

    /********************
     * PREPARE FOR HEAD *
     ********************/

    curlh = handle->curlhandle;

    if ( CURLE_OK != 
        curl_easy_setopt(curlh,
                         CURLOPT_NOBODY,
                         1L) ) 
    {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "error while setting CURL option (CURLOPT_NOBODY). "
                    "(placeholder flags)");
    }

    /* uses WRITEFUNCTION, as supplied in s3r_open */

    if ( CURLE_OK != 
        curl_easy_setopt(curlh,
                         CURLOPT_HEADERDATA,
                         &sds) ) 
    {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "error while setting CURL option (CURLOPT_HEADERDATA). "
                    "(placeholder flags)");
    }

    HDassert( handle->httpverb == NULL );
    handle->httpverb = (char *)H5MM_malloc(sizeof(char) * 8);
    if (handle->httpverb == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_CANTALLOC, FAIL,
                    "unable to allocate space for S3 request HTTP verb");
    }
    HDmemcpy(handle->httpverb, "HEAD", 5);

    /*******************
     * PERFORM REQUEST *
     *******************/

    /* these parameters fetch the entire file,
     * but, with a NULL destination and NOBODY and HEADERDATA supplied above,
     * only http metadata will be sent by server and recorded by s3comms
     */
    if (FAIL ==
        H5FD_s3comms_s3r_read(handle, 0, 0, NULL) )
    {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "problem in reading during getsize.\n");
    }

    /******************
     * PARSE RESPONSE *
     ******************/

    start = strstr(headerresponse,
                   "\r\nContent-Length: ");
    if (start == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "could not find \"Content-Length\" in response.\n");
    }

    /* move "start" to beginning of value in line; find end of line
     */
    start = start + strlen("\r\nContent-Length: ");
    end = strstr(start,
                 "\r\n");
    if (end == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "could not find end of content length line");
    }

    /* place null terminator at end of numbers 
     */
    *end = '\0'; 

    content_length = strtoul((const char *)start,
                             NULL,
                             0);
    if (content_length == 0         || 
        content_length == ULONG_MAX || 
        errno          == ERANGE) /* errno set by strtoul */
    {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
            "could not convert found \"Content-Length\" response (\"%s\")",
            start); /* range is null-terminated, remember */
    }

    handle->filesize = (size_t)content_length;

    /**********************
     * UNDO HEAD SETTINGS *
     **********************/

    if ( CURLE_OK != 
        curl_easy_setopt(curlh,
                         CURLOPT_NOBODY,
                         0) ) 
    {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "error while setting CURL option (CURLOPT_NOBODY). "
                    "(placeholder flags)");
    }

    if ( CURLE_OK != 
        curl_easy_setopt(curlh,
                         CURLOPT_HEADERDATA,
                         0) ) 
    {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "error while setting CURL option (CURLOPT_HEADERDATA). "
                    "(placeholder flags)");
    }

done:
    FUNC_LEAVE_NOAPI(ret_value);

} /* H5FD_s3comms_s3r_getsize */


/*----------------------------------------------------------------------------
 *
 * Function: H5FD_s3comms_s3r_open()
 *
 * Purpose:
 *
 *     Logically 'open' a file hosted on S3.
 *
 *     - create new Request Handle
 *     - copy supplied url
 *     - copy authentication info if supplied
 *     - create CURL handle
 *     - fetch size of file
 *         - connect with server and execute HEAD request
 *     - return request handle ready for reads
 *
 *     To use 'default' port to connect, `port` should be 0.
 *
 *     To prevent AWS4 authentication, pass null pointer to `region`, `id`,
 *     and `signing_key`.
 *
 *     Uses `H5FD_s3comms_parse_url()` to validate and parse url input.
 *
 * Return:
 *
 *     - Pointer to new request handle.
 *     - NULL 
 *         - authentication strings are not all NULL or all populated
 *         - url is NULL (no filename)
 *         - unable to parse url (malformed?)
 *         - error while performing `getsize()`
 *
 * Programmer: Jacob Smith
 *             2017-09-01
 *
 * Changes: 
 *
 *     - Incorporate into HDF environment.
 *     - Rename from `s3r_open()` to `H5FD_s3comms_s3r_open()`.
 *     --- Jacob Smith 2017-10-06
 *
 *     - Remove port number from signautre.
 *     - Name (`url`) must be complete url with http scheme and optional port
 *       number in string.
 *         - e.g., "http://bucket.aws.com:9000/myfile.dat?query=param"
 *     - Internal storage of host, resource, and port information moved into
 *       `parsed_url_t` struct pointer.
 *     --- Jacob Smith 2017-11-01
 *
 *----------------------------------------------------------------------------
 */
s3r_t *
H5FD_s3comms_s3r_open(const char          *url,
                      const char          *region,
                      const char          *id,
                      const unsigned char *signing_key)
{
    size_t        tmplen    = 0;
    CURL         *curlh     = NULL;
    s3r_t        *h         = NULL; /* "h" for handle */
    parsed_url_t *purl      = NULL;
    s3r_t        *ret_value = NULL;



    FUNC_ENTER_NOAPI_NOINIT

#if S3COMMS_DEBUG
    HDfprintf(stdout, "called H5FD_s3comms_s3r_open.\n");
#endif



    if (url == NULL || url[0] == '\0') {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL,
                    "url cannot be null.\n");
    }

    if (FAIL == H5FD_s3comms_parse_url(url, &purl)) {
        /* probably a malformed url, but could be internal error */
        HGOTO_ERROR(H5E_ARGS, H5E_CANTCREATE, NULL,
                    "unable to create parsed url structure");
    }
    HDassert( purl != NULL ); /* if above passes, this must be true */
    HDassert( purl->magic == S3COMMS_PARSED_URL_MAGIC );

    h = (s3r_t *)H5MM_malloc(sizeof(s3r_t));
    if (h == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_CANTALLOC, NULL,
                    "could not malloc space for handle.\n");
    }

    h->magic        = S3COMMS_S3R_MAGIC;
    h->purl         = purl;
    h->filesize     = 0;
    h->region       = NULL;
    h->secret_id    = NULL;
    h->signing_key  = NULL;
    h->httpverb     = NULL;

    /*************************************
     * RECORD AUTHENTICATION INFORMATION *
     *************************************/

    if ((region      != NULL && *region      != '\0') || 
        (id          != NULL && *id          != '\0') || 
        (signing_key != NULL && *signing_key != '\0'))
    {
        /* if one exists, all three must exist
         */
        if (region == NULL || region[0] == '\0') {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL,
                        "region cannot be null.\n");
        }
        if (id == NULL || id[0] == '\0') {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL,
                        "secret id cannot be null.\n");
        }
        if (signing_key == NULL || signing_key[0] == '\0') {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL,
                        "signing key cannot be null.\n");
        }

        /* copy strings 
         */
        tmplen = strlen(region) + 1;
        h->region = (char *)H5MM_malloc(sizeof(char) * tmplen);
        if (h->region == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL,
                        "could not malloc space for handle region copy.\n");
        }
        HDmemcpy(h->region, region, tmplen);

        tmplen = strlen(id) + 1;
        h->secret_id = (char *)H5MM_malloc(sizeof(char) * tmplen);
        if (h->secret_id == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL,
                        "could not malloc space for handle ID copy.\n");
        }
        HDmemcpy(h->secret_id, id, tmplen);

        tmplen = SHA256_DIGEST_LENGTH;
        h->signing_key = (unsigned char *)H5MM_malloc(sizeof(unsigned char) * \
                                                 tmplen);
        if (h->signing_key == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL,
                        "could not malloc space for handle key copy.\n");
        }
        HDmemcpy(h->signing_key, signing_key, tmplen);
    } /* if authentication information provided */

    /************************
     * INITIATE CURL HANDLE *
     ************************/

    curlh = curl_easy_init();

    if (curlh == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL,
                    "problem creating curl easy handle!\n");
    }

    if ( CURLE_OK != 
        curl_easy_setopt(curlh,
                         CURLOPT_HTTPGET,
                         1L) ) 
    {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL,
                    "error while setting CURL option (CURLOPT_HTTPGET). "
                    "(placeholder flags)");
    }

    if ( CURLE_OK != 
        curl_easy_setopt(curlh,
                         CURLOPT_HTTP_VERSION,
                         CURL_HTTP_VERSION_1_1) ) 
    {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL,
                    "error while setting CURL option (CURLOPT_HTTP_VERSION). "
                    "(placeholder flags)");
    }

    if ( CURLE_OK != 
        curl_easy_setopt(curlh,
                         CURLOPT_FAILONERROR,
                         1L) ) 
    {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL,
                    "error while setting CURL option (CURLOPT_FAILONERROR). "
                    "(placeholder flags)");
    }

    if ( CURLE_OK != 
        curl_easy_setopt(curlh,
                         CURLOPT_WRITEFUNCTION,
                         curlwritecallback) ) 
    {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL,
                    "error while setting CURL option (CURLOPT_WRITEFUNCTION). "
                    "(placeholder flags)");
    }

    if ( CURLE_OK != 
        curl_easy_setopt(curlh,
                         CURLOPT_URL,
                         url) )
    {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL,
                    "error while setting CURL option (CURLOPT_URL). "
                    "(placeholder flags)");
    }

#if S3COMMS_CURL_VERBOSITY > 1
    /* CURL will print (to stdout) information for each operation 
     */
    curl_easy_setopt(curlh, CURLOPT_VERBOSE, 1L);
#endif 

    h->curlhandle = curlh;

    /*******************
     * OPEN CONNECTION *
     * * * * * * * * * *
     *  GET FILE SIZE  *
     *******************/

    if (FAIL == 
        H5FD_s3comms_s3r_getsize(h) ) 
    {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL,
                     "problem in H5FD_s3comms_s3r_getsize.\n");
    }

    /*********************
     * FINAL PREPARATION *
     *********************/

    HDassert( h->httpverb != NULL );
    HDmemcpy(h->httpverb, "GET", 4);

    ret_value = h;

done:
    if (ret_value == NULL) {
        if (curlh != NULL) {
            curl_easy_cleanup(curlh);
        }
        HDassert( SUCCEED == H5FD_s3comms_free_purl(purl) );
        if (h != NULL) {
            H5MM_xfree(h->region);
            H5MM_xfree(h->secret_id);
            H5MM_xfree(h->signing_key);
            if (h->httpverb != NULL) {
                H5MM_xfree(h->httpverb);
            }
            H5MM_xfree(h);
        }
    }

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5FD_s3comms_s3r_open */


/*----------------------------------------------------------------------------
 *
 * Function: H5FD_s3comms_s3r_read()
 *
 * Purpose:
 *
 *     Read file pointed to by request handle, writing specified
 *     `offset` .. `offset + len` bytes to buffer `dest`.
 *
 *     If `len` is 0, reads entirety of file starting at `offset`.
 *     If `offset` and `len` are both 0, reads entire file.
 *
 *     If `offset` or `offset+len` is greater than the file size, read is 
 *     aborted and returns `FAIL`.
 *
 *     High-level routine to execute request as defined in provided handle.
 *
 *     Uses configured "curl easy handle" to perform request.
 *
 *     In event of error, buffer should remain unaltered.
 *
 *     If handle is set to authorize a request, creates a new (temporary)
 *     HTTP Request object (hrb_t) for generating requisite headers,
 *     which is then translated to a `curl slist` and set in the curl handle
 *     for the request.
 *
 *     `dest` may be NULL, but no body data will be recorded.
 *
 *     - NULL dest used for `s3r_getsize()`, in conjunction with
 *       CURLOPT_NOBODY to preempt transmission of file body data.
 *
 * Return:
 *
 *     - SUCCESS: `SUCCEED`
 *     - FAILURE: `FAIL`
 *
 * Programmer: Jacob Smith
 *             2017-08-22
 *
 * Changes:
 *
 *     - Revise structure to prevent unnecessary hrb_t element creation.
 *     - Rename tmprstr -> rangebytesstr to reflect purpose.
 *     - Insert needed `free()`s, particularly for `sds`.
 *     --- Jacob Smith 2017-08-23
 *
 *     - Revise heavily to accept buffer, range as parameters.
 *     - Utilize modified s3r_t format.
 *     --- Jacob Smith 2017-08-31
 *
 *     - Incorporate into HDF library.
 *     - Rename from `s3r_read()` to `H5FD_s3comms_s3r_read()`.
 *     - Return `herr_t` succeed/fail instead of S3code.
 *     - Update to use revised `hrb_t` and `hrb_node_t` structures.
 *     --- Jacob Smith 2017-10-06
 *
 *     - Update to use `parsed_url_t *purl` in handle.
 *     --- Jacob Smith 2017-11-01
 *
 *     - Better define behavior upon read past EOF
 *     --- Jacob Smith 2017-01-19
 *
 *----------------------------------------------------------------------------
 */
herr_t
H5FD_s3comms_s3r_read(s3r_t   *handle,
                      haddr_t  offset,
                      size_t   len,
                      void    *dest)
{
    CURL                  *curlh         = NULL;
    CURLcode               p_status      = CURLE_OK;
    struct curl_slist     *curlheaders   = NULL;
    hrb_node_t            *headers       = NULL;
    hrb_node_t            *node          = NULL;
    struct tm             *now           = NULL;
    char                  *rangebytesstr = NULL;
    hrb_t                 *request       = NULL;
    int                    ret           = 0; /* working variable to check  */
                                              /* return value of snprintf  */
    herr_t                 ret_value     = SUCCEED;
    struct s3r_datastruct *sds           = NULL;



    FUNC_ENTER_NOAPI_NOINIT

#if S3COMMS_DEBUG
    HDfprintf(stdout, "called H5FD_s3comms_s3r_read.\n");
#endif

    /**************************************
     * ABSOLUTELY NECESSARY SANITY-CHECKS *
     **************************************/

    if (handle == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "handle cannot be null.\n");
    }
    HDassert( handle->magic == S3COMMS_S3R_MAGIC );
/*
    if (handle->magic != S3COMMS_S3R_MAGIC) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "handle has invalid magic.\n");
    }
*/
    if (handle->curlhandle == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "handle has bad (null) curlhandle.\n")
    }
    if (handle->purl == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "handle has bad (null) url.\n")
    }
    HDassert( handle->purl->magic == S3COMMS_PARSED_URL_MAGIC );
    if (offset > handle->filesize || (len + offset) > handle->filesize) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "unable to read past EoF")
    }

    curlh = handle->curlhandle;

    /*********************
     * PREPARE WRITEDATA *
     *********************/

    if (dest != NULL) {
        sds = (struct s3r_datastruct *)H5MM_malloc(
                        sizeof(struct s3r_datastruct));
        if (sds == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_CANTALLOC, FAIL,
                        "could not malloc destination datastructure.\n");
        }

        sds->data = (char *)dest;
        sds->size = 0;
        if (CURLE_OK !=
            curl_easy_setopt(curlh,
                             CURLOPT_WRITEDATA,
                             sds) )
        {
            HGOTO_ERROR(H5E_ARGS, H5E_UNINITIALIZED, FAIL,
                        "error while setting CURL option (CURLOPT_WRITEDATA). "
                        "(placeholder flags)");
        }
    }

    /*********************
     * FORMAT HTTP RANGE *
     *********************/

    if (len > 0) {
        rangebytesstr = (char *)H5MM_malloc(sizeof(char) * \
                                            MAX_RANGE_BYTES_STR_LEN );
        if (rangebytesstr == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_CANTALLOC, FAIL,
                        "could not malloc range format string.\n");
        }
        ret = snprintf(rangebytesstr, 
                       (MAX_RANGE_BYTES_STR_LEN),
                       "bytes="H5_PRINTF_HADDR_FMT"-"H5_PRINTF_HADDR_FMT,
                       offset,
                       offset + len);
        HDassert( ret > 0 );
        HDassert( MAX_RANGE_BYTES_STR_LEN > ret );
    } else if (offset > 0) {
        rangebytesstr = (char *)H5MM_malloc(sizeof(char) * \
                                            MAX_RANGE_BYTES_STR_LEN);
        if (rangebytesstr == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_CANTALLOC, FAIL,
                        "could not malloc range format string.\n");
        }
        ret = snprintf(rangebytesstr, 
                       (MAX_RANGE_BYTES_STR_LEN),
                      "bytes="H5_PRINTF_HADDR_FMT"-",
                      offset);
        HDassert( ret > 0 );
        HDassert( MAX_RANGE_BYTES_STR_LEN > ret );
    }

    /*******************
     * COMPILE REQUEST *
     *******************/

    if (handle->signing_key == NULL) {
        /* Do not authenticate.
         */
        if (rangebytesstr != NULL) {
            /* Pass in range directly
             */
            char *bytesrange_ptr = NULL; /* pointer past "bytes=" portion */
            bytesrange_ptr = strchr(rangebytesstr, '=');
            HDassert( bytesrange_ptr != NULL );
            bytesrange_ptr++; /* move to first char past '=' */
            HDassert( *bytesrange_ptr != '\0' );

            if (CURLE_OK !=
                curl_easy_setopt(curlh,
                                 CURLOPT_RANGE,
                                 bytesrange_ptr) )
            {
                HGOTO_ERROR(H5E_VFL, H5E_UNINITIALIZED, FAIL,
                        "error while setting CURL option (CURLOPT_RANGE). ");
            } /* curl setopt failed */
        } /* if rangebytesstr is defined */
    } else {
        /* authenticate request */
        char authorization[512];
            /*   512 := approximate max length...
             *    67 <len("AWS4-HMAC-SHA256 Credential=///s3/aws4_request,"
             *           "SignedHeaders=,Signature=")>
             * +   8 <yyyyMMDD>
             * +  64 <hex(sha256())>
             * + 128 <max? len(secret_id)>
             * +  20 <max? len(region)>
             * + 128 <max? len(signed_headers)>
             */
        char buffer1[512]; /* -> Canonical Request -> Signature */
        char buffer2[256]; /* -> String To Sign -> Credential */
        char iso8601now[ISO8601_SIZE];
        char signed_headers[48]; 
            /* should be large enough for nominal listing:  
             * "host;range;x-amz-content-sha256;x-amz-date" 
             * + '\0', with "range;" possibly absent         
             */

        /* zero start of strings */
        authorization[0]  = 0;
        buffer1[0]        = 0;
        buffer2[0]        = 0;
        iso8601now[0]     = 0;
        signed_headers[0] = 0;

        /**** VERIFY INFORMATION EXISTS ****/

        if (handle->region == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "handle must have non-null region.\n");
        }
        if (handle->secret_id == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "handle must have non-null secret_id.\n");
        }
        if (handle->signing_key == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "handle must have non-null signing_key.\n");
        }
        if (handle->httpverb == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "handle must have non-null httpverb.\n");
        }
        if (handle->purl->host == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "handle must have non-null host.\n");
        }
        if (handle->purl->path == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "handle must have non-null resource.\n");
        }

        /**** CREATE HTTP REQUEST STRUCTURE (hrb_t) ****/

        request = H5FD_s3comms_hrb_init_request(
                      (const char *)handle->httpverb,
                      (const char *)handle->purl->path,
                      "HTTP/1.1");
        if (request == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "could not allocate hrb_t request.\n");
        }
        HDassert( request->magic == S3COMMS_HRB_MAGIC );

        now = gmnow();
        if ( ISO8601NOW(iso8601now, now) != (ISO8601_SIZE - 1)) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "could not format ISO8601 time.\n");
        }

        HDassert( SUCCEED ==
                  H5FD_s3comms_hrb_node_set(
                          &headers, 
                          "Host",
                          (const char *)handle->purl->host) );
        if (headers == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "problem building headers list. "
                        "(placeholder flags)\n");
        }
        HDassert( headers->magic == S3COMMS_HRB_NODE_MAGIC );

        if (rangebytesstr != NULL) {
            HDassert( SUCCEED ==
                      H5FD_s3comms_hrb_node_set(
                              &headers, 
                              "Range", 
                              (const char *)rangebytesstr) );
            if (headers == NULL) {
                HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                            "problem building headers list. "
                            "(placeholder flags)\n");
            }
            HDassert( headers->magic == S3COMMS_HRB_NODE_MAGIC );
        }
        HDassert( SUCCEED ==
                  H5FD_s3comms_hrb_node_set(
                          &headers, 
                          "x-amz-content-sha256", 
                          (const char *)EMPTY_SHA256) );
        if (headers == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "problem building headers list. "
                        "(placeholder flags)\n");
        }
        HDassert( headers->magic == S3COMMS_HRB_NODE_MAGIC );

        HDassert( SUCCEED ==
                  H5FD_s3comms_hrb_node_set(
                          &headers, 
                          "x-amz-date", 
                          (const char *)iso8601now) );

        if (headers == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "problem building headers list. "
                        "(placeholder flags)\n");
        }
        HDassert( headers->magic == S3COMMS_HRB_NODE_MAGIC );

        request->first_header = headers;

        /**** COMPUTE AUTHORIZATION ****/

        if (FAIL ==      /* buffer1 -> canonical request */
            H5FD_s3comms_aws_canonical_request(buffer1,
                    signed_headers,
                    request) )
        {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "(placeholder flags)\n");
        }
        if ( FAIL ==     /* buffer2->string-to-sign */
             H5FD_s3comms_tostringtosign(buffer2,
                                         buffer1,
                                         iso8601now,
                                         handle->region) )
        {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "(placeholder flags)\n");
        }
        if (FAIL ==     /* buffer1 -> signature */
            H5FD_s3comms_HMAC_SHA256(handle->signing_key,
                                     SHA256_DIGEST_LENGTH,
                                     buffer2,
                                     strlen(buffer2),
                                     buffer1) )
        {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "(placeholder flags)\n");
        }

        iso8601now[8] = 0; /* trim to yyyyMMDD */
        ret = S3COMMS_FORMAT_CREDENTIAL(buffer2, 
                                        handle->secret_id, 
                                        iso8601now, 
                                        handle->region, 
                                        "s3");
        HDassert( ret > 0 );
        HDassert( S3COMMS_MAX_CREDENTIAL_SIZE > ret );

        ret = snprintf(authorization, 
                512,
                "AWS4-HMAC-SHA256 Credential=%s,SignedHeaders=%s,Signature=%s",
                buffer2,
                signed_headers,
                buffer1);
        HDassert( ret > 0 );
        HDassert( 512 > ret );

        /* append authorization header to http request buffer
         */
        HDassert( SUCCEED ==
                  H5FD_s3comms_hrb_node_set(
                          &headers, 
                          "Authorization", 
                          (const char *)authorization) );
        if (headers == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "problem building headers list. "
                        "(placeholder flags)\n");
        }

        /* update hrb's "first header" pointer
         */
        request->first_header = headers;

        /**** SET CURLHANDLE HTTP HEADERS FROM GENERATED DATA ****/

        node = request->first_header;
        while (node != NULL) {
            HDassert( node->magic == S3COMMS_HRB_NODE_MAGIC );
            curlheaders = curl_slist_append(curlheaders, 
                                            (const char *)node->cat);
            if (curlheaders == NULL) {
                HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                            "could not append header to curl slist. "
                            "(placeholder flags)\n");
            }
            node = node->next;
        }

        /* sanity-check
         */
        if (curlheaders == NULL) {
            /* above loop was probably never run */
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "curlheaders was never populated.\n");
        }

        /* finally, set http headers in curl handle
         */
        if (CURLE_OK !=
            curl_easy_setopt(curlh,
                             CURLOPT_HTTPHEADER,
                             curlheaders) )
        {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "error while setting CURL option "
                        "(CURLOPT_HTTPHEADER). (placeholder flags)");
        }

    } /* if should authenticate (info provided) */

    /*******************
     * PERFORM REQUEST *
     *******************/

#if S3COMMS_CURL_VERBOSITY > 0
    /* In event of error, print detailed information to stderr
     * This is not the default behavior.
     */
    {
        long int httpcode = 0;
        char     curlerrbuf[CURL_ERROR_SIZE];
        curlerrbuf[0] = '\0';

        HDassert( CURLE_OK == 
                  curl_easy_setopt(curlh, CURLOPT_ERRORBUFFER, curlerrbuf) );

        p_status = curl_easy_perform(curlh);

        if (p_status != CURLE_OK) {
            HDassert( CURLE_OK ==
                 curl_easy_getinfo(curlh, CURLINFO_RESPONSE_CODE, &httpcode) );
            HDfprintf(stderr, "CURL ERROR CODE: %d\nHTTP CODE: %d\n", 
                     p_status, httpcode);
            HDfprintf(stderr, "%s\n", curl_easy_strerror(p_status));
            HGOTO_ERROR(H5E_VFL, H5E_CANTOPENFILE, FAIL,
                    "problem while performing request.\n");
        }
        HDassert( CURLE_OK == 
                  curl_easy_setopt(curlh, CURLOPT_ERRORBUFFER, NULL) );
    } /* verbose error reporting */
#else
    p_status = curl_easy_perform(curlh);

    if (p_status != CURLE_OK) {
        HGOTO_ERROR(H5E_VFL, H5E_CANTOPENFILE, FAIL,
                    "problem while performing request.\n");
    }
#endif

done:
    /* clean any malloc'd resources
     */
    if (curlheaders   != NULL) curl_slist_free_all(curlheaders);
    if (rangebytesstr != NULL) H5MM_xfree(rangebytesstr);
    if (sds           != NULL) H5MM_xfree(sds);
    if (request       != NULL) {
        while (headers != NULL) 
            HDassert( SUCCEED ==
                      H5FD_s3comms_hrb_node_set(&headers, headers->name, NULL));
        HDassert( NULL == headers );
        HDassert( SUCCEED == H5FD_s3comms_hrb_destroy(&request) );
        HDassert( NULL == request );
    }

    if (curlh != NULL) {
        /* clear any Range */
        HDassert( CURLE_OK == curl_easy_setopt(curlh, CURLOPT_RANGE, NULL) );

        /* clear headers */
        HDassert( CURLE_OK == 
                  curl_easy_setopt(curlh, CURLOPT_HTTPHEADER, NULL) );
    }

    FUNC_LEAVE_NOAPI(ret_value);

} /* H5FD_s3comms_s3r_read */



/****************************************************************************
 * MISCELLANEOUS FUNCTIONS
 ****************************************************************************/


/*----------------------------------------------------------------------------
 *
 * Function: gmnow()
 *
 * Purpose:
 *
 *    Get the output of `time.h`'s `gmtime()` call while minimizing setup 
 *    clutter where important.
 *
 * Return:
 *
 *    Pointer to resulting `struct tm`,as created by gmtime(time_t * T).
 *
 * Programmer: Jacob Smith
 *             2017-07-12
 *
 * Changes: None.
 *
 *----------------------------------------------------------------------------
 */
struct tm *
gmnow(void)
{
    time_t     now;
    time_t    *now_ptr = &now;
    struct tm *ret_value = NULL;

    /* Doctor assert, checks against error in time() */
    HDassert( (time_t)(-1) != time(now_ptr) );

    /* check against error when performing gmtime() */
    ret_value = gmtime(now_ptr);
    HDassert( ret_value != NULL );

    return ret_value;

} /* gmnow */


/*----------------------------------------------------------------------------
 *
 * Function: H5FD_s3comms_aws_canonical_request()
 *
 * Purpose:
 *
 *     Compose AWS "Canonical Request" (and signed headers string)
 *     as defined in the REST API documentation.
 *
 *     Both destination strings are null-terminated.
 *      
 *     Destination string arguments must be provided with adequate space.
 *
 *     Canonical Request format:
 *
 *      <HTTP VERB>"\n"
 *      <resource path>"\n"
 *      <query string>"\n"
 *      <header1>"\n" (`lowercase(name)`":"`trim(value)`)
 *      <header2>"\n"
 *      ... (headers sorted by name)
 *      <header_n>"\n"
 *      "\n"
 *      <signed headers>"\n" (`lowercase(header 1 name)`";"`header 2 name`;...)
 *      <hex-string of sha256sum of body> ("e3b0c4429...", e.g.)
 *
 * Return:
 *
 *     - SUCCESS: `SUCCEED`
 *         - writes canonical request to respective `...dest` strings
 *     - FAILURE: `FAIL`
 *         - one or more input argument was NULL
 *         - internal error
 *
 * Programmer: Jacob Smith
 *             2017-10-04
 *
 * Changes: None.
 *
 *----------------------------------------------------------------------------
 */
herr_t 
H5FD_s3comms_aws_canonical_request(char  *canonical_request_dest,
                                   char  *signed_headers_dest,
                                   hrb_t *http_request)
{
    hrb_node_t *node         = NULL;
    const char *query_params = ""; /* unused at present */
    herr_t      ret_value    = SUCCEED;
    int         ret          = 0; /* return value of snprintf */
    size_t      len          = 0; /* working string length variable */
    char        tmpstr[256];

    /* "query params" refers to the optional element in the URL, e.g.
     *     http://bucket.aws.com/myfile.txt?max-keys=2&prefix=J
     *                                      ^-----------------^
     *
     * Not handled/implemented as of 2017-10-xx.
     * Element introduced as empty placeholder and reminder.
     * Further research to be done if this is ever releveant for the
     * VFD use-cases.
     */



    FUNC_ENTER_NOAPI_NOINIT

#if S3COMMS_DEBUG
    HDfprintf(stdout, "called H5FD_s3comms_aws_canonical_request.\n");
#endif

    if (http_request == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "hrb object cannot be null.\n");
    }
    HDassert( http_request->magic == S3COMMS_HRB_MAGIC );

    if (canonical_request_dest == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "canonical request destination cannot be null.\n");
    }

    if (signed_headers_dest == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "signed headers destination cannot be null.\n");
    }

    /* HTTP verb, resource path, and query string lines
     */
    len = (strlen(http_request->verb) +
              strlen(http_request->resource) +
              strlen(query_params) +
              3 );
    ret = snprintf(canonical_request_dest,
                   len + 1,
                   "%s\n%s\n%s\n",
                   http_request->verb,
                   http_request->resource,
                   query_params);
    HDassert( ret > 0 );
    HDassert( len == (size_t)ret );

    /* write in canonical headers, building signed headers concurrently
     */
    node = http_request->first_header; /* assumed at first sorted */
    while (node != NULL) {
        size_t join_len  = 0; /* string len of joined header-value */

        HDassert( node->magic == S3COMMS_HRB_NODE_MAGIC );

        len = strlen(node->lowername);
        join_len = strlen(node->value) + len + 2; /* +2 <- ":\n" */
        ret = snprintf(tmpstr,
                       join_len + 1, /* +1 for null terminator */
                       "%s:%s\n",
                       node->lowername,
                       node->value);
        HDassert( ret > 0 );
        HDassert( join_len == (size_t)ret );
        strcat(canonical_request_dest, tmpstr);

        len += 1; /* semicolon */
        ret = snprintf(tmpstr,
                       len + 1,
                       "%s;",
                       node->lowername);
        HDassert( ret > 0 );
        HDassert( len == (size_t)ret );
        strcat(signed_headers_dest, tmpstr);

        node = node->next;
    }

    /* remove tailing ';' from signed headers sequence 
     */
    signed_headers_dest[strlen(signed_headers_dest) - 1] = '\0';

    /* append signed headers and payload hash
     * NOTE: at present, no HTTP body is handled, per the nature of
     *       requests/range-gets
     */ 
    strcat(canonical_request_dest, "\n");
    strcat(canonical_request_dest, signed_headers_dest);
    strcat(canonical_request_dest, "\n");
    strcat(canonical_request_dest, EMPTY_SHA256);

done:
    FUNC_LEAVE_NOAPI(ret_value);

} /* H5FD_s3comms_aws_canonical_request */


/*----------------------------------------------------------------------------
 *
 * Function: H5FD_s3comms_bytes_to_hex()
 *
 * Purpose:
 *
 *     Produce human-readable hex string [0-9A-F] from sequence of bytes.
 *
 *     For each byte (char), writes two-character hexadecimal representation.
 *
 *     No null-terminator appended.
 *
 *     Assumes `dest` is allocated to enough size (msg_len * 2).
 *
 *     Fails if either `dest` or `msg` are null.
 *
 *     `msg_len` message length of 0 has no effect.
 *
 * Return:
 *
 *     - SUCCESS: `SUCCEED`
 *         - hex string written to `dest` (not null-terminated)
 *     - FAILURE: `FAIL`
 *         - `dest == NULL`
 *         - `msg == NULL`
 *
 * Programmer: Jacob Smith
 *             2017-07-12
 *
 * Changes: 
 *
 *     - Integrate into HDF.
 *     - Rename from hex() to H5FD_s3comms_bytes_to_hex.
 *     - Change return type from `void` to `herr_t`.
 *     --- Jacob Smtih 2017-09-14
 *
 *     - Add bool parameter `lowercase` to configure upper/lowercase output
 *       of a-f hex characters.
 *     --- Jacob Smith 2017-09-19
 *
 *     - Change bool type to `hbool_t`
 *     --- Jacob Smtih 2017-10-11
 *
 *----------------------------------------------------------------------------
 */
herr_t 
H5FD_s3comms_bytes_to_hex(char                *dest,
                          const unsigned char *msg,
                          size_t               msg_len,
                          hbool_t              lowercase)
{
    size_t i         = 0;
    herr_t ret_value = SUCCEED;



    FUNC_ENTER_NOAPI_NOINIT

#if S3COMMS_DEBUG
    HDfprintf(stdout, "called H5FD_s3comms_bytes_to_hex.\n");
#endif

    if (dest == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, 
                    "hex destination cannot be null.\n")
    }
    if (msg == NULL) {
       HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, 
                   "bytes sequence cannot be null.\n")
    }

    for (i = 0; i < msg_len; i++) {
        HDassert( 2 == snprintf(&(dest[i * 2]),
                                3, /* 'X', 'X', '\n' */
                                (lowercase == TRUE) ? "%02x"
                                                    : "%02X",
                                msg[i]) );
    }

done:
    FUNC_LEAVE_NOAPI(ret_value);

} /* H5FD_s3comms_bytes_to_hex */


/*----------------------------------------------------------------------------
 *
 * Function: H5FD_s3comms_free_purl()
 *
 * Purpose: 
 *
 *     Release resources from a parsed_url_t pointer.
 *
 *     If pointer is null, nothing happens.
 *
 * Return: 
 *
 *     `SUCCEED` (never fails)
 *
 * Programmer: Jacob Smith
 *             2017-11-01
 *
 * Changes: None.
 *
 *----------------------------------------------------------------------------
 */
herr_t
H5FD_s3comms_free_purl(parsed_url_t *purl) 
{
    FUNC_ENTER_NOAPI_NOINIT_NOERR

#if S3COMMS_DEBUG
    HDprintf("called H5FD_s3comms_free_purl.\n");
#endif

    if (purl != NULL) {
        HDassert( purl->magic == S3COMMS_PARSED_URL_MAGIC );
        if (purl->scheme != NULL) H5MM_xfree(purl->scheme); 
        if (purl->host   != NULL) H5MM_xfree(purl->host);
        if (purl->port   != NULL) H5MM_xfree(purl->port);
        if (purl->path   != NULL) H5MM_xfree(purl->path);
        if (purl->query  != NULL) H5MM_xfree(purl->query);
        purl->magic = (unsigned long)(~S3COMMS_PARSED_URL_MAGIC);
        H5MM_xfree(purl);
    }

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* H5FD_s3comms_free_purl */


/*----------------------------------------------------------------------------
 *
 * Function: H5FD_s3comms_HMAC_SHA256()
 *
 * Purpose:
 *
 *     Generate Hash-based Message Authentication Checksum using the SHA-256 
 *     hashing algorithm.
 *
 *     Given a key, message, and respective lengths (to accommodate null 
 *     characters in either), generate _hex string_ of authentication checksum 
 *     and write to `dest`.
 *
 *     `dest` must be at least `SHA256_DIGEST_LENGTH * 2` characters in size.
 *     Not enforceable by this function.
 *     `dest` will _not_ be null-terminated by this function.
 *
 * Return: 
 *
 *     - SUCCESS: `SUCCEED`
 *         - hex string written to `dest` (not null-terminated)
 *     - FAILURE: `FAIL`
 *         - `dest == NULL`
 *         - error while generating hex string output
 *
 * Programmer: Jacob Smith
 *             2017-07-??
 *
 * Changes: 
 *
 *     - Integrate with HDF5.
 *     - Rename from `HMAC_SHA256` to `H5FD_s3comms_HMAC_SHA256`.
 *     - Rename output paremeter from `md` to `dest`.
 *     - Return `herr_t` type instead of `void`.
 *     - Call `H5FD_s3comms_bytes_to_hex` to generate hex cleartext for output.
 *     --- Jacob Smith 2017-09-19
 *
 *     - Use static char array instead of malloc'ing `md`
 *     --- Jacob Smith 2017-10-10
 *
 *----------------------------------------------------------------------------
 */
herr_t
H5FD_s3comms_HMAC_SHA256(const unsigned char *key,
                         size_t               key_len,
                         const char          *msg,
                         size_t               msg_len,
                         char                *dest)
{
    unsigned char md[SHA256_DIGEST_LENGTH];
    unsigned int  md_len    = SHA256_DIGEST_LENGTH;
    herr_t        ret_value = SUCCEED;



    FUNC_ENTER_NOAPI_NOINIT

#if S3COMMS_DEBUG
    HDfprintf(stdout, "called H5FD_s3comms_HMAC_SHA256.\n");
#endif

    if (dest == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "destination cannot be null.");
    }

    HMAC(EVP_sha256(),
         key,
         (int)key_len,
         (const unsigned char *)msg,
         msg_len,
         md,
         &md_len); 

    if (FAIL ==
        H5FD_s3comms_bytes_to_hex(dest, 
                                  (const unsigned char *)md, 
                                  (size_t)md_len,
                                  true))
    {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "could not convert to hex string.");
    }

done:
    FUNC_LEAVE_NOAPI(ret_value);

} /* H5FD_s3comms_HMAC_SHA256 */


/*----------------------------------------------------------------------------
 *
 * Function: H5FD_s3comms_nlowercase()
 *
 * Purpose:
 *
 *     From string starting at `s`, write `len` characters to `dest`,
 *     converting all to lowercase.
 *
 *     Behavior is undefined if `s` is NULL or `len` overruns the allocated
 *     space of either `s` or `dest`.
 *
 *     Provided as convenience.
 *
 * Return:
 *
 *     - SUCCESS: `SUCCEED`
 *         - upon completion, `dest` is populated
 *     - FAILURE: `FAIL`
 *         - `dest == NULL`
 *
 * Programmer: Jacob Smith
 *             2017-09-18
 *
 * Changes: None.
 *
 *----------------------------------------------------------------------------
 */
herr_t
H5FD_s3comms_nlowercase(char       *dest,
                        const char *s,
                        size_t      len)
{
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT 

#if S3COMMS_DEBUG
    HDfprintf(stdout, "called H5FD_s3comms_nlowercase.\n");
#endif

    if (dest == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "destination cannot be null.\n");
    }

    if (len > 0) {
        HDmemcpy(dest, s, len);
        do {
            len--;
            dest[len] = (char)tolower( (int)dest[len] );
        } while (len > 0);
    }

done:
    FUNC_LEAVE_NOAPI(ret_value);

} /* H5FD_s3comms_nlowercase */


/*----------------------------------------------------------------------------
 * 
 * Function: H5FD_s3comms_parse_url()
 *
 * Purpose:
 *
 *     Parse URL-like string and stuff URL componenets into 
 *     `parsed_url` structure, if possible.
 *
 *     Expects null-terminated string of format:
 *     SCHEME "://" HOST [":" PORT ] ["/" [ PATH ] ] ["?" QUERY]
 *     where SCHEME :: "[a-zA-Z/.-]+"
 *           PORT   :: "[0-9]"
 *
 *     Stores resulting structure in argument pointer `purl`, if successful,
 *     creating and populating new `parsed_url_t` structure pointer.
 *     Empty or absent elements are NULL in new purl structure.
 *
 * Return:
 *
 *     - SUCCESS: `SUCCEED`
 *         - `purl` pointer is populated
 *     - FAILURE: `FAIL`
 *         - unable to parse
 *             - `purl` is unaltered (probably NULL)
 *
 * Programmer: Jacob Smith
 *             2017-10-30
 *
 * Changes: None.
 *
 *----------------------------------------------------------------------------
 */
herr_t
H5FD_s3comms_parse_url(const char    *str,
                       parsed_url_t **_purl)
{
    parsed_url_t *purl         = NULL; /* pointer to new structure */
    const char   *tmpstr       = NULL; /* working pointer in string */
    const char   *curstr       = str;  /* "start" pointer in string */
    long int      len          = 0;    /* substring length */
    long int      urllen       = 0;    /* length of passed-in url string */
    unsigned int  i            = 0;
    herr_t        ret_value    = FAIL; 



    FUNC_ENTER_NOAPI_NOINIT;

#if S3COMMS_DEBUG
    HDprintf("called H5FD_s3comms_parse_url.\n");
#endif

    if (str == NULL || *str == '\0') {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "invalid url string");
    }

    urllen = (long int)strlen(str);

    purl = (parsed_url_t *)H5MM_malloc(sizeof(parsed_url_t));
    if (purl == NULL) { 
        HGOTO_ERROR(H5E_ARGS, H5E_CANTALLOC, FAIL,
                    "can't allocate space for parsed_url_t");
    }
    purl->magic  = S3COMMS_PARSED_URL_MAGIC;
    purl->scheme = NULL;
    purl->host   = NULL;
    purl->port   = NULL;
    purl->path   = NULL;
    purl->query  = NULL;

    /***************
     * READ SCHEME *
     ***************/

    tmpstr = strchr(curstr, ':');
    if (tmpstr == NULL) { 
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "invalid SCHEME consruction: probably not URL");
    }
    len = tmpstr - curstr;
    HDassert( (0 <= len) && (len < urllen) );

    /* check for restrictions 
     */
    for (i = 0; i < len; i++) {
        /* scheme = [a-zA-Z+-.]+ (terminated by ":") */
        if (!isalpha(curstr[i]) && 
             '+' != curstr[i] && 
             '-' != curstr[i] && 
             '.' != curstr[i])
        {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "invalid SCHEME consruction");
        }
    }
    /* copy lowercased scheme to structure
     */
    purl->scheme = (char *)H5MM_malloc(sizeof(char) * (size_t)(len + 1));
    if (purl->scheme == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_CANTALLOC, FAIL,
                    "can't allocate space for SCHEME");
    }
    (void)strncpy(purl->scheme, curstr, (size_t)len);
    purl->scheme[len] = '\0';
    for ( i = 0; i < len; i++ ) {
        purl->scheme[i] = (char)tolower(purl->scheme[i]);
    }

    /* Skip "://" */
    tmpstr += 3;
    curstr = tmpstr;

    /*************
     * READ HOST *
     *************/

    if (*curstr == '[') {
        /* IPv6 */
        while (']' != *tmpstr) {
            if (tmpstr == 0) { /* end of string reached! */
                HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                            "reached end of URL: incomplete IPv6 HOST");
            }
            tmpstr++;
        }
        tmpstr++;
    } else {
        while (0 != *tmpstr) {
            if (':' == *tmpstr || 
                '/' == *tmpstr || 
                '?' == *tmpstr) 
            {
                break;
            }
            tmpstr++;
        }
    } /* if IPv4 or IPv6 */
    len = tmpstr - curstr;
    if (len == 0) { 
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "HOST substring cannot be empty");
    } else if (len > urllen) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "problem with length of HOST substring");
    }

    /* copy host 
     */
    purl->host = (char *)H5MM_malloc(sizeof(char) * (size_t)(len + 1));
    if (purl->host == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_CANTALLOC, FAIL,
                    "can't allocate space for HOST");
    }
    (void)strncpy(purl->host, curstr, (size_t)len);
    purl->host[len] = 0;

    /*************
     * READ PORT *
     *************/

    if (':' == *tmpstr) {
        tmpstr += 1; /* advance past ':' */
        curstr = tmpstr;
        while ((0 != *tmpstr) && ('/' != *tmpstr) && ('?' != *tmpstr)) {
            tmpstr++;
        }
        len = tmpstr - curstr;
        if (len == 0) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "PORT element cannot be empty");
        } else if (len > urllen) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "problem with length of PORT substring");
        }
        for (i = 0; i < len; i ++) {
            if (!isdigit(curstr[i])) { 
                HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                            "PORT is not a decimal string"); 
            }
        }

        /* copy port 
         */
        purl->port = (char *)H5MM_malloc(sizeof(char) * (size_t)(len + 1));
        if (purl->port == NULL) { /* cannot malloc */
                HGOTO_ERROR(H5E_ARGS, H5E_CANTALLOC, FAIL,
                            "can't allocate space for PORT");
        }
        (void)strncpy(purl->port, curstr, (size_t)len);
        purl->port[len] = 0;
    } /* if PORT element */

    /*************
     * READ PATH *
     *************/

    if ('/' == *tmpstr) {
        /* advance past '/' */
        tmpstr += 1;
        curstr = tmpstr;
       
        /* seek end of PATH
         */
        while ((0 != *tmpstr) && ('?' != *tmpstr)) {
            tmpstr++;
        }
        len = tmpstr - curstr;
        if (len > urllen) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "problem with length of PATH substring");
        }
        if (len > 0) {
            purl->path = (char *)H5MM_malloc(sizeof(char) * (size_t)(len + 1));
            if (purl->path == NULL) {
                    HGOTO_ERROR(H5E_ARGS, H5E_CANTALLOC, FAIL,
                                "can't allocate space for PATH");
            } /* cannot malloc path pointer */
            (void)strncpy(purl->path, curstr, (size_t)len);
            purl->path[len] = 0;
        }
    } /* if PATH element */

    /**************
     * READ QUERY *
     **************/

    if ('?' == *tmpstr) {
        tmpstr += 1;
        curstr = tmpstr;
        while (0 != *tmpstr) {
            tmpstr++; 
        }
        len = tmpstr - curstr;
        if (len == 0) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "QUERY cannot be empty");
        } else if (len > urllen) {
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                        "problem with length of QUERY substring");
        }
        purl->query = (char *)H5MM_malloc(sizeof(char) * (size_t)(len + 1));
        if (purl->query == NULL) {
            HGOTO_ERROR(H5E_ARGS, H5E_CANTALLOC, FAIL,
                        "can't allocate space for QUERY");
        } /* cannot malloc path pointer */
        (void)strncpy(purl->query, curstr, (size_t)len);
        purl->query[len] = 0;
    } /* if QUERY exists */



    *_purl = purl;
    ret_value =  SUCCEED;

done:
    if (ret_value == FAIL) {
        H5FD_s3comms_free_purl(purl);
    }
    FUNC_LEAVE_NOAPI(ret_value);

} /* H5FD_s3comms_parse_url */


/*----------------------------------------------------------------------------
 *
 * Function: H5FD_s3comms_percent_encode_char()
 *
 * Purpose:
 *
 *     "Percent-encode" utf-8 character `c`, e.g.,
 *         '$' -> "%24"
 *         '¢' -> "%C2%A2"
 *
 *     `c` cannot be null.
 *
 *     Does not (currently) accept multibyte characters...
 *     limit to (?) u+00ff, well below upper bound for two-byte utf-8 encoding
 *        (u+0080..u+07ff).
 *
 *     Writes output to `repr`.
 *     `repr` cannot be null.
 *     Assumes adequate space i `repr`...
 *         >>> char[4] or [7] for most chatacters,
 *         >>> [13] as theoretical maximum.
 *
 *     Representation `repr` is null-terminated.
 *
 *     Stores length of representation (without null termintor) at pointer
 *     `repr_len`.
 *
 * Return : SUCCEED/FAIL
 *
 *     - SUCCESS: `SUCCEED`
 *         - percent-encoded representation  written to `repr`
 *         - 'repr' is null-terminated
 *     - FAILURE: `FAIL`
 *         - `c` or `repr` was NULL
 *
 * Programmer: Jacob Smith
 *
 * Changes:
 *
 *     - Integrate into HDF.
 *     - Rename from `hexutf8` to `H5FD_s3comms_percent_encode_char`.
 *     --- Jacob Smith 2017-09-15
 *
 *----------------------------------------------------------------------------
 */
herr_t
H5FD_s3comms_percent_encode_char(char                *repr,
                                 const unsigned char  c,
                                 size_t              *repr_len)
{
    unsigned int        acc        = 0;
    unsigned int        i          = 0;
    unsigned int        k          = 0;
    unsigned int        stack[4]   = {0, 0, 0, 0};
    unsigned int        stack_size = 0;
    herr_t              ret_value  = SUCCEED;
#if S3COMMS_DEBUG
    unsigned char       s[2]       = {c, 0}; 
    unsigned char       hex[3]     = {0, 0, 0};
#endif



    FUNC_ENTER_NOAPI_NOINIT

#if S3COMMS_DEBUG
    HDfprintf(stdout, "called H5FD_s3comms_percent_encode_char.\n");
#endif

    if (repr == NULL) {
       HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "no destination `repr`.\n")
    }

#if S3COMMS_DEBUG
    H5FD_s3comms_bytes_to_hex((char *)hex, s, 1, FALSE);
    HDfprintf(stdout, "    CHAR: \'%s\'\n", s);
    HDfprintf(stdout, "    CHAR-HEX: \"%s\"\n", hex);
#endif

    if (c <= (unsigned char)0x7f) {
        /* character represented in a single "byte"
         * and single percent-code
         */
#if S3COMMS_DEBUG
        HDfprintf(stdout, "    SINGLE-BYTE\n");
#endif
        *repr_len = 3;
        HDassert( 3 == snprintf(repr, 4, "%%%02X", c) );
    } else {
        /* multi-byte, multi-percent representation
         */
#if S3COMMS_DEBUG
        HDfprintf(stdout, "    MULTI-BYTE\n");
#endif
        stack_size = 0;
        k = (unsigned int)c;
        *repr_len = 0;
        do {
            /* push number onto stack in six-bit slices
             */
            acc = k;
            acc >>= 6; /* cull least */
            acc <<= 6; /* six bits   */
            stack[stack_size++] = k - acc; /* max six-bit number */
            k = acc >> 6;
        } while (k > 0);

        /* now have "stack" of two to four six-bit numbers
         * to be put into UTF-8 byte fields
         */

#if S3COMMS_DEBUG
        HDfprintf(stdout, "    STACK:\n    {\n");
        for (i = 0; i < stack_size; i++) {
            H5FD_s3comms_bytes_to_hex((char *)hex, 
                                      (unsigned char *)(&stack[i]),
                                      1,
                                      FALSE);
            hex[2] = 0;
            HDfprintf(stdout, "      %s,\n", hex);
        }
        HDfprintf(stdout, "    }\n");
#endif

        /****************
         * leading byte *
         ****************/

        /* prepend 11[1[1]]0 to first byte */
        /* 110xxxxx, 1110xxxx, or 11110xxx */
        acc = 0xC0; /* 2^7 + 2^6 -> 11000000 */
        acc += (stack_size > 2) ? 0x20 : 0; 
        acc += (stack_size > 3) ? 0x10 : 0;
        stack_size -= 1;
        HDassert( 3 == snprintf(repr, 4, "%%%02X", acc + stack[stack_size]) );
        *repr_len += 3;

        /************************
         * continuation byte(s) *
         ************************/

        /* 10xxxxxx */
        for (i = 0; i < stack_size; i++) {
            HDassert( 3 == snprintf(&repr[i*3 + 3], 
                                    4,
                                    "%%%02X",
                                    128 + stack[stack_size - 1 - i]) );
            *repr_len += 3;
        }
    }
    *(repr + *repr_len) = '\0';

done:
    FUNC_LEAVE_NOAPI(ret_value);

} /* H5FD_s3comms_percent_encode_char */


/*----------------------------------------------------------------------------
 *
 * Function: H5FD_s3comms_signing_key()
 *
 * Purpose:
 *
 *     Create AWS4 "Signing Key" from secret key, AWS region, and timestamp.
 *
 *     Sequentially runs HMAC_SHA256 on strings in specified order,
 *     generating re-usable checksum (according to documentation, valid for 
 *     7 days from time given).
 *
 *     `secret` is `access key id` for targeted service/bucket/resource.
 *
 *     `iso8601now` must conform to format, yyyyMMDD'T'hhmmss'Z'
 *     e.g. "19690720T201740Z".
 *
 *     `region` should be one of AWS service region names, e.g. "us-east-1".
 *
 *     Hard-coded "service" algorithm requirement to "s3".
 *
 *     Inputs must be null-terminated strings.
 *
 *     Writes to `md` the raw byte data, length of `SHA256_DIGEST_LENGTH`.
 *     Programmer must ensure that `md` is appropriatley allocated.
 *
 * Return:
 *
 *     - SUCCESS: `SUCCEED`
 *         - raw byte data of signing key written to `md`
 *     - FAILURE: `FAIL`
 *         - if any input arguments was NULL
 *
 * Programmer: Jacob Smith
 *             2017-07-13
 *
 * Changes: 
 *
 *     - Integrate into HDF5.
 *     - Return herr_t type.
 *     --- Jacob Smith 2017-09-18
 *
 *     - NULL check and fail of input parameters.
 *     --- Jacob Smith 2017-10-10
 *
 *----------------------------------------------------------------------------
 */
herr_t
H5FD_s3comms_signing_key(unsigned char *md,
                         const char    *secret,
                         const char    *region,
                         const char    *iso8601now)
{
    char          *AWS4_secret     = NULL;
    size_t         AWS4_secret_len = 0;
    unsigned char  datekey[SHA256_DIGEST_LENGTH];
    unsigned char  dateregionkey[SHA256_DIGEST_LENGTH];
    unsigned char  dateregionservicekey[SHA256_DIGEST_LENGTH];
    int            ret             = 0; /* return value of snprintf */
    herr_t         ret_value       = SUCCEED;



    FUNC_ENTER_NOAPI_NOINIT

#if S3COMMS_DEBUG
    HDfprintf(stdout, "called H5FD_s3comms_signing_key.\n");
#endif

    if (md == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, 
                    "Destination `md` cannot be NULL.\n")
    }
    if (secret == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, 
                    "`secret` cannot be NULL.\n")
    }
    if (region == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, 
                    "`region` cannot be NULL.\n")
    }
    if (iso8601now == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, 
                    "`iso8601now` cannot be NULL.\n")
    }

    AWS4_secret_len = 4 + strlen(secret) + 1;
    AWS4_secret = (char*)H5MM_malloc(sizeof(char *) * AWS4_secret_len);
    if (AWS4_secret == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, 
                    "Could not allocate space.\n")
    }

    /* prepend "AWS4" to start of the secret key 
     */
    ret = snprintf(AWS4_secret, AWS4_secret_len,"%s%s", "AWS4", secret);
    HDassert( ret > 0 );
    HDassert( (AWS4_secret_len - 1) == (size_t)ret );

    /* hash_func, key, len(key), msg, len(msg), digest_dest, digest_len_dest
     * we know digest length, so ignore via NULL
     */
    HMAC(EVP_sha256(),
         (const unsigned char *)AWS4_secret,
         (int)strlen(AWS4_secret),
         (const unsigned char*)iso8601now,
         8, /* 8 --> length of 8 --> "yyyyMMDD"  */
         datekey,
         NULL);
    HMAC(EVP_sha256(),
         (const unsigned char *)datekey,
         SHA256_DIGEST_LENGTH,
         (const unsigned char *)region,
         strlen(region),
         dateregionkey,
         NULL);
    HMAC(EVP_sha256(),
         (const unsigned char *)dateregionkey, 
         SHA256_DIGEST_LENGTH,
         (const unsigned char *)"s3",
         2,
         dateregionservicekey,
         NULL);
    HMAC(EVP_sha256(),
         (const unsigned char *)dateregionservicekey,
         SHA256_DIGEST_LENGTH,
         (const unsigned char *)"aws4_request",
         12,
         md,
         NULL);

done:
    H5MM_xfree(AWS4_secret);
    FUNC_LEAVE_NOAPI(ret_value);

} /* H5FD_s3comms_signing_key */


/*----------------------------------------------------------------------------
 *
 * Function: H5FD_s3comms_tostringtosign()
 *
 * Purpose:
 *
 *     Get AWS "String to Sign" from Canonical Request, timestamp, 
 *     and AWS "region".
 *
 *     Common bewteen single request and "chunked upload",
 *     conforms to:
 *         "AWS4-HMAC-SHA256\n" +
 *         <ISO8601 date format> + "\n" +  // yyyyMMDD'T'hhmmss'Z'
 *         <yyyyMMDD> + "/" + <AWS Region> + "/s3/aws4-request\n" +
 *         hex(SHA256(<CANONICAL-REQUEST>))
 *
 *     Inputs `creq` (canonical request string), `now` (ISO8601 format),
 *     and `region` (s3 region designator string) must all be
 *     null-terminated strings.
 *
 *     Result is written to `dest` with null-terminator.
 *     It is left to programmer to ensure `dest` has adequate space.
 *
 * Return:
 *
 *     - SUCCESS: `SUCCEED`
 *         - "string to sign" writtn to `dest` and null-terminated
 *     - FAILURE: `FAIL`
 *         - if any of the inputs are NULL
 *         - if an error is encountered while computing checkshum
 *
 * Programmer: Jacob Smith
 *             2017-07-??
 *
 * Changes: 
 *
 *     - Integrate with HDF5.
 *     - Rename from `tostringtosign` to `H5FD_s3comms_tostringtosign`.
 *     - Return `herr_t` instead of characters written.
 *     - Use HDF-friendly bytes-to-hex function (`H5FD_s3comms_bytes_to_hex`)
 *       instead of general-purpose, deprecated `hex()`.
 *     - Adjust casts to openssl's `SHA256`.
 *     - Input strings are now `const`.
 *     --- Jacob Smith 2017-09-19
 *
 *----------------------------------------------------------------------------
 */
herr_t
H5FD_s3comms_tostringtosign(char       *dest,
                            const char *req,
                            const char *now,
                            const char *region)
{
    unsigned char checksum[SHA256_DIGEST_LENGTH * 2 + 1];
    size_t        d         = 0;
    char          day[9];
    char          hexsum[SHA256_DIGEST_LENGTH * 2 + 1];
    size_t        i         = 0;
    int           ret       = 0; /* snprintf return value */
    herr_t        ret_value = SUCCEED;
    char          tmp[128];



    FUNC_ENTER_NOAPI_NOINIT

#if S3COMMS_DEBUG
    HDfprintf(stdout, "called H5FD_s3comms_tostringtosign.\n");
#endif

    HDassert( dest != NULL );

    if (req == NULL)  {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, 
                    "canonical request cannot be null.\n")
    }
    if (now == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, 
                    "Timestring cannot be NULL.\n")
    }
    if (region == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, 
                    "Region cannot be NULL.\n")
    }



    for (i = 0; i < 128; i++) {
        tmp[i] = '\0';
    }
    for (i = 0; i < SHA256_DIGEST_LENGTH * 2 + 1; i++) {
        checksum[i] = '\0';
        hexsum[i] = '\0';
    }
    strncpy(day, now, 8);
    day[8] = '\0';
    ret = snprintf(tmp, 127, "%s/%s/s3/aws4_request", day, region);
    HDassert( ret > 0 );
    HDassert( 127 > ret ); /* size of tmp buffer space */



    HDmemcpy((dest + d), "AWS4-HMAC-SHA256\n", 17);
    d = 17;

    HDmemcpy((dest+d), now, strlen(now));
    d += strlen(now);
    dest[d++] = '\n';

    HDmemcpy((dest + d), tmp, strlen(tmp));
    d += strlen(tmp);
    dest[d++] = '\n';

    SHA256((const unsigned char *)req, 
           strlen(req), 
           checksum);

    if (FAIL == 
        H5FD_s3comms_bytes_to_hex(hexsum,
                                  (const unsigned char *)checksum,
                                  SHA256_DIGEST_LENGTH,
                                  true))
    {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "could not create hex string");
    }

    for (i = 0; i < SHA256_DIGEST_LENGTH * 2; i++) {
        dest[d++] = hexsum[i];
    }

    dest[d] = '\0';

done:
    FUNC_LEAVE_NOAPI(ret_value)

} /* H5ros3_tostringtosign */


/*----------------------------------------------------------------------------
 *
 * Function: H5FD_s3comms_trim()
 *
 * Purpose:
 *
 *     Remove all whitespace characters from start and end of a string `s`
 *     of length `s_len`, writing trimmed string copy to `dest`. 
 *     Stores number of characters remaining at `n_written`.
 *
 *     Destination for trimmed copy `dest` cannot be null.
 *     `dest` must have adequate space allocated for trimmed copy.
 *         If inadequate space, behavior is undefined, possibly resulting
 *         in segfault or overwrite of other data.
 *
 *     If `s` is NULL or all whitespace, `dest` is untouched and `n_written`
 *     is set to 0.
 *
 * Return:
 *
 *     - SUCCESS: `SUCCEED`
 *     - FAILURE: `FAIL`
 *         - `dest == NULL`
 *
 * Programmer: Jacob Smith
 *             2017-09-18
 *
 * Changes: 
 *
 *     - Rename from `trim()` to `H5FD_s3comms_trim()`.
 *     - Incorporate into HDF5.
 *     - Returns `herr_t` type.
 *     --- Jacob Smith 2017-??-??
 *
 *----------------------------------------------------------------------------
 */
herr_t 
H5FD_s3comms_trim(char   *dest,
                  char   *s,
                  size_t  s_len,
                  size_t *n_written)
{
    herr_t               ret_value = SUCCEED;



    FUNC_ENTER_NOAPI_NOINIT

#if S3COMMS_DEBUG
    HDfprintf(stdout, "called H5FD_s3comms_trim.\n");
#endif

    if (dest == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, 
                    "destination cannot be null.")
    }
    if (s == NULL) {
        s_len = 0;
    }



    if (s_len > 0) {
        /* Find first non-whitespace character from start;
         * reduce total length per character.
         */
        while ((s_len > 0) &&
               isspace((unsigned char)s[0]) && s_len > 0) 
        {
             s++;
             s_len--;
        }

        /* Find first non-whitespace chracter from tail;
         * reduce length per-character.
         * If length is 0 already, there is no non-whitespace character.
         */
        if (s_len > 0) {
            do {
                s_len--;
            } while( isspace((unsigned char)s[s_len]) );
            s_len++;

            /* write output into dest 
             */
            HDmemcpy(dest, s, s_len);
        }
    }

    *n_written = s_len;

done:
    FUNC_LEAVE_NOAPI(ret_value)

} /* H5FD_s3comms_trim */


/*----------------------------------------------------------------------------
 *
 * Function: H5FD_s3comms_uriencode()
 *
 * Purpose:
 *
 *     URIencode (percent-encode) every byte except "[a-zA-Z0-9]-._~".
 *
 *     For each charcter in souce string `_s` from `s[0]` to `s[s_len-1]`,
 *     writes to `dest` either the raw character or its percent-encoded
 *     equivalent.
 *
 *     See `H5FD_s3comms_bytes_to_hex` for information on percent-encoding.
 *
 *     Space (' ') character encoded as "%20" (not "+")
 *
 *     Forward-slash ('/') encoded as "%2F" only when `encode_slash == true`.
 *
 *     Records number of characters written at `n_written`.
 *
 *     Assumes that `dest` has been allocated with enough space.
 *
 *     Neither `dest` nor `s` can be NULL.
 *
 *     `s_len == 0` will have no effect.
 *
 * Return:
 *
 *     - SUCCESS: `SUCCEED`
 *     - FAILURE: `FAIL`
 *         - source strings `s` or destination `dest` are NULL
 *         - error while attempting to percent-encode a character
 *
 * Programmer: Jacob Smith
 *             2017-07-??
 *
 * Changes:
 *
 *     - Integrate to HDF environment.
 *     - Rename from `uriencode` to `H5FD_s3comms_uriencode`.
 *     - Change return from characters written to herr_t;
 *       move to i/o parameter `n_written`.
 *     - No longer append null-terminator to string;
 *       programmer may append or not as appropriate upon return.
 *     --- Jacob Smith 2017-09-15
 *
 *----------------------------------------------------------------------------
 */
herr_t 
H5FD_s3comms_uriencode(char       *dest,
                       const char *s,
                       size_t      s_len,
                       hbool_t     encode_slash,
                       size_t     *n_written)
{
    char   c         = 0;
    size_t dest_off  = 0;
    char   hex_buffer[13]; 
    size_t hex_off   = 0; 
    size_t hex_len   = 0; 
    herr_t ret_value = SUCCEED;
    size_t s_off     = 0;



    FUNC_ENTER_NOAPI_NOINIT

#if S3COMMS_DEBUG
    HDfprintf(stdout, "H5FD_s3comms_uriencode called.\n");
#endif

    if (s == NULL) 
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "source string cannot be NULL");
    if (dest == NULL) 
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                    "destination cannot be NULL");

    /* Write characters to destination, converting to percent-encoded
     * "hex-utf-8" strings if necessary.
     * e.g., '$' -> "%24"
     */
    for (s_off = 0; s_off < s_len; s_off++) {
        c = s[s_off];
        if (isalnum(c) ||
            c == '.'   ||
            c == '-'   ||
            c == '_'   ||
            c == '~'   ||
            (c == '/' && encode_slash == FALSE))
        {
            dest[dest_off++] = c;
        } else {
            hex_off = 0;
            if (FAIL == 
                H5FD_s3comms_percent_encode_char(hex_buffer,
                                                 (const unsigned char)c,
                                                 &hex_len))
            {
                hex_buffer[0] = c;
                hex_buffer[1] = 0;
                HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL,
                            "unable to percent-encode character \'%s\' "
                            "at %d in \"%s\"", hex_buffer, (int)s_off, s);
            }

            for (hex_off = 0; hex_off < hex_len; hex_off++) {
                dest[dest_off++] = hex_buffer[hex_off];
            }
        }
    }

    HDassert( dest_off >= s_len );

    *n_written = dest_off;

done:
    FUNC_LEAVE_NOAPI(ret_value)

} /* H5FD_s3comms_uriencode */

