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

/*
 * Purpose: unit-test functionality of the routines in `tools/lib/h5tools_utils`
 *
 * Jacob Smith 2017-11-10
 */

#include "H5private.h"
#include "h5tools_utils.h"
/* #include "h5test.h" */ /* linking failure */

#ifndef _H5TEST_

#define AT() fprintf(stdout, "   at %s:%d in %s()...\n",        \
                     __FILE__, __LINE__, FUNC);

#define FAILED(msg) {                 \
    fprintf(stdout, "*FAILED*"); AT() \
    if (msg == NULL) {                \
        fprintf(stdout,"(NULL)\n");   \
    } else {                          \
        fprintf(stdout, "%s\n", msg); \
    }                                 \
    fflush(stdout);                   \
}

#define TESTING(msg) {                       \
    fprintf(stdout, "TESTING %-62s", (msg)); \
    fflush(stdout);                          \
}

#define PASSED() {                \
    fprintf(stdout, " PASSED\n"); \
    fflush(stdout);               \
}

#endif /* ifndef _H5TEST_ */

#ifndef __js_test__

#define __js_test__ 1L

/*****************************************************************************
 *
 * FILE-LOCAL TESTING MACROS
 *
 * Purpose:
 *
 *     1. Upon test failure, goto-jump to single-location teardown in test 
 *        function. E.g., `error:` (consistency with HDF corpus) or
 *        `failed:` (reflects purpose).
 *            >>> using "error", in part because `H5E_BEGIN_TRY` expects it.
 *     2. Increase clarity and reduce overhead found with `TEST_ERROR`.
 *        e.g., "if(somefunction(arg, arg2) < 0) TEST_ERROR:"
 *        requires reading of entire line to know whether this if/call is
 *        part of the test setup, test operation, or a test unto itself.
 *     3. Provide testing macros with optional user-supplied failure message;
 *        if not supplied (NULL), generate comparison output in the spirit of 
 *        test-driven development. E.g., "expected 5 but was -3"
 *        User messages clarify test's purpose in code, encouraging description
 *        without relying on comments.
 *     4. Configurable expected-actual order in generated comparison strings.
 *        Some prefer `VERIFY(expected, actual)`, others 
 *        `VERIFY(actual, expected)`. Provide preprocessor ifdef switch
 *        to satifsy both parties, assuming one paradigm per test file.
 *        (One could #undef and redefine the flag through the file as desired,
 *         but _why_.)
 *
 *     Provided as courtesy, per consideration for inclusion in the library 
 *     proper.
 *
 *     Macros:
 * 
 *         JSVERIFY_EXP_ACT - ifdef flag, configures comparison order
 *         FAIL_IF()        - check condition
 *         FAIL_UNLESS()    - check _not_ condition
 *         JSVERIFY()       - long-int equality check; prints reason/comparison
 *         JSVERIFY_NOT()   - long-int inequality check; prints
 *         JSVERIFY_STR()   - string equality check; prints
 *
 * Programmer: Jacob Smith
 *             2017-10-24
 *
 *****************************************************************************/


/*----------------------------------------------------------------------------
 *
 * ifdef flag: JSVERIFY_EXP_ACT
 * 
 * JSVERIFY macros accept arguments as (EXPECTED, ACTUAL[, reason]) 
 * default, if this is undefined, is (ACTUAL, EXPECTED[, reason])
 *
 *----------------------------------------------------------------------------
 */
#define JSVERIFY_EXP_ACT 1L


/*----------------------------------------------------------------------------
 *
 * Macro: JSFAILED_AT()
 *
 * Purpose:
 *
 *     Preface a test failure by printing "*FAILED*" and location to stdout
 *     Similar to `H5_FAILED(); AT();` from h5test.h
 *
 *     *FAILED* at somefile.c:12 in function_name()...
 *
 * Programmer: Jacob Smith
 *             2017-10-24
 *
 *----------------------------------------------------------------------------
 */
#define JSFAILED_AT() {                                                   \
    HDprintf("*FAILED* at %s:%d in %s()...\n", __FILE__, __LINE__, FUNC); \
}


