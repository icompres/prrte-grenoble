/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * Data server for PRTE
 */
#ifndef PRTE_SETOP_SERVER_H
#define PRTE_SETOP_SERVER_H

#include "prte_config.h"
#include "types.h"

#include "src/rml/rml_types.h"
#include "src/pmix/pmix-internal.h"
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/prted/pmix/pmix_server.h"

typedef struct prte_setop{
    pmix_list_item_t super;
    pmix_psetop_directive_t op;
    pmix_value_t * input_names;
    size_t n_input_names;
    pmix_value_t * output_names;
    size_t n_output_names;

    pmix_info_t *op_info;
    size_t n_op_info;

    pmix_data_array_t ** pset_info_arrays; 
    size_t npset_info_arrays;
}prte_setop_t;
PMIX_CLASS_DECLARATION(prte_setop_t);

void setop_server_init();
void setop_server_close();

pmix_status_t set_highest_job_rank(pmix_nspace_t nspace, pmix_rank_t highest_rank);

int prte_pset_define_from_parray(char *pset_name, pmix_pointer_array_t *parray, prte_pset_flags_t flags);

pmix_status_t prte_op_handle_verify(pmix_info_t *op_handle);
int prte_ophandle_get_nth_op(pmix_info_t *rc_handle, size_t index, prte_setop_t **setop);
int prte_ophandle_get_num_ops(pmix_info_t *rc_handle, size_t *num_ops);
pmix_status_t prte_ophandle_get_output(pmix_info_t *op_handle, pmix_data_array_t **output_names);
int ophandle_execute(pmix_proc_t client, pmix_info_t *rc_handle, size_t op_index_start, size_t *op_index_end);
pmix_status_t set_op_exec(pmix_proc_t client, prte_setop_t *setop, size_t *noutput, pmix_proc_t ***result, size_t **nmembers);
pmix_status_t prte_ophandle_inform_daemons(pmix_info_t *info_rc_op_handle, pmix_psetop_directive_t op);

/* DEPRECATED */
pmix_status_t pset_op_exec(pmix_psetop_directive_t directive, char **input_psets, size_t npsets, pmix_info_t *params, size_t nparams, 
                                    size_t *noutput, pmix_proc_t ***result, size_t **nmembers);

#endif /* PRTE_SETOP_SERVER_H */
