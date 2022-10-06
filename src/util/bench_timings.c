#include "bench_timings.h"


int timings_my_rank = -1;
int timings_cur_res_change_id = 0;
int timings_cur_rc_status = 0;




/*
 * LIST FUNCTIONS
 */


void push(node_t * head, void *val) {
    node_t * current = head;
    while (current->next != 0) {
        current = current->next;
    }

    /* now we can add a new variable */
    current->next = (node_t *) malloc(sizeof(node_t));
    current->next->val = val;
    current->next->next = 0;
}

int pop(node_t ** head) {
    int retval = -1;
    node_t * next_node = 0;

    if (*head == 0) {
        return -1;
    }

    next_node = (*head)->next;
    free((*head)->val);
    free(*head);
    *head = next_node;
    return retval;
}

/*
 ************* INIT & TIMESTAMP ****************      
 */

void init_add_timing(node_t * list, void ** timing, size_t frame_size){
    *timing = calloc(1, frame_size);
    push(list, (void*) *timing);
}

void make_timestamp_base(long * ptr_to_dest){
    struct timeval timestamp;
    gettimeofday(&timestamp, 0);

    *ptr_to_dest = timestamp.tv_sec * 1000000 + timestamp.tv_usec;
}

void make_timestamp_root(long * ptr_to_dest){
    if(0 != timings_my_rank)return;

    struct timeval timestamp;
    gettimeofday(&timestamp, 0);

    *ptr_to_dest = timestamp.tv_sec * 1000000 + timestamp.tv_usec;
}

void make_timestamp_rootonly(long * ptr_to_dest, int rank){

    struct timeval timestamp;
    gettimeofday(&timestamp, 0);

    *ptr_to_dest = timestamp.tv_sec * 1000000 + timestamp.tv_usec;
}

void set_res_change_id(int *dest, const char *delta_pset){

    *dest = atoi(&delta_pset[strlen(delta_pset)-1]) + 1;
    timings_cur_res_change_id = *dest;
}

/*
 * *********** WRITE OUPUT ********************
 */


/* Base frame */

void print_list_to_file_base(node_t * head,  const char *filename) {

    
    struct timeval start, end;
    gettimeofday(&start, 0);
    long l_start = start.tv_sec * 1000000 + start.tv_usec;
 
    FILE *f = fopen(filename, "w+");
    if(NULL == f){
        printf("Error opening file: %s\n", strerror(errno));
        return;
    }

    char header[] = "res_change_id, app_start, init_start, init_end, confirm_start, confirm_end, sim_start, sim_end, finalize_start, finalize_end, start_writing, end_writing";
    fprintf(f, "%s\n", header);

    node_t * current = head->next;
    while (current != 0) {
        timing_frame_base_t *timing = (timing_frame_base_t *) current->val;

        fprintf(f, "%d,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld",   timing->res_change_id, timing->app_start, timing->init_start, timing->init_end, timing->confirm_start, 
                                                timing->confirm_end, timing->sim_start, timing->sim_end, timing->finalize_start, timing->finalize_end);
        pop(&current);
    }

    
    gettimeofday(&end, 0);
    long l_end = end.tv_sec * 1000000 + end.tv_usec;

    fprintf(f, "%ld,%ld\n", l_start, l_end);

    fclose(f);


}



/* P4est frame */

void print_list_to_file_p4est(node_t * head, char *filename) {


    FILE *f = fopen(filename, "w+");
    


    char header[] = "iteration, res_change_id, num_procs, delta_procs, rc_status, rc_start, get_rc_start_1, request_start, get_rc_start_2, psetop_start, handle_rc_start, handle_rc_end, accept_start, accept_end, rc_end, partition_start, interval_start, interval_end, iteration_end";
    fprintf(f, "%s\n", header);
    
    node_t * current = head->next;
    while (current != 0) {
        timing_frame_p4est_t *timing = (timing_frame_p4est_t *) current->val;
        fprintf(f, "%d,%d,%d,%d,%d,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld\n",   timing->iteration, timing->res_change_id, timing->num_procs, timing->delta_procs, timing->rc_status, 
                                                timing->rc_start, timing->get_rc_start_1, timing->request_start, timing->get_rc_start_2, 
                                                timing->psetop_start, timing->handle_rc_start, timing->handle_rc_end, timing->accept_start, 
                                                timing->accept_end, timing->rc_end, timing->partition_start, timing->interval_start, 
                                                timing->interval_end, timing->iteration_end);
        pop(&current);
    }

    fclose(f);

}