/*----------------------------------------------------------------------------
 *
 * Macro: FAIL_IF()
 *
 * Purpose:  
 *
 *     Make tests more accessible and less cluttered than
 *         `if (thing == otherthing()) TEST_ERROR` 
 *         paradigm.
 *
 *     The following lines are roughly equivalent:
 *
 *         `if (myfunc() < 0) TEST_ERROR;` (as seen elsewhere in HDF tests)
 *         `FAIL_IF(myfunc() < 0)`
 *
 *     Prints a generic "FAILED AT" line to stdout and jumps to `error`,
 *     similar to `TEST_ERROR` in h5test.h
 *
 * Programmer: Jacob Smith
 *             2017-10-23
 *
 *----------------------------------------------------------------------------
 */
#define FAIL_IF(condition) \
if (condition) {           \
    JSFAILED_AT()          \
    goto error;           \
}


/*----------------------------------------------------------------------------
 *
 * Macro: FAIL_UNLESS()
 *
 * Purpose:
 *
 *     TEST_ERROR wrapper to reduce cognitive overhead from "negative tests",
 *     e.g., "a != b".
 *     
 *     Opposite of FAIL_IF; fails if the given condition is _not_ true.
 *
 *     `FAIL_IF( 5 != my_op() )`
 *     is equivalent to
 *     `FAIL_UNLESS( 5 == my_op() )`
 *     However, `JSVERIFY(5, my_op(), "bad return")` may be even clearer.
 *         (see JSVERIFY)
 *
 * Programmer: Jacob Smith
 *             2017-10-24
 *
 *----------------------------------------------------------------------------
 */
#define FAIL_UNLESS(condition) \
if (!(condition)) {            \
    JSFAILED_AT()              \
    goto error;               \
}


/*----------------------------------------------------------------------------
 *
 * Macro: JSERR_LONG()
 *
 * Purpose:
 *
 *     Print an failure message for long-int arguments.
 *     ERROR-AT printed first.
 *     If `reason` is given, it is printed on own line and newlined after
 *     else, prints "expected/actual" aligned on own lines.
 *
 *     *FAILED* at myfile.c:488 in somefunc()...
 *     forest must be made of trees.
 *
 *     or
 *
 *     *FAILED* at myfile.c:488 in somefunc()...
 *       ! Expected 425
 *       ! Actual   3
 *
 * Programmer: Jacob Smith
 *             2017-10-24
 *
 *----------------------------------------------------------------------------
 */
#define JSERR_LONG(expected, actual, reason) {           \
    JSFAILED_AT()                                        \
    if (reason!= NULL) {                                 \
        HDprintf("%s\n", (reason));                      \
    } else {                                             \
        HDprintf("  ! Expected %ld\n  ! Actual   %ld\n", \
                  (long)(expected), (long)(actual));     \
    }                                                    \
}


/*----------------------------------------------------------------------------
 *
 * Macro: JSERR_STR()
 *
 * Purpose:
 *
 *     Print an failure message for string arguments.
 *     ERROR-AT printed first.
 *     If `reason` is given, it is printed on own line and newlined after
 *     else, prints "expected/actual" aligned on own lines.
 *
 *     *FAILED*  at myfile.c:421 in myfunc()...
 *     Blue and Red strings don't match!
 *
 *     or
 *
 *     *FAILED*  at myfile.c:421 in myfunc()...
 *     !!! Expected:
 *     this is my expected
 *     string
 *     !!! Actual:
 *     not what I expected at all
 *
 * Programmer: Jacob Smith
 *             2017-10-24
 *
 *----------------------------------------------------------------------------
 */
#define JSERR_STR(expected, actual, reason) {           \
    JSFAILED_AT()                                       \
    if ((reason) != NULL) {                             \
        HDprintf("%s\n", (reason));                     \
    } else {                                            \
        HDprintf("!!! Expected:\n%s\n!!!Actual:\n%s\n", \
                 (expected), (actual));                 \
    }                                                   \
}

#ifdef JSVERIFY_EXP_ACT


