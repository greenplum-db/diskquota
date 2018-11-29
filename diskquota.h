#ifndef DISK_QUOTA_H
#define DISK_QUOTA_H

#include "storage/lwlock.h"

typedef enum
{
	NAMESPACE_QUOTA,
	ROLE_QUOTA
} QuotaType;

typedef struct
{
	LWLock	   *lock;		/* protects shared memory of blackMap */
} disk_quota_shared_state;
struct DiskQuotaLocks
{
	LWLock *mq_lock;
};
typedef struct DiskQuotaLocks DiskQuotaLocks;
struct MessageBox
{
	int launcher_pid;
	int req_pid;
	int cmd;
	int result;
	int data[4];
};
enum MessageCommand
{
	CMD_CREATE_EXTENSION = 1,
	CMD_DROP_EXTENSION,
};
enum MessageResult
{
	ERR_PENDING = 0,
	ERR_OK,
	ERR_UNKNOWN,
};
#define mb_data_length  sizeof(((MessageBox*)0)->data)
typedef struct MessageBox MessageBox;
typedef enum MessageCommand MessageCommand;
typedef enum MessageResult MessageResult;
// FIXME: it's better to hide the name, not to expose it
extern DiskQuotaLocks dq_locks;
extern MessageBox *message_box;

/* enforcement interface*/
extern void init_disk_quota_enforcement(void);
extern void diskquota_invalidate_db(Oid dbid);

/* quota model interface*/
extern void init_disk_quota_shmem(void);
extern void init_disk_quota_model(void);
extern void refresh_disk_quota_model(bool force);
extern bool quota_check_common(Oid reloid);

/* quotaspi interface */
extern void init_disk_quota_hook(void);

extern int   diskquota_naptime;
extern int   diskquota_max_active_tables;

#endif
