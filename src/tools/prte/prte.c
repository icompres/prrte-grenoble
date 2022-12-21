/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2021 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2007-2009 Sun Microsystems, Inc. All rights reserved.
 * Copyright (c) 2007-2017 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Geoffroy Vallee. All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * Copyright (c) 2021      Amazon.com, Inc. or its affiliates.  All Rights
 *                         reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "src/include/constants.h"
#include "src/include/version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_STRINGS_H
#    include <strings.h>
#endif /* HAVE_STRINGS_H */
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#    include <sys/param.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif /* HAVE_SYS_TYPES_H */
#ifdef HAVE_SYS_WAIT_H
#    include <sys/wait.h>
#endif /* HAVE_SYS_WAIT_H */
#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif /* HAVE_SYS_TIME_H */
#include <fcntl.h>
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_POLL_H
#    include <poll.h>
#endif

#include "src/event/event-internal.h"
#include "src/mca/base/base.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/pmix/pmix-internal.h"
#include "src/threads/pmix_mutex.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/prte_cmd_line.h"
#include "src/util/daemon_init.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_os_path.h"
#include "src/util/output.h"
#include "src/util/pmix_path.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_getcwd.h"
#include "src/util/pmix_show_help.h"

#include "src/class/pmix_pointer_array.h"
#include "src/runtime/prte_progress_threads.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/odls/odls.h"
#include "src/mca/plm/plm.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/rml/rml.h"
#include "src/mca/schizo/base/base.h"
#include "src/mca/state/base/base.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"
#include "src/runtime/prte_setop_server.h"

#include "include/prte.h"
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/prted/prted.h"


typedef struct {
    prte_pmix_lock_t lock;
    pmix_status_t status;
    pmix_info_t *info;
    size_t ninfo;
} mylock_t;

pmix_rank_t highest_rank_global = 6; 


static int res_change_cnt = 0;
static size_t cur_alloc_reservation_number = 0;
static pmix_nspace_t spawnednspace;
static pmix_proc_t myproc;
static bool signals_set = false;
static bool forcibly_die = false;
static prte_event_t term_handler;
static prte_event_t epipe_handler;
static int term_pipe[2];
static pmix_mutex_t prun_abort_inprogress_lock = PMIX_MUTEX_STATIC_INIT;
static prte_event_t *forward_signals_events = NULL;
static char *mypidfile = NULL;
static bool verbose = false;
static bool want_prefix_by_default = (bool) PRTE_WANT_PRTE_PREFIX_BY_DEFAULT;
static void abort_signal_callback(int signal);
static void clean_abort(int fd, short flags, void *arg);
static void signal_forward_callback(int fd, short args, void *cbdata);
static void epipe_signal_callback(int fd, short args, void *cbdata);
static int prep_singleton(const char *name);

static void rchandler(size_t evhdlr_registration_id, pmix_status_t status,
                       const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                       pmix_info_t results[], size_t nresults,
                       pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata);
void prte_master_recv(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                      prte_rml_tag_t tag, void *cbdata);

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t *) cbdata;
    PMIX_ACQUIRE_OBJECT(lock);
    PRTE_PMIX_WAKEUP_THREAD(lock);
}

static void setupcbfunc(pmix_status_t status, pmix_info_t info[], size_t ninfo,
                        void *provided_cbdata, pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    mylock_t *mylock = (mylock_t *) provided_cbdata;
    size_t n;

    if (NULL != info) {
        mylock->ninfo = ninfo;
        PMIX_INFO_CREATE(mylock->info, mylock->ninfo);
        /* cycle across the provided info */
        for (n = 0; n < ninfo; n++) {
            PMIX_INFO_XFER(&mylock->info[n], &info[n]);
        }
    } else {
        mylock->info = NULL;
        mylock->ninfo = 0;
    }
    mylock->status = status;

    /* release the caller */
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }

    PRTE_PMIX_WAKEUP_THREAD(&mylock->lock);
}

static void spcbfunc(pmix_status_t status, char nspace[], void *cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t *) cbdata;

    PMIX_ACQUIRE_OBJECT(lock);
    lock->status = status;
    if (PMIX_SUCCESS == status) {
        lock->msg = strdup(nspace);
    }
    PRTE_PMIX_WAKEUP_THREAD(lock);
}

static void parent_died_fn(size_t evhdlr_registration_id, pmix_status_t status,
                           const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                           pmix_info_t results[], size_t nresults,
                           pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    clean_abort(0, 0, NULL);
    cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
}

static void evhandler_reg_callbk(pmix_status_t status, size_t evhandler_ref, void *cbdata)
{
    mylock_t *lock = (mylock_t *) cbdata;

    lock->status = status;
    PRTE_PMIX_WAKEUP_THREAD(&lock->lock);
}


static int wait_pipe[2];

static int wait_dvm(pid_t pid)
{
    char reply;
    int rc;
    int status;

    close(wait_pipe[1]);
    do {
        rc = read(wait_pipe[0], &reply, 1);
    } while (0 > rc && EINTR == errno);

    if (1 == rc && 'K' == reply) {
        return 0;
    } else if (0 == rc) {
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
    }
    return 255;
}

static void setup_sighandler(int signal, prte_event_t *ev, prte_event_cbfunc_t cbfunc)
{
    prte_event_signal_set(prte_event_base, ev, signal, cbfunc, ev);
    prte_event_signal_add(ev, NULL);
}

