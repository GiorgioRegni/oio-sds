/*
OpenIO SDS meta2
Copyright (C) 2014 Worldine, original work as part of Redcurrant
Copyright (C) 2015 OpenIO, modified as part of OpenIO Software Defined Storage

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 3.0 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library.
*/

#ifndef G_LOG_DOMAIN
# define G_LOG_DOMAIN "meta2.remote"
#endif

#include <metautils/lib/metautils.h>

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "meta2_remote.h"
#include "meta2_macros.h"

static gint
meta2_remote_container_common_fd_v2 (int *fd, gint ms, GError ** err, const char *op,
    struct hc_url_s *url, const char *stgpol)
{
	struct code_handler_s codes[] = {
		{CODE_FINAL_OK, REPSEQ_FINAL, NULL, NULL},
		{0, 0, NULL, NULL}
	};
	struct reply_sequence_data_s data = { NULL, 0, codes };

	EXTRA_ASSERT (fd != NULL);
	EXTRA_ASSERT (op != NULL);
	EXTRA_ASSERT (url != NULL);

	MESSAGE request = metautils_message_create_named(op);
	metautils_message_add_url (request, url);
	if (stgpol)
	    metautils_message_add_field_str(request, NAME_MSGKEY_STGPOLICY, stgpol);

	gboolean rc = metaXClient_reply_sequence_run(err, request, fd, ms, &data);
	metautils_message_destroy(request);
	if (!rc)
		GSETERROR(err, "An error occured while executing the request");
	return rc;
}

static gint
meta2_remote_container_common_v2 (const addr_info_t * m2, gint ms, GError ** err,
		const char *op, struct hc_url_s *url, const char *stgpol)
{
	int fd = addrinfo_connect(m2, ms, err);
	if (fd < 0) {
		GSETERROR(err, "Connection failed");
		return 0;
	}
	gint rc = meta2_remote_container_common_fd_v2(&fd, ms, err, op, url, stgpol);
	metautils_pclose(&fd);
	return rc;
}

/* ------------------------------------------------------------------------- */

gboolean
meta2_remote_container_create_v3 (const addr_info_t *m2, gint ms, GError **err,
		struct hc_url_s *url, const char *stgpol)
{
	return meta2_remote_container_common_v2 (m2, ms, err, NAME_MSGNAME_M2_CREATE, url, stgpol);
}

/* ------------------------------------------------------------------------- */

GSList*
meta2_remote_content_add_in_fd (int *fd, gint ms, GError **err,
		struct hc_url_s *url, content_length_t content_length,
		GByteArray *metadata, GByteArray **new_metadata)
{
	gboolean get_sys_metadata (GError **err0, gpointer udata, gint code, MESSAGE rep) {
		(void)udata, (void)code, (void)err0;
		if (!rep)
			return FALSE;
		if (!new_metadata)
			return TRUE;
		*new_metadata = NULL;
		gsize fieldLen=0;
		void *field = metautils_message_get_field (rep, NAME_MSGKEY_MDSYS, &fieldLen);
		if (field)
			*new_metadata = g_byte_array_append( g_byte_array_new(), field, fieldLen);
		return TRUE;
	}

	struct code_handler_s codes [] = {
		{ CODE_PARTIAL_CONTENT, REPSEQ_BODYMANDATORY, chunk_info_concat, NULL },
		{ CODE_FINAL_OK, REPSEQ_FINAL, chunk_info_concat, get_sys_metadata },
		{ 0,0,NULL,NULL}
	};
	GSList *result = NULL;
	struct reply_sequence_data_s data = { &result , 0 , codes };

	EXTRA_ASSERT (fd != NULL);
	EXTRA_ASSERT (url != NULL);
	MESSAGE request = metautils_message_create_named (NAME_MSGNAME_M2_CONTENTADD);
	metautils_message_add_url (request, url);
	metautils_message_add_field_strint64 (request, NAME_MSGKEY_CONTENTLENGTH, content_length);

	if (metadata)
		metautils_message_add_field (request, NAME_MSGKEY_MDSYS, metadata->data, metadata->len);
	if (!metaXClient_reply_sequence_run (err, request, fd, ms, &data)) {
		GSETERROR(err,"Cannot execute the query and receive all the responses");
		g_slist_free_full(result, (GDestroyNotify) chunk_info_clean);
		result = NULL;
	}

	metautils_message_destroy(request);
	return result;
}

/* ------------------------------------------------------------------------- */


