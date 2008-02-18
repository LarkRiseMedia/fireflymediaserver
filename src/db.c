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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "daapd.h"
#include "db.h"
#include "err.h"

// FIXME: modularize the db handlers
#include "db-sql-sqlite2.h"
#include "db-sql-sqlite3.h"
#include "playlists.h"
#include "util.h"
#include "redblack.h"

#ifdef DEBUG
#  ifndef ASSERT
#    define ASSERT(f)         \
        if(f)                 \
            {}                \
        else                  \
            io_err_printf(0,"Assert error in %s, line %d\n",__FILE__,__LINE__)
#  endif /* ndef ASSERT */
#else /* ndef DEBUG */
#  ifndef ASSERT
#    define ASSERT(f)
#  endif
#endif

/* Typedefs */
typedef struct tag_plugin_db_fn {
    int(*db_open)(char **, char *);
    int(*db_close)(void);
    uint32_t(*db_flags)(void);

    // proper db functions
    int(*db_add)(char **, MEDIA_NATIVE *);
    int(*db_del)(char **, uint32_t);
    int(*db_enum_start)(char **, void **);
    int(*db_enum_fetch)(char **, void *, MEDIA_STRING **);
    int(*db_enum_reset)(char **, void *);
    int(*db_enum_end)(char**, void *);

    void(*db_hint)(int);

    int (*db_fetch_item)(char **, uint32_t, void **, MEDIA_STRING **);
    void (*db_dispose_item)(void *, MEDIA_STRING *);
} PLUGIN_DB_FN;

typedef struct enum_helper_t {
    PLENUMHANDLE handle;
    char **result;
    void *opaque;
    char *composite_query;
    uint32_t old_playlist;

    /* index/limits */
    int current_position;

    /* distinct */
    struct rbtree *pdistinct;
    char *last_value;
    int nextop;
    int(*enum_start)(char **, DB_QUERY *);
    int(*enum_fetch)(char **, char ***, DB_QUERY *);
} ENUMHELPER;

typedef struct db_cache_entry_t {
    MEDIA_NATIVE *pmn;
    int refcount;
    struct db_cache_entry_t *next;
    struct db_cache_entry_t *prev;
} DB_CACHE_ENTRY;

typedef struct db_cache_list_t {
    int max_length;
    int current_length;
    DB_CACHE_ENTRY cache_list;
    DB_CACHE_ENTRY *tail;
} DB_CACHE_LIST;

typedef struct db_path_node_t {
    char *path;
    int fetched;
    uint32_t index;
    uint32_t id;
} DB_PATH_NODE;

#define MAYBEFREE(a) if((a)) free((a));

/* Globals */
static int db_revision_no=2;                          /**< current revision of the db */
static pthread_once_t db_initlock=PTHREAD_ONCE_INIT;  /**< to initialize the rwlock */
static pthread_rwlock_t db_rwlock;                    /**< pthread r/w sync for the database */
static PLUGIN_DB_FN *db_pfn = NULL;                   /**< link to db plugin funcs */
static DB_CACHE_LIST db_cache_list = { 32, 0, { NULL, 0, NULL, NULL }, NULL};
static struct rbtree *db_path_lookup;

/* This could arguably go somewhere else, but we'll put it here  */
#define OFFSET_OF(__type, __field)      ((size_t) (&((__type*) 0)->__field))
#define FF_FIELD_ENTRY(__name,__dmap,__type) { #__name, __dmap, __type, OFFSET_OF(MEDIA_NATIVE,__name) }

FIELD_LOOKUP ff_field_data[] = {
    FF_FIELD_ENTRY(id,"dmap.itemid",FT_INT32),                         // 0
    FF_FIELD_ENTRY(path,NULL,FT_STRING),
    FF_FIELD_ENTRY(fname,NULL,FT_STRING),
    FF_FIELD_ENTRY(title,"dmap.itemname",FT_STRING),
    FF_FIELD_ENTRY(artist,"dmap.songartist",FT_STRING),
    FF_FIELD_ENTRY(album,"dmap.songalbum",FT_STRING),                  // 5
    FF_FIELD_ENTRY(genre,"dmap.songgenre",FT_STRING),
    FF_FIELD_ENTRY(comment,"dmap.songcomment",FT_STRING),
    FF_FIELD_ENTRY(type,"daap.songformat",FT_STRING), /* wav, etc */
    FF_FIELD_ENTRY(composer,"daap.songcomposer",FT_STRING),
    FF_FIELD_ENTRY(orchestra,NULL,FT_STRING),                          // 10
    FF_FIELD_ENTRY(conductor,NULL,FT_STRING),
    FF_FIELD_ENTRY(grouping,NULL,FT_STRING),
    FF_FIELD_ENTRY(url,"daap.songdataurl",FT_STRING),
    FF_FIELD_ENTRY(bitrate,"daap.songbitrate",FT_INT32),
    FF_FIELD_ENTRY(samplerate,"daap.songsamplerate",FT_INT32),         // 15
    FF_FIELD_ENTRY(song_length,"daap.songtime",FT_INT32),
    FF_FIELD_ENTRY(file_size,"daap.songsize",FT_INT64),
    FF_FIELD_ENTRY(year,"daap.songyear",FT_INT32),
    FF_FIELD_ENTRY(track,"daap.songtracknumber",FT_INT32),
    FF_FIELD_ENTRY(total_tracks,"daap.songtrackcount",FT_INT32),       // 20
    FF_FIELD_ENTRY(disc,"daap.songdiscnumber",FT_INT32),
    FF_FIELD_ENTRY(total_discs,"daap.songdisccount",FT_INT32),
    FF_FIELD_ENTRY(bpm,NULL,FT_INT32),
    FF_FIELD_ENTRY(compilation,"daap.songcompilation",FT_INT32),
    FF_FIELD_ENTRY(rating,NULL,FT_INT32),                              // 25
    FF_FIELD_ENTRY(play_count,NULL,FT_INT32),
    FF_FIELD_ENTRY(data_kind,"daap.songdatakind",FT_INT32),
    FF_FIELD_ENTRY(item_kind,NULL,FT_INT32),
    FF_FIELD_ENTRY(description,"daap.songdescription",FT_STRING),
    FF_FIELD_ENTRY(time_added,"daap.songdateadded",FT_INT32),          // 30
    FF_FIELD_ENTRY(time_modified,"daap.songdatemodified",FT_INT32),
    FF_FIELD_ENTRY(time_played,NULL,FT_INT32),
    FF_FIELD_ENTRY(disabled,NULL,FT_INT32),
    FF_FIELD_ENTRY(sample_count,NULL,FT_INT64),
    FF_FIELD_ENTRY(codectype,NULL,FT_STRING),                          // 35
    FF_FIELD_ENTRY(idx,NULL,FT_INT32),
    FF_FIELD_ENTRY(has_video,NULL,FT_INT32),
    FF_FIELD_ENTRY(contentrating,NULL,FT_INT32),
    FF_FIELD_ENTRY(bits_per_sample,NULL,FT_INT32),
    FF_FIELD_ENTRY(album_artist,NULL,FT_STRING),                       // 40
};