int main(int argc, char *argv[])
{
    int rc = 1, i, j;
    char *param, *timeoutenv, *ptr, *tpath, *cptr;
    prte_pmix_lock_t lock;
    pmix_list_t apps;
    prte_pmix_app_t *app;
    pmix_info_t *iptr, info;
    pmix_status_t ret;
    bool flag;
    size_t n, ninfo, param_len;
    pmix_app_t *papps;
    size_t napps;
    mylock_t mylock;
    uint32_t ui32;
    char **pargv;
    int pargc;
    prte_job_t *jdata;
    prte_app_context_t *dapp;
    bool proxyrun = false;
    void *jinfo;
    pmix_proc_t pname;
    pmix_value_t *val;
    pmix_data_array_t darray;
    char **hostfiles = NULL;
    char **hosts = NULL;
    prte_schizo_base_module_t *schizo;
    prte_ess_base_signal_t *sig;
    char **targv, **options;
    pmix_status_t code;
    char *personality;
    pmix_cli_result_t results;
    pmix_cli_item_t *opt;

    /* init the globals */
    PMIX_CONSTRUCT(&apps, pmix_list_t);
    if (NULL == (param = getenv("PRTE_BASENAME"))) {
        prte_tool_basename = pmix_basename(argv[0]);
    } else {
        prte_tool_basename = strdup(param);
    }
    if (0 == strcmp(prte_tool_basename, "prterun")) {
        prte_tool_actual = "prterun";
    } else {
        prte_tool_actual = "prte";
    }
    pargc = argc;
    pargv = pmix_argv_copy_strip(argv); // strip any incoming quoted arguments

    /* save a pristine copy of the environment for launch purposes.
     * This MUST be done so that we can pass it to any local procs we
     * spawn - otherwise, those local procs will get a bunch of
     * params only relevant to PRRTE. Skip all PMIx and PRRTE params
     * as those are only targeting us
     */
    prte_launch_environ = NULL;
    for (i=0; NULL != environ[i]; i++) {
        if (0 != strncmp(environ[i], "PMIX_", 5) &&
            0 != strncmp(environ[i], "PRTE_", 5)) {
            pmix_argv_append_nosize(&prte_launch_environ, environ[i]);
        }
    }

    /* because we have to use the schizo framework and init our hostname
     * prior to parsing the incoming argv for cmd line options, do a hacky
     * search to support passing of impacted options (e.g., verbosity for schizo) */
    rc = prte_schizo_base_parse_prte(pargc, 0, pargv, NULL);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    rc = prte_schizo_base_parse_pmix(pargc, 0, pargv, NULL);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    /* init the tiny part of PRTE we initially need */
    prte_init_util(PRTE_PROC_MASTER);

    /** setup callbacks for abort signals - from this point
     * forward, we need to abort in a manner that allows us
     * to cleanup. However, we cannot directly use libevent
     * to trap these signals as otherwise we cannot respond
     * to them if we are stuck in an event! So instead use
     * the basic POSIX trap functions to handle the signal,
     * and then let that signal handler do some magic to
     * avoid the hang
     *
     * NOTE: posix traps don't allow us to do anything major
     * in them, so use a pipe tied to a libevent event to
     * reach a "safe" place where the termination event can
     * be created
     */
    if (0 != (rc = pipe(term_pipe))) {
        exit(1);
    }
    /* setup an event to attempt normal termination on signal */
    rc = prte_event_base_open();
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "Unable to initialize event library\n");
        exit(1);
    }
    prte_event_set(prte_event_base, &term_handler, term_pipe[0], PRTE_EV_READ, clean_abort, NULL);
    prte_event_add(&term_handler, NULL);

    /* Set both ends of this pipe to be close-on-exec so that no
     children inherit it */
    if (pmix_fd_set_cloexec(term_pipe[0]) != PRTE_SUCCESS ||
        pmix_fd_set_cloexec(term_pipe[1]) != PRTE_SUCCESS) {
        fprintf(stderr, "unable to set the pipe to CLOEXEC\n");
        prte_progress_thread_finalize(NULL);
        exit(1);
    }

    /* setup callback for SIGPIPE */
    setup_sighandler(SIGPIPE, &epipe_handler, epipe_signal_callback);

    /* point the signal trap to a function that will activate that event */
    signal(SIGTERM, abort_signal_callback);
    signal(SIGINT, abort_signal_callback);
    signal(SIGHUP, abort_signal_callback);

    /* open the SCHIZO framework */
    rc = prte_mca_base_framework_open(&prte_schizo_base_framework,
                                      PRTE_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    if (PRTE_SUCCESS != (rc = prte_schizo_base_select())) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    /* look for any personality specification */
    personality = NULL;
    for (i = 0; NULL != argv[i]; i++) {
        if (0 == strcmp(argv[i], "--personality")) {
            personality = argv[i + 1];
            break;
        }
    }

    /* detect if we are running as a proxy and select the active
     * schizo module for this tool */
    schizo = prte_schizo_base_detect_proxy(personality);
    if (NULL == schizo) {
        pmix_show_help("help-schizo-base.txt", "no-proxy", true, prte_tool_basename, personality);
        return 1;
    }
    if (0 != strcmp(schizo->name, "prte")) {
        proxyrun = true;
    } else {
        /* if we are using the "prte" personality, but we
         * are not actually running as "prte" or are actively
         * testing the proxy capability , then we are acting
         * as a proxy */
        if (0 != strcmp(prte_tool_basename, "prte") || prte_schizo_base.test_proxy_launch) {
            proxyrun = true;
        }
    }
    if (NULL == personality) {
        personality = schizo->name;
    }
    /* ensure we don't confuse any downstream PRRTE tools on
     * choice of proxy since some environments forward their envars */
    unsetenv("PRTE_MCA_schizo_proxy");

    /* parse the input argv to get values, including everyone's MCA params */
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    rc = schizo->parse_cli(pargv, &results, PMIX_CLI_WARN);
    if (PRTE_SUCCESS != rc) {
        PMIX_DESTRUCT(&results);
        if (PRTE_OPERATION_SUCCEEDED == rc) {
            return PRTE_SUCCESS;
        }
        if (PRTE_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n", prte_tool_basename, prte_strerror(rc));
        }
        return rc;
    }

    /* check if we are running as root - if we are, then only allow
     * us to proceed if the allow-run-as-root flag was given. Otherwise,
     * exit with a giant warning message
     */
    if (0 == geteuid()) {
        schizo->allow_run_as_root(&results); // will exit us if not allowed
    }

    /* if we were given a keepalive pipe, set up to monitor it now */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_KEEPALIVE);
    if (NULL != opt) {
        pmix_setenv("PMIX_KEEPALIVE_PIPE", opt->values[0], true, &environ);
    }

    /* check for debug options */
    if (pmix_cmd_line_is_taken(&results, PRTE_CLI_DEBUG)) {
        prte_debug_flag = true;
    }
    if (pmix_cmd_line_is_taken(&results, PRTE_CLI_DEBUG_DAEMONS)) {
        prte_debug_daemons_flag = true;
    }
    if (pmix_cmd_line_is_taken(&results, PRTE_CLI_LEAVE_SESSION_ATTACHED)) {
        prte_leave_session_attached = true;
    }

    /* detach from controlling terminal
     * otherwise, remain attached so output can get to us
     */
    if (pmix_cmd_line_is_taken(&results, PRTE_CLI_DAEMONIZE)) {
        pipe(wait_pipe);
        prte_state_base_parent_fd = wait_pipe[1];
        prte_daemon_init_callback(NULL, wait_dvm);
        close(wait_pipe[0]);
    } else {
#if defined(HAVE_SETSID)
        /* see if we were directed to separate from current session */
        if (pmix_cmd_line_is_taken(&results, PRTE_CLI_SET_SID)) {
            setsid();
        }
#endif
    }

    if (pmix_cmd_line_is_taken(&results, PRTE_CLI_NO_READY_MSG)) {
        prte_state_base_ready_msg = false;
    }

    if (pmix_cmd_line_is_taken(&results, PRTE_CLI_SYSTEM_SERVER)) {
        /* we should act as system-level PMIx server */
        pmix_setenv("PRTE_MCA_pmix_system_server", "1", true, &environ);
    }
    /* always act as session-level PMIx server */
    pmix_setenv("PRTE_MCA_pmix_session_server", "1", true, &environ);
    /* if we were asked to report a uri, set the MCA param to do so */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_REPORT_URI);
    if (NULL != opt) {
        prte_pmix_server_globals.report_uri = strdup(opt->values[0]);
    }

    /* if we are supporting a singleton, push its ID into the environ
     * so it can get picked up and registered by server init */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_SINGLETON);
    if (NULL != opt) {
        prte_pmix_server_globals.singleton = strdup(opt->values[0]);
    }

    /* Setup MCA params */
    prte_register_params();

    /* default to a persistent DVM */
    prte_persistent = true;

    /* if we are told to daemonize, then we cannot have apps */
    if (!pmix_cmd_line_is_taken(&results, PRTE_CLI_DAEMONIZE)) {
        /* see if they want to run an application - let's parse
         * the cmd line to get it */
        rc = prte_parse_locals(schizo, &apps, pargv, &hostfiles, &hosts);
        // not-found => no app given
        if (PRTE_SUCCESS != rc && PRTE_ERR_NOT_FOUND != rc) {
            PRTE_UPDATE_EXIT_STATUS(rc);
            goto DONE;
        }
        /* did they provide an app? */
        if (PMIX_SUCCESS != rc || 0 == pmix_list_get_size(&apps)) {
            if (proxyrun) {
                pmix_show_help("help-prun.txt", "prun:executable-not-specified", true,
                               prte_tool_basename, prte_tool_basename);
                PRTE_UPDATE_EXIT_STATUS(rc);
                goto DONE;
            }
            /* nope - just need to wait for instructions */
        } else {
            /* they did provide an app - this is only allowed
             * when running as a proxy! */
            if (!proxyrun) {
                pmix_show_help("help-prun.txt", "prun:executable-incorrectly-given", true,
                               prte_tool_basename, prte_tool_basename);
                PRTE_UPDATE_EXIT_STATUS(rc);
                goto DONE;
            }
            /* mark that we are not a persistent DVM */
            prte_persistent = false;
        }
    }

    /* setup PRTE infrastructure */
    if (PRTE_SUCCESS != (ret = prte_init(&pargc, &pargv, PRTE_PROC_MASTER))) {
        PRTE_ERROR_LOG(ret);
        return ret;
    }
    /* get my proc ID */
    ret = PMIx_Get(NULL, PMIX_PROCID, NULL, 0, &val);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
        goto DONE;
    }
    memcpy(&myproc, val->data.proc, sizeof(pmix_proc_t));
    PMIX_VALUE_RELEASE(val);

    /* setup callbacks for signals we should forward */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_FWD_SIGNALS);
    if (NULL != opt) {
        param = opt->values[0];
    } else {
        param = NULL;
    }
    if (PRTE_SUCCESS != (rc = prte_ess_base_setup_signals(param))) {
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
        goto DONE;
    }
    if (0 < (i = pmix_list_get_size(&prte_ess_base_signals))) {
        forward_signals_events = (prte_event_t *) malloc(sizeof(prte_event_t) * i);
        if (NULL == forward_signals_events) {
            ret = PRTE_ERR_OUT_OF_RESOURCE;
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
            goto DONE;
        }
        i = 0;
        PMIX_LIST_FOREACH(sig, &prte_ess_base_signals, prte_ess_base_signal_t)
        {
            setup_sighandler(sig->signal, forward_signals_events + i, signal_forward_callback);
            ++i;
        }
    }
    signals_set = true;

    /* if we are supporting a singleton, add it to our jobs */
    if (NULL != prte_pmix_server_globals.singleton) {
        rc = prep_singleton(prte_pmix_server_globals.singleton);
        if (PRTE_SUCCESS != ret) {
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
            goto DONE;
        }
    }

    /* setup the keepalive event registration */
    PRTE_PMIX_CONSTRUCT_LOCK(&mylock.lock);
    code = PMIX_ERR_JOB_TERMINATED;
    PMIX_LOAD_PROCID(&pname, "PMIX_KEEPALIVE_PIPE", PMIX_RANK_UNDEF);
    PMIX_INFO_LOAD(&info, PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC);
    PMIx_Register_event_handler(&code, 1, &info, 1, parent_died_fn, evhandler_reg_callbk,
                                (void *) &mylock);
    PRTE_PMIX_WAIT_THREAD(&mylock.lock);
    PRTE_PMIX_DESTRUCT_LOCK(&mylock.lock);

    /* check for launch directives in case we were launched by a
     * tool wanting to direct our operation - this needs to be
     * done prior to starting the DVM as it may include instructions
     * on the daemon executable, the fork/exec agent to be used by
     * the daemons, or other directives impacting the DVM itself. */
    PMIX_LOAD_PROCID(&pname, myproc.nspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info, PMIX_OPTIONAL, NULL, PMIX_BOOL);
    /*  Have to cycle over directives we support*/
    ret = PMIx_Get(&pname, PMIX_FORKEXEC_AGENT, &info, 1, &val);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS == ret) {
        /* set our fork/exec agent */
        PMIX_VALUE_RELEASE(val);
    }

    /* start the DVM */

    /* get the daemon job object - was created by ess/hnp component */
    if (NULL == (jdata = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace))) {
        pmix_show_help("help-prun.txt", "bad-job-object", true, prte_tool_basename);
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
        goto DONE;
    }
    /* ess/hnp also should have created a daemon "app" */
    if (NULL == (dapp = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, 0))) {
        pmix_show_help("help-prun.txt", "bad-app-object", true, prte_tool_basename);
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
        goto DONE;
    }

    /* Did the user specify a prefix, or want prefix by default? */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_PREFIX);
    if (NULL != opt || want_prefix_by_default) {
        if (NULL != opt) {
            param = strdup(opt->values[0]);
        } else {
            /* --enable-prun-prefix-default was given to prun */
            param = strdup(prte_install_dirs.prefix);
        }
        /* "Parse" the param, aka remove superfluous path_sep. */
        param_len = strlen(param);
        while (0 == strcmp(PRTE_PATH_SEP, &(param[param_len - 1]))) {
            param[param_len - 1] = '\0';
            param_len--;
            if (0 == param_len) {
                pmix_show_help("help-prun.txt", "prun:empty-prefix", true, prte_tool_basename,
                               prte_tool_basename);
                PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
                goto DONE;
            }
        }
        prte_set_attribute(&dapp->attributes, PRTE_APP_PREFIX_DIR, PRTE_ATTR_GLOBAL, param,
                           PMIX_STRING);
        free(param);
    } else {
        /* Check if called with fully-qualified path to prte.
           (Note: Put this second so can override with --prefix (above). */
        tpath = NULL;
        if ('/' == argv[0][0]) {
            char *tmp_basename = NULL;
            tpath = pmix_dirname(argv[0]);

            if (NULL != tpath) {
                /* Quick sanity check to ensure we got
                   something/bin/<exec_name> and that the installation
                   tree is at least more or less what we expect it to
                   be */
                tmp_basename = pmix_basename(tpath);
                if (0 == strcmp("bin", tmp_basename)) {
                    char *tmp = tpath;
                    tpath = pmix_dirname(tmp);
                    free(tmp);
                } else {
                    free(tpath);
                    tpath = NULL;
                }
                free(tmp_basename);
            }
            prte_set_attribute(&dapp->attributes, PRTE_APP_PREFIX_DIR, PRTE_ATTR_GLOBAL,
                               tpath, PMIX_STRING);
        }
    }

    /* setup to listen for commands sent specifically to me, even though I would probably
     * be the one sending them! Unfortunately, since I am a participating daemon,
     * there are times I need to send a command to "all daemons", and that means *I* have
     * to receive it too
     */
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_DAEMON,
                  PRTE_RML_PERSISTENT, prte_daemon_recv, NULL);

    /* setup to capture job-level info */
    PMIX_INFO_LIST_START(jinfo);

    /* see if we ourselves were spawned by someone */
    ret = PMIx_Get(&prte_process_info.myproc, PMIX_PARENT_ID, NULL, 0, &val);
    if (PMIX_SUCCESS == ret) {
        PMIX_LOAD_PROCID(&prte_process_info.my_parent, val->data.proc->nspace, val->data.proc->rank);
        PMIX_VALUE_RELEASE(val);
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_REQUESTOR_IS_TOOL, NULL, PMIX_BOOL);
        /* indicate that we are launching on behalf of a parent */
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_PARENT_ID, &prte_process_info.my_parent, PMIX_PROC);
    } else {
        PMIX_LOAD_PROCID(&prte_process_info.my_parent, prte_process_info.myproc.nspace, prte_process_info.myproc.rank);
    }

    /* add any hostfile directives to the daemon job */
    if (prte_persistent) {
        opt = pmix_cmd_line_get_param(&results, PRTE_CLI_HOSTFILE);
        if (NULL != opt) {
            tpath = pmix_argv_join(opt->values, ',');
            prte_set_attribute(&dapp->attributes, PRTE_APP_HOSTFILE,
                               PRTE_ATTR_GLOBAL, tpath, PMIX_STRING);
            free(tpath);
        }

        /* Did the user specify any hosts? */
        opt = pmix_cmd_line_get_param(&results, PRTE_CLI_HOST);
        if (NULL != opt) {
            char *tval;
            tval = pmix_argv_join(opt->values, ',');
            prte_set_attribute(&dapp->attributes, PRTE_APP_DASH_HOST,
                               PRTE_ATTR_GLOBAL, tval, PMIX_STRING);
            free(tval);
        }
    } else {
        /* the directives might be in the app(s) */
        if (NULL != hostfiles) {
            char *tval;
            tval = pmix_argv_join(hostfiles, ',');
            prte_set_attribute(&dapp->attributes, PRTE_APP_HOSTFILE,
                               PRTE_ATTR_GLOBAL, tval, PMIX_STRING);
            free(tval);
            pmix_argv_free(hostfiles);
        }
        if (NULL != hosts) {
            char *tval;
            tval = pmix_argv_join(hosts, ',');
            prte_set_attribute(&dapp->attributes, PRTE_APP_DASH_HOST,
                               PRTE_ATTR_GLOBAL, tval, PMIX_STRING);
            free(tval);
            pmix_argv_free(hosts);
        }
    }

    /* spawn the DVM - we skip the initial steps as this
     * isn't a user-level application */
    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOCATE);

    /* we need to loop the event library until the DVM is alive */
    while (prte_event_base_active && !prte_dvm_ready) {
        prte_event_loop(prte_event_base, PRTE_EVLOOP_ONCE);
    }

    /* check if something went wrong with setting up the dvm, bail out */
    if (!prte_dvm_ready) {
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
        goto DONE;
    }

    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_REPORT_PID);
    if (NULL != opt) {
        /* if the string is a "-", then output to stdout */
        if (0 == strcmp(opt->values[0], "-")) {
            fprintf(stdout, "%lu\n", (unsigned long) getpid());
        } else if (0 == strcmp(opt->values[0], "+")) {
            /* output to stderr */
            fprintf(stderr, "%lu\n", (unsigned long) getpid());
        } else {
            char *leftover;
            int outpipe;
            /* see if it is an integer pipe */
            leftover = NULL;
            outpipe = strtol(opt->values[0], &leftover, 10);
            if (NULL == leftover || 0 == strlen(leftover)) {
                /* stitch together the var names and URI */
                pmix_asprintf(&leftover, "%lu", (unsigned long) getpid());
                /* output to the pipe */
                rc = pmix_fd_write(outpipe, strlen(leftover) + 1, leftover);
                free(leftover);
                close(outpipe);
            } else {
                /* must be a file */
                FILE *fp;
                fp = fopen(opt->values[0], "w");
                if (NULL == fp) {
                    prte_output(0, "Impossible to open the file %s in write mode\n", opt->values[0]);
                    PRTE_UPDATE_EXIT_STATUS(1);
                    goto DONE;
                }
                /* output my PID */
                fprintf(fp, "%lu\n", (unsigned long) getpid());
                fclose(fp);
                mypidfile = strdup(opt->values[0]);
            }
        }
    }

    if (prte_persistent) {
        PMIX_INFO_LIST_RELEASE(jinfo);
        goto proceed;
    }

    /***** CHECK FOR LAUNCH DIRECTIVES - ADD THEM TO JOB INFO IF FOUND ****/
    PMIX_LOAD_PROCID(&pname, myproc.nspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info, PMIX_OPTIONAL, NULL, PMIX_BOOL);
    ret = PMIx_Get(&pname, PMIX_LAUNCH_DIRECTIVES, &info, 1, &val);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS == ret) {
        iptr = (pmix_info_t *) val->data.darray->array;
        ninfo = val->data.darray->size;
        for (n = 0; n < ninfo; n++) {
            PMIX_INFO_LIST_XFER(ret, jinfo, &iptr[n]);
        }
        PMIX_VALUE_RELEASE(val);
    }

    /* pass the personality */
    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_PERSONALITY, personality, PMIX_STRING);

    /* get display options */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_DISPLAY);
    if (NULL != opt) {
        ret = prte_schizo_base_parse_display(opt, jinfo);
        if (PRTE_SUCCESS != ret) {
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
            goto DONE;
        }
    }

    /* get output options */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_OUTPUT);
    if (NULL != opt) {
        ret = prte_schizo_base_parse_output(opt, jinfo);
        if (PRTE_SUCCESS != ret) {
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
            goto DONE;
        }
    }

    /* check for runtime options */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_RTOS);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_RUNTIME_OPTIONS, opt->values[0], PMIX_STRING);
    }

    /* check what user wants us to do with stdin */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_STDIN);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_STDIN_TGT, opt->values[0], PMIX_STRING);
    }

    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_MAPBY);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_MAPBY, opt->values[0], PMIX_STRING);
    }

    /* if the user specified a ranking policy, then set it */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_RANKBY);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_RANKBY, opt->values[0], PMIX_STRING);
    }

    /* if the user specified a binding policy, then set it */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_BINDTO);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_BINDTO, opt->values[0], PMIX_STRING);
    }

    /* check for an exec agent */
   opt = pmix_cmd_line_get_param(&results, PRTE_CLI_EXEC_AGENT);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_EXEC_AGENT, opt->values[0], PMIX_STRING);
    }

    /* mark if recovery was enabled */
    if (pmix_cmd_line_is_taken(&results, PRTE_CLI_ENABLE_RECOVERY)) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_JOB_RECOVERABLE, NULL, PMIX_BOOL);
    }
    /* record the max restarts */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_MAX_RESTARTS);
    if (NULL != opt) {
        ui32 = strtol(opt->values[0], NULL, 10);
        PMIX_LIST_FOREACH(app, &apps, prte_pmix_app_t)
        {
            PMIX_INFO_LIST_ADD(ret, app->info, PMIX_MAX_RESTARTS, &ui32, PMIX_UINT32);
        }
    }
    /* if continuous operation was specified */
    if (pmix_cmd_line_is_taken(&results, PRTE_CLI_CONTINUOUS)) {
        /* mark this job as continuously operating */
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_JOB_CONTINUOUS, NULL, PMIX_BOOL);
    }
