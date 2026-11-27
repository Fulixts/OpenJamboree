/* OpenJamboree online play: the host's own game plays Nintendo's NPLN servers for the players on its ZeroTier network
   (docs/ONLINE_SPEC.md). Every *.nintendo.net name points at the host (lan.c sfdnsres), port 443 on the host is
   remapped to online_port, and this server answers there: TLS (Mbed TLS, our CA's certificate from <exe>_online/),
   HTTP/2 (nghttp2) and gRPC on top. This first version logs every gRPC call and answers UNIMPLEMENTED, so the game's
   order of calls can be read from the log before the services are written. */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "psa/crypto.h"
#include "mbedtls/debug.h"
#define NGHTTP2_STATICLIB
#include "nghttp2/nghttp2.h"

extern const char* ini_value(const char*, const char*);

u16 online_port(void){ static int p; if(!p){ p=atoi(ini_value("online_port","47443")); if(p<=0 || p>65535) p=47443; } return (u16)p; }

/* ============================================================ TLS setup (once) */
static mbedtls_entropy_context g_ent; static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_x509_crt g_crt; static mbedtls_pk_context g_key; static mbedtls_ssl_config g_conf;
static const char* g_alpn[]={"h2",NULL};
static int g_tls_ok;
/* online_tls_debug=1..4: Mbed TLS's own trace of each handshake in the log (alerts, chosen cipher, certificate checks) */
static void tls_dbg(void* u, int lv, const char* f, int line, const char* m){ (void)u; (void)f; (void)line; LOG("tls[%d]: %s",lv,m); }

static int tls_init(void){
  char d[1100], c[1200], k[1200]; snprintf(d,sizeof d,"%s%s_online",g_exedir,g_exename);
  snprintf(c,sizeof c,"%s/leaf.pem",d); snprintf(k,sizeof k,"%s/leaf.key",d);
  psa_crypto_init();
  mbedtls_entropy_init(&g_ent); mbedtls_ctr_drbg_init(&g_drbg); mbedtls_x509_crt_init(&g_crt); mbedtls_pk_init(&g_key); mbedtls_ssl_config_init(&g_conf);
  int r=mbedtls_ctr_drbg_seed(&g_drbg,mbedtls_entropy_func,&g_ent,(const u8*)"openjamboree",12);
  if(!r) r=mbedtls_x509_crt_parse_file(&g_crt,c);
  if(!r) r=mbedtls_pk_parse_keyfile(&g_key,k,NULL,mbedtls_ctr_drbg_random,&g_drbg);
  if(!r) r=mbedtls_ssl_config_defaults(&g_conf,MBEDTLS_SSL_IS_SERVER,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT);
  if(!r){ mbedtls_ssl_conf_rng(&g_conf,mbedtls_ctr_drbg_random,&g_drbg); r=mbedtls_ssl_conf_own_cert(&g_conf,&g_crt,&g_key); }
  if(!r) r=mbedtls_ssl_conf_alpn_protocols(&g_conf,g_alpn);
  { int dl=atoi(ini_value("online_tls_debug","0")); if(dl>0){ mbedtls_debug_set_threshold(dl); mbedtls_ssl_conf_dbg(&g_conf,tls_dbg,NULL); } }
  if(r){ char e[160]; mbedtls_strerror(r,e,sizeof e); LOG("online: server TLS setup failed (%s): %s\n",c,e); return 0; }
  return 1; }

/* ============================================================ one connection */
typedef struct Stream { int32_t id; char path[160]; u8* body; size_t n, cap; u8* resp; size_t rn, ro; int status; struct Stream* next; } Stream;
typedef struct { SOCKET s; mbedtls_ssl_context ssl; nghttp2_session* h2; Stream* st; } Conn;

static int bio_send(void* ctx, const unsigned char* b, size_t n){ int r=send(*(SOCKET*)ctx,(const char*)b,(int)n,0); return r<0? MBEDTLS_ERR_NET_SEND_FAILED : r; }
static int bio_recv(void* ctx, unsigned char* b, size_t n){ int r=recv(*(SOCKET*)ctx,(char*)b,(int)n,0); return r<0? MBEDTLS_ERR_NET_RECV_FAILED : r==0? MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY : r; }

static Stream* stream_get(Conn* c, int32_t id, int make){
  for(Stream* s=c->st;s;s=s->next) if(s->id==id) return s;
  if(!make) return NULL;
  Stream* s=calloc(1,sizeof *s); s->id=id; s->next=c->st; c->st=s; return s; }
