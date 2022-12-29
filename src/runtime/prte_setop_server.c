/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012-2016 Los Alamos National Security, LLC.
 *                         All rights reserved
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017-2018 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <string.h>

#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif

#include "src/class/pmix_pointer_array.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/util/name_fns.h"

#include "src/runtime/prte_setop_server.h"


static int dummy_name_ctr = 0;
static pmix_rank_t highest_job_rank = 0;
static char *prte_pset_base_name = "prrte://base_name/";
static pmix_list_t *highest_rank_ever_list = NULL;

void setop_server_init(){
    highest_rank_ever_list = PMIX_NEW(pmix_list_t);
}

void setop_server_close(){
    PMIX_RELEASE(highest_rank_ever_list);
}


pmix_status_t set_highest_job_rank(pmix_nspace_t nspace, pmix_rank_t highest_rank){
    pmix_data_array_t darray;
    prte_info_item_t *info = NULL, *info_array;

    if(NULL == highest_rank_ever_list){
        return PMIX_ERR_INIT;
    }
    PMIX_LIST_FOREACH(info, highest_rank_ever_list, prte_info_item_t){
        if(PMIX_CHECK_KEY(&info->info, nspace)){
            PMIX_VALUE_LOAD(&info->info.value, &highest_rank, PMIX_PROC_RANK);
            return PMIX_SUCCESS;
        }
    }

    info = PMIX_NEW(prte_info_item_t);

    PMIX_INFO_LOAD(&info->info, nspace, &highest_rank, PMIX_PROC_RANK);
    pmix_list_append(highest_rank_ever_list, &info->super);
    return PMIX_SUCCESS;
}

pmix_status_t get_highest_job_rank(pmix_nspace_t nspace, pmix_rank_t *highest_rank){
    pmix_data_array_t darray;
    prte_info_item_t *info = NULL, *info_array;

    if(NULL == highest_rank_ever_list){
        return PMIX_ERR_INIT;
    }
    PMIX_LIST_FOREACH(info, highest_rank_ever_list, prte_info_item_t){
        if(PMIX_CHECK_KEY(&info->info, nspace)){
            *highest_rank = info->info.value.data.rank;
            return PMIX_SUCCESS;
        }
    }
    return PMIX_ERR_NOT_FOUND;
}

static void prte_setop_con(prte_setop_t *setop){
    setop->op = PMIX_PSETOP_NULL;
    setop->input_names = NULL;
    setop->n_input_names = 0;
    setop->output_names = NULL;
    setop->n_output_names = 0;
    setop->op_info = NULL;
    setop->n_op_info = 0;
    setop->pset_info_arrays = NULL;
    setop->npset_info_arrays = 0;
}
static void prte_setop_des(prte_setop_t *setop){
    if(0 != setop->npset_info_arrays){
        free(setop->pset_info_arrays);
    }
}

PMIX_CLASS_INSTANCE(prte_setop_t, pmix_list_item_t, prte_setop_con, prte_setop_des);

void get_new_pset_name(char ** new_name){
    if (*new_name != NULL){
        return;
    }
    /* TODO: generate unique name */
    *new_name = strdup("dummy_name");
    return;
}

int proc_cmp(pmix_proc_t p1, pmix_proc_t p2){
    return (0 == strcmp(p1.nspace, p2.nspace) && p1.rank == p2.rank);
}

int prte_pset_define_from_parray(char *pset_name, pmix_pointer_array_t *parray, prte_pset_flags_t flags){
    pmix_data_buffer_t *buf;
    prte_daemon_cmd_flag_t cmd = PRTE_DYNRES_DEFINE_PSET;
    int ret, ndaemons = prte_process_info.num_daemons;
    pmix_proc_t daemon_procid;

    size_t n, i, size = 0;

    for(n = 0; n < parray->size; n++){
        prte_proc_t *prte_proc = (prte_proc_t *) pmix_pointer_array_get_item(parray, n);
        if(NULL != prte_proc){
            size ++;
        }
    }

    PMIX_LOAD_PROCID(&daemon_procid, PRTE_PROC_MY_HNP->nspace, 0);
    for(i = 0; i < ndaemons; i++){
        PMIX_DATA_BUFFER_CREATE(buf);
        if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &cmd, 1, PMIX_UINT8))){
            PRTE_ERROR_LOG(ret);
            return ret;            
        }
        if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &size, 1, PMIX_SIZE))){
            PRTE_ERROR_LOG(ret);
            return ret;            
        }

        if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &pset_name, 1, PMIX_STRING))){
            PRTE_ERROR_LOG(ret);
            return ret;            
        }

        /* pack the PSet flags */
        if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &flags, 1, PMIX_UINT16))){
            PRTE_ERROR_LOG(ret);
            return ret;
        }    
    
    
        for(n = 0; n < parray->size; n++){
            prte_proc_t *prte_proc = (prte_proc_t *) pmix_pointer_array_get_item(parray, n);
            if(NULL != prte_proc){
                if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &prte_proc->name, 1, PMIX_PROC))){
                    PRTE_ERROR_LOG(ret);
                    return ret;
                }
            }
        }

        daemon_procid.rank = i;
        //prte_rml.send_buffer_nb(&daemon_procid, buf, PRTE_RML_TAG_MALLEABILITY, prte_rml_send_callback, NULL);
        PRTE_RML_SEND(ret, daemon_procid.rank, buf, PRTE_RML_TAG_MALLEABILITY);
    }
    return ret;
}

