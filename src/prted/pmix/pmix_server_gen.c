/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/iof/iof.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/rml/rml.h"
#include "src/mca/schizo/schizo.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_show_help.h"

#include "src/prted/pmix/pmix_server_internal.h"

static int dummy_name_ctr = 0;

static void pmix_server_stdin_push(int sd, short args, void *cbdata);

static void _client_conn(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    prte_proc_t *p, *ptr;
    prte_job_t *jdata;
    int i;

    PMIX_ACQUIRE_OBJECT(cd);

    if (NULL != cd->server_object) {
        /* we were passed back the prte_proc_t */
        p = (prte_proc_t *) cd->server_object;
    } else {
        /* find the named process */
        p = NULL;
        if (NULL == (jdata = prte_get_job_data_object(cd->proc.nspace))) {
            return;
        }
        for (i = 0; i < jdata->procs->size; i++) {
            if (NULL == (ptr = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, i))) {
                continue;
            }
            if (!PMIX_CHECK_NSPACE(cd->proc.nspace, ptr->name.nspace)) {
                continue;
            }
            if (cd->proc.rank == ptr->name.rank) {
                p = ptr;
                break;
            }
        }
    }
    if (NULL != p) {
        PRTE_FLAG_SET(p, PRTE_PROC_FLAG_REG);
        PRTE_ACTIVATE_PROC_STATE(&p->name, PRTE_PROC_STATE_REGISTERED);
    }

    if (NULL != cd->cbfunc) {
        cd->cbfunc(PMIX_SUCCESS, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

pmix_status_t pmix_server_client_connected_fn(const pmix_proc_t *proc, void *server_object,
                                              pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    PRTE_PMIX_THREADSHIFT(proc, server_object, PRTE_SUCCESS,
                          NULL, NULL, 0, _client_conn,
                          cbfunc, cbdata);
    return PRTE_SUCCESS;
}

static void _client_finalized(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    prte_job_t *jdata;
    pmix_data_buffer_t *buf;
    prte_plm_cmd_flag_t command = PRTE_DYNRES_LOCAL_PROCS_FINALIZED;
    prte_proc_t *p, *ptr;
    int i;
    pmix_status_t ret = PMIX_SUCCESS;

    PMIX_ACQUIRE_OBJECT(cd);

    if (NULL != cd->server_object) {
        /* we were passed back the prte_proc_t */
        p = (prte_proc_t *) cd->server_object;
    } else {
        /* find the named process */
        p = NULL;
        if (NULL == (jdata = prte_get_job_data_object(cd->proc.nspace))) {
            /* this tool was not started by us and we have
             * no job record for it - this shouldn't happen,
             * so let's error log it */
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            /* ensure they don't hang */
            goto release;
        }
        for (i = 0; i < jdata->procs->size; i++) {
            if (NULL == (ptr = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, i))) {
                continue;
            }
            if (!PMIX_CHECK_NSPACE(cd->proc.nspace, ptr->name.nspace)) {
                continue;
            }
            if (cd->proc.rank == ptr->name.rank) {
                p = ptr;
                break;
            }
        }
        if (NULL != p) {
            /* if we came thru this code path, then this client must be an
             * independent tool that connected to us - i.e., it wasn't
             * something we spawned. For accounting purposes, we have to
             * ensure the job complete procedure is run - otherwise, slots
             * and other resources won't correctly be released */
            PRTE_FLAG_SET(p, PRTE_PROC_FLAG_IOF_COMPLETE);
            PRTE_FLAG_SET(p, PRTE_PROC_FLAG_WAITPID);
        }
        PRTE_ACTIVATE_PROC_STATE(&cd->proc, PRTE_PROC_STATE_TERMINATED);
    }

    if (NULL != p) {
        PRTE_FLAG_SET(p, PRTE_PROC_FLAG_HAS_DEREG);
    }

    //printf("client Finalized!!\n");
    if(NULL != p){
        /* FIXME? */
        //PRTE_FLAG_SET(p, PRTE_PROC_FLAG_IOF_COMPLETE);
        //PRTE_FLAG_SET(p, PRTE_PROC_FLAG_WAITPID);
        
        bool rc_finalization = false;
        prte_res_change_t *res_change;
        PMIX_LIST_FOREACH(res_change, &prte_pmix_server_globals.res_changes, prte_res_change_t){
            if(PMIX_RES_CHANGE_SUB == res_change->rc_type){
                pmix_server_pset_t *rc_pset;
                PMIX_LIST_FOREACH(rc_pset, &prte_pmix_server_globals.psets, pmix_server_pset_t){
                    if(0 == strcmp(res_change->rc_pset, rc_pset->name)){
                        for(i = 0; i < rc_pset->num_members; i++){
                            if(PMIX_CHECK_PROCID(&rc_pset->members[i], &p->name)){
                                if(++res_change->nlocalprocs_finalized == res_change->nlocalprocs){
                                    pmix_proc_t master;
                                    PMIX_LOAD_PROCID(&master, PRTE_PROC_MY_NAME->nspace, PRTE_PROC_MY_HNP->rank);

                                    PMIX_DATA_BUFFER_CREATE(buf);
                                    /* pack the command */
                                    if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &command, 1, PMIX_UINT8))){
                                        PMIX_DATA_BUFFER_RELEASE(buf);
                                        PMIX_ERROR_LOG(ret);
                                        goto release;
                                    }
                                    /* pack the delta pset name of the resource change */
                                    if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &rc_pset->name, 1, PMIX_STRING))){
                                        PMIX_DATA_BUFFER_RELEASE(buf);
                                        PMIX_ERROR_LOG(ret);
                                        goto release;
                                    }

                                    /* pack the delta pset name of the resource change*/
                                    if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &res_change->nlocalprocs, 1, PMIX_SIZE))){
                                        PMIX_DATA_BUFFER_RELEASE(buf);
                                        PMIX_ERROR_LOG(ret);
                                        goto release;;
                                    }

                                    //prte_rml.send_buffer_nb(&master, buf, PRTE_RML_TAG_MALLEABILITY, prte_rml_send_callback, NULL);
                                    PRTE_RML_SEND(ret, master.rank, buf, PRTE_RML_TAG_MALLEABILITY);
                                    //Inform Master: rc_pset and nlocalprocs
                                    rc_finalization = true;
                                    break;
                                }
                            }
                        }
                    }
                    if(rc_finalization){
                        break;
                    }
                }
            }
        }
        /*
        if(!rc_finalization){
            PRTE_ACTIVATE_PROC_STATE(p, PRTE_PROC_STATE_TERMINATED);
        }
        */
    }

release:
    /* release the caller */
    if (NULL != cd->cbfunc) {
        //cd->cbfunc(ret, cd->cbdata);
        cd->cbfunc(PMIX_SUCCESS, cd->cbdata);
     }
    PMIX_RELEASE(cd);

}

pmix_status_t pmix_server_client_finalized_fn(const pmix_proc_t *proc, void *server_object,
                                              pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    PRTE_PMIX_THREADSHIFT(proc, server_object, PRTE_SUCCESS,
                          NULL, NULL, 0, _client_finalized,
                          cbfunc, cbdata);
    return PRTE_SUCCESS;
}

static void _client_abort(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    prte_proc_t *p;

    PMIX_ACQUIRE_OBJECT(cd);

    if (NULL != cd->server_object) {
        p = (prte_proc_t *) cd->server_object;
        p->exit_code = cd->status;
        PRTE_ACTIVATE_PROC_STATE(&p->name, PRTE_PROC_STATE_CALLED_ABORT);
    }

    /* release the caller */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(PMIX_SUCCESS, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

pmix_status_t pmix_server_abort_fn(const pmix_proc_t *proc, void *server_object, int status,
                                   const char msg[], pmix_proc_t procs[], size_t nprocs,
                                   pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    PRTE_PMIX_THREADSHIFT(proc, server_object, status, msg, procs, nprocs, _client_abort, cbfunc,
                          cbdata);
    return PRTE_SUCCESS;
}

static void _register_events(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;

    PMIX_ACQUIRE_OBJECT(cd);

    /* need to implement this */

    if (NULL != cd->cbfunc) {
        cd->cbfunc(PRTE_SUCCESS, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

/* hook for the local PMIX server to pass event registrations
 * up to us - we will assume the responsibility for providing
 * notifications for registered events */
pmix_status_t pmix_server_register_events_fn(pmix_status_t *codes, size_t ncodes,
                                             const pmix_info_t info[], size_t ninfo,
                                             pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;

    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    cd = PMIX_NEW(prte_pmix_server_op_caddy_t);
    cd->codes = codes;
    cd->ncodes = ncodes;
    cd->info = (pmix_info_t *) info;
    cd->ninfo = ninfo;
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;
    prte_event_set(prte_event_base, &(cd->ev), -1, PRTE_EV_WRITE, _register_events, cd);
    prte_event_set_priority(&(cd->ev), PRTE_MSG_PRI);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);
    return PMIX_SUCCESS;
}

static void _deregister_events(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;

    PMIX_ACQUIRE_OBJECT(cd);

    /* need to implement this */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(PRTE_SUCCESS, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}
/* hook for the local PMIX server to pass event deregistrations
 * up to us */
pmix_status_t pmix_server_deregister_events_fn(pmix_status_t *codes, size_t ncodes,
                                               pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;

    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    cd = PMIX_NEW(prte_pmix_server_op_caddy_t);
    cd->codes = codes;
    cd->ncodes = ncodes;
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;
    prte_event_set(prte_event_base, &(cd->ev), -1, PRTE_EV_WRITE, _deregister_events, cd);
    prte_event_set_priority(&(cd->ev), PRTE_MSG_PRI);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);
    return PRTE_SUCCESS;
}

static void _notify_release(int status, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;

    PMIX_ACQUIRE_OBJECT(cd);

    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PMIX_RELEASE(cd);
}

/* someone has sent us an event that we need to distribute
 * to our local clients */
void pmix_server_notify(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                        prte_rml_tag_t tg, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;
    int cnt, rc;
    pmix_proc_t source;
    pmix_data_range_t range = PMIX_RANGE_SESSION;
    pmix_status_t code, ret;
    size_t ninfo;
    pmix_rank_t vpid;

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s PRTE Notification received from %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(sender));

    /* unpack the daemon who broadcast the event */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &vpid, &cnt, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    /* if I am the one who sent it, then discard it */
    if (vpid == PRTE_PROC_MY_NAME->rank) {
        return;
    }

    /* unpack the status code */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &code, &cnt, PMIX_STATUS))) {
        PMIX_ERROR_LOG(ret);
        return;
    }

    /* unpack the source */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &source, &cnt, PMIX_PROC))) {
        PMIX_ERROR_LOG(ret);
        return;
    }

    /* unpack the range */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &range, &cnt, PMIX_DATA_RANGE))) {
        PMIX_ERROR_LOG(ret);
        return;
    }

    cd = PMIX_NEW(prte_pmix_server_op_caddy_t);

    /* unpack the #infos that were provided */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &cd->ninfo, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(ret);
        PMIX_RELEASE(cd);
        return;
    }
    /* reserve a spot for an additional flag */
    ninfo = cd->ninfo + 1;
    /* create the space */
    PMIX_INFO_CREATE(cd->info, ninfo);

    if (0 < cd->ninfo) {
        /* unpack into it */
        cnt = cd->ninfo;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, cd->info, &cnt, PMIX_INFO))) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(cd->info, cd->ninfo);
            PMIX_RELEASE(cd);
            return;
        }
    }
    cd->ninfo = ninfo;

    /* protect against infinite loops by marking that this notification was
     * passed down to the server by me */
    PMIX_INFO_LOAD(&cd->info[ninfo - 1], "prte.notify.donotloop", NULL, PMIX_BOOL);

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s NOTIFYING PMIX SERVER OF STATUS %s SOURCE %s RANGE %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PMIx_Error_string(code), source.nspace,
                        PMIx_Data_range_string(range));

    ret = PMIx_Notify_event(code, &source, range, cd->info, cd->ninfo, _notify_release, cd);
    if (PMIX_SUCCESS != ret) {
        if (PMIX_OPERATION_SUCCEEDED != ret) {
            PMIX_ERROR_LOG(ret);
        }
        if (NULL != cd->info) {
            PMIX_INFO_FREE(cd->info, cd->ninfo);
        }
        PMIX_RELEASE(cd);
    }
}