static void stream_drop(Conn* c, int32_t id){
  for(Stream** p=&c->st;*p;p=&(*p)->next) if((*p)->id==id){ Stream* s=*p; *p=s->next; free(s->body); free(s->resp); free(s); return; } }

#define NV(n,v) { (uint8_t*)(n), (uint8_t*)(v), sizeof(n)-1, strlen(v), NGHTTP2_NV_FLAG_NONE }

/* response body: the gRPC message (5-byte prefix + protobuf), then the trailers with grpc-status */
static nghttp2_ssize resp_read(nghttp2_session* h, int32_t id, uint8_t* buf, size_t len, uint32_t* fl, nghttp2_data_source* src, void* u){ (void)u;
  Stream* s=src->ptr; size_t k=s->rn-s->ro; if(k>len) k=len; memcpy(buf,s->resp+s->ro,k); s->ro+=k;
  if(s->ro>=s->rn){ *fl|=NGHTTP2_DATA_FLAG_EOF|NGHTTP2_DATA_FLAG_NO_END_STREAM;
    char st[12]; snprintf(st,sizeof st,"%d",s->status);
    nghttp2_nv t[]={ { (uint8_t*)"grpc-status",(uint8_t*)st,11,strlen(st),NGHTTP2_NV_FLAG_NONE } };
    nghttp2_submit_trailer(h,id,t,1); }
  return (nghttp2_ssize)k; }

/* a complete request: answer it from npln.c, or UNIMPLEMENTED (logged) for methods not written yet */
extern int npln_call(const char* path, const u8* req, size_t n, int* status, u8** out, size_t* outn);
static void grpc_call(Conn* c, Stream* s){
  const u8* msg= s->n>=5? s->body+5 : (const u8*)""; size_t mn= s->n>=5? s->n-5 : 0;
  int st=0; u8* out=NULL; size_t on=0;
  if(npln_call(s->path,msg,mn,&st,&out,&on)<0){
    char hex[3*48+1]={0}; size_t show= mn>48? 48 : mn; for(size_t i=0;i<show;i++) sprintf(hex+i*3,"%02x ",msg[i]);
    LOG("online: grpc %s (%u bytes) not implemented: %s\n",s->path,(unsigned)mn,hex);
    nghttp2_nv h[]={ NV(":status","200"), NV("content-type","application/grpc"), NV("grpc-status","12"), NV("grpc-message","not implemented by the OpenJamboree host yet") };
    nghttp2_submit_response2(c->h2,s->id,h,sizeof h/sizeof h[0],NULL); return; }
  LOG("online: grpc %s -> %u bytes, status %d\n",s->path,(unsigned)on,st);
  if(st){ /* errors go out trailers-only: no message, grpc-status in the one HEADERS frame */
    char sts[12]; snprintf(sts,sizeof sts,"%d",st); free(out);
    nghttp2_nv eh[]={ NV(":status","200"), NV("content-type","application/grpc"), { (uint8_t*)"grpc-status",(uint8_t*)sts,11,strlen(sts),NGHTTP2_NV_FLAG_NONE } };
    nghttp2_submit_response2(c->h2,s->id,eh,3,NULL); return; }
  s->status=st; s->rn=5+on; s->ro=0; s->resp=malloc(s->rn);
  s->resp[0]=0; s->resp[1]=(u8)(on>>24); s->resp[2]=(u8)(on>>16); s->resp[3]=(u8)(on>>8); s->resp[4]=(u8)on; if(on) memcpy(s->resp+5,out,on); free(out);
  nghttp2_nv h[]={ NV(":status","200"), NV("content-type","application/grpc") };
  nghttp2_data_provider2 dp; dp.source.ptr=s; dp.read_callback=resp_read;
  nghttp2_submit_response2(c->h2,s->id,h,2,&dp); }

static int on_begin_headers(nghttp2_session* h, const nghttp2_frame* f, void* u){ (void)h;
  if(f->hd.type==NGHTTP2_HEADERS && f->headers.cat==NGHTTP2_HCAT_REQUEST) stream_get((Conn*)u,f->hd.stream_id,1); return 0; }
static int on_header(nghttp2_session* h, const nghttp2_frame* f, const uint8_t* n, size_t nl, const uint8_t* v, size_t vl, uint8_t fl, void* u){ (void)h; (void)fl;
  Stream* s=stream_get((Conn*)u,f->hd.stream_id,0); if(!s) return 0;
  if(nl==5 && !memcmp(n,":path",5)){ size_t k= vl<sizeof s->path-1? vl : sizeof s->path-1; memcpy(s->path,v,k); s->path[k]=0; }
  return 0; }
