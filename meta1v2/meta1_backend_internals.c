/*
OpenIO SDS meta1v2
Copyright (C) 2014 Worldine, original work as part of Redcurrant
Copyright (C) 2015 OpenIO, modified as part of OpenIO Software Defined Storage

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef G_LOG_DOMAIN
# define G_LOG_DOMAIN "grid.meta1"
#endif

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>

#include <sqlite3.h>

#include <metautils/lib/metautils.h>
#include <metautils/lib/metacomm.h>
#include <sqliterepo/sqliterepo.h>

#include "./internals.h"
#include "./internals_sqlite.h"
#include "./meta1_prefixes.h"
#include "./meta1_backend.h"
#include "./meta1_backend_internals.h"

int meta1_backend_log_level = 0;

static int
m1_to_sqlx(enum m1v2_open_type_e t)
{
	switch (t & 0x03) {
		case M1V2_OPENBASE_LOCAL:
			return SQLX_OPEN_LOCAL;
		case M1V2_OPENBASE_MASTERONLY:
			return SQLX_OPEN_MASTERONLY;
		case M1V2_OPENBASE_SLAVEONLY:
			return SQLX_OPEN_SLAVEONLY;
		case M1V2_OPENBASE_MASTERSLAVE:
			return SQLX_OPEN_MASTERSLAVE;
	}

	g_assert_not_reached();
	return SQLX_OPEN_LOCAL;
}

GError*
_open_and_lock(struct meta1_backend_s *m1, struct hc_url_s *url,
		enum m1v2_open_type_e how, struct sqlx_sqlite3_s **handle)
{
	EXTRA_ASSERT(m1 != NULL);
	EXTRA_ASSERT(url != NULL);
	EXTRA_ASSERT(handle != NULL);

	GRID_TRACE2("%s(%p,%p,%d,%p)", __FUNCTION__, (void*)m1,
			hc_url_get (url, HCURL_HEXID), how, (void*)handle);

	if (!hc_url_has (url, HCURL_HEXID))
		return NEWERROR (CODE_BAD_REQUEST, "Partial URL (missing HEXID)");
	if (!m1b_check_ns_url (m1, url))
		return NEWERROR(CODE_NAMESPACE_NOTMANAGED, "Invalid NS");

	gchar base[5];
	const guint8 *cid = hc_url_get_id(url);
	g_snprintf(base, sizeof(base), "%02X%02X", cid[0], cid[1]);

	if (!meta1_prefixes_is_managed(m1->prefixes, cid))
		return NEWERROR(CODE_RANGE_NOTFOUND, "prefix [%s] not managed", base);

	/* Now open/lock the base in a way suitable for our op */
	struct sqlx_name_s n = {.base=base, .type=NAME_SRVTYPE_META1, .ns=m1->backend.ns_name};
	GError *err = sqlx_repository_open_and_lock(m1->backend.repo, &n, m1_to_sqlx(how), handle, NULL);

	if (err != NULL) {
		if (!CODE_IS_REDIRECT(err->code))
			g_prefix_error(&err, "Open/Lock error: ");  
		return err;
	}

	EXTRA_ASSERT(*handle != NULL);
	GRID_TRACE("Opened and locked [%s][%s] -> [%s][%s]",
			base, NAME_SRVTYPE_META1,
			(*handle)->name.base, (*handle)->name.type);
	return NULL;
}

