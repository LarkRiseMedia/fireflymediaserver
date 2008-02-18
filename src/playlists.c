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

#include "includes.h"

#include "conf.h"
#include "daapd.h"
#include "db.h"
#include "err.h"
#include "smart-parser.h"
#include "ff-dbstruct.h"
#include "ff-plugins.h"
#include "os.h"
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

struct plenumhandle_t {
    PLAYLIST *ppl;
    int nextop;  /* unused? */
    RBLIST *rblist;
    void *last_value;
};

#define MAYBEFREE(a) if((a)) free((a))

/** Globals */
static PLAYLIST pl_list = { NULL, NULL, NULL, NULL };
uint64_t pl_id = 1;     // First playlist to be created will be the library
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
    "Invalid parameter passed",
};

static PLAYLIST *pl_find(uint32_t id);
static void pl_save(PLAYLIST *ppl);
static void pl_set_error(char **pe, int error, ...);
static void pl_purge(PLAYLIST *ppl);
static int pl_add_playlist_item_nolock(char **pe, PLAYLIST *ppl, uint32_t songid);
static int pl_contains_item(uint32_t pl_id, uint32_t song_id);
static int pl_compare(const void *v1, const void *v2, const void *vso);

/* here's a nice hack... */
MEDIA_NATIVE *db_fetch_item_nolock(char **pe, int id);

/**
 * load a playlist.
 *
 * FIXME: should leverage add_playlist
 */
int pl_load(char *filename) {
    PLAYLIST_NATIVE *ppln;
    PLAYLIST *ppl;
    PLAYLIST *pcurrent;
    HANDLE handle;
    unsigned char *line;
    char *sep;
    uint32_t id;
    int added = 1;

    DPRINTF(E_INF,L_PL,"Loading playlist %s\n",filename);
    handle = io_new();
    if(!handle) DPRINTF(E_FATAL,L_PL,"malloc error in pl_load\n");

    if(!io_open(handle,"file://%U?mode=r&ascii=1", filename)) {
        DPRINTF(E_LOG,L_PL,"Can't open %s: %s\n",filename,io_errstr(handle));
        io_dispose(handle);
        return FALSE;
    }

    ppln = (PLAYLIST_NATIVE *)malloc(sizeof(PLAYLIST_NATIVE));
    ppl = (PLAYLIST*)malloc(sizeof(PLAYLIST));

    if((!ppln) || (!ppl)) DPRINTF(E_FATAL,L_PL,"malloc error in pl_load\n");

    memset(ppln,0,sizeof(PLAYLIST_NATIVE));
    memset(ppl,0,sizeof(PLAYLIST));

    ppl->prb = rbinit(pl_compare,NULL);
    ppl->ppln = ppln;

    // load up the file...
    while(io_allocline(handle, &line) && line) {
        DPRINTF(E_DBG,L_PL,"Loaded line: %s",line);

        while((line[strlen((char*)line)-1] == '\n') || (line[strlen((char *)line)-1] == '\r'))
            line[strlen((char*)line)-1] = '\0';

        if(NULL != (sep = strchr((char*)line,':'))) {
            *sep++ = '\0';
            if(strcasecmp((char*)line,"id") == 0) {
                ppl->ppln->id = util_atoui32(sep);
            } else if(strcasecmp((char*)line,"name") == 0) {
                ppl->ppln->title = strdup(sep);
            } else if(strcasecmp((char*)line,"type") == 0) {
                ppl->ppln->type = util_atoui32(sep);
            } else if(strcasecmp((char*)line,"query") == 0) {
                ppl->ppln->query = strdup(sep);
            } else if(strcasecmp((char*)line,"timestamp") == 0) {
                ppl->ppln->db_timestamp = util_atoui32(sep);
            } else if(strcasecmp((char*)line,"path") == 0) {
                ppl->ppln->path = strdup(sep);
            } else if(strcasecmp((char*)line,"idx") == 0) {
                ppl->ppln->idx = util_atoui32(sep);
            }
        } else {
            if((id = util_atoui32((char*)line))) {
                // must be an id?
                pl_add_playlist_item_nolock(NULL, ppl, id);
            }
        }

        free(line);
    }

    if(ppln->type & PL_DYNAMIC) {
        ppl->pt = sp_init();
        if(ppl->pt) {
            if(!sp_parse(ppl->pt,ppl->ppln->query,SP_TYPE_PLAYLIST)) {
                DPRINTF(E_LOG,L_MISC,"Bad query in %s: %s\n",ppl->ppln->title,sp_get_error(ppl->pt));
                sp_dispose(ppl->pt);
                added = 0;
            }
        } else {
            DPRINTF(E_LOG,L_MISC,"Couldn't init parse tree\n");
            added=0;
        }
    }

    if(added) {
        pcurrent = &pl_list;
        while(pcurrent->next)
            pcurrent = pcurrent->next;

        pcurrent->next = ppl;
        ppl->next = NULL;
    } else {
        /* free up the struct */
        if(ppln) {
            if(ppln->title) free(ppln->title);
            if(ppln->query) free(ppln->query);
            if(ppln->path) free(ppln->path);
            free(ppln);
        }

        if(ppl) {
            /* walk the tree... */
            rbdestroy(ppl->prb);
            free(ppl);
        }
    }

    io_dispose(handle);
    return TRUE;
}


