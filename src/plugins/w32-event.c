/*
 * $Id: $
 */

#include "compat.h"
#include "ff-plugins.h"

/* Forwards */
PLUGIN_INFO *plugin_info(void);
void plugin_handler(int, int, void *, int);

#define PIPE_BUFFER_SIZE 4096

/* Globals */
PLUGIN_EVENT_FN _pefn = { plugin_handler };

PLUGIN_INFO _pi = { 
    PLUGIN_VERSION,        /* version */
    PLUGIN_EVENT,          /* type */
    "w32-event/" VERSION,  /* server */
    NULL,                  /* url */
    NULL,                  /* output fns */
    &_pefn,                /* event fns */
    NULL,                  /* transocde fns */
    NULL,                  /* fns exported by ff */
    NULL,                  /* rend info */
    NULL                   /* codec list */
};

typedef struct tag_plugin_msg {
    int size;
    int event_id;
    int intval;
    char vp[1];
} PLUGIN_MSG;

#define infn ((PLUGIN_INPUT_FN *)(_pi.pi))

PLUGIN_INFO *plugin_info(void) {
    return &_pi;
}

/** NO LOG IN HERE!  We'll go into an endless loop.  :) */
void plugin_handler(int event_id, int intval, void *vp, int len) {
    int total_len = 3 * sizeof(int) + len + 1;
    PLUGIN_MSG *pmsg;
    int port = 9999;
    SOCKET sock;
    struct sockaddr_in servaddr;

    pmsg = (PLUGIN_MSG*)malloc(total_len);
    if(!pmsg) {
//        infn->log(E_LOG,"Malloc error in w32-event.c/plugin_handler\n");
        return;
    }

    memset(pmsg,0,total_len);
    pmsg->size = total_len;
    pmsg->event_id = event_id;
    pmsg->intval = intval;
    memcpy(&pmsg->vp,vp,len);

    sock = socket(AF_INET,SOCK_DGRAM,0);
    if(sock == INVALID_SOCKET) {
        free(pmsg);
        return;
    }

    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    servaddr.sin_port = htons(port);

    sendto(sock,(char*)pmsg,total_len,0,(struct sockaddr *)&servaddr,sizeof(servaddr));

    closesocket(sock);
    free(pmsg);
    return;
}
