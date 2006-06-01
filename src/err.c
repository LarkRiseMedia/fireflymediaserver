/*
 * $Id$
 * Generic error handling
 *
 * Copyright (C) 2003 Ron Pedde (ron@pedde.com)
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

/**
 * \file err.c
 * Error handling, logging, and memory leak checking.
 *
 * Most of these functions should not be used directly.  For the most
 * part, they are hidden in macros like #DPRINTF and #MEMNOTIFY.  The
 * only function here that is really directly useable is log_setdest()
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "err.h"
#ifndef ERR_LEAN
# include "os.h"
# include "plugin.h"
#endif

#ifndef PACKAGE
# define PACKAGE "unknown daemon"
#endif

static int err_debuglevel=0; /**< current debuglevel, set from command line with -d */
static int err_logdest=0; /**< current log destination */
static char err_filename[PATH_MAX + 1];
static FILE *err_file=NULL; /**< if logging to file, the handle of that file */
static pthread_mutex_t err_mutex=PTHREAD_MUTEX_INITIALIZER; /**< for serializing log messages */
static unsigned int err_debugmask=0xFFFFFFFF; /**< modules to debug, see \ref log_categories */

/** text list of modules to match for setting debug mask */
static char *err_categorylist[] = {
    "config","webserver","database","scan","query","index","browse",
    "playlist","art","daap","main","rend","xml","parse","plugin",NULL
};

/*
 * Forwards
 */

static int _err_lock(void);
static int _err_unlock(void);


/**
 * if we are logging to a file, then re-open the file.  This
 * would help for log rotation
 */
void err_reopen(void) {
    int err;

    if(!(err_logdest & LOGDEST_LOGFILE))
        return;
    
    _err_lock();
    fclose(err_file);
    err_file = fopen(err_filename,"a");
    if(!err_file) {
        /* what to do when you lose your logging mechanism?  Keep
         * going?
         */
        _err_unlock();
        err = errno;
        err_setdest(err_logdest & (~LOGDEST_LOGFILE));
        err_setdest(err_logdest | LOGDEST_SYSLOG);

        DPRINTF(E_LOG,L_MISC,"Could not rotate log file: %s\n",
                strerror(err));
        return;
    }
    _err_unlock();
    DPRINTF(E_LOG,L_MISC,"Rotated logs\n");
}

/**
 * Write a printf-style formatted message to the log destination.
 * This can be stderr, syslog/eventviewer, or a logfile, as determined by
 * err_setdest().  Note that this function should not be directly
 * used, rather it should be used via the #DPRINTF macro.
 *
 * \param level Level at which to log \ref log_levels
 * \param cat the category to log \ref log_categories
 * \param fmt printf-style
 */
void err_log(int level, unsigned int cat, char *fmt, ...)
{
    va_list ap;
    char timebuf[256];
    char errbuf[4096];
    struct tm tm_now;
    time_t tt_now;

    if(level > 1) {
        if(level > err_debuglevel)
            return;

        if(!(cat & err_debugmask))
            return;
    } /* we'll *always* process a log level 0 or 1 */

    va_start(ap, fmt);
    vsnprintf(errbuf, sizeof(errbuf), fmt, ap);
    va_end(ap);

    _err_lock(); /* atomic file writes */

    if((err_logdest & LOGDEST_LOGFILE) && err_file) {
        tt_now=time(NULL);
        localtime_r(&tt_now,&tm_now);
        strftime(timebuf,sizeof(timebuf),"%Y-%m-%d %T",&tm_now);
        fprintf(err_file,"%s: %s",timebuf,errbuf);
        if(!level) fprintf(err_file,"%s: Aborting\n",timebuf);
        fflush(err_file);
    }
    
    /* always log to stderr on fatal error */
    if((err_logdest & LOGDEST_STDERR) || (!level)) {
        fprintf(stderr, "%s",errbuf);
        if(!level) fprintf(stderr,"Aborting\n");
    }
    
    /* alwyas log fatals to syslog */
    if(!level) {
        os_opensyslog(); // (app,LOG_PID,LOG_DAEMON);
        os_syslog(level,errbuf);
        os_closesyslog();
    }

#ifndef ERR_LEAN
    plugin_event_dispatch(PLUGIN_EVENT_LOG, level, errbuf, (int)strlen(errbuf)+1);
#endif

    _err_unlock();
    
    if(!level) {
        exit(EXIT_FAILURE);      /* this should go to an OS-specific exit routine */
    }
}
 