pmix_status_t pmix_server_notify_event(pmix_status_t code, const pmix_proc_t *source,
                                       pmix_data_range_t range, pmix_info_t info[], size_t ninfo,
                                       pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    int rc;
    prte_grpcomm_signature_t *sig;
    pmix_data_buffer_t pbkt;
    pmix_status_t ret;
    size_t n;

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s local process %s generated event code %s range %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(source),
                        PMIx_Error_string(code), PMIx_Data_range_string(range));

    /* we can get events prior to completing prte_init as we have
     * to init PMIx early so that PRRTE components can use it */
    PMIX_ACQUIRE_THREAD(&prte_init_lock);
    if (!prte_initialized) {
        PMIX_RELEASE_THREAD(&prte_init_lock);
        goto done;
    }
    PMIX_RELEASE_THREAD(&prte_init_lock);

    /* check to see if this is one we sent down */
    for (n = 0; n < ninfo; n++) {
        if (0 == strcmp(info[n].key, "prte.notify.donotloop")) {
            /* yep - do not process */
            goto done;
        }
    }

    /* if this is notification of procs being ready for debug, then
     * we treat this as a state change */
    if (PMIX_READY_FOR_DEBUG == code) {
        PRTE_ACTIVATE_PROC_STATE((pmix_proc_t*)source, PRTE_PROC_STATE_READY_FOR_DEBUG);
        goto done;
    }

    /* a local process has generated an event - we need to xcast it
     * to all the daemons so it can be passed down to their local
     * procs */
    PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);

    /* we need to add a flag indicating this came from us as we are going to get it echoed
     * back to us by the broadcast */
    if (PMIX_SUCCESS
        != (rc = PMIx_Data_pack(NULL, &pbkt, &PRTE_PROC_MY_NAME->rank, 1, PMIX_PROC_RANK))) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return rc;
    }

    /* pack the status code */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &code, 1, PMIX_STATUS))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return ret;
    }
    /* pack the source */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, (pmix_proc_t *) source, 1, PMIX_PROC))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return ret;
    }
    /* pack the range */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &range, 1, PMIX_DATA_RANGE))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return ret;
    }
    /* pack the number of infos */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &ninfo, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return ret;
    }
    if (0 < ninfo) {
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, info, ninfo, PMIX_INFO))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            return ret;
        }
    }

    /* goes to all daemons */
    sig = PMIX_NEW(prte_grpcomm_signature_t);
    if (NULL == sig) {
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return PMIX_ERR_NOMEM;
    }
    sig->signature = (pmix_proc_t *) malloc(sizeof(pmix_proc_t));
    if (NULL == sig->signature) {
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        PMIX_RELEASE(sig);
        return PMIX_ERR_NOMEM;
    }
    PMIX_LOAD_PROCID(&sig->signature[0], PRTE_PROC_MY_NAME->nspace, PMIX_RANK_WILDCARD);
    sig->sz = 1;
    if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(sig, PRTE_RML_TAG_NOTIFICATION, &pbkt))) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        PMIX_RELEASE(sig);
        return PMIX_ERROR;
    }
    PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
    /* maintain accounting */
    PMIX_RELEASE(sig);

done:
    /* we do not need to execute a callback as we did this atomically */
    return PMIX_OPERATION_SUCCEEDED;
}

void pmix_server_jobid_return(int status, pmix_proc_t *sender,
                              pmix_data_buffer_t *buffer, prte_rml_tag_t tg,
                              void *cbdata)
{
    pmix_server_req_t *req;
    int rc, room;
    int32_t ret, cnt;
    pmix_nspace_t jobid;
    pmix_proc_t proc;

    /* unpack the status - this is already a PMIx value */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ret, &cnt, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    /* unpack the jobid */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &jobid, &cnt, PMIX_PROC_NSPACE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    /* unpack our tracking room number */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &room, &cnt, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        /* we are hosed */
        return;
    }

    /* retrieve the request */
    req = (pmix_server_req_t*)pmix_pointer_array_get_item(&prte_pmix_server_globals.local_reqs, room);
    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, room, NULL);

    if (NULL == req) {
        /* we are hosed */
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        prte_output(0, "UNABLE TO RETRIEVE SPWN_REQ FOR JOB %s [room=%d]", jobid, room);
        return;
    }

    PMIX_LOAD_PROCID(&proc, jobid, 0);
    /* the tool is not a client of ours, but we can provide at least some information */
    rc = prte_pmix_server_register_tool(jobid);
    if (PRTE_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        // we can live without it
    }

    req->toolcbfunc(ret, &proc, req->cbdata);

    /* cleanup */
    PMIX_RELEASE(req);
}

