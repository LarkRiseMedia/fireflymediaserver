/*
 * $Id$
 * sqlite2-specific db implementation
 *
 * Copyright (C) 2005 Ron Pedde (ron@pedde.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _DB_SQL_SQLITE3_
#define _DB_SQL_SQLITE3_

/* db funcs */
extern int db_sqlite3_open(char **pe, char *dsn);
extern int db_sqlite3_close(void);

/* add a media object */
extern int db_sqlite3_add(char **pe, MEDIA_NATIVE *pmo);
extern int db_sqlite3_del(char **pe, uint32_t id);

/* walk through a table */
extern int db_sqlite3_enum_items_begin(char **pe, void **opaque);
extern int db_sqlite3_enum_items_fetch(char **pe, void *opaque, MEDIA_STRING **ppmo);
extern int db_sqlite3_enum_end(char **pe, void *opaque);
extern int db_sqlite3_enum_restart(char **pe, void *opaque);
extern int db_sqlite3_fetch_item(char **pe, uint32_t id, void **opaque, MEDIA_STRING **ppms);
extern void db_sqlite3_dispose_item(void *opaque, MEDIA_STRING *ppms);
extern void db_sqlite3_hint(int hint);


#endif /* _DB_SQL_SQLITE3_ */