#ifdef PMIX_ABORT_NONZERO_EXIT
    /* if ignore non-zero exit was specified */
    if (pmix_cmd_line_is_taken(&results, PRTE_CLI_TERM_NONZERO)) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_ABORT_NONZERO_EXIT, NULL, PMIX_BOOL);
    }
#endif
    /* if stop-on-exec was specified */
    if (pmix_cmd_line_is_taken(&results, PRTE_CLI_STOP_ON_EXEC)) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_DEBUG_STOP_ON_EXEC, NULL, PMIX_BOOL);
    }

    /* check for a job timeout specification, to be provided in seconds
     * as that is what MPICH used
     */
    timeoutenv = NULL;
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_TIMEOUT);
    if (NULL != opt || NULL != (timeoutenv = getenv("MPIEXEC_TIMEOUT"))) {
        if (NULL != timeoutenv) {
            i = strtol(timeoutenv, NULL, 10);
            /* both cannot be present, or they must agree */
            if (NULL != opt) {
                n = strtol(opt->values[0], NULL, 10);
                if (i != n) {
                    pmix_show_help("help-prun.txt", "prun:timeoutconflict", false,
                                   prte_tool_basename, n, timeoutenv);
                    PRTE_UPDATE_EXIT_STATUS(1);
                    goto DONE;
                }
            }
        } else {
            i = strtol(opt->values[0], NULL, 10);
        }
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_JOB_TIMEOUT, &i, PMIX_INT);
    }
    if (pmix_cmd_line_is_taken(&results, PRTE_CLI_STACK_TRACES)) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_TIMEOUT_STACKTRACES, NULL, PMIX_BOOL);
    }
    if (pmix_cmd_line_is_taken(&results, PRTE_CLI_REPORT_STATE)) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_TIMEOUT_REPORT_STATE, NULL, PMIX_BOOL);
    }
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_SPAWN_TIMEOUT);
    if (NULL != opt) {
        i = strtol(opt->values[0], NULL, 10);
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_SPAWN_TIMEOUT, &i, PMIX_INT);
    }
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_DO_NOT_AGG_HELP);
    if (NULL != opt) {
        flag = false;
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_LOG_AGG, &flag, PMIX_BOOL);
    }

    /* give the schizo components a chance to add to the job info */
    schizo->job_info(&results, jinfo);

    /* pickup any relevant envars */
    ninfo = 4;
    PMIX_INFO_CREATE(iptr, ninfo);
    flag = true;
    PMIX_INFO_LOAD(&iptr[0], PMIX_SETUP_APP_ENVARS, &flag, PMIX_BOOL);
    ui32 = geteuid();
    PMIX_INFO_LOAD(&iptr[1], PMIX_USERID, &ui32, PMIX_UINT32);
    ui32 = getegid();
    PMIX_INFO_LOAD(&iptr[2], PMIX_GRPID, &ui32, PMIX_UINT32);
    PMIX_INFO_LOAD(&iptr[3], PMIX_PERSONALITY, personality, PMIX_STRING);

    PRTE_PMIX_CONSTRUCT_LOCK(&mylock.lock);
    ret = PMIx_server_setup_application(prte_process_info.myproc.nspace, iptr, ninfo, setupcbfunc,
                                        &mylock);
    if (PMIX_SUCCESS != ret) {
        prte_output(0, "Error setting up application: %s", PMIx_Error_string(ret));
        PRTE_PMIX_DESTRUCT_LOCK(&mylock.lock);
        PRTE_UPDATE_EXIT_STATUS(ret);
        goto DONE;
    }
    PRTE_PMIX_WAIT_THREAD(&mylock.lock);
    PMIX_INFO_FREE(iptr, ninfo);
    if (PMIX_SUCCESS != mylock.status) {
        prte_output(0, "Error setting up application: %s", PMIx_Error_string(mylock.status));
        PRTE_UPDATE_EXIT_STATUS(mylock.status);
        PRTE_PMIX_DESTRUCT_LOCK(&mylock.lock);
        goto DONE;
    }
    PRTE_PMIX_DESTRUCT_LOCK(&mylock.lock);
    /* transfer any returned ENVARS to the job_info */
    if (NULL != mylock.info) {
        for (n = 0; n < mylock.ninfo; n++) {
            if (PMIX_CHECK_KEY(&mylock.info[n], PMIX_SET_ENVAR) ||
                PMIX_CHECK_KEY(&mylock.info[n], PMIX_ADD_ENVAR) ||
                PMIX_CHECK_KEY(&mylock.info[n], PMIX_UNSET_ENVAR) ||
                PMIX_CHECK_KEY(&mylock.info[n], PMIX_PREPEND_ENVAR) ||
                PMIX_CHECK_KEY(&mylock.info[n], PMIX_APPEND_ENVAR)) {
                PMIX_INFO_LIST_XFER(ret, jinfo, &mylock.info[n]);
            }
        }
        PMIX_INFO_FREE(mylock.info, mylock.ninfo);
    }

    /* convert the job info into an array */
    PMIX_INFO_LIST_CONVERT(ret, jinfo, &darray);
    if (PMIX_ERR_EMPTY == ret) {
        iptr = NULL;
        ninfo = 0;
    } else if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PRTE_UPDATE_EXIT_STATUS(rc);
        goto DONE;
    } else {
        iptr = (pmix_info_t *) darray.array;
        ninfo = darray.size;
    }
    PMIX_INFO_LIST_RELEASE(jinfo);

    /* convert the apps to an array */
    napps = pmix_list_get_size(&apps);
    PMIX_APP_CREATE(papps, napps);

    n = 0;
    PMIX_LIST_FOREACH(app, &apps, prte_pmix_app_t)
    {
        papps[n].cmd = strdup(app->app.cmd);
        papps[n].argv = pmix_argv_copy(app->app.argv);
        papps[n].env = pmix_argv_copy(app->app.env);
        papps[n].cwd = strdup(app->app.cwd);
        papps[n].maxprocs = app->app.maxprocs;
        PMIX_INFO_LIST_CONVERT(ret, app->info, &darray);
        if (PMIX_SUCCESS != ret) {
            if (PMIX_ERR_EMPTY == ret) {
                papps[n].info = NULL;
                papps[n].ninfo = 0;
            } else {
                PMIX_ERROR_LOG(ret);
                PRTE_UPDATE_EXIT_STATUS(rc);
                goto DONE;
            }
        } else {
            papps[n].info = (pmix_info_t *) darray.array;
            papps[n].ninfo = darray.size;
        }
        ++n;
    }

    if (verbose) {
        prte_output(0, "Spawning job");
    }

    /* let the PMIx server handle it for us so that all the job infos
     * get properly recorded - e.g., forwarding IOF */
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    ret = PMIx_Spawn_nb(iptr, ninfo, papps, napps, spcbfunc, &lock);
    if (PRTE_SUCCESS != ret) {
        prte_output(0, "PMIx_Spawn failed (%d): %s", ret, PMIx_Error_string(ret));
        rc = ret;
        PRTE_UPDATE_EXIT_STATUS(rc);
        goto DONE;
    }
    /* we have to cycle the event library here so we can process
     * the spawn request */
    while (prte_event_base_active && lock.active) {
        prte_event_loop(prte_event_base, PRTE_EVLOOP_ONCE);
    }
    PMIX_ACQUIRE_OBJECT(&lock.lock);
    if (PMIX_SUCCESS != lock.status) {
        PRTE_UPDATE_EXIT_STATUS(lock.status);
        goto DONE;
    }
    PMIX_LOAD_NSPACE(spawnednspace, lock.msg);
    PRTE_PMIX_DESTRUCT_LOCK(&lock);

    if (verbose) {
        prte_output(0, "JOB %s EXECUTING", PRTE_JOBID_PRINT(spawnednspace));
    }

    /* check what user wants us to do with stdin */
    PMIX_LOAD_NSPACE(pname.nspace, spawnednspace);
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_STDIN);
    if (NULL != opt) {
        if (0 == strcmp(opt->values[0], "all")) {
            pname.rank = PMIX_RANK_WILDCARD;
        } else if (0 == strcmp(opt->values[0], "none")) {
            pname.rank = PMIX_RANK_INVALID;
        } else {
            pname.rank = 0;
        }
    } else {
        pname.rank = 0;
    }
    if (PMIX_RANK_INVALID != pname.rank) {
        PMIX_INFO_CREATE(iptr, 1);
        PMIX_INFO_LOAD(&iptr[0], PMIX_IOF_PUSH_STDIN, NULL, PMIX_BOOL);
        PRTE_PMIX_CONSTRUCT_LOCK(&lock);
        ret = PMIx_IOF_push(&pname, 1, NULL, iptr, 1, opcbfunc, &lock);
        if (PMIX_SUCCESS != ret && PMIX_OPERATION_SUCCEEDED != ret) {
            prte_output(0, "IOF push of stdin failed: %s", PMIx_Error_string(ret));
        } else if (PMIX_SUCCESS == ret) {
            PRTE_PMIX_WAIT_THREAD(&lock);
        }
        PRTE_PMIX_DESTRUCT_LOCK(&lock);
        PMIX_INFO_FREE(iptr, 1);
    }

    /* Initialize the setop server */
    setop_server_init();

    /* Listen for special commands send to the master (Resource Changes)*/
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_MASTER,
              PRTE_RML_PERSISTENT, prte_master_recv, NULL);
    

    /* Set the highest job rank in the setop server */
    size_t nprocs = prte_get_job_data_object(spawnednspace)->num_procs;    
    set_highest_job_rank(spawnednspace, nprocs);

    /* create a pset for the job */
    char *pset_name = strdup("RM://jobs/0");
    pmix_pointer_array_t *pset_procs_parray = prte_get_job_data_object(spawnednspace)->procs;
    prte_pset_define_from_parray(pset_name, pset_procs_parray, PRTE_PSET_FLAG_ADD); 
    free(pset_name);

    //printf("\nPRRTE HNP Server pid:\n %lu\n\n", (unsigned long) getpid());
    //char hostname[256];
    //gethostname(hostname, 256);
    //FILE *f = fopen("/opt/hpc/build/examples/tests/hnp", "a");
    //fprintf(f, "Proc %s: pid: %lu, host: %s\n", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (unsigned long)getpid(), hostname);
    //fclose(f);

    /* Initialize the master timing list */
    master_timing_list = (node_t *)calloc(1, sizeof(node_t));
    timings_my_rank = 0;