static void _toolconn(int sd, short args, void *cbdata)
{
    pmix_server_req_t *cd = (pmix_server_req_t *) cbdata;
    int rc;
    char *tmp;
    size_t n;
    pmix_data_buffer_t *buf;
    prte_plm_cmd_flag_t command = PRTE_PLM_ALLOC_JOBID_CMD;
    pmix_status_t xrc;

    PMIX_ACQUIRE_OBJECT(cd);

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s PROCESSING TOOL CONNECTION",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* check for directives */
    if (NULL != cd->info) {
        for (n = 0; n < cd->ninfo; n++) {
            if (PMIX_CHECK_KEY(&cd->info[n], PMIX_EVENT_SILENT_TERMINATION)) {
                cd->flag = PMIX_INFO_TRUE(&cd->info[n]);
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_VERSION_INFO)) {
                /* we ignore this for now */
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_USERID)) {
                PMIX_VALUE_GET_NUMBER(xrc, &cd->info[n].value, cd->uid, uid_t);
                if (PMIX_SUCCESS != xrc) {
                    if (NULL != cd->toolcbfunc) {
                        cd->toolcbfunc(xrc, NULL, cd->cbdata);
                    }
                    PMIX_RELEASE(cd);
                    return;
                }
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_GRPID)) {
                PMIX_VALUE_GET_NUMBER(xrc, &cd->info[n].value, cd->gid, gid_t);
                if (PMIX_SUCCESS != xrc) {
                    if (NULL != cd->toolcbfunc) {
                        cd->toolcbfunc(xrc, NULL, cd->cbdata);
                    }
                    PMIX_RELEASE(cd);
                    return;
                }
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_NSPACE)) {
                PMIX_LOAD_NSPACE(cd->target.nspace, cd->info[n].value.data.string);
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_RANK)) {
                cd->target.rank = cd->info[n].value.data.rank;
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_HOSTNAME)) {
                cd->operation = strdup(cd->info[n].value.data.string);
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_CMD_LINE)) {
                cd->cmdline = strdup(cd->info[n].value.data.string);
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_LAUNCHER)) {
                cd->launcher = PMIX_INFO_TRUE(&cd->info[n]);
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_PROC_PID)) {
                PMIX_VALUE_GET_NUMBER(xrc, &cd->info[n].value, cd->pid, pid_t);
                if (PMIX_SUCCESS != xrc) {
                    if (NULL != cd->toolcbfunc) {
                        cd->toolcbfunc(xrc, NULL, cd->cbdata);
                    }
                    PMIX_RELEASE(cd);
                    return;
                }
            }
        }
    }

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s TOOL CONNECTION FROM UID %d GID %d NSPACE %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        cd->uid, cd->gid, cd->target.nspace);

    /* if we are not the HNP or master, and the tool doesn't
     * already have a self-assigned name, then
     * we need to ask the master for one */
    if (PMIX_NSPACE_INVALID(cd->target.nspace) || PMIX_RANK_INVALID == cd->target.rank) {
        /* if we are the HNP, we can directly assign the jobid */
        if (PRTE_PROC_IS_MASTER) {
            /* the new nspace is our base nspace with an "@N" extension */
            pmix_asprintf(&tmp, "%s@%u", prte_plm_globals.base_nspace, prte_plm_globals.next_jobid);
            PMIX_LOAD_PROCID(&cd->target, tmp, 0);
            free(tmp);
            prte_plm_globals.next_jobid++;
        } else {
            cd->room_num = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, cd);
            /* we need to send this to the HNP for a jobid */
            PMIX_DATA_BUFFER_CREATE(buf);
            rc = PMIx_Data_pack(NULL, buf, &command, 1, PMIX_UINT8);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
            rc = PMIx_Data_pack(NULL, buf, &cd->room_num, 1, PMIX_INT);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
            /* send it to the HNP for processing - might be myself! */
            PRTE_RML_SEND(rc, PRTE_PROC_MY_HNP->rank,
                          buf, PRTE_RML_TAG_PLM);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
                xrc = prte_pmix_convert_rc(rc);
                pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, cd->room_num, NULL);
                PMIX_DATA_BUFFER_RELEASE(buf);
                if (NULL != cd->toolcbfunc) {
                    cd->toolcbfunc(xrc, NULL, cd->cbdata);
                }
                PMIX_RELEASE(cd);
            }
            return;
        }
    }

    /* the tool is not a client of ours, but we can provide at least some information */
    rc = prte_pmix_server_register_tool(cd->target.nspace);
    if (PMIX_SUCCESS != rc) {
        rc = prte_pmix_convert_rc(rc);
    }
    if (NULL != cd->toolcbfunc) {
        cd->toolcbfunc(rc, &cd->target, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

void pmix_tool_connected_fn(pmix_info_t *info, size_t ninfo, pmix_tool_connection_cbfunc_t cbfunc,
                            void *cbdata)
{
    pmix_server_req_t *cd;

    prte_output_verbose(2, prte_pmix_server_globals.output, "%s TOOL CONNECTION REQUEST RECVD",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* need to threadshift this request */
    cd = PMIX_NEW(pmix_server_req_t);
    cd->info = info;
    cd->ninfo = ninfo;
    cd->toolcbfunc = cbfunc;
    cd->cbdata = cbdata;

    prte_event_set(prte_event_base, &(cd->ev), -1, PRTE_EV_WRITE, _toolconn, cd);
    prte_event_set_priority(&(cd->ev), PRTE_MSG_PRI);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);
}

static void lgcbfn(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;

    if (NULL != cd->cbfunc) {
        cd->cbfunc(cd->status, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

void pmix_server_log_fn(const pmix_proc_t *client, const pmix_info_t data[], size_t ndata,
                        const pmix_info_t directives[], size_t ndirs, pmix_op_cbfunc_t cbfunc,
                        void *cbdata)
{
    size_t n, cnt, dcnt;
    pmix_data_buffer_t *buf;
    int rc = PRTE_SUCCESS;
    pmix_data_buffer_t pbuf, dbuf;
    pmix_byte_object_t pbo, dbo;
    pmix_status_t ret;

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s logging info",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    PMIX_DATA_BUFFER_CONSTRUCT(&dbuf);
    /* if we are the one that passed it down, then we don't pass it back */
    dcnt = 0;
    for (n = 0; n < ndirs; n++) {
        if (PMIX_CHECK_KEY(&directives[n], "prte.log.noloop")) {
            if (PMIX_INFO_TRUE(&directives[n])) {
                rc = PMIX_SUCCESS;
                goto done;
            }
        }
        else {
            ret = PMIx_Data_pack(NULL, &dbuf, (pmix_info_t *) &directives[n], 1, PMIX_INFO);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
            }
            dcnt++;
        }
    }

    PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
    cnt = 0;

    for (n = 0; n < ndata; n++) {
        /* ship this to our HNP/MASTER for processing, even if that is us */
        ret = PMIx_Data_pack(NULL, &pbuf, (pmix_info_t *) &data[n], 1, PMIX_INFO);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
        }
        ++cnt;
    }
    if (0 < cnt) {
        PMIX_DATA_BUFFER_CREATE(buf);
        /* pack the source of this log request */
        rc = PMIx_Data_pack(NULL, buf, (void*)client, 1, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        /* pack number of info provided */
        rc = PMIx_Data_pack(NULL, buf, &cnt, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        /* pack number of directives given */
        rc = PMIx_Data_pack(NULL, buf, &dcnt, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        /* bring over the packed info blob */
        rc = PMIx_Data_unload(&pbuf, &pbo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        rc = PMIx_Data_pack(NULL, buf, &pbo, 1, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
        /* pack the directives blob */
        rc = PMIx_Data_unload(&dbuf, &dbo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        rc = PMIx_Data_pack(NULL, buf, &dbo, 1, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&dbo);
        /* send the result to the HNP */
        PRTE_RML_SEND(rc, PRTE_PROC_MY_HNP->rank, buf,
                      PRTE_RML_TAG_LOGGING);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
        }
    }

done:
    /* we cannot directly execute the callback here
     * as it would threadlock - so shift to somewhere
     * safe */
    PRTE_PMIX_THREADSHIFT(PRTE_NAME_WILDCARD, NULL, rc, NULL, NULL, 0, lgcbfn, cbfunc, cbdata);
}

pmix_status_t pmix_server_job_ctrl_fn(const pmix_proc_t *requestor, const pmix_proc_t targets[],
                                      size_t ntargets, const pmix_info_t directives[], size_t ndirs,
                                      pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    int rc, j;
    int32_t signum;
    size_t m, n;
    prte_proc_t *proc;
    pmix_nspace_t jobid;
    pmix_pointer_array_t parray, *ptrarray;
    pmix_data_buffer_t *cmd;
    prte_daemon_cmd_flag_t cmmnd;
    prte_grpcomm_signature_t *sig;
    pmix_proc_t *proct;

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s job control request from %s:%d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        requestor->nspace, requestor->rank);

    for (m = 0; m < ndirs; m++) {
        if (0 == strncmp(directives[m].key, PMIX_JOB_CTRL_KILL, PMIX_MAX_KEYLEN)) {
            /* convert the list of targets to a pointer array */
            if (NULL == targets) {
                ptrarray = NULL;
            } else {
                PMIX_CONSTRUCT(&parray, pmix_pointer_array_t);
                for (n = 0; n < ntargets; n++) {
                    if (PMIX_RANK_WILDCARD == targets[n].rank) {
                        /* create an object */
                        proc = PMIX_NEW(prte_proc_t);
                        PMIX_LOAD_PROCID(&proc->name, targets[n].nspace, PMIX_RANK_WILDCARD);
                    } else {
                        /* get the proc object for this proc */
                        if (NULL == (proc = prte_get_proc_object(&targets[n]))) {
                            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                            continue;
                        }
                        PMIX_RETAIN(proc);
                    }
                    pmix_pointer_array_add(&parray, proc);
                }
                ptrarray = &parray;
            }
            if (PRTE_SUCCESS != (rc = prte_plm.terminate_procs(ptrarray))) {
                PRTE_ERROR_LOG(rc);
            }
            if (NULL != ptrarray) {
                /* cleanup the array */
                for (j = 0; j < parray.size; j++) {
                    if (NULL != (proc = (prte_proc_t *) pmix_pointer_array_get_item(&parray, j))) {
                        PMIX_RELEASE(proc);
                    }
                }
                PMIX_DESTRUCT(&parray);
            }
        } else if (0 == strncmp(directives[m].key, PMIX_JOB_CTRL_TERMINATE, PMIX_MAX_KEYLEN)) {
            if (NULL == targets) {
                /* terminate the daemons and all running jobs */
                PMIX_DATA_BUFFER_CREATE(cmd);
                /* pack the command */
                cmmnd = PRTE_DAEMON_HALT_VM_CMD;
                rc = PMIx_Data_pack(NULL, cmd, &cmmnd, 1, PMIX_UINT8);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DATA_BUFFER_RELEASE(cmd);
                    return rc;
                }
                /* goes to all daemons */
                sig = PMIX_NEW(prte_grpcomm_signature_t);
                sig->signature = (pmix_proc_t *) malloc(sizeof(pmix_proc_t));
                sig->sz = 1;
                PMIX_LOAD_PROCID(&sig->signature[0], PRTE_PROC_MY_NAME->nspace, PMIX_RANK_WILDCARD);
                if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(sig, PRTE_RML_TAG_DAEMON, cmd))) {
                    PRTE_ERROR_LOG(rc);
                }
                PMIX_DATA_BUFFER_RELEASE(cmd);
                PMIX_RELEASE(sig);
            }
        } else if (0 == strncmp(directives[m].key, PMIX_JOB_CTRL_SIGNAL, PMIX_MAX_KEYLEN)) {
            PMIX_DATA_BUFFER_CREATE(cmd);
            cmmnd = PRTE_DAEMON_SIGNAL_LOCAL_PROCS;
            /* pack the command */
            rc = PMIx_Data_pack(NULL, cmd, &cmmnd, 1, PMIX_UINT8);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(cmd);
                return rc;
            }
            /* pack the target jobid */
            if (NULL == targets) {
                PMIX_LOAD_NSPACE(&jobid, NULL);
            } else {
                proct = (pmix_proc_t *) &targets[0];
                PMIX_LOAD_NSPACE(&jobid, proct->nspace);
            }
            rc = PMIx_Data_pack(NULL, cmd, &jobid, 1, PMIX_PROC_NSPACE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(cmd);
                return rc;
            }
            /* pack the signal */
            PMIX_VALUE_GET_NUMBER(rc, &directives[m].value, signum, int32_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_DATA_BUFFER_RELEASE(cmd);
                return rc;
            }
            rc = PMIx_Data_pack(NULL, cmd, &signum, 1, PMIX_INT32);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(cmd);
                return rc;
            }
            /* goes to all daemons */
            sig = PMIX_NEW(prte_grpcomm_signature_t);
            sig->signature = (pmix_proc_t *) malloc(sizeof(pmix_proc_t));
            sig->sz = 1;
            PMIX_LOAD_PROCID(&sig->signature[0], PRTE_PROC_MY_NAME->nspace, PMIX_RANK_WILDCARD);
            if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(sig, PRTE_RML_TAG_DAEMON, cmd))) {
                PRTE_ERROR_LOG(rc);
            }
            PMIX_DATA_BUFFER_RELEASE(cmd);
            PMIX_RELEASE(sig);
        }
    }

    return PMIX_OPERATION_SUCCEEDED;
}

static void relcb(void *cbdata)
{
    prte_pmix_mdx_caddy_t *cd = (prte_pmix_mdx_caddy_t *) cbdata;

    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PMIX_RELEASE(cd);
}
static void group_release(int status, pmix_data_buffer_t *buf, void *cbdata)
{
    prte_pmix_mdx_caddy_t *cd = (prte_pmix_mdx_caddy_t *) cbdata;
    int32_t cnt;
    int rc = PRTE_SUCCESS;
    pmix_status_t ret;
    uint32_t cid;
    size_t n;
    pmix_byte_object_t bo;
    int32_t byused;

    PMIX_ACQUIRE_OBJECT(cd);

    if (PRTE_SUCCESS != status) {
        rc = status;
        goto complete;
    }

    if (1 == cd->mode) {
        /* a context id was requested, get it */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buf, &cid, &cnt, PMIX_UINT32);
        /* error if they didn't return it */
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto complete;
        }
        cd->ninfo++;
    }
    /* if anything is left in the buffer, then it is
     * modex data that needs to be stored */
    PMIX_BYTE_OBJECT_CONSTRUCT(&bo);
    byused = buf->bytes_used - (buf->unpack_ptr - buf->base_ptr);
    if (0 < byused) {
        bo.bytes = buf->unpack_ptr;
        bo.size = byused;
    }
    if (NULL != bo.bytes && 0 < bo.size) {
        cd->ninfo++;
    }

    if (0 < cd->ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        n = 0;
        if (1 == cd->mode) {
            PMIX_INFO_LOAD(&cd->info[n], PMIX_GROUP_CONTEXT_ID, &cid, PMIX_UINT32);
            ++n;
        }
        if (NULL != bo.bytes && 0 < bo.size) {
            PMIX_INFO_LOAD(&cd->info[n], PMIX_GROUP_ENDPT_DATA, &bo, PMIX_BYTE_OBJECT);
        }
    }

