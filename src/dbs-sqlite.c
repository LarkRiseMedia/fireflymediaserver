/*
 * $Id$
 * sqlite-specific db implementation
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

#define _XOPEN_SOURCE 600

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite.h>

#include "err.h"
#include "mp3-scanner.h"
#include "db-generic.h"
#include "dbs-sqlite.h"

/* Globals */
static sqlite *db_sqlite_songs; /**< Database that holds the mp3 info */
static pthread_mutex_t db_sqlite_mutex = PTHREAD_MUTEX_INITIALIZER; /**< sqlite not reentrant */
static sqlite_vm *db_sqlite_pvm;
static int db_sqlite_in_scan=0;

/* Forwards */
int db_sqlite_get_size(DBQUERYINFO *pinfo, char **valarray);
int db_sqlite_build_dmap(DBQUERYINFO *pinfo, char **valarray, char *presult, int len);
void db_sqlite_build_mp3file(char **valarray, MP3FILE *pmp3);

#define STR(a) (a) ? (a) : ""
#define MAYBEFREE(a) { if((a)) free((a)); };

/**
 * lock the db_mutex
 */
void db_sqlite_lock(void) {
    int err;

    if((err=pthread_mutex_lock(&db_sqlite_mutex))) {
	DPRINTF(E_FATAL,L_DB,"cannot lock sqlite lock: %s\n",strerror(err));
    }
}

/**
 * unlock the db_mutex
 */
int db_sqlite_unlock(void) {
    return pthread_mutex_unlock(&db_sqlite_mutex);
}


/**
 * open sqlite database
 */
int db_sqlite_open(char *parameters) {
    char db_path[PATH_MAX + 1];
    char *perr;
    
    snprintf(db_path,sizeof(db_path),"%s/songs.db",parameters);
    
    db_sqlite_lock();
    db_sqlite_songs=sqlite_open(db_path,0,&perr);
    if(!db_sqlite_songs)
	DPRINTF(E_FATAL,L_DB,"db_sqlite_open: %s (%s)\n",perr,db_path);

    db_sqlite_unlock();
    return 0;
}

/**
 * initialize the sqlite database, reloading if requested
 *
 * \param reload whether or not to do a full reload on the db
 */
int db_sqlite_init(int reload) {
    int err;
    char *perr;

    if(reload) {
	/* this may or may not fail, depending if the index is already in place */
	db_sqlite_lock();
	sqlite_exec(db_sqlite_songs,"DROP INDEX idx_path",NULL,NULL,&perr);
	err=sqlite_exec(db_sqlite_songs,"DELETE FROM songs",NULL,NULL,&perr);
	db_sqlite_unlock();
	if(err != SQLITE_OK) {
	    DPRINTF(E_FATAL,L_DB,"Cannot reaload tables: %s\n",perr);
	}
    }

    return 0;
}

/**
 * close the database
 */
int db_sqlite_deinit(void) {
    db_sqlite_lock();
    sqlite_close(db_sqlite_songs);
    db_sqlite_unlock();

    return 0;
}


/**
 * start a background scan
 */
int db_sqlite_start_scan(void) {
    char *perr;
    int err;

    db_sqlite_lock();
    err=sqlite_exec(db_sqlite_songs,"UPDATE songs SET updated=0",NULL,NULL,&perr);
    db_sqlite_unlock();

    if(err != SQLITE_OK)
	DPRINTF(E_FATAL,L_DB,"db_sqlite_start_scan: %s\n",perr);
    
    db_sqlite_in_scan=1;

    return 0;
}

/**
 * stop a db scan
 */
int db_sqlite_end_scan(void) {
    char *perr;
    int err;

    db_sqlite_lock();
    sqlite_exec(db_sqlite_songs,"PRAGMA synchronous=NORMAL",NULL,NULL,&perr);
    err=sqlite_exec(db_sqlite_songs,"DELETE FROM songs WHERE updated=0",NULL,NULL,&perr);
    db_sqlite_unlock();

    if(err != SQLITE_OK)
	DPRINTF(E_FATAL,L_DB,"db_sqlite_end_scan: %s\n",perr);
    
    db_sqlite_in_scan=0;
    return 0;
}


/**
 * add a database item
 *
 * \param pmp3 mp3 file to add
 */
