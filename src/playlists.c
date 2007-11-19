/*
 * $Id: $
 * implementation file for in-memory playlist implementation
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

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "daapd.h"
#include "db-generic.h"
#include "err.h"
#include "smart-parser.h"
#include "ff-dbstruct.h"
#include "ff-plugins.h"
#include "playlists.h"
#include "redblack.h"
#include "util.h"

#define PL_NO_DUPS 1

/** Typedefs */
typedef struct sortorder_t {
    int field;
    int direction;
    struct sororder_t *next;
} SORTORDER;

typedef struct playlist_t {
    PLAYLIST_NATIVE *ppln;
    PARSETREE pt;            /**< Only valid for smart playlists */
    struct playlist_t *next;
    struct rbtree *prb;
} PLAYLIST;

/** Globals */
static PLAYLIST pl_list = { NULL, NULL, NULL, NULL };
uint64_t pl_id = 2;
char *pl_error_list[] = {
    "Success",
    "Can't create smart playlist without query",
    "Must specify path and index for file-based playlists",
    "Must specify a name when creating a playlist",
    "Alloc error",
    "Query syntax error: %s",
    "Can't create playlist with duplicate name",
    "Database error: %s",
    "Can't find playlist id (%d)",
    "Can only add items to static playlists",
    "Invalid media item id (%d)",
    "Internal red/black tree error",
    "Operation on invalid playlist (id %d)",
};

static void pl_set_error(char **pe, int error, ...);
static void pl_purge(PLAYLIST *ppl);
static int pl_add_playlist_item_nolock(char **pe, uint32_t playlistid, uint32_t songid);

/**
 * push error text into the error buffer passed by the calling function
 * note that the error strings are sprintf-ified, so this takes sprintf-style
 * parameters
 *
 * @param pe caller supplied error buffer
 * @param error the underlying playlist error (index to pl_error_list)
 */
void pl_set_error(char **pe, int error, ...) {
    va_list ap;

    if(!pe)
        return;

    va_start(ap, error);
    *pe = util_vasprintf(pl_error_list[error],ap);
    va_end(ap);

    DPRINTF(E_SPAM,L_PL,"Raising error: %s\n",pe);
}

/**
 * Compare to MP3 items, returning (as is typical) < 0 if p1 < p2,
 * 0 if they are identical, or > 1 if p1 > p2, based on the sort
 * criteria specified
 *
 * @param p1 first mp3 file to compare
 * @param p2 second mp3 file to compare
 * @param ps criteria for sorting
 * @returns < 0 if p1 < p2, 0 if p1 == p2, and > 0 if p1 > p2
 */
int pl_compare(const void *v1, const void *v2, const void *vso) {
    SORTORDER *ps = (SORTORDER *)vso;
    int id1 = *((int*)v1);
    int id2 = *((int*)v2);

    MEDIA_NATIVE *p1 = (MEDIA_NATIVE *)v1;
    MEDIA_NATIVE *p2 = (MEDIA_NATIVE *)v2;

    int result = 1; // default - return in insert order

    /* FIXME: look these things up from a LRU cache based on tree depth */
    p1 = db_fetch_item(NULL, id1);
    if(!p1) {
        DPRINTF(E_LOG,L_PL,"DANGER: pl_compare: null item fetch\n");
        return 0;
    }

    p2 = db_fetch_item(NULL, id2);
    if(!p2) {
        DPRINTF(E_LOG,L_PL,"DANGER: pl_compare: null item fetch\n");
        db_dispose_item(p1);
        return 0;
    }

    if(!ps) { /* default sort order  -- by id */
        if(p1->id < p2->id) {
            result = -1;
        } else if(p1->id > p2->id) {
            result = 1;
        } else {
            result = 0;
        }
    }

    db_dispose_item(p1);
    db_dispose_item(p2);

    return result;
}


/**
 * populate/refresh a smart playlist by bulk scan on the database
 *
 * NOTE: this assumes the playlist lock is held.
 *
 * @param
 */