proceed:
    /* loop the event lib until an exit event is detected */
    while (prte_event_base_active) {
        prte_event_loop(prte_event_base, PRTE_EVLOOP_ONCE);
    }

    PMIX_ACQUIRE_OBJECT(prte_event_base_active);

    /* close the push of our stdin */
    PMIX_INFO_LOAD(&info, PMIX_IOF_COMPLETE, NULL, PMIX_BOOL);
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    ret = PMIx_IOF_push(NULL, 0, NULL, &info, 1, opcbfunc, &lock);
    if (PMIX_SUCCESS != ret && PMIX_OPERATION_SUCCEEDED != ret) {
        prte_output(0, "IOF close of stdin failed: %s", PMIx_Error_string(ret));
    } else if (PMIX_SUCCESS == ret) {
        PRTE_PMIX_WAIT_THREAD(&lock);
    }
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    PMIX_INFO_DESTRUCT(&info);

DONE:
    /* cleanup and leave */
    prte_finalize();

    if (NULL != mypidfile) {
        unlink(mypidfile);
    }

    if (prte_debug_flag) {
        fprintf(stderr, "exiting with status %d\n", prte_exit_status);
    }
    exit(prte_exit_status);
}

static void clean_abort(int fd, short flags, void *arg)
{
    /* if we have already ordered this once, don't keep
     * doing it to avoid race conditions
     */
    if (pmix_mutex_trylock(&prun_abort_inprogress_lock)) { /* returns 1 if already locked */
        if (forcibly_die) {
            /* exit with a non-zero status */
            exit(1);
        }
        fprintf(stderr,
                "%s: abort is already in progress...hit ctrl-c again to forcibly terminate\n\n",
                prte_tool_basename);
        forcibly_die = true;
        /* reset the event */
        prte_event_add(&term_handler, NULL);
        return;
    }

    fflush(stderr);
    /* ensure we exit with a non-zero status */
    PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
    /* ensure that the forwarding of stdin stops */
    prte_job_term_ordered = true;
    /* tell us to be quiet - hey, the user killed us with a ctrl-c,
     * so need to tell them that!
     */
    prte_execute_quiet = true;
    prte_abnormal_term_ordered = true;
    /* We are in an event handler; the job completed procedure
     will delete the signal handler that is currently running
     (which is a Bad Thing), so we can't call it directly.
     Instead, we have to exit this handler and setup to call
     job_completed() after this. */
    prte_plm.terminate_orteds();
}

static bool first = true;
static bool second = true;

static void surekill(void)
{
    prte_proc_t *child;
    int n;
    pid_t pid;

    /* we don't know how far we got, so be careful here */
    if (NULL != prte_local_children) {
        for (n=0; n < prte_local_children->size; n++) {
            child = (prte_proc_t*)pmix_pointer_array_get_item(prte_local_children, n);
            if (NULL != child && 0 < child->pid) {
                pid = child->pid;
#if HAVE_SETPGID
                {
                    pid_t pgrp;
                    pgrp = getpgid(pid);
                    if (-1 != pgrp) {
                        /* target the lead process of the process
                         * group so we ensure that the signal is
                         * seen by all members of that group. This
                         * ensures that the signal is seen by any
                         * child processes our child may have
                         * started
                         */
                        pid = -pgrp;
                    }
                }
#endif
                kill(pid, SIGKILL);
            }
        }
    }
}

/*
 * Attempt to terminate the job and wait for callback indicating
 * the job has been aborted.
 */
static void abort_signal_callback(int fd)
{
    uint8_t foo = 1;
    char *msg = "Abort is in progress...hit ctrl-c again to forcibly terminate\n\n";

    /* if this is the first time thru, just get
     * the current time
     */
    if (first) {
        first = false;
        /* tell the event lib to attempt to abnormally terminate */
        if (-1 == write(term_pipe[1], &foo, 1)) {
            exit(1);
        }
    } else if (second) {
        if (-1 == write(2, (void *) msg, strlen(msg))) {
            exit(1);
        }
        fflush(stderr);
        second = false;
    } else {
        surekill();  // ensure we attempt to kill everything
        pmix_os_dirpath_destroy(prte_process_info.jobfam_session_dir, true, NULL);
        exit(1);
    }
}

static int prep_singleton(const char *name)
{
    char *ptr, *p1;
    prte_job_t *jdata;
    prte_node_t *node;
    prte_proc_t *proc;
    int rc;
    pmix_rank_t rank;
    prte_app_context_t *app;
    char cwd[PRTE_PATH_MAX];

    ptr = strdup(name);
    p1 = strrchr(ptr, '.');
    *p1 = '\0';
    ++p1;
    rank = strtoul(p1, NULL, 10);
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, ptr);
    free(ptr);
    rc = prte_set_job_data_object(jdata);
    if (PRTE_SUCCESS != rc) {
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
        PMIX_RELEASE(jdata);
        return PRTE_ERR_FATAL;
    }
    /* must have an app */
    app = PMIX_NEW(prte_app_context_t);
    app->app = strdup(jdata->nspace);
    app->num_procs = 1;
    pmix_argv_append_nosize(&app->argv, app->app);
    getcwd(cwd, sizeof(cwd));
    app->cwd = strdup(cwd);
    pmix_pointer_array_set_item(jdata->apps, 0, app);
    jdata->num_apps = 1;

    /* add a map */
    jdata->map = PMIX_NEW(prte_job_map_t);
    /* add our node to the map since the singleton must
     * be here */
    node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, PRTE_PROC_MY_NAME->rank);
    PMIX_RETAIN(node);
    pmix_pointer_array_add(jdata->map->nodes, node);
    ++(jdata->map->num_nodes);

    /* create a proc for the singleton */
    proc = PMIX_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&proc->name, jdata->nspace, rank);
    proc->rank = proc->name.rank;
    proc->parent = PRTE_PROC_MY_NAME->rank;
    proc->app_idx = 0;
    proc->app_rank = rank;
    proc->local_rank = 0;
    proc->node_rank = 0;
    proc->state = PRTE_PROC_STATE_RUNNING;
    /* link it to the job */
    PMIX_RETAIN(jdata);
    proc->job = jdata;
    /* link it to the app */
    PMIX_RETAIN(proc);
    pmix_pointer_array_set_item(&app->procs, rank, proc);
    app->first_rank = rank;
    /* link it to the node */
    PMIX_RETAIN(node);
    proc->node = node;
    /* add it to the job */
    pmix_pointer_array_set_item(jdata->procs, rank, proc);
    jdata->num_procs = 1;
    jdata->num_local_procs = 1;
    /* add it to the node */
    PMIX_RETAIN(proc);
    pmix_pointer_array_add(node->procs, proc);
    node->num_procs = 1;
    node->slots_inuse = 1;

    return PRTE_SUCCESS;
}

static void signal_forward_callback(int signum, short args, void *cbdata)
{
    pmix_status_t rc;
    pmix_proc_t proc;
    pmix_info_t info;

    if (verbose) {
        fprintf(stderr, "%s: Forwarding signal %d to job\n", prte_tool_basename, signum);
    }

    /* send the signal out to the processes */
    PMIX_LOAD_PROCID(&proc, spawnednspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info, PMIX_JOB_CTRL_SIGNAL, &signum, PMIX_INT);
    rc = PMIx_Job_control(&proc, 1, &info, 1, NULL, NULL);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "Signal %d could not be sent to job %s (returned %s)", signum,
                spawnednspace, PMIx_Error_string(rc));
    }
}

/**
 * Deal with sigpipe errors
 */
static int sigpipe_error_count = 0;
static void epipe_signal_callback(int fd, short args, void *cbdata)
{
    sigpipe_error_count++;

    if (10 < sigpipe_error_count) {
        /* time to abort */
        prte_output(0, "%s: SIGPIPE detected - aborting", prte_tool_basename);
        clean_abort(0, 0, NULL);
    }

    return;
}

/* Resource changes */
typedef struct _info_release_cbdata{
    pmix_info_t *info;
    size_t ninfo;
}info_release_cbdata;

void info_release_fn(void *cbdata){
    info_release_cbdata * info_cbdata = (info_release_cbdata *)cbdata;
    if(NULL != info_cbdata->info){
        PMIX_INFO_FREE(info_cbdata->info, info_cbdata->ninfo);
    }
    free(info_cbdata);
}

void reserve_node(prte_node_t *node, size_t reservation_number){
    prte_node_reservation_t *node_reservation;
    node_reservation = PMIX_NEW(prte_node_reservation_t);
    strcpy(node_reservation->node_name, node->name);
    node_reservation->reservation_number = reservation_number;
    pmix_list_append(&prte_pmix_server_globals.node_reservations, &node_reservation->super);
}

void surrender_node(prte_node_t *node){
    prte_node_reservation_t *node_reservation, *nr_next;
    PMIX_LIST_FOREACH_SAFE(node_reservation, nr_next, &prte_pmix_server_globals.node_reservations, prte_node_reservation_t){
        if(0 == strcmp(node_reservation->node_name, node->name)){
            pmix_list_remove_item(&prte_pmix_server_globals.node_reservations, &node_reservation->super);
            PMIX_RELEASE(node_reservation);
            return;
        }
    }
}
void surrender_reservation(size_t reservation_number){
    prte_node_reservation_t *node_reservation, *nr_next;
    PMIX_LIST_FOREACH_SAFE(node_reservation, nr_next, &prte_pmix_server_globals.node_reservations, prte_node_reservation_t){
        if(node_reservation->reservation_number == reservation_number){
            pmix_list_remove_item(&prte_pmix_server_globals.node_reservations, &node_reservation->super);
            PMIX_RELEASE(node_reservation);
        }
    }
}

bool node_reserved(prte_node_t *node){
    prte_node_reservation_t *node_reservation;
    PMIX_LIST_FOREACH(node_reservation, &prte_pmix_server_globals.node_reservations, prte_node_reservation_t){
        if(0 == strcmp(node_reservation->node_name, node->name)){
            return true;
        }
    }
    return false;
}

bool node_reserved_by_number(prte_node_t *node, size_t reservation_number){
    prte_node_reservation_t *node_reservation;
    PMIX_LIST_FOREACH(node_reservation, &prte_pmix_server_globals.node_reservations, prte_node_reservation_t){
        if(0 == strcmp(node_reservation->node_name, node->name) && node_reservation->reservation_number == reservation_number){
            return true;
        }
    }
    return false;
}


static pmix_status_t parse_rc_cmd(char *cmd, pmix_res_change_type_t *_rc_type, size_t *nprocs){

    char * token= strtok(cmd, " ");
    if(token==NULL || 0!=strncmp(token, "pmix_session", 12)){
        return PMIX_ERR_BAD_PARAM;
    }

    token= strtok(NULL, " ");
    if(token == NULL)return PMIX_ERR_BAD_PARAM;
    if(0 == strncmp(token, "add", 3))*_rc_type = PMIX_RES_CHANGE_ADD;
    else if(0 == strncmp(token, "sub", 3))*_rc_type = PMIX_RES_CHANGE_SUB;
    else return PMIX_ERR_BAD_PARAM;

    token = strtok(NULL, " ");
    return ((*nprocs = atoi(token)) <= 0) ? PMIX_ERR_BAD_PARAM : PMIX_SUCCESS;



}

/* updates the job data object by adding resources and creates a corresponding delta PSet.
 * We use this job data object in the launch process.
 * THE ACTUAL JOB DATA IS UPDATED AT THE PRTE MASTER!
 */