int prte_ophandle_get_num_ops(pmix_info_t *rc_handle, size_t *num_ops){
    size_t n, i, ninfo;
    
    pmix_info_t  *info, *info2;
    prte_setop_t *setop;

    for(n = 0; n < rc_handle[0].value.data.darray->size; n++){

        info = (pmix_info_t *) rc_handle[0].value.data.darray->array;
        ninfo = rc_handle[0].value.data.darray->size; 
        for(i = 0; i < ninfo; i++){
            if(PMIX_CHECK_KEY(&info[i], "mpi.set_op_handles")){
                info2 = (pmix_info_t *) info[i].value.data.darray->array;
                *num_ops = 1 + info[i].value.data.darray->size;
                return PMIX_SUCCESS;
            }
        }        
    }
    return PMIX_ERR_BAD_PARAM;
}

int prte_ophandle_get_nth_op(pmix_info_t *rc_handle, size_t index, prte_setop_t **_setop){
    size_t n, k, m, i, ninfo, nsetop_info, ninfo2;
    
    size_t n_op_output = 0;
    char ** output = NULL;

    pmix_value_t *val_ptr;
    pmix_info_t *setop_info, *info, *info2;
    prte_setop_t *setop;

    for(n = 0; n < rc_handle[0].value.data.darray->size; n++){
        if(0 == index){
            setop_info = (pmix_info_t *) rc_handle[0].value.data.darray->array;
            nsetop_info = rc_handle[0].value.data.darray->size;
        }else{
            info = (pmix_info_t *) rc_handle[0].value.data.darray->array;
            ninfo = rc_handle[0].value.data.darray->size; 

            for(i = 0; i < ninfo; i++){
                if(PMIX_CHECK_KEY(&info[i], "mpi.set_op_handles")){

                    /*set op handles */
                    info2 = (pmix_info_t *) info[i].value.data.darray->array;

                    if(index - 1 >= info[i].value.data.darray->size){
                        return PMIX_ERR_BAD_PARAM;
                    }

                    setop_info = (pmix_info_t *) info2[index - 1].value.data.darray->array;
                    nsetop_info = info2[index - 1].value.data.darray->size;
                }
            }
        }
    }

    *_setop = PMIX_NEW(prte_setop_t);
    setop = *_setop;
    for(n = 0; n < nsetop_info; n++){
        
        /* Get the op type */
        if(PMIX_CHECK_KEY(&setop_info[n], PMIX_PSETOP_TYPE)){
            setop->op = setop_info[n].value.data.uint8;
        }else if(PMIX_CHECK_KEY(&setop_info[n], "mpi.op_info")){            
            info2 = (pmix_info_t *) setop_info[n].value.data.darray->array;
            ninfo2 = setop_info[n].value.data.darray->size;
            for(k = 0; k < ninfo2; k++){
                if(PMIX_CHECK_KEY(&info2[k], "mpi.op_info.info")){                   
                    setop->n_op_info = info2[k].value.data.darray->size;
                    setop->op_info = (pmix_info_t *) info2[k].value.data.darray->array;
                }
                /* Get the input sets */
                else if(PMIX_CHECK_KEY(&info2[k], "mpi.op_info.input")){                    
                    setop->input_names = (pmix_value_t *) info2[k].value.data.darray->array;
                    setop->n_input_names = info2[k].value.data.darray->size;
                }
                /* Get the output sets */
                else if(PMIX_CHECK_KEY(&info2[k], "mpi.op_info.output")){
                    setop->output_names = (pmix_value_t *) info2[k].value.data.darray->array;
                    setop->n_output_names = info2[k].value.data.darray->size;
                }
                /* Get the set infos */
                else if(PMIX_CHECK_KEY(&info2[k], "mpi.op_info.set_info")){
                    
                    val_ptr = (pmix_value_t *) info2[k].value.data.darray->array;
                    setop->npset_info_arrays = info2[k].value.data.darray->size;
                    setop->pset_info_arrays = malloc(setop->npset_info_arrays * sizeof(pmix_data_array_t *));
                    for(i = 0; i < setop->npset_info_arrays; i++){
                        setop->pset_info_arrays[i] = val_ptr[i].data.darray;
                    }
                }
            }
        }
    }
    return PMIX_SUCCESS;
}