int pl_update_smart(char **pe, PLAYLIST *ppl) {
    DBQUERYINFO dbq;
    MEDIAOBJECT pmo;
    int err;
    int song_id;
    char *e_db;

    ASSERT((ppl) && (ppl->ppln) && (ppl->ppln->type == PL_SMART));

    if((!ppl) || (!ppl->ppln) || (ppl->ppln->type != PL_SMART))
        return DB_E_SUCCESS; /* ?? */

    pl_purge(ppl);

    /* if it's a smart playlist, should bulk update the playlist */
    /* FIXME: use the passed filter if the underlying db supports queries */
    memset((void*)&dbq,0,sizeof(dbq));

    dbq.query_type = queryTypeItems;
    dbq.index_type = indexTypeNone;
    dbq.playlist_id = 1;
    dbq.db_id = 1;

    if((err=db_enum_start(&e_db,&dbq)) != 0) {
        pl_set_error(pe,PL_E_DBERROR,e_db);
        if(e_db) free(e_db);
        db_enum_end(NULL);
        return PL_E_DBERROR;
    }

    /* now fetch each and see if it applies to this playlist */
    pmo.kind = OBJECT_TYPE_STRING;
    while((DB_E_SUCCESS == (err = db_enum_fetch_row(&e_db, pmo.pmstring, &dbq))) && (&pmo.pmstring)) {
        /* got one.. */
        if(pmo.kind == OBJECT_TYPE_STRING)
            song_id = atoi(pmo.pmstring->id);
        else
            song_id = pmo.pmnative->id;

        if(sp_matches(ppl->pt, &pmo)) {
            if(PL_E_SUCCESS != (err = pl_add_playlist_item_nolock(pe, ppl->ppln->id, song_id)))
                return err;
        }
    }

    db_enum_end(NULL);

    if(err != DB_E_SUCCESS) {
        pl_set_error(pe,PL_E_DBERROR,e_db);
        free(e_db);
        return PL_E_DBERROR;
    }

    return PL_E_SUCCESS;
}

/**
 * add a new playlist.
 *
 * @param buffer for error return.  NOTE:  Must be freed by caller!
 * @param name name of new playlist
 * @param type type of playlist
 * @param query query (if smart)
 * @param path path to playlist (if path based)
 * @param index path file index (if path based)
 * @param id playlist id (returned)
 * @returns PL_SUCCESS on success, error code on failure
 */
int pl_add_playlist(char **pe, char *name, int type, char *query, char *path, int index, uint32_t *id) {
    PLAYLIST *pcurrent;
    PLAYLIST *pnew;
    PLAYLIST_NATIVE *ppln;
    PARSETREE pt = NULL;
    char *e_db;

    ASSERT(name);
    ASSERT((type != PL_SMART) || (query))

    if((!name) || (!strlen(name))) {
        pl_set_error(pe, PL_E_NONAME);
        return PL_E_NONAME;
    }

    /* Check for stupidness */
    if((PL_SMART == type) && (!query)) {
        pl_set_error(pe, PL_E_NOCLAUSE);
        return PL_E_NOCLAUSE;
    }

    if(((PL_STATICFILE == type) || (PL_STATICXML == type)) && !path) {
        pl_set_error(pe, PL_E_NOPATH);
        return PL_E_NOPATH;
    }

    if(PL_SMART == type) {
        pt = sp_init();
        if(!pt) {
            pl_set_error(pe, PL_E_MALLOC);
            return PL_E_MALLOC;
        }

        if(!sp_parse(pt,query,SP_TYPE_PLAYLIST)) {
            pl_set_error(pe,PL_E_QUERY,sp_get_error(pt));

            DPRINTF(E_LOG,L_PL,"Error parsing playlist: %s\n",sp_get_error(pt));

            sp_dispose(pt);
            return PL_E_QUERY;
        }
    }

    util_mutex_lock(l_pl);

    /* maybe check for duplicates? */
#ifdef PL_NO_DUPS
    pcurrent = pl_list.next;
    while(pcurrent) {
        if(0 == strcasecmp(name, pcurrent->ppln->title)) {
            util_mutex_unlock(l_pl);
            if(pt) sp_dispose(pt);
            pl_set_error(pe,PL_E_NAMEDUP);
            return PL_E_NAMEDUP;
        }
        pcurrent = pcurrent->next;
    }
#endif
    /* FIXME: Check for duplicates on path/index for file based playlists */

    /* now, let's add it */
    pnew = (PLAYLIST *)malloc(sizeof(PLAYLIST));
    ppln = (M3UFILE *)malloc(sizeof(PLAYLIST_NATIVE));

    if((!pnew) || (!ppln)) {
        if(pnew) free(pnew);
        if(ppln) free(ppln);
        util_mutex_unlock(l_pl);
        if(pt) sp_dispose(pt);
        pl_set_error(pe,PL_E_MALLOC);
        return PL_E_MALLOC;
    }

    /* FIXME: Add the right comparison info for this playlist */
    pnew->prb = rbinit(pl_compare,NULL);
    if(!pnew->prb) {
        if(pnew) free(pnew);
        if(ppln) free(ppln);
        if(pt) sp_dispose(pt);
        util_mutex_unlock(l_pl);
        pl_set_error(pe,PL_E_RBTREE);
        return PL_E_RBTREE;
    }

    memset(ppln, 0, sizeof(PLAYLIST_NATIVE));

    ppln->title = strdup(name);
    ppln->type = type;

    if(PL_SMART == type) {
        ppln->query = strdup(query);
    }

    if((PL_STATICFILE == type) || (PL_STATICXML == type)) {
        ppln->path = path;
        ppln->index = index;
    }

    ppln->id = pl_id++;
    *id = ppln->id;
    ppln->items = 0;
    ppln->db_timestamp = (uint32_t) time(NULL);

    pnew->ppln = ppln;
    pnew->pt = pt;

    /* run to the bottom of the pl_list */
    pcurrent = &pl_list;
    while(pcurrent->next)
        pcurrent = pcurrent->next;

    if(PL_SMART == type) {
        if(!pl_update_smart(&e_db,pnew)) {
            pl_set_error(pe,PL_E_DBERROR,e_db);
            free(e_db);
            sp_dispose(pt);
            free(pnew);
            util_mutex_unlock(l_pl);
            return PL_E_DBERROR;
        }
    }

    pcurrent->next = pnew;
    pnew->next = NULL;

    util_mutex_unlock(l_pl);
    return PL_E_SUCCESS;
}