/*----------------------------------------------------------------------------
 *
 * Macro: JSVERIFY()
 *
 * Purpose: 
 *
 *     Verify that two long integers are equal.
 *     If unequal, print failure message 
 *     (with `reason`, if not NULL; expected/actual if NULL)
 *     and jump to `error` at end of function
 *
 * Programmer: Jacob Smith
 *             2017-10-24
 *
 *----------------------------------------------------------------------------
 */
#define JSVERIFY(expected, actual, reason)     \
if ((long)(actual) != (long)(expected)) {      \
    JSERR_LONG((expected), (actual), (reason)) \
    goto error;                                \
} /* JSVERIFY */


/*----------------------------------------------------------------------------
 *
 * Macro: JSVERIFY_NOT()
 *
 * Purpose: 
 *
 *     Verify that two long integers are _not_ equal.
 *     If equal, print failure message 
 *     (with `reason`, if not NULL; expected/actual if NULL)
 *     and jump to `error` at end of function
 *
 * Programmer: Jacob Smith
 *             2017-10-24
 *
 *----------------------------------------------------------------------------
 */
#define JSVERIFY_NOT(expected, actual, reason) \
if ((long)(actual) == (long)(expected)) {      \
    JSERR_LONG((expected), (actual), (reason)) \
    goto error;                                \
} /* JSVERIFY_NOT */


/*----------------------------------------------------------------------------
 *
 * Macro: JSVERIFY_STR()
 *
 * Purpose: 
 *
 *     Verify that two strings are equal.
 *     If unequal, print failure message 
 *     (with `reason`, if not NULL; expected/actual if NULL)
 *     and jump to `error` at end of function
 *
 * Programmer: Jacob Smith
 *             2017-10-24
 *
 *----------------------------------------------------------------------------
 */
#define JSVERIFY_STR(expected, actual, reason) \
if (strcmp((actual), (expected)) != 0) {       \
    JSERR_STR((expected), (actual), (reason)); \
    goto error;                                \
} /* JSVERIFY_STR */


#else /* JSVERIFY_EXP_ACT not defined                                        */
      /* Repeats macros above, but with actual/expected parameters reversed. */


/*----------------------------------------------------------------------------
 * Macro: JSVERIFY()
 * See: JSVERIFY documentation above.
 * Programmer: Jacob Smith
 *             2017-10-14
 *----------------------------------------------------------------------------
 */
#define JSVERIFY(actual, expected, reason)      \
if ((long)(actual) != (long)(expected)) {       \
    JSERR_LONG((expected), (actual), (reason)); \
    goto error;                                 \
} /* JSVERIFY */


/*----------------------------------------------------------------------------
 * Macro: JSVERIFY_NOT()
 * See: JSVERIFY_NOT documentation above.
 * Programmer: Jacob Smith
 *             2017-10-14
 *----------------------------------------------------------------------------
 */
#define JSVERIFY_NOT(actual, expected, reason) \
if ((long)(actual) == (long)(expected)) {      \
    JSERR_LONG((expected), (actual), (reason)) \
    goto error;                                \
} /* JSVERIFY_NOT */


/*----------------------------------------------------------------------------
 * Macro: JSVERIFY_STR()
 * See: JSVERIFY_STR documentation above.
 * Programmer: Jacob Smith
 *             2017-10-14
 *----------------------------------------------------------------------------
 */
#define JSVERIFY_STR(actual, expected, reason) \
if (strcmp((actual), (expected)) != 0) {       \
    JSERR_STR((expected), (actual), (reason)); \
    goto error;                                \
} /* JSVERIFY_STR */

#endif /* ifdef/else JSVERIFY_EXP_ACT */

#endif /* __js_test__ */

/* if > 0, be very verbose when performing tests */
#define H5TOOLS_UTILS_TEST_DEBUG 0

/******************/
/* TEST FUNCTIONS */
/******************/