/* FIXME: static */
char *db_error_list[] = {
    "Success",
    "Misc SQL Error: %s",
    "Duplicate Playlist: %s",
    "Missing playlist spec",
    "Cannot add playlist items to a playlist of that type",
    "No rows returned",
    "Invalid playlist id: %d",
    "Invalid song id: %d",
    "Parse error: %s",
    "No backend database support for type: %s",
    "Could not initialize thread pool",
    "Passed buffer too small for result",
    "Wrong db schema.  Use mtd-update to upgrade the db.",
    "Database error: %s",
    "Malloc error",
    "Path not found",
    "Internal pthreads error",
    "Playlist error: %s",
    "general/cache_dir not specified",
    "Backend db does not implement that function",
};

/* Forwards */
static void db_writelock(void);
static void db_readlock(void);
static int db_unlock(void);
static void db_init_once(void);
static void db_utf8_validate(MP3FILE *pmp3);
static int db_utf8_validate_string(char *string);
static void db_trim_strings(MP3FILE *pmp3);
static void db_trim_string(char *string);
static MEDIA_NATIVE *db_string_to_native(MEDIA_STRING *pmos);
static MEDIA_STRING *db_native_to_string(MEDIA_NATIVE *pmon);
static void db_set_error(char **pe, int err, ...);

static char *db_util_strdup(char *string);

static int db_enum_items_start(char **pe, DB_QUERY *pinfo);
static int db_enum_playlist_start(char **pe, DB_QUERY *pinfo);
static int db_enum_browse_start(char **pe, DB_QUERY *pinfo);

static int db_enum_items_fetch(char **pe, char ***result, DB_QUERY *pquery);
static int db_enum_playlist_fetch(char **pe, char ***result, DB_QUERY *pquery);
static int db_enum_browse_fetch(char **pe, char ***result, DB_QUERY *pquery);

/* db cache layer */
static MEDIA_NATIVE *db_cache_fetch(char **pe, uint32_t id);
static void db_cache_update(MEDIA_NATIVE *pmn);
static void db_cache_promote(DB_CACHE_ENTRY *pentry);
static void db_cache_insert(MEDIA_NATIVE *pmn);
static DB_CACHE_ENTRY *db_cache_find(int id);
static void db_cache_dispose_item(MEDIA_NATIVE *pmo);


/* path-to-id mapping */
static int db_path_compare(const void *p1, const void *p2, const void *arg);

/* lock-free functions */
MEDIA_NATIVE *db_fetch_item_nolock(char **pe, int id);
int db_del_nolock(char **pe, uint32_t id);

#define DB_STR_COPY(field) pnew->field=db_util_strdup(pmos->field)
#define DB_INT32_COPY(field) pnew->field=util_atoui32(pmos->field)
#define DB_INT64_COPY(field) pnew->field=util_atoui64(pmos->field)


void db_cache_dump(void) {
    DB_CACHE_ENTRY *pentry;

    return;

    DPRINTF(E_DBG,L_DB,"db_cache_list.cache_list: %08x\n",&db_cache_list.cache_list);
    DPRINTF(E_DBG,L_DB,"db_cache_list.cache_list.next: %08x\n",db_cache_list.cache_list.next);
    DPRINTF(E_DBG,L_DB,"db_cache_list.tail: %08x\n",db_cache_list.tail);
    DPRINTF(E_DBG,L_DB,"\n");

    pentry = db_cache_list.cache_list.next;
    while(pentry) {
        DPRINTF(E_DBG,L_DB,"ID: %d: %08x\n",pentry->pmn->id,pentry);
        DPRINTF(E_DBG,L_DB,"  prev: %08x\n",pentry->prev);
        DPRINTF(E_DBG,L_DB,"  next: %08x\n",pentry->next);
        DPRINTF(E_DBG,L_DB,"\n");
        pentry=pentry->next;
    }
}

/*
 * caching functions.
 */
DB_CACHE_ENTRY *db_cache_find(int id) {
    DB_CACHE_ENTRY *pentry;

    //    DPRINTF(E_DBG,L_DB,"Finding %d in cache\n",id);
    pentry = db_cache_list.cache_list.next;
    while(pentry) {
        if(id == pentry->pmn->id)
            return pentry;
        pentry = pentry->next;
    }
    return NULL;
}

void db_cache_insert(MEDIA_NATIVE *pmn) {
    DB_CACHE_ENTRY *pentry;

    //    DPRINTF(E_DBG,L_DB,"Adding %d to cache. Current size: %d\n",pmn->id,db_cache_list.current_length);
    pentry = (DB_CACHE_ENTRY *)malloc(sizeof(DB_CACHE_ENTRY));
    if(!pentry)
        return;

    pentry->pmn = pmn;

    pentry->refcount = 1;
    pentry->prev = &db_cache_list.cache_list;
    pentry->next = db_cache_list.cache_list.next;
    db_cache_list.cache_list.next = pentry;

    if(pentry->next) {
        pentry->next->prev = pentry;
    } else {
        db_cache_list.tail = pentry;
    }

    /* maybe delete last */
    while((db_cache_list.current_length >= db_cache_list.max_length) &&
          (db_cache_list.tail->refcount == 0)) {
        /* gotta delete */
        pentry = db_cache_list.tail;
        db_cache_list.tail = db_cache_list.tail->prev;
        db_cache_list.tail->next = NULL;
        db_cache_dispose_item(pentry->pmn);
        free(pentry);
        db_cache_list.current_length--;
    }

    db_cache_list.current_length++;
}

/*
 * move a cache entry to the front of the list
 */
void db_cache_promote(DB_CACHE_ENTRY *pentry) {
    //    DPRINTF(E_DBG,L_DB,"promoting %d in cache\n",pentry->pmn->id);

    if(pentry == db_cache_list.cache_list.next) {
        return;
    }

    if(pentry == db_cache_list.tail)
        db_cache_list.tail = pentry->prev;

    if(pentry->next)
        pentry->next->prev = pentry->prev;
    pentry->prev->next = pentry->next;

    pentry->next = db_cache_list.cache_list.next;
    pentry->prev = &db_cache_list.cache_list;

    if(pentry->next)
        pentry->next->prev = pentry;

    db_cache_list.cache_list.next = pentry;
}

