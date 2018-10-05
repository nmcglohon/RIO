//Elsa Gonsiorowski
//Rensselaer Polytechnic Institute
//Decemeber 13, 2013

#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "mpi.h"
#include "ross.h"
#include "io-config.h"
#include <vector>

using namespace std;

//#define RIO_DEBUG

// Command Line Options
int g_io_number_of_files = 1;
unsigned int g_io_store = 2;
unsigned int g_io_evt_ts_mode = 0;
unsigned int g_io_checkpoint_gvt_interval = 1000;
unsigned int g_io_overwrite_policy = 0;
unsigned int g_io_keep_last_n_checkpoints = 1;
char g_io_checkpoint_base_name[8192];
char g_io_load_checkpoint_name[8192];

const tw_optdef io_opts[] = {
    TWOPT_GROUP("RIO"),
    TWOPT_UINT("io-files", g_io_number_of_files, "io files"),
    TWOPT_UINT("io-store", g_io_store, "io store mode (0 Load | 1 Save | 2 Disable"),
    TWOPT_UINT("io-evt-ts-mode", g_io_evt_ts_mode, "post-simulation event timestep mode (0 Reset (default) | 1 Unaltered"),
    TWOPT_UINT("rio-checkpoint-gvt-interval", g_io_checkpoint_gvt_interval, "frequency of RIO checkpointing in multiples of GVT (Default 1)"),
    TWOPT_UINT("rio-overwrite-policy", g_io_overwrite_policy, "define RIO checkpoint overwrite behavior (0 Never (default) | 1 Always | 2 Keep Last N"),
    TWOPT_UINT("rio-keep-last-n-checkpoints", g_io_keep_last_n_checkpoints, "keep last N checkpoints when overwrite policy is 2 (Keep Last N), (Default 1000)"),
    TWOPT_CHAR("rio-checkpoint-base-name", g_io_checkpoint_base_name, "base name for RIO checkpointing for this simulation (Default: 'rio-checkpoint')"),
    TWOPT_CHAR("rio-load-checkpoint", g_io_load_checkpoint_name, "path to checkpoint you wish to load - if undefined, simulation will start from scratch"),
    TWOPT_END()
};

// User-Set Variable Initializations
io_partition * g_io_partitions;
io_lptype * g_io_lp_types = NULL;
io_lptype * io_lp_type_registry = NULL;
io_load_type g_io_load_at = NONE;
char g_io_checkpoint_name[1024];
int g_io_events_buffered_per_rank = 0;
tw_eventq g_io_buffered_events;
// tw_eventq g_io_free_events;

std::vector<tw_event*> g_io_in_transit_gvt_events;

// Local Variables
static unsigned long l_io_kp_offset = 0;    // MPI_Exscan
static unsigned long l_io_lp_offset = 0;    // MPI_Exscan
static unsigned long l0_io_total_kp = 0;    // MPI_Reuced on 0
static unsigned long l0_io_total_lp = 0;    // MPI_Reuced on 0
static unsigned long l_io_min_parts = 0;    // MPI_Allreduce
static int l_io_init_flag = 0;
static int l_io_append_flag = 0;

char model_version[41];

void io_register_model_version (char *sha1) {
    strcpy(model_version, sha1);
}

tw_event * io_get_free_event()
{
    return tw_eventq_pop(&g_tw_pe[0]->free_q);
}

void io_track_event(tw_event* e)
{
    g_io_in_transit_gvt_events.push_back(e);
}

void io_remove_tracked_event(tw_event* e)
{
    tw_eventid search_id = e->event_id;

    std::vector<tw_event*>::iterator it = g_io_in_transit_gvt_events.begin();
    for (; it != g_io_in_transit_gvt_events.end(); )
    {
        if ( (*it)->event_id == search_id ) {
            it = g_io_in_transit_gvt_events.erase(it);
        }
        else
            ++it;
    }
}

void io_remove_stale_events_from_register(tw_pe *pe)
{
    tw_stime last_gvt_ts = pe->GVT;
    int pruned_count;

    std::vector<tw_event*>::iterator it = g_io_in_transit_gvt_events.begin();
    for (; it != g_io_in_transit_gvt_events.end(); )
    {
        tw_stime event_recv_ts = (*it)->recv_ts;
        if (event_recv_ts < last_gvt_ts) {
            it = g_io_in_transit_gvt_events.erase(it);
            pruned_count++;        }
        else
            ++it; //manually advance iterator
    }    
    // printf("Pruned: %d\n",pruned_count);
}

