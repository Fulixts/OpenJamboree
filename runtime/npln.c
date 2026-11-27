/* OpenJamboree online play: the NPLN services the host's built-in server answers (online.c carries them over
   TLS + HTTP/2 + gRPC). Message layouts are the .proto files kinnay rebuilt in NPLN-Protocols (latest/), checked
   against the calls the game makes (docs/ONLINE_SPEC.md). A small protobuf codec is enough: these are flat messages.

   npln_call(path, request, n, &status, &out, &outn): 0 = handled (status is the grpc-status, out the response
   message, owned by the caller), -1 = unknown method (the server answers UNIMPLEMENTED and logs it). */
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
extern const char* ini_value(const char*, const char*);

/* ============================================================ protobuf */
typedef struct { u8* p; size_t n, cap; } PB;
static void pb_need(PB* b, size_t k){ if(b->n+k>b->cap){ b->cap=(b->n+k)*2+64; b->p=realloc(b->p,b->cap); } }
static void pb_varint(PB* b, u64 v){ pb_need(b,10); do{ u8 c=(u8)(v&0x7F); v>>=7; if(v) c|=0x80; b->p[b->n++]=c; }while(v); }
static void pb_key(PB* b, u32 field, u32 wt){ pb_varint(b,((u64)field<<3)|wt); }
static void pb_bytes(PB* b, u32 field, const void* d, size_t n){ pb_key(b,field,2); pb_varint(b,n); pb_need(b,n); memcpy(b->p+b->n,d,n); b->n+=n; }
static void pb_str(PB* b, u32 field, const char* s){ if(s && *s) pb_bytes(b,field,s,strlen(s)); }
static void pb_int(PB* b, u32 field, u64 v){ if(v){ pb_key(b,field,0); pb_varint(b,v); } }
static void pb_msg(PB* b, u32 field, PB* m){ pb_bytes(b,field,m->p? m->p : (u8*)"",m->n); free(m->p); m->p=NULL; m->n=m->cap=0; }
/* google.protobuf.Timestamp / Duration: {int64 seconds = 1; int32 nanos = 2} */
static void pb_seconds(PB* b, u32 field, i64 s){ PB t={0}; pb_int(&t,1,(u64)s); pb_msg(b,field,&t); }

/* reads field `want` (first occurrence) of a length-delimited or varint kind; returns 1 if found */
static int pb_get(const u8* p, size_t n, u32 want, const u8** d, size_t* dn, u64* v){
  size_t o=0;
  while(o<n){
    u64 key=0; int sh=0; while(o<n){ u8 c=p[o++]; key|=(u64)(c&0x7F)<<sh; if(!(c&0x80)) break; sh+=7; }
    u32 f=(u32)(key>>3), wt=(u32)(key&7);
    if(wt==0){ u64 x=0; sh=0; while(o<n){ u8 c=p[o++]; x|=(u64)(c&0x7F)<<sh; if(!(c&0x80)) break; sh+=7; } if(f==want){ if(v) *v=x; return 1; } }
    else if(wt==2){ u64 l=0; sh=0; while(o<n){ u8 c=p[o++]; l|=(u64)(c&0x7F)<<sh; if(!(c&0x80)) break; sh+=7; } if(o+l>n) return 0; if(f==want){ *d=p+o; *dn=(size_t)l; return 1; } o+=(size_t)l; }
    else if(wt==1) o+=8; else if(wt==5) o+=4; else return 0; }
  return 0; }
/* iterates the occurrences of length-delimited field `want`: call with *o=0, returns 0 at the end */
static int pb_next(const u8* p, size_t n, u32 want, size_t* o, const u8** d, size_t* dn){
  while(*o<n){
    u64 key=0; int sh=0; while(*o<n){ u8 c=p[(*o)++]; key|=(u64)(c&0x7F)<<sh; if(!(c&0x80)) break; sh+=7; }
    u32 f=(u32)(key>>3), wt=(u32)(key&7);
    if(wt==0){ while(*o<n && (p[(*o)++]&0x80)); }
    else if(wt==2){ u64 l=0; sh=0; while(*o<n){ u8 c=p[(*o)++]; l|=(u64)(c&0x7F)<<sh; if(!(c&0x80)) break; sh+=7; } if(*o+l>n) return 0; if(f==want){ *d=p+*o; *dn=(size_t)l; *o+=(size_t)l; return 1; } *o+=(size_t)l; }
    else if(wt==1) *o+=8; else if(wt==5) *o+=4; else return 0; }
  return 0; }