GSList*
meta2_remote_content_spare_in_fd_full (int *fd, gint ms, GError **err,
		struct hc_url_s *url, gint count, gint distance,
		const gchar *notin, const gchar *broken)
{
	struct code_handler_s codes [] = {
		{ CODE_PARTIAL_CONTENT, REPSEQ_BODYMANDATORY, chunk_info_concat, NULL },
		{ CODE_FINAL_OK, REPSEQ_FINAL, chunk_info_concat, NULL },
		{ 0,0,NULL,NULL}
	};
	GSList *result = NULL;
	struct reply_sequence_data_s data = { &result , 0 , codes };

	EXTRA_ASSERT (fd != NULL);
	EXTRA_ASSERT (url != NULL);
	MESSAGE request = metautils_message_create_named (NAME_MSGNAME_M2_CONTENTSPARE);
	metautils_message_add_url (request, url);

	if (count > 0)
		metautils_message_add_field_strint64(request, NAME_MSGKEY_COUNT, count);
	if (distance > 0)
		metautils_message_add_field_strint64(request, NAME_MSGKEY_DISTANCE, distance);
	if (notin && *notin)
		metautils_message_add_field_str(request, NAME_MSGKEY_NOTIN, notin);
	if (broken && *broken)
		metautils_message_add_field_str(request, NAME_MSGKEY_BROKEN, broken);

	if (!metaXClient_reply_sequence_run (err, request, fd, ms, &data)) {
		GSETERROR(err,"Cannot execute the query and receive all the responses");
		g_slist_free_full (result, (GDestroyNotify) chunk_info_clean);
		result = NULL;
	}

	metautils_message_destroy(request);
	return result;
}

/* ------------------------------------------------------------------------- */

gboolean
meta2raw_remote_update_chunks (struct metacnx_ctx_s *ctx, GError **err,
		struct hc_url_s *url, struct meta2_raw_content_s *content,
		gboolean allow_update, char *position_prefix)
{
	struct code_handler_s codes [] = {
		{ CODE_FINAL_OK, REPSEQ_FINAL, NULL, NULL },
		{ 0,0,NULL,NULL}
	};
	struct reply_sequence_data_s data = { NULL , 0 , codes };

	EXTRA_ASSERT (ctx != NULL);
	EXTRA_ASSERT (url != NULL);
	EXTRA_ASSERT (content != NULL);
	MESSAGE request = metautils_message_create_named (NAME_MSGNAME_M2RAW_SETCHUNKS);
	metautils_message_add_url (request, url);
	metautils_message_add_body_unref (request, meta2_maintenance_marshall_content (content, NULL));
	if (allow_update)
		metautils_message_add_field_str(request, NAME_MSGKEY_ALLOWUPDATE, "1");
	if (position_prefix)
		metautils_message_add_field_str(request, NAME_MSGKEY_POSITIONPREFIX, position_prefix);

	gboolean rc = metaXClient_reply_sequence_run_context (err, ctx, request, &data);
	if (!rc)
		GSETERROR(err,"Cannot execute the query and receive all the responses");
	metautils_message_destroy(request);
	return rc;
}

gboolean
meta2raw_remote_update_content (struct metacnx_ctx_s *ctx, GError **err,
		struct hc_url_s *url, struct meta2_raw_content_s *content,
		gboolean allow_update)
{
	struct code_handler_s codes [] = {
		{ CODE_FINAL_OK, REPSEQ_FINAL, NULL, NULL },
		{ 0,0,NULL,NULL}
	};
	struct reply_sequence_data_s data = { NULL , 0 , codes };

	EXTRA_ASSERT (ctx != NULL);
	EXTRA_ASSERT (url != NULL);
	EXTRA_ASSERT (content != NULL);
	MESSAGE request = metautils_message_create_named (NAME_MSGNAME_M2RAW_SETCONTENT);
	metautils_message_add_body_unref (request, meta2_maintenance_marshall_content (content, err));
	if (allow_update)
		metautils_message_add_field_str (request, NAME_MSGKEY_ALLOWUPDATE, "1");

	gboolean rc = metaXClient_reply_sequence_run_context (err, ctx, request, &data);
	if (!rc)
		GSETERROR(err,"Cannot execute the query and receive all the responses");
	metautils_message_destroy(request);
	return rc;
}

