/*-------------------------------------------------------------------------
 *
 * message_def.h
 *
 * Portions Copyright (c) 2023-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	   src/message_def.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef MESSAGE_DEF_H
#define MESSAGE_DEF_H

typedef struct TestMessage
{
	int a;
	int b;
} TestMessage;

#define MSG_DEBUG 1
#define MSG_TestMessage 2
#define TIMEOUT_EVENT 3

#endif