//ONLY CALL THIS RIGHT BEFORE CHECKPOINTING
void io_prune_transit_queue(tw_pe *pe)
{
    tw_stime last_gvt_ts = pe->GVT;
    int pruned_count = 0;

    std::vector<tw_event*>::iterator it = g_io_in_transit_gvt_events.begin();
    for (; it != g_io_in_transit_gvt_events.end(); )
    {
        tw_stime event_recv_ts = (*it)->recv_ts;
        tw_event_owner owner = (tw_event_owner) (*it)->state.owner;
        if ((event_recv_ts < last_gvt_ts) || (owner == TW_pe_free_q)) {
            it = g_io_in_transit_gvt_events.erase(it);
            pruned_count++;        }
        else
            ++it; //manually advance iterator
    }    
    // printf("Pruned: %d\n",pruned_count);
}

// tw_event * io_event_grab(tw_pe *pe) {
//     if (!l_io_init_flag || g_io_events_buffered_per_rank == 0) {
//       // the RIO system has not been initialized
//       // or we are not buffering events
//       return pe->abort_event;
//     }

//     tw_clock start = tw_clock_read();
//     tw_event  *e = tw_eventq_pop(&g_io_free_events);

//     if (e) {
//         e->cancel_next = NULL;
//         e->caused_by_me = NULL;
//         e->cause_next = NULL;
//         e->prev = e->next = NULL;

//         memset(&e->state, 0, sizeof(e->state));
//         memset(&e->event_id, 0, sizeof(e->event_id));
//         tw_eventq_push(&g_io_buffered_events, e);
//     } else {
//         printf("WARNING: did not allocate enough events to RIO buffer\n");
//         e = pe->abort_event;
//     }
//     pe->stats.s_rio_load += (tw_clock_read() - start);
//     e->state.owner = IO_buffer;
//     return e;
// }

void io_event_cancel(tw_event *e) {
    io_remove_tracked_event(e);
}

// void io_event_cancel(tw_event *e) {
//     tw_eventq_delete_any(&g_io_buffered_events, e);
//     tw_eventq_push(&g_io_free_events, e);
// }

void io_init() {
    int i;

    assert(l_io_init_flag == 0 && "ERROR: RIO system already initialized");
    l_io_init_flag = 1;

    MPI_Exscan(&g_tw_nkp, &l_io_kp_offset, 1, MPI_UNSIGNED_LONG, MPI_SUM, MPI_COMM_WORLD);
    MPI_Exscan(&g_tw_nlp, &l_io_lp_offset, 1, MPI_UNSIGNED_LONG, MPI_SUM, MPI_COMM_WORLD);
    MPI_Reduce(&g_tw_nkp, &l0_io_total_kp, 1, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&g_tw_nlp, &l0_io_total_lp, 1, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);

    // Use collectives where ever possible
    MPI_Allreduce(&g_tw_nkp, &l_io_min_parts, 1, MPI_UNSIGNED_LONG, MPI_MIN, MPI_COMM_WORLD);

    if (g_tw_mynode == 0) {
        printf("*** IO SYSTEM INIT ***\n\tFiles: %d\n\tParts: %lu\n\n", g_io_number_of_files, l0_io_total_kp);
    }

    if (strcmp(g_io_checkpoint_base_name, "") == 0) {
        strcpy(g_io_checkpoint_base_name,"rio-checkpoint");
    }

    unsigned int g_io_checkpoints_saved = 0;
}

// void io_init() {
//     int i;

//     assert(l_io_init_flag == 0 && "ERROR: RIO system already initialized");
//     l_io_init_flag = 1;

//     MPI_Exscan(&g_tw_nkp, &l_io_kp_offset, 1, MPI_UNSIGNED_LONG, MPI_SUM, MPI_COMM_WORLD);
//     MPI_Exscan(&g_tw_nlp, &l_io_lp_offset, 1, MPI_UNSIGNED_LONG, MPI_SUM, MPI_COMM_WORLD);
//     MPI_Reduce(&g_tw_nkp, &l0_io_total_kp, 1, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
//     MPI_Reduce(&g_tw_nlp, &l0_io_total_lp, 1, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);

