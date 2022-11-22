#include "postgres.h"
#include "storage/dsm.h"

typedef struct DsmLooper DsmLooper;

typedef dsm_segment* (*message_handler)(int message_id, void* req);

// Called by server
DsmLooper* init_looper(const char* name, message_handler handler);
// Called by client
DsmLooper* attach_looper(const char* name);

//ShmMessage* alloc_message(size_t msg_len);
dsm_segment* init_message(int msg_id, size_t payload_len);
void deinit_message(dsm_segment* dsm_seg);
int get_msg_id(dsm_segment* dsm_seg);
void* get_payload_ptr(dsm_segment* dsm_seg);
void send_delayed_event(int timout, void* event);

// Send a request to looper, and block until response arrives
dsm_segment* send_request_and_wait(DsmLooper* looper, dsm_segment* dsm_seg);

void loop(DsmLooper* looper);