static void setup_resource_add(prte_job_t *job_data, size_t rc_nprocs, prte_proc_t ***_delta_procs, size_t reservation_number){
    
    size_t n;
    prte_proc_t *proc;
    
    /* determine highest rank in this job. 
     * For simplicity we use a stack approach for resources to avoid fragmentation of the rank space.
     * Until a consens dealing with the consistency of job updates on the client side,
     * this also avoids dealing with recycled process ids on the client side 
     * Moreover the prte proc state machine requires unique ids*/    
    pmix_rank_t num_procs = job_data->num_procs;
    pmix_rank_t highest_rank = 0;
    for(n = 0; n < job_data->procs->size; n++){
        if(NULL == (proc = pmix_pointer_array_get_item(job_data->procs, n))){
            continue;
        }
        proc->rank = proc->name.rank;
        if(proc->rank > highest_rank){
            highest_rank = proc->rank;
        }
    }
    if(highest_rank_global > highest_rank){
        highest_rank = highest_rank_global;
    }


    /* create the delta pset starting at highest rank 
     * FIXME: Consider Multi app contexts
     */
    prte_proc_t **delta_procs = malloc(rc_nprocs * sizeof(prte_proc_t*));
    *_delta_procs = delta_procs;
    pmix_rank_t offset = 1;
    for(n = 0; n < rc_nprocs; n++){
        delta_procs[n] = PMIX_NEW(prte_proc_t);
        PMIX_LOAD_NSPACE(delta_procs[n]->name.nspace, spawnednspace);
        delta_procs[n]->name.rank = delta_procs[n]->rank = delta_procs[n]->app_rank =  highest_rank + offset;
        delta_procs[n]->app_idx = 0;
        delta_procs[n]->state = PRTE_PROC_STATE_INIT;
        ++offset;
    }

    /* Iterate over the nodes and add processes where possible */
    prte_node_t **job_nodes = (prte_node_t **) job_data->map->nodes->addr; 
    size_t proc_index = 0, node_index, daemon_index;

    while(proc_index < rc_nprocs){
        prte_node_t *node;

        /* first fill up nodes allocated to the job */
        for(node_index = 0; node_index < job_data->map->nodes->size; node_index++){
            if(NULL == (node = job_nodes[node_index])){
                continue;
            }
            /* This node is already reserved for another allocation request*/
            if(node_reserved(node) && ! node_reserved_by_number(node, reservation_number)){
                continue;
            }

            /* Get the daemon of this node to set it as the parent of the proc later on*/
            prte_proc_t *daemon_proc = NULL;
            pmix_rank_t parent_vpid = PMIX_RANK_INVALID;
            for(daemon_index = 0; daemon_index < prte_get_job_data_object(PRTE_PROC_MY_PROCID->nspace)->procs->size; daemon_index++){
                
                daemon_proc = pmix_pointer_array_get_item(prte_get_job_data_object(PRTE_PROC_MY_PROCID->nspace)->procs, daemon_index);
                
                if(NULL != daemon_proc && (0 == strcmp(daemon_proc->node->name,node->name))){
                    parent_vpid = daemon_proc->name.rank;
                }
            }

            int32_t cur_slot = node->slots_inuse;
            for(; cur_slot < node->slots && proc_index < rc_nprocs; ){
                /* set parent */
                delta_procs[proc_index]->parent = parent_vpid;
               /* set node */
                PMIX_RETAIN(node);
                delta_procs[proc_index]->node = node;
                delta_procs[proc_index]->node_rank = cur_slot;
                delta_procs[proc_index]->local_rank = cur_slot;
                /* add proc to node */
                PMIX_RETAIN(delta_procs[proc_index]);
                pmix_pointer_array_add(node->procs, delta_procs[proc_index]);
                node->num_procs++;
                node->slots_inuse++;
                node->slots_available--;
                /* and connect it back to its job object, if not already done */
                if (NULL == delta_procs[proc_index]->job) {
                    PMIX_RETAIN(job_data);
                    delta_procs[proc_index]->job = job_data;
                }
                cur_slot++;
                proc_index++;
            }
            if(proc_index == rc_nprocs){
                break;
            }
        }

        
        /* If we reach this, there were not enough nodes to add all processes. 
         * So we try to add another node from the daemon job 
         */
        if(proc_index < rc_nprocs){
            bool node_added = false, already_allocated;
            prte_job_t *djob = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
            prte_node_t *dnode;

            /* Find a node from the DVM that is not yet assigned to the job */
            for(n = 0; n < djob->map->nodes->size; n++){
                if(NULL == (dnode = pmix_pointer_array_get_item(djob->map->nodes, n))){
                    continue;
                }

                /* This node is already reserved for another allocation request*/
                if(node_reserved(dnode) && ! node_reserved_by_number(dnode, reservation_number)){
                    printf("reservation error\n");
                    continue;
                }

                /* Do we already have this node in our job? */
                already_allocated = false;
                for(node_index = 0; node_index < job_data->map->nodes->size; node_index++){
                    if(NULL == (node = job_nodes[node_index])){
                        continue;
                    }
                    if(0 == strcmp(dnode->name, node->name)){
                        already_allocated = true;
                        break;
                    }
                }

                /* add the node to the job */
                if(!already_allocated){
                    //printf("NOT ENOUGH NODES: Adding new node %s to job %s to fullfill the request\n", dnode->name, job_data->nspace);
                    PMIX_RETAIN(dnode);
                    pmix_pointer_array_add(job_data->map->nodes, dnode);
                    job_data->total_slots_alloc += dnode->slots_available;
                    job_data->map->num_nodes++;
                    job_data->num_daemons_reported++;
                    node_added = true;
                    break;
                }

            }
            /* We couldn't add another node to satisfy the request so leave now */
            if(!node_added){
                break;
            }
        }


    }

    /* TODO: cleanly exit res_change handler. NOTE: This should not happen as we check and reserve the resources before */
    if(proc_index < rc_nprocs){
        printf("Not enough nodes/slots available for this request.\n");
    }

    /* add the procs to the job and app context */
    prte_app_context_t *app = job_data->apps->addr[0];
    for(n = 0; n < rc_nprocs; n++){
        pmix_pointer_array_add(job_data->procs, delta_procs[n]);
        PMIX_RETAIN(delta_procs[n]);
        pmix_pointer_array_add(&app->procs, delta_procs[n]);
        job_data->num_procs++;
        app->num_procs++;
    }

    /* FIXME: Do we still need this? */
    bool fully_described = true;
    prte_set_attribute(&job_data->attributes, PRTE_JOB_FIXED_DVM, PRTE_ATTR_GLOBAL, &fully_described, PMIX_BOOL);

    /* Update highest rank */
    highest_rank_global = highest_rank + rc_nprocs;
}


/* updates the job data object by adding resources and creates a corresponding delta PSet.
 * We use this job data object in the launch process.
 * THE ACTUAL JOB DATA IS UPDATED AT THE PRTE MASTER!
 */
static void setup_resource_add_new(pmix_proc_t client, char *delta_pset_name, size_t reservation_number){
    
    size_t n;
    prte_proc_t *proc;
    pmix_server_pset_t *delta_pset = NULL;
    
    prte_job_t *job_data = prte_get_job_data_object(client.nspace);

    /* We just created this PSet and we are inside of an event so we will always find it */
    PMIX_LIST_FOREACH(delta_pset, &prte_pmix_server_globals.psets, pmix_server_pset_t){
        if(0 == strcmp(delta_pset->name, delta_pset_name)){
            break;
        }
    }

    /* create the delta prte procs starting at highest rank 
     * FIXME: Consider Multi app contexts
     */
    prte_proc_t **delta_procs = malloc(delta_pset->num_members * sizeof(prte_proc_t*));

    pmix_rank_t offset = 1;
    for(n = 0; n < delta_pset->num_members; n++){
        delta_procs[n] = PMIX_NEW(prte_proc_t);
        PMIX_LOAD_NSPACE(delta_procs[n]->name.nspace, delta_pset->members[n].nspace);

        delta_procs[n]->name.rank = delta_procs[n]->rank = delta_procs[n]->app_rank =  delta_pset->members[n].rank;
        delta_procs[n]->app_idx = 0;
        delta_procs[n]->state = PRTE_PROC_STATE_INIT;
        ++offset;
    }

    /* Iterate over the nodes and add processes where possible */
    prte_node_t **job_nodes = (prte_node_t **) job_data->map->nodes->addr; 
    size_t proc_index = 0, node_index, daemon_index;

    while(proc_index < delta_pset->num_members){
        prte_node_t *node;

        /* first fill up nodes allocated to the job */
        for(node_index = 0; node_index < job_data->map->nodes->size; node_index++){
            if(NULL == (node = job_nodes[node_index])){
                continue;
            }
            /* This node is already reserved for another allocation request*/
            if(node_reserved(node) && ! node_reserved_by_number(node, reservation_number)){
                continue;
            }

            /* TODO: Only empty nodes */

            /* Get the daemon of this node to set it as the parent of the proc later on*/
            prte_proc_t *daemon_proc = NULL;
            pmix_rank_t parent_vpid = PMIX_RANK_INVALID;
            for(daemon_index = 0; daemon_index < prte_get_job_data_object(PRTE_PROC_MY_PROCID->nspace)->procs->size; daemon_index++){
                
                daemon_proc = pmix_pointer_array_get_item(prte_get_job_data_object(PRTE_PROC_MY_PROCID->nspace)->procs, daemon_index);
                
                if(NULL != daemon_proc && (0 == strcmp(daemon_proc->node->name,node->name))){
                    parent_vpid = daemon_proc->name.rank;
                }
            }

            int32_t cur_slot = node->slots_inuse;
            for(; cur_slot < node->slots && proc_index < delta_pset->num_members; ){
                /* set parent */
                delta_procs[proc_index]->parent = parent_vpid;
               /* set node */
                PMIX_RETAIN(node);
                delta_procs[proc_index]->node = node;
                delta_procs[proc_index]->node_rank = cur_slot;
                delta_procs[proc_index]->local_rank = cur_slot;
                /* add proc to node */
                PMIX_RETAIN(delta_procs[proc_index]);
                pmix_pointer_array_add(node->procs, delta_procs[proc_index]);
                node->num_procs++;
                node->slots_inuse++;
                node->slots_available--;
                /* and connect it back to its job object, if not already done */
                if (NULL == delta_procs[proc_index]->job) {
                    PMIX_RETAIN(job_data);
                    delta_procs[proc_index]->job = job_data;
                }
                cur_slot++;
                proc_index++;
            }
            if(proc_index == delta_pset->num_members){
                break;
            }
        }

        
        /* If we reach this, there were not enough nodes in the job to add all processes. 
         * So we try to add another node from the daemon job 
         */
        if(proc_index < delta_pset->num_members){
            bool node_added = false, already_allocated;
            prte_job_t *djob = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
            prte_node_t *dnode;

            /* Find a node from the DVM that is not yet assigned to the job */
            for(n = 0; n < djob->map->nodes->size; n++){
                if(NULL == (dnode = pmix_pointer_array_get_item(djob->map->nodes, n))){
                    continue;
                }

                /* This node is already reserved for another allocation request*/
                if(node_reserved(dnode) && ! node_reserved_by_number(dnode, reservation_number)){
                    printf("reservation error\n");
                    continue;
                }

                /* Do we already have this node in our job? */
                already_allocated = false;
                for(node_index = 0; node_index < job_data->map->nodes->size; node_index++){
                    if(NULL == (node = job_nodes[node_index])){
                        continue;
                    }
                    if(0 == strcmp(dnode->name, node->name)){
                        already_allocated = true;
                        break;
                    }
                }

                /* add the node to the job */
                if(!already_allocated){
                    //printf("NOT ENOUGH NODES: Adding new node %s to job %s to fullfill the request\n", dnode->name, job_data->nspace);
                    PMIX_RETAIN(dnode);
                    pmix_pointer_array_add(job_data->map->nodes, dnode);
                    job_data->total_slots_alloc += dnode->slots_available;
                    job_data->map->num_nodes++;
                    job_data->num_daemons_reported++;
                    node_added = true;
                    break;
                }

            }
            /* We couldn't add another node to satisfy the request so leave now */
            if(!node_added){
                break;
            }
        }


    }

    /* TODO: cleanly exit res_change handler. NOTE: This should not happen as we check and reserve the resources before */
    if(proc_index < delta_pset->num_members){
        printf("Not enough nodes/slots available for this request.\n");
    }

    /* add the procs to the job and app context */
    prte_app_context_t *app = job_data->apps->addr[0];
    for(n = 0; n < delta_pset->num_members; n++){
        pmix_pointer_array_add(job_data->procs, delta_procs[n]);
        PMIX_RETAIN(delta_procs[n]);
        pmix_pointer_array_add(&app->procs, delta_procs[n]);
        job_data->num_procs++;
        app->num_procs++;
    }

    /* FIXME: Do we still need this? */
    bool fully_described = true;
    prte_set_attribute(&job_data->attributes, PRTE_JOB_FIXED_DVM, PRTE_ATTR_GLOBAL, &fully_described, PMIX_BOOL);

}

/* creates a copy of the job data, adjusts it to account for resource subtraction and defines the corresponding delta PSet
 * The job data object is ONLY used to update the PMIx namespace. The actual job data IS NOT TOUCHED!
 */