int db_sqlite_add(MP3FILE *pmp3) {
    int err;
    char *perr;

    DPRINTF(E_SPAM,L_DB,"Entering db_sqlite_add\n");

    if(!pmp3->time_added)
	pmp3->time_added = (int)time(NULL);
    
    if(!pmp3->time_modified)
	pmp3->time_modified = (int)time(NULL);

    pmp3->db_timestamp = (int)time(NULL);
    pmp3->play_count=0;
    pmp3->time_played=0;

    db_sqlite_lock();
    err=sqlite_exec_printf(db_sqlite_songs,"INSERT INTO songs VALUES"
			   "(NULL,"   // id
			   "'%q',"  // path
			   "'%q',"  // fname
			   "'%q',"  // title
			   "'%q',"  // artist
			   "'%q',"  // album
			   "'%q',"  // genre
			   "'%q',"  // comment
			   "'%q',"  // type
			   "'%q',"  // composer
			   "'%q',"  // orchestra
			   "'%q',"  // conductor
			   "'%q',"  // grouping
			   "'%q',"  // url
			   "%d,"    // bitrate
			   "%d,"    // samplerate
			   "%d,"    // song_length
			   "%d,"    // file_size
			   "%d,"    // year
			   "%d,"    // track
			   "%d,"    // total_tracks
			   "%d,"    // disc
			   "%d,"    // total_discs
			   "%d,"    // bpm
			   "%d,"    // compilation
			   "%d,"    // rating
			   "0,"     // play_count
			   "%d,"    // data_kind
			   "%d,"    // item_kind
			   "'%q',"  // description
			   "%d,"    // time_added
			   "%d,"    // time_modified
			   "%d,"    // time_played
			   "%d,"    // db_timestamp
			   "%d,"    // disabled
			   "1,"     // updated
			   "0)",    // force_update    
			   NULL,NULL,
			   &perr,
			   STR(pmp3->path),
			   STR(pmp3->fname),
			   STR(pmp3->title),
			   STR(pmp3->artist),
			   STR(pmp3->album),
			   STR(pmp3->genre),
			   STR(pmp3->comment),
			   STR(pmp3->type),
			   STR(pmp3->composer),
			   STR(pmp3->orchestra),
			   STR(pmp3->conductor),
			   STR(pmp3->grouping),
			   STR(pmp3->url),
			   pmp3->bitrate,
			   pmp3->samplerate,
			   pmp3->song_length,
			   pmp3->file_size,
			   pmp3->year,
			   pmp3->track,
			   pmp3->total_tracks,
			   pmp3->disc,
			   pmp3->total_discs,
			   pmp3->bpm,
			   pmp3->compilation,
			   pmp3->rating,
			   pmp3->data_kind,
			   pmp3->item_kind,
			   STR(pmp3->description),
			   pmp3->time_added,
			   pmp3->time_modified,
			   pmp3->time_played,
			   pmp3->db_timestamp,
			   pmp3->disabled);
    db_sqlite_unlock();
    if(err != SQLITE_OK)
	DPRINTF(E_FATAL,L_DB,"Error inserting file %s in database: %s\n",
		pmp3->fname,perr);

    DPRINTF(E_SPAM,L_DB,"Exiting db_sqlite_add\n");
    return 0;
}

/**
 * update a database item
 *
 * \param pmp3 mp3 file to update
 */
int db_sqlite_update(MP3FILE *pmp3) {
    int err;
    char *perr;

    if(!pmp3->time_modified)
	pmp3->time_modified = (int)time(NULL);

    pmp3->db_timestamp = (int)time(NULL);

    db_sqlite_lock();
    err=sqlite_exec_printf(db_sqlite_songs,"UPDATE songs SET "
			   "title='%q',"  // title
			   "artist='%q',"  // artist
			   "album='%q',"  // album
			   "genre='%q',"  // genre
			   "comment='%q',"  // comment
			   "type='%q',"  // type
			   "composer='%q',"  // composer
			   "orchestra='%q',"  // orchestra
			   "conductor='%q',"  // conductor
			   "grouping='%q',"  // grouping
			   "url='%q',"  // url
			   "bitrate=%d,"    // bitrate
			   "samplerate=%d,"    // samplerate
			   "song_length=%d,"    // song_length
			   "file_size=%d,"    // file_size
			   "year=%d,"    // year
			   "track=%d,"    // track
			   "total_tracks=%d,"    // total_tracks
			   "disc=%d,"    // disc
			   "total_discs=%d,"    // total_discs
			   "time_modified=%d,"    // time_modified
			   "db_timestamp=%d,"    // db_timestamp
			   "bpm=%d,"    // bpm
			   "compilation=%d,"    // compilation
			   "rating=%d,"    // rating
			   "updated=1"     // updated
			   " WHERE path='%q'",
			   NULL,NULL,
			   &perr,
			   STR(pmp3->title),
			   STR(pmp3->artist),
			   STR(pmp3->album),
			   STR(pmp3->genre),
			   STR(pmp3->comment),
			   STR(pmp3->type),
			   STR(pmp3->composer),
			   STR(pmp3->orchestra),
			   STR(pmp3->conductor),
			   STR(pmp3->grouping),
			   STR(pmp3->url),
			   pmp3->bitrate,
			   pmp3->samplerate,
			   pmp3->song_length,
			   pmp3->file_size,
			   pmp3->year,
			   pmp3->track,
			   pmp3->total_tracks,
			   pmp3->disc,
			   pmp3->total_discs,
			   pmp3->time_modified,
			   pmp3->db_timestamp,
			   pmp3->bpm,
			   pmp3->compilation,
			   pmp3->rating,
			   pmp3->path);
    db_sqlite_unlock();
    if(err != SQLITE_OK)
	DPRINTF(E_FATAL,L_DB,"Error updating file %s in database: %s\n",
		pmp3->fname,perr);

    return 0;
}