complete:
    ret = prte_pmix_convert_rc(rc);
    /* return to the local procs in the collective */
    if (NULL != cd->infocbfunc) {
        cd->infocbfunc(ret, cd->info, cd->ninfo, cd->cbdata, relcb, cd);
    } else {
        if (NULL != cd->info) {
            PMIX_INFO_FREE(cd->info, cd->ninfo);
        }
        PMIX_RELEASE(cd);
    }
}

pmix_status_t pmix_server_group_fn(pmix_group_operation_t op, char *gpid,
                                   const pmix_proc_t procs[], size_t nprocs,
                                   const pmix_info_t directives[], size_t ndirs,
                                   pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_mdx_caddy_t *cd;
    int rc;
    size_t i, mode = 0;
    pmix_server_pset_t *pset;
    bool fence = false;
    pmix_byte_object_t *bo = NULL;

    /* they are required to pass us an id */
    if (NULL == gpid) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* check the directives */
    for (i = 0; i < ndirs; i++) {
        /* see if they want a context id assigned */
        if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_ASSIGN_CONTEXT_ID)) {
            if (PMIX_INFO_TRUE(&directives[i])) {
                mode = 1;
            }
        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_EMBED_BARRIER)) {
            fence = PMIX_INFO_TRUE(&directives[i]);
        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_ENDPT_DATA)) {
            bo = (pmix_byte_object_t *) &directives[i].value.data.bo;
        }
    }

    if (PMIX_GROUP_CONSTRUCT == op) {
        /* add it to our list of known process sets */
        pset = PMIX_NEW(pmix_server_pset_t);
        pset->name = strdup(gpid);
        pset->num_members = nprocs;
        PMIX_PROC_CREATE(pset->members, pset->num_members);
        memcpy(pset->members, procs, nprocs * sizeof(pmix_proc_t));
        pmix_list_append(&prte_pmix_server_globals.psets, &pset->super);
    } else if (PMIX_GROUP_DESTRUCT == op) {
        /* find this process set on our list of groups */
        PMIX_LIST_FOREACH(pset, &prte_pmix_server_globals.psets, pmix_server_pset_t)
        {
            if (0 == strcmp(pset->name, gpid)) {
                pmix_list_remove_item(&prte_pmix_server_globals.psets, &pset->super);
                PMIX_RELEASE(pset);
                break;
            }
        }
    }

    /* if they don't want us to do a fence and they don't want a
     * context id assigned, then we are done */
    if (!fence && 0 == mode) {
        return PMIX_OPERATION_SUCCEEDED;
    }

    cd = PMIX_NEW(prte_pmix_mdx_caddy_t);
    cd->infocbfunc = cbfunc;
    cd->cbdata = cbdata;
    cd->mode = mode;

    /* compute the signature of this collective */
    if (NULL != procs) {
        cd->sig = PMIX_NEW(prte_grpcomm_signature_t);
        cd->sig->sz = nprocs;
        cd->sig->signature = (pmix_proc_t *) malloc(cd->sig->sz * sizeof(pmix_proc_t));
        memcpy(cd->sig->signature, procs, cd->sig->sz * sizeof(pmix_proc_t));
    }
    PMIX_DATA_BUFFER_CREATE(cd->buf);
    /* if they provided us with a data blob, send it along */
    if (NULL != bo) {
        /* We don't own the byte_object and so we have to
         * copy it here */
        rc = PMIx_Data_embed(cd->buf, bo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }
    /* pass it to the global collective algorithm */
    if (PRTE_SUCCESS != (rc = prte_grpcomm.allgather(cd->sig, cd->buf,
                                                     mode, PMIX_SUCCESS,
                                                     group_release, cd))) {
        PRTE_ERROR_LOG(rc);
        PMIX_RELEASE(cd);
        return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_server_iof_pull_fn(const pmix_proc_t procs[], size_t nprocs,
                                      const pmix_info_t directives[], size_t ndirs,
                                      pmix_iof_channel_t channels, pmix_op_cbfunc_t cbfunc,
                                      void *cbdata)
{
    prte_iof_sink_t *sink;
    size_t i;
    bool stop = false;

    /* no really good way to do this - we have to search the directives to
     * see if we are being asked to stop the specified channels before
     * we can process them */
    for (i = 0; i < ndirs; i++) {
        if (PMIX_CHECK_KEY(&directives[i], PMIX_IOF_STOP)) {
            stop = PMIX_INFO_TRUE(&directives[i]);
            break;
        }
    }

    /* Set up I/O forwarding sinks and handlers for stdout and stderr for each proc
     * requesting I/O forwarding */
    for (i = 0; i < nprocs; i++) {
        if (channels & PMIX_FWD_STDOUT_CHANNEL) {
            if (stop) {
                /* ask the IOF to stop forwarding this channel */
            } else {
                PRTE_IOF_SINK_DEFINE(&sink, &procs[i], fileno(stdout), PRTE_IOF_STDOUT,
                                     prte_iof_base_write_handler);
                PRTE_IOF_SINK_ACTIVATE(sink->wev);
            }
        }
        if (channels & PMIX_FWD_STDERR_CHANNEL) {
            if (stop) {
                /* ask the IOF to stop forwarding this channel */
            } else {
                PRTE_IOF_SINK_DEFINE(&sink, &procs[i], fileno(stderr), PRTE_IOF_STDERR,
                                     prte_iof_base_write_handler);
                PRTE_IOF_SINK_ACTIVATE(sink->wev);
            }
        }
    }
    return PMIX_OPERATION_SUCCEEDED;
}

static void pmix_server_stdin_push(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    pmix_byte_object_t *bo = (pmix_byte_object_t *) cd->server_object;
    size_t n;

    for (n = 0; n < cd->nprocs; n++) {
        PRTE_OUTPUT_VERBOSE((1, prte_pmix_server_globals.output,
                             "%s pmix_server_stdin_push to dest %s: size %zu",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             PRTE_NAME_PRINT(&cd->procs[n]),
                             bo->size));
        prte_iof.push_stdin(&cd->procs[n], (uint8_t *) bo->bytes, bo->size);
    }

    if (NULL == bo->bytes || 0 == bo->size) {
        cd->cbfunc(PMIX_ERR_IOF_COMPLETE, cd->cbdata);
    } else {
        cd->cbfunc(PMIX_SUCCESS, cd->cbdata);
    }

    PMIX_RELEASE(cd);
}

pmix_status_t pmix_server_stdin_fn(const pmix_proc_t *source, const pmix_proc_t targets[],
                                   size_t ntargets, const pmix_info_t directives[], size_t ndirs,
                                   const pmix_byte_object_t *bo, pmix_op_cbfunc_t cbfunc,
                                   void *cbdata)
{
    // Note: We are ignoring the directives / ndirs at the moment
    PRTE_IO_OP(targets, ntargets, bo, pmix_server_stdin_push, cbfunc, cbdata);

    // Do not send PMIX_OPERATION_SUCCEEDED since the op hasn't completed yet.
    // We will send it back when we are done by calling the cbfunc.
    return PMIX_SUCCESS;
}

/************************ RESOURCE CHANGES *******************************************/
typedef struct{
    pmix_info_t *info;
    size_t ninfo;
}prte_pmix_info_caddy_t;

static void prte_pmix_info_relfn(void *cbdata){
    prte_pmix_info_caddy_t *cd = (prte_pmix_info_caddy_t *)cbdata;
    if(NULL != cd->info){
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    free(cd);    
}

void get_new_pset_name(char ** new_name){
    if (*new_name != NULL){
        return;
    }
    /* TODO: generate unique name */
    *new_name = strdup("dummy_name");
    return;
}

int proc_cmp(pmix_proc_t p1, pmix_proc_t p2){
    return (0 == strcmp(p1.nspace, p2.nspace) && p1.rank==p2.rank);
}

static pmix_status_t pset_intersection(pmix_server_pset_t p1, pmix_server_pset_t p2, pmix_proc_t *result, size_t *nmembers){
    size_t n, k;
    size_t res_ptr=0;
    size_t nprocs_max = p1.num_members +p2.num_members;

    /* Greedily fill in all procs from p2 which are not in p1 */
    for(n=0; n<p2.num_members; n++){
        int found=0;
        for(k=0; k<p1.num_members; k++){
            found+= proc_cmp(p2.members[n], p1.members[k]);
            if(found){
                PMIX_PROC_LOAD(&result[res_ptr],p2.members[n].nspace, p2.members[n].rank);
                res_ptr++;
                break;
            }
        }
    }
    *nmembers=res_ptr;
    return PMIX_SUCCESS;

}

static pmix_status_t pset_difference(pmix_server_pset_t p1, pmix_server_pset_t p2, pmix_proc_t *result, size_t *nmembers){
    size_t n, k;
    size_t res_ptr=0;
    size_t nprocs_max = p1.num_members +p2.num_members;

    /* Greedily fill in all procs from p2 which are not in p1 */
    for(n=0; n < p1.num_members; n++){
        int found = 0;
        for(k = 0; k < p2.num_members; k++){
            found += proc_cmp(p1.members[n], p2.members[k]);
        }
        if(0 == found){
            PMIX_PROC_LOAD(&result[res_ptr], p1.members[n].nspace, p1.members[n].rank);
            res_ptr++;
        }
    }
    *nmembers=res_ptr;
    return PMIX_SUCCESS;

}

static pmix_status_t pset_union(pmix_server_pset_t p1, pmix_server_pset_t p2, pmix_proc_t *result, size_t *nmembers){
    size_t n, k;
    size_t res_ptr=0;
    size_t nprocs_max = p1.num_members +p2.num_members;

    /* fill in all procs from p1 */
    for(n=0; n < p1.num_members; n++){
        PMIX_PROC_LOAD(&result[res_ptr],p1.members[n].nspace, p1.members[n].rank);
        res_ptr++;
    }
    /* Greedily fill in all procs from p2 which are not in p1 (b.c. procs from p1 were already added) */
    for(n=0; n < p2.num_members; n++){
        int found=0;
        for(k=0; k < p1.num_members; k++){
            found+= proc_cmp(p2.members[n], p1.members[k]);
        }
        if(0 == found){
            PMIX_PROC_LOAD(&result[res_ptr],p2.members[n].nspace, p2.members[n].rank);
            res_ptr++;
        }
    }
    *nmembers=res_ptr;

    return PMIX_SUCCESS;

}

static pmix_status_t pset_op_exec(pmix_psetop_directive_t directive, char *pset1_name, char *pset2_name, pmix_proc_t **result, size_t *nmembers){
    
    pmix_status_t ret;
    pmix_server_pset_t *pset_list_iter;
    pmix_server_pset_t *pset1=NULL;
    pmix_server_pset_t *pset2=NULL;


    /* Lookup if the specified psets exist */
    PMIX_LIST_FOREACH(pset_list_iter, &prte_pmix_server_globals.psets, pmix_server_pset_t){
        if(0 == strcmp(pset_list_iter->name, pset1_name))pset1=pset_list_iter;
        if(0 == strcmp(pset_list_iter->name, pset2_name))pset2=pset_list_iter;
    }
    if(pset1==NULL || pset2==NULL){
        return PMIX_ERR_NOT_FOUND;
    }

    ret = PMIX_SUCCESS;

    *result = NULL;
    switch(directive){
        case PMIX_PSETOP_UNION: {
            *result=(pmix_proc_t *)malloc((pset1->num_members+pset2->num_members)*sizeof(pmix_proc_t));
            ret = pset_union(*pset1, *pset2, *result, nmembers);
            break;
        }
        case PMIX_PSETOP_DIFFERENCE:{
            *result=(pmix_proc_t *)malloc(MAX(pset1->num_members, pset2->num_members)*sizeof(pmix_proc_t));
            ret = pset_difference(*pset1, *pset2, *result, nmembers);
            break;
        }
        case PMIX_PSETOP_INTERSECTION:{
            *result=(pmix_proc_t *)malloc(MAX(pset1->num_members, pset2->num_members)*sizeof(pmix_proc_t));
            ret = pset_intersection(*pset1, *pset2, *result, nmembers);
            break;
        }
        default: 
            ret = PMIX_ERR_BAD_PARAM;
    }
    if(PMIX_SUCCESS != ret){
        if(NULL != *result){
            free(*result);
        }
    }else{
        *result = realloc(*result, *nmembers * sizeof(pmix_proc_t));
    }

    return ret;
}



void pmix_server_define_pset(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                               prte_rml_tag_t tg, void *cbdata){

    int n = 1;
    pmix_status_t ret;
    size_t pset_size = 0;
    char * pset_name = (char*) malloc(PMIX_MAX_KEYLEN);
    memset(pset_name, 0, PMIX_MAX_KEYLEN);

    pmix_proc_t *pset_procs;
    /* unpack the size of the pset */
    if(PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &pset_size, &n, PMIX_SIZE))){
        PRTE_ERROR_LOG(ret);
        return;
    }
    /* unpack the name of the pset */
    if(PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &pset_name, &n, PMIX_STRING))){
        PRTE_ERROR_LOG(ret);
        return;
    }
    /* unpack the processes of the pset */
    PMIX_PROC_CREATE(pset_procs, pset_size);
    int p;
    for(p = 0; p < pset_size; p++){
        if(PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &pset_procs[p], &n, PMIX_PROC))){
            PRTE_ERROR_LOG(ret);
            if(NULL != pset_name){
                free(pset_name);
            }
            PMIX_PROC_RELEASE(pset_procs);
            return;
        }
    }
    /* add the pset to our server globals */
    pmix_server_pset_t *pset = PMIX_NEW(pmix_server_pset_t);
    pset->name = strdup(pset_name);
    pset->num_members = pset_size;
    PMIX_PROC_CREATE(pset->members, pset->num_members);
    memcpy(pset->members, pset_procs, pset_size * sizeof(pmix_proc_t));
    pmix_list_append(&prte_pmix_server_globals.psets, &pset->super);
    /* also pass it down to the pmix_server */
    //printf("pset define %s\n", pset_name);
    PMIx_server_define_process_set(pset_procs, pset_size, pset_name);
    PMIX_PROC_RELEASE(pset_procs);
    
    if(NULL != pset_name){
        free(pset_name);
    }

}

