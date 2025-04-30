#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <assert.h>
#include <immintrin.h>
#include <mpi.h>
#include <infiniband/verbs.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <locale.h>

// Constants for the hash table and RDMA operations

#define TOTAL_BUCKETS (256)
#define ENTRY_SIZE (128)
#define MAX_BUCKET_ENTRIES (32767)
#define SIZE_PER_BUCKET (4 * 1024 * 1024)
#define GLOBAL_TABLE_SIZE (1 * 1024 * 1024 * 1024)

// Global variables for MPI and data chunk handling

int rank, size, local_chunk_size, chunk_offset, pending_reads;
int *chunk_sizes = NULL, *chunk_offsets = NULL;
char *local_buffer = NULL;
void *memory_block;
static int system_page_size;

// Structures for RDMA connection details and context

struct connection_details {
    int lid, qpn, psn;
    uint32_t rkey;
    uint64_t addr;
};

struct rdma_context {
    struct ibv_context *device_context;
    struct ibv_comp_channel *completion_channel;
    struct ibv_pd *protection_domain;
    struct ibv_mr *registered_memory;
    struct ibv_dm *device_memory;
    struct ibv_cq *completion_queue;
    struct ibv_qp *queue_pair;
    struct ibv_qp_ex *extended_queue_pair;
    char *buf;
    int size, transmission_flags, receive_depth, pending;
    struct ibv_port_attr port_attributes;
};

// Data structures for word records and hash buckets

typedef struct {
    char current_word[64];
    uint64_t frequency, locations[7];
} Record;

typedef struct {
    char header[128];
    Record records[MAX_BUCKET_ENTRIES];
    int record_count;
} Bucket;

typedef struct {
    char current_word[64];
    int bucket_id;
    uint64_t location;
} WordPacket;

typedef struct {
    int *chunk_sizes, *chunk_offsets;
} ChunkDetails;

// Global RDMA resources

struct ibv_mr *all_mrs;
struct ibv_cq *completion_queue;
struct ibv_qp *queue_pair;
uint16_t *all_qp_numbers;
struct connection_details *all_info;
Bucket **word_buckets;

// Function prototypes

void modify_hash_table(WordPacket word_packet);
uint16_t getLocalId(struct ibv_context *device_context, int ib_port);
uint32_t getQueuePairNumber(struct ibv_qp *queue_pair);
unsigned int generate_word_hash(const char *current_word);
int location_compare(const void *a, const void *b);
void identify_highest_frequency();
char **process_query_file(const char *filename, int *unique_word_count);
static int check_rdma_completion(struct rdma_context *context, int operation_type);
static struct rdma_context *initialize_connection_context(struct ibv_device *device, int port_number);
static int establish_rdma_connection(struct rdma_context *context, int port_number, int psn, struct connection_details *destination);
static int post_receive_buffer(struct rdma_context *context, int receive_count);
int retrieve_port_info(struct ibv_context *device_context, int port, struct ibv_port_attr *port_attr);
void create_contiguous_memory();
void initialize_rdma_resources(const char *query_file);
int determine_file_size(const char *filename);
ChunkDetails compute_offsets(int total_file_size, int num_processes);
char *read_file_chunk(const char *file_path, int chunk_size, int start_offset, int process_rank);
int find_last_alphanumeric_index(const char *buffer, int size);
void fix_word_split_across_chunks(int rank, int size, char **local_buffer, int *local_chunk_size, int *chunk_offset);
void analyze_text_buffer(char *buffer, int portion_size);
void initialize_hash_table();
void destroy_hash_table(Bucket **table);
void modify_hash_table(WordPacket word_packet);

uint16_t getLocalId(struct ibv_context *device_context, int ib_port) {
    struct ibv_port_attr port_attrs;
    if (ibv_query_port(device_context, ib_port, &port_attrs) != 0) {
        return 0;
    }
    return port_attrs.lid;
}

uint32_t getQueuePairNumber(struct ibv_qp *queue_pair) {
    return queue_pair ? queue_pair->qp_num : 0;
}

// Hash function to generate a hash value for a word

unsigned int generate_word_hash(const char *current_word) {
    unsigned int hash_value = 0;
    unsigned int even_multiplier = 121, odd_multiplier = 1331;
    for (size_t idx = 0; current_word[idx] != '\0'; idx++) {
        unsigned int multiplier = (idx & 1) ? odd_multiplier : even_multiplier;
        hash_value += toupper(current_word[idx]) * multiplier;
    }
    return hash_value % 2048;
}

// Comparison function for sorting locations

int location_compare(const void *a, const void *b) {
    uint64_t location_a = *(const uint64_t *)a;
    uint64_t location_b = *(const uint64_t *)b;

    return (location_a > location_b) - (location_a < location_b);
}

// Identifies and prints the record with the highest frequency