/**
 * Update the playlist item counts
 */
int db_sqlite_update_playlists(void) {
    int err;
    char *perr;
    char **resarray;
    int rows, cols, index;
    char query[1204];
    
    db_sqlite_lock();
    err=sqlite_get_table(db_sqlite_songs,"select * from playlists",&resarray,&rows,&cols,&perr);
    if(err != SQLITE_OK) {
	DPRINTF(E_FATAL,L_DB,"Cannot select from playlists: %s\n",perr);
    }
    db_sqlite_unlock();

    for(index=1;index <= rows; index ++) {
	DPRINTF(E_DBG,L_DB,"Updating playlist counts for %s\n",resarray[cols * index + 1]);
	if(atoi(resarray[cols * index + 2])) { // is a smart playlist
	    snprintf(query,sizeof(query),"UPDATE playlists SET items=(SELECT COUNT(*) "
		     "FROM songs WHERE %s) WHERE id=%s",resarray[cols * index + 4],
		     resarray[cols * index]);
	} else {
	    snprintf(query,sizeof(query),"UPDATE playlists SET items=(SELECT COUNT(*) "
		     "FROM playlistitems WHERE id=%s) WHERE id=%s",
		     resarray[cols * index], resarray[cols * index]);
	}

	db_sqlite_lock();
	sqlite_exec(db_sqlite_songs,query,NULL,NULL,&perr);
	db_sqlite_unlock();
    }


    db_sqlite_lock();
    sqlite_free_table(resarray);
    db_sqlite_unlock();

    return 0;
}


/**
 * start enum based on the DBQUERYINFO struct passed
 *
 * \param pinfo DBQUERYINFO struct detailing what to enum
 */