static void pb_getstr(const u8* p, size_t n, u32 f, char* out, size_t cap){ const u8* d; size_t dn; out[0]=0;
  if(pb_get(p,n,f,&d,&dn,NULL)){ if(dn>=cap) dn=cap-1; memcpy(out,d,dn); out[dn]=0; } }

/* ============================================================ users (in memory, on the host) */
/* a player is identified by the NSA id token their PC sends (acc's placeholder JWT carries a per-PC subject);
   the hash of the token's payload names the user, so the same PC always gets the same user */
static u64 fnv(const void* d, size_t n){ const u8* p=d; u64 h=1469598103934665603ull; for(size_t i=0;i<n;i++){ h^=p[i]; h*=1099511628211ull; } return h; }
static u64 token_user(const u8* req, size_t n, u32 tokfield){
  const u8* t; size_t tn; const u8* s; size_t sn;
  if(pb_get(req,n,tokfield,&t,&tn,NULL) && pb_get(t,tn,1,&s,&sn,NULL)){
    const u8* dot=memchr(s,'.',sn); if(dot){ const u8* p=dot+1; const u8* e=memchr(p,'.',sn-(p-s)); if(e) return fnv(p,e-p) | 1; }
    return fnv(s,sn) | 1; }
  return 1; }

static void user_msg(PB* out, const char* tenant, u64 id){
  char name[200], acct[64]; time_t now=time(NULL);
  snprintf(name,sizeof name,"%s/users/%016llx",*tenant? tenant : "tenants/t-adf89f68-lp1",(unsigned long long)id);
  snprintf(acct,sizeof acct,"accounts/%016llx",(unsigned long long)id);
  pb_str(out,1,name); pb_str(out,2,acct); pb_seconds(out,3,(i64)now-86400); pb_seconds(out,4,(i64)now);
  pb_int(out,5,id & 0x7FFFFFFFFFFFull); }

static void token_msg(PB* out, const char* user, u64 id){
  char a[64], r[64]; snprintf(a,sizeof a,"ojam-access-%016llx",(unsigned long long)id); snprintf(r,sizeof r,"ojam-refresh-%016llx",(unsigned long long)id);
  pb_str(out,1,user); pb_str(out,2,a); pb_str(out,3,r); pb_seconds(out,4,28800); }

static u64 user_from_name(const char* s){ const char* u=strstr(s,"/users/"); return u? strtoull(u+7,NULL,16) : 1; }

/* ============================================================ nn.npln.auth.v1.Auth */
static int auth(const char* m, const u8* q, size_t n, PB* out){
  char tenant[160];
  if(!strcmp(m,"CreateUser")){ pb_getstr(q,n,1,tenant,sizeof tenant); u64 id=token_user(q,n,3); user_msg(out,tenant,id);
    LOG("online: auth CreateUser -> user %016llx\n",(unsigned long long)id); return 0; }
  if(!strcmp(m,"ListUsers")){ pb_getstr(q,n,1,tenant,sizeof tenant); PB u={0}; user_msg(&u,tenant,1); /* no token here: answered by CreateUser */
    (void)u; free(u.p); return 0; }                                                       /* empty list: the game then creates its user */
  if(!strcmp(m,"IssueToken")){ char user[200]; pb_getstr(q,n,1,user,sizeof user); u64 id= *user? user_from_name(user) : token_user(q,n,2);
    if(!*user) snprintf(user,sizeof user,"tenants/t-adf89f68-lp1/users/%016llx",(unsigned long long)id);
    PB t={0}; token_msg(&t,user,id); pb_msg(out,1,&t); LOG("online: auth IssueToken for %s\n",user); return 0; }
  if(!strcmp(m,"RefreshToken")){ char user[200]; pb_getstr(q,n,1,user,sizeof user); PB t={0}; token_msg(&t,user,user_from_name(user)); pb_msg(out,1,&t); return 0; }
  if(!strcmp(m,"IssuePrearrangedUserToken")){ pb_getstr(q,n,1,tenant,sizeof tenant); u64 v=0; const u8* d; size_t dn; pb_get(q,n,2,&d,&dn,&v);
    u64 id=token_user(q,n,3) ^ (v*0x9E3779B97F4A7C15ull); PB u={0}; user_msg(&u,tenant,id); pb_msg(out,1,&u);
    char user[200]; snprintf(user,sizeof user,"%s/users/%016llx",*tenant? tenant : "tenants/t-adf89f68-lp1",(unsigned long long)id);
    PB t={0}; token_msg(&t,user,id); pb_msg(out,2,&t); return 0; }
  if(!strcmp(m,"ValidateToken") || !strcmp(m,"DeleteUser")) return 0;
  return -1; }

