#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>


extern int timings_my_rank;
extern int timings_cur_res_change_id;
extern int timings_cur_rc_status;




/*
 * LIST FUNCTIONS
 */
typedef struct node {
    void *val;
    struct node * next;
} node_t;

void push(node_t * head, void *val);

int pop(node_t ** head);

/*
 ************* INIT & TIMESTAMP ****************      
 */

void init_add_timing(node_t * list, void ** timing, size_t frame_size);

void make_timestamp_base(long * ptr_to_dest);

void make_timestamp_root(long * ptr_to_dest);

void make_timestamp_rootonly(long * ptr_to_dest, int rank);

void set_res_change_id(int *dest, const char *delta_pset);

/*
 * *********** WRITE OUPUT ********************
 */


/* Base frame */
typedef struct{
    long app_start;
    long init_start;
    long init_end;
    long confirm_start;
    long confirm_end;
    long sim_start;
    long sim_end;
    long finalize_start;
    long finalize_end;
    int res_change_id;
}timing_frame_base_t;

void print_list_to_file_base(node_t * head,  const char *filename);



/* P4est frame */
typedef struct{
    long iteration_start;
    long rc_start;
    long get_rc_start_1;
    long request_start;
    long get_rc_start_2;
    long psetop_start;
    long handle_rc_start;
    long handle_rc_end;
    long accept_start;
    long accept_end;
    long rc_end;
    long partition_start;
    long interval_start;
    long interval_end;
    long iteration_end;
    int iteration;
    int res_change_id;
    int num_procs;
    int delta_procs;
    int rc_status;
}timing_frame_p4est_t;


void print_list_to_file_p4est(node_t * head, char *filename);

/* Synth frame */
typedef struct{
    long iteration_start;
    long rc_start;
    long get_rc_start;
    long psetop_start;
    long accept_start;
    long reinit_start;
    long rc_end;
    long rebalance_start;
    long work_start;
    long work_end;
    long iteration_end;
    int iteration;
    int res_change_id;
    int num_procs;
    int delta_procs;
    int rc_status;
}timing_frame_synth_t;


void print_list_to_file_synth(node_t * head, char *filename);


/* PRRTE master frame */
typedef struct{
    long rc_start;
    long jdata_start;
    long jdata_end;
    long pset_start;
    long rc_publish_start;
    long apply_start;
    long rc_end1;
    long rc_deregistered;
    long rc_finalized;
    long rc_end2;
    int res_change_id;
    int res_change_type;
    int res_change_size;
}timing_frame_master_t;

void print_list_to_file_master(node_t * head,  const char *filename);

/* PRRTE daemon frame */
typedef struct{
    long rc_publish_start;
    long rc_publish_end;
    long rc_apply_start;
    long rc_apply_end;
    long rc_unpublish;
    long rc_finalize;
    long rc_end;
    int res_change_id;
    int res_change_type;
    int res_change_size;
}timing_frame_daemon_t;

void print_list_to_file_daemon(node_t * head,  const char *filename);



extern node_t *master_timing_list;
extern node_t *daemon_timing_list;
extern timing_frame_master_t *cur_master_timing_frame;
extern timing_frame_daemon_t *cur_daemon_timing_frame;