void db_cache_update(MEDIA_NATIVE *pmn) {
    MEDIA_NATIVE *pmn_new, *pmn_existing;
    DB_CACHE_ENTRY *pcache;
    int field;
    int offset;
    void *pold, *pnew;

    //    DPRINTF(E_DBG,L_DB,"write-through cache update of %d\n",pmn->id);
    for(field = 0; field < SG_LAST; field++) {
        memcpy(pmn_new,pmn,sizeof(MEDIA_NATIVE));

        switch(ff_field_data[field].type) {
        case FT_STRING:
            offset = ff_field_data[field].offset;
            pold = (void*)((pmn) + offset);
            pnew = (void*)((pmn_new) + offset);

            *((char**)pnew) = strdup(*((char**)pold));
            break;
        }
    }

    util_mutex_lock(l_pl);
    /* got a new copy of the pmn */
    pcache = db_cache_find(pmn->id);
    if(pcache) {
        /* promote and update */
        db_cache_promote(pcache);
        pmn_existing = pcache->pmn;
        pcache->pmn = pmn_new;
        db_cache_dispose_item(pmn_existing);
    } else {
        /* just add */
        db_cache_insert(pmn_new);
    }
    util_mutex_unlock(l_pl);
}

MEDIA_NATIVE *db_cache_fetch(char **pe, uint32_t id) {
    DB_CACHE_ENTRY *pentry;
    MEDIA_NATIVE *pmn = NULL;
    MEDIA_STRING *pms;
    void *opaque;

    util_mutex_lock(l_pl);
    db_cache_dump();

    /* first, see if it is in the cache */
    DPRINTF(E_DBG,L_DB,"Fetching %d from cache\n",id);

    config.stats.db_id_fetches++;

    pentry = db_cache_find(id);
    if(pentry) {
        DPRINTF(E_DBG,L_DB,"Cache hit\n");
        config.stats.db_id_hits++;
        pmn = pentry->pmn;
        db_cache_promote(pentry);
        pentry->refcount++;
    } else {
        DPRINTF(E_DBG,L_DB,"Cache miss\n");
        if(DB_E_SUCCESS == db_pfn->db_fetch_item(pe, id, &opaque, &pms)) {
            if(pms) {
                pmn = db_string_to_native(pms);
                db_cache_insert(pmn);
            }

            db_pfn->db_dispose_item(opaque, pms);
        } else {
            DPRINTF(E_DBG,L_DB,"Couldn't fetch from underlying storage\n");
        }
    }
    db_cache_dump();
    util_mutex_unlock(l_pl);
    return pmn;
}

/**
 * decrement refcount on a media object
 *
 * @param pmo media object to dec refcount
 */
void db_dispose_item(MEDIA_NATIVE *pmo) {
    DB_CACHE_ENTRY *pentry;

    util_mutex_lock(l_pl);

    pentry = db_cache_find(pmo->id);
    if(pentry)
        pentry->refcount--;

    util_mutex_unlock(l_pl);
}

/**
 * *really* dispose of a item fetched by db_fetch_item
 *
 * @param pmo media object to dispose
 */
void db_cache_dispose_item(MEDIA_NATIVE *pmo) {
    ASSERT(pmo);

    if(!pmo)
        return;

    MAYBEFREE(pmo->path);
    MAYBEFREE(pmo->fname);
    MAYBEFREE(pmo->title);
    MAYBEFREE(pmo->artist);
    MAYBEFREE(pmo->album);
    MAYBEFREE(pmo->genre);
    MAYBEFREE(pmo->comment);
    MAYBEFREE(pmo->type);
    MAYBEFREE(pmo->composer);
    MAYBEFREE(pmo->orchestra);
    MAYBEFREE(pmo->conductor);
    MAYBEFREE(pmo->grouping);
    MAYBEFREE(pmo->description);
    MAYBEFREE(pmo->url);
    MAYBEFREE(pmo->codectype);
    MAYBEFREE(pmo->album_artist);
    free(pmo);
}

/*
 * db_readlock
 *
 * If this fails, something is so amazingly hosed, we might just as well
 * terminate.
 */
void db_readlock(void) {
    int err;

    DPRINTF(E_SPAM,L_LOCK,"entering db_readlock\n");
    if((err=pthread_rwlock_rdlock(&db_rwlock))) {
        DPRINTF(E_FATAL,L_DB,"cannot lock rdlock: %s\n",strerror(err));
    }
    DPRINTF(E_SPAM,L_LOCK,"db_readlock acquired\n");
}

/*
 * db_writelock
 *
 * same as above
 */
void db_writelock(void) {
    int err;

    DPRINTF(E_SPAM,L_LOCK,"entering db_writelock\n");
    if((err=pthread_rwlock_wrlock(&db_rwlock))) {
        DPRINTF(E_FATAL,L_DB,"cannot lock rwlock: %s\n",strerror(err));
    }
    DPRINTF(E_SPAM,L_LOCK,"db_writelock acquired\n");
}

/*
 * db_unlock
 *
 * useless, but symmetrical
 */
int db_unlock(void) {
    DPRINTF(E_SPAM,L_LOCK,"releasing db lock\n");
    return pthread_rwlock_unlock(&db_rwlock);
}

/**
 * Build an error string
 */
void db_set_error(char **pe, int error, ...) {
    va_list ap;
    char *errorptr;

    if(!pe)
        return;

    va_start(ap, error);
    errorptr = util_vasprintf(db_error_list[error], ap);
    va_end(ap);

    DPRINTF(E_SPAM,L_MISC,"Raising error: %s\n",errorptr);
    *pe = errorptr;
}

/**
 * Must dynamically initialize the rwlock, as Mac OSX 10.3 (at least)
 * doesn't have a static initializer for rwlocks
 */
void db_init_once(void) {
    pthread_rwlock_init(&db_rwlock,NULL);
}

/**
 * open first, before we drop privs.  The init is called
 * shortly after (but probably after dropping privs)
 *
 * @param pe error string buffer
 * @param type what kind of db (sqlite, sqlite3, etc)
 * @param parameters specific parameters to pass to the db
 * @returns DB_E_SUCESS on success;
 */
int db_open(char **pe, char *type, char *parameters) {
    int result;

    ASSERT(type);

    if(!type) {
        db_set_error(pe,DB_E_BADPROVIDER,"<null>");
        return DB_E_BADPROVIDER;
    }

    DPRINTF(E_DBG,L_DB,"Opening database\n");

    if(pthread_once(&db_initlock,db_init_once))
        return DB_E_PTHREAD;

    db_pfn = (PLUGIN_DB_FN*)malloc(sizeof(PLUGIN_DB_FN));
    if(!db_pfn) {
        db_set_error(pe, DB_E_MALLOC);
        return DB_E_MALLOC;
    }

    memset(db_pfn,0,sizeof(PLUGIN_DB_FN));

#ifdef HAVE_LIBSQLITE
    if(0 == strcasecmp(type,"sqlite")) {
        db_pfn->db_open = db_sqlite2_open;
        db_pfn->db_close = db_sqlite2_close;
        db_pfn->db_add = db_sqlite2_add;
        db_pfn->db_del = db_sqlite2_del;
        db_pfn->db_enum_start = db_sqlite2_enum_items_begin;
        db_pfn->db_enum_fetch = db_sqlite2_enum_items_fetch;
        db_pfn->db_enum_reset = db_sqlite2_enum_restart;
        db_pfn->db_enum_end = db_sqlite2_enum_end;
        db_pfn->db_fetch_item = db_sqlite2_fetch_item;
        db_pfn->db_dispose_item = db_sqlite2_dispose_item;
        db_pfn->db_hint = db_sqlite2_hint;
    }
#endif
#ifdef HAVE_LIBSQLITE3
        db_pfn->db_open = db_sqlite3_open;
        db_pfn->db_close = db_sqlite3_close;
        db_pfn->db_add = db_sqlite3_add;
        db_pfn->db_del = db_sqlite3_del;
        db_pfn->db_enum_start = db_sqlite3_enum_items_begin;
        db_pfn->db_enum_fetch = db_sqlite3_enum_items_fetch;
        db_pfn->db_enum_reset = db_sqlite3_enum_restart;
        db_pfn->db_enum_end = db_sqlite3_enum_end;
        db_pfn->db_fetch_item = db_sqlite3_fetch_item;
        db_pfn->db_dispose_item = db_sqlite3_dispose_item;
        db_pfn->db_hint = db_sqlite3_hint;
#endif

    if(!db_pfn) {
        db_set_error(pe,DB_E_BADPROVIDER,type);
        return DB_E_BADPROVIDER;
    }

    result=db_pfn->db_open(pe, parameters);

    DPRINTF(E_DBG,L_DB,"Results: %d\n",result);
    return result;
}

