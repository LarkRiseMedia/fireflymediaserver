%{

#include <stdio.h>
#include "playlist.h"

#define YYERROR_VERBOSE 1

/* Forwards */

extern PL_NODE *pl_newpredicate(int tag, int op, char *value, int type);
extern PL_NODE *pl_newexpr(PL_NODE *arg1, int op, PL_NODE *arg2);
extern int pl_addplaylist(char *name, PL_NODE *root);

/* Globals */

int pl_number=2;

%}

%left OR AND

%union {
    unsigned int ival;
    char *cval;
    PL_NODE *plval;    
}

%token <ival> ARTIST 
%token <ival> ALBUM 
%token <ival> GENRE

%token <ival> EQUALS
%token <ival> LESS
%token <ival> LESSEQUAL
%token <ival> GREATER
%token <ival> GREATEREQUAL
%token <ival> IS 
%token <ival> INCLUDES

%token <ival> OR 
%token <ival> AND
%token <ival> NOT

%token <cval> ID
%token <ival> NUM

%token <ival> YEAR

%type <plval> expression
%type <plval> predicate
%type <ival> strtag
%type <ival> inttag
%type <ival> strbool
%type <ival> intbool
%type <ival> playlist

%%

playlistlist: playlist {}
| playlistlist playlist {}
;

playlist: ID '{' expression '}' { $$ = pl_addplaylist($1, $3); }
;

expression: expression AND expression { $$=pl_newexpr($1,$2,$3); }
| expression OR expression { $$=pl_newexpr($1,$2,$3); }
| '(' expression ')' { $$=$2; }
| predicate
;

predicate: strtag strbool ID { $$=pl_newpredicate($1, $2, $3, T_STR); }
| inttag intbool NUM { $$=pl_newpredicate($1, $2, $3, T_INT); }
;

inttag: YEAR
;

intbool: EQUALS { $$ = $1; }
| LESS { $$ = $1; }
| LESSEQUAL { $$ = $1; }
| GREATER { $$ = $1; }
| GREATEREQUAL { $$ = $1; }
| NOT intbool { $$ = $2 | 0x80000000; }
;

strtag: ARTIST
| ALBUM
| GENRE
;

strbool: IS { $$=$1; }
| INCLUDES { $$=$1; }
| NOT strbool { $$=$2 | 0x80000000; }
;

%%

PL_NODE *pl_newpredicate(int tag, int op, char *value, int type) {
    PL_NODE *pnew;

    pnew=(PL_NODE*)malloc(sizeof(PL_NODE));
    if(!pnew)
	return NULL;

    pnew->op=op;
    pnew->type=type;
    pnew->arg1.ival=tag;
    pnew->arg2.cval=value;
    return pnew;
}

PL_NODE *pl_newexpr(PL_NODE *arg1, int op, PL_NODE *arg2) {
    PL_NODE *pnew;

    pnew=(PL_NODE*)malloc(sizeof(PL_NODE));
    if(!pnew)
	return NULL;

    pnew->op=op;
    pnew->arg1.plval=arg1;
    pnew->arg2.plval=arg2;
    return pnew;
}

int pl_addplaylist(char *name, PL_NODE *root) {
    SMART_PLAYLIST *pnew;

    pnew=(SMART_PLAYLIST *)malloc(sizeof(SMART_PLAYLIST));
    if(!pnew)
	return -1;

    pnew->next=pl_smart.next;
    pnew->name=name;
    pnew->root=root;
    pnew->id=pl_number++;
    pl_smart.next=pnew;

    return 0;
}