static int on_data(nghttp2_session* h, uint8_t fl, int32_t id, const uint8_t* d, size_t n, void* u){ (void)h; (void)fl;
  Stream* s=stream_get((Conn*)u,id,0); if(!s) return 0;
  if(s->n+n>s->cap){ s->cap=(s->n+n)*2+256; s->body=realloc(s->body,s->cap); } memcpy(s->body+s->n,d,n); s->n+=n; return 0; }
static int on_frame(nghttp2_session* h, const nghttp2_frame* f, void* u){ (void)h;
  if((f->hd.type==NGHTTP2_HEADERS || f->hd.type==NGHTTP2_DATA) && (f->hd.flags&NGHTTP2_FLAG_END_STREAM)){
    Stream* s=stream_get((Conn*)u,f->hd.stream_id,0); if(s) grpc_call((Conn*)u,s); }
  return 0; }
static int on_close(nghttp2_session* h, int32_t id, uint32_t e, void* u){ (void)h; (void)e; stream_drop((Conn*)u,id); return 0; }

static int flush_h2(Conn* c){
  for(;;){ const uint8_t* d; nghttp2_ssize n=nghttp2_session_mem_send2(c->h2,&d); if(n<0) return -1; if(n==0) return 0;
    for(nghttp2_ssize o=0;o<n;){ int w=mbedtls_ssl_write(&c->ssl,d+o,(size_t)(n-o)); if(w<0){ if(w==MBEDTLS_ERR_SSL_WANT_WRITE) continue; return -1; } o+=w; } } }

static void conn_thread(void* arg){
  Conn* c=arg; char who[64]="?";
  { struct sockaddr_in a; socklen_t l=sizeof a; if(!getpeername(c->s,(struct sockaddr*)&a,&l)) snprintf(who,sizeof who,"%s:%u",inet_ntoa(a.sin_addr),ntohs(a.sin_port)); }
  mbedtls_ssl_init(&c->ssl);
  if(mbedtls_ssl_setup(&c->ssl,&g_conf)) goto out;
  mbedtls_ssl_set_bio(&c->ssl,&c->s,bio_send,bio_recv,NULL);
  int r; while((r=mbedtls_ssl_handshake(&c->ssl))!=0){ if(r!=MBEDTLS_ERR_SSL_WANT_READ && r!=MBEDTLS_ERR_SSL_WANT_WRITE){ char e[160]; mbedtls_strerror(r,e,sizeof e); LOG("online: TLS handshake with %s failed: %s\n",who,e); goto out; } }
  const char* alpn=mbedtls_ssl_get_alpn_protocol(&c->ssl);
  LOG("online: %s connected (%s, ALPN %s)\n",who,mbedtls_ssl_get_version(&c->ssl),alpn? alpn : "none");
  nghttp2_session_callbacks* cb; nghttp2_session_callbacks_new(&cb);
  nghttp2_session_callbacks_set_on_begin_headers_callback(cb,on_begin_headers);
  nghttp2_session_callbacks_set_on_header_callback(cb,on_header);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cb,on_data);
  nghttp2_session_callbacks_set_on_frame_recv_callback(cb,on_frame);
  nghttp2_session_callbacks_set_on_stream_close_callback(cb,on_close);
  nghttp2_session_server_new(&c->h2,cb,c); nghttp2_session_callbacks_del(cb);
  nghttp2_settings_entry se[]={{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS,100}};
  nghttp2_submit_settings(c->h2,NGHTTP2_FLAG_NONE,se,1);
  if(flush_h2(c)) goto done;
  u8 buf[16384];
  for(;;){
    int n=mbedtls_ssl_read(&c->ssl,buf,sizeof buf);
    if(n==MBEDTLS_ERR_SSL_WANT_READ || n==MBEDTLS_ERR_SSL_WANT_WRITE) continue;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
    if(n==MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
#endif
    if(n<=0) break;
    if(nghttp2_session_mem_recv2(c->h2,buf,(size_t)n)<0) break;
    if(flush_h2(c)) break;
    if(!nghttp2_session_want_read(c->h2) && !nghttp2_session_want_write(c->h2)) break; }
done:
  LOG("online: %s disconnected\n",who);
  nghttp2_session_del(c->h2);
  while(c->st) stream_drop(c,c->st->id);
out:
  mbedtls_ssl_free(&c->ssl); closesocket(c->s); free(c); }