/**
 * send a hint to the underlying database, so it can
 * potentially optimize upcoming transactions
 *
 * @param hint hint to send (@see ff-dbstruct.h)
 */
void db_hint(int hint) {
    RBLIST *rblist;
    DB_PATH_NODE *pnode, *pnext;

    if(db_pfn->db_hint)
        db_pfn->db_hint(hint);

    switch(hint) {
    case DB_HINT_FULLSCAN_START:
        db_writelock();
        rblist = rbopenlist(db_path_lookup);
        if(rblist) {
            while(NULL != (pnode = (DB_PATH_NODE*)rbreadlist(rblist))) {
                pnode->fetched=0;
            }
            rbcloselist(rblist);
        } else {
            DPRINTF(E_LOG,L_DB,"Can't open path lookup for tree traversal\n");
        }
        db_unlock();
        break;

    case DB_HINT_FULLSCAN_END:
        db_writelock();
        pnode=(DB_PATH_NODE*)rblookup(RB_LUFIRST, NULL, db_path_lookup);
        while(pnode) {
            pnext = (DB_PATH_NODE*)rblookup(RB_LUGREAT, (void*)pnode, db_path_lookup);
            if(!pnode->fetched) {
                DPRINTF(E_INF,L_DB,"File disappeared: %s\n", pnode->path);
                db_del_nolock(NULL,pnode->id);
                rbdelete(pnode, db_path_lookup);
                free(pnode->path);
                free(pnode);
            }
            pnode = pnext;
        }
        db_unlock();
        break;

    default:
        break;
    }
}

static int db_path_compare(const void *p1, const void *p2, const void *arg) {
    DB_PATH_NODE *ppn1, *ppn2;
    int result;

    ppn1 = (DB_PATH_NODE*)p1;
    ppn2 = (DB_PATH_NODE*)p2;

    DPRINTF(E_SPAM,L_DB,"Comparing %s:%d to %s:%d\n",ppn1->path,ppn1->index,
            ppn2->path,ppn2->index);
    result = strcmp(ppn1->path, ppn2->path);
    if(result)
        return result;

    if(ppn1->index < ppn2->index)
        return -1;
    if(ppn1->index > ppn2->index)
        return 1;

    return 0;
}

/**
 * do the startup processing for the database.  This is stuff that
 * can be done after privs are dropped
 *
 * @param reload this is a hint from main about whether or not a full
 *               reload has been requested
 * @returns DB_E_SUCCESS on success
 */
int db_init(int reload) {
    uint32_t id;
    int result;
    char *pe;
    MEDIA_STRING *pmo;
    uint32_t m_id;
    DB_PATH_NODE *pnew;
    void *opaque;

    /* this should arguably be done in pl_init, rather than here */
    pl_add_playlist(&pe,"Library",PL_STATICWEB,NULL,NULL,0,&id);
    if(id != 1) {
        DPRINTF(E_FATAL,L_DB,"Can't add library playlist: %s\n",pe);
        free(pe);
    }

    /* walk through and add all the items */
    if(DB_E_SUCCESS != (result = db_pfn->db_enum_start(&pe,&opaque))) {
        DPRINTF(E_FATAL,L_DB,"Error populating initial playlist: %s\n",pe);
        free(pe);
        return result;
    }

    db_path_lookup = rbinit(db_path_compare, NULL);

    /* FIXME: assumes string return */
    while((DB_E_SUCCESS == (result=db_pfn->db_enum_fetch(&pe, opaque, &pmo))) && pmo) {
        /* got a row */

        /* add to path map */
        pnew = (DB_PATH_NODE*)malloc(sizeof(DB_PATH_NODE));
        if(!pnew)
            DPRINTF(E_FATAL,L_DB,"Malloc error allocating path map entry\n");

        pnew->path = strdup(pmo->path);
        pnew->index = util_atoui32(pmo->idx);
        pnew->id = util_atoui32(pmo->id);
        pnew->fetched = 0;

        if(!rbsearch((const void*)pnew, db_path_lookup)) {
            DPRINTF(E_FATAL,L_DB,"Can't insert into path map\n");
        }

        m_id = util_atoui32(pmo->id);
        if(m_id) {
            if(PL_E_SUCCESS != (result = pl_add_playlist_item(&pe, 1, m_id))) {
                DPRINTF(E_LOG,L_DB,"Error inserting item into library: %s\n",
                        pe);
                free(pe);
                db_pfn->db_enum_end(NULL,opaque);
                return result;
            }
        }
    }

    db_pfn->db_enum_end(NULL,opaque);

    pl_init(NULL);

    return DB_E_SUCCESS;
}

int db_deinit(void) {
    return DB_E_SUCCESS;
}

int db_revision(void) {
    return db_revision_no;
}

/**
 * add a media item to the database.  This needs to also
 * invalidate the cache on adds, but doesn't, currently.
 *
 * @param pe error string buffer
 * @param pmo media object
 * @returns DB_E_SUCCESS on success, error code on failure with pe allocated
 */
int db_add(char **pe, MEDIA_NATIVE *pmo) {
    int result;
    MEDIA_NATIVE *ptemp;

    ptemp = db_fetch_path(NULL, pmo->path, pmo->idx);
    if(ptemp) {
        pmo->id = ptemp->id;
        db_dispose_item(ptemp);
    }

    pmo->time_modified = (uint32_t)time(NULL);

    if(!pmo->id) {
        pmo->time_added = pmo->time_modified;
        pmo->play_count = 0;
        pmo->time_played = 0;
    }

    if(db_pfn->db_add) {
        db_writelock();
        db_utf8_validate(pmo);
        db_trim_strings(pmo);

        result = db_pfn->db_add(pe,pmo);
        /* FIXME: deadlock?  Do I ever acquired a db lock with the playlist
         * lock held? */
        if(DB_E_SUCCESS == result)
            pl_advise_add(pmo);

        db_unlock();
        return result;
    }

    /* write through the cache */
    //    db_cache_update(pmo);

    return DB_E_SUCCESS;
}