int db_sqlite_enum_start(DBQUERYINFO *pinfo) {
    char scratch[4096];
    char query[4096];
    char query_select[255];
    char query_count[255];
    char query_rest[4096];

    int is_smart;
    int have_clause=0;
    int err;
    char *perr;
    char **resarray;
    int rows, cols;
    int browse=0;
    int results;
    
    const char *ptail;

    query[0] = '\0';
    query_select[0] = '\0';
    query_count[0] = '\0';
    query_rest[0] = '\0';

    switch(pinfo->query_type) {
    case queryTypeItems:
	strcpy(query_select,"SELECT * FROM songs ");
	strcpy(query_count,"SELECT COUNT(*) FROM songs ");
	break;

    case queryTypePlaylists:
	strcpy(query_select,"SELECT * FROM playlists ");
	strcpy(query_count,"SELECT COUNT (*) FROM playlists ");
	break;

    case queryTypePlaylistItems:  /* Figure out if it's smart or dull */
	db_sqlite_lock();
	sprintf(scratch,"SELECT smart,query FROM playlists WHERE id=%d",pinfo->playlist_id);
	DPRINTF(E_DBG,L_DB,"Executing %s\n",scratch);
	err=sqlite_get_table(db_sqlite_songs,scratch,&resarray,&rows,&cols,&perr);
	if(err != SQLITE_OK) {
	    DPRINTF(E_LOG,L_DB|L_DAAP,"Error: %s\n",perr);
	    db_sqlite_unlock();
	    return -1;
	}
	is_smart=atoi(resarray[2]); 
	have_clause=1;
	if(is_smart) {
	    sprintf(query_select,"SELECT * FROM songs ");
	    sprintf(query_count,"SELECT COUNT(id) FROM songs ");
	    sprintf(query_rest,"WHERE (%s)",resarray[3]);
	} else {
	    sprintf(query_select,"SELECT * FROM songs ");
	    sprintf(query_count,"SELECT COUNT(id) FROM songs ");
	    sprintf(query_rest,"WHERE (id IN (SELECT songid FROM playlistitems WHERE id=%d))",
		    pinfo->playlist_id);
	}
	sqlite_free_table(resarray);
	db_sqlite_unlock();
	break;

	/* Note that sqlite doesn't support COUNT(DISTINCT x) */
    case queryTypeBrowseAlbums:
	strcpy(query_select,"SELECT DISTINCT album FROM songs ");
	strcpy(query_count,"SELECT COUNT(album) FROM (SELECT DISTINCT album FROM songs ");
	browse=1;
	break;

    case queryTypeBrowseArtists:
	strcpy(query_select,"SELECT DISTINCT artist FROM songs ");
	strcpy(query_count,"SELECT COUNT(artist) FROM (SELECT DISTINCT artist FROM songs ");
	browse=1;
	break;

    case queryTypeBrowseGenres:
	strcpy(query_select,"SELECT DISTINCT genre FROM songs ");
	strcpy(query_count,"SELECT COUNT(genre) FROM (SELECT DISTINCT genre FROM songs ");
	browse=1;
	break;

    case queryTypeBrowseComposers:
	strcpy(query_select,"SELECT DISTINCT composer FROM songs ");
	strcpy(query_count,"SELECT COUNT(composer) FROM (SELECT DISTINCT composer FROM songs ");
	browse=1;
	break;
    default:
	DPRINTF(E_LOG,L_DB|L_DAAP,"Unknown query type\n");
	return -1;
    }

    /* Apply the query/filter */
    if(pinfo->whereclause) {
	if(have_clause)
	    strcat(query_rest," AND ");

	strcat(query_rest,"(");
	strcat(query_rest,pinfo->whereclause);
	strcat(query_rest,")");
    }

    /* find out how many hits */
    strcpy(scratch,query_count);
    strcat(scratch,query_rest);
    if(browse) 
	strcat(scratch,")");

    DPRINTF(E_DBG,L_DB,"result count query: %s\n",scratch);

    db_sqlite_lock();
    err=sqlite_get_table(db_sqlite_songs,scratch,&resarray,&rows,&cols,&perr);
    if(err != SQLITE_OK) {
	db_sqlite_unlock();
	DPRINTF(E_LOG,L_DB,"Error in results query: %s\n",perr);
	return -1;
    }

    results=atoi(resarray[1]);
    sqlite_free_table(resarray);
    db_sqlite_unlock();

    /* update the playlist counts */
    if(pinfo->query_type == queryTypePlaylistItems) {
	sprintf(scratch,"UPDATE playlists SET items=%d WHERE id=%d",
		results,pinfo->playlist_id);
	db_sqlite_lock();
	sqlite_exec(db_sqlite_songs,scratch,NULL,NULL,&perr);
	db_sqlite_unlock();
    }
    
    DPRINTF(E_DBG,L_DB,"Number of results: %d\n",results);

    strcpy(query,query_select);
    strcat(query,query_rest);

    /* Apply any index */
    switch(pinfo->index_type) {
    case indexTypeFirst:
	sprintf(scratch," LIMIT %d",pinfo->index_high);
	break;
    case indexTypeLast:
	if(pinfo->index_low >= results) {
	    sprintf(scratch," LIMIT %d",pinfo->index_low); /* unnecessary */
	} else {
	    sprintf(scratch," LIMIT %d OFFSET %d",pinfo->index_low, results=pinfo->index_low);
	}
	break;
    case indexTypeSub:
	sprintf(scratch," LIMIT %d OFFSET %d",pinfo->index_high - pinfo->index_low,
		pinfo->index_low);
	break;
    case indexTypeNone:
	break;
    default:
	DPRINTF(E_LOG,L_DB,"Bad indexType: %d\n",(int)pinfo->index_type);
	scratch[0]='\0';
	break;
    }

    if(pinfo->index_type != indexTypeNone)
	strcat(query,scratch);

    /* start fetching... */
    db_sqlite_lock();
    err=sqlite_compile(db_sqlite_songs,query,&ptail,&db_sqlite_pvm,&perr);
    db_sqlite_unlock();

    DPRINTF(E_DBG,L_DB,"Enum query: %s\n",query);

    if(err != SQLITE_OK) {
	DPRINTF(E_LOG,L_DB,"Could not compile query: %s\n",query);
	return -1;
    }

    return 0;
}

int db_sqlite_enum_size(DBQUERYINFO *pinfo, int *count) {
    const char **valarray;
    const char **colarray;
    int err;
    char *perr;
    int cols;
    int total_size=0;
    int record_size;

    DPRINTF(E_DBG,L_DB,"Enumerating size\n");

    *count=0;

    db_sqlite_lock();
    while((err=sqlite_step(db_sqlite_pvm,&cols,&valarray,&colarray)) == SQLITE_ROW) {
	if((record_size = db_sqlite_get_size(pinfo,(char**)valarray))) {
	    total_size += record_size;
	    *count = *count + 1;
	}
    }

    if(err != SQLITE_DONE) {
	sqlite_finalize(db_sqlite_pvm,&perr);
	db_sqlite_unlock();
	DPRINTF(E_FATAL,L_DB,"sqlite_step: %s\n",perr);
    }

    db_sqlite_unlock();
    db_sqlite_enum_reset(pinfo);

    DPRINTF(E_DBG,L_DB,"Got size: %d\n",total_size);
    return total_size;
}


/**
 * fetch the next record from the enum
 */
