/*
 * $Id$
 * Rendezvous - OSX style
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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <unistd.h>
#include <pwd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>

#include <libc.h>
#include <arpa/nameser.h>
#include <CoreFoundation/CoreFoundation.h>
#include <DNSServiceDiscovery/DNSServiceDiscovery.h>

#include "daapd.h"
#include "err.h"
#include "os-unix.h"
#include "rend.h"

CFRunLoopRef rend_runloop;
CFRunLoopSourceRef rend_rls;
pthread_t rend_tid;

#define MAX_TEXT_LEN 255

/* Forwards */
void *rend_pipe_monitor(void* arg);
void *rend_runloop_threadproc(void* arg);

/*
 * rend_stoprunloop
 */
static void rend_stoprunloop(void) {
    CFRunLoopStop(rend_runloop);
}

/*
 * rend_sigint
 */
/*
static void rend_sigint(int sigraised) {
    DPRINTF(E_INF,L_REND,"SIGINT\n");
    rend_stoprunloop();
}
*/

/*
 * rend_handler
 */
static void rend_handler(CFMachPortRef port, void *msg, CFIndex size, void *info) {
    DNSServiceDiscovery_handleReply(msg);
}

/*
 * rend_addtorunloop
 */
static int rend_addtorunloop(dns_service_discovery_ref client) {
    mach_port_t port=DNSServiceDiscoveryMachPort(client);

    if(!port)
        return -1;
    else {
        CFMachPortContext context = { 0, 0, NULL, NULL, NULL };
        Boolean shouldFreeInfo;
        CFMachPortRef cfMachPort=CFMachPortCreateWithPort(kCFAllocatorDefault,
                                                          port, rend_handler,
                                                          &context, &shouldFreeInfo);

        CFRunLoopSourceRef rls=CFMachPortCreateRunLoopSource(NULL,cfMachPort,0);
        CFRunLoopAddSource(CFRunLoopGetCurrent(),
                           rls,kCFRunLoopDefaultMode);
        CFRelease(rls);
        return 0;
    }
}

/*
 * rend_reply
 */
static void rend_reply(DNSServiceRegistrationReplyErrorType errorCode, void *context) {
    switch(errorCode) {
    case kDNSServiceDiscoveryNoError:
        DPRINTF(E_DBG,L_REND,"Registered successfully\n");
        break;
    case kDNSServiceDiscoveryNameConflict:
        DPRINTF(E_WARN,L_REND,"Error - name in use\n");
        break;
    default:
        DPRINTF(E_WARN,L_REND,"Error %d\n",errorCode);
        break;
    }
}

/**
 * Add a text stanza to the buffer (pascal-style multistring)
 *
 * @param buffer where to put the text info
 * @param string what pascal string to append
 */
void rend_add_text(char *buffer, char *string) {
    char *ptr=&buffer[strlen(buffer)];
    *ptr=strlen(string);
    strcpy(ptr+1,string);
}

int rend_running(void) {
    return TRUE;
}

int rend_stop(void) {
    rend_stoprunloop();
    pthread_join(rend_tid,NULL);

    return 1;
}

int rend_register(char *name, char *type, int port, char *ifact, char *txt) {
    unsigned short usPort;
    dns_service_discovery_ref dns_ref=NULL;
    char *src,*dst;
    int len;

    src=dst=txt;
    while(src && (*src) && (src - txt < MAX_TEXT_LEN)) {
        len = (*src);
        if((src + len + 1) - txt < MAX_TEXT_LEN) {
            memmove(dst,src+1,len);
            dst += len;
            if(*src) {
                *dst++ = '\001';
            } else {
                *dst='\0';
            }
        }
        src += len + 1;
    }

    DPRINTF(E_DBG,L_REND,"Registering %s.%s (%d)\n",type,name,port);
    usPort=htons(port);
    dns_ref=DNSServiceRegistrationCreate(name,type,"",usPort,txt,rend_reply,nil);
    if(rend_addtorunloop(dns_ref)) {
        DPRINTF(E_WARN,L_REND,"Add to runloop failed\n");
        return -1;
    } else {
        return 0;
    }
}

/**
 * not implemeneted
 *
 * @param name name of service to unregistery
 * @param type type of service to unregister
 * @param port port of service to nregister
 */
int rend_unregister(char *name, char *type, int port) {
    return -1;
}


/**
 * execute the runloop
 *
 * @param arg unused
 */
void *rend_runloop_threadproc(void* arg) {
    CFRunLoopSourceContext context;

    rend_runloop = CFRunLoopGetCurrent();
    rend_rls = CFRunLoopSourceCreate(NULL,0,&context);
    CFRunLoopAddSource(CFRunLoopGetCurrent(),rend_rls,kCFRunLoopDefaultMode);

    DPRINTF(E_DBG,L_REND,"Starting runloop\n");
    CFRunLoopRun();
    DPRINTF(E_DBG,L_REND,"Exiting runloop\n");

    CFRelease(rend_rls);

    return NULL;
}


/*
 * rend_private_init
 *
 * start up the rendezvous services
 */
int rend_init(char *user) {
    sigset_t set;

    if((sigemptyset(&set) == -1) ||
       (sigaddset(&set,SIGINT) == -1) ||
       (sigaddset(&set,SIGHUP) == -1) ||
       (sigaddset(&set,SIGCHLD) == -1) ||
       (sigaddset(&set,SIGTERM) == -1) ||
       (sigaddset(&set,SIGPIPE) == -1) ||
       (pthread_sigmask(SIG_BLOCK, &set, NULL) == -1)) {
        DPRINTF(E_LOG,L_MAIN,"Error setting signal set\n");
        return -1;
    }

    DPRINTF(E_DBG,L_REND,"Starting rendezvous services\n");

    DPRINTF(E_DBG,L_REND,"Starting runloop thread\n");

    if(pthread_create(&rend_tid,NULL,rend_runloop_threadproc,NULL)) {
        DPRINTF(E_FATAL,L_REND,"Could not start thread.  Terminating\n");
        /* should kill parent, too */
        exit(EXIT_FAILURE);
    }

    return 0;
}