static void setup_resource_sub_new(pmix_proc_t client, prte_job_t *job_data_prev, prte_job_t *job_data_cpy, char *delta_pset_name){
    
    size_t n, i, rm_nodes = 0;
    int node_index;
    prte_attribute_t *attr;
    prte_proc_t *delta_proc;
    pmix_server_pset_t *delta_pset = NULL;
    prte_job_t *job_data_orig; 

    /* We just created this PSet and we are inside of an event so we will always find it */
    PMIX_LIST_FOREACH(delta_pset, &prte_pmix_server_globals.psets, pmix_server_pset_t){
        if(0 == strcmp(delta_pset->name, delta_pset_name)){
            break;
        }
    }

    /* If no previous job data is given, we use the currently stored job data */
    if(NULL == job_data_prev){
        job_data_orig = prte_get_job_data_object(client.nspace);
    }else{
        job_data_orig = job_data_prev;
    }
    

    PMIX_RELEASE(job_data_cpy->apps);
    PMIX_RELEASE(job_data_cpy->procs);
    
    /* create a copy of our job data object 
     * We need to use a copy as we do not want to change our stored job data,
     * instead we want to send an adjusted job data object only to update the pmix sever's job data
     */
    memcpy(job_data_cpy, job_data_orig, sizeof(prte_job_t));
    /* TODO: copy job attributes */
    /* copy the attributes list */
    memset(&job_data_cpy->attributes, 0, sizeof(pmix_list_t));
    PMIX_CONSTRUCT(&job_data_cpy->attributes, pmix_list_t);

    /* set these attributes, just in case they aren't set yet */
    bool fully_described = true;
    //prte_set_attribute(&job_data_cpy->attributes, PRTE_JOB_FULLY_DESCRIBED, PRTE_ATTR_GLOBAL, &fully_described, PMIX_BOOL);
    prte_set_attribute(&job_data_cpy->attributes, PRTE_JOB_FIXED_DVM, PRTE_ATTR_GLOBAL, &fully_described, PMIX_BOOL);
    prte_set_attribute(&job_data_cpy->attributes, PRTE_JOB_LAUNCH_PROXY, PRTE_ATTR_GLOBAL, &prte_process_info.myproc, PMIX_PROC);

    /* TODO: */
    //size_t list_length = job_data_cpy_orig->attributes.prte_list_length;
    //int ctr=0;
    //PMIX_LIST_FOREACH(attr, &job_data_cpy_orig->attributes, prte_attribute_t){
    //        prte_add_attribute(&new_attributes_list, attr->key, attr->local, &attr->data.data, attr->data.type);
    //        printf("prte attribute: %d\n", attr->key);
    //}

    /* TODO: prte_app_copy */
    /* copy app contexts (as we adjust the num procs) */
    job_data_cpy->apps = PMIX_NEW(pmix_pointer_array_t);
    pmix_pointer_array_init(job_data_cpy->apps, job_data_orig->apps->size, PRTE_GLOBAL_ARRAY_MAX_SIZE, 2);
    for(n = 0; n < job_data_orig->apps->size; n++){
        prte_app_context_t *app_ptr = pmix_pointer_array_get_item(job_data_orig->apps, n);
        if(NULL != app_ptr){
            prte_app_context_t *app_cpy = PMIX_NEW(prte_app_context_t);
            memcpy(app_cpy, app_ptr, sizeof(prte_app_context_t));
            PMIX_CONSTRUCT(&app_cpy->attributes, pmix_list_t);

            i = 0;
            PMIX_LIST_FOREACH(attr, &app_ptr->attributes, prte_attribute_t){
                prte_add_attribute(&app_cpy->attributes, attr->key, attr->local, &attr->data.data, attr->data.type);
                i++;   
            }
            for(i = 0; i < app_cpy->num_procs; i++){
                prte_proc_t *app_proc;
                // Protect the procs so we can release the job data later
                if(NULL != (app_proc = pmix_pointer_array_get_item(&app_cpy->procs, i))){
                    PMIX_RETAIN(app_proc);
                }
            }
            pmix_pointer_array_add(job_data_cpy->apps, app_cpy);
        }
    }
    /* copy the procs array so we can adjust it without changing the actual job data */
    job_data_cpy->procs = PMIX_NEW(pmix_pointer_array_t);
    pmix_pointer_array_init(job_data_cpy->procs, 4, PRTE_GLOBAL_ARRAY_MAX_SIZE, PRTE_GLOBAL_ARRAY_BLOCK_SIZE);

    for(n = 0; n < job_data_orig->procs->size; n++){
        prte_proc_t *proc_ptr = pmix_pointer_array_get_item(job_data_orig->procs, n);
        if(NULL != proc_ptr){
            int ret = pmix_pointer_array_add(job_data_cpy->procs, proc_ptr);
            /* Protect the procs when we release the array later when releasing the job data */
            PMIX_RETAIN(proc_ptr); 
        }
    }

    /* copy the map and nodes, protect the nodes in the list*/
    job_data_cpy->map = PMIX_NEW(prte_job_map_t);
    memcpy(job_data_cpy->map, job_data_orig->map, sizeof(prte_job_map_t));
    job_data_cpy->map->nodes = PMIX_NEW(pmix_pointer_array_t);
    prte_node_t *node;
    for(node_index = 0; node_index < job_data_orig->map->nodes->size; node_index++){
        if(NULL == (node = job_data_orig->map->nodes->addr[node_index])){
            continue;
        }

        /* create a node copy with all copyable values */
        prte_node_t *node_cpy; // = PMIX_NEW(prte_node_t);
        prte_node_copy(&node_cpy, node);

        /* create a copy of the proc array */
        node_cpy->procs = PMIX_NEW(pmix_pointer_array_t);
        for(n = 0; n < node->procs->size; n++){
            prte_proc_t *proc_ptr = pmix_pointer_array_get_item(node->procs, n);
            if(NULL != proc_ptr){
                int ret = pmix_pointer_array_add(node_cpy->procs, proc_ptr);
                /* Protect the procs when we release the array later when releasing the job data copy */
                PMIX_RETAIN(proc_ptr); 
            }
        }

        /* copy the attribute list */
        PMIX_CONSTRUCT(&node_cpy->attributes, pmix_list_t);
        PMIX_LIST_FOREACH(attr, &node->attributes, prte_attribute_t){
            prte_add_attribute(&node_cpy->attributes, attr->key, attr->local, &attr->data.data, attr->data.type); 
        }

        pmix_pointer_array_add(job_data_cpy->map->nodes, node);
        
    }

    //PMIX_RETAIN(job_data_cpy->map->nodes);

    /* We traverse the list of nodes and their procs in reverse order and choose the ones from the delta PSet */
    prte_app_context_t *app = job_data_cpy->apps->addr[0]; //FIXME: multi-app
    prte_node_t **job_nodes = (prte_node_t **) job_data_cpy->map->nodes->addr; // or should we use dvm nodes?
    
    size_t proc_index = 0;
    for(node_index = job_data_cpy->map->nodes->size - 1; node_index >= 0; node_index--){
        if(NULL == (node = job_nodes[node_index])){
            continue;
        }
        
        
        int32_t cur_slot = node->slots_inuse - 1;
        for(; cur_slot >= 0 && proc_index < delta_pset->num_members; ){
            for(i = node->procs->size; i >= 0; i--){
                if(NULL == (delta_proc = pmix_pointer_array_get_item(node->procs, i))){
                    continue;
                }

                /* found a proc to remove */
                if( PMIX_CHECK_NSPACE(delta_proc->name.nspace, delta_pset->members[proc_index].nspace) &&
                    delta_proc->name.rank == delta_pset->members[proc_index].rank &&
                    delta_proc->node_rank == cur_slot){
                        //PRTE_FLAG_TEST
                    break;
                }

                if(i == 0){
                    delta_proc = NULL;
                }

            }

            --cur_slot;

            if(NULL == delta_proc){
                continue;
            }

            /* we added this proc to the delta pset. Now remove it from the job & app proc lists */
            for(n = 0; n < job_data_cpy->procs->size; n++){
                prte_proc_t * proct;
                if(NULL == (proct = pmix_pointer_array_get_item(job_data_cpy->procs, n))){
                    continue;
                }
                if(proct->name.rank == delta_proc->name.rank){
                    int ret = pmix_pointer_array_set_item(job_data_cpy->procs, n, NULL);
                    pmix_pointer_array_set_item( proct->node->procs, n, NULL);
                    proct->node->num_procs--;
                    PMIX_RELEASE(proct); // for referenece counting
                }
            }

            for(n = 0; n < app->procs.size; n++){
                prte_proc_t * proct; 
                if(NULL == (proct = pmix_pointer_array_get_item(job_data_cpy->procs, n))){
                    continue;
                }
                if(proct->name.rank == delta_proc->name.rank){ 

                    pmix_pointer_array_set_item(&app->procs, n, NULL);
                    PMIX_RELEASE(proct); // for referenece counting
                }
            }
            /*if(--cur_slot == 0){
                ++rm_nodes;
            }*/
            proc_index++;
        }
        if(proc_index == delta_pset->num_members){
            break;
        }
    }
    if(proc_index < delta_pset->num_members){
        printf("Not enough nodes/slots available for this request\n");
    }

    /* set the number of nodes and procs accordingly */
    job_data_cpy->num_procs -= delta_pset->num_members;
    app->num_procs -= delta_pset->num_members;

    /* TODO: Need to respect procs from other jobs as well as remove nodes from job */
    //job_data_cpy->map->num_nodes -= rm_nodes;

    
    

    memset(&job_data_cpy->children, 0, sizeof(pmix_list_t));
    PMIX_CONSTRUCT(&job_data_cpy->children, pmix_list_t);

    /* prepend the launch message with the sub command, so it is handled correctly when using the launch process */ 
    prte_daemon_cmd_flag_t command = PRTE_DAEMON_DVM_SUB_PROCS;
    PMIx_Data_pack(NULL, &job_data_cpy->launch_msg, &command, 1, PMIX_UINT8);
}

/* creates a copy of the job data, adjusts it to account for resource subtraction and defines the corresponding delta PSet
 * The job data object is ONLY used to update the PMIx namespace. The actual job data IS NOT TOUCHED!
 */
static void setup_resource_sub(pmix_nspace_t job, prte_job_t *job_data_cpy, size_t rc_nprocs, prte_proc_t ***_delta_procs){
    
    size_t n, i, rm_nodes = 0;
    int node_index;
    prte_attribute_t *attr;
    prte_proc_t *delta_proc;

    prte_job_t *job_data_orig = prte_get_job_data_object(job);
    

    PMIX_RELEASE(job_data_cpy->apps);
    PMIX_RELEASE(job_data_cpy->procs);
    
    /* create a copy of our job data object 
     * We need to use a copy as we do not want to change our stored job data,
     * instead we want to send an adjusted job data object only to update the pmix sever's job data
     */
    memcpy(job_data_cpy, job_data_orig, sizeof(prte_job_t));

    /* TODO: copy job attributes */
    /* copy the attributes list */
    memset(&job_data_cpy->attributes, 0, sizeof(pmix_list_t));
    PMIX_CONSTRUCT(&job_data_cpy->attributes, pmix_list_t);

    /* set these attributes, just in case they aren't set yet */
    bool fully_described = true;
    //prte_set_attribute(&job_data_cpy->attributes, PRTE_JOB_FULLY_DESCRIBED, PRTE_ATTR_GLOBAL, &fully_described, PMIX_BOOL);
    prte_set_attribute(&job_data_cpy->attributes, PRTE_JOB_FIXED_DVM, PRTE_ATTR_GLOBAL, &fully_described, PMIX_BOOL);
    prte_set_attribute(&job_data_cpy->attributes, PRTE_JOB_LAUNCH_PROXY, PRTE_ATTR_GLOBAL, &prte_process_info.myproc, PMIX_PROC);

    //size_t list_length = job_data_cpy_orig->attributes.prte_list_length;
    //int ctr=0;
    //PMIX_LIST_FOREACH(attr, &job_data_cpy_orig->attributes, prte_attribute_t){
    //        prte_add_attribute(&new_attributes_list, attr->key, attr->local, &attr->data.data, attr->data.type);
    //        printf("prte attribute: %d\n", attr->key);
    //}


    /* copy app contexts (as we adjust the num procs) */
    job_data_cpy->apps = PMIX_NEW(pmix_pointer_array_t);
    pmix_pointer_array_init(job_data_cpy->apps, job_data_orig->apps->size, PRTE_GLOBAL_ARRAY_MAX_SIZE, 2);
    for(n = 0; n < job_data_orig->apps->size; n++){
        prte_app_context_t *app_ptr = pmix_pointer_array_get_item(job_data_orig->apps, n);
        if(NULL != app_ptr){
            prte_app_context_t *app_cpy = PMIX_NEW(prte_app_context_t);
            memcpy(app_cpy, app_ptr, sizeof(prte_app_context_t));
            PMIX_CONSTRUCT(&app_cpy->attributes, pmix_list_t);

            i = 0;
            PMIX_LIST_FOREACH(attr, &app_ptr->attributes, prte_attribute_t){
                prte_add_attribute(&app_cpy->attributes, attr->key, attr->local, &attr->data.data, attr->data.type);
                i++;   
            }
            for(i = 0; i < app_cpy->num_procs; i++){
                prte_proc_t *app_proc;
                // Protect the procs so we can release the job data later
                if(NULL != (app_proc = pmix_pointer_array_get_item(&app_cpy->procs, i))){
                    PMIX_RETAIN(app_proc);
                }
            }
            pmix_pointer_array_add(job_data_cpy->apps, app_cpy);
        }
    }
    /* copy the procs array so we can adjust it without changing the actual job data */
    job_data_cpy->procs = PMIX_NEW(pmix_pointer_array_t);
    pmix_pointer_array_init(job_data_cpy->procs, 4, PRTE_GLOBAL_ARRAY_MAX_SIZE,
                            PRTE_GLOBAL_ARRAY_BLOCK_SIZE);
    for(n = 0; n < job_data_orig->procs->size; n++){
        prte_proc_t *proc_ptr = pmix_pointer_array_get_item(job_data_orig->procs, n);
        if(NULL != proc_ptr){
            int ret = pmix_pointer_array_add(job_data_cpy->procs, proc_ptr);
            /* Protect the procs when we release the array later when releasing the job data */
            PMIX_RETAIN(proc_ptr); 
        }
    }

    /* copy the map and nodes, protect the nodes in the list*/
    job_data_cpy->map = PMIX_NEW(prte_job_map_t);
    memcpy(job_data_cpy->map, job_data_orig->map, sizeof(prte_job_map_t));
    job_data_cpy->map->nodes = PMIX_NEW(pmix_pointer_array_t);
    prte_node_t *node;
    for(node_index = 0; node_index < job_data_orig->map->nodes->size; node_index++){
        if(NULL == (node = job_data_orig->map->nodes->addr[node_index])){
            continue;
        }

        /* create a node copy with all copyable values */
        prte_node_t *node_cpy; // = PMIX_NEW(prte_node_t);
        prte_node_copy(&node_cpy, node);

        /* create a copy of the proc array */
        node_cpy->procs = PMIX_NEW(pmix_pointer_array_t);
        for(n = 0; n < node->procs->size; n++){
            prte_proc_t *proc_ptr = pmix_pointer_array_get_item(node->procs, n);
            if(NULL != proc_ptr){
                int ret = pmix_pointer_array_add(node_cpy->procs, proc_ptr);
                /* Protect the procs when we release the array later when releasing the job data copy */
                PMIX_RETAIN(proc_ptr); 
            }
        }

        /* copy the attribute list */
        PMIX_CONSTRUCT(&node_cpy->attributes, pmix_list_t);
        PMIX_LIST_FOREACH(attr, &node->attributes, prte_attribute_t){
            prte_add_attribute(&node_cpy->attributes, attr->key, attr->local, &attr->data.data, attr->data.type); 
        }

        pmix_pointer_array_add(job_data_cpy->map->nodes, node);
        
    }

    
    //PMIX_RETAIN(job_data_cpy->map->nodes);

    /* create the delta pset, i.e. determine the procs to be finalized */
    prte_proc_t **delta_procs = malloc(rc_nprocs * sizeof(prte_proc_t*));
    *_delta_procs = delta_procs;
    /* We traverse the list of nodes and their procs in reverse order and choose the ones of our job */
    prte_app_context_t *app = job_data_cpy->apps->addr[0]; //FIXME: multi-app
    prte_node_t **job_nodes = (prte_node_t **) job_data_cpy->map->nodes->addr; // or should we use dvm nodes?
    
    size_t proc_index = 0;
    for(node_index = job_data_cpy->map->nodes->size - 1; node_index >= 0; node_index--){
        if(NULL == (node = job_nodes[node_index])){
            continue;
        }
        
        
        int32_t cur_slot = node->slots_inuse - 1;
        for(; cur_slot >= 0 && proc_index < rc_nprocs; ){
            for(i = node->procs->size; i >= 0; i--){
                if(NULL == (delta_procs[proc_index] = pmix_pointer_array_get_item(node->procs, i))){
                    continue;
                }

                if(delta_procs[proc_index]->name.rank > highest_rank_global){
                    highest_rank_global = delta_procs[proc_index]->name.rank;
                }

                /* found a proc to remove */
                if( PMIX_CHECK_NSPACE(delta_procs[proc_index]->name.nspace, job) && 
                    delta_procs[proc_index]->node_rank == cur_slot){
                        //PRTE_FLAG_TEST
                    break;
                }

                if(i == 0){
                    delta_procs[proc_index] = NULL;
                }

            }

            --cur_slot;

            if(NULL == delta_procs[proc_index]){
                continue;
            }

            /* we added this proc to the delta pset. Now remove it from the job & app proc lists */
            for(n = 0; n < job_data_cpy->procs->size; n++){
                prte_proc_t * proct;
                if(NULL == (proct = pmix_pointer_array_get_item(job_data_cpy->procs, n))){
                    continue;
                }
                if(proct->name.rank == delta_procs[proc_index]->name.rank){
                    int ret = pmix_pointer_array_set_item(job_data_cpy->procs, n, NULL);
                    pmix_pointer_array_set_item( proct->node->procs, n, NULL);
                    proct->node->num_procs--;
                    PMIX_RELEASE(proct); // for referenece counting
                }
            }

            for(n = 0; n < app->procs.size; n++){
                prte_proc_t * proct; 
                if(NULL == (proct = pmix_pointer_array_get_item(job_data_cpy->procs, n))){
                    continue;
                }
                if(proct->name.rank == delta_procs[proc_index]->name.rank){ 

                    pmix_pointer_array_set_item(&app->procs, n, NULL);
                    PMIX_RELEASE(proct); // for referenece counting
                }
            }
            /*if(--cur_slot == 0){
                ++rm_nodes;
            }*/
            proc_index++;
        }
        if(proc_index == rc_nprocs){
            break;
        }
    }
    if(proc_index < rc_nprocs){
        printf("Not enough nodes/slots available for this request\n");
    }

    /* set the number of nodes and procs accordingly */
    job_data_cpy->num_procs -= rc_nprocs;
    app->num_procs -= rc_nprocs;

    /* TODO: Need to respect procs from other jobs as well as remove nodes from job */
    //job_data_cpy->map->num_nodes -= rm_nodes;

    memset(&job_data_cpy->children, 0, sizeof(pmix_list_t));
    PMIX_CONSTRUCT(&job_data_cpy->children, pmix_list_t);

    /* prepend the launch message with the sub command, so it is handle correctly when using the launch process*/ 
    prte_daemon_cmd_flag_t command = PRTE_DAEMON_DVM_SUB_PROCS;
    PMIx_Data_pack(NULL, &job_data_cpy->launch_msg, &command, 1, PMIX_UINT8);

    free(delta_procs);
}