/*----------------------------------------------------------------------------
 *
 * Function: test_parse_tuple()
 *
 * Purpose: 
 *
 *     Provide unit tests and specification for the `parse_tuple()` function.
 *
 * Return:
 *
 *     0   Tests passed.
 *     1   Tests failed.
 *
 * Programmer: Jacob Smith
 *             2017-11-11
 *
 * Changes: None.
 *
 *----------------------------------------------------------------------------
 */
static unsigned
test_parse_tuple(void)
{
    /*************************
     * TEST-LOCAL STRUCTURES *
     *************************/

    struct testcase {
        const char *test_msg;     /* info about test case */
        const char *in_str;       /* input string */
        int         sep;          /* separator "character" */
        herr_t      exp_ret;      /* expected SUCCEED / FAIL */
        unsigned    exp_nelems;   /* expected number of elements */
                                  /* (no more than 7!)           */
        const char *exp_elems[7]; /* list of elements (no more than 7!) */
    };

    /******************
     * TEST VARIABLES *
     ******************/

    struct testcase cases[] = {
        {   "bad start",
            "words(before)",
            ';',
            FAIL,
            0,
            {NULL},
        },
        {   "tuple not closed",
            "(not ok",
            ',',
            FAIL,
            0,
            {NULL},
        },
        {   "empty tuple",
            "()",
            '-',
            SUCCEED,
            1,
            {""},
        },
        {   "no separator",
            "(stuff keeps on going)",
            ',',
            SUCCEED,
            1,
            {"stuff keeps on going"},
        },
        {   "4-ple, escaped seperator",
            "(elem0,elem1,el\\,em2,elem3)", /* "el\,em" */
            ',',
            SUCCEED,
            4,
            {"elem0", "elem1", "el,em2", "elem3"},
        },
        {   "5-ple, escaped escaped separator",
            "(elem0,elem1,el\\\\,em2,elem3)",
            ',',
            SUCCEED,
            5,
            {"elem0", "elem1", "el\\", "em2", "elem3"},
        },
        {   "escaped non-comma separator",
            "(5-2-7-2\\-6-2)",
            '-',
            SUCCEED,
            5,
            {"5","2","7","2-6","2"},
        },
        {   "embedded close-paren",
            "(be;fo)re)",
            ';',
            SUCCEED,
            2,
            {"be", "fo)re"},
        },
        {   "embedded non-escaping backslash",
            "(be;fo\\re)",
            ';',
            SUCCEED,
            2,
            {"be", "fo\\re"},
        },
        {   "double close-paren at end",
            "(be;fore))",
            ';',
            SUCCEED,
            2,
            {"be", "fore)"},
        },
        {   "empty elements",
            "(;a1;;a4;)",
            ';',
            SUCCEED,
            5,
            {"", "a1", "", "a4", ""},
        },
        {   "nested tuples with different separators",
            "((4,e,a);(6,2,a))",
            ';',
            SUCCEED,
            2,
            {"(4,e,a)","(6,2,a)"},
        },
        {   "nested tuples with same separators",
            "((4,e,a),(6,2,a))",
            ',',
            SUCCEED,
            6,
            {"(4","e","a)","(6","2","a)"},
        },
    };
    struct testcase   tc;
    unsigned          n_tests       = 13;
    unsigned          i             = 0;
    unsigned          count         = 0;
    unsigned          elem_i        = 0;
    char            **parsed        = NULL;
    char             *cpy           = NULL;
    herr_t            success       = TRUE;
    hbool_t           show_progress = FALSE;



    TESTING("arbitrary-count tuple parsing");

#if H5TOOLS_UTILS_TEST_DEBUG > 0
        show_progress = TRUE;
#endif

    /*********
     * TESTS *
     *********/

    for (i = 0; i < n_tests; i++) {

        /* SETUP
         */
        HDassert(parsed == NULL);
        HDassert(cpy == NULL);
        tc = cases[i];
        if (show_progress == TRUE) {
            printf("testing %d: %s...\n", i, tc.test_msg);
        }

        /* VERIFY
         */
        success = parse_tuple(tc.in_str, tc.sep,
                              &cpy, &count, &parsed);

        JSVERIFY( tc.exp_ret,    success, "function returned incorrect value" )
        JSVERIFY( tc.exp_nelems, count,   NULL )
        if (success == SUCCEED) {
            FAIL_IF( parsed == NULL )
            for (elem_i = 0; elem_i < count; elem_i++) {
                JSVERIFY_STR( tc.exp_elems[elem_i], parsed[elem_i], NULL )
            }
            /* TEARDOWN */
            HDassert(parsed != NULL);
            HDassert(cpy    != NULL);
            free(parsed);
            parsed = NULL;
            free(cpy);
            cpy = NULL;
        } else {
            FAIL_IF( parsed != NULL )
        } /* if parse_tuple() == SUCCEED or no */

    } /* for each testcase */

    PASSED();
    return 0;

error:
    /***********
     * CLEANUP *
     ***********/

    if (parsed != NULL) free(parsed);
    if (cpy    != NULL) free(cpy);

    return 1;

} /* test_parse_tuple */