void identify_highest_frequency() {
    Record *most_frequent_record = NULL;

    // Iterate through all buckets to find the most frequent word

    for (int i = 0; i < TOTAL_BUCKETS; i++) {
        Bucket *current_bucket = word_buckets[i];
        if (current_bucket && current_bucket->record_count > 0) {
            for (int j = 0; j < current_bucket->record_count; j++) {
                Record *current_record = &current_bucket->records[j];
                if (most_frequent_record == NULL || current_record->frequency > most_frequent_record->frequency) {
                    most_frequent_record = current_record;
                }
            }
        }
    }

    if (most_frequent_record == NULL) {
        printf("Rank %d: No valid records found.\n", rank);
        return;
    }

    // Sort locations and print the result

    qsort(most_frequent_record->locations, 7, sizeof(uint64_t), location_compare);

    printf("Rank %d: %s - Freq: %lu; Loc (<= 7): ", rank,
           most_frequent_record->current_word,
           most_frequent_record->frequency);

    // Print up to 7 locations where the word appears

    for (int k = 0; k < 7; k++) {
        if (most_frequent_record->locations[k] != 0) {
            printf("%lu ", most_frequent_record->locations[k]);
        }
    }

    printf("\n");
}

// Processes the query file and returns a list of unique words

char **process_query_file(const char *filename, int *unique_word_count) {
    printf("\n====== ====== ====== ======\n   Starting the query ... \n====== ====== ====== ======\n");

    FILE *input_file = fopen(filename, "r");
    if (!input_file) {
        fprintf(stderr, "Cannot open file %s.\n", filename);
        exit(EXIT_FAILURE);
    }

    int capacity = 10;
    *unique_word_count = 0;
    char **words = malloc(capacity * sizeof(char *));
    if (!words) {
        fprintf(stderr, "Memory allocation failed.\n");
        exit(EXIT_FAILURE);
    }

    char current_char;
    char word_buffer[64] = {0};
    int buffer_index = 0;

    // Helper function to check for duplicates

    int is_word_duplicate(char **word_list, int total_count, const char *word) {
        for (int i = 0; i < total_count; ++i) {
            if (strcmp(word_list[i], word) == 0) {
                return 1;
            }
        }
        return 0;
    }

    // Read words from the file and store unique ones

    while ((current_char = fgetc(input_file)) != EOF) {
        if (isalnum(current_char)) {
            if (buffer_index < sizeof(word_buffer) - 1) {
                word_buffer[buffer_index++] = tolower(current_char);
            }
        } else if (buffer_index > 0) {
            word_buffer[buffer_index] = '\0';

            // Convert word to uppercase

            for (int i = 0; word_buffer[i] != '\0'; ++i) {
                word_buffer[i] = toupper(word_buffer[i]);
            }

            // Add word if it's not a duplicate

            if (!is_word_duplicate(words, *unique_word_count, word_buffer)) {
                words[(*unique_word_count)++] = strdup(word_buffer);

                // Reallocate memory if capacity is reached

                if (*unique_word_count >= capacity) {
                    capacity *= 2;
                    words = realloc(words, capacity * sizeof(char *));
                    if (!words) {
                        fprintf(stderr, "Failed to reallocate memory.\n");
                        exit(EXIT_FAILURE);
                    }
                }
            }
            buffer_index = 0;
        }
    }

    // Handle the last word if any

    if (buffer_index > 0) {
        word_buffer[buffer_index] = '\0';

        for (int i = 0; word_buffer[i] != '\0'; ++i) {
            word_buffer[i] = toupper(word_buffer[i]);
        }

        if (!is_word_duplicate(words, *unique_word_count, word_buffer)) {
            words[(*unique_word_count)++] = strdup(word_buffer);
        }
    }

    fclose(input_file);
    return words;
}

// Checks for RDMA operation completion

static int check_rdma_completion(struct rdma_context *context, int operation_type) {
    struct ibv_wc completion_entry;
    int poll_result;
    int flushed_status = 0;

    // Poll the completion queue for completion entries

    while ((poll_result = ibv_poll_cq(context->completion_queue, 1, &completion_entry)) == 1) {
        if (completion_entry.status == IBV_WC_SUCCESS) {
            if ((completion_entry.opcode == IBV_WC_RDMA_WRITE && operation_type == IBV_WC_RDMA_WRITE) ||
                (completion_entry.opcode == IBV_WC_RDMA_READ && operation_type == IBV_WC_RDMA_READ)) {
                return 0;
            }
        } else if (completion_entry.status == IBV_WC_WR_FLUSH_ERR) {
            flushed_status = 1;
        } else {
            fprintf(stderr, "Completion queue returned status %d\n", completion_entry.status);
            return -1;
        }
    }

    if (poll_result < 0) {
        fprintf(stderr, "Polling issue occurred (%d)\n", poll_result);
    }

    return flushed_status;
}

// Initializes RDMA connection context

