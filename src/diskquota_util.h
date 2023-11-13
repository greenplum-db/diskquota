/*-------------------------------------------------------------------------
 *
 * diskquota_util.h
 *
 * Portions Copyright (c) 2023-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	   src/diskquota_util.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef DISKQUOTA_UTIL_H
#define DISKQUOTA_UTIL_H

extern Datum get_oid_auto_case_convert(Oid (*f)(const char *name, bool missing_ok), const char *name);
extern int64 get_size_in_mb(char *str);
extern void  check_superuser(void);

extern char *GetNamespaceName(Oid spcid, bool skip_name);
extern char *GetTablespaceName(Oid spcid, bool skip_name);
extern char *GetUserName(Oid relowner, bool skip_name);

extern bool get_rel_owner_schema_tablespace(Oid relid, Oid *ownerOid, Oid *nsOid, Oid *tablespaceoid);
extern bool get_rel_name_namespace(Oid relid, Oid *nsOid, char *relname);

extern bool  is_valid_dbid(Oid dbid);
extern char *get_db_name(Oid dbid);

#endif