pmix_status_t prte_op_handle_verify(pmix_info_t *op_handle){
    size_t num_ops, n, k, j;
    int rc;
    prte_setop_t *setop;
    prte_res_change_t *res_change;

    if(PMIX_SUCCESS != prte_ophandle_get_num_ops(op_handle, &num_ops)){
        return rc;
    }

    for(n = 0; n < num_ops; n++){
        if(PMIX_SUCCESS != prte_ophandle_get_nth_op(op_handle, n, &setop)){
            return rc;
        }

        if((PMIX_PSETOP_ADD == setop->op || PMIX_PSETOP_SUB == setop->op || PMIX_PSETOP_REPLACE == setop->op) && 
            0 < pmix_list_get_size(&prte_pmix_server_globals.res_changes))
        {
            return PMIX_ERR_EXISTS;
            PMIX_LIST_FOREACH(res_change, &prte_pmix_server_globals.res_changes, prte_res_change_t){
                for(k = 0; k < res_change->num_assoc_psets; k++){
                    for(j = 0; j < setop->n_input_names; j++){
                        if(0 == strcmp(res_change->assoc_psets[k], setop->input_names[j].data.string)){
                            PMIX_RELEASE(setop);
                            return PMIX_ERR_EXISTS;
                        }
                    }
                }
                for(k = 0; k < res_change->num_rc_psets; k++){
                    for(j = 0; j < setop->n_input_names; j++){
                        if(0 == strcmp(res_change->rc_psets[k], setop->input_names[j].data.string)){
                            PMIX_RELEASE(setop);
                            return PMIX_ERR_EXISTS;
                        }
                    }
                }
            }
        }
    }
    return PMIX_SUCCESS;

}

pmix_status_t prte_ophandle_get_output(pmix_info_t *op_handle, pmix_data_array_t **output_names){
    size_t n, k, idx = 0, n_output = 0, num_ops = 0;
    pmix_status_t ret;
    pmix_value_t * val_ptr;
    prte_setop_t *setop;

    ret = prte_ophandle_get_num_ops(op_handle, &num_ops);
    if(PMIX_SUCCESS != ret){
        return ret;
    }

    for(n = 0; n < num_ops; n++){
        ret = prte_ophandle_get_nth_op(op_handle, n, &setop);
        if(PMIX_SUCCESS != ret){
            return ret;
        }
        n_output += setop->n_output_names;
        PMIX_RELEASE(setop);
    }

    PMIX_DATA_ARRAY_CREATE(*output_names, n_output, PMIX_VALUE);
    val_ptr = (pmix_value_t *) (*output_names)->array;
    for(n = 0; n < num_ops; n++){
        ret = prte_ophandle_get_nth_op(op_handle, n, &setop);
        if(PMIX_SUCCESS != ret){
            PMIX_DATA_ARRAY_FREE(*output_names);
            return ret;
        }
        for(k = 0; k < setop->n_output_names; k++){
           PMIX_VALUE_LOAD(&val_ptr[idx++], setop->output_names[k].data.string, PMIX_STRING);
        }
        PMIX_RELEASE(setop);
    }
    return PMIX_SUCCESS;
}

pmix_status_t prte_ophandle_set_output(pmix_info_t *rc_handle, size_t index, pmix_value_t *output_names, size_t noutput){
   size_t n, k, m, i, ninfo, nsetop_info, ninfo2;
    
    size_t n_op_output = 0;
    char ** output = NULL;

    pmix_value_t *val_ptr;
    pmix_info_t *setop_info, *info, *info2;
    prte_setop_t *setop;

    for(n = 0; n < rc_handle[0].value.data.darray->size; n++){
        if(0 == index){
            setop_info = (pmix_info_t *) rc_handle[0].value.data.darray->array;
            nsetop_info = rc_handle[0].value.data.darray->size;
        }else{
            info = (pmix_info_t *) rc_handle[0].value.data.darray->array;
            ninfo = rc_handle[0].value.data.darray->size; 

            for(i = 0; i < ninfo; i++){
                if(PMIX_CHECK_KEY(&info[i], "mpi.set_op_handles")){

                    /*set op handles */
                    info2 = (pmix_info_t *) info[i].value.data.darray->array;

                    if(index - 1 >= info[i].value.data.darray->size){
                        return PMIX_ERR_BAD_PARAM;
                    }

                    setop_info = (pmix_info_t *) info2[index - 1].value.data.darray->array;
                    nsetop_info = info2[index - 1].value.data.darray->size;
                }
            }
        }
    }

    for(n = 0; n < nsetop_info; n++){
        
        if(PMIX_CHECK_KEY(&setop_info[n], "mpi.op_info")){            
            info2 = (pmix_info_t *) setop_info[n].value.data.darray->array;
            ninfo2 = setop_info[n].value.data.darray->size;
            for(k = 0; k < ninfo2; k++){
                if(PMIX_CHECK_KEY(&info2[k], "mpi.op_info.output")){
                    if(0 != info2[k].value.data.darray->size){
                        PMIX_DATA_ARRAY_DESTRUCT(info2[k].value.data.darray);
                    }
                    PMIX_DATA_ARRAY_CONSTRUCT(info2[k].value.data.darray, noutput, PMIX_VALUE);
                    pmix_value_t * val_ptr = (pmix_value_t *) info2[k].value.data.darray->array;
                    for(i = 0; i < noutput; i++){
                        PMIX_VALUE_LOAD(&val_ptr[i], output_names[i].data.string, PMIX_STRING);
                    }
                    
                    return PMIX_SUCCESS;
                }
            }
        }
    }
    return PMIX_ERR_NOT_FOUND;

}