static struct rdma_context *initialize_connection_context(struct ibv_device *device, int port_number) {
    struct rdma_context *context = calloc(1, sizeof(struct rdma_context));
    if (!context) {
        fprintf(stderr, "RDMA context allocation failed.\n");
        return NULL;
    }

    int memory_permissions = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ;
    context->size = 4096;
    context->receive_depth = 500;
    context->transmission_flags = IBV_SEND_SIGNALED;

    // Allocate aligned buffer

    if (posix_memalign((void **)&context->buf, system_page_size, context->size) != 0) {
        fprintf(stderr, "Aligned buffer allocation failed.\n");
        free(context);
        return NULL;
    }

    memset(context->buf, 0x7b, context->size);

    // Open RDMA device

    context->device_context = ibv_open_device(device);
    if (!context->device_context) {
        fprintf(stderr, "Unable to open device: %s.\n", ibv_get_device_name(device));
        free(context->buf);
        free(context);
        return NULL;
    }

    // Create completion channel

    context->completion_channel = ibv_create_comp_channel(context->device_context);
    if (!context->completion_channel) {
        fprintf(stderr, "Completion channel creation failed.\n");
        ibv_close_device(context->device_context);
        free(context->buf);
        free(context);
        return NULL;
    }

    // Allocate protection domain

    context->protection_domain = ibv_alloc_pd(context->device_context);
    if (!context->protection_domain) {
        fprintf(stderr, "Protection domain allocation failed.\n");
        ibv_destroy_comp_channel(context->completion_channel);
        ibv_close_device(context->device_context);
        free(context->buf);
        free(context);
        return NULL;
    }

    // Register memory region

    context->registered_memory = ibv_reg_mr(context->protection_domain, context->buf, context->size, memory_permissions);
    if (!context->registered_memory) {
        fprintf(stderr, "Memory region registration failed.\n");
        ibv_dealloc_pd(context->protection_domain);
        ibv_destroy_comp_channel(context->completion_channel);
        ibv_close_device(context->device_context);
        free(context->buf);
        free(context);
        return NULL;
    }

    // Create completion queue

    context->completion_queue = ibv_create_cq(context->device_context, 501, NULL, context->completion_channel, 0);
    if (!context->completion_queue) {
        fprintf(stderr, "Completion queue creation failed.\n");
        ibv_dereg_mr(context->registered_memory);
        ibv_dealloc_pd(context->protection_domain);
        ibv_destroy_comp_channel(context->completion_channel);
        ibv_close_device(context->device_context);
        free(context->buf);
        free(context);
        return NULL;
    }

    // Set up Queue Pair attributes

    struct ibv_qp_init_attr qp_attributes = {
        .send_cq = context->completion_queue,
        .recv_cq = context->completion_queue,
        .cap = {
            .max_send_wr = 300,
            .max_recv_wr = 300,
            .max_send_sge = 1,
            .max_recv_sge = 1,
            .max_inline_data = 16
        },
        .qp_type = IBV_QPT_RC,
        .sq_sig_all = 1
    };

    // Create Queue Pair

    context->queue_pair = ibv_create_qp(context->protection_domain, &qp_attributes);
    if (!context->queue_pair) {
        fprintf(stderr, "Queue pair creation failed.\n");
        ibv_destroy_cq(context->completion_queue);
        ibv_dereg_mr(context->registered_memory);
        ibv_dealloc_pd(context->protection_domain);
        ibv_destroy_comp_channel(context->completion_channel);
        ibv_close_device(context->device_context);
        free(context->buf);
        free(context);
        return NULL;
    }

    // Transition Queue Pair to INIT state

    struct ibv_qp_attr init_attributes = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = port_number,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE
    };

    if (ibv_modify_qp(context->queue_pair, &init_attributes,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) != 0) {
        fprintf(stderr, "Failed to transition queue pair to INIT state.\n");
        ibv_destroy_qp(context->queue_pair);
        ibv_destroy_cq(context->completion_queue);
        ibv_dereg_mr(context->registered_memory);
        ibv_dealloc_pd(context->protection_domain);
        ibv_destroy_comp_channel(context->completion_channel);
        ibv_close_device(context->device_context);
        free(context->buf);
        free(context);
        return NULL;
    }

    return context;
}

// Establishes RDMA connection to a destination