/* ============================================================ nn.npln.ugcstore.v1.Ugcstore
   documents (the player's saved online data) live in memory on the host: name -> serialized Document. a document
   nobody wrote yet is NOT_FOUND (5), which is what a new player gets from Nintendo too */
typedef struct Doc { char name[200]; u8* data; size_t n; struct Doc* next; } Doc;
static Doc* g_docs; static PMutex g_doclk; static int g_doclk_init;
static void doclk(void){ if(!g_doclk_init){ pm_init(&g_doclk); g_doclk_init=1; } pm_lock(&g_doclk); }
static int ugcstore(const char* m, const u8* q, size_t n, PB* out){
  if(!strcmp(m,"GetDocument")){ char name[200]; pb_getstr(q,n,1,name,sizeof name);
    doclk(); Doc* d=g_docs; while(d && strcmp(d->name,name)) d=d->next;
    if(d){ pb_need(out,d->n); memcpy(out->p,d->data,d->n); out->n=d->n; } pm_unlock(&g_doclk);
    LOG("online: ugcstore GetDocument %s -> %s\n",name,d? "found" : "not found");
    return d? 0 : 5; }
  if(!strcmp(m,"CommitDocuments")){
    size_t o=0; const u8* op; size_t opn; int nops=0; i64 now=(i64)time(NULL);
    while(pb_next(q,n,2,&o,&op,&opn)){
      const u8* r; size_t rn; const u8* doc; size_t dn; char name[200];
      if(pb_get(op,opn,1,&r,&rn,NULL) && pb_get(r,rn,1,&doc,&dn,NULL)){          /* update_document.document */
        pb_getstr(doc,dn,1,name,sizeof name);
        PB nd={0}; pb_need(&nd,dn); memcpy(nd.p,doc,dn); nd.n=dn; pb_seconds(&nd,4,now);   /* stamp update_time */
        doclk(); Doc* d=g_docs; while(d && strcmp(d->name,name)) d=d->next;
        if(!d){ d=calloc(1,sizeof *d); snprintf(d->name,sizeof d->name,"%s",name); d->next=g_docs; g_docs=d; }
        free(d->data); d->data=nd.p; d->n=nd.n; pm_unlock(&g_doclk);
        LOG("online: ugcstore wrote %s (%u bytes)\n",name,(unsigned)dn); }
      else if(pb_get(op,opn,2,&r,&rn,NULL)){                                         /* delete_document */
        pb_getstr(r,rn,1,name,sizeof name);
        doclk(); for(Doc** pp=&g_docs;*pp;pp=&(*pp)->next) if(!strcmp((*pp)->name,name)){ Doc* d=*pp; *pp=d->next; free(d->data); free(d); break; } pm_unlock(&g_doclk);
        LOG("online: ugcstore deleted %s\n",name); }
      PB wr={0}; pb_seconds(&wr,1,now); pb_msg(out,1,&wr); nops++; }
    pb_seconds(out,2,now);
    return 0; }
  return -1; }

/* who is calling: the access token of the call's "authorization: Bearer ojam-access-<id>" header (one per thread,
   online.c sets it before each call). requests name their own user "tenants/current/users/current" */
#ifdef _MSC_VER
static __declspec(thread) u64 t_caller;
#else
static __thread u64 t_caller;
#endif
void npln_set_caller(const char* a){ const char* k= a? strstr(a,"ojam-access-") : NULL; t_caller= k? strtoull(k+12,NULL,16) : 0; }
static void real_user(char* u, size_t cap){
  if(t_caller && (!*u || strstr(u,"/users/current"))) snprintf(u,cap,"tenants/t-adf89f68-lp1/users/%016llx",(unsigned long long)t_caller); }