/* Step 4: Send the alloc response command to the requesting daemon -> pmix_server_gen.c*/
void alloc_response_cbfunc(pmix_status_t status, pmix_info_t info[], size_t ninfo, void *cbdata, pmix_release_cbfunc_t release_fn, void *release_cbdata){

    int n, ret, room_number, length;
    size_t reservation_number;
    char * alloc_id = NULL;
    info_release_cbdata * info_cbdata = (info_release_cbdata *)cbdata;
    pmix_data_buffer_t *buffer;
    pmix_info_t *info_rc_op_handle = NULL;
    
    pmix_proc_t *receiver;
    prte_daemon_cmd_flag_t cmd = PRTE_DYNRES_ALLOC_REQ_RESPOND;
    
    /* Get the required info to pack the buffer */
    for(n = 0; n < info_cbdata->ninfo; n++){
        if(PMIX_CHECK_KEY(&info_cbdata->info[n], "prte.hotel.room_number")){
            room_number = info_cbdata->info[n].value.data.integer;
        }
        else if(PMIX_CHECK_KEY(&info_cbdata->info[n], "prte.alloc.requestor")){
            receiver = info_cbdata->info[n].value.data.proc;
        }
        else if(PMIX_CHECK_KEY(&info_cbdata->info[n], "prte.alloc.reservation_number")){
            reservation_number = info_cbdata->info[n].value.data.size;
        }
        else if(PMIX_CHECK_KEY(&info_cbdata->info[n], "mpi.rc_op_handle")){
            info_rc_op_handle = &info_cbdata->info[n];
        }
    }


    /* Convert the reservation_number to string to be used as alloc_id */
    length = snprintf( NULL, 0, "%zu", reservation_number );
    alloc_id = malloc( length + 1 );   
    snprintf( alloc_id, length + 1, "%zu", reservation_number);

    /* Pack the buffer */
    PMIX_DATA_BUFFER_CREATE(buffer);
    ret = PMIx_Data_pack(receiver, buffer, &cmd, 1, PMIX_UINT8);
        if(ret != PMIX_SUCCESS){
        PRTE_ERROR_LOG(ret);
        return;
    }

    ret = PMIx_Data_pack(receiver, buffer, &status, 1, PMIX_STATUS);
    if(ret != PMIX_SUCCESS){
        PRTE_ERROR_LOG(ret);
        return;
    }

    ret = PMIx_Data_pack(receiver, buffer, &room_number, 1, PMIX_INT);
        if(ret != PMIX_SUCCESS){
        PRTE_ERROR_LOG(ret);
        return;
    }

    ret = PMIx_Data_pack(receiver, buffer, &alloc_id, 1, PMIX_STRING);
        if(ret != PMIX_SUCCESS){
        PRTE_ERROR_LOG(ret);
        return;
    }

    ret = PMIx_Data_pack(receiver, buffer, (void*) info_rc_op_handle, 1, PMIX_INFO);
        if(ret != PMIX_SUCCESS){
        PRTE_ERROR_LOG(ret);
        return;
    }

    
    /* Send alloc response command to daemon -> pmix_server_gen.c */
    PRTE_RML_SEND(status, receiver->rank, buffer, PRTE_RML_TAG_MALLEABILITY);

    /* Free the callback data (also frees the input info objects)*/
    //if(NULL != info_cbdata->info){
    //    PMIX_INFO_FREE(info_cbdata->info, info_cbdata->ninfo);
    //}
    free(alloc_id);
    free(info_cbdata);
}

/* Callback for resource change requests 
 * 1. Parse the request
 * 2. Execute the set operations
 * 3. perform resource operations 
 * 4. SUB: get launch message and send launch command (will only update PMIx namespace)
 * 5. Send resource change query data to all daemons
 * 6. ADD: Get launch message and send launch command
 */
static void _rchandler_new(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *scd;
    prte_grpcomm_signature_t *sig;
    prte_daemon_cmd_flag_t cmd;
    prte_job_t *job_data_prev = NULL, *job_data_after = NULL;

    pmix_info_t *info, *info_rc_op_handle;
    pmix_value_t *value_ptr;
    pmix_status_t rc;
    pmix_data_buffer_t *buf;

    pmix_rank_t ndaemons;
    pmix_proc_t daemon_procid, client, *client_ptr;

    size_t n, k, sz, ninfo, num_ops, end_index, num_add = 0, num_sub = 0, reservation_number = 0;
    int32_t ninput = 1, noutput = 1;

    char **input_psets = NULL, **output_psets = NULL;
    
    scd = (prte_pmix_server_op_caddy_t *) cbdata;
    ninfo = scd->ninfo;
    info = scd->info;
    ndaemons = prte_process_info.num_daemons;

    init_add_timing(master_timing_list, (void **) &cur_master_timing_frame, sizeof(timing_frame_master_t));
    
    //if(0 < pmix_list_get_size(&prte_pmix_server_globals.res_changes)){
    //    scd->infocbfunc(PMIX_ERR_BAD_PARAM, NULL, 0, scd->cbdata, NULL, NULL);
    //    return;
    //}


    /* 1. check the provided info */
    for(n = 0; n < ninfo; n++){   
        if(PMIX_CHECK_KEY(&info[n], "mpi.rc_op_handle")){
            info_rc_op_handle = &info[n];
        }
        else if(PMIX_CHECK_KEY(&info[n], "prte.alloc.client")){
            client_ptr = info[n].value.data.proc;
        }
        else if(PMIX_CHECK_KEY(&info[n], "prte.alloc.reservation_number")){
            reservation_number = info[n].value.data.size;
        }
    }

    /* Check if this is a valid op_handle */
    if(PMIX_SUCCESS != (rc = prte_op_handle_verify(info_rc_op_handle))){
        surrender_reservation(reservation_number);
        if(NULL != scd->infocbfunc){
            scd->infocbfunc(rc, NULL, 0, scd->cbdata, NULL, NULL);
        }
        return;
    }

    /* 2. Execute the set operations: Creates the new PSets */
    rc = ophandle_execute(*client_ptr, info_rc_op_handle, 0, &end_index);
    if(rc != PMIX_SUCCESS){
        surrender_reservation(reservation_number);
        if(NULL != scd->infocbfunc){
            scd->infocbfunc(rc, NULL, 0, scd->cbdata, NULL, NULL);
        }
        return;
    }

    /* 3. PERFORM THE RESOURCE OPERATIONS */
    rc = prte_ophandle_get_num_ops(info_rc_op_handle, &num_ops);
    if(rc != PMIX_SUCCESS){
        PRTE_ERROR_LOG(rc);
        return;
    }

    /****** First do the additions */
    prte_setop_t *setop;
    for(n = 0; n < num_ops; n++){
        rc = prte_ophandle_get_nth_op(info_rc_op_handle, n, &setop);
        if(rc != PMIX_SUCCESS){
            PRTE_ERROR_LOG(rc);
            return;
        }
        if(PMIX_RES_CHANGE_ADD == setop->op){
            setup_resource_add_new(*client_ptr, setop->output_names[0].data.string, reservation_number);
            ++num_add;
        }

        PMIX_RELEASE(setop);
    }

    /* And launch new procs*/
    if(0 < num_add){
        prte_ophandle_inform_daemons(info_rc_op_handle, PMIX_RES_CHANGE_ADD);

        prte_state_caddy_t *cd = PMIX_NEW(prte_state_caddy_t);
        cd->jdata = prte_get_job_data_object(client_ptr->nspace);;
        cd->job_state = PRTE_JOB_STATE_LAUNCH_APPS;
        prte_plm_base_launch_apps(0,0, cd);
    }

    /****** Then do the subtractions. We need to keep the last job_data_oject */
    for(n = 0; n < num_ops; n++){

        rc = prte_ophandle_get_nth_op(info_rc_op_handle, n++, &setop);
        if(rc != PMIX_SUCCESS){
            PRTE_ERROR_LOG(rc);
            return;
        }

        if(PMIX_RES_CHANGE_SUB == setop->op){
            job_data_after = PMIX_NEW(prte_job_t);
            setup_resource_sub_new(*client_ptr, job_data_prev, job_data_after, setop->output_names[0].data.string);
            ++num_sub;

            if(NULL != job_data_prev){
                PMIX_RELEASE(job_data_prev);
            }

            job_data_prev = job_data_after;
        }

        PMIX_RELEASE(setop);
    }
    

    /* The request does not involve any resource operation so we are finished. Send the response */
    if(0 == num_add + num_sub){
        if(NULL != scd->infocbfunc){
            scd->infocbfunc(PMIX_SUCCESS, NULL, 0, scd->cbdata, NULL, NULL);
        }
        return; 
    }
    
    /* 4. SUB: retrieve the data needed by the "launcher" && send "launch" command to daemons
     * When removing procs we need to do this before we make the query info available
     */
    if(0 < num_sub && NULL != job_data_after){
        prte_state_caddy_t *cd = PMIX_NEW(prte_state_caddy_t);
        cd->jdata = job_data_after;
        cd->job_state = PRTE_JOB_STATE_SUB;
        
        prte_plm_base_launch_apps(0,0, cd);
                
        /* 5. Inform daemons about res change so they can answer related queries */
        prte_ophandle_inform_daemons(info_rc_op_handle, PMIX_RES_CHANGE_SUB);

        /* We created a copy of the job data so release it here */
        PMIX_RELEASE(job_data_after);
    }



    surrender_reservation(reservation_number);
    
    /* This callback will send a response to the requesting daemon*/
    if(NULL != scd->infocbfunc){
        scd->infocbfunc(PMIX_SUCCESS, NULL, 0, scd->cbdata, NULL, NULL);
    }  
}