//     // Use collectives where ever possible
//     MPI_Allreduce(&g_tw_nkp, &l_io_min_parts, 1, MPI_UNSIGNED_LONG, MPI_MIN, MPI_COMM_WORLD);

//     if (g_tw_mynode == 0) {
//         printf("*** IO SYSTEM INIT ***\n\tFiles: %d\n\tParts: %lu\n\n", g_io_number_of_files, l0_io_total_kp);
//     }

//     g_io_free_events.size = 0;
//     g_io_free_events.head = g_io_free_events.tail = NULL;
//     g_io_buffered_events.size = 0;
//     g_io_buffered_events.head = g_io_buffered_events.tail = NULL;

//     if (strcmp(g_io_checkpoint_base_name, "") == 0) {
//         strcpy(g_io_checkpoint_base_name,"rio-checkpoint");
//     }

//     unsigned int g_io_checkpoints_saved = 0;
// }

// This run is part of a larger set of DISPARATE runs
// append the .md and .lp files
void io_appending_job() {
    if (l_io_init_flag == 1) {
        l_io_append_flag = 1;
    }
}

void io_load_checkpoint(char * master_filename, io_load_type load_at) {
    strcpy(g_io_checkpoint_name, master_filename);
    g_io_load_at = load_at;
}

void io_read_checkpoint() {
    int i, cur_part, rc;
    int mpi_rank = g_tw_mynode;
    int number_of_mpitasks = tw_nnodes();

    // assert that IO system has been init
    assert(g_io_number_of_files != 0 && "ERROR: IO variables not set: # of files\n");

    // TODO: check to make sure io system is init'd?

    MPI_File fh;
    MPI_Status status;
    char filename[257];

    // Read MH

    // Metadata datatype
    MPI_Datatype MPI_IO_PART;
    MPI_Type_contiguous(io_partition_field_count, MPI_INT, &MPI_IO_PART);
    MPI_Type_commit(&MPI_IO_PART);
    int partition_md_size;
    MPI_Type_size(MPI_IO_PART, &partition_md_size);
    MPI_Offset offset = (long long) partition_md_size * l_io_kp_offset;

    io_partition my_partitions[g_tw_nkp];

    sprintf(filename, "%s.rio-md", g_io_checkpoint_name);
    rc = MPI_File_open(MPI_COMM_WORLD, filename, MPI_MODE_RDONLY, MPI_INFO_NULL, &fh);
    if (rc != 0) {
        printf("ERROR: could not MPI_File_open %s\n", filename);
    }
	MPI_File_read_at_all(fh, offset, &my_partitions, g_tw_nkp, MPI_IO_PART, &status);
    MPI_File_close(&fh);

#ifdef RIO_DEBUG
    for (i = 0; i < g_tw_nkp; i++) {
        printf("Rank %d read metadata\n\tpart %d\n\tfile %d\n\toffset %d\n\tsize %d\n\tlp count %d\n\tevents %d\n\n", mpi_rank,
            my_partitions[i].part, my_partitions[i].file, my_partitions[i].offset,
            my_partitions[i].size, my_partitions[i].lp_count, my_partitions[i].ev_count);
    }
#endif

    // error check
    int count_sum = 0;
    for (i = 0; i < g_tw_nkp; i++) {
        count_sum += my_partitions[i].lp_count;
    }
    assert(count_sum == g_tw_nlp && "ERROR: wrong number of LPs in partitions");

    // read size array
    offset = sizeof(size_t) * l_io_lp_offset;
    size_t * model_sizes = (size_t *) calloc(g_tw_nlp, sizeof(size_t));
    int index = 0;

    sprintf(filename, "%s.rio-lp", g_io_checkpoint_name);
    rc = MPI_File_open(MPI_COMM_WORLD, filename, MPI_MODE_RDONLY, MPI_INFO_NULL, &fh);
    if (rc != 0) {
        printf("ERROR: could not MPI_File_open %s\n", filename);
    }
    for (cur_part = 0; cur_part < g_tw_nkp; cur_part++){
        int data_count = my_partitions[cur_part].lp_count;
        if (cur_part < l_io_min_parts) {
            MPI_File_read_at_all(fh, offset, &model_sizes[index], data_count, MPI_UNSIGNED_LONG, &status);
        } else {
            MPI_File_read_at(fh, offset, &model_sizes[index], data_count, MPI_UNSIGNED_LONG, &status);
        }
        index += data_count;
        offset += (long long) data_count * sizeof(size_t);
    }
    MPI_File_close(&fh);

    // DATA FILES
    int all_lp_i = 0;
    for (cur_part = 0; cur_part < g_tw_nkp; cur_part++) {
        // Read file
        char buffer[my_partitions[cur_part].size];
        void * b = buffer;
        sprintf(filename, "%s.rio-data-%d", g_io_checkpoint_name, my_partitions[cur_part].file);

        // Must use non-collectives, can't know status of other MPI-ranks
        rc = MPI_File_open(MPI_COMM_SELF, filename, MPI_MODE_RDONLY, MPI_INFO_NULL, &fh);
        if (rc != 0) {
            printf("ERROR: could not MPI_File_open %s\n", filename);
        }
        if (cur_part < l_io_min_parts) {
            MPI_File_read_at_all(fh, (long long) my_partitions[cur_part].offset, buffer, my_partitions[cur_part].size, MPI_BYTE, &status);
        } else {
            MPI_File_read_at(fh, (long long) my_partitions[cur_part].offset, buffer, my_partitions[cur_part].size, MPI_BYTE, &status);
        }
        MPI_File_close(&fh);


        // Load LPs
        int c;
        for (c = 0; c < g_tw_nlp; c++) {
            if (g_tw_lp[c]->kp->id == cur_part) {
                b += io_lp_deserialize(g_tw_lp[c], b);
                int lp_type_index = g_tw_lp_typemap(g_tw_lp[c]->gid);
                ((deserialize_f)g_io_lp_types[lp_type_index].deserialize)(g_tw_lp[c]->cur_state, b, g_tw_lp[c]);
                b += model_sizes[all_lp_i];
                all_lp_i++;
            }
        }

        // assert(my_partitions[cur_part].ev_count <= g_io_free_events.size);
        for (i = 0; i < my_partitions[cur_part].ev_count; i++) {
            // SEND THESE EVENTS
            // tw_event *ev = tw_eventq_pop(&g_io_free_events);
            tw_event *ev = io_get_free_event();
            b += io_event_deserialize(ev, b);
            void * msg = tw_event_data(ev);
            memcpy(msg, b, g_tw_msg_sz);
            b += g_tw_msg_sz;
            // buffer event to send after initialization
            tw_eventq_push(&g_io_buffered_events, ev);
        }
    }

    free(model_sizes);

    return;
}