int db_sqlite_enum_fetch(DBQUERYINFO *pinfo, unsigned char **pdmap) {
    const char **valarray;
    const char **colarray;
    int err;
    char *perr;
    int cols;
    int result_size=-1;
    unsigned char *presult;

    db_sqlite_lock();
    err=sqlite_step(db_sqlite_pvm,&cols,&valarray,&colarray);
    db_sqlite_unlock();

    while((err == SQLITE_ROW) && (result_size)) {
	result_size=db_sqlite_get_size(pinfo,(char**)valarray);
	if(result_size) {
	    presult=(unsigned char*)malloc(result_size);
	    if(!presult)
		return 0;
	    db_sqlite_build_dmap(pinfo,(char**)valarray,presult,result_size);
	    DPRINTF(E_DBG,L_DB,"Building response for %s (size %d)\n",valarray[3],result_size);
	    *pdmap = presult;
	    return result_size;
	}
    }

    if(err == SQLITE_DONE) {
	return -1;
    }

    db_sqlite_lock();
    sqlite_finalize(db_sqlite_pvm,&perr);
    db_sqlite_unlock();

    DPRINTF(E_FATAL,L_DB,"sqlite_step: %s\n",perr);
    return NULL;
}

/**
 * start the enum again
 */
int db_sqlite_enum_reset(DBQUERYINFO *pinfo) {
    db_sqlite_enum_end();
    return db_sqlite_enum_start(pinfo);
}


/**
 * stop the enum
 */
int db_sqlite_enum_end(void) {
    char *perr;

    db_sqlite_lock();
    sqlite_finalize(db_sqlite_pvm,&perr);
    db_sqlite_unlock();

    return 0;
}

int db_sqlite_get_size(DBQUERYINFO *pinfo, char **valarray) {
    int size;

    switch(pinfo->query_type) {
    case queryTypeBrowseArtists: /* simple 'mlit' entry */
    case queryTypeBrowseAlbums:
    case queryTypeBrowseGenres:
    case queryTypeBrowseComposers:
	return valarray[0] ? (8 + strlen(valarray[0])) : 0;
    case queryTypePlaylists:
	size = 8;   /* mlit */
	size += 12; /* miid */
	size += 12; /* mimc */
	size += 9;  /* aeSP */
	size += (8 + strlen(valarray[1])); /* minm */
	return size;
	break;
    case queryTypeItems:
    case queryTypePlaylistItems:  /* essentially the same query */
	size = 8; /* mlit */
	if(db_wantsmeta(pinfo->meta, metaItemKind)) 
	    /* mikd */
	    size += 9; 
	if(db_wantsmeta(pinfo->meta, metaSongDataKind))
	    /* asdk */
	    size += 9;
	if(valarray[13] && db_wantsmeta(pinfo->meta, metaSongDataURL)) 
	    /* asul */
	    size += (8 + strlen(valarray[13]));
	if(valarray[5] && db_wantsmeta(pinfo->meta, metaSongAlbum))    
	    /* asal */
	    size += (8 + strlen(valarray[5]));
	if(valarray[4] && db_wantsmeta(pinfo->meta, metaSongArtist))   
	    /* asar */
	    size += (8 + strlen(valarray[4]));
	if(valarray[23] && atoi(valarray[23]) && db_wantsmeta(pinfo->meta, metaSongBPM))      
	    /* asbt */
	    size += 10;
	if(valarray[14] && atoi(valarray[14]) && db_wantsmeta(pinfo->meta, metaSongBitRate))  
	    /* asbr */
	    size += 10;
	if(valarray[7] && db_wantsmeta(pinfo->meta, metaSongComment))  
	    /* ascm */
	    size += (8 + strlen(valarray[7]));
	if(valarray[24] && atoi(valarray[24]) && db_wantsmeta(pinfo->meta,metaSongCompilation)) 
	    /* asco */
	    size += 9;
	if(valarray[9] && db_wantsmeta(pinfo->meta, metaSongComposer))
	    /* ascp */
	    size += (8 + strlen(valarray[9]));
	if(valarray[12] && db_wantsmeta(pinfo->meta, metaSongGrouping))
	    /* agrp */
	    size += (8 + strlen(valarray[12]));
	if(valarray[30] && atoi(valarray[30]) && db_wantsmeta(pinfo->meta, metaSongDateAdded))
	    /* asda */
	    size += 12;
	if(valarray[31] && atoi(valarray[31]) && db_wantsmeta(pinfo->meta,metaSongDateModified))
	    /* asdm */
	    size += 12;
	if(valarray[22] && atoi(valarray[22]) && db_wantsmeta(pinfo->meta, metaSongDiscCount))
	    /* asdc */
	    size += 10;
	if(valarray[6] && db_wantsmeta(pinfo->meta, metaSongGenre))
	    /* asgn */
	    size += (8 + strlen(valarray[6]));
	if(db_wantsmeta(pinfo->meta,metaItemId))
	    /* miid */
	    size += 12;
	if(valarray[8] && db_wantsmeta(pinfo->meta,metaSongFormat))
	    /* asfm */
	    size += (8 + strlen(valarray[8]));
	if(valarray[29] && db_wantsmeta(pinfo->meta,metaSongDescription))
	    /* asdt */
	    size += (8 + strlen(valarray[29]));
	if(valarray[3] && db_wantsmeta(pinfo->meta,metaItemName))
	    /* minm */
	    size += (8 + strlen(valarray[3]));
	if(valarray[34] && atoi(valarray[34]) && db_wantsmeta(pinfo->meta,metaSongDisabled))
	    /* asdb */
	    size += 9;
	if(valarray[15] && atoi(valarray[15]) && db_wantsmeta(pinfo->meta,metaSongSampleRate))
	    /* assr */
	    size += 12;
	if(valarray[17] && atoi(valarray[17]) && db_wantsmeta(pinfo->meta,metaSongSize))
	    /* assz */
	    size += 12;

	/* In the old daap code, we always returned 0 for asst and assp
	 * (song start time, song stop time).  I don't know if this
	 * is required, so I'm going to disabled it
	 */

	if(valarray[16] && atoi(valarray[16]) && db_wantsmeta(pinfo->meta, metaSongTime))
	    /* astm */
	    size += 12;
	if(valarray[20] && atoi(valarray[20]) && db_wantsmeta(pinfo->meta, metaSongTrackCount))
	    /* astc */
	    size += 10;
	if(valarray[19] && atoi(valarray[19]) && db_wantsmeta(pinfo->meta, metaSongTrackNumber))
	    /* astn */
	    size += 10;
	if(valarray[25] && atoi(valarray[25]) && db_wantsmeta(pinfo->meta, metaSongUserRating))
	    /* asur */
	    size += 9;
	if(valarray[18] && atoi(valarray[18]) && db_wantsmeta(pinfo->meta, metaSongYear))
	    /* asyr */
	    size += 10;
	if(db_wantsmeta(pinfo->meta, metaContainerItemId))
	    /* mcti */
	    size += 12;

	return size;
	break;

    default:
	DPRINTF(E_LOG,L_DB|L_DAAP,"Unknown query type: %d\n",(int)pinfo->query_type);
	return 0;
    }
    return 0;
}

