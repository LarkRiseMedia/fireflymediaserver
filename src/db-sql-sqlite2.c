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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#define _XOPEN_SOURCE 500

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif

#include "daapd.h"
#include "conf.h"
#include "err.h"
#include "ff-dbstruct.h"
#include "db.h"
#include "db-sql-sqlite2.h"
#include "util.h"

#ifndef TRUE
#  define TRUE 1
#  define FALSE 0
#endif

typedef struct db_sqlite2_enum_helper_t {
    const char *tail;
    char *query;
    sqlite_vm *pvm;
} DB_SQLITE2_EH;

/* Globals */
static pthread_mutex_t db_sqlite2_mutex;
static pthread_key_t db_sqlite2_key;
static char db_sqlite2_path[PATH_MAX + 1];
extern char *db_sqlite2_initial;

#define DB_SQLITE2_VERSION 14

/* Forwards */
static void db_sqlite2_lock(void);
static void db_sqlite2_unlock(void);
static int db_sqlite2_enum_begin_helper(char **pe, void *opaque);
static int db_sqlite2_exec(char **pe, int loglevel, char *fmt, ...);
static int db_sqlite2_insert_id(void);
static int db_sqlite2_fetch_row(char **pe, void **opaque, char ***row, char *fmt, ...);
static void db_sqlite2_dispose_row(void *opaque);
static void db_sqlite2_set_version(int version);
static int db_sqlite2_enum_begin(char **pe, void **opaque, char *fmt, ...);
static int db_sqlite2_enum_fetch(char **pe, void *opaque, char ***row);
static int db_sqlite2_fetch_int(char **pe, int *ival, char *fmt, ...);

extern char *db_sqlite_updates[];


/**
 * delete a media object by id
 *
 * @param pe error buffer
 * @param id id of media object to delete
 * @returns DB_E_SUCCESS on success, error on failure with pe allocated
 */
int db_sqlite2_del(char **pe, uint32_t id) {
    return db_sqlite2_exec(pe,E_LOG,"delete from songs where id=%d",id);
}

/**
 * insert a media object into the database
 *
 * @param pe error buffer
 * @param pmo object to add
 * @returns DB_E_SUCCESS on success.  pmo->id gets updated on add/update
 */
int db_sqlite2_add(char **pe, MEDIA_NATIVE *pmo) {
    char *sql;
    char *term;
    int field,pass;
    int offset;
    int err;

    db_sqlite2_lock();

    if(pmo->id) {
        /* update query */
        sql = util_asprintf("update songs set ");
        for(field = 1; field < SG_LAST; field++) { /* skip id */
            offset = ff_field_data[field].offset;

            switch(ff_field_data[field].type) {
            case FT_INT32:
                sql = util_aasprintf(sql,"%s = %d%c ",ff_field_data[field].name,
                                     *((uint32_t*)(((void*)pmo)+offset)),
                                     (field == (SG_LAST - 1)) ? ' ' : ',');
                break;
            case FT_INT64:
                sql = util_aasprintf(sql,"%s = %llu%c ",ff_field_data[field].name,
                                     *((uint64_t*)(((void*)pmo)+offset)),
                                     (field == (SG_LAST - 1)) ? ' ' : ',');
                break;
            case FT_STRING:
                term = sqlite_mprintf("%Q",*(char**)(((void*)pmo)+offset));
                sql = util_aasprintf(sql,"%s = %s%c ",ff_field_data[field].name,
                                     term, (field == (SG_LAST - 1)) ? ' ' : ',');
                sqlite_freemem(term);
                break;
            default:
                DPRINTF(E_FATAL,L_DB,"Unhandled data type in db_add for '%s'\n",
                        ff_field_data[field].name);
                break;
            }
        }
        sql = util_aasprintf(sql,"where id=%d",pmo->id);
    } else {
        /* insert query */
        sql = util_asprintf("insert into songs (");
        for(pass = 0; pass < 2; pass++) {
            for(field = 1; field < SG_LAST; field++) { /* skip id */
                if(!pass) {
                    sql = util_aasprintf(sql,"%s%c ",ff_field_data[field].name,
                                         (field == (SG_LAST - 1)) ? ')' : ',');
                } else {
                    offset = ff_field_data[field].offset;
                    switch(ff_field_data[field].type) {
                    case FT_INT32:
                        sql = util_aasprintf(sql,"%d%c ",
                                             *((uint32_t*)(((void*)pmo)+offset)),
                                             (field == (SG_LAST - 1)) ? ')' : ',');
                        break;
                    case FT_INT64:
                        sql = util_aasprintf(sql,"%llu%c ",
                                             *((uint64_t*)(((void*)pmo)+offset)),
                                             (field == (SG_LAST - 1)) ? ')' : ',');
                        break;
                    case FT_STRING:
                        term = sqlite_mprintf("%Q",*(char**)((((void*)pmo)+offset)));
                        sql = util_aasprintf(sql,"%s%c ", term,
                                             (field == (SG_LAST - 1)) ? ')' : ',');
                        sqlite_freemem(term);
                        break;
                    default:
                        DPRINTF(E_FATAL,L_DB,"Unhandled data type in db_add for '%s'\n",
                                ff_field_data[field].name);
                        break;
                    }
                }
            }
            if(!pass)
                sql = util_aasprintf(sql," values (");
        }
    }
    db_sqlite2_unlock();

    if(DB_E_SUCCESS == (err = db_sqlite2_exec(pe, E_FATAL, "%s", sql))) {
        if(!pmo->id)
            pmo->id = (uint32_t)db_sqlite2_insert_id();
    }

    return err;
}