void io_load_events(tw_pe * me) {
    int i;
    int event_count = g_io_buffered_events.size;
    tw_stime original_lookahead = g_tw_lookahead;
    //These messages arrive before the first conservative window
    //checking for valid lookahead is unnecessary
    g_tw_lookahead = 0;
    for (i = 0; i < event_count; i++) {
        me->cur_event = me->abort_event;
        me->cur_event->caused_by_me = NULL;

        tw_event *e = tw_eventq_pop(&g_io_buffered_events);
        // e->dest_lp will be a GID after being loaded from checkpoint
        tw_event *n = tw_event_new((tw_lpid)e->dest_lp, e->recv_ts, e->src_lp);
        void *emsg = tw_event_data(e);
        void *nmsg = tw_event_data(n);
        memcpy(&(n->cv), &(e->cv), sizeof(tw_bf));
        memcpy(nmsg, emsg, g_tw_msg_sz);
        tw_event_free(me, e);
        // tw_eventq_push(&g_io_free_events, e);
        tw_event_send(n);

        if (me->cev_abort) {
            tw_error(TW_LOC, "ran out of events during io_load_events");
        }
    }
    g_tw_lookahead = original_lookahead;
}

void io_create_checkpoint(tw_pe *me, char * master_filename)
{
    // io_prune_transit_queue(me);

    int ranks_per_file = tw_nnodes() / g_io_number_of_files;
    int data_file = g_tw_mynode / ranks_per_file;

    printf("create checkpoint\n");

    io_store_checkpoint(master_filename, data_file);    

    // printf("DONE!\n");
    // tw_error(TW_LOC,"exiting\n");
}