/**
 * delete all the entries from a playlist.  This
 * leaves the redblack tree, just purges the data
 * from it.  This assumes that the playlist lock is held
 *
 * @param ppl playlist to purge entries from
 */
void pl_purge(PLAYLIST *ppl) {
    const void *ptr;
    uint32_t id;
    uint32_t *pid;

    ASSERT(ppl && ppl->prb);

    if((!ppl) || (!ppl->prb))
        return;

    ptr = rblookup(RB_LUFIRST,NULL,ppl->prb);
    while(ptr) {
        id = *(uint32_t*)ptr;
        pid = (uint32_t*)rbdelete((void*)&id,ppl->prb);
        if(pid)
            free(pid);
        ptr = rblookup(RB_LUFIRST,NULL,ppl->prb);
    }

    ppl->ppln->items = 0;
}

/**
 * given a playlist id, find the playlist.  Assumes playlist
 * lock is held.
 *
 * @param id playlist id to find
 * @returns
 */
PLAYLIST *pl_find(uint32_t id) {
    PLAYLIST *plcurrent = pl_list.next;

    while(plcurrent) {
        if(!plcurrent->ppln)
            DPRINTF(E_FATAL,L_PL,"NULL native playlist.  Whoops!\n");
        if(plcurrent->ppln->id == id)
            return plcurrent;
        plcurrent = plcurrent->next;
    }

    return NULL;
}


int pl_add_playlist_item(char **pe, uint32_t playlistid, uint32_t songid) {
    int result;

    util_mutex_lock(l_pl);
    result = pl_add_playlist_item_nolock(pe, playlistid, songid);
    util_mutex_unlock(l_pl);

    return result;
}


/**
 * Add an item to a playlist.  Locks the playlist mutex
 *
 * @param pe error buffer
 * @param
 */