int db_del(char **pe, uint32_t id) {
    int result;

    db_writelock();
    result = db_del_nolock(pe, id);
    db_unlock();
    return result;
}

int db_del_nolock(char **pe, uint32_t id) {
    int result = DB_E_NOTIMPL;

    if(db_pfn->db_del) {
        result = db_pfn->db_del(pe, id);
    }

    if(DB_E_SUCCESS == result)
        pl_advise_del(id);

    return result;
}

/**
 * start enumerating all items, based on the specifications set up
 * in the query.  (items, distinct, playlists, etc)
 *
 * If the enum_start returns an error, the lock will not be held -
 * the caller should not call db_enum_end
 *
 * @param pe error string buffer
 * @param pinfo db query to enumerate
 * @returns DB_E_SUCCESS on success, error with pe allocated otherwise
 */
int db_enum_start(char **pe, DB_QUERY *pinfo) {
    ENUMHELPER *peh;
    PLAYLIST_NATIVE *plold;
    char *e_pl = NULL;
    char *name;
    int err;

    db_readlock();

    if(pinfo->limit == 0)
        pinfo->limit = INT_MAX;

    if(pinfo->playlist_id == 0)
        pinfo->playlist_id = 1;

    pinfo->priv = (void*)malloc(sizeof(ENUMHELPER));
    if(!pinfo->priv) {
        db_set_error(pe,DB_E_MALLOC);
        db_unlock();
        return DB_E_MALLOC;
    }
    memset(pinfo->priv,0,sizeof(ENUMHELPER));
    peh = pinfo->priv;

    if(pinfo->filter) {
        if(pinfo->playlist_id != 1) {
            /* construct a new query, based on playlist */
            plold = pl_fetch_playlist_id(&e_pl,pinfo->playlist_id);
            if(!plold) {
                db_set_error(pe,DB_E_PLAYLIST,e_pl);
                free(e_pl);
                db_unlock();
                return DB_E_PLAYLIST;
            }

            peh->composite_query = util_asprintf("(%s) and (%s)",
                                                 plold->query,
                                                 pinfo->filter);
            pl_dispose_playlist(plold);
        } else {
            peh->composite_query = strdup(pinfo->filter);
        }

        peh->old_playlist = pinfo->playlist_id;

        /* got the new query in pinfo->query, now set up a hidden playlist */
        name = util_asprintf("temp-%x-%p",util_get_threadid(), &pinfo);

        err = pl_add_playlist(&e_pl, name, PL_SMART | PL_HIDDEN,
                              peh->composite_query, NULL, 0,
                              &pinfo->playlist_id);
        if(err != PL_E_SUCCESS) {
            db_set_error(pe,DB_E_PLAYLIST,e_pl);
            free(e_pl);
            if(peh->composite_query) free(peh->composite_query);
            db_unlock();
            return DB_E_PLAYLIST;
        }

        DPRINTF(E_DBG,L_PL,"Created new playlist %d with query %s\n",
                pinfo->playlist_id, peh->composite_query);
    }

    switch(pinfo->query_type) {
    case QUERY_TYPE_ITEMS:
        peh->enum_start = db_enum_items_start;
        peh->enum_fetch = db_enum_items_fetch;
        //        peh->enum_reset = db_enum_items_reset;
        //        peh->enum_end = db_enum_items_end;
        break;
    case QUERY_TYPE_PLAYLISTS:
        peh->enum_start = db_enum_playlist_start;
        peh->enum_fetch = db_enum_playlist_fetch;
        //        peh->enum_reset = db_enum_playlist_reset;
        //        peh->enum_end = db_enum_playlist_end;
        break;
    case QUERY_TYPE_DISTINCT:
        peh->enum_start = db_enum_browse_start;
        peh->enum_fetch = db_enum_browse_fetch;
        //        peh->enum_reset = db_enum_browse_reset;
        //        peh->enum_end = db_enum_browse_end;
        break;
    default:
        DPRINTF(E_FATAL,L_DB,"Bad query type: %d\n", pinfo->query_type);
        break;
    }

    err = peh->enum_start(pe, pinfo);
    if(err != DB_E_SUCCESS) {
        if(peh->composite_query) {
            free(peh->composite_query);
            pl_delete_playlist(NULL,pinfo->playlist_id);
        }
    }

    return err;
}

int db_enum_items_start(char **pe, DB_QUERY *pinfo) {
    char *e_pl;
    ENUMHELPER *peh;
    PLAYLIST_NATIVE *ppn;

    peh = (ENUMHELPER*)pinfo->priv;

    /* this won't work with a query */
    ppn = pl_fetch_playlist_id(&e_pl, pinfo->playlist_id);
    if(!ppn) {
        DPRINTF(E_LOG,L_DB,"Error fetching playlist %d\n",pinfo->playlist_id);
        db_unlock();
        db_set_error(pe,DB_E_PLAYLIST,e_pl);
        free(e_pl);
        free(pinfo->priv);
        return DB_E_PLAYLIST;
    }
    pinfo->totalcount = ppn->items;
    pl_dispose_playlist(ppn);
    peh->handle = (void*)pl_enum_items_start(&e_pl, pinfo->playlist_id);

    if(!peh->handle) {
        DPRINTF(E_LOG,L_DB,"Error starting playlist enumeration: %s\n",e_pl);
        db_unlock();
        db_set_error(pe,DB_E_PLAYLIST,e_pl);
        free(e_pl);
        free(pinfo->priv);
        return DB_E_PLAYLIST;
    }

    return DB_E_SUCCESS;
}

int db_enum_playlist_start(char **pe, DB_QUERY *pinfo) {
    char *e_pl;
    ENUMHELPER *peh;

    peh = (ENUMHELPER*)pinfo->priv;

    pl_get_playlist_count(pe, &pinfo->totalcount);

    peh->handle = (void*)pl_enum_start(&e_pl);

    if(!peh->handle) {
        DPRINTF(E_LOG,L_DB,"Error starting playlist enumeration: %s\n",e_pl);
        db_unlock();
        db_set_error(pe,DB_E_PLAYLIST,e_pl);
        free(e_pl);
        free(pinfo->priv);
        return DB_E_PLAYLIST;
    }

    return DB_E_SUCCESS;
}

int db_distinct_compare(const void *v1, const void *v2, const void *pinfo) {
    int field_id = *((int*)(pinfo));  /* might care if we're doing 'The' fixups */
    char *f1 = v1? (char*)v1 : "";
    char *f2 = v2? (char*)v2 : "";

    return strcasecmp(f1,f2);
}