void io_store_checkpoint(char * master_filename, int data_file_number) {
    // printf("Store Start\n");
    int i, c, cur_kp;
    int mpi_rank = g_tw_mynode;
    int number_of_mpitasks = tw_nnodes();
    int rank_events = 0;
    int rank_lps = 0;

    assert(g_io_number_of_files != 0 && "Error: IO variables not set: # of file or # of parts\n");

    // Set up Comms
    MPI_File fh;
    MPI_Status status;
    MPI_Comm file_comm;
    int file_number = data_file_number;
    int file_comm_count;

    MPI_Comm_split(MPI_COMM_WORLD, file_number, g_tw_mynode, &file_comm);
    MPI_Comm_size(file_comm, &file_comm_count);

    MPI_Offset offset;
    long long contribute = 0;

    // printf("rio data\n");

    char filename[256];
    sprintf(filename, "%s.rio-data-%d", master_filename, file_number);

    // ASSUMPTION FOR MULTIPLE PARTS-PER-RANK
    // Each MPI-Rank gets its own file
    io_partition my_partitions[g_tw_nkp];

    size_t all_lp_sizes[g_tw_nlp];
    int all_lp_i = 0;

    for (cur_kp = 0; cur_kp < g_tw_nkp; cur_kp++) {
        int lps_on_kp = g_tw_kp[cur_kp]->lp_count;

        // Gather LP size data
        int lp_size = sizeof(io_lp_store);
        int sum_model_size = 0;

        // always do this loop to allow for interleaved LP types in g_tw_lp
        // TODO: add short cut for one-type, non-dynamic models?
        for (c = 0; c < g_tw_nlp; c++) {
            if (g_tw_lp[c]->kp->id == cur_kp) {
                int lp_type_index = g_tw_lp_typemap(g_tw_lp[c]->gid);
                all_lp_sizes[all_lp_i] = ((model_size_f)g_io_lp_types[lp_type_index].model_size)(g_tw_lp[c]->cur_state, g_tw_lp[c]);
                sum_model_size += all_lp_sizes[all_lp_i];
                all_lp_i++;
            }
        }

        int event_count = 0;
        int sum_event_size = 0;
        if (cur_kp == 0) {
            // Event Metadata
            event_count = g_io_buffered_events.size;
            sum_event_size = event_count * (g_tw_msg_sz + sizeof(io_event_store));
        }

        int sum_lp_size = lps_on_kp * lp_size;
        int sum_size = sum_lp_size + sum_model_size + sum_event_size;

        my_partitions[cur_kp].part = cur_kp + l_io_kp_offset;
        my_partitions[cur_kp].file = file_number;
        my_partitions[cur_kp].size = sum_size;
        my_partitions[cur_kp].lp_count = lps_on_kp;
        my_partitions[cur_kp].ev_count = event_count;

        contribute += sum_size;
        rank_events += event_count;
        rank_lps += lps_on_kp;
    }

    // MPI EXSCAN FOR OFFSET
    offset = (long long) 0;
    if (file_comm_count > 1) {
        MPI_Exscan(&contribute, &offset, 1, MPI_LONG_LONG, MPI_SUM, file_comm);
    }

    int global_lp_i = 0;
    for (cur_kp = 0; cur_kp < g_tw_nkp; cur_kp++) {

        // ** START Serialize **

        int sum_size = my_partitions[cur_kp].size;
        int event_count = my_partitions[cur_kp].ev_count;
        int lps_on_kp = my_partitions[cur_kp].lp_count;

        char buffer[sum_size];
        void * b;

        // LPs
        for (c = 0, b = buffer; c < g_tw_nlp; c++) {
            if (g_tw_lp[c]->kp->id == cur_kp) {
                b += io_lp_serialize(g_tw_lp[c], b);
                int lp_type_index = g_tw_lp_typemap(g_tw_lp[c]->gid);
                ((serialize_f)g_io_lp_types[lp_type_index].serialize)(g_tw_lp[c]->cur_state, b, g_tw_lp[c]);
                b += all_lp_sizes[global_lp_i];
                global_lp_i++;
            }
        }

        // Events
        for (i = 0; i < event_count; i++) {
            tw_event *ev = tw_eventq_pop(&g_io_buffered_events);
            b += io_event_serialize(ev, b);
            void * msg = tw_event_data(ev);
            memcpy(b, msg, g_tw_msg_sz);
            tw_event_free(g_tw_pe[0], ev);
            // tw_eventq_push(&g_io_free_events, ev);
            b += g_tw_msg_sz;
        }

        // Write
        MPI_File_open(file_comm, filename, MPI_MODE_CREATE | MPI_MODE_WRONLY | MPI_MODE_APPEND, MPI_INFO_NULL, &fh);
        if (cur_kp < l_io_min_parts) {
            MPI_File_write_at_all(fh, offset, &buffer, sum_size, MPI_BYTE, &status);
            // possible optimization here: re-calc l_io_min_parts for file_comm
        } else {
            MPI_File_write_at(fh, offset, &buffer, sum_size, MPI_BYTE, &status);
        }
        MPI_File_close(&fh);

        my_partitions[cur_kp].offset = offset;
        offset += (long long) sum_size;
    }

    MPI_Comm_free(&file_comm);

    int amode;
    if (l_io_append_flag) {
        amode = MPI_MODE_CREATE | MPI_MODE_RDWR | MPI_MODE_APPEND;
    } else {
        amode = MPI_MODE_CREATE | MPI_MODE_RDWR;
    }

    // Write Metadata
    MPI_Datatype MPI_IO_PART;
    MPI_Type_contiguous(io_partition_field_count, MPI_INT, &MPI_IO_PART);
    MPI_Type_commit(&MPI_IO_PART);

    int psize;
    MPI_Type_size(MPI_IO_PART, &psize);

    offset = (long long) sizeof(io_partition) * l_io_kp_offset;

    // printf("rio metadata\n");

    sprintf(filename, "%s.rio-md", master_filename);
    MPI_File_open(MPI_COMM_WORLD, filename, amode, MPI_INFO_NULL, &fh);
    MPI_File_write_at_all(fh, offset, &my_partitions, g_tw_nkp, MPI_IO_PART, &status);
    MPI_File_close(&fh);

#ifdef RIO_DEBUG
    for (cur_kp = 0; cur_kp < g_tw_nkp; cur_kp++) {
        printf("Rank %d storing metadata\n\tpart %d\n\tfile %d\n\toffset:\t%lu\n\tsize %lu\n\tlp count %d\n\tevents %d\n\n", mpi_rank,
            my_partitions[cur_kp].part, my_partitions[cur_kp].file, my_partitions[cur_kp].offset,
            my_partitions[cur_kp].size, my_partitions[cur_kp].lp_count, my_partitions[cur_kp].ev_count);
    }
#endif

    // printf("rio lp file\n");


    // Write model size array
    offset = sizeof(size_t) * l_io_lp_offset;
    sprintf(filename, "%s.rio-lp", master_filename);
    MPI_File_open(MPI_COMM_WORLD, filename, amode, MPI_INFO_NULL, &fh);
    MPI_File_write_at_all(fh, offset, all_lp_sizes, g_tw_nlp, MPI_UNSIGNED_LONG, &status);
    MPI_File_close(&fh);

    if (l_io_append_flag == 1) {
        printf("%lu parts written\n", g_tw_nkp);
    }

    int global_events = 0;
    int global_lps = 0;
    MPI_Reduce(&rank_events, &global_events, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);

    // printf("rio txt\n");


    // WRITE READ ME
    if (mpi_rank == 0 && (l_io_append_flag == 0 || data_file_number == 0) ) {
        FILE *file;
        char filename[256];
        sprintf(filename, "%s.txt", master_filename);
        file = fopen(filename, "w+");
        if (file) {
            fprintf(file, "This file was auto-generated by RIO.\n\n");
    #if HAVE_CTIME
            time_t raw_time;
            time(&raw_time);
            fprintf(file, "Date Created:\t%s", ctime(&raw_time));
    #endif

            fprintf(file, "\n## Version Information\n\n");
    #ifdef ROSS_VERSION
            fprintf(file, "ROSS Version:\t%s\n", ROSS_VERSION);
    #endif
    #ifdef RIO_VERSION
            fprintf(file, "RIO Version:\t%s\n", RIO_VERSION);
    #endif
            fprintf(file, "MODEL Version:\t%s\n", model_version);

            fprintf(file, "\n## CHECKPOINT INFORMATION\n\n");
            fprintf(file, "Name:\t\t%s\n", master_filename);
            if (l_io_append_flag == 0) {
                fprintf(file, "Data Files:\t%d\n", g_io_number_of_files);
                fprintf(file, "Partitions:\t%lu\n", l0_io_total_kp);
                fprintf(file, "Total Events:\t%d\n", global_events);
                fprintf(file, "Total LPs:\t%lu\n", l0_io_total_lp);
            } else {
                fprintf(file, "Append Flag:\tON\n");
                fprintf(file, "Data Files:\t%d+?\n", g_io_number_of_files);
                fprintf(file, "Partitions:\t%lu+?\n", l0_io_total_kp);
                fprintf(file, "Total Events:\t%d+?\n", global_events);
                fprintf(file, "Total LPs:\t%lu+?\n", l0_io_total_lp);
            }


            fprintf(file, "\n## BUILD SETTINGS\n\n");
    #ifdef RAND_NORMAL
            fprintf(file, "RAND_NORMAL\tON\n");
    #else
            fprintf(file, "RAND_NORMAL\tOFF\n");
    #endif
    #ifdef ROSS_CLOCK_i386
            fprintf(file, "ARCH:\t\ti386\n");
    #endif
    #ifdef ROSS_CLOCK_amd64
            fprintf(file, "ARCH:\t\tx86_64\n");
    #endif
    #ifdef ROSS_CLOCK_ia64
            fprintf(file, "ARCH:\t\tia64\n");
    #endif
    #ifdef ROSS_CLOCK_ppc
            fprintf(file, "ARCH:\t\tPPC 64\n");
    #endif
    #ifdef ROSS_CLOCK_bgl
            fprintf(file, "ARCH:\t\tBG/L\n");
    #endif
    #ifdef ROSS_CLOCK_bgq
            fprintf(file, "ARCH:\t\tBG/Q\n");
    #endif

            fprintf(file, "\n## RUN TIME SETTINGS\n\n");
            tw_opt_settings(file);
            fflush(file);
        }

    }

    // printf("Store End\n");

}