int pl_add_playlist_item_nolock(char **pe, uint32_t playlistid, uint32_t songid) {
    /* find the playlist */
    PLAYLIST *plcurrent;
    MEDIA_NATIVE *pmn;
    uint32_t *pid;
    const void *val;

    if(NULL == (plcurrent = pl_find(playlistid))) {
        pl_set_error(pe,PL_E_NOTFOUND,playlistid);
        return PL_E_NOTFOUND;
    }

    /* got it, now add the song id */
    if(plcurrent->ppln->type == PL_SMART) {
        pl_set_error(pe,PL_E_STATICONLY);
        return PL_E_STATICONLY;
    }

    /* make sure it's a valid song id */
    /* FIXME: replace this with a db_exists type function */
    pmn = db_fetch_item(NULL,songid);
    if(!pmn) {
        pl_set_error(pe,PL_E_BADSONGID,songid);
        return PL_E_BADSONGID;
    }

    db_dispose_item(pmn);

    /* okay, it's valid, so let's add it */
    if(!plcurrent->prb)
        DPRINTF(E_FATAL,L_PL,"redblack tree not present in playlist\n");

    pid = (uint32_t *)malloc(sizeof(uint32_t));
    if(!pid) {
        pl_set_error(pe,PL_E_MALLOC);
        return PL_E_MALLOC;
    }

    val = rbsearch((const void*)pid, plcurrent->prb);
    if(!val)
        DPRINTF(E_FATAL,L_SCAN,"redblack tree insert error\n");

    return PL_E_SUCCESS;
}


/**
 * Edit an existing playlist
 *
 * @param pe error string buffer
 * @param name new name of playlist
 * @param query new playlist query (PL_SMART only)
 * @returns PL_E_SUCCESS on success, error code otherwise.
 */
int pl_edit_playlist(char **pe, uint32_t id, char *name, char *query) {
    PLAYLIST *ppl;
    PARSETREE ppt_new;
    int ecode;

    if((name == NULL) && (query == NULL))
        return PL_E_SUCCESS;

    /* can't edit playlist 1 (default library) */
    if(id == 1) {
        pl_set_error(pe, PL_E_BADPLID);
        return PL_E_BADPLID;
    }

    util_mutex_lock(l_pl);

    /* find the playlist by id */
    ppl = pl_find(id);
    if(!ppl) {
        pl_set_error(pe, PL_E_NOTFOUND, id);
        util_mutex_unlock(l_pl);
        return PL_E_NOTFOUND;
    }

    if((ppl->ppln->type == PL_SMART) && (query)) {
        /* we are updating the query... */
        ppt_new = sp_init();
        if(!ppt_new) {
            pl_set_error(pe, PL_E_MALLOC);
            util_mutex_unlock(l_pl);
            return PL_E_MALLOC;
        }

        if(!sp_parse(ppt_new, query, SP_TYPE_PLAYLIST)) {
            pl_set_error(pe, PL_E_QUERY,sp_get_error(ppt_new));
            DPRINTF(E_LOG,L_PL,"Error parsing playlist: %s\n",sp_get_error(ppt_new));
            sp_dispose(ppt_new);
            util_mutex_unlock(l_pl);
            return PL_E_QUERY;
        }

        sp_dispose(ppl->pt);
        ppl->pt = ppt_new;

        /* now, repopulate the playlist */
        if(PL_E_SUCCESS != (ecode = pl_update_smart(pe, ppl))) {
            util_mutex_unlock(l_pl);
            return ecode;
        }
    }

    /* now update name if necessary */
    if(name) {
        if(ppl->ppln->title) free(ppl->ppln->title);
        ppl->ppln->title = strdup(name);
    }

    util_mutex_unlock(l_pl);
    return PL_E_SUCCESS;
}

/**
 * delete an existing playlist
 *
 * @param pe error string buffer
 * @param playlistid id of the playlist to delete
 *
 */
int pl_delete_playlist(char **pe, uint32_t playlistid) {
    PLAYLIST *ppl, *ppl_prev;

    util_mutex_lock(l_pl);

    ppl_prev = &pl_list;
    ppl = ppl_prev->next;

    while(ppl) {
        if(!ppl->ppln)
            DPRINTF(E_FATAL,L_PL,"no ppln in ppl\n");

        if(ppl->ppln->id == playlistid)
            break;

        ppl_prev = ppl;
        ppl = ppl->next;
    }

    if(!ppl) {
        util_mutex_unlock(l_pl);
        pl_set_error(pe,PL_E_NOTFOUND,playlistid);
        return PL_E_NOTFOUND;
    }

    pl_purge(ppl);

    if(ppl->ppln) {
        if(ppl->ppln->title) free(ppl->ppln->title);
        if(ppl->ppln->query) free(ppl->ppln->query);
        if(ppl->ppln->path) free(ppl->ppln->path);
    }

    free(ppl->ppln);
    if(ppl->pt) sp_dispose(ppl->pt);
    if(ppl->prb) rbdestroy(ppl->prb);

    ppl_prev->next = ppl->next;
    free(ppl);

    util_mutex_unlock(l_pl);
    return PL_E_SUCCESS;
}