/* ============================================================ nn.npln.matchmaking.v1 (GameSessionService, Matchmaker)
   the host is the matchmaker for every PC that connects to it. the game first looks for a room with a
   MatchmakingTicket and, when none turns up, makes its own with a GameSessionCreationTicket and waits in it as the
   room owner. so: a creation ticket succeeds at once with a session holding only its creator; a matchmaking ticket
   joins the oldest session of the same matchmaking_config that still has room (online_match_size players, 4 by
   default) or times out after online_search_wait seconds. the players then reach each other through Pia, peer to
   peer; the session only lists them. */
#define MM_USERS 8
typedef struct Ticket {
  int kind;                                                                    /* 0 creation, 1 matchmaking */
  char name[96], config[160], parent[96], backfill[96];
  u8* props; size_t propn;                                                     /* creation: game_session.properties, raw */
  u8* defs[MM_USERS]; size_t defn[MM_USERS]; char user[MM_USERS][120]; int nu;   /* user_definitions, raw */
  int state; i64 born; int session;   /* creation: 1 pending 2 succeeded 3 cancelled; matchmaking: 1 searching 3 succeeded 4 timed out 6 cancelled */
  struct Ticket* next; } Ticket;
typedef struct Session { int id; char name[96], config[160]; u8* props; size_t propn; char user[MM_USERS][120]; u8* defs[MM_USERS]; size_t defn[MM_USERS]; int nu; i64 born; struct Session* next; } Session;
static Ticket* g_tk; static Session* g_ss; static int g_tkid=0x100, g_ssid=0x100; static PMutex g_mmlk; static int g_mmlk_init;
static void mmlk(void){ if(!g_mmlk_init){ pm_init(&g_mmlk); g_mmlk_init=1; } pm_lock(&g_mmlk); }
static int mm_size(void){ int k=atoi(ini_value("online_match_size","4")); return k<1? 1 : k>MM_USERS? MM_USERS : k; }
static int mm_wait(void){ return atoi(ini_value("online_search_wait","10")); }

static Ticket* tk_find(const char* name){ for(Ticket* t=g_tk;t;t=t->next) if(!strcmp(t->name,name)) return t; return NULL; }
static Session* ss_find(const char* name){ for(Session* s=g_ss;s;s=s->next) if(!strcmp(s->name,name)) return s; return NULL; }
static Session* ss_id(int id){ for(Session* s=g_ss;s;s=s->next) if(s->id==id) return s; return NULL; }
static int ss_has(const Session* s, const char* user){ for(int i=0;i<s->nu;i++) if(!strcmp(s->user[i],user)) return 1; return 0; }
static void ss_add(Session* s, const Ticket* t){
  for(int i=0;i<t->nu && s->nu<MM_USERS;i++){ if(ss_has(s,t->user[i])) continue;
    snprintf(s->user[s->nu],sizeof s->user[0],"%s",t->user[i]);
    s->defs[s->nu]=malloc(t->defn[i]+1); memcpy(s->defs[s->nu],t->defs[i],t->defn[i]); s->defn[s->nu]=t->defn[i]; s->nu++; } }