int db_sqlite_build_dmap(DBQUERYINFO *pinfo, char **valarray, char *presult, int len) {
    unsigned char *current = presult;

    switch(pinfo->query_type) {
    case queryTypeBrowseArtists: /* simple 'mlit' entry */
    case queryTypeBrowseAlbums:
    case queryTypeBrowseGenres:
    case queryTypeBrowseComposers:
	return db_dmap_add_string(current,"mlit",valarray[0]);
    case queryTypePlaylists:
	/* do I want to include the mlit? */
	current += db_dmap_add_container(current,"mlit",len - 8);
	current += db_dmap_add_int(current,"miid",atoi(valarray[0]));
	current += db_dmap_add_int(current,"mimc",atoi(valarray[3]));
	current += db_dmap_add_char(current,"aeSP",atoi(valarray[2]));
	current += db_dmap_add_string(current,"minm",valarray[1]);
	break;
    case queryTypeItems:
    case queryTypePlaylistItems:  /* essentially the same query */
	current += db_dmap_add_container(current,"mlit",len-8);
	if(db_wantsmeta(pinfo->meta, metaItemKind)) 
	    current += db_dmap_add_char(current,"mikd",(char)atoi(valarray[28]));
	if(db_wantsmeta(pinfo->meta, metaSongDataKind))
	    current += db_dmap_add_char(current,"asdk",(char)atoi(valarray[27]));
	if(valarray[13] && db_wantsmeta(pinfo->meta, metaSongDataURL)) 
	    current += db_dmap_add_string(current,"asul",valarray[13]);
	if(valarray[5] && db_wantsmeta(pinfo->meta, metaSongAlbum))    
	    current += db_dmap_add_string(current,"asal",valarray[5]);
	if(valarray[4] && db_wantsmeta(pinfo->meta, metaSongArtist))   
	    current += db_dmap_add_string(current,"asar",valarray[4]);
	if(valarray[23] && atoi(valarray[23]) && db_wantsmeta(pinfo->meta, metaSongBPM))      
	    current += db_dmap_add_short(current,"asbt",(short)atoi(valarray[23]));
	if(valarray[14] && atoi(valarray[14]) && db_wantsmeta(pinfo->meta, metaSongBitRate))  
	    current += db_dmap_add_short(current,"asbr",(short)atoi(valarray[14]));
	if(valarray[7] && db_wantsmeta(pinfo->meta, metaSongComment))  
	    current += db_dmap_add_string(current,"ascm",valarray[7]);
	if(valarray[24] && atoi(valarray[24]) && db_wantsmeta(pinfo->meta,metaSongCompilation)) 
	    current += db_dmap_add_char(current,"asco",(char)atoi(valarray[24]));
	if(valarray[9] && db_wantsmeta(pinfo->meta, metaSongComposer))
	    current += db_dmap_add_string(current,"ascp",valarray[9]);
	if(valarray[12] && db_wantsmeta(pinfo->meta, metaSongGrouping))
	    current += db_dmap_add_string(current,"agrp",valarray[12]);
	if(valarray[30] && atoi(valarray[30]) && db_wantsmeta(pinfo->meta, metaSongDateAdded))
	    current += db_dmap_add_int(current,"asda",(int)atoi(valarray[30]));
	if(valarray[31] && atoi(valarray[31]) && db_wantsmeta(pinfo->meta,metaSongDateModified))
	    current += db_dmap_add_int(current,"asdm",(int)atoi(valarray[31]));
	if(valarray[22] && atoi(valarray[22]) && db_wantsmeta(pinfo->meta, metaSongDiscCount))
	    current += db_dmap_add_short(current,"asdc",(short)atoi(valarray[22]));
	if(valarray[6] && db_wantsmeta(pinfo->meta, metaSongGenre))
	    current += db_dmap_add_string(current,"asgn",valarray[6]);
	if(db_wantsmeta(pinfo->meta,metaItemId))
	    current += db_dmap_add_int(current,"miid",(int)atoi(valarray[0]));
	if(valarray[8] && db_wantsmeta(pinfo->meta,metaSongFormat))
	    current += db_dmap_add_string(current,"asfm",valarray[8]);
	if(valarray[29] && db_wantsmeta(pinfo->meta,metaSongDescription))
	    current += db_dmap_add_string(current,"asdt",valarray[29]);
	if(valarray[3] && db_wantsmeta(pinfo->meta,metaItemName))
	    current += db_dmap_add_string(current,"minm",valarray[3]);
	if(valarray[34] && atoi(valarray[34]) && db_wantsmeta(pinfo->meta,metaSongDisabled))
	    current += db_dmap_add_char(current,"asdb",(char)atoi(valarray[34]));
	if(valarray[15] && atoi(valarray[15]) && db_wantsmeta(pinfo->meta,metaSongSampleRate))
	    current += db_dmap_add_int(current,"assr",atoi(valarray[15]));
	if(valarray[17] && atoi(valarray[17]) && db_wantsmeta(pinfo->meta,metaSongSize))
	    current += db_dmap_add_int(current,"assz",atoi(valarray[17]));
	if(valarray[16] && atoi(valarray[16]) && db_wantsmeta(pinfo->meta, metaSongTime))
	    current += db_dmap_add_int(current,"astm",atoi(valarray[16]));
	if(valarray[20] && atoi(valarray[20]) && db_wantsmeta(pinfo->meta, metaSongTrackCount))
	    current += db_dmap_add_short(current,"astc",(short)atoi(valarray[20]));
	if(valarray[19] && atoi(valarray[19]) && db_wantsmeta(pinfo->meta, metaSongTrackNumber))
	    current += db_dmap_add_short(current,"astn",(short)atoi(valarray[19]));
	if(valarray[25] && atoi(valarray[25]) && db_wantsmeta(pinfo->meta, metaSongUserRating))
	    current += db_dmap_add_char(current,"asur",(char)atoi(valarray[25]));
	if(valarray[18] && atoi(valarray[18]) && db_wantsmeta(pinfo->meta, metaSongYear))
	    current += db_dmap_add_short(current,"asyr",(short)atoi(valarray[18]));
	if(db_wantsmeta(pinfo->meta, metaContainerItemId))
	    current += db_dmap_add_int(current,"mcti",atoi(valarray[0]));
	return 0;
	break;

    default:
	DPRINTF(E_LOG,L_DB|L_DAAP,"Unknown query type: %d\n",(int)pinfo->query_type);
	return 0;
    }
    return 0;
}

