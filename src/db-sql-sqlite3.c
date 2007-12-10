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
 * This file handles sqlite3 databases.  SQLite3 databases
 * should have a dsn of:
 *
 * sqlite3:/path/to/folder
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
#include <sqlite3.h>
#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include "daapd.h"
#include "conf.h"
#include "err.h"
#include "db.h"
#include "db-sql-sqlite3.h"
#include "util.h"

#ifndef TRUE
#  define TRUE 1
#  define FALSE 0
#endif


/* Globals */
static pthread_mutex_t db_sqlite3_mutex = PTHREAD_MUTEX_INITIALIZER; /**< sqlite not reentrant */
static sqlite3_stmt *db_sqlite3_stmt;
static const char *db_sqlite3_ptail;
static char **db_sqlite3_row = NULL;
static pthread_key_t db_sqlite3_key;
static char db_sqlite3_path[PATH_MAX + 1];

#define DB_SQLITE3_VERSION 13


/* Forwards */
static void db_sqlite3_lock(void);
static void db_sqlite3_unlock(void);
extern char *db_sqlite3_initial;
static int db_sqlite3_enum_begin_helper(char **pe);
static int db_sqlite3_exec(char **pe, int loglevel, char *fmt, ...);



int db_sqlite3_add(char **pe, MEDIA_NATIVE *pmo) {
    return DB_E_SUCCESS;
}

/**
 * get (or create) the db handle
 */
sqlite3 *db_sqlite3_handle(void) {
    sqlite3 *pdb = NULL;
    char *pe = NULL;

    pdb = (sqlite3*)pthread_getspecific(db_sqlite3_key);
    if(pdb == NULL) { /* don't have a handle yet */
        DPRINTF(E_DBG,L_DB,"Creating new db handle\n");
        if(sqlite3_open(db_sqlite3_path,&pdb) != SQLITE_OK) {
            db_sqlite3_set_error(&pe,DB_E_SQL_ERROR,sqlite3_errmsg(pdb));
            DPRINTF(E_FATAL,L_DB,"db_sqlite3_open: %s (%s)\n",pe,db_sqlite3_path);
            db_sqlite3_unlock();
            return NULL;
        }
        sqlite3_busy_timeout(pdb,30000);  /* 30 seconds */
        pthread_setspecific(db_sqlite3_key,(void*)pdb);
    }

    return pdb;
}

/**
 * Build an error string
 *
 * @param pe error buffer
 * @param error error number
 */