/* settles pending tickets (lock held; called from every stream poll) */
static void mm_tick(void){
  i64 now=(i64)time(NULL);
  for(Ticket* t=g_tk;t;t=t->next){ if(t->state!=1) continue;
    if(t->kind==0){
      Session* s=calloc(1,sizeof *s); s->id=g_ssid++; s->born=now; snprintf(s->config,sizeof s->config,"%s",t->config);
      snprintf(s->name,sizeof s->name,"%s/gameSessions/%08x",*t->parent? t->parent : "tenants/current",s->id);
      if(t->propn){ s->props=malloc(t->propn); memcpy(s->props,t->props,t->propn); s->propn=t->propn; }
      ss_add(s,t); s->next=g_ss; g_ss=s; t->state=2; t->session=s->id;
      LOG("online: matchmaking opened room %s for %s\n",s->name,t->user[0]); continue; }
  }
  /* matchmaking tickets searching with the same config are grouped: at once when they fill a room, or when two or
     more have been searching together for online_match_gather seconds. the room is the one the oldest ticket asks
     to backfill (its own room), else a new one. a ticket alone times out after online_search_wait seconds and the
     game tries again */
  for(Ticket* t=g_tk;t;t=t->next){ if(t->kind!=1 || t->state!=1) continue;
    Ticket* grp[MM_USERS]; int ng=0, users=0; i64 newest=0;
    for(Ticket* u=g_tk;u;u=u->next) if(u->kind==1 && u->state==1 && !strcmp(u->config,t->config) && ng<MM_USERS && users+u->nu<=mm_size()){
      grp[ng++]=u; users+=u->nu; if(u->born>newest) newest=u->born; }
    if(ng<2 && users<mm_size()){ if(now-t->born>=mm_wait()) t->state=4; continue; }
    if(users<mm_size() && now-newest<atoi(ini_value("online_match_gather","3"))) continue;
    Ticket* old=grp[0]; for(int i=1;i<ng;i++) if(grp[i]->born<old->born) old=grp[i];
    Session* s= *old->backfill? ss_find(old->backfill) : NULL;
    if(!s) for(Session* r=g_ss;r;r=r->next) if(r->nu && !strcmp(r->user[0],old->user[0])){ s=r; break; }   /* list is newest first */
    if(s && s->nu+users-1>mm_size()) s=NULL;
    if(!s){ s=calloc(1,sizeof *s); s->id=g_ssid++; s->born=now; snprintf(s->config,sizeof s->config,"%s",t->config);
      snprintf(s->name,sizeof s->name,"%s/gameSessions/%08x",*t->parent? t->parent : "tenants/current",s->id); s->next=g_ss; g_ss=s; }
    for(int i=0;i<ng;i++){ ss_add(s,grp[i]); grp[i]->state=3; grp[i]->session=s->id; }
    LOG("online: matchmaking put %d player(s) in room %s (%d/%d)\n",users,s->name,s->nu,mm_size()); } }