static int establish_rdma_connection(struct rdma_context *context, int port_number, int psn, struct connection_details *destination) {

    // Transition to RTR (Ready to Receive) state

    struct ibv_qp_attr qp_attr_rtr;
    memset(&qp_attr_rtr, 0, sizeof(qp_attr_rtr));
    qp_attr_rtr.qp_state = IBV_QPS_RTR;
    qp_attr_rtr.path_mtu = IBV_MTU_1024;
    qp_attr_rtr.dest_qp_num = destination->qpn;
    qp_attr_rtr.rq_psn = destination->psn;
    qp_attr_rtr.max_dest_rd_atomic = 1;
    qp_attr_rtr.min_rnr_timer = 12;

    qp_attr_rtr.ah_attr.is_global = 0;
    qp_attr_rtr.ah_attr.dlid = destination->lid;
    qp_attr_rtr.ah_attr.sl = 0;
    qp_attr_rtr.ah_attr.src_path_bits = 0;
    qp_attr_rtr.ah_attr.port_num = port_number;

    if (ibv_modify_qp(context->queue_pair, &qp_attr_rtr,
                      IBV_QP_STATE |
                      IBV_QP_AV |
                      IBV_QP_PATH_MTU |
                      IBV_QP_DEST_QPN |
                      IBV_QP_RQ_PSN |
                      IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
        fprintf(stderr, "Failed to transition QP to RTR state.\n");
        return 1;
    }

    // Transition to RTS (Ready to Send) state

    struct ibv_qp_attr qp_attr_rts;
    memset(&qp_attr_rts, 0, sizeof(qp_attr_rts));
    qp_attr_rts.qp_state = IBV_QPS_RTS;
    qp_attr_rts.timeout = 14;
    qp_attr_rts.retry_cnt = 7;
    qp_attr_rts.rnr_retry = 7;
    qp_attr_rts.sq_psn = psn;
    qp_attr_rts.max_rd_atomic = 1;

    if (ibv_modify_qp(context->queue_pair, &qp_attr_rts,
                      IBV_QP_STATE |
                      IBV_QP_TIMEOUT |
                      IBV_QP_RETRY_CNT |
                      IBV_QP_RNR_RETRY |
                      IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC)) {
        fprintf(stderr, "Failed to transition QP to RTS state.\n");
        return 1;
    }

    return 0;
}

// Posts receive buffers for incoming RDMA messages

static int post_receive_buffer(struct rdma_context *context, int receive_count) {
    int posted_count = 0;

    // Post multiple receive work requests

    while (posted_count < receive_count) {
        struct ibv_sge scatter_gather_element = {
            .addr = (uintptr_t)(context->buf + (posted_count * context->size / receive_count)),
            .length = context->size / receive_count,
            .lkey = context->registered_memory->lkey
        };

        struct ibv_recv_wr work_request = {
            .wr_id = (uint64_t)posted_count,
            .sg_list = &scatter_gather_element,
            .num_sge = 1,
            .next = NULL
        };

        struct ibv_recv_wr *failed_request = NULL;

        if (ibv_post_recv(context->queue_pair, &work_request, &failed_request)) {
            fprintf(stderr, "Failed to post receive buffer %d\n", posted_count);
            break;
        }
        posted_count++;
    }

    return posted_count;
}

// Retrieves port information for the RDMA device

int retrieve_port_info(struct ibv_context *device_context, int port, struct ibv_port_attr *port_attr) {
    if (!device_context || !port_attr) {
        fprintf(stderr, "Invalid device context or attribute structure.\n");
        return -1;
    }
    return ibv_query_port(device_context, port, port_attr);
}

// Creates a contiguous memory block for RDMA operations

void create_contiguous_memory() {
    memory_block = calloc(TOTAL_BUCKETS, sizeof(Bucket));
    if (!memory_block) {
        fprintf(stderr, "Memory allocation failed for contiguous block.\n");
        exit(EXIT_FAILURE);
    }

    // Copy existing buckets into the contiguous memory block

    for (int i = 0; i < TOTAL_BUCKETS; i++) {
        if (word_buckets[i]) {
            memcpy((char *)memory_block + i * sizeof(Bucket), word_buckets[i], sizeof(Bucket));
        }
    }
}

// Initializes RDMA resources and handles RDMA operations

void initialize_rdma_resources(const char *query_file) {
    struct rdma_context *connect[size];
    struct connection_details my_info[size];
    struct ibv_mr *registered_memory[size];
    struct ibv_device **dev_list;
    struct ibv_device *ib_dev;

    system_page_size = sysconf(_SC_PAGESIZE);
    dev_list = ibv_get_device_list(NULL);

    if (!dev_list) {
        perror("Failed to get IB devices list");
        return;
    }
    ib_dev = *dev_list;

    // Initialize RDMA contexts for all processes

    for (int l = 0; l < size; l++) {
        connect[l] = initialize_connection_context(ib_dev, 1);
        if (!connect[l])
            return;

        int routs = post_receive_buffer(connect[l], connect[l]->receive_depth);
        if (routs < connect[l]->receive_depth ||
            ibv_req_notify_cq(connect[l]->completion_queue, 0) ||
            retrieve_port_info(connect[l]->device_context, 1, &connect[l]->port_attributes)) {
            fprintf(stderr, "Initialization error in loop %d (routs=%d)\n", l, routs);
            return;
        }

        // Store local RDMA connection details

        my_info[l].lid = connect[l]->port_attributes.lid;
        if (connect[l]->port_attributes.link_layer != IBV_LINK_LAYER_ETHERNET && !my_info[l].lid) {
            return;
        }

        my_info[l].qpn = connect[l]->queue_pair->qp_num;
        my_info[l].psn = lrand48() & 0xffffff;

        // Create contiguous memory and register it

        create_contiguous_memory();
        registered_memory[l] = ibv_reg_mr(connect[l]->protection_domain, memory_block, TOTAL_BUCKETS * sizeof(Bucket),
                                          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);
        my_info[l].rkey = registered_memory[l]->rkey;
        my_info[l].addr = (uint64_t)registered_memory[l]->addr;
    }

    // Exchange RDMA connection details with all other processes

    struct connection_details *all_info = malloc(size * size * sizeof(struct connection_details));
    if (!all_info) {
        fprintf(stderr, "Memory allocation failed for all_info.\n");
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }
    MPI_Allgather(my_info, size * sizeof(struct connection_details), MPI_BYTE, all_info, size * sizeof(struct connection_details), MPI_BYTE, MPI_COMM_WORLD);

    // Establish RDMA connections with all other processes

    for (int l = 0; l < size; l++) {
        int ret = establish_rdma_connection(connect[l], 1, my_info[l].psn, &all_info[l * size + rank]);
        if (ret == 1)
            printf("Failed for rank %d, l=%d\n", rank, l);
    }
    MPI_Barrier(MPI_COMM_WORLD);

    // Process the query file and perform RDMA operations if rank is 1

    if (rank == 1) {
        int unique_word_count = 0;
        char **words = process_query_file(query_file, &unique_word_count);
        for (int index = 0; index < unique_word_count; index++) {
            unsigned int word_hash = generate_word_hash(words[index]);
            int process_id = (word_hash >> 8) & 0x7;
            int bucket_id = word_hash & 0xFF;
            struct ibv_send_wr wr, *bad_wr;
            struct ibv_sge sge;
            uint32_t rdma_rkey = all_info[process_id * size + rank].rkey;
            uint64_t remote_addr = all_info[process_id * size + rank].addr + bucket_id * sizeof(Bucket);
            Bucket *local_bucket = (Bucket *)malloc(sizeof(Bucket));
            if (local_bucket == NULL) {
                fprintf(stderr, "Failed to allocate memory for local_bucket.\n");
                exit(1);
            }

            // Initialize local bucket

            for (int index = 0; index < MAX_BUCKET_ENTRIES; index++) {
                memset(local_bucket->records[index].current_word, '\0', sizeof(local_bucket->records[index].current_word));
                local_bucket->records[index].frequency = 0;
                for (int sub_index = 0; sub_index < 7; sub_index++)
                    local_bucket->records[index].locations[sub_index] = 0;
            }

            // Register local bucket memory region

            struct ibv_mr *mr1 = ibv_reg_mr(connect[process_id]->protection_domain, local_bucket, sizeof(Bucket),
                                            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);
            if (!mr1) {
                fprintf(stderr, "Memory registration failed.\n");
                exit(1);
            }

            // Prepare RDMA read work request

            sge.addr = (uint64_t)local_bucket;
            sge.length = sizeof(Bucket);
            sge.lkey = mr1->lkey;

            memset(&wr, 0, sizeof(wr));
            wr.opcode = IBV_WR_RDMA_READ;
            wr.wr.rdma.remote_addr = remote_addr;
            wr.wr.rdma.rkey = rdma_rkey;
            wr.sg_list = &sge;
            wr.num_sge = 1;

            // Post RDMA read operation

            if (ibv_post_send(connect[process_id]->queue_pair, &wr, &bad_wr)) {
                fprintf(stderr, "RDMA read request failed.\n");
                exit(1);
            }

            // Wait for RDMA read completion

            int ret = check_rdma_completion(connect[process_id], IBV_WR_RDMA_READ);

            // Null-terminate the words in the bucket

            for (int inner_index = 0; inner_index < 32767; inner_index++) {
                local_bucket->records[inner_index].current_word[sizeof(local_bucket->records[inner_index].current_word) - 1] = '\0';
            }

            // Search for the word in the bucket and print frequency and locations

            if (local_bucket->records[0].current_word[0] != '0') {
                unsigned char *raw_data = (unsigned char *)local_bucket->records[0].current_word;
            }
            for (int inner_index = 0; inner_index < 32767; inner_index++) {
                if (strcmp(local_bucket->records[inner_index].current_word, words[index]) == 0) {
                    printf("%s - Freq: %lu", local_bucket->records[inner_index].current_word, local_bucket->records[inner_index].frequency);
                    int loc_count = 0;
                    qsort(local_bucket->records[inner_index].locations, 7, sizeof(uint64_t), location_compare);
                    for (int sub_index = 0; sub_index < 7; sub_index++) {
                        if (local_bucket->records[inner_index].locations[sub_index] != 0) {
                            if (loc_count == 0) {
                                printf("; Loc (<= 7): ");
                            } else {
                                printf(" ");
                            }
                            printf("%lu", local_bucket->records[inner_index].locations[sub_index]);
                            loc_count++;
                        }
                    }
                    if (loc_count == 0) {
                        printf("\n");
                    } else {
                        printf(" \n");
                    }
                    break;
                }
                printf("%s - Freq: %d ", words[index], 0);
                printf("\n");
                break;
            }
        }
    }
}

// Determines the size of the input file

int determine_file_size(const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        fprintf(stderr, "Unable to open file %s.\n", filename);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "Failed to seek to the end of the file %s.\n", filename);
        fclose(file);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    int size = ftell(file);
    if (size == -1) {
        fprintf(stderr, "Could not determine size of the file %s.\n", filename);
        fclose(file);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    fclose(file);
    return size;
}

// Computes chunk sizes and offsets for each process

ChunkDetails compute_offsets(int total_file_size, int num_processes) {
    ChunkDetails result;
    result.chunk_sizes = malloc(num_processes * sizeof(int));
    result.chunk_offsets = malloc(num_processes * sizeof(int));

    if (!result.chunk_sizes || !result.chunk_offsets) {
        fprintf(stderr, "Memory allocation failed for chunk details.\n");
        free(result.chunk_sizes);
        free(result.chunk_offsets);
        exit(EXIT_FAILURE);
    }

    int chunk_size = total_file_size / num_processes;
    int extra_bytes = total_file_size % num_processes;
    int offset = 0;

    // Distribute file size among processes

    for (int i = 0; i < num_processes; i++) {
        result.chunk_sizes[i] = chunk_size + (i < extra_bytes ? 1 : 0);
        result.chunk_offsets[i] = offset;
        offset += result.chunk_sizes[i];
    }

    return result;
}

// Reads a specific chunk of the file

char *read_file_chunk(const char *file_path, int chunk_size, int start_offset, int process_rank) {
    FILE *file = fopen(file_path, "rb");
    if (!file) {
        fprintf(stderr, "Process %d could not open file %s for reading.\n", process_rank, file_path);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    char *buffer = malloc((chunk_size + 1) * sizeof(char));
    if (!buffer) {
        fprintf(stderr, "Process %d failed to allocate memory for buffer.\n", process_rank);
        fclose(file);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    if (fseek(file, start_offset, SEEK_SET) != 0) {
        fprintf(stderr, "Process %d could not seek to offset %d in file %s.\n", process_rank, start_offset, file_path);
        free(buffer);
        fclose(file);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    // Read the chunk into the buffer

    size_t bytes_read = fread(buffer, 1, chunk_size, file);
    if (bytes_read != (size_t)chunk_size) {
        fprintf(stderr, "Warning: Process %d read %zu bytes instead of %d bytes from file %s.\n", process_rank, bytes_read, chunk_size, file_path);
    }
    buffer[bytes_read] = '\0';

    fclose(file);
    return buffer;
}

// Finds the index of the last non-alphanumeric character

int find_last_alphanumeric_index(const char *buffer, int size) {
    for (int i = size - 1; i >= 0; i--) {
        if (!isalnum(buffer[i])) {
            return i;
        }
    }
    return -1;
}

// Adjusts for words split across chunk boundaries

void fix_word_split_across_chunks(int rank, int size, char **local_buffer, int *local_chunk_size, int *chunk_offset) {

    // If not the first process, receive extra characters from the previous process

    if (rank > 0) {
        int chars_to_receive;
        MPI_Recv(&chars_to_receive, 1, MPI_INT, rank - 1, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        if (chars_to_receive > 0) {
            char *received_buffer = malloc(chars_to_receive + 1);
            if (!received_buffer) {
                fprintf(stderr, "Failed to allocate memory for received data.\n");
                MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
            }

            MPI_Recv(received_buffer, chars_to_receive + 1, MPI_CHAR, rank - 1, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            *chunk_offset -= chars_to_receive;
            *local_buffer = realloc(*local_buffer, *local_chunk_size + chars_to_receive + 1);
            if (!*local_buffer) {
                fprintf(stderr, "Failed to reallocate buffer.\n");
                free(received_buffer);
                MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
            }

            // Prepend received characters to the local buffer

            memmove((*local_buffer) + chars_to_receive, *local_buffer, *local_chunk_size);
            memcpy(*local_buffer, received_buffer, chars_to_receive);

            *local_chunk_size += chars_to_receive;
            (*local_buffer)[*local_chunk_size] = '\0';
            free(received_buffer);
        }
    }

    // If not the last process, send extra characters to the next process

    if (rank < size - 1) {
        int split_index = find_last_alphanumeric_index(*local_buffer, *local_chunk_size) + 1;
        int chars_to_send = *local_chunk_size - split_index;

        MPI_Send(&chars_to_send, 1, MPI_INT, rank + 1, 0, MPI_COMM_WORLD);

        if (chars_to_send > 0) {
            MPI_Send(*local_buffer + split_index, chars_to_send + 1, MPI_CHAR, rank + 1, 0, MPI_COMM_WORLD);
        }

        *local_chunk_size = split_index;
        (*local_buffer)[*local_chunk_size] = '\0';
    }
}

// Analyzes the text buffer and distributes word packets to appropriate processes

void analyze_text_buffer(char *buffer, int portion_size) {
    int index = 0, packet_counts[8] = {0};
    WordPacket **packet_list = malloc(8 * sizeof(WordPacket *));
    for (int index = 0; index < 8; index++)
        packet_list[index] = malloc(packet_counts[index] * sizeof(WordPacket));

    // Parse words in the buffer

    while (index < portion_size) {
        while (index < portion_size && !isalnum(buffer[index]))
            index++;
        int word_start = index;
        while (index < portion_size && isalnum(buffer[index]))
            index++;
        int word_end = index;
        if (word_end - word_start >= 1 && word_end - word_start <= 62) {
            char current_word[63] = {0};
            strncpy(current_word, buffer + word_start, word_end - word_start);
            for (int inner_index = 0; inner_index < word_end - word_start; inner_index++)
                current_word[inner_index] = toupper(current_word[inner_index]);

            // Hash the word to determine the target process and bucket

            unsigned int word_hash = generate_word_hash(current_word);
            int process_id = (word_hash >> 8) & 0x7;
            int bucket_id = word_hash & 0xFF;
            uint64_t location = chunk_offset + word_start;
            packet_counts[process_id]++;
            packet_list[process_id] = realloc(packet_list[process_id], sizeof(WordPacket) * packet_counts[process_id]);
            if (packet_list[process_id] == NULL) {
                fprintf(stderr, "Memory allocation failed for packet_list in process %d\n", process_id);
                MPI_Abort(MPI_COMM_WORLD, 1);
            }

            // Create a word packet

            WordPacket word_packet;
            strncpy(word_packet.current_word, current_word, sizeof(word_packet.current_word) - 1);
            word_packet.current_word[sizeof(word_packet.current_word) - 1] = '\0';
            word_packet.bucket_id = bucket_id;
            word_packet.location = location;
            packet_list[process_id][packet_counts[process_id] - 1] = word_packet;
        }
    }

    MPI_Barrier(MPI_COMM_WORLD);
    MPI_Request request;

    // Send packet counts to other processes

    for (int index = 0; index < size; index++) {
        int target_rank = ((rank + index) % size);
        MPI_Isend(&packet_counts[target_rank], 1, MPI_INT, target_rank, 0, MPI_COMM_WORLD, &request);
    }

    int recv_counts[8];

    // Receive packet counts from other processes

    for (int index = 0; index < size; index++) {
        int source_rank = (rank - index + size) % size;
        MPI_Recv(&recv_counts[source_rank], 1, MPI_INT, source_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        int target_rank = (rank + index + 1) % size;
        MPI_Isend(packet_list[target_rank], sizeof(WordPacket) * packet_counts[target_rank], MPI_BYTE, target_rank, 0, MPI_COMM_WORLD, &request);
    }

    // Receive word packets and modify the hash table

    for (int index = 0; index < size; index++) {
        int source_rank = (rank - index + size) % size;
        if (recv_counts[source_rank] > 0) {
            WordPacket *recv_packets = malloc(sizeof(WordPacket) * recv_counts[source_rank]);
            MPI_Recv(recv_packets, sizeof(WordPacket) * recv_counts[source_rank], MPI_BYTE, source_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            for (int inner_index = 0; inner_index < recv_counts[source_rank]; inner_index++) {
                modify_hash_table(recv_packets[inner_index]);
            }
            free(recv_packets);
        }
    }
    free(packet_list);
}

// Initializes the hash table (array of buckets)

void initialize_hash_table() {
    word_buckets = calloc(TOTAL_BUCKETS, sizeof(Bucket *));
    if (word_buckets == NULL) {
        fprintf(stderr, "Unable to allocate memory for the bucket array.\n");
        exit(EXIT_FAILURE);
    }

    for (int bucket_idx = 0; bucket_idx < TOTAL_BUCKETS; bucket_idx++) {
        word_buckets[bucket_idx] = calloc(1, sizeof(Bucket));
        if (word_buckets[bucket_idx] == NULL) {
            fprintf(stderr, "Allocation failed for bucket %d.\n", bucket_idx);

            // Cleanup in case of failure

            for (int cleanup_idx = 0; cleanup_idx < bucket_idx; cleanup_idx++) {
                free(word_buckets[cleanup_idx]);
            }
            free(word_buckets);
            exit(EXIT_FAILURE);
        }

        // Set up records for each bucket

        for (int record_idx = 0; record_idx < MAX_BUCKET_ENTRIES; record_idx++) {
            memset(word_buckets[bucket_idx]->records[record_idx].current_word, '\0',
                   sizeof(word_buckets[bucket_idx]->records[record_idx].current_word));
            word_buckets[bucket_idx]->records[record_idx].frequency = 0;
            memset(word_buckets[bucket_idx]->records[record_idx].locations, 0,
                   sizeof(word_buckets[bucket_idx]->records[record_idx].locations));
        }
    }
}

// Frees the memory allocated for the hash table

void destroy_hash_table(Bucket **table) {
    if (!table) {
        return;
    }

    // Free each bucket

    for (int i = 0; i < TOTAL_BUCKETS; i++) {
        if (table[i]) {
            free(table[i]);
        }
    }
    free(table);
}

// Modifies the hash table by adding or updating a word record

void modify_hash_table(WordPacket word_packet) {
    Bucket *bucket = word_buckets[word_packet.bucket_id];
    if (!bucket) {
        fprintf(stderr, "Bucket %d does not exist.\n", word_packet.bucket_id);
        return;
    }

    // Check if the word already exists in the bucket

    for (int i = 0; i < bucket->record_count; i++) {
        Record *record = &bucket->records[i];
        if (strcmp(record->current_word, word_packet.current_word) == 0) {
            record->frequency++;

            // Update locations

            if (record->locations[6] != 0) {
                int max_index = 0;
                for (int j = 1; j < 7; j++) {
                    if (record->locations[j] > record->locations[max_index]) {
                        max_index = j;
                    }
                }
                if (word_packet.location < record->locations[max_index]) {
                    record->locations[max_index] = word_packet.location;
                }
            } else {
                for (int j = 0; j < 7; j++) {
                    if (record->locations[j] == 0) {
                        record->locations[j] = word_packet.location;
                        break;
                    }
                }
            }
            return;
        }
    }

    // Add a new record if the word doesn't exist

    if (bucket->record_count < MAX_BUCKET_ENTRIES) {
        Record *new_record = &bucket->records[bucket->record_count];
        new_record->frequency = 1;
        new_record->locations[0] = word_packet.location;
        strncpy(new_record->current_word, word_packet.current_word, sizeof(new_record->current_word) - 1);
        new_record->current_word[sizeof(new_record->current_word) - 1] = '\0';
        bucket->record_count++;
    } else {
        fprintf(stderr, "Bucket %d is full, unable to add record '%s'.\n", word_packet.bucket_id, word_packet.current_word);
    }
}

int main(int argc, char *argv[]) {

    // Initialize MPI environment

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    char hostname[20];
    gethostname(hostname, sizeof(hostname));

    if (argc < 3) {
        if (rank == 0) {
            fprintf(stderr, "Usage: %s <filename> <query_file>\n", argv[0]);
        }
        MPI_Finalize();
        return EXIT_FAILURE;
    }

    const char *filename = argv[1], *query_file = argv[2];
    int file_size;
    ChunkDetails sizes_offsets;

    // Allocate memory for chunk sizes and offsets

    chunk_sizes = malloc(size * sizeof(int));
    chunk_offsets = malloc(size * sizeof(int));
    if (!chunk_sizes || !chunk_offsets) {
        fprintf(stderr, "Memory allocation failed for chunk_sizes or chunk_offsets.\n");
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    if (rank == 0) {

        // Determine file size and compute chunk sizes and offsets

        file_size = determine_file_size(filename);
        sizes_offsets = compute_offsets(file_size, size);
        memcpy(chunk_sizes, sizes_offsets.chunk_sizes, size * sizeof(int));
        memcpy(chunk_offsets, sizes_offsets.chunk_offsets, size * sizeof(int));
        free(sizes_offsets.chunk_sizes);
        free(sizes_offsets.chunk_offsets);
    }

    // Broadcast chunk sizes and offsets to all processes

    MPI_Bcast(chunk_sizes, size, MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Bcast(chunk_offsets, size, MPI_INT, 0, MPI_COMM_WORLD);

    // Each process reads its assigned chunk of the file

    local_chunk_size = chunk_sizes[rank];
    chunk_offset = chunk_offsets[rank];
    local_buffer = read_file_chunk(filename, local_chunk_size, chunk_offset, rank);
    fix_word_split_across_chunks(rank, size, &local_buffer, &local_chunk_size, &chunk_offset);

    // Initialize hash table and process the text buffer

    initialize_hash_table();
    analyze_text_buffer(local_buffer, local_chunk_size);
    free(local_buffer);

    MPI_Barrier(MPI_COMM_WORLD);

    // Identify and print the word with the highest frequency in each process

    for (int step = 0; step < size; step++) {
        if (rank == step) {
            identify_highest_frequency();
        }
        MPI_Barrier(MPI_COMM_WORLD);
    }

    // Initialize RDMA resources and perform RDMA operations

    initialize_rdma_resources(query_file);
    destroy_hash_table(word_buckets);

    if (rank == 0) {
        free(chunk_sizes);
        free(chunk_offsets);
    }

    MPI_Finalize();
    return 0;
}