/**
 * delete a playlist item
 *
 * @param pe error string buffer
 * @param playlistid playlist to delete item from
 * @param songid item to delete from the playlist
 * @returns PL_E_SUCCESS on success, error code & pe filled on failure
 */
int pl_delete_playlist_item(char **pe, uint32_t playlistid, uint32_t songid) {
    uint32_t *pid;
    PLAYLIST *ppl;

    ASSERT(playlistid != 1);

    if(playlistid == 1) {
        pl_set_error(pe,PL_E_BADPLID,playlistid);
        return PL_E_BADPLID;
    }

    util_mutex_lock(l_pl);

    /* find the playlist by id */
    ppl = pl_find(playlistid);
    if(!ppl) {
        util_mutex_unlock(l_pl);
        pl_set_error(pe, PL_E_NOTFOUND, playlistid);
        return PL_E_NOTFOUND;
    }

    pid = (uint32_t*)rbdelete((void*)&songid,ppl->prb);
    if(!pid) {
        util_mutex_unlock(l_pl);
        pl_set_error(pe,PL_E_BADSONGID,songid);
        return PL_E_BADSONGID;
    }

    ppl->ppln->items--;

    util_mutex_unlock(l_pl);
    return PL_E_SUCCESS;
}

/**
 * get a count of the number of playlists.  This is obviously
 * informational, and subject to races.  This should really be
 * backed by a db version
 *
 * @param pe error string buffer
 * @param count returns the number of playlists
 * @returns PL_E_SUCCESS with count filled on success, error code otherwise
 */
int pl_get_playlist_count(char **pe, int *count) {
    PLAYLIST *ppl = pl_list.next;
    int result = 0;

    util_mutex_lock(l_pl);

    while(ppl) {
        result++;
        ppl = ppl->next;
    }

    util_mutex_unlock(l_pl);

    *count = result;
    return PL_E_SUCCESS;
}

/**
 * return a playlist (in native format)
 *
 * @param pe error buffer
 * @param path path of static plalist
 * @param index index of static playlist (like itunes plid)
 * @returns native playlist, or null if not found
 */
PLAYLIST_NATIVE *pl_fetch_playlist(char **pe, char *path, uint32_t index) {
    PLAYLIST *ppl = pl_list.next;
    PLAYLIST_NATIVE *pnew;

    ASSERT(path);

    if(!path)
        return NULL;

    util_mutex_lock(l_pl);

    while(ppl) {
        if(!ppl->ppln)
            DPRINTF(E_FATAL,L_PL,"pl_fetch_playlist: no native playlist\n");

        if(ppl->ppln->path && (0 == strcasecmp(ppl->ppln->path,path))) {
            if(ppl->ppln->index == index) {
                pnew = (PLAYLIST_NATIVE*)malloc(sizeof(PLAYLIST_NATIVE));
                if(!pnew)
                    DPRINTF(E_FATAL,L_PL,"pl_fetch_playlist: malloc\n");

                memcpy(pnew,ppl->ppln,sizeof(PLAYLIST_NATIVE));
                if(ppl->ppln->title) pnew->title = strdup(ppl->ppln->title);
                if(ppl->ppln->query) pnew->query = strdup(ppl->ppln->query);
                if(ppl->ppln->path) pnew->path = strdup(ppl->ppln->path);

                util_mutex_unlock(l_pl);
                return pnew;
            }
        }
        ppl = ppl->next;
    }

    util_mutex_unlock(l_pl);
    return NULL;
}

/**
 * dispose of a native playlist created by pl_fetch
 *
 * @param ppln native playlist returned from pl_fetch_playlist
 */
void pl_dispose_playlist(PLAYLIST_NATIVE *ppln) {
    if(ppln->title) free(ppln->title);
    if(ppln->query) free(ppln->query);
    if(ppln->path) free(ppln->path);

    free(ppln);
}