/* Synth frame */


void print_list_to_file_synth(node_t * head, char *filename) {


    FILE *f = fopen(filename, "w+");
    char header[] = "iteration, res_change_id, num_procs, delta_procs, rc_status, rc_start, get_rc_start, psetop_start, accept_start, accept_end, reinit_start, rc_end, rebalance_start, work_start, work_end, iteration_end";
    fprintf(f, "%s\n", header);
    
    node_t * current = head->next;
    while (current != 0) {
        timing_frame_synth_t *timing = (timing_frame_synth_t *) current->val;
        fprintf(f, "%d,%d,%d,%d,%d,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld\n",   timing->iteration, timing->res_change_id, timing->num_procs, timing->delta_procs, timing->rc_status, 
                                                timing->rc_start, timing->get_rc_start, timing->psetop_start, timing->accept_start, timing->reinit_start, 
                                                timing->rc_end, timing->rebalance_start, timing->work_start, timing->work_end, timing->iteration_end);
        pop(&current);
    }

    fclose(f);
}


/* PRRTE master frame */

void print_list_to_file_master(node_t * head,  const char *filename) {

 
    FILE *f = fopen(filename, "w+");
    if(NULL == f){
        printf("Error opening file: %s\n", strerror(errno));
        return;
    }

    char header[] = "res_change_id, res_change_type, res_change_size, rc_start, jdata_start, jdata_end, pset_start, rc_publish_start, apply_start, rc_end1, rc_deregistered, rc_finalized, rc_end2;";
    fprintf(f, "%s\n", header);

    node_t * current = head->next;
    while (current != 0) {
        timing_frame_master_t *timing = (timing_frame_base_t *) current->val;

        fprintf(f, "%d,%d,%d,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld\n", timing->res_change_id, timing->res_change_type, timing->res_change_size, timing->rc_start, timing->jdata_start, timing->jdata_end, timing->pset_start, timing->rc_publish_start, timing->apply_start, timing->rc_end1, timing->rc_deregistered, timing->rc_finalized, timing->rc_end2);
        pop(&current);
    }

    fclose(f);
}

/* PRRTE daemon frame */

void print_list_to_file_daemon(node_t * head,  const char *filename) {

 
    FILE *f = fopen(filename, "w+");
    if(NULL == f){
        printf("Error opening file: %s\n", strerror(errno));
        return;
    }

    char header[] = "res_change_id, res_change_type, res_change_size,rc_publish_start,rc_publish_end,rc_apply_start,rc_apply_end,rc_unpublish,rc_finalize,rc_end";    
    fprintf(f, "%s\n", header);

    node_t * current = head->next;
    while (current != 0) {
        timing_frame_daemon_t *timing = (timing_frame_base_t *) current->val;

        fprintf(f, "%d,%d,%d,%ld,%ld,%ld,%ld,%ld,%ld,%ld\n",   timing->res_change_id, timing->res_change_type, timing->res_change_size, timing->rc_publish_start,timing->rc_publish_end,timing->rc_apply_start,timing->rc_apply_end,timing->rc_unpublish,timing->rc_finalize,timing->rc_end);
        pop(&current);
    }

    fclose(f);

}

node_t *master_timing_list = NULL;
node_t *daemon_timing_list = NULL;
timing_frame_master_t *cur_master_timing_frame = NULL;
timing_frame_daemon_t *cur_daemon_timing_frame = NULL;