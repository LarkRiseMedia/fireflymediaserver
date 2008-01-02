/*
 * $Id: $
 * header file for in-memory playlist implementation
 *
 * Copyright (C) 2006 Ron Pedde (rpedde@users.sourceforge.net)
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

#ifndef _PLAYLISTS_H_
#define _PLAYLISTS_H_

#include "ff-dbstruct.h"

/** Error codes */
#define PL_E_SUCCESS     0
#define PL_E_NOCLAUSE    1
#define PL_E_NOPATH      2
#define PL_E_NONAME      3
#define PL_E_MALLOC      4
#define PL_E_QUERY       5
#define PL_E_NAMEDUP     6
#define PL_E_DBERROR     7
#define PL_E_NOTFOUND    8
#define PL_E_STATICONLY  9
#define PL_E_BADSONGID   10
#define PL_E_RBTREE      11
#define PL_E_BADPLID     12
#define PL_E_INVALID     13

typedef struct plenumhandle_t *PLENUMHANDLE;

extern int pl_init(char **pe);
extern int pl_deinit(char **pe);

extern int pl_add_playlist(char **pe, char *name, int type, char *query, char *path, int index, uint32_t *id);
extern int pl_add_playlist_item(char **pe, uint32_t playlistid, uint32_t songid);
extern int pl_edit_playlist(char **pe, uint32_t id, char *name, char *query);
extern int pl_delete_playlist(char **pe, uint32_t playlistid);
extern int pl_delete_playlist_item(char **pe, uint32_t playlistid, uint32_t songid);
extern int pl_get_playlist_count(char **pe, int *count);

extern PLAYLIST_NATIVE *pl_fetch_playlist(char **pe, char *path, uint32_t index);
extern PLAYLIST_NATIVE *pl_fetch_playlist_id(char **pe, uint32_t id);
extern void pl_dispose_playlist(PLAYLIST_NATIVE *ppln);

extern PLENUMHANDLE pl_enum_items_start(char **pe, uint32_t playlist_id);
extern int pl_enum_items_reset(char **pe, PLENUMHANDLE pleh);
extern uint32_t pl_enum_items_fetch(char **pe, PLENUMHANDLE pleh);
extern void pl_enum_items_end(PLENUMHANDLE pleh);

extern PLENUMHANDLE pl_enum_start(char **pe);
extern int pl_enum_reset(char **pe, PLENUMHANDLE pleh);
extern int pl_enum_fetch(char **pe, char ***result, PLENUMHANDLE pleh);
extern void pl_enum_end(PLENUMHANDLE pleh);

/* Advise functions, specific to the db functions */
extern void pl_advise_add(MEDIA_NATIVE *pmn);
extern void pl_advise_del(uint32_t id);

#endif /* _PLAYLISTS_H_ */