/**
 * simple get/set interface to debuglevel to avoid global
 */
void err_setlevel(int level) {
    _err_lock();
    err_debuglevel = level;
    _err_unlock();
}

/**
 * get current debug level
 */
int err_getlevel(void) {
    int level;
    _err_lock();
    level = err_debuglevel;
    _err_unlock();

    return level;
}


/**
 * get the logfile destination
 */
int err_getdest(void) {
    int dest;
    
    _err_lock();
    dest=err_logdest;
    _err_unlock();

    return dest;
}


int err_setlogfile(char *file) {
    if(strcmp(file,err_filename) == 0)
        return TRUE;

    _err_lock();

    if(err_file) {
        fclose(err_file);
    }

    memset(err_filename,0,sizeof(err_filename));
    strncpy(err_filename,file,sizeof(err_filename)-1);

    err_file = fopen(err_filename,"a");
    if(err_file == NULL) {
        err_logdest &= ~LOGDEST_LOGFILE;

        os_opensyslog(); // (app,LOG_PID,LOG_DAEMON);
        os_syslog(1,"Error opening logfile");
        os_closesyslog();
        
        _err_unlock();
        return FALSE;
    }

    _err_unlock();
    return TRUE;
}

/**
 * Sets the log destination.  (stderr, syslog, or logfile)
 *
 * \param app appname (used only for syslog destination)
 * \param destination where to log to \ref log_dests "as defined in err.h"
 */
void err_setdest(int destination) {
    fprintf(stderr,"setting dest to %d\n",destination);

    if(err_logdest == destination)
        return;

    _err_lock();
    if((err_logdest & LOGDEST_LOGFILE) &&
       (!(destination & LOGDEST_LOGFILE))) {
        /* used to be logging to file, not any more */
        fclose(err_file);
    }

    err_logdest=destination;
    _err_unlock();
}
/**
 * Set the debug mask.  Given a comma separated list, this walks
 * through the err_categorylist and sets the bitfields for the
 * requested log modules.
 *
 * \param list comma separated list of modules to debug.
 */
extern int err_setdebugmask(char *list) {
    unsigned int rack;
    char *token, *str, *last;
    char *tmpstr;
    int index;

    err_debugmask=0x80000000; /* always log L_MISC! */
    str=tmpstr=strdup(list);
    if(!str)
        return 0;
    
    _err_lock();
    while(1) {
        token=strtok_r(str,",",&last);
        str=NULL;

        if(token) {
            rack=1;
            index=0;
            while((err_categorylist[index]) &&
                  (strcasecmp(err_categorylist[index],token))) {
                rack <<= 1;
                index++;
            }

            if(!err_categorylist[index]) {
                DPRINTF(E_LOG,L_MISC,"Unknown module: %s\n",token);
                free(tmpstr);
                return 1;
            } else {
                DPRINTF(E_DBG,L_MISC,"Adding module %s to debug list (0x%08x)\n",token,rack);
                err_debugmask |= rack;
            }
        } else break; /* !token */
    }

    DPRINTF(E_INF,L_MISC,"Debug mask is 0x%08x\n",err_debugmask);
    free(tmpstr);
    _err_unlock();

    return 0;
}

/**
 * Lock the error mutex.  This is used to serialize
 * log messages, as well as protect access to the memory
 * list, when memory debugging is enabled.
 *
 * \returns 0 on success, otherwise -1 with errno set
 */
int _err_lock(void) {
    int err;

    if((err=pthread_mutex_lock(&err_mutex))) {
        errno=err;
        return -1;
    }

    return 0;
}

/**
 * Unlock the error mutex
 *
 * \returns 0 on success, otherwise -1 with errno set
 */
int _err_unlock(void) {
    int err;

    if((err=pthread_mutex_unlock(&err_mutex))) {
        errno=err;
        return -1;
    }

    return 0;
}