/**
 * return the id for a given path (or 0 if it does not exist)
 *
 * \param path path of the file we are looking for
 */
int db_sqlite_get_id(char *path) {
    int err, rows, cols;
    char *perr;
    char **resarray;
    int retval=0;
    
    db_sqlite_lock();
    err=sqlite_get_table_printf(db_sqlite_songs,"SELECT * FROM songs WHERE path='%q'",
				&resarray, &rows, &cols, &perr, path);
    db_sqlite_unlock();

    if(rows != 0) {
	retval=atoi(resarray[cols]);
    }

    db_sqlite_lock();
    sqlite_free_table(resarray);
    db_sqlite_unlock();

    return retval;
}

int db_sqlite_atoi(const char *what) {
    return what ? atoi(what) : 0;
}
char *db_sqlite_strdup(const char *what) {
    return what ? (strlen(what) ? strdup(what) : NULL) : NULL;
}


void db_sqlite_build_mp3file(char **valarray, MP3FILE *pmp3) {
    memset(pmp3,0x00,sizeof(MP3FILE));
    pmp3->id=db_sqlite_atoi(valarray[0]);
    pmp3->path=db_sqlite_strdup(valarray[1]);
    pmp3->fname=db_sqlite_strdup(valarray[2]);
    pmp3->title=db_sqlite_strdup(valarray[3]);
    pmp3->artist=db_sqlite_strdup(valarray[4]);
    pmp3->album=db_sqlite_strdup(valarray[5]);
    pmp3->genre=db_sqlite_strdup(valarray[6]);
    pmp3->comment=db_sqlite_strdup(valarray[7]);
    pmp3->type=db_sqlite_strdup(valarray[8]);
    pmp3->composer=db_sqlite_strdup(valarray[9]);
    pmp3->orchestra=db_sqlite_strdup(valarray[10]);
    pmp3->conductor=db_sqlite_strdup(valarray[11]);
    pmp3->grouping=db_sqlite_strdup(valarray[12]);
    pmp3->url=db_sqlite_strdup(valarray[13]);
    pmp3->bitrate=db_sqlite_atoi(valarray[14]);
    pmp3->samplerate=db_sqlite_atoi(valarray[15]);
    pmp3->song_length=db_sqlite_atoi(valarray[16]);
    pmp3->file_size=db_sqlite_atoi(valarray[17]);
    pmp3->year=db_sqlite_atoi(valarray[18]);
    pmp3->track=db_sqlite_atoi(valarray[19]);
    pmp3->total_tracks=db_sqlite_atoi(valarray[20]);
    pmp3->disc=db_sqlite_atoi(valarray[21]);
    pmp3->total_discs=db_sqlite_atoi(valarray[22]);
    pmp3->bpm=db_sqlite_atoi(valarray[23]);
    pmp3->compilation=db_sqlite_atoi(valarray[24]);
    pmp3->rating=db_sqlite_atoi(valarray[25]);
    pmp3->play_count=db_sqlite_atoi(valarray[26]);
    pmp3->data_kind=db_sqlite_atoi(valarray[27]);
    pmp3->item_kind=db_sqlite_atoi(valarray[28]);
    pmp3->description=db_sqlite_strdup(valarray[29]);
    pmp3->time_added=db_sqlite_atoi(valarray[30]);
    pmp3->time_modified=db_sqlite_atoi(valarray[31]);
    pmp3->time_played=db_sqlite_atoi(valarray[32]);
    pmp3->db_timestamp=db_sqlite_atoi(valarray[33]);
    pmp3->disabled=db_sqlite_atoi(valarray[34]);
}