/* a JWT-shaped matchmaking id token (unsigned placeholder signature): the SDK may decode it */
static void b64u(const u8* d, size_t n, char* o){
  static const char t[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"; size_t k=0;
  for(size_t i=0;i<n;i+=3){ u32 v=(u32)d[i]<<16 | (i+1<n? (u32)d[i+1]<<8 : 0) | (i+2<n? d[i+2] : 0);
    o[k++]=t[v>>18&63]; o[k++]=t[v>>12&63]; if(i+1<n) o[k++]=t[v>>6&63]; if(i+2<n) o[k++]=t[v&63]; }
  o[k]=0; }
static void mm_token(char* o, size_t cap, const Session* s, const char* us, const char* user){
  char js[700], body[1000]; i64 now=(i64)time(NULL);
  snprintf(js,sizeof js,"{\"iss\":\"https://t-adf89f68-lp1.lp1.t.npln.srv.nintendo.net\",\"sub\":\"%s\",\"aud\":\"%s\",\"iat\":%lld,\"exp\":%lld,"
    "\"typ\":\"matchmaking_id_token\",\"game_session\":\"%s\",\"user_session\":\"%s\"}",user,s->name,(long long)now,(long long)now+86400,s->name,us);
  b64u((const u8*)js,strlen(js),body);
  snprintf(o,cap,"eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9.%s.b3BlbmphbWJvcmVlLWxvY2FsLXNpZ25hdHVyZQ",body); }

static void user_session_name(char* o, size_t cap, const Session* s, int i){ snprintf(o,cap,"%s/userSessions/%02x",s->name,i); }
static void user_session_msg(PB* u, const Session* s, int i){ char nm[140]; user_session_name(nm,sizeof nm,s,i);
  pb_str(u,1,nm); pb_str(u,2,s->user[i]); pb_seconds(u,4,s->born); pb_int(u,5,2); }
/* nn.npln.matchmaking.v1.GameSession */
static void session_msg(PB* out, const Session* s){
  pb_str(out,1,s->name); pb_int(out,2,(u64)mm_size()); pb_int(out,3,(u64)s->nu); pb_int(out,4,s->nu<mm_size());
  pb_int(out,7,2);                                                          /* ACTIVE */
  pb_seconds(out,10,s->born);
  if(s->propn) pb_bytes(out,11,s->props,s->propn);
  for(int i=0;i<s->nu;i++){ PB u={0}; user_session_msg(&u,s,i); pb_msg(out,12,&u); } }
/* MatchedUserSession list for the users of one ticket (or all of a session when t is NULL) */
static void matched_msgs(PB* out, u32 field, const Session* s, const Ticket* t){
  for(int i=0;i<s->nu;i++){
    if(t){ int mine=0; for(int k=0;k<t->nu;k++) if(!strcmp(t->user[k],s->user[i])) mine=1; if(!mine) continue; }
    PB m={0}; char nm[140], tok[1200]; user_session_name(nm,sizeof nm,s,i);
    pb_bytes(&m,1,s->defs[i],s->defn[i]); pb_str(&m,2,nm); mm_token(tok,sizeof tok,s,nm,s->user[i]); pb_str(&m,3,tok);
    pb_msg(out,field,&m); } }
/* GameSessionCreationTicket {name 1, config 2, defs 3, state 4, matched 5, session 6} or
   MatchmakingTicket {name 1, config 2, backfill 3, defs 4, state 5, matched 6, session 7} */
static void ticket_msg(PB* out, const Ticket* t){
  int k=t->kind; pb_str(out,1,t->name); pb_str(out,2,t->config);
  for(int i=0;i<t->nu;i++) pb_bytes(out,k? 4 : 3,t->defs[i],t->defn[i]);
  pb_int(out,k? 5 : 4,(u64)t->state);
  Session* s= (t->state==2 && !k) || (t->state==3 && k)? ss_id(t->session) : NULL;
  if(s){ matched_msgs(out,k? 6 : 5,s,t); PB g={0}; session_msg(&g,s); pb_msg(out,k? 7 : 6,&g); } }

static int ticket_new(int kind, const u8* q, size_t n, PB* out){
  const u8* tk; size_t tn; Ticket* t=calloc(1,sizeof *t); t->kind=kind; pb_getstr(q,n,1,t->parent,sizeof t->parent);
  if(pb_get(q,n,2,&tk,&tn,NULL)){ pb_getstr(tk,tn,2,t->config,sizeof t->config);
    const u8* bf; size_t bn; if(kind && pb_get(tk,tn,3,&bf,&bn,NULL)) pb_getstr(bf,bn,1,t->backfill,sizeof t->backfill);
    const u8* gs; size_t gn; const u8* pr; size_t pn;
    if(!kind && pb_get(tk,tn,6,&gs,&gn,NULL) && pb_get(gs,gn,11,&pr,&pn,NULL)){ t->props=malloc(pn+1); memcpy(t->props,pr,pn); t->propn=pn; }
    size_t o=0; const u8* d; size_t dn;
    while(t->nu<MM_USERS && pb_next(tk,tn,kind? 4 : 3,&o,&d,&dn)){ t->defs[t->nu]=malloc(dn+1); memcpy(t->defs[t->nu],d,dn); t->defn[t->nu]=dn;
      pb_getstr(d,dn,1,t->user[t->nu],sizeof t->user[0]); real_user(t->user[t->nu],sizeof t->user[0]); t->nu++; } }
  { static int said; if(said<4){ said++; char hx[3*400+1]={0}; size_t sh= n>400? 400 : n; for(size_t i=0;i<sh;i++) sprintf(hx+i*3,"%02x ",q[i]);
      LOG("online: %s ticket request (%u bytes): %s\n",kind? "matchmaking" : "room",(unsigned)n,hx); } }
  t->state=1; t->born=(i64)time(NULL);
  mmlk(); snprintf(t->name,sizeof t->name,"%s/%s/%08x",*t->parent? t->parent : "tenants/current",kind? "matchmakingTickets" : "gameSessionCreationTickets",g_tkid++);
  t->next=g_tk; g_tk=t; ticket_msg(out,t); pm_unlock(&g_mmlk);
  LOG("online: %s ticket %s for %s, config %s\n",kind? "matchmaking" : "room",t->name,t->nu? t->user[0] : "?",t->config);
  return 0; }

static int gamesession(const char* m, const u8* q, size_t n, PB* out){
  if(!strcmp(m,"ListLatencyMeasurementServers")) return 0;                 /* none: latencies stay empty */
  if(!strcmp(m,"CreateGameSessionCreationTicket")) return ticket_new(0,q,n,out);
  if(!strcmp(m,"CancelGameSessionCreationTicket")){ char nm[96]; pb_getstr(q,n,1,nm,sizeof nm);
    mmlk(); Ticket* t=tk_find(nm); if(t && t->state==1) t->state=3; pm_unlock(&g_mmlk); LOG("online: room ticket %s cancelled\n",nm); return 0; }
  if(!strcmp(m,"GetGameSession")){ char nm[96]; pb_getstr(q,n,1,nm,sizeof nm);
    mmlk(); Session* s=ss_find(nm); if(s) session_msg(out,s); pm_unlock(&g_mmlk); return s? 0 : 5; }
  if(!strcmp(m,"SyncGameSession")){ const u8* g; size_t gn; char nm[96]="";
    if(pb_get(q,n,1,&g,&gn,NULL)) pb_getstr(g,gn,1,nm,sizeof nm);
    mmlk(); Session* s=ss_find(nm); if(s) for(int i=0;i<s->nu;i++){ PB u={0}; user_session_msg(&u,s,i); pb_msg(out,1,&u); }
    pm_unlock(&g_mmlk); return 0; }
  if(!strcmp(m,"IssueMatchmakingIdToken")){ char nm[96]; pb_getstr(q,n,1,nm,sizeof nm);
    mmlk(); Session* s=ss_find(nm); if(s){ matched_msgs(out,1,s,NULL); PB g={0}; session_msg(&g,s); pb_msg(out,2,&g); } pm_unlock(&g_mmlk); return s? 0 : 5; }
  return -1; }
static int matchmaker(const char* m, const u8* q, size_t n, PB* out){
  if(!strcmp(m,"CreateMatchmakingTicket")) return ticket_new(1,q,n,out);
  if(!strcmp(m,"CancelMatchmakingTicket")){ char nm[96]; pb_getstr(q,n,1,nm,sizeof nm);
    mmlk(); Ticket* t=tk_find(nm); if(t && t->state==1) t->state=6; pm_unlock(&g_mmlk); LOG("online: matchmaking ticket %s cancelled\n",nm); return 0; }
  return -1; }

/* server streams (Track*Ticket): the ticket now, then again whenever its state changes; the stream ends after a
   final state. npln_stream_poll is called by online.c a few times a second */
typedef struct { char name[96]; int sent; } Track;
void* npln_stream_open(const char* path, const u8* req, size_t n){
  if(!strstr(path,"/TrackGameSessionCreationTicket") && !strstr(path,"/TrackMatchmakingTicket")) return NULL;
  Track* k=calloc(1,sizeof *k); pb_getstr(req,n,1,k->name,sizeof k->name); k->sent=-1;
  LOG("online: tracking %s\n",k->name); return k; }
/* 1 = a new message in *out (caller frees); *fin = 1 when the stream should end (after this message, if any) */
int npln_stream_poll(void* h, u8** out, size_t* outn, int* fin){
  Track* k=h; *fin=0; mmlk(); mm_tick(); Ticket* t=tk_find(k->name); int r=0;
  if(!t){ *fin=1; pm_unlock(&g_mmlk); return 0; }
  if(t->state!=k->sent){ PB b={0}; ticket_msg(&b,t); *out=b.p; *outn=b.n; k->sent=t->state; r=1;
    LOG("online: ticket %s -> state %d\n",t->name,t->state); }
  if(t->state!=1) *fin=1;
  pm_unlock(&g_mmlk); return r; }
void npln_stream_close(void* h){ free(h); }

/* ============================================================ dispatch */
int npln_call(const char* path, const u8* req, size_t n, int* status, u8** out, size_t* outn){
  /* "/nn.npln.<svc>.v1.<Service>/<Method>" */
  const char* m=strrchr(path,'/'); if(!m) return -1; m++;
  PB b={0}; int r=-1;
  if(strstr(path,".auth.v1.Auth/")) r=auth(m,req,n,&b);
  else if(strstr(path,".ugcstore.v1.Ugcstore/")) r=ugcstore(m,req,n,&b);
  else if(strstr(path,".matchmaking.v1.GameSessionService/")) r=gamesession(m,req,n,&b);
  else if(strstr(path,".matchmaking.v1.Matchmaker/")) r=matchmaker(m,req,n,&b);
  if(r<0){ free(b.p); return -1; }
  *status=r; *out=b.p; *outn=b.n; return 0; }