/* perform a pset operation and distribute the pset define command */
void pmix_server_define_pset_op(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                               prte_rml_tag_t tg, void *cbdata){


    pmix_status_t ret;
    size_t i;
    int n = 1;
    int p, room_num;
    pmix_psetop_directive_t directive;
    pmix_info_t *pset_op_info = NULL;
    pmix_data_buffer_t *buf_resp, *buf_all;
    pmix_proc_t *result_pset_members = NULL;
    char *pset1_name = NULL, *pset2_name = NULL, *pset_result_name = NULL;
    uint8_t op_cmd = PRTE_DYNRES_CLIENT_PSETOP;
    uint8_t def_cmd = PRTE_DYNRES_DEFINE_PSET;


    /* unpack the directive */
    if(PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &directive, &n, PMIX_UINT8))){
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }
    
    /* unpack the info objects */
    PMIX_INFO_CREATE(pset_op_info, 3);
    for(i = 0; i < 3; i++){
        
        if(PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &pset_op_info[i], &n, PMIX_INFO))){
            PRTE_ERROR_LOG(ret);
            goto ERROR;
        }
    }

    /* unpack the room number of the senders req tracker */
    if(PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &room_num, &n, PMIX_INT))){
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }

    /* unload the pset names */    
    
    size_t sz;
    ret=PMIX_SUCCESS;
    for (n = 0; n < 3; n++) {
        //printf("unloading value %d: key=%s\n", n, pset_op_info[n].key);
        if(0 == strcmp(pset_op_info[n].key, PMIX_PSETOP_P1)){
            PMIX_VALUE_UNLOAD(ret, &pset_op_info[n].value, (void**)&pset1_name, &sz);
        }else if(0 == strcmp(pset_op_info[n].key, PMIX_PSETOP_P2)){
            PMIX_VALUE_UNLOAD(ret, &pset_op_info[n].value, (void**)&pset2_name, &sz);
        }else if(0 == strcmp(pset_op_info[n].key, PMIX_PSETOP_PREF_NAME)){
            PMIX_VALUE_UNLOAD(ret, &pset_op_info[n].value, (void**)&pset_result_name, &sz);
        }
    }

    /* if we didn't get the names of the psets thats an error */ 
    if(PMIX_SUCCESS != ret || pset1_name==NULL || pset2_name==NULL){
        free(pset1_name);
        free(pset2_name);
        free(pset_result_name);
        goto ERROR;
    }
    
    /* get a name for the new pset based on client pref */
    get_new_pset_name(&pset_result_name);

    /* perform the pset operation */
    size_t nmembers;
    
    if(PMIX_SUCCESS!= (ret = pset_op_exec(directive, pset1_name, pset2_name, &result_pset_members, &nmembers))){
        goto ERROR;
    }

    /* first send an answer to the sender */
    n = 1;    
    PMIX_DATA_BUFFER_CREATE(buf_resp);
    /* pack the cmd */
    if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf_resp, &op_cmd, 1, PMIX_UINT8))){
        PMIX_DATA_BUFFER_RELEASE(buf_resp);
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }
    /* pack the directive */
    if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf_resp, &directive, 1, PMIX_UINT8))){
        PMIX_DATA_BUFFER_RELEASE(buf_resp);
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }
    /* pack the room number */
    if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf_resp, &room_num, 1, PMIX_INT))){
        PMIX_DATA_BUFFER_RELEASE(buf_resp);
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }
    /* pack the info objects with the names */
    for (i = 0; i < 3; i++) {
        if(0 == strcmp(pset_op_info[i].key, PMIX_PSETOP_PREF_NAME)){
            PMIX_INFO_LOAD(&pset_op_info[i], PMIX_PSETOP_PRESULT, pset_result_name, PMIX_STRING);
        }
        if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf_resp, &pset_op_info[i], 1, PMIX_INFO))){
            PMIX_DATA_BUFFER_RELEASE(buf_resp);
            PRTE_ERROR_LOG(ret);
            goto ERROR;
        }
    }
    /* pack the pset size */
    if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf_resp, &nmembers, 1, PMIX_SIZE))){
        PMIX_DATA_BUFFER_RELEASE(buf_resp);
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }
    /* pack the procs */
    for(p = 0; p < nmembers; p++){
        if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf_resp, &result_pset_members[p], 1, PMIX_PROC))){
            PMIX_DATA_BUFFER_RELEASE(buf_resp);
            PRTE_ERROR_LOG(ret);
            goto ERROR;
        }
    }
    //printf("send to sender\n");
    /* send it back to the sender */
    //prte_rml.send_buffer_nb(sender, buf_resp, PRTE_RML_TAG_MALLEABILITY, prte_rml_send_callback, NULL);
    PRTE_RML_SEND(ret, sender->rank, buf_resp, PRTE_RML_TAG_MALLEABILITY);

    /* send a PSET_DEFINE msg to all deamons but the sender, including ourself (if we are not the sender) */
    prte_job_t *daemon_job = prte_get_job_data_object(PRTE_PROC_MY_PROCID->nspace);
    pmix_proc_t target;
    PMIX_LOAD_PROCID(&target, PRTE_PROC_MY_HNP->nspace, 0);
    for(p = 0; p < daemon_job->num_procs; p++){
        target.rank = p;

        if(target.rank == sender->rank){
            continue;
        }

        /* pack the cmd */
        PMIX_DATA_BUFFER_CREATE(buf_all);
        if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf_all, &def_cmd, 1, PMIX_UINT8))){
            PMIX_DATA_BUFFER_RELEASE(buf_all);
            PRTE_ERROR_LOG(ret);
            goto ERROR;
        }
        /* pack the pset size */
        if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf_all, &nmembers, 1, PMIX_SIZE))){
            PMIX_DATA_BUFFER_RELEASE(buf_all);
            PRTE_ERROR_LOG(ret);
            goto ERROR;
        }
        /* pack the pset name */
        if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf_all, &pset_result_name, 1, PMIX_STRING))){
            PMIX_DATA_BUFFER_RELEASE(buf_all);
            PRTE_ERROR_LOG(ret);
            goto ERROR;
        }
        /* pack the procs */
        size_t proc;
        for(proc = 0; proc < nmembers; proc++){
            if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf_all, &result_pset_members[proc], 1, PMIX_PROC))){
                PMIX_DATA_BUFFER_RELEASE(buf_all);
                PRTE_ERROR_LOG(ret);
                goto ERROR;
            }
        }
        //printf("send to others\n");
        //prte_rml.send_buffer_nb(&target, buf_all, PRTE_RML_TAG_MALLEABILITY, prte_rml_send_callback, NULL);
        PRTE_RML_SEND(ret, target.rank, buf_all, PRTE_RML_TAG_MALLEABILITY);
    }
    goto CLEANUP;

