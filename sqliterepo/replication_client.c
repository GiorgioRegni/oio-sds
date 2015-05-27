/*
OpenIO SDS sqliterepo
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
# define G_LOG_DOMAIN "sqliterepo"
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <glib.h>

#include <metautils/lib/metautils.h>
#include <metautils/lib/metacomm.h>

#include "sqliterepo.h"
#include "sqlx_remote.h"
#include "internals.h"

struct TableSequence;

static GByteArray*
_pack_RESTORE(struct sqlx_name_s *name, GByteArray *dump)
{
	GByteArray *encoded = sqlx_pack_RESTORE(name, dump->data, dump->len);
	g_byte_array_unref(dump);
	return encoded;
}

GError *
peer_restore(const gchar *target, struct sqlx_name_s *name,
		GByteArray *dump)
{
	GError *err = NULL;

	if (!target) {
		g_byte_array_unref(dump);
		return NULL;
	}

	GByteArray *encoded = _pack_RESTORE(name, dump);
	struct gridd_client_s *client = gridd_client_create(target, encoded, NULL, NULL);
	g_byte_array_unref(encoded);

	if (!client)
		return NEWERROR(CODE_INTERNAL_ERROR, "Failed to create client to [%s], bad address?", target);

	gridd_client_set_timeout(client, 5.0, 30.0);
	gridd_client_start(client);
	if (!(err = gridd_client_loop(client)))
		err = gridd_client_error(client);
	gridd_client_free(client);
	return err;
}

void
peers_restore(gchar **targets, struct sqlx_name_s *name,
		GByteArray *dump)
{
	GError *err = NULL;

	if (!targets || !targets[0]) {
		g_byte_array_unref(dump);
		return ;
	}

	GByteArray *encoded = _pack_RESTORE(name, dump);
	struct gridd_client_s **clients = gridd_client_create_many(targets, encoded, NULL, NULL);
	g_byte_array_unref(encoded);
	gridd_clients_set_timeout(clients, 5.0, 30.0);

	gridd_clients_start(clients);
	if (!(err = gridd_clients_loop(clients)))
		err = gridd_clients_error(clients);
	gridd_clients_free(clients);

	if (err) {
		GRID_WARN("RESTORE failed [%s][%s] : (%d) %s", name->base, name->type,
				err->code, err->message);
		g_clear_error(&err);
	}
}

GError *
peer_dump(const gchar *target, struct sqlx_name_s *name, gboolean chunked,
		peer_dump_cb callback, gpointer cb_arg)
{
	struct gridd_client_s *client;
	GByteArray *encoded;
	GError *err = NULL;

	gboolean on_reply(gpointer ctx, MESSAGE reply) {
		GError *err2 = NULL;
		gsize bsize = 0;
		gint64 remaining = -1;
		(void) ctx;

		err2 = message_extract_strint64(reply, "remaining", &remaining);
		g_clear_error(&err2);

		void *b = message_get_BODY(reply, &bsize);
		if (!b) {
			GByteArray *dump = g_byte_array_new();
			g_byte_array_append(dump, b, bsize);
			err2 = callback(dump, remaining, cb_arg);
		}
		if (err2 != NULL) {
			GRID_ERROR("Failed to use result of dump: (%d) %s",
					err2->code, err2->message);
			g_clear_error(&err2);
			return FALSE;
		}
		return TRUE;
	}

	GRID_TRACE2("%s(%s,%p,%d,%p,%p)", __FUNCTION__, target, name, chunked,
			callback, cb_arg);

	if (!target)
		return NEWERROR(CODE_INTERNAL_ERROR, "No target URL");

	encoded = sqlx_pack_DUMP(name, chunked);
	client = gridd_client_create(target, encoded, NULL, on_reply);
	g_byte_array_unref(encoded);

	if (!client)
		return NEWERROR(CODE_INTERNAL_ERROR, "Failed to create client to [%s], bad address?", target);

	// set a long timeout to allow moving large meta2 bases
	gridd_client_set_timeout(client, 3600.0, 4000.0);
	gridd_client_start(client);
	if (!(err = gridd_client_loop(client))) {
		err = gridd_client_error(client);
	}

	gridd_client_free(client);

	return err;
}

GError *
peer_dump_gba(const gchar *target, struct sqlx_name_s *name, GByteArray **result)
{
	GError *peer_dump_gba_cb(GByteArray *chunk, gint64 remaining, gpointer arg)
	{
		(void) remaining;
		(void) arg;
		*result = chunk;
		return NULL;
	}

	return peer_dump(target, name, FALSE, peer_dump_gba_cb, NULL);
}