static pmix_status_t pset_intersection(pmix_server_pset_t **psets, size_t npsets, pmix_proc_t **result, size_t *nmembers){
    size_t n, k, i;
    size_t res_ptr = 0;
    size_t nprocs_max = 0;

    for(n = 0; n < npsets; n++){
        nprocs_max = MAX(nprocs_max, psets[n]->num_members); 
    }
    *result = (pmix_proc_t *) malloc(nprocs_max * sizeof(pmix_proc_t));

    for(n = 0; n < psets[0]->num_members; n++){
        for(i = 1; i < npsets; i++){
            int found=0;
            for(k = 0; k < psets[i]->num_members; k++){
                found += proc_cmp(psets[i]->members[k], psets[0]->members[n]);
                if(0 < found){
                    break;
                }
            }
            if(0 != found){
                PMIX_PROC_LOAD(&(*result)[res_ptr], psets[0]->members[n].nspace, psets[0]->members[n].rank);
                res_ptr++;
            }
        }
    }
    *nmembers=res_ptr;
    *result = realloc(*result, *nmembers * sizeof(pmix_proc_t));

    return PMIX_SUCCESS;

}

static pmix_status_t pset_difference(pmix_server_pset_t **psets, size_t npsets, pmix_proc_t **result, size_t *nmembers){
    size_t n, k, i;
    size_t res_ptr = 0;
    size_t nprocs_max = 0;

    /* Allocate enough memory for worst case */
    for(n = 0; n < npsets; n++){
        nprocs_max = MAX(nprocs_max, psets[n]->num_members); 
    }
    *result = (pmix_proc_t *) malloc(nprocs_max * sizeof(pmix_proc_t));

    /* Fill in the procs */
    for(n = 0; n < psets[0]->num_members; n++){
        for(i = 1; i < npsets; i++){
            int found=0;
            for(k = 0; k < psets[i]->num_members; k++){
                found += proc_cmp(psets[i]->members[k], psets[0]->members[n]);
                if(0 < found){
                    break;
                }
            }
            if(0 == found){
                PMIX_PROC_LOAD(&(*result)[res_ptr], psets[0]->members[n].nspace, psets[0]->members[n].rank);
                res_ptr++;
            }
        }
    }
    *nmembers = res_ptr;

    /* Realloc to actual size */
    *result = realloc(*result, *nmembers * sizeof(pmix_proc_t));

    return PMIX_SUCCESS;

}

static pmix_status_t pset_union(pmix_server_pset_t **psets, size_t npsets, pmix_proc_t **result, size_t *nmembers){
    size_t n, k, i;
    size_t res_ptr = 0;
    size_t nprocs_max = 0;

    for(n = 0; n < npsets; n++){
        nprocs_max += psets[n]->num_members; 
    }
    *result = (pmix_proc_t *) malloc(nprocs_max * sizeof(pmix_proc_t));

    /* fill in all procs from p1 */
    for(n = 0; n < psets[0]->num_members; n++){
        PMIX_PROC_LOAD(&(*result)[res_ptr], psets[0]->members[n].nspace, psets[0]->members[n].rank);
        res_ptr++;
    }
    for(i = 1; i < npsets; i++){
        /* Greedily fill in all procs from p2 which are not in p1 (b.c. procs from p1 were already added) */
        for(n = 0; n < psets[i]->num_members; n++){
            int found=0;
            for(k = 0; k < res_ptr; k++){
                found += proc_cmp((*result)[k], psets[i]->members[n]);
                if(0 < found){
                    break;
                }
            }
            if(0 == found){
                PMIX_PROC_LOAD(&(*result)[res_ptr], psets[i]->members[n].nspace, psets[i]->members[n].rank);
                res_ptr++;
            }
        }
    }
    *nmembers = res_ptr;
    *result = realloc(*result, *nmembers * sizeof(pmix_proc_t));

    return PMIX_SUCCESS;

}