GError*
__create_user(struct sqlx_sqlite3_s *sq3, struct hc_url_s *url)
{
	if (!hc_url_has_fq_container (url))
		return NEWERROR(CODE_BAD_REQUEST, "Partial URL");

	static const gchar *sql = "INSERT INTO users "
		"('cid','account','user') VALUES (?,?,?)";

	GError *err = NULL;
	sqlite3_stmt *stmt = NULL;
	struct sqlx_repctx_s *repctx = NULL;
	int rc;

	EXTRA_ASSERT(sq3 != NULL);
	EXTRA_ASSERT(sq3->db != NULL);

	err = sqlx_transaction_begin(sq3, &repctx);
	if (NULL != err)
		return err;

	/* Prepare the statement */
	sqlite3_prepare_debug(rc, sq3->db, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		err = M1_SQLITE_GERROR(sq3->db, rc);
	else {
		(void) sqlite3_bind_blob(stmt, 1, hc_url_get_id(url), hc_url_get_id_size(url), NULL);
		(void) sqlite3_bind_text(stmt, 2, hc_url_get(url, HCURL_ACCOUNT), -1, NULL);
		(void) sqlite3_bind_text(stmt, 3, hc_url_get(url, HCURL_USER), -1, NULL);

		/* Run the results */
		do { rc = sqlite3_step(stmt); } while (rc == SQLITE_ROW);

		if (rc != SQLITE_OK && rc != SQLITE_DONE) {
			err = M1_SQLITE_GERROR(sq3->db, rc);
			if (rc == SQLITE_CONSTRAINT) {
				g_prefix_error(&err, "Already created? ");
				err->code = CODE_CONTAINER_EXISTS;
			}
		}

		sqlite3_finalize_debug(rc, stmt);
	}

	if (err)
		GRID_DEBUG("User creation failed : (%d) %s", err->code, err->message);

	return sqlx_transaction_end(repctx, err);
}

GError*
__info_user(struct sqlx_sqlite3_s *sq3, struct hc_url_s *url, gboolean ac,
		struct hc_url_s ***result)
{
	GError *err = NULL;
	sqlite3_stmt *stmt = NULL;
	GPtrArray *gpa;
	int rc;
	gboolean found;

	EXTRA_ASSERT(sq3 != NULL);
	EXTRA_ASSERT(sq3->db != NULL);
	EXTRA_ASSERT(url != NULL);

retry:
	/* Prepare the statement */
	sqlite3_prepare_debug(rc, sq3->db, "SELECT account,user FROM users WHERE cid = ?", -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return M1_SQLITE_GERROR(sq3->db, rc);
	(void) sqlite3_bind_blob(stmt, 1, hc_url_get_id (url), hc_url_get_id_size (url), NULL);

	/* Run the results */
	found = FALSE;
 	gpa = result ? g_ptr_array_new() : NULL;
	do { if (SQLITE_ROW == (rc = sqlite3_step(stmt))) {
		found = TRUE;
		if (!gpa) continue;
		struct hc_url_s *u = hc_url_empty ();
		hc_url_set (u, HCURL_NS, hc_url_get (url, HCURL_NS));
		hc_url_set (u, HCURL_ACCOUNT, (char*)sqlite3_column_text(stmt, 0));
		hc_url_set (u, HCURL_USER, (char*)sqlite3_column_text(stmt, 1));
		hc_url_set (u, HCURL_HEXID, hc_url_get (url, HCURL_HEXID));
		g_ptr_array_add(gpa, u);
	} } while (rc == SQLITE_ROW);

	if (rc != SQLITE_DONE && rc != SQLITE_OK) {
		err = M1_SQLITE_GERROR(sq3->db, rc);
		g_prefix_error(&err, "DB error: ");
	}

	sqlite3_finalize_debug(rc,stmt);
	stmt = NULL;

	if (err) {
		if (gpa) {
			g_ptr_array_set_free_func (gpa, (GDestroyNotify)hc_url_clean);
			g_ptr_array_free (gpa, TRUE);
		}
		return err;
	}

	if (!found) {
		if (gpa) g_ptr_array_free (gpa, TRUE);
		if (ac) {
			ac = FALSE; /* do not retry */
			err = __create_user (sq3, url);
			if (!err) goto retry;
		}
		return NEWERROR(CODE_USER_NOTFOUND, "no such container");
	}
	if (gpa)
		*result = (struct hc_url_s**) metautils_gpa_to_array(gpa, TRUE);
	return NULL;
}

void
gpa_str_free(GPtrArray *gpa)
{
	if (!gpa)
		return;
	g_ptr_array_set_free_func (gpa, g_free);
	g_ptr_array_free(gpa, TRUE);
}

gboolean
m1b_check_ns (struct meta1_backend_s *m1, const char *ns)
{
	if (!m1 || !ns)
		return FALSE;
	return 0 == strcmp (m1->backend.ns_name, ns);
}

gboolean
m1b_check_ns_url (struct meta1_backend_s *m1, struct hc_url_s *url)
{
	if (!url)
		return FALSE;
	return m1b_check_ns (m1, hc_url_get (url, HCURL_NS));
}

