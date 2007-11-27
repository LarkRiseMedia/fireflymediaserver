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

#ifndef _DB_SQL_SQLITE2_
#define _DB_SQL_SQLITE2_

/* db funcs */
extern int db_sqlite2_open(char **pe, char *dsn);
extern int db_sqlite2_close(void);

/* add a media object */
extern int db_sqlite2_add(char **pe, MEDIA_NATIVE *pmo);

/* walk through a table */
extern int db_sqlite2_enum_begin(char **pe);
extern int db_sqlite2_enum_fetch(char **pe, MEDIA_STRING **ppmo);
extern int db_sqlite2_enum_end(char **pe);
extern int db_sqlite2_enum_restart(char **pe);

#endif /* _DB_SQL_SQLITE2_ */