static pmix_status_t pset_add(pmix_proc_t client, pmix_server_pset_t **psets, size_t npsets, pmix_info_t *info, size_t ninfo, pmix_proc_t **result, size_t *nmembers){
    size_t n;
    size_t num_procs = 0;
    pmix_status_t rc;
    pmix_rank_t highest_rank = 0;

    for(n = 0; n < ninfo; n++){
        if(PMIX_CHECK_KEY(&info[n], "mpi.op_info.info.num_procs")){
            sscanf(info[n].value.data.string, "%zu", &num_procs);
            break;
        }
    }
    if(0 == num_procs){
        return PMIX_ERR_BAD_PARAM;
    }

    rc = get_highest_job_rank(client.nspace, &highest_rank);
    if(PMIX_SUCCESS != rc){
        return rc;
    }

    *result = (pmix_proc_t *) malloc(num_procs * sizeof(pmix_proc_t));
    *nmembers = num_procs;

    pmix_rank_t offset = 1;
    for(n = 0; n < num_procs; n++, offset++){
        PMIX_PROC_LOAD(&(*result)[n], client.nspace, highest_rank + offset);
    }
    rc = set_highest_job_rank(client.nspace, highest_rank + num_procs);

    return rc;
}

static pmix_status_t pset_sub(pmix_proc_t client, pmix_server_pset_t **psets, size_t npsets, pmix_info_t *info, size_t ninfo, pmix_proc_t **result, size_t *nmembers){
    size_t n, i, index = 0, num_procs = 0;
    
    for(n = 0; n < ninfo; n++){
        if(PMIX_CHECK_KEY(&info[n], "mpi.op_info.info.num_procs")){
            sscanf(info[n].value.data.string, "%zu", &num_procs);
            break;
        }
    }
    if(0 == num_procs){
        return PMIX_ERR_BAD_PARAM;
    }

    *result = (pmix_proc_t *) malloc(num_procs * sizeof(pmix_proc_t));

    *nmembers = num_procs;
    for(n = 0; n < npsets; n++){
        for(i = psets[n]->num_members - 1; i >= 0;  i--){
            PMIX_PROC_LOAD(&(*result)[index], psets[n]->members[i].nspace, psets[n]->members[i].rank);
            index ++;
            if(index == num_procs){
                return PMIX_SUCCESS;
            }
        }
    }
 
    free(*result);
    return PMIX_ERR_BAD_PARAM;
}


pmix_status_t set_op_exec(pmix_proc_t client, prte_setop_t *setop, size_t *noutput, pmix_proc_t ***result, size_t **nmembers){
    
    pmix_status_t ret;
    size_t n, max_output_size = 0;;
    pmix_server_pset_t *pset_list_iter;
    pmix_server_pset_t **psets;
    char **input_psets;
    
    input_psets = malloc(setop->n_input_names * sizeof(char *));
    for(n = 0; n < setop->n_input_names; n++){
        input_psets[n] = strdup(setop->input_names[n].data.string);
    }

    /* Lookup the psets in our pset list */
    psets = malloc(setop->n_input_names * sizeof(pmix_server_pset_t *));
    for(n = 0; n < setop->n_input_names; n++){
        psets[n] = NULL;
        /* Lookup if the specified psets exist */
        PMIX_LIST_FOREACH(pset_list_iter, &prte_pmix_server_globals.psets, pmix_server_pset_t){
            if(0 == strcmp(pset_list_iter->name, input_psets[n])){
                psets[n] = pset_list_iter;
            }
        }
        /* Pset not found */
        if(NULL == psets[n]){
            free(psets);
            return PMIX_ERR_BAD_PARAM;
        }
    }
    ret = PMIX_SUCCESS;

    /* Execute the operation */    
    switch(setop->op){
        case PMIX_PSETOP_ADD:{
            *noutput = 1;
            *result = (pmix_proc_t **) malloc(*noutput * sizeof(pmix_proc_t *));
            *nmembers = malloc(*noutput * sizeof(size_t));
            ret = pset_add(client, psets, setop->n_input_names, setop->op_info, setop->n_op_info, &(*result)[0], *nmembers);
            break;
        }
        case PMIX_PSETOP_SUB:{
            *noutput = 1;
            *result = (pmix_proc_t **) malloc(*noutput * sizeof(pmix_proc_t *));
            *nmembers = malloc(*noutput * sizeof(size_t));
            ret = pset_sub(client, psets, setop->n_input_names, setop->op_info, setop->n_op_info, &(*result)[0], *nmembers);
            break;
        }
        case PMIX_PSETOP_UNION: {
            *noutput = 1;
            *result = (pmix_proc_t **) malloc(*noutput * sizeof(pmix_proc_t *));
            *nmembers = malloc(*noutput * sizeof(size_t));
            ret = pset_union(psets, setop->n_input_names, &(*result)[0], *nmembers);
            break;
        }
        case PMIX_PSETOP_DIFFERENCE:{
            *noutput = 1;
            *result = (pmix_proc_t **) malloc(*noutput * sizeof(pmix_proc_t *));
            *nmembers = malloc(*noutput * sizeof(size_t));
            ret = pset_difference(psets, setop->n_input_names, &(*result)[0], *nmembers);
            break;
        }
        case PMIX_PSETOP_INTERSECTION:{
            *noutput = 1;
            *result = (pmix_proc_t **) malloc(*noutput * sizeof(pmix_proc_t *));
            *nmembers = malloc(*noutput * sizeof(size_t));
            ret = pset_intersection(psets, setop->n_input_names, &(*result)[0], *nmembers);
            break;
        }
        default: 
            ret = PMIX_ERR_BAD_PARAM;
    }


    if(PMIX_SUCCESS != ret){
        if(NULL != *result){
            free(*result);
        }
    }

    free(psets);

    return ret;
}

