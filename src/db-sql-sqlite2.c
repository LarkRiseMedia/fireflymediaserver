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

/*
 * This file handles sqlite2 databases.  SQLite2 databases
 * should have a dsn of:
 *
 * sqlite2:/path/to/folder
 *
 * The actual db will be appended to the passed path.
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


/* Globals */
static pthread_mutex_t db_sqlite2_mutex = PTHREAD_MUTEX_INITIALIZER; /**< sqlite not reentrant */
static sqlite_vm *db_sqlite2_pvm;
static pthread_key_t db_sqlite2_key;

static char db_sqlite2_path[PATH_MAX + 1];

#define DB_SQLITE2_VERSION 13


/* Forwards */
static void db_sqlite2_lock(void);
static void db_sqlite2_unlock(void);
extern char *db_sqlite2_initial;
static int db_sqlite2_enum_begin_helper(char **pe);
static int db_sqlite2_exec(char **pe, int loglevel, char *fmt, ...);
static int db_sqlite2_insert_id(void);

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
                sql = util_aasprintf(sql,"%s = %ll%c ",ff_field_data[field].name,
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
                        sql = util_aasprintf(sql,"%ll%c ",
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

    DPRINTF(E_DBG,L_DB,"Executing sql: %s\n",sql);

    return DB_E_SUCCESS;
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
    sqlite_close(pdb);
}

/**
 * lock the db_mutex
 */
void db_sqlite2_lock(void) {
    int err;

    //    DPRINTF(E_SPAM,L_LOCK,"entering db_sqlite2_lock\n");
    if((err=pthread_mutex_lock(&db_sqlite2_mutex))) {
        DPRINTF(E_FATAL,L_DB,"cannot lock sqlite lock: %s\n",strerror(err));
    }
    //    DPRINTF(E_SPAM,L_LOCK,"acquired db_sqlite2_lock\n");
}

/**
 * unlock the db_mutex
 */
void db_sqlite2_unlock(void) {
    int err;

    //    DPRINTF(E_SPAM,L_LOCK,"releasing db_sqlite2_lock\n");
    if((err=pthread_mutex_unlock(&db_sqlite2_mutex))) {
        DPRINTF(E_FATAL,L_DB,"cannot unlock sqlite2 lock: %s\n",strerror(err));
    }
    //    DPRINTF(E_SPAM,L_LOCK,"released db_sqlite2_lock\n");
}


/**
 * returns the db version of the current database
 *
 * @returns db version
 */
int db_sqlite2_db_version(void) {
    return 0;
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

    pthread_key_create(&db_sqlite2_key, (void*)db_sqlite2_freedb);
    snprintf(db_sqlite2_path,sizeof(db_sqlite2_path),"%s/songs.db",dsn);

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

    if(db_sqlite2_db_version() != DB_SQLITE2_VERSION) {
        /* got to rescan */
        db_sqlite2_exec(NULL,E_DBG,"drop table songs");
        db_sqlite2_exec(NULL,E_FATAL,db_sqlite2_initial);
    }

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

    va_start(ap,fmt);
    query=sqlite_vmprintf(fmt,ap);
    va_end(ap);

    DPRINTF(E_DBG,L_DB,"Executing: %s\n",query);

    db_sqlite2_lock();
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
 * start enumerating rows in a select
 */
int db_sqlite2_enum_begin(char **pe) {
    db_sqlite2_lock();
    return db_sqlite2_enum_begin_helper(pe);
}

int db_sqlite2_enum_begin_helper(char **pe) {
    int err;
    char *perr;
    const char *ptail;

    DPRINTF(E_DBG,L_DB,"Executing: select * from songs\n");
    err=sqlite_compile(db_sqlite2_handle(),"select * from songs",
                       &ptail,&db_sqlite2_pvm,&perr);

    if(err != SQLITE_OK) {
        db_sqlite2_set_error(pe,DB_E_SQL_ERROR,perr);
        sqlite_freemem(perr);
        db_sqlite2_unlock();
        return DB_E_SQL_ERROR;
    }

    /* otherwise, we leave the db locked while we walk through the enums */
    return DB_E_SUCCESS;
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
int db_sqlite2_enum_fetch(char **pe, MEDIA_STRING **ppmo) {
    int err;
    char *perr=NULL;
    const char **colarray;
    int cols;
    int counter=10;
    const char ***pr = (const char ***)ppmo;

    while(counter--) {
        err=sqlite_step(db_sqlite2_pvm,&cols,(const char ***)pr,&colarray);
        if(err != SQLITE_BUSY)
            break;
        usleep(100);
    }

    if(err == SQLITE_DONE) {
        *pr = NULL;
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
int db_sqlite2_enum_end(char **pe) {
    int err;
    char *perr;

    err = sqlite_finalize(db_sqlite2_pvm,&perr);
    if(err != SQLITE_OK) {
        db_sqlite2_set_error(pe,DB_E_SQL_ERROR,perr);
        sqlite_freemem(perr);
        db_sqlite2_unlock();
        return DB_E_SQL_ERROR;
    }

    db_sqlite2_unlock();
    return DB_E_SUCCESS;
}

/**
 * restart the enumeration
 */
int db_sqlite2_enum_restart(char **pe) {
    return db_sqlite2_enum_begin_helper(pe);
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
"   fname           VARCHAR(255) NOT NULL,\n"
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
"   db_timestamp    INTEGER DEFAULT 0,\n"
"   disabled        INTEGER DEFAULT 0,\n"
"   sample_count    INTEGER DEFAULT 0,\n"               /* 35 */
"   force_update    INTEGER DEFAULT 0,\n"
"   codectype       VARCHAR(5) DEFAULT NULL,\n"
"   idx             INTEGER NOT NULL,\n"
"   has_video       INTEGER DEFAULT 0,\n"
"   contentrating   INTEGER DEFAULT 0,\n"                /* 40 */
"   bits_per_sample INTEGER DEFAULT 0,\n"
"   album_artist    VARCHAR(1024)\n"
");\n";