void pl_save(PLAYLIST *ppl) {
    PLAYLIST_NATIVE *ppln = ppl->ppln;
    char *ppath;
    char *pcache_dir;
    IOHANDLE handle;
    RBLIST *rblist;
    uint32_t *pid;
    uint32_t song_id;

    if(ppln->id == 1) // don't bother saving the library playlist
        return;

    if(ppln->type & PL_HIDDEN)
        return;

    pcache_dir = conf_alloc_string("general","cache_dir",NULL);
    if(pcache_dir) {
        ppath = util_asprintf("%s/playlists/%s.playlist",pcache_dir,ppln->title);
        free(pcache_dir);

        handle = io_new();
        if(handle) {
            if(io_open(handle,"file://%U?ascii=1&mode=w",ppath)) {
                io_buffer(handle);
                io_printf(handle,"# Auto-saved playlist - timestamp:  %d\n\n",time(NULL));
                io_printf(handle,"id:%u\n",ppln->id);
                io_printf(handle,"name:%s\n",ppln->title);
                io_printf(handle,"type:%d\n",ppln->type);
                if(ppln->query)
                    io_printf(handle,"query:%s\n",ppln->query);
                io_printf(handle,"timestamp:%d\n",ppln->db_timestamp);
                if(ppln->path)
                    io_printf(handle,"path:%s\n",ppln->path);
                io_printf(handle,"idx:%d\n",ppln->idx);

                io_printf(handle,"\n# Item list\n\n");

                rblist = rbopenlist(ppl->prb);
                if(rblist) {
                    while(NULL != (pid = (uint32_t*)rbreadlist(rblist))) {
                        song_id = *pid;
                        io_printf(handle,"%u\n",song_id);
                    }
                    rbcloselist(rblist);
                }
            } else {
                DPRINTF(E_LOG,L_PL,"Can't write playlist: %s\n",ppath);
            }
            io_dispose(handle);
        }
            free(ppath);
    }
}


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
    MEDIA_NATIVE *p1;
    MEDIA_NATIVE *p2;
    int result;

    if(!ps) { /* unspecified sort order */
        /* return in id order */
        if(id1 < id2)
            return -1;
        if(id1 > id2)
            return 1;
        return 0;
    }

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
    MEDIA_NATIVE *pmn;
    int err;
    uint32_t song_id;
    uint32_t *pid;
    char *e_db;
    PLAYLIST *plibrary;
    RBLIST *rblist;

    ASSERT((ppl) && (ppl->ppln) && (ppl->ppln->type & PL_DYNAMIC));

    if((!ppl) || (!ppl->ppln) || (!(ppl->ppln->type & PL_DYNAMIC)))
        return DB_E_SUCCESS; /* ?? */

    pl_purge(ppl);

    plibrary = pl_find(1);
    if(!plibrary)
        return PL_E_SUCCESS;

    rblist = rbopenlist(plibrary->prb);
    if(!rblist)
        return PL_E_SUCCESS;

    while(NULL != (pid = (uint32_t*)rbreadlist(rblist))) {
        song_id = *pid;
        pmn = db_fetch_item_nolock(&e_db, song_id);
        if(!pmn) {
            rbcloselist(rblist);
            pl_set_error(pe,PL_E_DBERROR,e_db);
            free(e_db);
            return PL_E_DBERROR;
        }

        if(sp_matches_native(ppl->pt, pmn)) {
            if(PL_E_SUCCESS != (err = pl_add_playlist_item(pe, ppl->ppln->id, song_id))) {
                DPRINTF(E_DBG,L_PL,"can't add item to playlist\n");
                rbcloselist(rblist);
                return err;
            }
        }
    }

    rbcloselist(rblist);

    DPRINTF(E_DBG,L_PL,"Updated smart playlist: items: %d\n",ppl->ppln->items);
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
    ASSERT((type & PL_DYNAMIC) || (query));

    DPRINTF(E_DBG,L_PL,"Adding playlist %s\n",name);

    if((!name) || (!strlen(name))) {
        pl_set_error(pe, PL_E_NONAME);
        return PL_E_NONAME;
    }

    /* Check for stupidness */
    if((type & PL_DYNAMIC) && (!query)) {
        pl_set_error(pe, PL_E_NOCLAUSE);
        return PL_E_NOCLAUSE;
    }

    if(((PL_STATICFILE == type) || (PL_STATICXML == type)) && (!path)) {
        pl_set_error(pe, PL_E_NOPATH);
        return PL_E_NOPATH;
    }

    if(type & PL_DYNAMIC) {
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

    //util_mutex_lock(l_pl);

    /* maybe check for duplicates? */
#ifdef PL_NO_DUPS
    pcurrent = pl_list.next;
    while(pcurrent) {
        if(0 == strcasecmp(name, pcurrent->ppln->title)) {
            //util_mutex_unlock(l_pl);
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
        //util_mutex_unlock(l_pl);
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
        //util_mutex_unlock(l_pl);
        pl_set_error(pe,PL_E_RBTREE);
        return PL_E_RBTREE;
    }

    memset(ppln, 0, sizeof(PLAYLIST_NATIVE));

    ppln->title = strdup(name);
    ppln->type = type;

    if(type & PL_DYNAMIC) {
        ppln->query = strdup(query);
    }

    if((PL_STATICFILE == type) || (PL_STATICXML == type)) {
        ppln->path = path;
        ppln->idx = index;
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

    pcurrent->next = pnew;
    pnew->next = NULL;

    if(type & PL_DYNAMIC) {
        DPRINTF(E_DBG,L_PL,"Updating smart playlist\n");
        if(PL_E_SUCCESS != pl_update_smart(&e_db,pnew)) {
            pl_set_error(pe,PL_E_DBERROR,e_db);
            free(e_db);
            sp_dispose(pt);
            pcurrent->next = NULL;
            free(pnew);
            //util_mutex_unlock(l_pl);
            return PL_E_DBERROR;
        }
    }

    DPRINTF(E_DBG,L_PL,"Added playlist as %d\n",ppln->id);

    pl_save(pnew);

    //util_mutex_unlock(l_pl);
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

    DPRINTF(E_DBG,L_PL,"Purging playlist %s\n",ppl->ppln->title);

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
    PLAYLIST *ppl;

    DPRINTF(E_DBG,L_PL,"Adding item %d to playlist %d\n",songid, playlistid);

    //util_mutex_lock(l_pl);
    ppl = pl_find(playlistid);
    if(NULL == ppl) {
        DPRINTF(E_DBG,L_PL,"Can't find playlist in add_item\n");
        pl_set_error(pe,PL_E_NOTFOUND,playlistid);
        return PL_E_NOTFOUND;
    }

    result = pl_add_playlist_item_nolock(pe, ppl, songid);
    //util_mutex_unlock(l_pl);

    return result;
}


/**
 * Add an item to a playlist.  Locks the playlist mutex
 *
 * @param pe error buffer
 * @param
 */
int pl_add_playlist_item_nolock(char **pe, PLAYLIST *ppl, uint32_t songid) {
    /* find the playlist */
    MEDIA_NATIVE *pmn;
    uint32_t *pid;
    const void *val;

    /* make sure it's a valid song id */
    /* FIXME: replace this with a db_exists type function */
    pmn = db_fetch_item_nolock(NULL,songid);
    if(!pmn) {
        DPRINTF(E_DBG,L_PL,"Can't find song in add_item\n");
        pl_set_error(pe,PL_E_BADSONGID,songid);
        return PL_E_BADSONGID;
    }

    db_dispose_item(pmn);

    /* okay, it's valid, so let's add it */
    if(!ppl->prb)
        DPRINTF(E_FATAL,L_PL,"redblack tree not present in playlist\n");

    pid = (uint32_t *)malloc(sizeof(uint32_t));
    if(!pid) {
        pl_set_error(pe,PL_E_MALLOC);
        return PL_E_MALLOC;
    }

    *pid = songid;

    val = rbsearch((const void*)pid, ppl->prb);
    if(!val) {
        DPRINTF(E_FATAL,L_SCAN,"redblack tree insert error\n");
        pl_set_error(pe,PL_E_RBTREE);
        return PL_E_RBTREE;
    }

    ppl->ppln->items++;
    DPRINTF(E_DBG,L_PL,"New playlist size: %d\n",ppl->ppln->items);

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

    //util_mutex_lock(l_pl);

    /* find the playlist by id */
    ppl = pl_find(id);
    if(!ppl) {
        pl_set_error(pe, PL_E_NOTFOUND, id);
        //util_mutex_unlock(l_pl);
        return PL_E_NOTFOUND;
    }

    if((ppl->ppln->type & PL_DYNAMIC) && (query)) {
        /* we are updating the query... */
        ppt_new = sp_init();
        if(!ppt_new) {
            pl_set_error(pe, PL_E_MALLOC);
            //util_mutex_unlock(l_pl);
            return PL_E_MALLOC;
        }

        if(!sp_parse(ppt_new, query, SP_TYPE_PLAYLIST)) {
            pl_set_error(pe, PL_E_QUERY,sp_get_error(ppt_new));
            DPRINTF(E_LOG,L_PL,"Error parsing playlist: %s\n",sp_get_error(ppt_new));
            sp_dispose(ppt_new);
            //util_mutex_unlock(l_pl);
            return PL_E_QUERY;
        }

        sp_dispose(ppl->pt);
        ppl->pt = ppt_new;

        /* now, repopulate the playlist */
        if(PL_E_SUCCESS != (ecode = pl_update_smart(pe, ppl))) {
            //util_mutex_unlock(l_pl);
            return ecode;
        }
    }

    /* now update name if necessary */
    if(name) {
        if(ppl->ppln->title) free(ppl->ppln->title);
        ppl->ppln->title = strdup(name);
    }

    //util_mutex_unlock(l_pl);
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

    //util_mutex_lock(l_pl);

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
        //util_mutex_unlock(l_pl);
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

    //util_mutex_unlock(l_pl);
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

    /* find the playlist by id */
    ppl = pl_find(playlistid);
    if(!ppl) {
        pl_set_error(pe, PL_E_NOTFOUND, playlistid);
        return PL_E_NOTFOUND;
    }

    pid = (uint32_t*)rbdelete((void*)&songid,ppl->prb);
    if(!pid) {
        pl_set_error(pe,PL_E_BADSONGID,songid);
        return PL_E_BADSONGID;
    }

    ppl->ppln->items--;

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

    //util_mutex_lock(l_pl);

    while(ppl) {
        /* dont' count hidden playlists */
        if((ppl->ppln) && (!(ppl->ppln->type & PL_HIDDEN)))
            result++;
        ppl = ppl->next;
    }

    //util_mutex_unlock(l_pl);

    *count = result;
    return PL_E_SUCCESS;
}


/**
 * return a playlist (in native format) for a particular playlist id
 *
 * @param pe error buffer
 * @param id id of playlist to fetch
 */
PLAYLIST_NATIVE *pl_fetch_playlist_id(char **pe, uint32_t id) {
    PLAYLIST *ppl;
    PLAYLIST_NATIVE *pnew;

    //util_mutex_lock(l_pl);
    if(NULL == (ppl = pl_find(id))) {
        //util_mutex_unlock(l_pl);
        pl_set_error(pe,PL_E_NOTFOUND,id);
        return NULL;
    }

    pnew = (PLAYLIST_NATIVE*)malloc(sizeof(PLAYLIST_NATIVE));
    if(!pnew) {
        DPRINTF(E_FATAL,L_PL,"pl_fetch_playlist: malloc\n");
        pl_set_error(pe, PL_E_MALLOC);
        //util_mutex_unlock(l_pl);
        return NULL;
    }

    memcpy(pnew,ppl->ppln,sizeof(PLAYLIST_NATIVE));
    if(ppl->ppln->title) pnew->title = strdup(ppl->ppln->title);
    if(ppl->ppln->query) pnew->query = strdup(ppl->ppln->query);
    if(ppl->ppln->path) pnew->path = strdup(ppl->ppln->path);

    //util_mutex_unlock(l_pl);
    return pnew;
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

    //util_mutex_lock(l_pl);

    while(ppl) {
        if(!ppl->ppln)
            DPRINTF(E_FATAL,L_PL,"pl_fetch_playlist: no native playlist\n");

        if(ppl->ppln->path && (0 == strcasecmp(ppl->ppln->path,path))) {
            if(ppl->ppln->idx == index) {
                pnew = (PLAYLIST_NATIVE*)malloc(sizeof(PLAYLIST_NATIVE));
                if(!pnew) {
                    DPRINTF(E_FATAL,L_PL,"pl_fetch_playlist: malloc\n");
                    //util_mutex_unlock(l_pl);
                    pl_set_error(pe, PL_E_MALLOC);
                    return NULL;
                }

                memcpy(pnew,ppl->ppln,sizeof(PLAYLIST_NATIVE));
                if(ppl->ppln->title) pnew->title = strdup(ppl->ppln->title);
                if(ppl->ppln->query) pnew->query = strdup(ppl->ppln->query);
                if(ppl->ppln->path) pnew->path = strdup(ppl->ppln->path);

                //util_mutex_unlock(l_pl);
                return pnew;
            }
        }
        ppl = ppl->next;
    }

    //util_mutex_unlock(l_pl);
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

/**
 * walk a playlist.  This assumes that a readlock
 * is held
 */
PLENUMHANDLE pl_enum_items_start(char **pe, uint32_t playlist_id) {
    PLENUMHANDLE pleh;
    PLAYLIST *ppl;

    ppl = pl_find(playlist_id);
    if(!ppl) {
        DPRINTF(E_DBG,L_PL,"Can't find plid %d\n",playlist_id);
        pl_set_error(pe,PL_E_NOTFOUND,playlist_id);
        return NULL;
    }

    pleh = (PLENUMHANDLE)malloc(sizeof(struct plenumhandle_t));
    if(!pleh) {
        pl_set_error(pe,PL_E_MALLOC);
        return NULL;
    }

    pleh->ppl = ppl;
    pleh->rblist = rbopenlist(ppl->prb);

    if(!pleh->rblist) {
        pl_set_error(pe,PL_E_RBTREE);
        return NULL;
    }

    return pleh;
}

int pl_enum_items_reset(char **pe, PLENUMHANDLE pleh) {
    ASSERT(pleh);

    if(!pleh)
        return PL_E_INVALID;

    rbcloselist(pleh->rblist);
    pleh->rblist = rbopenlist(pleh->ppl->prb);
    if(!pleh->rblist) {
        pl_set_error(pe,PL_E_RBTREE);
        return PL_E_RBTREE;
    }

    return PL_E_SUCCESS;
}

uint32_t pl_enum_items_fetch(char **pe, PLENUMHANDLE pleh) {
    uint32_t *ptr;

    ASSERT(pleh);
    if(!pleh) {
        pl_set_error(pe,PL_E_INVALID);
        return 0;
    }

    ptr = (uint32_t *)rbreadlist(pleh->rblist);
    pleh->last_value = ptr;

    if(!ptr)
        return 0;

    return *(uint32_t*)ptr;
}

void pl_enum_items_end(PLENUMHANDLE pleh) {
    ASSERT(pleh);

    rbcloselist(pleh->rblist);
    if(pleh)
        free(pleh);
}


/**
 * enumerate playlists
 *
 * @param pe error buffer
 * @returns enumeration handle, or null on error
 */
PLENUMHANDLE pl_enum_start(char **pe) {
    PLENUMHANDLE pleh;

    DPRINTF(E_DBG,L_PL,"Enumerating playlists\n");
    /* readlock must be set by calling function */

    pleh = (PLENUMHANDLE)malloc(sizeof(struct plenumhandle_t));
    memset(pleh,0,sizeof(struct plenumhandle_t));

    pleh->ppl = &pl_list;
    pleh->last_value = (PLAYLIST_STRING*)malloc(sizeof(PLAYLIST_STRING));

    if(!pleh->last_value) {
        free(pleh);
        return NULL;
    }

    memset(pleh->last_value,0,sizeof(PLAYLIST_STRING));
    return pleh;
}

/**
 * reset enumeration
 *
 * @param pe error buffer
 * @param pleh enumeration handle, as retrived by enum_start
 * @returns PL_E_SUCCESS on success
 */
int pl_enum_reset(char **pe, PLENUMHANDLE pleh) {
    ASSERT(pleh);

    if(!pleh)
        return PL_E_SUCCESS;

    pleh->ppl = &pl_list;
    return PL_E_SUCCESS;
}

/**
 * fetch the next playlist
 *
 * @param pe error buffer
 * @param pleh enumeration handle, as retrieved by enum_start
 * @returns DB_E_SUCCESS on success, error string
 */
int pl_enum_fetch(char **pe, char ***result, PLENUMHANDLE pleh) {
    PLAYLIST_STRING *ppls;

    DPRINTF(E_DBG,L_PL,"Fetching next playlist\n");

    if(!pleh)
        return PL_E_SUCCESS;

    ppls = (PLAYLIST_STRING*) pleh->last_value;

    if(ppls) {
        MAYBEFREE(ppls->id);
        MAYBEFREE(ppls->title);
        MAYBEFREE(ppls->type);
        MAYBEFREE(ppls->items);
        MAYBEFREE(ppls->query);
        MAYBEFREE(ppls->db_timestamp);
        MAYBEFREE(ppls->path);
        MAYBEFREE(ppls->idx);
    }

    /* find the next non-hidden playlist */
    while(1) {
        pleh->ppl = pleh->ppl->next;
        if(!pleh->ppl) {
            *result = NULL;
            memset(ppls,0,sizeof(PLAYLIST_STRING));
            return PL_E_SUCCESS;
        }
        if(pleh->ppl->ppln && (pleh->ppl->ppln->type & PL_HIDDEN)) {
            pleh->ppl = pleh->ppl->next;
        } else {
            break;
        }
    }

    /* yuck */
    ppls->id = util_asprintf("%d",pleh->ppl->ppln->id);
    ppls->type = util_asprintf("%d",pleh->ppl->ppln->type);
    ppls->items = util_asprintf("%d",pleh->ppl->ppln->items);
    ppls->db_timestamp = util_asprintf("%d",pleh->ppl->ppln->db_timestamp);
    ppls->idx = util_asprintf("%d",pleh->ppl->ppln->idx);

    if(pleh->ppl->ppln->title)
        ppls->title = strdup(pleh->ppl->ppln->title);
    if(pleh->ppl->ppln->query)
        ppls->query = strdup(pleh->ppl->ppln->query);
    if(pleh->ppl->ppln->path)
        ppls->path = strdup(pleh->ppl->ppln->path);

    *result = (char**)ppls;

    return PL_E_SUCCESS;
}

/**
 * finish enumerating plalists
 *
 * @param pleh enumeration handle, from enum_start
 */
void pl_enum_end(PLENUMHANDLE pleh) {
    PLAYLIST_STRING *ppls;

    if(!pleh)
        return;

    ppls = (PLAYLIST_STRING*) pleh->last_value;

    if(ppls) {
        MAYBEFREE(ppls->id);
        MAYBEFREE(ppls->title);
        MAYBEFREE(ppls->type);
        MAYBEFREE(ppls->items);
        MAYBEFREE(ppls->query);
        MAYBEFREE(ppls->db_timestamp);
        MAYBEFREE(ppls->path);
        MAYBEFREE(ppls->idx);

        free(ppls);
    }

    free(pleh);
}


/**
 * see if an item is in a specific playlist
 *
 * @param pl_id playlist to check
 * @param song_id id of song to check for
 * @return TRUE or FALSE
 */
int pl_contains_item(uint32_t pl_id, uint32_t song_id) {
    PLAYLIST *ppl;

    if(!(ppl = pl_find(pl_id)))
        return FALSE;

    if(rbfind((void*)&song_id,ppl->prb))
        return TRUE;

    return FALSE;
}

/**
 * when the database gets asked to add a new media object,
 * it lets us know that it did, that way we can update smart
 * playlists, the in-memory db cache, and whatnot
 *
 * @param pmn new entry added (or updated)
 */
void pl_advise_add(MEDIA_NATIVE *pmn) {
    PLAYLIST *ppl;
    int is_edit = FALSE;

    /* this can be an add or an update... if it's an add, it won't already
     * be in playlist 1
     */

    if(pl_contains_item(1, pmn->id)) {
        is_edit = TRUE;
    }

    ppl = pl_list.next;

    /* walk through all the playlists, adding them if necessary */
    while(ppl) {
        if((ppl->ppln->id) && (!is_edit))
            pl_add_playlist_item(NULL, 1, pmn->id);
        else if((!is_edit) && (ppl->ppln->type & PL_DYNAMIC) && (sp_matches_native(ppl->pt, pmn)))
            pl_add_playlist_item(NULL, ppl->ppln->id, pmn->id);
        else if((is_edit) && (ppl->ppln->type & PL_DYNAMIC) && (!sp_matches_native(ppl->pt, pmn)))
            pl_delete_playlist_item(NULL, ppl->ppln->id, pmn->id);

        ppl = ppl->next;
    }
}

/**
 * warning from the db driver that an item has been deleted from the
 * database
 */
void pl_advise_del(uint32_t id) {
    PLAYLIST *ppl;

    ppl = pl_list.next;

    while(ppl) {
        if(pl_contains_item(ppl->ppln->id, id))
            pl_delete_playlist_item(NULL, ppl->ppln->id, id);
        ppl = ppl->next;
    }
}


/**
 * set up the playlists
 *
 * @returns DB_E_SUCCESS on success, failure code & pe allocated otherwise
 */
int pl_init(char **pe) {
    DIR *playlist_dir;
    char de[sizeof(struct dirent) + MAXNAMLEN + 1];
    struct dirent *pde;
    char *path;
    char *relative_path = NULL;
    char *cache_dir;
    int err;
    struct stat sb;

    /* db is open, Library playlist is set up... load persistent playlists */
    cache_dir =  conf_alloc_string("general","cache_dir",NULL);
    if(!cache_dir) {
        DPRINTF(E_LOG,L_PL,"Can't find cache_dir.  Not loading playlists\n");
        return DB_E_SUCCESS;
    }

    path = util_asprintf("%s/playlists", cache_dir);
    free(cache_dir);
    if(!path) DPRINTF(E_FATAL,L_PL,"Malloc error in pl_init\n");

    if(NULL == (playlist_dir = opendir(path))) {
        free(path);
        DPRINTF(E_LOG,L_SCAN,"opendir (%s): %s\n",path,strerror(errno));
        return DB_E_SUCCESS;
    }

    /* find the playlists */
    while(1) {
        pde = (struct dirent *)&de;
        err = readdir_r(playlist_dir,(struct dirent*)&de, &pde);
        if(err == -1) {
            /* stop scanning, but return success to continue startup */
            DPRINTF(E_LOG,L_PL,"Error scanning playlists: %s\n",strerror(errno));
            closedir(playlist_dir);
            free(path);
            return DB_E_SUCCESS;
        }

        if(!pde)
            break;

        if(!strcmp(pde->d_name,".") || (!strcmp(pde->d_name,"..")))
            continue;

        relative_path = util_asprintf("%s/%s",path,pde->d_name);

        if(!os_lstat(relative_path, &sb)) {
            if(S_ISDIR(sb.st_mode))
                continue;
        }

        /* do stuff with playlist */
        DPRINTF(E_DBG,L_PL,"Found playlist %s\n",relative_path);
        pl_load(relative_path);

        free(relative_path);
    }

    free(path);
    return DB_E_SUCCESS;
}

int pl_deinit(char **pe) {
    return DB_E_SUCCESS;
}