void db_sqlite3_set_error(char **pe, int error, ...) {
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
 * free a thread-specific db handle
 */
void db_sqlite3_freedb(sqlite3 *pdb) {
    sqlite3_close(pdb);
}

/**
 * lock the db_mutex
 */
void db_sqlite3_lock(void) {
    int err;

    if((err=pthread_mutex_lock(&db_sqlite3_mutex))) {
        DPRINTF(E_FATAL,L_DB,"cannot lock sqlite lock: %s\n",strerror(err));
    }
}

/**
 * unlock the db_mutex
 */
void db_sqlite3_unlock(void) {
    int err;

    if((err=pthread_mutex_unlock(&db_sqlite3_mutex))) {
        DPRINTF(E_FATAL,L_DB,"cannot unlock sqlite3 lock: %s\n",strerror(err));
    }
}

/**
 *
 */
char *db_sqlite3_vmquery(char *fmt,va_list ap) {
    return sqlite3_vmprintf(fmt,ap);
}

/**
 *
 */
void db_sqlite3_vmfree(char *query) {
    sqlite3_free(query);
}

/**
 * returns the db version of the current database
 *
 * @returns db version
 */
int db_sqlite3_db_version(void) {
    return 0;
}

/**
 * open a sqlite3 database
 *
 * @param dsn the full dns to the database
 *        (sqlite3:/path/to/database)
 *
 * @returns DB_E_SUCCESS on success
 */
int db_sqlite3_open(char **pe, char *dsn) {
    sqlite3 *pdb;

    pthread_key_create(&db_sqlite3_key, (void*)db_sqlite3_freedb);
    snprintf(db_sqlite3_path,sizeof(db_sqlite3_path),"%s/songs3.db",dsn);

    db_sqlite3_lock();
    if(sqlite3_open(db_sqlite3_path,&pdb) != SQLITE_OK) {
        db_sqlite2_set_error(pe,DB_E_SQL_ERROR,sqlite3_errmsg(pdb));
        DPRINTF(E_LOG,L_DB,"db_sqlite3_open: %s (%s)\n",pe ? *pe : "Unknown",
            db_sqlite3_path);
        db_sqlite3_unlock();
        return DB_E_SQL_ERROR;
    }
    sqlite3_close(pdb);
    db_sqlite3_unlock();

    if(db_sqlite3_db_version() != DB_SQLITE3_VERSION) {
        db_sqlite3_exec(NULL,E_DBG,"drop table songs");
        db_sqlite3_exec(NULL,E_FATAL,db_sqlite3_initial);
    }

    return DB_E_SUCCESS;
}

/**
 * close the database
 */
int db_sqlite3_close(void) {
    /* this doens't actually make much sense, as the closes get done by the threads */
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
int db_sqlite3_exec(char **pe, int loglevel, char *fmt, ...) {
    va_list ap;
    char *query;
    int err;
    char *perr;

    db_sqlite3_lock();

    va_start(ap,fmt);
    query=sqlite3_vmprintf(fmt,ap);
    va_end(ap);

    DPRINTF(E_DBG,L_DB,"Executing: %s\n",query);

    err=sqlite3_exec(db_sqlite3_handle(),query,NULL,NULL,&perr);
    if(err != SQLITE_OK) {
        db_sqlite2_set_error(pe,DB_E_SQL_ERROR,perr);

        DPRINTF(loglevel == E_FATAL ? E_LOG : loglevel,L_DB,"Query: %s\n",
                query);
        DPRINTF(loglevel,L_DB,"Error: %s\n",perr);
        sqlite3_free(perr);
    } else {
        DPRINTF(E_DBG,L_DB,"Rows: %d\n",sqlite3_changes(db_sqlite3_handle()));
    }
    sqlite3_free(query);

    db_sqlite3_unlock();

    if(err != SQLITE_OK)
        return DB_E_SQL_ERROR;
    return DB_E_SUCCESS;
}

/**
 * start enumerating rows in a select
 */
int db_sqlite3_enum_begin(char **pe) {
    db_sqlite3_lock();
    return db_sqlite3_enum_begin_helper(pe);
}

int db_sqlite3_enum_begin_helper(char **pe) {
    int err;

    DPRINTF(E_DBG,L_DB,"Executing: select * from songs\n");
    err=sqlite3_prepare(db_sqlite3_handle(),"select * from songs",-1,
                        &db_sqlite3_stmt,&db_sqlite3_ptail);

    if(err != SQLITE_OK) {
        db_sqlite2_set_error(pe,DB_E_SQL_ERROR,sqlite3_errmsg(db_sqlite3_handle()));
        db_sqlite3_unlock();
        return DB_E_SQL_ERROR;
    }

    DPRINTF(E_SPAM,L_DB,"Prepared statement: %08X\n",db_sqlite3_stmt);

    /* otherwise, we leave the db locked while we walk through the enums */
    if(db_sqlite3_row)
        free(db_sqlite3_row);
    db_sqlite3_row=NULL;

    return DB_E_SUCCESS;

}

/**
 * fetch the next row.  This will return DB_E_SUCCESS if it got a
 * row, or it's done.  If it's done, the row will be empty, otherwise
 * it will be full of data.  Either way, if fetch fails, you must close.
 *
 * @param pe error string, if result isn't DB_E_SUCCESS
 * @param pr pointer to a row struct
 *
 * @returns DB_E_SUCCESS with *pr=NULL when end of table,
 *          DB_E_SUCCESS with a valid row when more data,
 *          DB_E_* on error
 */
int db_sqlite3_enum_fetch(char **pe, MEDIA_STRING **ppmo) {
    int err;
    int cols;
    int idx;
    int counter=10;
    char ***pr = (char ***)ppmo;

    while(counter--) {
        DPRINTF(E_SPAM,L_DB,"Fetching statement: %08X\n",db_sqlite3_stmt);
        err=sqlite3_step(db_sqlite3_stmt);
        if(err != SQLITE_BUSY)
            break;
        usleep(1000);
    }

    if(err == SQLITE_DONE) {
        *pr = NULL;
        if(db_sqlite3_row)
            free(db_sqlite3_row);
        db_sqlite3_row = NULL;
        return DB_E_SUCCESS;
    }

    if(err == SQLITE_ROW) {
        DPRINTF(E_SPAM,L_DB,"Got row\n");
        cols = sqlite3_column_count(db_sqlite3_stmt);

        if(!db_sqlite3_row) {
            /* gotta alloc space */
            db_sqlite3_row = (char**)malloc((sizeof(char*)) * cols);
            if(!db_sqlite3_row)
                DPRINTF(E_FATAL,L_DB,"Malloc error\n");
        }

        for(idx=0; idx < cols; idx++) {
            db_sqlite3_row[idx] = (char*) sqlite3_column_blob(db_sqlite3_stmt,idx);
        }

        *pr = db_sqlite3_row;
        return DB_E_SUCCESS;
    }

    if(db_sqlite3_row)
        free(db_sqlite3_row);
    db_sqlite3_row = NULL;

    db_sqlite2_set_error(pe,DB_E_SQL_ERROR,sqlite3_errmsg(db_sqlite3_handle()));
    sqlite3_finalize(db_sqlite3_stmt);

    return DB_E_SQL_ERROR;
}

/**
 * end the db enumeration
 */
int db_sqlite3_enum_end(char **pe) {
    int err;

    if(db_sqlite3_row)
        free(db_sqlite3_row);
    db_sqlite3_row = NULL;

    err = sqlite3_finalize(db_sqlite3_stmt);
    if(err != SQLITE_OK) {
        db_sqlite2_set_error(pe,DB_E_SQL_ERROR,sqlite3_errmsg(db_sqlite3_handle()));
        db_sqlite3_unlock();
        return DB_E_SQL_ERROR;
    }

    db_sqlite3_unlock();
    return DB_E_SUCCESS;
}

/**
 * restart the enumeration
 */
int db_sqlite3_enum_restart(char **pe) {
    return db_sqlite3_enum_begin_helper(pe);
}

/**
 * get the id of the last auto_update inserted item
 *
 * @returns autoupdate value
 */

int db_sqlite3_insert_id(void) {
    int result;

    db_sqlite3_lock();
    result = (int)sqlite3_last_insert_rowid(db_sqlite3_handle());
    db_sqlite3_unlock();

    return result;
}



char *db_sqlite3_initial =
"create table songs (\n"
"   id              INTEGER PRIMARY KEY NOT NULL,\n"
"   path            VARCHAR(4096) NOT NULL,\n"
"   fname           VARCHAR(255) NOT NULL,\n"
"   title           VARCHAR(1024) DEFAULT NULL,\n"
"   artist          VARCHAR(1024) DEFAULT NULL,\n"
"   album           VARCHAR(1024) DEFAULT NULL,\n"
"   genre           VARCHAR(255) DEFAULT NULL,\n"
"   comment         VARCHAR(4096) DEFAULT NULL,\n"
"   type            VARCHAR(255) DEFAULT NULL,\n"
"   composer        VARCHAR(1024) DEFAULT NULL,\n"
"   orchestra       VARCHAR(1024) DEFAULT NULL,\n"
"   conductor       VARCHAR(1024) DEFAULT NULL,\n"
"   grouping        VARCHAR(1024) DEFAULT NULL,\n"
"   url             VARCHAR(1024) DEFAULT NULL,\n"
"   bitrate         INTEGER DEFAULT 0,\n"
"   samplerate      INTEGER DEFAULT 0,\n"
"   song_length     INTEGER DEFAULT 0,\n"
"   file_size       INTEGER DEFAULT 0,\n"
"   year            INTEGER DEFAULT 0,\n"
"   track           INTEGER DEFAULT 0,\n"
"   total_tracks    INTEGER DEFAULT 0,\n"
"   disc            INTEGER DEFAULT 0,\n"
"   total_discs     INTEGER DEFAULT 0,\n"
"   bpm             INTEGER DEFAULT 0,\n"
"   compilation     INTEGER DEFAULT 0,\n"
"   rating          INTEGER DEFAULT 0,\n"
"   play_count      INTEGER DEFAULT 0,\n"
"   data_kind       INTEGER DEFAULT 0,\n"
"   item_kind       INTEGER DEFAULT 0,\n"
"   description     INTEGER DEFAULT 0,\n"
"   time_added      INTEGER DEFAULT 0,\n"
"   time_modified   INTEGER DEFAULT 0,\n"
"   time_played     INTEGER DEFAULT 0,\n"
"   db_timestamp    INTEGER DEFAULT 0,\n"
"   disabled        INTEGER DEFAULT 0,\n"
"   sample_count    INTEGER DEFAULT 0,\n"
"   force_update    INTEGER DEFAULT 0,\n"
"   codectype       VARCHAR(5) DEFAULT NULL,\n"
"   idx             INTEGER NOT NULL,\n"
"   has_video       INTEGER DEFAULT 0,\n"
"   contentrating   INTEGER DEFAULT 0,\n"
"   bits_per_sample INTEGER DEFAULT 0,\n"
"   album_artist    VARCHAR(1024)\n"
");\n";