int db_enum_browse_start(char **pe, DB_QUERY *pinfo) {
    DB_QUERY pinfo2;
    int err;
    char **rows;
    int count = 0;
    void *val;
    ENUMHELPER *peh;
    char *value;

    // We'll want to handle the query, etc
    memcpy(&pinfo2,pinfo,sizeof(DB_QUERY));
    pinfo2.query_type = QUERY_TYPE_ITEMS;

    DPRINTF(E_DBG,L_DB,"Browsing playlist %d\n",pinfo2.playlist_id);

    peh = (ENUMHELPER*)pinfo->priv;

    pinfo->totalcount=0;

    peh->pdistinct = rbinit(db_distinct_compare,&pinfo->distinct_field);

    // Walk through the db and get all distincts
    if(DB_E_SUCCESS == db_enum_start(pe, &pinfo2)) {
        while((DB_E_SUCCESS == (err=db_enum_fetch(pe, &rows, &pinfo2))) && rows) {
            value = rows[pinfo->distinct_field] ? rows[pinfo->distinct_field] : "";

            if((strlen(value)) && (!rbfind((void*)rows[pinfo->distinct_field],peh->pdistinct))) {
                count++;
                DPRINTF(E_DBG,L_DB,"Adding distinct: %s\n",rows[pinfo->distinct_field]);
                val=(void*)rbsearch((const void*)strdup(rows[pinfo->distinct_field]), peh->pdistinct);
                if(!val) {
                    DPRINTF(E_LOG,L_DB,"Error adding distinct value %s\n",
                            rows[pinfo->distinct_field]);
                }

            }
        }

        if(err != DB_E_SUCCESS) {
            DPRINTF(E_LOG,L_DB,"Error enumerating playlist: %s\n",pe);
            db_enum_end(NULL, pinfo);
            return err;
        }

    } else {
        DPRINTF(E_LOG,L_DB,"Error starting enumeration: %s\n",pe);
        free(pe);
    }

    db_enum_end(pe, &pinfo2);
    pinfo->totalcount = count;
    peh->nextop = RB_LUFIRST;
    peh->last_value = NULL;
    return DB_E_SUCCESS;
}

/**
 * fetch the next media item from the database
 *
 * @param pe error buffer
 * @param ppln pointer to receive media object
 * @returns DB_E_SUCCESS on success, error code with pe allocated on failure
 */
int db_enum_fetch(char **pe, char ***result, DB_QUERY *pquery) {
    ENUMHELPER *peh;
    int err;

    ASSERT((pquery) && (pquery->priv));

    if(!pquery || !pquery->priv) {
        *result = NULL;
        return DB_E_SUCCESS;
    }

    peh = (ENUMHELPER*)pquery->priv;

    if(peh->current_position >= pquery->offset + pquery->limit) {
        *result = NULL;
        return DB_E_SUCCESS;
    }

    /* FIXME: could give hints to the item fetches about whether or
     * not they are going to be skipped, allowing for a partial
     * fetch
     */
    while(1) {
        config.stats.db_enum_fetches++;
        err = peh->enum_fetch(pe, result, pquery);
        if((peh->current_position >= pquery->offset) ||
           (err != DB_E_SUCCESS) ||
           (!*result)) {
            peh->current_position++;
            break;
        }
        peh->current_position++;
    }

    return err;
}

int db_enum_items_fetch(char **pe, char ***result, DB_QUERY *pquery) {
    uint32_t id;
    ENUMHELPER *peh;
    int err;

    peh = (ENUMHELPER*)pquery->priv;

    if(peh->result) {
        db_pfn->db_dispose_item(peh->opaque, (MEDIA_STRING*)peh->result);
        peh->result = NULL;
    }

    id = pl_enum_items_fetch(NULL, peh->handle);
    if(!id) {
        *result = NULL;
        return DB_E_SUCCESS;
    }

    /* fetch the item -- won't cache this */
    config.stats.db_enum_fetches++;
    if(DB_E_SUCCESS == (err = db_pfn->db_fetch_item(pe, id, &peh->opaque, (MEDIA_STRING **)&peh->result))) {
        *result = peh->result;
    }

    return DB_E_SUCCESS;
}

int db_enum_playlist_fetch(char **pe, char ***result, DB_QUERY *pquery) {
    ENUMHELPER *peh;

    peh = (ENUMHELPER*)pquery->priv;

    return pl_enum_fetch(pe, result, peh->handle);
}

int db_enum_browse_fetch(char **pe, char ***result, DB_QUERY *pquery) {
    ENUMHELPER *peh;
    char *ptr;

    peh = (ENUMHELPER*)pquery->priv;

    ptr = (char*)rblookup(peh->nextop, peh->last_value, peh->pdistinct);
    peh->nextop = RB_LUNEXT;
    peh->last_value = ptr;

    DPRINTF(E_DBG,L_DB,"Returning browse: %s\n",ptr);
    if(ptr)
        *result = &peh->last_value; /* hack hack hack */
    else
        *result = NULL;

    return DB_E_SUCCESS;
}



/**
 * reset enumeration
 *
 * @param pe error buffer
 * @returns DB_E_SUCCESS on success, error code with pe allocate on failure
 */
int db_enum_reset(char **pe, DB_QUERY *pquery) {
    switch(pquery->query_type) {
    case QUERY_TYPE_ITEMS:
        pl_enum_items_reset(pe, ((ENUMHELPER*)pquery->priv)->handle);
        break;
    case QUERY_TYPE_PLAYLISTS:
        pl_enum_reset(pe, ((ENUMHELPER*)pquery->priv)->handle);
        break;
    case QUERY_TYPE_DISTINCT:
        break;
    }
    return DB_E_SUCCESS;
}

/**
 * finish enumeration
 *
 * @param pe error buffer
 * @returns DB_E_SUCCESS on success, error code with pe allocate on failure
 */
int db_enum_end(char **pe, DB_QUERY *pquery) {
    ENUMHELPER *peh;
    char *pelement;

    ASSERT((pquery) && (pquery->priv));

    if((!pquery) || (!pquery->priv))
        return DB_E_SUCCESS;

    peh = (ENUMHELPER*)pquery->priv;

    switch(pquery->query_type) {
    case QUERY_TYPE_ITEMS:
        if(peh->result) {
            DPRINTF(E_DBG,L_PL,"Freeing last result\n");
            free(peh->result);
        }
        DPRINTF(E_DBG,L_PL,"Ending playlist enumeration\n");
        pl_enum_items_end(peh->handle);
        break;
    case QUERY_TYPE_PLAYLISTS:
        pl_enum_end(peh->handle);
        break;
    case QUERY_TYPE_DISTINCT:
        if(peh->pdistinct) {
            pelement = (char*)rblookup(RB_LUFIRST,NULL,peh->pdistinct);
            while(pelement) {
                pelement = (char*)rbdelete((void*)pelement,peh->pdistinct);
                if(pelement)
                    free(pelement);
                pelement = (void*)rblookup(RB_LUFIRST,NULL,peh->pdistinct);
            }
            rbdestroy(peh->pdistinct);
            peh->pdistinct = NULL;
        }
        break;
    }

    if(peh->composite_query) {
        free(peh->composite_query);
        pl_delete_playlist(NULL,pquery->playlist_id);
    }

    DPRINTF(E_DBG,L_PL,"Freeing prive\n");
    free(peh);

    db_unlock();
    return DB_E_SUCCESS;
}