ERROR:
    //printf("error\n");
    /* send a special CLIENT_PSET_OP msg to the sender indicating failure */
    PMIX_DATA_BUFFER_CREATE(buf_resp);
    pmix_psetop_directive_t null_directive = PMIX_PSETOP_NULL;
    if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf_resp, &op_cmd, 1, PMIX_UINT8))){
        /* if this fails there's nothing we can do */
        PMIX_DATA_BUFFER_RELEASE(buf_all);
        PRTE_ERROR_LOG(ret);
        goto CLEANUP;
    }
    if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf_resp, &room_num, 1, PMIX_UINT8))){
        PRTE_ERROR_LOG(ret);
    }
    if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf_resp, &null_directive, 1, PMIX_UINT8))){
        PRTE_ERROR_LOG(ret);
    }
    /* send a response to the sender even though there might be missing crucial information */
    //prte_rml.send_buffer_nb(sender, buf_resp, PRTE_RML_TAG_MALLEABILITY, prte_rml_send_callback, NULL);
    PRTE_RML_SEND(ret, sender->rank, buf_resp, PRTE_RML_TAG_MALLEABILITY);

CLEANUP:
    if(NULL != pset1_name){
        free(pset1_name);
    }
    if(NULL != pset2_name){
        free(pset2_name);
    }
    if(NULL != pset_result_name){
        free(pset_result_name);
    }
    if(NULL != result_pset_members){
        free(result_pset_members);
    }

    if(NULL != pset_op_info){
        PMIX_INFO_FREE(pset_op_info, 3);
    }
    //printf("return\n");
    return;
    
}

/* receive the answer of a psetop request from master and callback the waiting client */
void pmix_server_client_define_pset_op(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                               prte_rml_tag_t tg, void *cbdata){
    pmix_status_t ret;
    int n=1;
    size_t  i, nmembers;
    int p, room_num = INT_MIN;
    pmix_psetop_directive_t directive;
    pmix_info_t *pset_op_info = NULL;
    pmix_proc_t *pset_procs = NULL;
    pmix_server_req_t *req = NULL;
    uint8_t op_cmd = PRTE_DYNRES_CLIENT_PSETOP;
    uint8_t def_cmd = PRTE_DYNRES_DEFINE_PSET;

    /* unpack the directive */
    
    if(PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &directive, &n, PMIX_UINT8))){
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }
    if(PMIX_PSETOP_NULL == directive){
        goto ERROR;
    }

    /* unpack the room number of the senders req tracker */
    if(PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &room_num, &n, PMIX_INT))){
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }
    
    /* unpack the info objects */
    PMIX_INFO_CREATE(pset_op_info, 3);
    for(i = 0; i < 3; i++){
        if(PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &pset_op_info[i], &n, PMIX_INFO))){
            PRTE_ERROR_LOG(ret);
            goto ERROR;
        }
    }

    if(PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &nmembers, &n, PMIX_SIZE))){
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }

    /* unpack the procs */
    PMIX_PROC_CREATE(pset_procs, nmembers);
    for(i=0; i < nmembers; i++){
        if(PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &pset_procs[i], &n, PMIX_PROC))){
            PRTE_ERROR_LOG(ret);
            goto ERROR;
        }
    }
    /* unload the pset names */    
    char *pset1_name = NULL, *pset2_name = NULL, *pset_result_name=NULL;
    size_t sz;
    ret=PMIX_SUCCESS;
    for (n = 0; n < 3; n++) {
        if(0 == strcmp(pset_op_info[n].key, PMIX_PSETOP_P1)){
            PMIX_VALUE_UNLOAD(ret, &pset_op_info[n].value, (void**)&pset1_name, &sz);
        }else if(0 == strcmp(pset_op_info[n].key, PMIX_PSETOP_P2)){
            PMIX_VALUE_UNLOAD(ret, &pset_op_info[n].value, (void**)&pset2_name, &sz);
        }else if(0 == strcmp(pset_op_info[n].key, PMIX_PSETOP_PRESULT)){
            PMIX_VALUE_UNLOAD(ret, &pset_op_info[n].value, (void**)&pset_result_name, &sz);
        }
    }

    /* if we didn't get the names of the psets thats an error */ 
    if(PMIX_SUCCESS != ret || pset1_name==NULL || pset2_name==NULL){
        ret = PMIX_ERR_INVALID_ARG;
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }
    /* add the pset to our server globals */
    pmix_server_pset_t *pset = PMIX_NEW(pmix_server_pset_t);
    pset->name = strdup(pset_result_name);
    pset->num_members = nmembers;
    PMIX_PROC_CREATE(pset->members, pset->num_members);
    memcpy(pset->members, pset_procs, nmembers * sizeof(pmix_proc_t));
    pmix_list_append(&prte_pmix_server_globals.psets, &pset->super);
    /* call the callback function to release th client */
    pmix_hotel_checkout_and_return_occupant(&prte_pmix_server_globals.reqs, room_num, &req);
    pmix_info_t *reply;
    PMIX_INFO_CREATE(reply, 3);
    /* load the procs */
    pmix_data_array_t *proc_darry;
    PMIX_DATA_ARRAY_CREATE(proc_darry, nmembers, PMIX_PROC);
    pmix_proc_t *darray_procs = (pmix_proc_t *) proc_darry->array;
    for(n = 0; n < nmembers; n++){
        PMIX_PROC_LOAD(&darray_procs[n], pset_procs[n].nspace, pset_procs[n].rank);
    }

    PMIX_INFO_LOAD(&reply[0], PMIX_QUERY_PSET_MEMBERSHIP, proc_darry, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_FREE(proc_darry);
    /* load the pset name */
    PMIX_INFO_LOAD(&reply[1], PMIX_PSETOP_PRESULT, pset_result_name, PMIX_STRING);
    /* load the pset size */
    PMIX_INFO_LOAD(&reply[2], PMIX_PSETOP_PSET_SIZE, &nmembers, PMIX_SIZE); 
    
    prte_pmix_info_caddy_t *cd = malloc(sizeof(prte_pmix_info_caddy_t));
    cd->info=reply;
    cd->ninfo=3;
    req->psopcbfunc(PMIX_SUCCESS, directive, reply, 3, req->cbdata, prte_pmix_info_relfn, cd);

    /* everything worked fine, now cleanup and exit */
    goto CLEANUP;

ERROR:
    /* without the room number there's nothing we can do */
    if(room_num == INT_MIN){
        goto CLEANUP;
    }

    /* we can call the callback to inform the pmix_server about error */
    pmix_hotel_checkout_and_return_occupant(&prte_pmix_server_globals.reqs, room_num, &req);
    req->psopcbfunc(PMIX_ERR_SERVER_FAILED_REQUEST, PMIX_PSETOP_NULL, NULL, 0, req->cbdata, NULL, NULL);

CLEANUP:
    if(NULL != pset1_name){
        free(pset1_name);
    }
    if(NULL != pset2_name){
        free(pset2_name);
    }
    if(NULL != pset_result_name){
        free(pset_result_name);
    }
    if(NULL != pset_procs){
        PMIX_PROC_FREE(pset_procs, nmembers);
    }
    if(NULL != pset_op_info){
        PMIX_INFO_FREE(pset_op_info, 3);
    }
    if(NULL != req){
        PMIX_RELEASE(req);
    }
    //printf("server_client return\n");
    return;        

}

/* Make the specified resource change available for queries from the local clients */
void pmix_server_define_res_change(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                               prte_rml_tag_t tg, void *cbdata){
    int n = 1, ret;
    prte_res_change_t *res_change = PMIX_NEW(prte_res_change_t);
    pmix_res_change_type_t rc_type;
    char *delta_pset_name = (char*) malloc(PMIX_MAX_KEYLEN);
    char *assoc_pset_name = (char*) malloc(PMIX_MAX_KEYLEN);

    if(NULL == daemon_timing_list)
        daemon_timing_list = (node_t *) calloc(1, sizeof(node_t));
    init_add_timing(daemon_timing_list, &cur_daemon_timing_frame, sizeof(timing_frame_daemon_t));
    make_timestamp_base(&cur_daemon_timing_frame->rc_publish_start);
    
    /* unpack the type of the resource change */
    ret = PMIx_Data_unpack(NULL, buffer, &res_change->rc_type, &n, PMIX_UINT8);
    if (PMIX_SUCCESS != ret) {
        PMIX_RELEASE(res_change);
        PMIX_ERROR_LOG(ret);
        return;
    }
    /* unpack the name of the pset describing the resource change */
    ret = PMIx_Data_unpack(NULL, buffer, &delta_pset_name, &n, PMIX_STRING);
    if (PMIX_SUCCESS != ret) {
        PMIX_RELEASE(res_change);
        PMIX_ERROR_LOG(ret);
        return;
    }
    strncpy(res_change->rc_pset, delta_pset_name, PMIX_MAX_KEYLEN);
    free(delta_pset_name);

    /* unpack the name of the associated pset of the resource change */
    ret = PMIx_Data_unpack(NULL, buffer, &assoc_pset_name, &n, PMIX_STRING);
    if (PMIX_SUCCESS != ret) {
        PMIX_RELEASE(res_change);
        PMIX_ERROR_LOG(ret);
        return;
    }
    strncpy(res_change->associated_pset, assoc_pset_name, PMIX_MAX_KEYLEN);
    free(assoc_pset_name);

    /* Dump job info before resource change */
    prte_job_t *job_to_print = NULL;
    pmix_server_pset_t *rc_pset_ptr;
    PMIX_LIST_FOREACH(rc_pset_ptr, &prte_pmix_server_globals.psets, pmix_server_pset_t){
        if(0 == strcmp(rc_pset_ptr->name, res_change->rc_pset)){
            job_to_print = prte_get_job_data_object(rc_pset_ptr->members[0].nspace);
            break;
        }
    }
    

    /* initialized the tracking */
    res_change->nlocalprocs = res_change->nlocalprocs_finalized = res_change->nglobalprocs_finalized = res_change->nglobalprocs_terminated = 0;
    res_change->queryable = true;

    set_res_change_id(&cur_daemon_timing_frame->res_change_id, res_change->rc_pset);
    cur_daemon_timing_frame->res_change_type = res_change->rc_type;
    /* For resource substraction we need to save the number of local processes
     * so that we can keep track of finalizing clients
     * If all local clients of this res change finalized we will inform the master
     * and if the master received all local contributions the resource can be substracted
     */  

    PMIX_LIST_FOREACH(rc_pset_ptr, &prte_pmix_server_globals.psets, pmix_server_pset_t){
        if(0 == strcmp(rc_pset_ptr->name, res_change->rc_pset)){
            cur_daemon_timing_frame->res_change_size = rc_pset_ptr->num_members;
            if(PMIX_RES_CHANGE_SUB == res_change->rc_type){
                size_t p, c;
                prte_proc_t *local_child;
                prte_node_t *local_node = prte_get_proc_object(PRTE_PROC_MY_NAME)->node;
                if(NULL == local_node){
                    break;
                }
                for(p = 0; p < rc_pset_ptr->num_members; p++){
                    pmix_proc_t rc_proc = rc_pset_ptr->members[p];
                    for(c = 0; c < local_node->procs->size; c++){
                        if(NULL == (local_child = pmix_pointer_array_get_item(local_node->procs, c))){
                            continue;
                        }else{
                        }
                        if(PMIX_CHECK_PROCID(&rc_proc, &local_child->name)){
                            res_change->nlocalprocs++;
                            break;
                        }
                    }
                }
                res_change->nprocs = rc_pset_ptr->num_members;
                break;
            }
        }
    }
    
    
    /* add the resource change to the local server globals */
    pmix_list_append(&prte_pmix_server_globals.res_changes, &res_change->super);

    make_timestamp_base(&cur_daemon_timing_frame->rc_publish_end);

}