pmix_status_t pset_op_exec(pmix_psetop_directive_t directive, char **input_psets, size_t npsets, pmix_info_t *params, size_t nparams, size_t *noutput, pmix_proc_t ***result, size_t **nmembers){
    
    pmix_status_t ret;
    size_t n, max_output_size = 0;;
    pmix_server_pset_t *pset_list_iter;
    pmix_server_pset_t **psets;


    /* Lookup the psets in our pset list */
    psets = malloc(npsets * sizeof(pmix_server_pset_t *));
    for(n = 0; n < npsets; n++){
        psets[n] = NULL;
        /* Lookup if the specified psets exist */
        PMIX_LIST_FOREACH(pset_list_iter, &prte_pmix_server_globals.psets, pmix_server_pset_t){
            if(0 == strcmp(pset_list_iter->name, input_psets[n])){
                psets[n] = pset_list_iter;
            }
        }
        /* Pset not found */
        if(NULL == psets[n]){
            free(psets);
            return PMIX_ERR_BAD_PARAM;
        }
    }
    ret = PMIX_SUCCESS;

    /* Execute the operation */    
    switch(directive){
        case PMIX_PSETOP_UNION: {
            *noutput = 1;
            *result = (pmix_proc_t **) malloc(*noutput * sizeof(pmix_proc_t *));
            *nmembers = malloc(*noutput * sizeof(size_t));
            ret = pset_union(psets, npsets, &(*result)[0], *nmembers);
            break;
        }
        case PMIX_PSETOP_DIFFERENCE:{
            *noutput = 1;
            *result = (pmix_proc_t **) malloc(*noutput * sizeof(pmix_proc_t *));
            *nmembers = malloc(*noutput * sizeof(size_t));
            ret = pset_difference(psets, npsets, &(*result)[0], *nmembers);
            break;
        }
        case PMIX_PSETOP_INTERSECTION:{
            *noutput = 1;
            *result = (pmix_proc_t **) malloc(*noutput * sizeof(pmix_proc_t *));
            *nmembers = malloc(*noutput * sizeof(size_t));
            ret = pset_intersection(psets, npsets, &(*result)[0], *nmembers);
            break;
        }
        default: 
            ret = PMIX_ERR_BAD_PARAM;
    }

    if(PMIX_SUCCESS != ret){
        if(NULL != *result){
            free(*result);
        }
    }

    free(psets);

    return ret;
}
pmix_psetop_directive_t prte_pset_get_op(pmix_server_pset_t *pset){
    if(PRTE_FLAG_TEST(pset, PRTE_PSET_FLAG_ADD)){
        return PMIX_PSETOP_ADD;
    }
    if(PRTE_FLAG_TEST(pset, PRTE_PSET_FLAG_SUB)){
        return PMIX_PSETOP_SUB;
    }
    if(PRTE_FLAG_TEST(pset, PRTE_PSET_FLAG_UNION)){
        return PMIX_PSETOP_UNION;
    }
    if(PRTE_FLAG_TEST(pset, PRTE_PSET_FLAG_DIFFERENCE)){
        return PMIX_PSETOP_DIFFERENCE;
    }
    if(PRTE_FLAG_TEST(pset, PRTE_PSET_FLAG_INTERSECTION)){
        return PMIX_PSETOP_INTERSECTION;
    }

    return PMIX_PSETOP_NULL;
}

void prte_pset_set_flags(pmix_server_pset_t *pset, pmix_psetop_directive_t op){
    switch(op){
        case PMIX_PSETOP_ADD:
            PRTE_FLAG_SET(pset, PRTE_PSET_FLAG_ADD);
            return;
        case PMIX_PSETOP_SUB:
            PRTE_FLAG_SET(pset, PRTE_PSET_FLAG_SUB);
            return;
        case PMIX_PSETOP_UNION:
            PRTE_FLAG_SET(pset, PRTE_PSET_FLAG_UNION);
            return;
        case PMIX_PSETOP_DIFFERENCE:
            PRTE_FLAG_SET(pset, PRTE_PSET_FLAG_DIFFERENCE);
            return;
        case PMIX_PSETOP_INTERSECTION:
            PRTE_FLAG_SET(pset, PRTE_PSET_FLAG_INTERSECTION);
            return;
    }
}

void setop_pub_cbfunc(pmix_status_t status, void *cbdata){
    return;
}