/**
 * wrapper for pl_add_playlist.  returns DB_E_SUCCESS on success,
 * error code otherwise, with pe set.  pe must be freed by caller
 *
 * @param pe error buffer
 * @param name name of playlist to add
 * @param type type of playlist to add
 * @param clause the db query (for smart playlists only)
 * @param path path of underlying playlist (for file based playlists)
 * @param index index in playlist (for files that contain multiple playlists)
 * @return DB_E_SUCCESS on success, error code otherwise
 */
int db_add_playlist(char **pe, char *name, int type, char *clause, char *path, int index, uint32_t *playlistid) {
    return pl_add_playlist(pe, name, type, clause, path, index, playlistid);
}

/**
 * wrapper for pl_add_playlist_item.  Add an item to a playlist
 * (for playlists != PL_SMART).  Returns DB_E_SUCCESS on success,
 * error code with pe allocated on failure.  User must free pe on
 * failure.
 *
 * @param pe error buffer
 * @param playlistid id of playlist to add media object to
 * @param songid media object to be added
 * @return DB_E_SUCCESS on succes, error code with pe allocated otherwise
 */
int db_add_playlist_item(char **pe, int playlistid, int songid) {
    return pl_add_playlist_item(pe, playlistid, songid);
}

/**
 * wrapper for pl_delete_playlist_item.  Delete an item from a playlist
 * (for playlists != PL_SMART).  Returns DB_E_SUCCESS on success,
 * error code with pe allocated on failure.  User must free pe on
 * failure.
 *
 * @param pe error buffer
 * @param playlistid id of playlist to delete media object fromo
 * @param songid media object to be deleted
 * @return DB_E_SUCCESS on succes, error code with pe allocated otherwise
 */
int db_delete_playlist_item(char **pe, int playlistid, int songid) {
    return pl_delete_playlist_item(pe, playlistid, songid);
}

/**
 * wrapper for pl_edit_playlist.  Edit an existing playlist.
 *
 * @param pe error buffer
 * @param id id of playlist to edit
 * @param name new name of playlist
 * @param clause new playlist query (when type == PL_SMART)
 * @returns DB_E_SUCCESS on success, error code with pe allocated otherwise
 */
int db_edit_playlist(char **pe, int id, char *name, char *clause) {
    return pl_edit_playlist(pe, id, name, clause);
}

/**
 * wrapper for pl_delete_playlists.  Deletes a playlist.
 * Returns DB_E_SUCCESS on success, error code with pe allocated on
 * failure.  User must free pe on failure.
 *
 * @param pe error buffer
 * @param playlistid id of playlist to delete
 * @return DB_E_SUCCESS on succes, error code with pe allocated otherwise
 */
int db_delete_playlist(char **pe, int playlistid) {
    return pl_delete_playlist(pe, playlistid);
}

/**
 * fetch a playlist object for a specific path and index
 *
 * @param pe error buffer
 * @param path path of playlist (for file based playlists)
 * @param index index of playlist (for files with multiple playlists)
 * @returns native playlist object, or NULL on failure (not found)
 */
PLAYLIST_NATIVE *db_fetch_playlist(char **pe, char *path, int index) {
    return pl_fetch_playlist(pe, path, index);
}

/**
 * wrapper for pl_dispose_playlist
 *
 * @param ppln playlist object to dispose of (from db_fetch_playlist)
 */
void db_dispose_playlist(PLAYLIST_NATIVE *ppln) {
    pl_dispose_playlist(ppln);
}

/**
 * fetch an item from the database.  Returns pointer to media object
 * on success, NULL otherwise.
 *
 * @param pe error buffer
 * @param id media object id
 * @returns MEDIA_STRING* if successful, NULL otherwise
 */
MEDIA_NATIVE *db_fetch_item(char **pe, int id) {
    MEDIA_NATIVE *pnative;

    db_readlock();
    pnative = db_fetch_item_nolock(pe, id);
    db_unlock();
    return pnative;
}

MEDIA_NATIVE *db_fetch_item_nolock(char **pe, int id) {
    return db_cache_fetch(pe,id);
}


/**
 * get a media object by path
 *
 * @param pe error buffer
 * @param path path of media object
 * @param index index of media object
 * @returns media object on success, NULL with pe allocated otherwise
 */
MEDIA_NATIVE *db_fetch_path(char **pe, char *path, int index) {
    DB_PATH_NODE path_node;
    DB_PATH_NODE *pnode;

    /* this lookups into path cache... */
    path_node.path = path;
    path_node.index = index;

    pnode = (DB_PATH_NODE*)rbfind((void*)&path_node,db_path_lookup);
    if(!pnode) {
        DPRINTF(E_DBG,L_DB,"Couldn't find %s:%d\n",path,index);
        return NULL;
    }

    // Mark node as fetched in case we are in a scan
    pnode->fetched = 1;

    DPRINTF(E_DBG,L_DB,"Fetching item %s:%d\n",path,index);
    return db_fetch_item(pe, pnode->id);
}

/**
 * fetch the old object, update play count and time_played
 *
 * @param pe error string
 * @param id media id of object to update
 * @returns DB_E_SUCCESS on success
 */
int db_playcount_increment(char **pe, int id) {
    MEDIA_NATIVE *pold;
    int err;

    db_writelock();

    pold = db_fetch_item_nolock(pe, id);
    if(!pold) {
        db_unlock();
        return DB_E_DB_ERROR;
    }

    /* otherwise, update and re-insert */
    pold->play_count++;
    pold->time_played = time(NULL);

    if(db_pfn->db_add) {
        if(DB_E_SUCCESS != ((err = db_pfn->db_add(pe, pold)))) {
            db_unlock();
            return err;
        }
    }

    /* FIXME: Advise playlists and update cache.  Call db_add? */

    db_unlock();
    return DB_E_SUCCESS;
}

/**
 * get count of all songs... this is a cheat, as I'll just get the
 * count from the "library" playlist
 *
 * @param pe error string
 * @param count returns the count of items
 * @returns DB_E_SUCCESS on success, error code and pe allocated on failure
 */
int db_get_song_count(char **pe, int *count) {
    PLAYLIST_NATIVE *ppln;
    char *pl_pe;
    int result = DB_E_SUCCESS;

    db_readlock();

    ppln = pl_fetch_playlist_id(&pl_pe, 1);
    if(!ppln) {
        result = DB_E_PLAYLIST;
        db_set_error(pe, DB_E_PLAYLIST, pl_pe);
        free(pl_pe);
        *count = 0;
    } else {
        *count = ppln->items;
        pl_dispose_playlist(ppln);
    }

    db_unlock();
    return DB_E_SUCCESS;
}

/**
 * return the number of playlists there are.
 *
 * @param pe error buffer
 * @param count returns the number of playlists
 * @returns DB_E_SUCCESS on success, pe allocated and error on error
 */