gboolean
meta2raw_remote_delete_chunks(struct metacnx_ctx_s *ctx, GError **err,
		struct hc_url_s *url, struct meta2_raw_content_s *content)
{
	struct code_handler_s codes [] = {
		{ CODE_FINAL_OK, REPSEQ_FINAL, NULL, NULL },
		{ 0,0,NULL,NULL}
	};
	struct reply_sequence_data_s data = { NULL , 0 , codes };

	EXTRA_ASSERT (ctx != NULL);
	EXTRA_ASSERT (url != NULL);
	EXTRA_ASSERT (content != NULL);
	MESSAGE request = metautils_message_create_named (NAME_MSGNAME_M2RAW_DELCHUNKS);
	metautils_message_add_url (request, url);
	metautils_message_add_body_unref (request, meta2_maintenance_marshall_content (content, NULL));
	
	gboolean rc = metaXClient_reply_sequence_run_context (err, ctx, request, &data);
	if (!rc)
		GSETERROR(err,"Cannot execute the query and receive all the responses");
	metautils_message_destroy (request);
	return rc;
}

static gboolean
concat_contents (GError **err, gpointer udata, gint code, guint8 *body, gsize bodySize)
{
	struct meta2_raw_content_s **pContent=NULL, *decoded=NULL;

	(void)code;

	pContent = (struct meta2_raw_content_s**) udata;
	if (!pContent)
		return FALSE;
	/*unserialize the body*/
	decoded = meta2_maintenance_content_unmarshall_buffer(body, bodySize, err);
	if (!decoded)
		return FALSE;
	/*append the chunks*/
	if (!(*pContent))
		*pContent = decoded;
	else {
		if ((*pContent)->raw_chunks)
			(*pContent)->raw_chunks = metautils_gslist_precat( (*pContent)->raw_chunks, decoded->raw_chunks);
		else
			(*pContent)->raw_chunks = decoded->raw_chunks;
		decoded->raw_chunks = NULL;
		meta2_maintenance_destroy_content(decoded);
	}
	return TRUE;
}

static struct meta2_raw_content_s*
meta2raw_remote_stat_content(struct metacnx_ctx_s *ctx, GError **err,
		struct hc_url_s *url, gboolean check_flags)
{
	struct code_handler_s codes [] = {
		{ CODE_FINAL_OK, REPSEQ_FINAL|REPSEQ_BODYMANDATORY, concat_contents, NULL },
		{ CODE_PARTIAL_CONTENT, REPSEQ_BODYMANDATORY, concat_contents, NULL },
		{ 0,0,NULL,NULL}
	};
	struct meta2_raw_content_s *result=NULL;
	struct reply_sequence_data_s data = { &result , 0 , codes };

	EXTRA_ASSERT (ctx != NULL);
	EXTRA_ASSERT (url != NULL);
	MESSAGE request = metautils_message_create_named (NAME_MSGNAME_M2RAW_GETCHUNKS);
	metautils_message_add_url (request, url);
	if (check_flags)
		metautils_message_add_field_str (request, NAME_MSGKEY_CHECK, "1");
	gboolean rc = metaXClient_reply_sequence_run_context (err, ctx, request, &data);
	if (!rc)
		GSETERROR(err,"Cannot execute the query and receive all the responses");
	metautils_message_destroy(request);
	return result;
}

struct meta2_raw_content_s*
meta2_remote_stat_content(struct metacnx_ctx_s *cnx, GError **err,
		struct hc_url_s *url)
{
	return meta2raw_remote_stat_content(cnx, err, url, FALSE);
}

gboolean
meta2_remote_modify_metadatasys(struct metacnx_ctx_s *ctx, GError **err,
		struct hc_url_s *url, const gchar* var_2)
{
	struct code_handler_s codes [] = {
		{ CODE_FINAL_OK, REPSEQ_FINAL, NULL, NULL },
		{ 0,0,NULL,NULL}
	};
	struct reply_sequence_data_s data = { NULL , 0 , codes };

	EXTRA_ASSERT (ctx != NULL);
	EXTRA_ASSERT (url != NULL);
	EXTRA_ASSERT (var_2 != NULL);

	MESSAGE request = metautils_message_create_named(NAME_MSGNAME_M2RAW_SETMDSYS);
	metautils_message_add_url (request, url);
	metautils_message_add_field_str (request, NAME_MSGKEY_VALUE, var_2);
	gboolean rc = metaXClient_reply_sequence_run_context (err, ctx, request, &data);
	if (!rc)
		GSETERROR(err,"Cannot execute the query and receive all the responses");
	metautils_message_destroy(request);
	return rc;
}

