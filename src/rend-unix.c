/*
 * $Id$
 * General unix rendezvous routines
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
# include "config.h"
#endif

#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include "daapd.h"
#include "err.h"
#include "rend-unix.h"

int rend_pipe_to[2];
int rend_pipe_from[2];
int rend_pid;

#define RD_SIDE 0
#define WR_SIDE 1

/*
 * rend_init
 *
 * Fork and set up message passing system 
 */
int rend_init(char *user) {
    int err;

    if(pipe((int*)&rend_pipe_to) == -1)
	return -1;

    if(pipe((int*)&rend_pipe_from) == -1) {
	err=errno;
	close(rend_pipe_to[RD_SIDE]);
	close(rend_pipe_to[WR_SIDE]);
	errno=err;
	return -1;
    }

    rend_pid=fork();
    if(rend_pid==-1) {
	err=errno;
	close(rend_pipe_to[RD_SIDE]);
	close(rend_pipe_to[WR_SIDE]);
	close(rend_pipe_from[RD_SIDE]);
	close(rend_pipe_from[WR_SIDE]);
	errno=err;
	return -1;
    }

    if(rend_pid) { /* parent */
	close(rend_pipe_to[RD_SIDE]);
	close(rend_pipe_from[WR_SIDE]);
	return 0;
    }

    /* child */
    close(rend_pipe_to[WR_SIDE]);
    close(rend_pipe_from[RD_SIDE]);

    /* something bad here... should really signal the parent, rather
     * than just zombieizing
     */
    rend_private_init(user); /* should only return when terminated */
    exit(0);
}

/*
 * rend_running
 *
 * See if the rendezvous daemon is runnig
 */
int rend_running(void) {
    REND_MESSAGE msg;
    int result;

    DPRINTF(ERR_DEBUG,"Status inquiry\n");
    msg.cmd=REND_MSG_TYPE_STATUS;
    result=rend_send_message(&msg);
    DPRINTF(ERR_DEBUG,"Returning status %d\n",result);
    return result;
}

/*
 *rend_stop
 *
 * Stop the rendezvous server
 */
int rend_stop(void) {
    REND_MESSAGE msg;

    msg.cmd=REND_MSG_TYPE_STOP;
    return rend_send_message(&msg);
}

/*
 * rend_register
 *
 * register a rendezvous name
 */
int rend_register(char *name, char *type, int port) {
    REND_MESSAGE msg;

    if((strlen(name)+1 > MAX_NAME_LEN) || (strlen(type)+1 > MAX_NAME_LEN)) {
	DPRINTF(ERR_FATAL,"Registration failed: name or type too long\n");
	return -1;
    }

    msg.cmd=REND_MSG_TYPE_REGISTER;
    strcpy(msg.name,name);
    strcpy(msg.type,type);
    msg.port=port;

    return rend_send_message(&msg);
}

/*
 * rend_unregister
 *
 * Stop advertising a rendezvous name
 */
int rend_unregister(char *name, char *type, int port) {
    return -1; /* not implemented */
}

/*
 * rend_send_message
 *
 * Send a rendezvous message
 */
int rend_send_message(REND_MESSAGE *pmsg) {
    int retval;

    if(r_write(rend_pipe_to[WR_SIDE],pmsg,sizeof(REND_MESSAGE)) == -1)
	return -1;

    if((retval=r_read(rend_pipe_from[RD_SIDE],&retval,sizeof(int)) == -1))
	return -1;

    return retval;
}

/*
 * rend_read_message
 *
 * read the message passed to the rend daemon
 */
int rend_read_message(REND_MESSAGE *pmsg) {
    return r_read(rend_pipe_to[RD_SIDE],pmsg,sizeof(REND_MESSAGE));
}

/*
 * rend_send_response
 *
 * Let the rendezvous daemon return a result
 */
int rend_send_response(int value) {
    return r_write(rend_pipe_from[WR_SIDE],&value,sizeof(int));
}