int ophandle_execute(pmix_proc_t client, pmix_info_t *rc_handle, size_t op_index_start, size_t *op_index_end){
    size_t n, k, m, i, num_ops;
    int str_len, ret;
    uint8_t def_cmd = PRTE_DYNRES_DEFINE_PSET;
    char * suffix;

    pmix_value_t * val_ptr;
    pmix_data_buffer_t *buf;
    pmix_rank_t r;
    pmix_psetop_directive_t op = PMIX_PSETOP_NULL;
    pmix_server_pset_t *pset;

    size_t ninfo = 0;
    pmix_info_t *info = NULL;

    size_t ninput = 0;
    char **input_psets = NULL;

    size_t n_op_output = 0;
    char ** output = NULL;

    size_t noutput = 0;
    pmix_value_t *output_psets = NULL;

    size_t ninfo_arrays;
    pmix_data_array_t ** info_arrays;
    
    size_t *pset_sizes;
    pmix_proc_t **member_arrays = NULL;

    prte_setop_t *setop;
    prte_pset_flags_t flags = PRTE_PSET_FLAG_NONE;

    bool next = true;
    ret = prte_ophandle_get_num_ops(rc_handle, &num_ops);
    if(PMIX_SUCCESS != ret){
        PRTE_ERROR_LOG(ret);
        return ret;
    }

    *op_index_end = op_index_start;
    for(; *op_index_end < num_ops; (*op_index_end)++){
        ret = prte_ophandle_get_nth_op(rc_handle, *op_index_end, &setop);
        if(PMIX_SUCCESS != ret){
            PRTE_ERROR_LOG(ret);
            return ret;
        }

        /* noop */
        if(PMIX_PSETOP_NULL == setop->op){
            PMIX_RELEASE(setop);
            continue;
        }
        /* Execute the specified operation */
        ret = set_op_exec(client, setop, &n_op_output, &member_arrays, &pset_sizes);
        if(PMIX_SUCCESS != ret){
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        

        /* Assign output names if not given */
        char * prefix;
        if(0 == setop->n_output_names){
            PMIX_VALUE_CREATE(setop->output_names, n_op_output);
            setop->n_output_names = n_op_output;
            for(i = 0; i < n_op_output; i++){
                str_len = snprintf( NULL, 0, "%d", dummy_name_ctr);
                suffix = (char *) malloc( str_len + 1 );   
                snprintf( suffix, str_len + 1, "%d", dummy_name_ctr);
                dummy_name_ctr++;
                setop->output_names[i].data.string = malloc(str_len + strlen(prte_pset_base_name)+ 1);
                strcpy(setop->output_names[i].data.string, prte_pset_base_name);
                strcat(setop->output_names[i].data.string, suffix);
                free(suffix);
            }
            
            if(PMIX_SUCCESS != (ret = prte_ophandle_set_output(rc_handle, *op_index_end, setop->output_names, setop->n_output_names))){
                PRTE_ERROR_LOG(ret);
                PMIX_RELEASE(setop);
                return ret;
            }
            PMIX_VALUE_FREE(setop->output_names, setop->n_output_names);
            PMIX_RELEASE(setop);
            if(PMIX_SUCCESS != (ret = prte_ophandle_get_nth_op(rc_handle, *op_index_end, &setop))){
                PRTE_ERROR_LOG(ret);
                return ret;
            }
        }

        
        /* add the pset to our server globals */
        for(i = 0; i < n_op_output; i++){

            pset = PMIX_NEW(pmix_server_pset_t);
            pset->name = strdup(setop->output_names[i].data.string);
            pset->num_members = pset_sizes[i];

            prte_pset_set_flags(pset, setop->op);
            PMIX_PROC_CREATE(pset->members, pset->num_members);
            memcpy(pset->members, member_arrays[i], pset_sizes[i] * sizeof(pmix_proc_t));

            pmix_list_append(&prte_pmix_server_globals.psets, &pset->super);
            /* also pass it down to the pmix_server */
            PMIx_server_define_process_set(member_arrays[i], pset_sizes[i], setop->output_names[i].data.string);
        
            /* send a PSET_DEFINE msg to all deamons, excluding ourself */
            prte_job_t *daemon_job = prte_get_job_data_object(PRTE_PROC_MY_PROCID->nspace);
            pmix_proc_t target;
            PMIX_LOAD_PROCID(&target, PRTE_PROC_MY_HNP->nspace, 0);

            for(r = 0; r < daemon_job->num_procs; r++){
                target.rank = r;

                if(target.rank == PRTE_PROC_MY_PROCID->rank){
                    continue;
                }

                /* pack the cmd */
                PMIX_DATA_BUFFER_CREATE(buf);
                if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &def_cmd, 1, PMIX_UINT8))){
                    PMIX_DATA_BUFFER_RELEASE(buf);
                    PRTE_ERROR_LOG(ret);
                    return ret;
                }
                /* pack the pset size */
                if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &pset_sizes[i], 1, PMIX_SIZE))){
                    PMIX_DATA_BUFFER_RELEASE(buf);
                    PRTE_ERROR_LOG(ret);
                    return ret;
                }
                /* pack the pset name */
                if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &setop->output_names[i].data.string, 1, PMIX_STRING))){
                    PMIX_DATA_BUFFER_RELEASE(buf);
                    PRTE_ERROR_LOG(ret);
                    return ret;
                }
                /* pack the PSet flags */
                if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &pset->flags, 1, PMIX_UINT16))){
                    PRTE_ERROR_LOG(ret);
                    return ret;
                }
                /* pack the procs */
                size_t proc;
                for(proc = 0; proc < pset_sizes[i]; proc++){
                    if(PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, buf, &member_arrays[i][proc], 1, PMIX_PROC))){
                        PMIX_DATA_BUFFER_RELEASE(buf);
                        PRTE_ERROR_LOG(ret);
                        return ret;
                    }
                }
                PRTE_RML_SEND(ret, target.rank, buf, PRTE_RML_TAG_MALLEABILITY);
            }
        }

        /* publish the PSet data */
        if(0 < setop->npset_info_arrays){
            for(n = 0; n < setop->npset_info_arrays; n++){
                info = (pmix_info_t *) setop->pset_info_arrays[n]->array;
                ninfo = setop->pset_info_arrays[n]->size;

                if(0 < ninfo){
                    pmix_server_publish_fn(PRTE_PROC_MY_NAME, info, ninfo, setop_pub_cbfunc, NULL);
                }
            }
        }
        
        PMIX_RELEASE(setop);
    }
    
    return PMIX_SUCCESS;
} 

