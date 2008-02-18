/*
 * $Id: db.h 1698 2007-11-19 05:48:56Z rpedde $
 * Header info for generic database
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

#ifndef _DB_GENERIC_H_
#define _DB_GENERIC_H_

#include "ff-dbstruct.h" /** for MP3FILE */
#include "smart-parser.h" /** for PARSETREE */
#include "webserver.h" /** for WS_CONNINFO */

extern int db_open(char **pe, char *type, char *parameters);
extern int db_init(int reload);
extern int db_deinit(void);

extern int db_revision(void);

extern int db_add(char **pe, MEDIA_NATIVE *pmo);
extern int db_del(char **pe, uint32_t id);

/* enumerate db items (songs) */
extern int db_enum_start(char **pe, DB_QUERY *pquery);
/* this is either a distinct, a playlist, or a media string */
extern int db_enum_fetch(char **pe, char ***result, DB_QUERY *pquery);
extern int db_enum_reset(char **pe, DB_QUERY *pquery);
extern int db_enum_end(char **pe, DB_QUERY *pquery);

/* playlist functions */
extern int db_add_playlist(char **pe, char *name, int type, char *clause, char *path, int index, uint32_t *playlistid);
extern int db_add_playlist_item(char **pe, int playlistid, int songid);
extern int db_edit_playlist(char **pe, int id, char *name, char *clause);
extern int db_delete_playlist(char **pe, int playlistid);
extern int db_delete_playlist_item(char **pe, int playlistid, int songid);
extern PLAYLIST_NATIVE *db_fetch_playlist(char **pe, char *path, int index);
extern void db_dispose_playlist(PLAYLIST_NATIVE *ppln);

extern MEDIA_NATIVE *db_fetch_item(char **pe, int id);
extern MEDIA_NATIVE *db_fetch_path(char **pe, char *path, int index);

extern void db_hint(int hint);

/* Holdover functions from old db interface...
 * should these be removed?  Refactored?
 */
extern int db_playcount_increment(char **pe, int id);
extern int db_get_song_count(char **pe, int *count);
extern int db_get_playlist_count(char **pe, int *count);
extern void db_dispose_item(MEDIA_NATIVE *pmo);


/* FIXME: won't work with db as modules */
extern char *db_error_list[];

#define DB_E_SUCCESS                 0x00
#define DB_E_SQL_ERROR               0x01 /**< some kind of sql error - typically bad syntax */
#define DB_E_DUPLICATE_PLAYLIST      0x02 /**< playlist already exists when adding */
#define DB_E_NOCLAUSE                0x03 /**< adding smart playlist with no clause */
#define DB_E_INVALIDTYPE             0x04 /**< trying to add playlist items to invalid type */
#define DB_E_NOROWS                  0x05 /**< sql query returned no rows */
#define DB_E_INVALID_PLAYLIST        0x06 /**< bad playlist id */
#define DB_E_INVALID_SONGID          0x07 /**< bad song id */
#define DB_E_PARSE                   0x08 /**< could not parse result */
#define DB_E_BADPROVIDER             0x09 /**< requested db backend not there */
#define DB_E_PROC                    0x0A /**< could not start threadpool */
#define DB_E_SIZE                    0x0B /**< passed buffer too small */
#define DB_E_WRONGVERSION            0x0C /**< must upgrade db */
#define DB_E_DB_ERROR                0x0D /**< gdbm error */
#define DB_E_MALLOC                  0x0E /**< malloc error */
#define DB_E_NOTFOUND                0x0F /**< path not found */
#define DB_E_PTHREAD                 0x10 /**< pthreads error */
#define DB_E_PLAYLIST                0x11 /**< playlist error */
#define DB_E_NOPATH                  0x12 /**< cache_dir not specified */
#define DB_E_NOTIMPL                 0x13 /**< backend doesn't implement */

#endif /* _DB_GENERIC_H_ */