/* Step 3: execute the resource change
 * The callback function needs to be called in the end to respond to the requesting daemon. 
 * The cbdata is the buffer to be sent to the daemon, i.e. [0]: the daemon id [1]: the room_number
 * The status indicates the result of the execution */
static void execute_resource_change(pmix_status_t status, pmix_proc_t *proc, pmix_info_t info[], size_t ninfo, pmix_info_cbfunc_t cbfunc, void *cbdata){
    prte_pmix_server_op_caddy_t *cd;

    /* If the allocation wasn't successful directly send an answer to the requesting daemon 
     * no release_func needed as the info objects are included in the cbdata anyways
     */
    if(PMIX_SUCCESS != status){
        if(NULL != cbfunc){
            cbfunc(status, info, ninfo, cbdata, NULL, NULL);
        }
        return;
    }

    cd = PMIX_NEW(prte_pmix_server_op_caddy_t);
    cd->proc = *proc;
    cd->ev.ev_base = prte_event_base;
    cd->codes = &status;
    cd->ncodes = 1;
    cd->info = (pmix_info_t *) info;
    cd->ninfo = ninfo;
    cd->infocbfunc = cbfunc;
    cd->cbdata = cbdata;
    cd->evncbfunc = NULL;

    prte_event_set(prte_event_base, &(cd->ev), -1, PRTE_EV_WRITE, _rchandler_new, cd);
    prte_event_set_priority(&(cd->ev), PRTE_MSG_PRI);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);
}


/* Step 2: The PMIX_allocation_request_nb callback function - We do not provide info, but the scheduler should 
 * It 
 *      -   processes the new resources - if any - and 
 *      -   calls 'execute_resource_change' to execute the resource change according to the infos in the cbdata
 *      -   provides the alloc_response_cbfunc to be executed afterwars to respond to the requesing daemon
 */
void alloc_cbfunc(pmix_status_t status, pmix_info_t info[], size_t ninfo, void *cbdata, pmix_release_cbfunc_t release_fn, void *release_cbdata){

    int n, i, ret, num_new_nodes, n_cbinfo;
    char **new_nodes = NULL;
    prte_node_t *node_ptr;
    pmix_data_buffer_t *buffer;
    pmix_info_t *next_info, *cbinfo;

    info_release_cbdata *icbdata = (info_release_cbdata *) cbdata;
    cbinfo = icbdata->info;
    n_cbinfo = icbdata->ninfo;


    /* Step 1: infos might include new resources to be added to the node pool */
    if(PMIX_SUCCESS == (ret = status) && 0 < ninfo){
        for(n = 0; n < ninfo; n++){
            if(PMIX_CHECK_KEY(&info[n], PMIX_NODE_LIST)){
                pmix_argv_split(info[n].value.data.string, ',');
                break;
            }
        }

        if(NULL == new_nodes){
            ret = PMIX_ERR_UNPACK_FAILURE;
            goto EXECUTE;
        }

        PMIX_ARGV_COUNT(num_new_nodes, new_nodes);

        for(n = 0; n < ninfo; n++){
            node_ptr = PMIX_NEW(prte_node_t);
            node_ptr->name = strdup(new_nodes[n]);

            /* Prep node for usage in job */


            /* Add this node to the node_pool */
            pmix_pointer_array_add(prte_node_pool, (void*) node_ptr);
        }
        pmix_argv_free(new_nodes);
    }


    /* Step 2: Execute the resource change 
     * (we might check the resource pool again in case of a negative response from the scheduler, as the node pool could have changed in the meantime) */
    
    /* The requestor id and the room number need to be included in the callback data buffer
     * The mpi_rc_info_handle and the reservation_number need to be included in the info array 
     * We provide all of them in the cbdata and reference the info in the info parameter, so it just gets release all at once
     */
EXECUTE:

    execute_resource_change(ret, PRTE_PROC_MY_NAME, cbinfo, n_cbinfo, alloc_response_cbfunc, cbdata);

    if(NULL != release_fn){
        release_fn(release_cbdata);
    }


}

/* Step 1:  A daemon has sent a resource allocation request to the master 
 *          -   Analyze the request to determine if we need to request more resources from the RM
 *          -   If required, reserve any nodes we can provide from our node pool
 *          -   Either dirctly call the PMIx_Allocation_request_nb callbaack (alloc_cb) function
 *              or send a PMIx_Allocation_request to the RM (with alloc_cb callback)
 */
void prte_master_process_alloc_req(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer, prte_rml_tag_t tag, void *cbdata){
    
    int ret, room_number, n, m, k, ninfo_int;

    uint8_t rc_type;

    size_t ninfo, sz, reservation_number, num_procs, slots = 0, free_slots_in_job = 0;
    pmix_info_t *info = NULL, *info_rc_op_handle = NULL, *info_ptr, *info_ptr2, *info_ptr3, *alloc_info, *cb_info;
    pmix_alloc_directive_t directive;

    pmix_proc_t client;
    prte_node_t *node;
    prte_job_t *jdata;
    prte_job_map_t *map;

    /* Increment the current reservation number. This will be the reservation number for this resource change as well as the alloc id */
    reservation_number = ++cur_alloc_reservation_number;


    /* Setp 1: UNLOAD buffer */
    n = 1;
    /* Unload the client proc */
    if(PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &client, &n, PMIX_PROC))){
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }
    if(PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &directive, &n, PMIX_UINT8))){
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }
    /* Unload the number of info objects */
    if(PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &ninfo, &n, PMIX_SIZE))){
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }
    /* Unload the info objects */
    PMIX_INFO_CREATE(info, ninfo);
    ninfo_int = (int) ninfo;
    if(PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, info, &ninfo_int, PMIX_INFO))){
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }
    /* Unload the room number */
    if(PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &room_number, &n, PMIX_INT))){
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }

    /* Step 2: Analyze the request they've sent us 
     * -> i.e.: rc_type & num_procs 
     */
    for(n = 0; n < ninfo; n++){
        if(PMIX_CHECK_KEY(&info[n], "mpi.rc_op_handle")){
            info_rc_op_handle = &info[n];
            break;
        }
    }

    /* So far we only support the mpi.rc_op_handle directive*/
    if(NULL == info_rc_op_handle){
        ret = PMIX_ERR_BAD_PARAM;
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }

    info_ptr = (pmix_info_t *) info_rc_op_handle->value.data.darray->array;

    rc_type = info_ptr[0].value.data.uint8;

    for(n = 0; n < info_rc_op_handle->value.data.darray->size; n++){

        if(PMIX_CHECK_KEY(&info_ptr[n], PMIX_RC_TYPE)){
            rc_type = info_ptr[n].value.data.uint8;
        }else if(PMIX_CHECK_KEY(&info_ptr[n], "mpi.op_info")){            
            info_ptr2 = (pmix_info_t *) info_ptr[n].value.data.darray->array;

            for(k = 0; k < info_ptr[n].value.data.darray->size; k++){
                if(PMIX_CHECK_KEY(&info_ptr2[k], "mpi.op_info.info")){
                    info_ptr3 = (pmix_info_t *) info_ptr2[k].value.data.darray->array;

                    for(m = 0; m < info_ptr2[k].value.data.darray->size; m++){
                        if(PMIX_CHECK_KEY(&info_ptr3[m], "mpi.op_info.info.num_procs")){
                            sscanf(info_ptr3[m].value.data.string, "%zu", &num_procs);
                        }
                    }
                }
            }
        }
    }

    /* TODO: Do more analysis. E.g. go over all set oeprations and aggregate the required resources */


    /* Create the callback data object - It will be freed by the alloc_response_cb */
    info_release_cbdata *alloc_cbdata = (info_release_cbdata *) malloc(sizeof(info_release_cbdata));
    alloc_cbdata->ninfo = 4 + ninfo_int;

    PMIX_INFO_CREATE(alloc_cbdata->info, 5);
    for(n = 0; n < ninfo_int; n++){
        PMIX_INFO_XFER(&alloc_cbdata->info[n], &info[n]);
    }
    PMIX_INFO_LOAD(&alloc_cbdata->info[n++], "prte.hotel.room_number", &room_number, PMIX_INT);
    PMIX_INFO_LOAD(&alloc_cbdata->info[n++], "prte.alloc.reservation_number", &reservation_number, PMIX_SIZE);
    PMIX_INFO_LOAD(&alloc_cbdata->info[n++], "prte.alloc.requestor", sender, PMIX_PROC);
    PMIX_INFO_LOAD(&alloc_cbdata->info[n++], "prte.alloc.client", &client, PMIX_PROC);
    
    /* Step 3: If they want to release resources proceed to execute the resource change (alloc_cb) 
     * We will communicate with the scheduler once the processes on the resources have terminated.
     */

    if(PMIX_RES_CHANGE_ADD != rc_type){
        
        /* No info provided so no release func needed */
        alloc_cbfunc(PMIX_SUCCESS, NULL, 0, alloc_cbdata, NULL, NULL);
        return;
    }
    

    /* Step 4: If they requested additional resources check if we can provide them from our own node pool 
     * If yes: proceed to execute the res change (alloc_cb). We do not need to communicate with the scheduler
     * If no, possibly reserve some nodes (include reservation number in cbdata) and goto Step 5
     */

    /* Count the free slots in the job */
    jdata = prte_get_job_data_object(client.nspace);
    map = jdata->map;
    for (n = 0; n < map->nodes->size && slots < num_procs; n++){
        if (NULL == (node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, n))) {
            continue;
        }
        if(node_reserved(node)){
            continue;
        }

        if(node->slots != node->slots_inuse){
            slots += node->slots - node->slots_inuse;
            /* Add node to reserved list*/
            reserve_node(node, reservation_number);
        }

    }
    
    /* Count free slots in the node pool */
    bool coscheduling = true;

    for (n = 0; n < prte_node_pool->size && slots < num_procs; n++) {
        if (NULL == (node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, n))) {
            continue;
        }

        /* If we coschedule we just fill up the free slots of the nodes even if other jobs are running on them */
        if(coscheduling){
            if(node->slots != node->slots_inuse && !node_reserved(node)){
                slots += node->slots - node->slots_inuse;
                /* Add node to reserved list*/
                reserve_node(node, reservation_number);
            }
        }else{
            if(node->slots_inuse == 0 && !node_reserved(node)){
                slots += node->slots;
                /* Add node to reseved list */
                reserve_node(node, reservation_number);
            }
        }
    }

    if(slots >= num_procs){
        /* We have enough resources, so call the callback here */

        alloc_cbfunc(PMIX_SUCCESS, NULL, 0, alloc_cbdata, NULL, NULL);

        return;
    }
    exit(1);
    /* Step 5:
     * We could not satisfy the request using our own resources, so we ask the scheduler for help
     * Send an allocation request to the scheduler. The alloc_cb will be triggered once the scheduler responds
     */
    PMIX_INFO_CREATE(alloc_info, 1);
    uint64_t num_cpus = (uint64_t) num_procs;
    PMIX_INFO_LOAD(alloc_info, PMIX_ALLOC_NUM_CPUS, &num_cpus, PMIX_UINT64);
    PMIx_Allocation_request_nb(PMIX_ALLOC_EXTEND, info, 1, alloc_cbfunc, alloc_cbdata);
    PMIX_INFO_FREE(alloc_info, 1);
    return;

ERROR:
    alloc_cbfunc(ret, NULL, 0, alloc_cbdata, NULL, NULL);
}

void prte_master_recv(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                      prte_rml_tag_t tag, void *cbdata){

    int n, ret;
    prte_daemon_cmd_flag_t command;

    n = 1;
    ret = PMIx_Data_unpack(NULL, buffer, &command, &n, PMIX_UINT8);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        return;
    }

    switch (command) {
        case PRTE_DYNRES_ALLOC_REQ_PROCESS:
            prte_master_process_alloc_req(status, sender, buffer, tag, cbdata);
            break;
    }

}

