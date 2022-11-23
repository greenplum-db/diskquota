#ifndef MSG_H
#define MSG_H

#include "postgres.h"
#include "storage/dsm.h"

typedef dsm_segment *(*message_handler)(int message_id, void *req);

typedef enum
{
	TIMEOUT_EVENT,
} MSG_ID;

typedef struct DsmLooper
{
	pid_t server_pid;
	pid_t client_pid;

	Latch server_latch;
	Latch client_latch;

	// Clients holds this lock before sending request, and release it after receiving response
	LWLock *loop_lock;
	// Server/Clients hold this lock when reading/writing messages
	LWLock *msg_lock;

	dsm_handle req_handle;
	dsm_handle rsp_handle;

	// Private to server
	message_handler msg_handler;
} DsmLooper;

// Called by server
DsmLooper *init_looper(const char *name, message_handler handler);
// Called by client
DsmLooper *attach_looper(const char *name);

// ShmMessage* alloc_message(size_t msg_len);
dsm_segment *init_message(int msg_id, size_t payload_len);
void         deinit_message(dsm_segment *dsm_seg);
int          get_msg_id(dsm_segment *dsm_seg);
void        *get_payload_ptr(dsm_segment *dsm_seg);
void         send_delayed_event(int timout, void *event);

// Send a request to looper, and block until response arrives
dsm_segment *send_request_and_wait(DsmLooper *looper, dsm_segment *dsm_seg);

void loop(DsmLooper *looper);

extern dsm_segment *diskquota_message_handler(int message_id, void *req);
extern void         init_timeout(void);
extern bool         handle_signal(void);
extern void         bgworker_schedule(void);
extern void         disk_quota_timeout(void);
#endif
