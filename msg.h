#include "postgres.h"
#include "storage/dsm.h"

typedef struct DsmLooper DsmLooper;

typedef struct ShmMessage {
	// FIXME: Find a smart way to handle this. This will be a invalid pointer in counterparty
	dsm_segment* msg_seg;
	int message_id;

	void* payload;
} ShmMessage;

DsmLooper* init_looper(const char* name);
DsmLooper* attach_looper(const char* name);
ShmMessage* alloc_message(size_t msg_len);
void send_message(DsmLooper* looper, const ShmMessage* msg);
ShmMessage* receive_message(const DsmLooper* looper);