pmix_status_t update_job_data_sub(prte_res_change_t *res_change){
    
    int a, n, p, ret, count;
    prte_job_t *jdata;
    prte_app_context_t *app;
    prte_node_t *node;
    pmix_proc_t rc_proc;
    prte_proc_t *job_proc; 
    pmix_server_pset_t *rc_pset;
    bool found_pset = false;

    //printf("%s: UPDATE_JOB_DATA_SUB\n", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* get the delta pset */
    PMIX_LIST_FOREACH(rc_pset, &prte_pmix_server_globals.psets, pmix_server_pset_t){
        if(0 == strcmp(rc_pset->name, res_change->rc_pset)){
            found_pset = true;
            break;
        }
    }

    /* get the job data */
    if (!found_pset || NULL == (jdata = prte_get_job_data_object(rc_pset->members[0].nspace))){
        return PMIX_ERR_NOT_FOUND;
    }
    
    /* update job data */
    for(p = 0; p < rc_pset->num_members; p++){
        rc_proc = rc_pset->members[p];
        /* update top level job data */
        for(n = 0; n < jdata->procs->size; n++){
            if(NULL == (job_proc = pmix_pointer_array_get_item(jdata->procs, n))){
                continue;
            }
            if(PMIX_CHECK_PROCID(&rc_proc, &job_proc->name)){
                if(PMIX_CHECK_RANK(PRTE_PROC_MY_NAME->rank, job_proc->parent)){
                    --jdata->num_local_procs;
                }
                pmix_pointer_array_set_item(jdata->procs, n, NULL);
                /* as we remove the process we need to update both, num_procs and num terminated */
                --jdata->num_procs;
                --jdata->num_terminated;
            }
        }
        /* Update app_context */
        for(a = 0; a < jdata->apps->size; a++){
            if(NULL == (app = pmix_pointer_array_get_item(jdata->apps, a))){
                continue;
            }
            for(n = 0; n < app->procs.size; n++){
                if(NULL == (job_proc = pmix_pointer_array_get_item(&app->procs, n))){
                    continue;
                }
                if(PMIX_CHECK_PROCID(&rc_proc, & job_proc->name)){
                    pmix_pointer_array_set_item(&app->procs, n, NULL);
                    --app->num_procs;
                }
            }
        }
        /* Update job map */
        for(a = 0; a < jdata->map->nodes->size; a++){
            if(NULL == (node = pmix_pointer_array_get_item(jdata->map->nodes, a))){
                continue;
            }
            /* update node */
            for(n = 0; n < node->procs->size; n++){
                if(NULL == (job_proc = pmix_pointer_array_get_item(node->procs, n))){
                    continue;
                }
                if(PMIX_CHECK_PROCID(&rc_proc, &job_proc->name)){
                    pmix_pointer_array_set_item(node->procs, n, NULL);
                    --node->num_procs;
                    /* stack assumption */
                    --node->next_node_rank;
                    --node->slots_inuse;
                    ++node->slots_available;
                }
            }
            /* node empty? We should remove it from the map */
            count = 0;
            for(n = 0; n < node->procs->size; n++){
                if(NULL == (job_proc = pmix_pointer_array_get_item(node->procs, n))){
                    continue;
                }
                if(PMIX_CHECK_NSPACE(job_proc->name.nspace, rc_pset->members[0].nspace)){
                    ++count;
                }
            }
            if(0 == count){
                //PMIX_RELEASE(node);
                pmix_pointer_array_set_item(jdata->map->nodes, a, NULL);                
                --jdata->map->num_nodes;
            }
            for(n = 0; n < prte_local_children->size; n++){
                if(NULL == (job_proc = pmix_pointer_array_get_item(prte_local_children, n))){
                    continue;
                }
                if(PMIX_CHECK_PROCID(&rc_proc, &job_proc->name)){
                    //printf("%s: removing local child: %d\n", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), rc_proc.rank);
                    pmix_pointer_array_set_item(prte_local_children, n, NULL);
                }
                /* Now we can release the proc object */
                //PMIX_RELEASE(job_proc);
            }
        }
    }
    return PMIX_SUCCESS;
}

/* Make the specified resource change unavailable for queries from the local clients */
void pmix_server_unpublish_res_change(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                               prte_rml_tag_t tg, void *cbdata){
    int n = 1, ret;
    bool non_default=true;
    prte_res_change_t *res_change = NULL, *res_change_next = NULL;
    char *rc_pset = (char*) malloc(PMIX_MAX_KEYLEN);
    pmix_proc_t notifier;
    make_timestamp_base(&cur_daemon_timing_frame->rc_unpublish);

    /* unpack the name of the pset describing the resource change */
    ret = PMIx_Data_unpack(NULL, buffer, &rc_pset , &n, PMIX_STRING);
    if (PMIX_SUCCESS != ret) {
        free(rc_pset);
        PMIX_RELEASE(res_change);
        PMIX_ERROR_LOG(ret);
        return;
    }
    /* find the specified resource change in the local server globals and remove it */
    PMIX_LIST_FOREACH_SAFE(res_change, res_change_next, &prte_pmix_server_globals.res_changes, prte_res_change_t){
        if(0 == strcmp(rc_pset, res_change->rc_pset)){

            res_change->queryable = false;

            if(PMIX_RES_CHANGE_SUB == res_change->rc_type){
                
                /* Notify local clients */
                pmix_info_t *event_info;
                PMIX_INFO_CREATE(event_info, 2);
                (void)snprintf(event_info[0].key, PMIX_MAX_KEYLEN, "%s", PMIX_EVENT_NON_DEFAULT);
                PMIX_VALUE_LOAD(&event_info[0].value, &non_default, PMIX_BOOL);
                (void)snprintf(event_info[1].key, PMIX_MAX_KEYLEN, "%s", PMIX_PSET_NAME);
                PMIX_VALUE_LOAD(&event_info[1].value, rc_pset, PMIX_STRING);
                ret = PMIx_Notify_event(PMIX_RC_FINALIZED, NULL, PMIX_RANGE_LOCAL, event_info, 2, NULL, NULL);
                PMIX_INFO_FREE(event_info, 2);

            }else{
                /* Dump job info before resource change */
                prte_job_t *job_to_print = NULL;
                pmix_server_pset_t *rc_pset_ptr;
                PMIX_LIST_FOREACH(rc_pset_ptr, &prte_pmix_server_globals.psets, pmix_server_pset_t){
                    if(0 == strcmp(rc_pset_ptr->name, res_change->rc_pset)){
                        job_to_print = prte_get_job_data_object(rc_pset_ptr->members[0].nspace);
                        break;
                    }
                }
                pmix_list_remove_item(&prte_pmix_server_globals.res_changes, &res_change->super);
                make_timestamp_base(&cur_daemon_timing_frame->rc_end);
                make_timestamp_root(&cur_master_timing_frame->rc_end2);
            }
            break;
        }
    }

    free(rc_pset);
    
    
}

/* A daemon reports local finalization of all procs included in a resource change. 
 * As master we need to keep track of the total number of finalied clients in the resource change
 * If all clients of the resource change finalized we need to send the DELETE_RES_CHANGE message to all damons 
 */

void pmix_server_local_finalization_reported(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                               prte_rml_tag_t tg, void *cbdata){
    int n = 1, p, ret;
    size_t nprocs;
    pmix_data_buffer_t *buf;
    prte_daemon_cmd_flag_t command = PRTE_DYNRES_UNPUBLISH_RES_CHANGE;
    bool non_default=true;
    prte_res_change_t *res_change = NULL, *res_change_next = NULL;
    char *rc_pset = (char*) malloc(PMIX_MAX_KEYLEN);
    

    /* unpack the name of the pset describing the resource change */
    ret = PMIx_Data_unpack(NULL, buffer, &rc_pset , &n, PMIX_STRING);
    if (PMIX_SUCCESS != ret) {
        free(rc_pset);
        PMIX_RELEASE(res_change);
        PMIX_ERROR_LOG(ret);
        return;
    }
    /* unpack the number of finalized local procs */
    ret = PMIx_Data_unpack(NULL, buffer, &nprocs , &n, PMIX_SIZE);
    if (PMIX_SUCCESS != ret) {
        free(rc_pset);
        PMIX_RELEASE(res_change);
        PMIX_ERROR_LOG(ret);
        return;
    }

    /* find the specified resource change in the local server globals and update the number of finalized procs */
    PMIX_LIST_FOREACH_SAFE(res_change, res_change_next, &prte_pmix_server_globals.res_changes, prte_res_change_t){
        if(0 == strcmp(rc_pset, res_change->rc_pset)){
            res_change->nglobalprocs_finalized += nprocs;
            //printf("%d of %d procs of res change %s finalized\n",res_change->nglobalprocs_finalized, res_change->nprocs, rc_pset);
            /* If all procs finlized we need to inform the daemons and clients to delete the resource change */
            if(res_change->nglobalprocs_finalized >= res_change->nprocs){
                //printf("        all procs of res change %s finalized\n", rc_pset);
                make_timestamp_base(&cur_master_timing_frame->rc_deregistered);
                
                prte_job_t *daemon_job = prte_get_job_data_object(PRTE_PROC_MY_PROCID->nspace);
                pmix_proc_t target;
                PMIX_LOAD_PROCID(&target, PRTE_PROC_MY_HNP->nspace, 0);
                for(p = 0; p < daemon_job->num_procs; p++){
                    target.rank = p;

                    PMIX_DATA_BUFFER_CREATE(buf);
                    /* pack the command */
                    if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &command, 1, PMIX_UINT8))){
                        free(rc_pset);
                        PMIX_DATA_BUFFER_RELEASE(buf);
                        PMIX_ERROR_LOG(ret);
                        return;
                    }
                    /* pack the delta pset name of the resource change */
                    if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &rc_pset, 1, PMIX_STRING))){
                        free(rc_pset);
                        PMIX_DATA_BUFFER_RELEASE(buf);
                        PMIX_ERROR_LOG(ret);
                        return;
                    }
                    //prte_rml.send_buffer_nb(&target, buf, PRTE_RML_TAG_MALLEABILITY, prte_rml_send_callback, NULL);
                    PRTE_RML_SEND(ret, target.rank, buf, PRTE_RML_TAG_MALLEABILITY);
                }
            }
            break;
        }
    }

    free(rc_pset);
}