int db_get_playlist_count(char **pe, int *count) {
    char *pl_pe;
    int err;

    db_readlock();

    if(PL_E_SUCCESS != ((err = pl_get_playlist_count(&pl_pe, count)))) {
        db_set_error(pe, DB_E_PLAYLIST, pl_pe);
        free(pl_pe);
        db_unlock();

        *count = 0;
        return DB_E_PLAYLIST;
    }

    db_unlock();
    return DB_E_SUCCESS;
}

/**
 * check the strings in a MP3FILE to ensure they are
 * valid utf-8.  If they are not, the string will be corrected
 *
 * \param pmp3 MP3FILE to verify for valid utf-8
 */
void db_utf8_validate(MP3FILE *pmp3) {
    int is_invalid=0;

    /* we won't bother with path and fname... those were culled with the
     * scan.  Even if they are invalid (_could_ they be?), then we
     * won't be able to open the file if we change them.  Likewise,
     * we won't do type or description, as these can't be bad, or they
     * wouldn't have been scanned */

    is_invalid = db_utf8_validate_string(pmp3->title);
    is_invalid |= db_utf8_validate_string(pmp3->artist);
    is_invalid |= db_utf8_validate_string(pmp3->album);
    is_invalid |= db_utf8_validate_string(pmp3->genre);
    is_invalid |= db_utf8_validate_string(pmp3->comment);
    is_invalid |= db_utf8_validate_string(pmp3->composer);
    is_invalid |= db_utf8_validate_string(pmp3->orchestra);
    is_invalid |= db_utf8_validate_string(pmp3->conductor);
    is_invalid |= db_utf8_validate_string(pmp3->grouping);
    is_invalid |= db_utf8_validate_string(pmp3->url);

    if(is_invalid) {
        DPRINTF(E_LOG,L_SCAN,"Invalid UTF-8 in %s\n",pmp3->path);
    }
}

/**
 * check a string to verify it is valid utf-8.  The passed
 * string will be in-place modified to be utf-8 clean by substituting
 * the character '?' for invalid utf-8 codepoints
 *
 * \param string string to clean
 */
int db_utf8_validate_string(char *string) {
    char *current = string;
    int run,r_current;
    int retval=0;

    if(!string)
        return 0;

     while(*current) {
        if(!((*current) & 0x80)) {
            current++;
        } else {
            run=0;

            /* it's a lead utf-8 character */
            if((*current & 0xE0) == 0xC0) run=1;
            if((*current & 0xF0) == 0xE0) run=2;
            if((*current & 0xF8) == 0xF0) run=3;

            if(!run) {
                /* high bit set, but invalid */
                *current++='?';
                retval=1;
            } else {
                r_current=0;
                while((r_current != run) && (*(current + r_current + 1)) &&
                      ((*(current + r_current + 1) & 0xC0) == 0x80)) {
                    r_current++;
                }

                if(r_current != run) {
                    *current++ = '?';
                    retval=1;
                } else {
                    current += (1 + run);
                }
            }
        }
    }

    return retval;
}

/**
 * Trim the spaces off the string values.  It throws off
 * browsing when there are some with and without spaces.
 * This should probably be better fixed by having clean tags,
 * but seemed simple enough, and it does make sense that
 * while we are cleaning tags for, say, utf-8 hygene we might
 * as well get this too.
 *
 * @param pmp3 mp3 struct to fix
 */
void db_trim_strings(MP3FILE *pmp3) {
    db_trim_string(pmp3->title);
    db_trim_string(pmp3->artist);
    db_trim_string(pmp3->album);
    db_trim_string(pmp3->genre);
    db_trim_string(pmp3->comment);
    db_trim_string(pmp3->composer);
    db_trim_string(pmp3->orchestra);
    db_trim_string(pmp3->conductor);
    db_trim_string(pmp3->grouping);
    db_trim_string(pmp3->url);
}

/**
 * trim trailing spaces in a string.  Used by db_trim_strings
 *
 * @param string string to trim
 */
void db_trim_string(char *string) {
    if(!string)
        return;

    while(strlen(string) && (string[strlen(string) - 1] == ' '))
        string[strlen(string) - 1] = '\0';
}


/**
 * convert a native media object to a string-based media
 * object.  It is the caller's reponsibiliby to free the
 * native object
 *
 * @param pmon native media object to convert
 * @return media object, or NULL on error
 */
MEDIA_STRING *db_native_to_string(MEDIA_NATIVE *pmon) {
    return NULL;
}

/**
 * convert a string-based media object to a native object
 * it is the caller's responsibility to free the string object
 *
 * @param pmos media object to convert
 * @returns media object, or NULL if error
 */
MEDIA_NATIVE *db_string_to_native(MEDIA_STRING *pmos) {
    MEDIA_NATIVE *pnew;

    ASSERT(pmos);

    if(!pmos)
        return NULL;

    pnew = (MEDIA_NATIVE*)malloc(sizeof(MEDIA_NATIVE));
    if(!pnew)
        return NULL;

    memset(pnew,0,sizeof(MEDIA_NATIVE));

    DB_STR_COPY(path);
    DB_INT32_COPY(idx);
    DB_STR_COPY(fname);
    DB_STR_COPY(title);
    DB_STR_COPY(artist);
    DB_STR_COPY(album);
    DB_STR_COPY(genre);
    DB_STR_COPY(comment);
    DB_STR_COPY(type);
    DB_STR_COPY(composer);
    DB_STR_COPY(orchestra);
    DB_STR_COPY(conductor);
    DB_STR_COPY(grouping);
    DB_STR_COPY(url);

    DB_INT32_COPY(bitrate);
    DB_INT32_COPY(samplerate);
    DB_INT32_COPY(song_length);
    DB_INT64_COPY(file_size);
    DB_INT32_COPY(year);
    DB_INT32_COPY(track);
    DB_INT32_COPY(total_tracks);
    DB_INT32_COPY(disc);
    DB_INT32_COPY(total_discs);
    DB_INT32_COPY(time_added);
    DB_INT32_COPY(time_modified);
    DB_INT32_COPY(time_played);
    DB_INT32_COPY(play_count);
    DB_INT32_COPY(rating);
    DB_INT32_COPY(disabled);
    DB_INT32_COPY(bpm);
    DB_INT32_COPY(id);

    DB_STR_COPY(description);
    DB_STR_COPY(codectype);

    DB_INT32_COPY(item_kind);
    DB_INT32_COPY(data_kind);
    DB_INT64_COPY(sample_count);
    DB_INT32_COPY(compilation);
    DB_INT32_COPY(contentrating);
    DB_INT32_COPY(has_video);
    DB_INT32_COPY(bits_per_sample);

    DB_STR_COPY(album_artist);

    return pnew;
}


/**
 * fixups for converting db strings.   Make zero length or null
 * strings null.
 */
char *db_util_strdup(char *string) {
    if(string && strlen(string))
        return strdup(string);

    return NULL;
}