/**
 * fetch a MP3FILE for a specific id
 *
 * \param id id to fetch
 */
MP3FILE *db_sqlite_fetch_item(int id) {
    int err,rows,cols;
    char *perr;
    char **resarray;
    MP3FILE *pmp3=NULL;

    db_sqlite_lock();
    err=sqlite_get_table_printf(db_sqlite_songs,"SELECT * FROM songs WHERE id=%d",
				&resarray, &rows, &cols, &perr, id);
    db_sqlite_unlock();

    if(rows != 0) {
	pmp3=(MP3FILE*)malloc(sizeof(MP3FILE));
	if(!pmp3) 
	    DPRINTF(E_FATAL,L_MISC,"Malloc error in db_sqlite_fetch_item\n");

	db_sqlite_build_mp3file((char **)&resarray[cols],pmp3);
    }

    db_sqlite_lock();
    sqlite_free_table(resarray);
    if(db_sqlite_in_scan) {
	sqlite_exec_printf(db_sqlite_songs,"UPDATE songs SET updated=1 WHERE id=%d",
			   NULL,NULL,&perr,id);
    }
    db_sqlite_unlock();

    return pmp3;
}


/** 
 * dispose of a MP3FILE struct that was obtained 
 * from db_sqlite_fetch_item
 *
 * \param pmp3 item obtained from db_sqlite_fetch_item
 */
void db_sqlite_dispose_item(MP3FILE *pmp3) {
    if(pmp3)
	return;

    MAYBEFREE(pmp3->path);
    MAYBEFREE(pmp3->fname);
    MAYBEFREE(pmp3->title);
    MAYBEFREE(pmp3->artist);
    MAYBEFREE(pmp3->album);
    MAYBEFREE(pmp3->genre);
    MAYBEFREE(pmp3->comment);
    MAYBEFREE(pmp3->type); 
    MAYBEFREE(pmp3->composer);
    MAYBEFREE(pmp3->orchestra);
    MAYBEFREE(pmp3->conductor);
    MAYBEFREE(pmp3->grouping);
    MAYBEFREE(pmp3->description);
    MAYBEFREE(pmp3->url);
    free(pmp3);
}

/**
 * count either the number of playlists, or the number of
 * songs
 *
 * \param type either countPlaylists or countSongs (type to count)
 */
extern int db_sqlite_get_count(CountType_t type) {
    char *table;
    int err, rows, cols;
    char *perr;
    char **resarray;
    int retval=0;
    
    switch(type) {
    case countPlaylists:
	table="playlists";
	break;

    case countSongs:
    default:
	table="songs";
	break;
    }

    db_sqlite_lock();
    err=sqlite_get_table_printf(db_sqlite_songs,"SELECT COUNT(*) FROM %q",
				&resarray, &rows, &cols, &perr, table);
    db_sqlite_unlock();

    if(rows != 0) {
	retval=atoi(resarray[cols]);
    }

    db_sqlite_lock();
    sqlite_free_table(resarray);
    db_sqlite_unlock();

    return retval;
}