/* All processes of the resource change terminated so update job data and remove resource change
 */

void pmix_server_res_change_complete(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                               prte_rml_tag_t tg, void *cbdata){
    int n = 1, p, ret;
    size_t nprocs;
    pmix_data_buffer_t *buf;
    bool non_default=true;
    prte_res_change_t *res_change, *res_change_next;
    char *rc_pset = (char*) malloc(PMIX_MAX_KEYLEN);
    make_timestamp_base(&cur_daemon_timing_frame->rc_finalize);
    //printf("%s: PMIX_SERVER_RES_CHANGE_COMPLETE\n", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* unpack the name of the pset describing the resource change */
    ret = PMIx_Data_unpack(NULL, buffer, &rc_pset , &n, PMIX_STRING);
    if (PMIX_SUCCESS != ret) {
        free(rc_pset);
        PMIX_ERROR_LOG(ret);
        return;
    }
    /* find the specified resource change in the local server globals, update job data and remove it */
    PMIX_LIST_FOREACH_SAFE(res_change, res_change_next, &prte_pmix_server_globals.res_changes, prte_res_change_t){
        if(0 == strcmp(rc_pset, res_change->rc_pset)){

            if(PMIX_RES_CHANGE_SUB == res_change->rc_type){
                update_job_data_sub(res_change);
            }
            pmix_list_remove_item(&prte_pmix_server_globals.res_changes, &res_change->super);
            make_timestamp_root(&cur_master_timing_frame->rc_end2);
            break;
        }
    }
    ///* Dump job info before resource change */
    //prte_job_t *job_to_print = NULL;
    //pmix_server_pset_t *rc_pset_ptr;
    //PMIX_LIST_FOREACH(rc_pset_ptr, &prte_pmix_server_globals.psets, pmix_server_pset_t){
    //    if(0 == strcmp(rc_pset_ptr->name, res_change->rc_pset)){
    //        job_to_print = prte_get_job_data_object(rc_pset_ptr->members[0].nspace);
    //        break;
    //    }
    //}
    make_timestamp_base(&cur_daemon_timing_frame->rc_end);
    free(rc_pset);
}

static char *get_prted_comm_cmd_str(prte_daemon_cmd_flag_t command);

void pmix_server_dynres(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                               prte_rml_tag_t tg, void *cbdata)
{
    int n, ret;
    prte_daemon_cmd_flag_t command;
    char *cmd_str = NULL;

    n = 1;
    ret = PMIx_Data_unpack(NULL, buffer, &command, &n, PMIX_UINT8);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        return;
    }

    cmd_str = get_prted_comm_cmd_str(command);
    PRTE_OUTPUT_VERBOSE((2, prte_debug_output,
                         "%s prted:dynres:process_commands() Processing Command: %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), cmd_str));
    //printf("%s prted:dynres:process_commands() Processing Command: %s\n",
    //                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), cmd_str);
    free(cmd_str);
    cmd_str = NULL; 

    switch (command) {
    case PRTE_DYNRES_DEFINE_PSET:
        pmix_server_define_pset(status, sender, buffer, tg, cbdata);
        break;
    case PRTE_DYNRES_SERVER_PSETOP:
        pmix_server_define_pset_op(status, sender, buffer, tg, cbdata);
        break;
    case PRTE_DYNRES_CLIENT_PSETOP:
        pmix_server_client_define_pset_op(status, sender, buffer, tg, cbdata);
        break;
    case PRTE_DYNRES_DEFINE_RES_CHANGE:
        pmix_server_define_res_change(status, sender, buffer, tg, cbdata);
        break;
    case PRTE_DYNRES_UNPUBLISH_RES_CHANGE:
        pmix_server_unpublish_res_change(status, sender, buffer, tg, cbdata);
        break;
    case PRTE_DYNRES_LOCAL_PROCS_FINALIZED:
        pmix_server_local_finalization_reported(status, sender, buffer, tg, cbdata);
        break;
    case PRTE_DYNRES_FINALIZE_RES_CHANGE:
        pmix_server_res_change_complete(status, sender, buffer, tg, cbdata);
        break;
    }
    

}

static char *get_prted_comm_cmd_str(prte_daemon_cmd_flag_t command)
{
    switch (command) {
    case PRTE_DYNRES_DEFINE_PSET:
        return strdup("PRTE_DYNRES_DEFINE_PSET");
    case PRTE_DYNRES_SERVER_PSETOP:
        return strdup("PRTE_DYNRES_SERVER_PSETOP");
    case PRTE_DYNRES_CLIENT_PSETOP:
        return strdup("PRTE_DYNRES_CLIENT_PSETOP");
    case PRTE_DYNRES_DEFINE_RES_CHANGE:
        return strdup("PRTE_DYNRES_DEFINE_RES_CHANGE");
    case PRTE_DYNRES_UNPUBLISH_RES_CHANGE:
        return strdup("PRTE_DYNRES_UNPUBLISH_RES_CHANGE");
    case PRTE_DYNRES_FINALIZE_RES_CHANGE:
        return strdup("PRTE_DYNRES_FINALIZE_RES_CHANGE");
    case PRTE_DYNRES_LOCAL_PROCS_FINALIZED:
        return strdup("PRTE_DYNRES_LOCAL_PROCS_FINALIZED");
    }

    return NULL;
    
}

pmix_status_t pset_operation_fn( const pmix_proc_t *client,
                                        pmix_psetop_directive_t directive,
                                        const pmix_info_t data[], size_t ndata,
                                        pmix_psetop_cbfunc_t cbfunc, void *cbdata)
{
    size_t n, k, sz;
    int room_num = INT_MIN;
    pmix_status_t rc;
    pmix_info_t *info = NULL;
    pmix_server_req_t *req = NULL;
    char *pset1_name = NULL, *pset2_name = NULL, *pset_result_name = NULL;
    char pset_buf[PMIX_MAX_KEYLEN];
    if (NULL == cbfunc) {
        PRTE_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        goto CLEANUP;
    }

    /* unload the pset names */
    for (n = 0; n < ndata; n++) {
        //printf("unloading value %d: key=%s\n", n, data[n].key);
        if(0 == strcmp(data[n].key, PMIX_PSETOP_P1)){
            PMIX_VALUE_UNLOAD(rc, &data[n].value, (void**)&pset1_name, &sz);
        }else if(0 == strcmp(data[n].key, PMIX_PSETOP_P2)){
            PMIX_VALUE_UNLOAD(rc, &data[n].value, (void**)&pset2_name, &sz);
        }else if(0 == strcmp(data[n].key, PMIX_PSETOP_PREF_NAME)){
            PMIX_VALUE_UNLOAD(rc, &data[n].value, (void**)&pset_result_name, &sz);
        }
        //printf("value=%s\n", data[n].value.data.string);
    }
    
    if(pset1_name == NULL || pset2_name == NULL){
        goto ERROR;
    }


    /* if they didnt provide a pref name just set a dummy name */
    if(NULL == pset_result_name){
        sprintf(pset_buf, "dummy_pset_name_%d", dummy_name_ctr++);
        pset_result_name = strdup(pset_buf);
    }
    /* load the names into an info object */
    PMIX_INFO_CREATE(info, 3);
    PMIX_INFO_LOAD(&info[0], PMIX_PSETOP_PREF_NAME, pset_result_name, PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_PSETOP_P1, pset1_name, PMIX_STRING);
    PMIX_INFO_LOAD(&info[2], PMIX_PSETOP_P2, pset2_name, PMIX_STRING); 

    
    int ret;
    pmix_data_buffer_t *buf;
    PMIX_DATA_BUFFER_CREATE(buf);
    prte_daemon_cmd_flag_t cmd = PRTE_DYNRES_SERVER_PSETOP;
    if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &cmd, 1, PMIX_UINT8))){
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }
    if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &directive, 1, PMIX_UINT8))){
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }
    if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &info[0], 1, PMIX_INFO))){
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }
    if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &info[1], 1, PMIX_INFO))){
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }
    if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &info[2], 1, PMIX_INFO))){
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }

    req = PMIX_NEW(pmix_server_req_t);
    req->psopcbfunc = cbfunc;
    req->cbdata = cbdata;
    if(PMIX_SUCCESS != (ret = pmix_hotel_checkin(&prte_pmix_server_globals.reqs, req, &room_num))){
        PMIX_RELEASE(req);
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }
    if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &room_num, 1, PMIX_INT))){
        PMIX_RELEASE(req);
        pmix_hotel_checkout(&prte_pmix_server_globals.reqs, room_num);
        PRTE_ERROR_LOG(ret);
        goto ERROR;
    }
    //printf("send pset op\n");
    //prte_rml.send_buffer_nb(PRTE_PROC_MY_HNP, buf, PRTE_RML_TAG_MALLEABILITY, prte_rml_send_callback, NULL);
    PRTE_RML_SEND(ret, PRTE_PROC_MY_HNP->rank, buf, PRTE_RML_TAG_MALLEABILITY);

    /* We have successfully sent the request to the master. 
     * The pmix server will be called back when we receive an answer.
     * So for now we can cleanup and return */
    goto CLEANUP;
       
ERROR:
    //printf("error\n");
    /* if we encountered an error directly callback the pmix server with an error status */    
    cbfunc(PMIX_ERR_SERVER_FAILED_REQUEST, PMIX_PSETOP_NULL, NULL, 0, cbdata, NULL, NULL);

CLEANUP:
    //printf("cleanup\n");
    if(NULL != pset_result_name){
        free(pset_result_name);
    }
    if(NULL != pset1_name){
        free(pset1_name);
    }
    if(NULL != pset2_name){
        free(pset2_name);
    }
    if(NULL != info){
        PMIX_INFO_FREE(info, 3);
    }
    //printf("return success\n");
    return PMIX_SUCCESS;
}