/* 5. Inform daemons about res change so they can answer related queries */
pmix_status_t prte_ophandle_inform_daemons(pmix_info_t *info_rc_op_handle, pmix_psetop_directive_t op){
    prte_setop_t *setop = NULL;

    pmix_data_buffer_t *buf2;
    pmix_proc_t daemon_procid;
    pmix_status_t ret;

    size_t ninput, noutput, num_ops, n, k;
    char** input_psets, **output_psets;

    prte_daemon_cmd_flag_t cmd = PRTE_DYNRES_DEFINE_RES_CHANGE;
    
    ret = prte_ophandle_get_num_ops(info_rc_op_handle, &num_ops);
    if(PMIX_SUCCESS != ret){
        return ret;
    }

    for(n = 0; n < num_ops; n++){      
        ret = prte_ophandle_get_nth_op(info_rc_op_handle, n, &setop);
        if(ret != PMIX_SUCCESS){
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        if(setop->op != op){
            PMIX_RELEASE(setop);
            continue;
        }

        ninput = setop->n_input_names;
        noutput = setop->n_output_names;
        input_psets = malloc(ninput * sizeof(char *));
        output_psets = malloc(noutput * sizeof(char *));
        for(k = 0; k < ninput; k++){
            input_psets[k] = strdup(setop->input_names[k].data.string);
        }
        for(k = 0; k < noutput; k++){
            output_psets[k] = strdup(setop->output_names[k].data.string);
        }


        for(k = 0; k < prte_process_info.num_daemons; k++){
            PMIX_DATA_BUFFER_CREATE(buf2);
            daemon_procid.rank = k;

            ret = PMIx_Data_pack(NULL, buf2, &cmd, 1, PMIX_UINT8);

            ret = PMIx_Data_pack(NULL, buf2, &setop->op, 1, PMIX_UINT8);

            ret = PMIx_Data_pack(NULL, buf2, &noutput, 1, PMIX_INT32);

            ret = PMIx_Data_pack(NULL, buf2, output_psets, noutput, PMIX_STRING);

            ret = PMIx_Data_pack(NULL, buf2, &ninput, 1, PMIX_INT32);

            ret = PMIx_Data_pack(NULL, buf2, input_psets, ninput, PMIX_STRING);

            /* No need to go through the send/recv for ourself.
             * At this point we are inside of an event so also no need to go through the event lib.
             * We can directly call the handler
             */
            if(k == PRTE_PROC_MY_NAME->rank){
                pmix_server_dynres(ret, PRTE_PROC_MY_NAME, buf2, PRTE_RML_TAG_MALLEABILITY, NULL);
                PMIX_DATA_BUFFER_RELEASE(buf2);
                continue;
            }

            //prte_rml.send_buffer_nb(&daemon_procid, buf2, PRTE_RML_TAG_MALLEABILITY, prte_rml_send_callback, NULL);
            PRTE_RML_SEND(ret, daemon_procid.rank, buf2, PRTE_RML_TAG_MALLEABILITY);
        }

        /* free pset names*/
        for(k = 0; k < ninput; k++){
            free(input_psets[k]);
        }
        free(input_psets);
        for(k = 0; k < noutput; k++){
            free(output_psets[k]);
        }
        free(output_psets);
    }

    return ret;
}