/* Initializes the size of the g_io_lp_types so that lptypes can be registered
* 		This is necessary as complicated models may not have a singular location where all LP types
* 		can be defined or referenced in a single file. Overcomes scope issues with the Codes simulator
* @param total_lp_types the total number of unique LP types that need their own io_lptype struct
* @note LP types that are derivative and super'd by the codes modelnet_base_lp should not be
*		considered in this count. They are considered modelnet_base_lps.
*/
void io_init_lp_types(size_t total_lp_types)
{
    io_lp_type_registry = (io_lptype*) calloc(total_lp_types+1, sizeof(io_lptype));
}

/* Registers an io_lptype with RIO. Not necessary if g_io_lp_types is manually defined.
* 		This is necessary as complicated models may not have a singular location where all LP types
* 		can be defined or referenced in a single file. Overcomes scope issues with the Codes simulator
* @param new_lptype io_lptype struct necessary to serialize/deserialize the LP related to it
* @param size_t map_position where in the g_tw_lp_typemap is the LP that relates to these io_lptypes
* @note codes does not initialize g_tw_lp_typemap or g_tw_lp_types
*/
void io_register_lp_type(io_lptype * new_lptype, size_t map_position)
{
    io_lp_type_registry[map_position] = *new_lptype;
}

void io_finalize_registry()
{
    g_io_lp_types = io_lp_type_registry;
}

// void io_register_lp_typemap(tw_typemap_f * typemap_fun_ptr)
// {
//     g_tw_lp_typemap = typemap_fun_ptr;
// }

/* Cleans up the g_io_lp_types variable as RIO alloc's this during io_init_lp_types() */
void io_cleanup_lp_types()
{
    free(io_lp_type_registry);
}