/* ============================================================ listener */
static void listen_thread(void* arg){ (void)arg;
  SOCKET l=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); int one=1; setsockopt(l,SOL_SOCKET,SO_REUSEADDR,(const char*)&one,sizeof one);
  struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_port=htons(online_port()); a.sin_addr.s_addr=INADDR_ANY;
  if(bind(l,(struct sockaddr*)&a,sizeof a) || listen(l,16)){ LOG("online: port %u is busy, this PC cannot host\n",online_port()); closesocket(l); return; }
  LOG("online: hosting on port %u (TLS + HTTP/2 + gRPC)\n",online_port());
  for(;;){
    SOCKET s=accept(l,NULL,NULL); if(s==INVALID_SOCKET) continue;
    setsockopt(s,IPPROTO_TCP,TCP_NODELAY,(const char*)&one,sizeof one);
    Conn* c=calloc(1,sizeof *c); c->s=s; plat_thread(conn_thread,c,1<<20); } }

/* ============================================================ Pia's NAT check
   (nn::pia NatPropertyDetecter, the nncs1/nncs2 servers, resolved to the host): 16-byte requests whose first
   big-endian word is the kind, 101 and 102 to nncs1, 103 to nncs2, on UDP 10025 and 10125. the answer is the kind,
   then the port and address the request came from as the host sees them; 102 is answered from the other port.
   after OpenSmash's nex_datagram (runtime/net_nex_core.c, GPL-3.0-or-later, commit 657a4d06) */
static void nat_thread(void* arg){ (void)arg;
  static const u16 ports[2]={10025,10125}; SOCKET s[2];
  for(int i=0;i<2;i++){ s[i]=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP); struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_port=htons(ports[i]); a.sin_addr.s_addr=INADDR_ANY;
    if(bind(s[i],(struct sockaddr*)&a,sizeof a)){ LOG("online: NAT check port %u is busy\n",ports[i]); return; } }
  LOG("online: NAT check on UDP %u and %u\n",ports[0],ports[1]);
  for(;;){
    fd_set rs; FD_ZERO(&rs); FD_SET(s[0],&rs); FD_SET(s[1],&rs); SOCKET mx= s[0]>s[1]? s[0] : s[1];
    if(select((int)mx+1,&rs,NULL,NULL,NULL)<=0) continue;
    for(int i=0;i<2;i++){ if(!FD_ISSET(s[i],&rs)) continue;
      u8 b[64]; struct sockaddr_in f; socklen_t fl=sizeof f; int n=recvfrom(s[i],(char*)b,sizeof b,0,(struct sockaddr*)&f,&fl);
      if(n<16) continue;
      u32 kind=(u32)b[0]<<24|(u32)b[1]<<16|(u32)b[2]<<8|b[3]; if(kind<101 || kind>103) continue;
      u16 port=ntohs(f.sin_port); u32 ip=ntohl(f.sin_addr.s_addr);
      u8 r[16]={b[0],b[1],b[2],b[3],0,0,(u8)(port>>8),(u8)port,(u8)(ip>>24),(u8)(ip>>16),(u8)(ip>>8),(u8)ip,0,0,0,0};
      SOCKET from= kind==102? s[i^1] : s[i];
      sendto(from,(const char*)r,16,0,(struct sockaddr*)&f,fl);
      static int said; if(said<12){ said++; LOG("online: NAT check %u from %s:%u\n",kind,inet_ntoa(f.sin_addr),port); } } } }

/* started once, by the first online call, on the PC that hosts (online_host empty or one of its own addresses) */
void online_server_start(void){
  static int started; if(started) return; started=1;
  const char* h=ini_value("online_host",""); extern u32 lan_ip(void);
  if(*h){ unsigned x,y,z,w; u32 ip= sscanf(h,"%u.%u.%u.%u",&x,&y,&z,&w)==4? (x<<24)|(y<<16)|(z<<8)|w : 0; if(ip && ip!=lan_ip() && (ip>>24)!=127) return; }
#ifdef _WIN32
  WSADATA wd; WSAStartup(MAKEWORD(2,2),&wd);
#endif
  if(!tls_init()) return;
  g_tls_ok=1;
  plat_thread(listen_thread,NULL,1<<20);
  plat_thread(nat_thread,NULL,1<<20); }