/**
 * Build an error string
 *
 * @param pe error buffer
 * @param error error number
 */
void db_sqlite2_set_error(char **pe, int error, ...) {
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
 * get (or create) the db handle
 */
sqlite *db_sqlite2_handle(void) {
    sqlite *pdb = NULL;
    char *perr;
    char *pe = NULL;

    pdb = (sqlite *)pthread_getspecific(db_sqlite2_key);
    if(pdb == NULL) { /* don't have a handle yet */
        if((pdb = sqlite_open(db_sqlite2_path,0666,&perr)) == NULL) {
            db_sqlite2_set_error(&pe,DB_E_SQL_ERROR,perr);
            DPRINTF(E_FATAL,L_DB,"db_sqlite2_open: %s (%s)\n",perr,
                    db_sqlite2_path);
            sqlite_freemem(perr);
            db_sqlite2_unlock();
            return NULL;
        }
        sqlite_busy_timeout(pdb,30000);  /* 30 seconds */
        pthread_setspecific(db_sqlite2_key,(void*)pdb);
    }

    return pdb;
}

/**
 * free a thread-specific db handle
 */
void db_sqlite2_freedb(sqlite *pdb) {
    db_sqlite2_lock();
    sqlite_close(pdb);
    db_sqlite2_unlock();
}

/**
 * lock the db_mutex
 */
void db_sqlite2_lock(void) {
    int err;

    //    DPRINTF(E_SPAM,L_LOCK,"%08x: entering db_sqlite2_lock\n",util_get_threadid());
    if((err=pthread_mutex_lock(&db_sqlite2_mutex))) {
        DPRINTF(E_FATAL,L_DB,"cannot lock sqlite lock: %s\n",strerror(err));
    }
    //    DPRINTF(E_SPAM,L_LOCK,"%08x: acquired db_sqlite2_lock\n",util_get_threadid());
}

/**
 * unlock the db_mutex
 */
void db_sqlite2_unlock(void) {
    int err;

    //    DPRINTF(E_SPAM,L_LOCK,"%08x: releasing db_sqlite2_lock\n",util_get_threadid());
    if((err=pthread_mutex_unlock(&db_sqlite2_mutex))) {
        DPRINTF(E_FATAL,L_DB,"cannot unlock sqlite2 lock: %s\n",strerror(err));
    }
    //    DPRINTF(E_SPAM,L_LOCK,"%08x: released db_sqlite2_lock\n",util_get_threadid());
}


/**
 * sets the db version of the current database in the db.version
 * file in the cache_dir
 *
 * @param version version to set
 */
void db_sqlite2_set_version(int version) {
    db_sqlite2_exec(NULL,E_DBG,"update config set value=%d where term='version'",
                        version);

}

int db_sqlite2_fetch_int(char **pe, int *ival, char *fmt, ...) {
    void *opaque;
    char **row;
    int err;
    va_list ap;
    char *query;
    int result;

    db_sqlite2_lock();
    va_start(ap,fmt);
    query = sqlite_vmprintf(fmt,ap);
    va_end(ap);
    db_sqlite2_unlock();

    if(DB_E_SUCCESS != (err = db_sqlite2_fetch_row(pe, &opaque, &row, query))) {
        db_sqlite2_lock();
        sqlite_freemem(query);
        db_sqlite2_unlock();
        return err;
    }

    db_sqlite2_lock();
    sqlite_freemem(query);
    db_sqlite2_unlock();

    if(row) {
        result = atoi(row[0]);
        *ival = result;
    } else {
        err = DB_E_NOROWS;
    }

    db_sqlite2_dispose_row(opaque);
    return err;
}

/**
 * returns the db version of the current database.  Returns
 * zero on error.
 *
 * @returns db version
 */
int db_sqlite2_db_version(void) {
    char *pe;
    int version;

    if(DB_E_SUCCESS == db_sqlite2_fetch_int(&pe, &version, "select value from"
                                            " config where term='version'")) {
        return version;
    }

    return 0;
}

/**
 * @param pe error buffer
 * @param ppmo returns the result
 * @return DB_E_SUCCESS on success, error code with pe allocated otherwise
 */
int db_sqlite2_fetch_item(char **pe, uint32_t id, void **opaque, MEDIA_STRING **ppms) {
    return db_sqlite2_fetch_row(pe, opaque, (char***)ppms, "select * from songs where id=%d",id);
}

int db_sqlite2_fetch_row(char **pe, void **opaque, char ***row, char *fmt, ...) {
    DB_SQLITE2_EH *peh;
    va_list ap;
    int err;

    peh = (DB_SQLITE2_EH*)malloc(sizeof(DB_SQLITE2_EH));

    if(!peh) {
        db_sqlite2_set_error(pe, DB_E_MALLOC);
        return DB_E_MALLOC;
    }

    *opaque = peh;

    db_sqlite2_lock();
    va_start(ap,fmt);
    peh->query = sqlite_vmprintf(fmt,ap);
    va_end(ap);

    if(DB_E_SUCCESS != (err = db_sqlite2_enum_begin_helper(pe, *opaque)))
        return err;

    if(DB_E_SUCCESS != (err = db_sqlite2_enum_fetch(pe, *opaque, row))) {
        db_sqlite2_enum_end(NULL, *opaque);
        return err;
    }

    return DB_E_SUCCESS;
}

/**
 * dispose of a row fetched via db_sqlite2_fetch
 *
 * @param ppms media object to destroy
 */
void db_sqlite2_dispose_item(void *opaque, MEDIA_STRING *ppms) {
    db_sqlite2_dispose_row(opaque);
}

void db_sqlite2_dispose_row(void *opaque) {
    if(opaque)
        db_sqlite2_enum_end(NULL,opaque);
}



/**
 * open a sqlite2 database
 *
 * @param dsn the full dns to the database
 *        (sqlite2:/path/to/database)
 *
 * @returns DB_E_SUCCESS on success
 */
int db_sqlite2_open(char **pe, char *dsn) {
    sqlite *pdb;
    char *perr;
    char *db_dir;
    int version;
    int max_version;
    int result;
    pthread_mutexattr_t mutexattr;

    pthread_mutexattr_init(&mutexattr);
    pthread_mutexattr_settype(&mutexattr,PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&db_sqlite2_mutex,&mutexattr);

    db_dir = conf_alloc_string("general","cache_dir",NULL);
    if(!db_dir) {
        db_sqlite2_set_error(pe,DB_E_NOPATH);
        return DB_E_NOPATH;
    }

    pthread_key_create(&db_sqlite2_key, (void*)db_sqlite2_freedb);
    snprintf(db_sqlite2_path,sizeof(db_sqlite2_path),"%s/songs.db",db_dir);
    free(db_dir);

    db_sqlite2_lock();
    pdb=sqlite_open(db_sqlite2_path,0666,&perr);
    if(!pdb) {
        db_sqlite2_set_error(pe,DB_E_SQL_ERROR,perr);
        DPRINTF(E_LOG,L_DB,"db_sqlite2_open: %s (%s)\n",perr,
            db_sqlite2_path);
        sqlite_freemem(perr);
        db_sqlite2_unlock();
        return DB_E_SQL_ERROR;
    }
    sqlite_close(pdb);
    db_sqlite2_unlock();

    version=db_sqlite2_db_version();

    if(!version) {
        /* got to rescan */
        DPRINTF(E_LOG,L_DB,"Can't determine db version.  Forcing a full update\n");
        db_sqlite2_exec(NULL,E_DBG,"drop table songs");
        db_sqlite2_exec(NULL,E_FATAL,db_sqlite2_initial);
        version = DB_SQLITE2_VERSION;
    } else if(version < DB_SQLITE2_VERSION) {
        /* have to do the upgrade */
        max_version = 0;
        while(db_sqlite_updates[max_version]) max_version++;

        DPRINTF(E_DBG,L_DB,"Current db version: %d\n",version);
        DPRINTF(E_DBG,L_DB,"Target db version:  %d\n",max_version);

        while(version < max_version) {
            DPRINTF(E_LOG,L_DB,"Upgrading db: %d --> %d\n",version, version+1);
            result = db_sqlite2_exec(pe,E_LOG,"%s",db_sqlite_updates[version]);
            if(result != DB_E_SUCCESS) {
                DPRINTF(E_FATAL,L_DB,"Error upgrading db: %s\n", pe ? *pe : "?");
                return result;
            }
            version++;
        }
    }

    if(version != DB_SQLITE2_VERSION) {
        /* existing db too new */
        DPRINTF(E_LOG,L_DB,"DB version %d to new.  I only understand %d.  Aborting\n",
                version,DB_SQLITE2_VERSION);
        db_sqlite2_set_error(pe,DB_E_WRONGVERSION);
        return DB_E_WRONGVERSION;
    }

    db_sqlite2_set_version(DB_SQLITE2_VERSION);

    return DB_E_SUCCESS;
}

/**
 * close the database
 */
int db_sqlite2_close(void) {
    return DB_E_SUCCESS;
}

/**
 * execute a throwaway query against the database, disregarding
 * the outcome
 *
 * @param pe db error structure
 * @param loglevel error level to return if the query fails
 * @param fmt sprintf-style arguements
 *
 * @returns DB_E_SUCCESS on success
 */
int db_sqlite2_exec(char **pe, int loglevel, char *fmt, ...) {
    va_list ap;
    char *query;
    int err;
    char *perr;

    db_sqlite2_lock();
    va_start(ap,fmt);
    query=sqlite_vmprintf(fmt,ap);
    va_end(ap);

    DPRINTF(E_DBG,L_DB,"Executing: %s\n",query);

    err=sqlite_exec(db_sqlite2_handle(),query,NULL,NULL,&perr);
    if(err != SQLITE_OK) {
        db_sqlite2_set_error(pe,DB_E_SQL_ERROR,perr);

        DPRINTF(loglevel == E_FATAL ? E_LOG : loglevel,L_DB,"Query: %s\n",
                query);
        DPRINTF(loglevel,L_DB,"Error: %s\n",perr);
        sqlite_freemem(perr);
    } else {
        DPRINTF(E_DBG,L_DB,"Rows: %d\n",sqlite_changes(db_sqlite2_handle()));
    }
    sqlite_freemem(query);

    db_sqlite2_unlock();

    if(err != SQLITE_OK)
        return DB_E_SQL_ERROR;
    return DB_E_SUCCESS;
}


/**
 * start a db enumeration
 */
int db_sqlite2_enum_items_begin(char **pe, void **opaque) {
    return db_sqlite2_enum_begin(pe,opaque,"select * from songs");
}

/**
 * walk a bunch of rows for a specific query
 */
int db_sqlite2_enum_begin(char **pe, void **opaque, char *fmt, ...) {
    DB_SQLITE2_EH *peh;
    va_list ap;

    peh = (DB_SQLITE2_EH*)malloc(sizeof(DB_SQLITE2_EH));
    if(!peh) {
        db_sqlite2_set_error(pe, DB_E_MALLOC);
        return DB_E_MALLOC;
    }

    *opaque = peh;

    db_sqlite2_lock();


    va_start(ap,fmt);
    peh->query = sqlite_vmprintf(fmt,ap);
    va_end(ap);

    return db_sqlite2_enum_begin_helper(pe, *opaque);
}

int db_sqlite2_enum_begin_helper(char **pe, void *opaque) {
    int err;
    char *perr;
    DB_SQLITE2_EH *peh = (DB_SQLITE2_EH*)opaque;

    err=sqlite_compile(db_sqlite2_handle(),peh->query,
                       &peh->tail,&peh->pvm,&perr);
    if(err != SQLITE_OK) {
        peh->pvm = NULL;
        db_sqlite2_set_error(pe,DB_E_SQL_ERROR,perr);
        sqlite_freemem(perr);
        db_sqlite2_enum_end(NULL, peh);
        return DB_E_SQL_ERROR;
    }

    /* otherwise, we leave the db locked while we walk through the enums */
    return DB_E_SUCCESS;
}

int db_sqlite2_enum_items_fetch(char **pe, void *opaque, MEDIA_STRING **ppmo) {
    return db_sqlite2_enum_fetch(pe,opaque,(char***)ppmo);
}

/**
 * fetch the next row
 *
 * @param pe error string, if result isn't DB_E_SUCCESS
 * @param pr pointer to a row struct
 *
 * @returns DB_E_SUCCESS with *pr=NULL when end of table,
 *          DB_E_SUCCESS with a valid row when more data,
 *          DB_E_* on error
 */
int db_sqlite2_enum_fetch(char **pe, void *opaque, char ***row) {
    int err;
    char *perr=NULL;
    const char **colarray;
    int cols;
    int counter=10;
    DB_SQLITE2_EH *peh;

    peh = (DB_SQLITE2_EH*)opaque;

    while(counter--) {
        err=sqlite_step(peh->pvm,&cols,(const char ***)row,&colarray);
        if(err != SQLITE_BUSY)
            break;
        usleep(100);
    }

    if(err == SQLITE_DONE) {
        *row = NULL;
        return DB_E_SUCCESS;
    }

    if(err == SQLITE_ROW) {
        return DB_E_SUCCESS;
    }

    db_sqlite2_set_error(pe,DB_E_SQL_ERROR,perr);
    return DB_E_SQL_ERROR;
}

/**
 * end the db enumeration
 */
int db_sqlite2_enum_end(char **pe, void *opaque) {
    int err;
    char *perr;
    DB_SQLITE2_EH *peh = (DB_SQLITE2_EH*)opaque;

    if(peh) {
        if(peh->query) {
            sqlite_freemem(peh->query);
            peh->query = NULL;
        }

        if(peh->pvm) {
            err = sqlite_finalize(peh->pvm,&perr);
            if(err != SQLITE_OK) {
                db_sqlite2_set_error(pe,DB_E_SQL_ERROR,perr);
                sqlite_freemem(perr);
                db_sqlite2_unlock();
                return DB_E_SQL_ERROR;
            }
        }
        free(peh);
    }

    db_sqlite2_unlock();
    return DB_E_SUCCESS;
}

/**
 * some db actions can be optimized by the db itself.
 * this is particularly true on prescans, when
 * the entire action can be wrapped in a transaction
 *
 * @param hint hint type (@see ff-dbstruct.h)
 */
void db_sqlite2_hint(int hint) {
    return;

    switch(hint) {
    case DB_HINT_PRESCAN_START:
        db_sqlite2_exec(NULL,E_LOG,"PRAGMA synchronous=off;PRAGMA temp_store=MEMORY;BEGIN;");

        break;
    case DB_HINT_PRESCAN_END:
        db_sqlite2_exec(NULL,E_LOG,"END;PRAGMA synchronous=on;");
        break;
    }
}


/**
 * restart the enumeration
 */
int db_sqlite2_enum_restart(char **pe, void *opaque) {
    return db_sqlite2_enum_begin_helper(pe, opaque);
}

/**
 * get the id of the last auto_update inserted item
 *
 * @returns autoupdate value
 */
int db_sqlite2_insert_id(void) {
    return sqlite_last_insert_rowid(db_sqlite2_handle());
}

char *db_sqlite2_initial =
"create table songs (\n"
"   id              INTEGER PRIMARY KEY NOT NULL,\n"      /* 0 */
"   path            VARCHAR(4096) NOT NULL,\n"
"   fname           VARCHAR(255) DEFAULT NULL,\n"
"   title           VARCHAR(1024) DEFAULT NULL,\n"
"   artist          VARCHAR(1024) DEFAULT NULL,\n"
"   album           VARCHAR(1024) DEFAULT NULL,\n"        /* 5 */
"   genre           VARCHAR(255) DEFAULT NULL,\n"
"   comment         VARCHAR(4096) DEFAULT NULL,\n"
"   type            VARCHAR(255) DEFAULT NULL,\n"
"   composer        VARCHAR(1024) DEFAULT NULL,\n"
"   orchestra       VARCHAR(1024) DEFAULT NULL,\n"      /* 10 */
"   conductor       VARCHAR(1024) DEFAULT NULL,\n"
"   grouping        VARCHAR(1024) DEFAULT NULL,\n"
"   url             VARCHAR(1024) DEFAULT NULL,\n"
"   bitrate         INTEGER DEFAULT 0,\n"
"   samplerate      INTEGER DEFAULT 0,\n"               /* 15 */
"   song_length     INTEGER DEFAULT 0,\n"
"   file_size       INTEGER DEFAULT 0,\n"
"   year            INTEGER DEFAULT 0,\n"
"   track           INTEGER DEFAULT 0,\n"
"   total_tracks    INTEGER DEFAULT 0,\n"               /* 20 */
"   disc            INTEGER DEFAULT 0,\n"
"   total_discs     INTEGER DEFAULT 0,\n"
"   bpm             INTEGER DEFAULT 0,\n"
"   compilation     INTEGER DEFAULT 0,\n"
"   rating          INTEGER DEFAULT 0,\n"               /* 25 */
"   play_count      INTEGER DEFAULT 0,\n"
"   data_kind       INTEGER DEFAULT 0,\n"
"   item_kind       INTEGER DEFAULT 0,\n"
"   description     INTEGER DEFAULT 0,\n"
"   time_added      INTEGER DEFAULT 0,\n"               /* 30 */
"   time_modified   INTEGER DEFAULT 0,\n"
"   time_played     INTEGER DEFAULT 0,\n"
"   disabled        INTEGER DEFAULT 0,\n"
"   sample_count    INTEGER DEFAULT 0,\n"
"   codectype       VARCHAR(5) DEFAULT NULL,\n"         /* 35 */
"   idx             INTEGER NOT NULL,\n"
"   has_video       INTEGER DEFAULT 0,\n"
"   contentrating   INTEGER DEFAULT 0,\n"
"   bits_per_sample INTEGER DEFAULT 0,\n"
"   album_artist    VARCHAR(1024)\n"                    /* 40 */
");\n"
"create table config (\n"
"   term            VARCHAR(255)    NOT NULL,\n"
"   subterm         VARCHAR(255)    DEFAULT NULL,\n"
"   value           VARCHAR(1024)   NOT NULL\n"
");\n"
"insert into config values ('version','','14');\n";
