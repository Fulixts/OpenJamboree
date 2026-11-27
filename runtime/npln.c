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

/* ============================================================ dispatch */
int npln_call(const char* path, const u8* req, size_t n, int* status, u8** out, size_t* outn){
  /* "/nn.npln.<svc>.v1.<Service>/<Method>" */
  const char* m=strrchr(path,'/'); if(!m) return -1; m++;
  PB b={0}; int r=-1;
  if(strstr(path,".auth.v1.Auth/")) r=auth(m,req,n,&b);
  else if(strstr(path,".ugcstore.v1.Ugcstore/")) r=ugcstore(m,req,n,&b);
  if(r<0){ free(b.p); return -1; }
  *status=r; *out=b.p; *outn=b.n; return 0; }