/*----------------------------------------------------------------------------
 *
 * Function:   test_populate_ros3_fa()
 *
 * Purpose:    Verify behavior of `populate_ros3_fa()`
 *
 * Return:     0 if test passes
 *             1 if failure
 *
 * Programmer: Jacob Smith
 *             2017-11-13
 *
 * Changes:    None
 *
 *----------------------------------------------------------------------------
 */
static unsigned
test_populate_ros3_fa(void)
{
    /*************************
     * TEST-LOCAL STRUCTURES *
     *************************/

    /************************
     * TEST-LOCAL VARIABLES *
     ************************/

    hbool_t show_progress = FALSE;
    int     bad_version   = 0xf87a;



    TESTING("programmatic fapl population");

#if H5TOOLS_UTILS_TEST_DEBUG > 0
    show_progress = TRUE;
#endif

    bad_version |= H5FD__CURR_ROS3_FAPL_T_VERSION;
    HDassert(bad_version != H5FD__CURR_ROS3_FAPL_T_VERSION);

    /*********
     * TESTS *
     *********/

    /* NULL fapl config pointer fails
     */
    {
        const char *values[] = {"x", "y", "z"};

        if (show_progress) { HDprintf("NULL fapl pointer\n"); }

        JSVERIFY( 0, h5tools_populate_ros3_fapl(NULL, values),
                  "fapl pointer cannot be null" )
    }

    /* NULL values pointer yields default fapl
     */
    {
        H5FD_ros3_fapl_t fa = {bad_version, TRUE, "u", "v", "w"};

        if (show_progress) { HDprintf("NULL values pointer\n"); }

        JSVERIFY( 1, h5tools_populate_ros3_fapl(&fa, NULL),
                  "NULL values pointer yields \"default\" fapl" )
        JSVERIFY( H5FD__CURR_ROS3_FAPL_T_VERSION, fa.version, NULL )
        JSVERIFY( FALSE, fa.authenticate, NULL )
        JSVERIFY_STR( "", fa.aws_region, NULL )
        JSVERIFY_STR( "", fa.secret_id,  NULL )
        JSVERIFY_STR( "", fa.secret_key, NULL )
    }

    /* all-empty values 
     * yields default fapl
     */
    {
        H5FD_ros3_fapl_t fa = {bad_version, TRUE, "u", "v", "w"};
        const char *values[] = {"", "", ""};

        if (show_progress) { HDprintf("all empty values\n"); }

        JSVERIFY( 1, h5tools_populate_ros3_fapl(&fa, values),
                  "empty values yields \"default\" fapl" )
        JSVERIFY( H5FD__CURR_ROS3_FAPL_T_VERSION, fa.version, NULL )
        JSVERIFY( FALSE, fa.authenticate, NULL )
        JSVERIFY_STR( "", fa.aws_region, NULL )
        JSVERIFY_STR( "", fa.secret_id,  NULL )
        JSVERIFY_STR( "", fa.secret_key, NULL )
    }

    /* successfully set fapl with values 
     * excess value is ignored
     */
    {
        H5FD_ros3_fapl_t fa = {bad_version, FALSE, "a", "b", "c"};
        const char *values[] = {"x", "y", "z", "a"};

        if (show_progress) { HDprintf("successful full set\n"); }

        JSVERIFY( 1, h5tools_populate_ros3_fapl(&fa, values),
                  "four values" )
        JSVERIFY( H5FD__CURR_ROS3_FAPL_T_VERSION, fa.version, NULL )
        JSVERIFY( TRUE, fa.authenticate, NULL )
        JSVERIFY_STR( "x", fa.aws_region, NULL )
        JSVERIFY_STR( "y", fa.secret_id, NULL )
        JSVERIFY_STR( "z", fa.secret_key,  NULL )
    }

    /* NULL region
     * yeilds default fapl
     */
    {
        H5FD_ros3_fapl_t fa = {bad_version, FALSE, "a", "b", "c"};
        const char *values[] = {NULL, "y", "z", NULL};

        if (show_progress) { HDprintf("NULL region\n"); }

        JSVERIFY( 0, h5tools_populate_ros3_fapl(&fa, values),
                  "could not fill fapl" )
        JSVERIFY( H5FD__CURR_ROS3_FAPL_T_VERSION, fa.version, NULL )
        JSVERIFY( FALSE, fa.authenticate, NULL )
        JSVERIFY_STR( "", fa.aws_region, NULL )
        JSVERIFY_STR( "", fa.secret_id, NULL )
        JSVERIFY_STR( "", fa.secret_key,  NULL )
    }

    /* empty region
     * yeilds default fapl
     */
    {
        H5FD_ros3_fapl_t fa = {bad_version, FALSE, "a", "b", "c"};
        const char *values[] = {"", "y", "z", NULL};

        if (show_progress) { HDprintf("empty region; non-empty id, key\n"); }

        JSVERIFY( 0, h5tools_populate_ros3_fapl(&fa, values),
                  "could not fill fapl" )
        JSVERIFY( H5FD__CURR_ROS3_FAPL_T_VERSION, fa.version, NULL )
        JSVERIFY( FALSE, fa.authenticate, NULL )
        JSVERIFY_STR( "", fa.aws_region, NULL )
        JSVERIFY_STR( "", fa.secret_id, NULL )
        JSVERIFY_STR( "", fa.secret_key,  NULL )
    }

    /* region overflow
     * yeilds default fapl
     */
    {
        H5FD_ros3_fapl_t fa = {bad_version, FALSE, "a", "b", "c"};
        const char *values[] = {
            "somewhere over the rainbow not too high "           \
            "there is another rainbow bounding some darkened sky",
            "y",
            "z"};

        if (show_progress) { HDprintf("region overflow\n"); }

        HDassert(strlen(values[0]) > H5FD__ROS3_MAX_REGION_LEN);

        JSVERIFY( 0, h5tools_populate_ros3_fapl(&fa, values),
                  "could not fill fapl" )
        JSVERIFY( H5FD__CURR_ROS3_FAPL_T_VERSION, fa.version, NULL )
        JSVERIFY( FALSE, fa.authenticate, NULL )
        JSVERIFY_STR( "", fa.aws_region, NULL )
        JSVERIFY_STR( "", fa.secret_id, NULL )
        JSVERIFY_STR( "", fa.secret_key,  NULL )
    }

    /* NULL id
     * yields default fapl
     */
    {
        H5FD_ros3_fapl_t fa = {bad_version, FALSE, "a", "b", "c"};
        const char *values[] = {"x", NULL, "z", NULL};

        if (show_progress) { HDprintf("NULL id\n"); }

        JSVERIFY( 0, h5tools_populate_ros3_fapl(&fa, values),
                  "could not fill fapl" )
        JSVERIFY( H5FD__CURR_ROS3_FAPL_T_VERSION, fa.version, NULL )
        JSVERIFY( FALSE, fa.authenticate, NULL )
        JSVERIFY_STR( "", fa.aws_region, NULL )
        JSVERIFY_STR( "", fa.secret_id, NULL )
        JSVERIFY_STR( "", fa.secret_key,  NULL )
    }

    /* empty id (non-empty region, key)
     * yeilds default fapl
     */
    {
        H5FD_ros3_fapl_t fa = {bad_version, FALSE, "a", "b", "c"};
        const char *values[] = {"x", "", "z", NULL};

        if (show_progress) { HDprintf("empty id; non-empty region and key\n"); }

        JSVERIFY( 0, h5tools_populate_ros3_fapl(&fa, values),
                  "could not fill fapl" )
        JSVERIFY( H5FD__CURR_ROS3_FAPL_T_VERSION, fa.version, NULL )
        JSVERIFY( FALSE, fa.authenticate, NULL )
        JSVERIFY_STR( "", fa.aws_region, NULL )
        JSVERIFY_STR( "", fa.secret_id, NULL )
        JSVERIFY_STR( "", fa.secret_key,  NULL )
    }

    /* id overflow
     * partial set: region
     */
    {
        H5FD_ros3_fapl_t fa = {bad_version, FALSE, "a", "b", "c"};
        const char *values[] = {
            "x",
            "Why is it necessary to solve the problem? "                     \
            "What benefits will you receive by solving the problem? "        \
            "What is the unknown? "                                          \
            "What is it you don't yet understand? "                          \
            "What is the information you have? "                             \
            "What isn't the problem? "                                       \
            "Is the information insufficient, redundant, or contradictory? " \
            "Should you draw a diagram or figure of the problem? "           \
            "What are the boundaries of the problem? "                       \
            "Can you separate the various parts of the problem?",
            "z"};

        if (show_progress) { HDprintf("id overflow\n"); }

        HDassert(strlen(values[1]) > H5FD__ROS3_MAX_SECRET_ID_LEN);

        JSVERIFY( 0, h5tools_populate_ros3_fapl(&fa, values),
                  "could not fill fapl" )
        JSVERIFY( H5FD__CURR_ROS3_FAPL_T_VERSION, fa.version, NULL )
        JSVERIFY( FALSE, fa.authenticate, NULL )
        JSVERIFY_STR( "x", fa.aws_region, NULL )
        JSVERIFY_STR( "", fa.secret_id, NULL )
        JSVERIFY_STR( "", fa.secret_key,  NULL )
    }

    /* NULL key
     * yields default fapl
     */
    {
        H5FD_ros3_fapl_t fa = {bad_version, FALSE, "a", "b", "c"};
        const char *values[] = {"x", "y", NULL, NULL};

        if (show_progress) { HDprintf("NULL key\n"); }

        JSVERIFY( 0, h5tools_populate_ros3_fapl(&fa, values),
                  "could not fill fapl" )
        JSVERIFY( H5FD__CURR_ROS3_FAPL_T_VERSION, fa.version, NULL )
        JSVERIFY( FALSE, fa.authenticate, NULL )
        JSVERIFY_STR( "", fa.aws_region, NULL )
        JSVERIFY_STR( "", fa.secret_id, NULL )
        JSVERIFY_STR( "", fa.secret_key,  NULL )
    }

    /* empty key (non-empty region, id)
     * yeilds authenticating fapl
     */
    {
        H5FD_ros3_fapl_t fa = {bad_version, FALSE, "a", "b", "c"};
        const char *values[] = {"x", "y", "", NULL};

        if (show_progress) { HDprintf("empty key; non-empty region and id\n"); }

        JSVERIFY( 1, h5tools_populate_ros3_fapl(&fa, values),
                  "could not fill fapl" )
        JSVERIFY( H5FD__CURR_ROS3_FAPL_T_VERSION, fa.version, NULL )
        JSVERIFY( TRUE, fa.authenticate, NULL )
        JSVERIFY_STR( "x", fa.aws_region, NULL )
        JSVERIFY_STR( "y", fa.secret_id, NULL )
        JSVERIFY_STR( "", fa.secret_key,  NULL )
    }

    /* empty key, region (non-empty id)
     * yeilds default fapl
     */
    {
        H5FD_ros3_fapl_t fa = {bad_version, FALSE, "a", "b", "c"};
        const char *values[] = {"", "y", "", NULL};

        if (show_progress) { HDprintf("empty key and region; non-empty id\n"); }

        JSVERIFY( 0, h5tools_populate_ros3_fapl(&fa, values),
                  "could not fill fapl" )
        JSVERIFY( H5FD__CURR_ROS3_FAPL_T_VERSION, fa.version, NULL )
        JSVERIFY( FALSE, fa.authenticate, NULL )
        JSVERIFY_STR( "", fa.aws_region, NULL )
        JSVERIFY_STR( "", fa.secret_id, NULL )
        JSVERIFY_STR( "", fa.secret_key,  NULL )
    }

    /* empty key, id (non-empty region)
     * yeilds default fapl
     */
    {
        H5FD_ros3_fapl_t fa = {bad_version, FALSE, "a", "b", "c"};
        const char *values[] = {"x", "", "", NULL};

        if (show_progress) { HDprintf("empty key and id; non-empty region\n"); }

        JSVERIFY( 0, h5tools_populate_ros3_fapl(&fa, values),
                  "could not fill fapl" )
        JSVERIFY( H5FD__CURR_ROS3_FAPL_T_VERSION, fa.version, NULL )
        JSVERIFY( FALSE, fa.authenticate, NULL )
        JSVERIFY_STR( "", fa.aws_region, NULL )
        JSVERIFY_STR( "", fa.secret_id, NULL )
        JSVERIFY_STR( "", fa.secret_key,  NULL )
    }

    /* key overflow
     * partial set: region, id
     */
    {
        H5FD_ros3_fapl_t fa = {bad_version, FALSE, "a", "b", "c"};
        const char *values[] = {
            "x",
            "y",
            "Why is it necessary to solve the problem? "                     \
            "What benefits will you receive by solving the problem? "        \
            "What is the unknown? "                                          \
            "What is it you don't yet understand? "                          \
            "What is the information you have? "                             \
            "What isn't the problem? "                                       \
            "Is the information insufficient, redundant, or contradictory? " \
            "Should you draw a diagram or figure of the problem? "           \
            "What are the boundaries of the problem? "                       \
            "Can you separate the various parts of the problem?"};

        if (show_progress) { HDprintf("key overflow\n"); }

        HDassert(strlen(values[2]) > H5FD__ROS3_MAX_SECRET_KEY_LEN);

        JSVERIFY( 0, h5tools_populate_ros3_fapl(&fa, values),
                  "could not fill fapl" )
        JSVERIFY( H5FD__CURR_ROS3_FAPL_T_VERSION, fa.version, NULL )
        JSVERIFY( FALSE, fa.authenticate, NULL )
        JSVERIFY_STR( "x", fa.aws_region, NULL )
        JSVERIFY_STR( "y", fa.secret_id, NULL )
        JSVERIFY_STR( "", fa.secret_key,  NULL )
    }

    PASSED();
    return 0;

error :
    /***********
     * CLEANUP *
     ***********/

    return 1;

} /* test_populate_ros3_fa */


/*----------------------------------------------------------------------------
 *
 * Function:   main()
 *
 * Purpose:    Run all test functions.
 *
 * Return:     0 iff all test pass
 *             1 iff any failures
 *
 * Programmer: Jacob Smith
 *             2017-11-10
 *
 * Changes:    None.
 *
 *----------------------------------------------------------------------------
 */
int
main(void)
{
    unsigned nerrors = 0;

#ifdef _H5TEST_
    h5reset(); /* h5test? */
#endif

    HDfprintf(stdout, "Testing h5tools_utils corpus.\n");

    nerrors += test_parse_tuple();
    nerrors += test_populate_ros3_fa();

    if (nerrors > 0) {
        HDfprintf(stdout, "***** %d h5tools_utils TEST%s FAILED! *****\n",
                 nerrors,
                 nerrors > 1 ? "S" : "");
        nerrors = 1;
    } else {
        HDfprintf(stdout, "All h5tools_utils tests passed\n");
    }

    return (int)nerrors;

} /* main */

