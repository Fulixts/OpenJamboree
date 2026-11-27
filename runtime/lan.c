/* OpenJamboree LAN play: the Switch's local wireless (ldn:u) over a real LAN or ZeroTier, with the ldn_mitm protocol
   (UDP/TCP 11452), plus the game's own sockets (bsd:u) on host sockets. No server: every PC talks to the others
   directly. Spec, offsets and the references this follows: docs/LAN_SPEC.md.

   One adapter is picked for everything (lan_interface= in the .ini, else a ZeroTier adapter, else the default route):
   its IP is the node IP the game sees, the source of its traffic and the base of the subnet broadcast. lan_peers= lists
   other PCs that also get every broadcast as unicast, for VPNs that don't carry broadcast and for two instances on one
   PC (127.0.0.1 and 127.0.0.2). */
#ifdef _WIN32
#include <winsock2.h>   /* before rt.h, which pulls windows.h */
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR,12)
#endif
#endif
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32

#define CLOSESOCK closesocket
#define SOCK_ERR WSAGetLastError()
#else
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define CLOSESOCK close
#define SOCK_ERR errno
#endif

extern const char* ini_value(const char*, const char*);
static u32 online_host_ip(void);
#define plat_sleep_ms(ms) plat_sleep_ns((i64)(ms)*1000000)

/* ============================================================ adapter */
typedef struct { u32 ip, mask; char name[96]; int zt; } NetIf;   /* ip and mask in host order */
static NetIf g_if; static int g_if_ready;
static u32 g_peers[16]; static int g_npeers;

static void net_startup(void){
#ifdef _WIN32
  static int done; if(!done){ WSADATA w; WSAStartup(MAKEWORD(2,2),&w); done=1; }
#endif
}

int lan_list_ifaces(NetIf* out, int max){
  int n=0;
#ifdef _WIN32
  ULONG sz=32768; IP_ADAPTER_ADDRESSES* a=malloc(sz);
  if(GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,NULL,a,&sz)==ERROR_BUFFER_OVERFLOW){ free(a); a=malloc(sz);
    if(GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,NULL,a,&sz)!=NO_ERROR){ free(a); return 0; } }
  for(IP_ADAPTER_ADDRESSES* p=a;p && n<max;p=p->Next){
    if(p->OperStatus!=IfOperStatusUp || p->IfType==IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for(IP_ADAPTER_UNICAST_ADDRESS* u=p->FirstUnicastAddress;u && n<max;u=u->Next){
      struct sockaddr_in* s=(struct sockaddr_in*)u->Address.lpSockaddr; u32 ip=ntohl(s->sin_addr.s_addr);
      if((ip>>16)==0xA9FE) continue;   /* 169.254 link-local */
      u32 pl=u->OnLinkPrefixLength; out[n].ip=ip; out[n].mask= pl? 0xFFFFFFFFu<<(32-pl) : 0;
      WideCharToMultiByte(CP_UTF8,0,p->FriendlyName,-1,out[n].name,sizeof out[n].name,NULL,NULL);
      char desc[160]; WideCharToMultiByte(CP_UTF8,0,p->Description,-1,desc,sizeof desc,NULL,NULL);
      out[n].zt= strstr(desc,"ZeroTier")!=NULL || strstr(out[n].name,"ZeroTier")!=NULL; n++; } }
  free(a);
#else
  struct ifaddrs* l; if(getifaddrs(&l)) return 0;
  for(struct ifaddrs* p=l;p && n<max;p=p->ifa_next){
    if(!p->ifa_addr || p->ifa_addr->sa_family!=AF_INET || (p->ifa_flags&IFF_LOOPBACK) || !(p->ifa_flags&IFF_UP)) continue;
    u32 ip=ntohl(((struct sockaddr_in*)p->ifa_addr)->sin_addr.s_addr); if((ip>>16)==0xA9FE) continue;
    out[n].ip=ip; out[n].mask= p->ifa_netmask? ntohl(((struct sockaddr_in*)p->ifa_netmask)->sin_addr.s_addr) : 0xFFFFFF00u;
    snprintf(out[n].name,sizeof out[n].name,"%s",p->ifa_name); out[n].zt= !strncmp(p->ifa_name,"zt",2); n++; }
  freeifaddrs(l);
#endif
  return n; }

static u32 parse_ip(const char* s){ unsigned a,b,c,d; if(sscanf(s,"%u.%u.%u.%u",&a,&b,&c,&d)!=4) return 0; return (a<<24)|(b<<16)|(c<<8)|d; }
static void ip_str(u32 ip, char* o){ sprintf(o,"%u.%u.%u.%u",ip>>24,(ip>>16)&255,(ip>>8)&255,ip&255); }

/* the default route's source address: a UDP "connect" to a public address picks it without sending anything */
static u32 default_route_ip(void){
  SOCKET s=socket(AF_INET,SOCK_DGRAM,0); if(s==INVALID_SOCKET) return 0;
  struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_port=htons(53); a.sin_addr.s_addr=htonl(0x08080808);
  u32 ip=0; if(!connect(s,(struct sockaddr*)&a,sizeof a)){ struct sockaddr_in m; socklen_t l=sizeof m; if(!getsockname(s,(struct sockaddr*)&m,&l)) ip=ntohl(m.sin_addr.s_addr); }
  CLOSESOCK(s); return ip; }

void lan_pick(void){
  net_startup();
  NetIf l[32]; int n=lan_list_ifaces(l,32);
  const char* want=ini_value("lan_interface","");
  memset(&g_if,0,sizeof g_if);
  u32 w= *want? parse_ip(want) : 0;
  if(w && (w>>24)==127){ g_if.ip=w; g_if.mask=0xFF000000u; snprintf(g_if.name,sizeof g_if.name,"loopback (test)"); }
  for(int i=0;i<n && !g_if.ip && w;i++) if(l[i].ip==w || (l[i].mask && (l[i].ip&l[i].mask)==(w&l[i].mask) && !(w&~l[i].mask))) g_if=l[i];
  for(int i=0;i<n && !g_if.ip && !w;i++) if(l[i].zt) g_if=l[i];
  if(!g_if.ip && !w){ u32 d=default_route_ip(); for(int i=0;i<n && !g_if.ip;i++) if(l[i].ip==d) g_if=l[i]; }
  if(!g_if.ip && n && !w) g_if=l[0];
  if(!g_if.mask) g_if.mask=0xFFFFFF00u;
  g_npeers=0; const char* p=ini_value("lan_peers","");
  while(*p && g_npeers<16){ u32 ip=parse_ip(p); if(ip) g_peers[g_npeers++]=ip; while(*p && *p!=',' && *p!=';' && *p!=' ') p++; while(*p==',' || *p==';' || *p==' ') p++; }
  char a[20],m[20]; ip_str(g_if.ip,a); ip_str(g_if.mask,m);
  if(g_if.ip) LOG("lan: adapter %s, %s mask %s%s, %d peer(s)\n",g_if.name,a,m,g_if.zt? " (ZeroTier)" : "",g_npeers);
  else LOG("lan: no usable network adapter (lan_interface=%s)\n",want);
  g_if_ready=1; }

static void need_if(void){ if(!g_if_ready) lan_pick(); }
u32 lan_ip(void){ need_if(); return g_if.ip; }
u32 lan_mask(void){ need_if(); return g_if.mask; }
const char* lan_if_name(void){ need_if(); return g_if.name; }
static u32 lan_bcast(void){ return g_if.ip | ~g_if.mask; }

static void sock_quiet_udp(SOCKET s){
#ifdef _WIN32
  BOOL f=FALSE; DWORD r; WSAIoctl(s,SIO_UDP_CONNRESET,&f,sizeof f,NULL,0,&r,NULL,NULL);   /* no WSAECONNRESET after an ICMP */
#else
  (void)s;
#endif
}
static void sock_nonblock(SOCKET s, int on){
#ifdef _WIN32
  u_long v=on; ioctlsocket(s,FIONBIO,&v);
#else
  int f=fcntl(s,F_GETFL,0); fcntl(s,F_SETFL,on? f|O_NONBLOCK : f&~O_NONBLOCK);
#endif
}

/* ============================================================ ldn_mitm packets (docs/LAN_SPEC.md 2.2) */
#define LAN_PORT 11452
#define LAN_MAGIC 0x11451400u
enum { PK_SCAN=0, PK_SCANRESP=1, PK_CONNECT=2, PK_SYNC=3 };
#define NI_SIZE 0x480

static int rle_enc(const u8* in, int n, u8* out, int cap){
  int o=0;
  for(int i=0;i<n;){
    if(in[i]){ if(o>=cap) return -1; out[o++]=in[i++]; continue; }
    int z=0; i++; while(i<n && !in[i] && z<255){ z++; i++; }
    if(o+2>cap) return -1; out[o++]=0; out[o++]=(u8)z; }
  return o; }
static int rle_dec(const u8* in, int n, u8* out, int cap){
  int o=0;
  for(int i=0;i<n;i++){
    if(o>=cap) return -1; out[o++]=in[i];
    if(!in[i]){ if(++i>=n) return -1; int z=in[i]; if(o+z>cap) return -1; memset(out+o,0,z); o+=z; } }
  return o; }

static int pk_build(u8* out, int type, const u8* body, int n){
  u8 c[2048]; int cn= n? rle_enc(body,n,c,2048-12) : -1;
  int comp= cn>=0 && cn<n;
  u32 m=LAN_MAGIC; memcpy(out,&m,4); out[4]=(u8)type; out[5]=(u8)comp;
  u16 len=(u16)(comp? cn : n), dl=(u16)(comp? n : 0), z=0;
  memcpy(out+6,&len,2); memcpy(out+8,&dl,2); memcpy(out+10,&z,2);
  memcpy(out+12,comp? c : body,len); return 12+len; }

/* parses one packet at p (n bytes available): returns bytes used, 0 = need more, -1 = garbage */
static int pk_parse(const u8* p, int n, int* type, u8* body, int* bn){
  if(n<12) return 0;
  u32 m; memcpy(&m,p,4); u16 len,dl; memcpy(&len,p+6,2); memcpy(&dl,p+8,2);
  if(m!=LAN_MAGIC || len>2048-12) return -1;
  if(n<12+len) return 0;
  *type=p[4];
  if(p[5]){ int d=rle_dec(p+12,len,body,NI_SIZE); if(d!=dl) return -1; *bn=d; }
  else { if(len>NI_SIZE) return -1; memcpy(body,p+12,len); *bn=len; }
  return 12+len; }

/* ============================================================ ldn state (docs/LAN_SPEC.md 1.3, 1.4) */
enum { ST_NONE, ST_INIT, ST_AP, ST_AP_CREATED, ST_STATION, ST_STATION_CONNECTED, ST_ERROR };
#define LDN_RC(d) RC(203,d)
#define NI_NODE(i) (0x68+(i)*0x40)

typedef struct { SOCKET s; u8 rx[4096]; int rxn; } TcpPeer;

static struct {
  PMutex lk; int init_lk;
  int state; u8 info[NI_SIZE]; KEvent* ev; u8 upd[8]; u8 prev_conn[8]; u32 reason;
  u8 adv[0x180]; u16 advn;
  u8 username[33]; u16 lcver;
  SOCKET udp, listener; TcpPeer st[8]; TcpPeer host;      /* host side: st[1..7]; station side: host */
  int synced;
  u8 scan[24][NI_SIZE]; int nscan, scanning;
  int thread;
} L;

static void lk(void){ if(!L.init_lk){ pm_init(&L.lk); L.init_lk=1; L.udp=L.listener=L.host.s=INVALID_SOCKET; for(int i=0;i<8;i++) L.st[i].s=INVALID_SOCKET; } pm_lock(&L.lk); }
static void ulk(void){ pm_unlock(&L.lk); }
static void signal_ev(void){ if(L.ev) kevent_signal(L.ev); }

static void set_state(int s){ if(L.state!=s){ LOG("lan: ldn state %d -> %d\n",L.state,s); L.state=s; signal_ev(); } }

/* node join/leave bits for GetNetworkInfoLatestUpdate, from the IsConnected flags (M:338-353) */
static int node_bits(void){
  int changed=0;
  for(int i=0;i<8;i++){ u8 c=L.info[NI_NODE(i)+0xB];
    if(c!=L.prev_conn[i]){ L.upd[i]|= c? 1 : 2; L.prev_conn[i]=c; changed=1; } }
  return changed; }

static void fake_mac(u8* m, u32 ip){ m[0]=2; m[1]=0; m[2]=(u8)(ip>>24); m[3]=(u8)(ip>>16); m[4]=(u8)(ip>>8); m[5]=(u8)ip; }

static void my_node(u8* n){
  memset(n,0,0x40); u32 ip=lan_ip(); memcpy(n,&ip,4); fake_mac(n+4,ip); n[0xB]=1;
  memcpy(n+0xC,L.username,32); memcpy(n+0x2E,&L.lcver,2); }

static int send_all(SOCKET s, const u8* p, int n){ while(n>0){ int r=send(s,(const char*)p,n,0); if(r<=0) return -1; p+=r; n-=r; } return 0; }

/* host: recount, push the whole NetworkInfo to every station, update the node bits (M:423-444) */
static void host_update(void){
  int cnt=1;
  for(int i=1;i<8;i++){ u8* nd=L.info+NI_NODE(i); nd[0xA]=(u8)i; if(L.st[i].s!=INVALID_SOCKET && nd[0xB]) cnt++; else if(L.st[i].s==INVALID_SOCKET) memset(nd,0,0x40), nd[0xA]=(u8)i; }
  L.info[0x67]=(u8)cnt;
  u8 pk[2048]; int n=pk_build(pk,PK_SYNC,L.info,NI_SIZE);
  for(int i=1;i<8;i++) if(L.st[i].s!=INVALID_SOCKET && L.info[NI_NODE(i)+0xB]) send_all(L.st[i].s,pk,n);
  if(node_bits()) signal_ev(); }

static void close_peer(TcpPeer* p){ if(p->s!=INVALID_SOCKET){ CLOSESOCK(p->s); p->s=INVALID_SOCKET; } p->rxn=0; }

static void destroy_network(void){
  for(int i=1;i<8;i++) close_peer(&L.st[i]);
  if(L.listener!=INVALID_SOCKET){ CLOSESOCK(L.listener); L.listener=INVALID_SOCKET; } }

static void station_drop(u32 reason){
  close_peer(&L.host); L.synced=0;
  if(L.state==ST_STATION_CONNECTED){ L.reason=reason; memset(L.info,0,NI_SIZE); node_bits(); set_state(ST_STATION); } }

static int in_subnet(u32 ip){ if(((ip^g_if.ip)&g_if.mask)==0) return 1; for(int i=0;i<g_npeers;i++) if(g_peers[i]==ip) return 1; return 0; }

/* ============================================================ network thread (owns every 11452 socket) */
static void tcp_input(TcpPeer* p, int slot){
  int r=recv(p->s,(char*)p->rx+p->rxn,(int)sizeof p->rx-p->rxn,0);
  if(r<=0){
    if(slot>0){ LOG("lan: station %d left\n",slot); close_peer(p); memset(L.info+NI_NODE(slot),0,0x40); host_update(); }
    else { LOG("lan: the host closed the room\n"); station_drop(6 /* SignalLost */); }
    return; }
  p->rxn+=r;
  for(;;){
    int type,bn; u8 body[NI_SIZE];
    int used=pk_parse(p->rx,p->rxn,&type,body,&bn);
    if(used==0) break;
    if(used<0){ p->rxn=0; break; }
    memmove(p->rx,p->rx+used,p->rxn-used); p->rxn-=used;
    if(slot>0 && type==PK_CONNECT && bn==0x40){
      memcpy(L.info+NI_NODE(slot),body,0x40); L.info[NI_NODE(slot)+0xA]=(u8)slot; L.info[NI_NODE(slot)+0xB]=1;
      char a[20]; u32 ip; memcpy(&ip,body,4); ip_str(ip,a); LOG("lan: %.32s joined as node %d (%s)\n",(char*)body+0xC,slot,a);
      host_update(); }
    else if(slot==0 && type==PK_SYNC && bn==NI_SIZE){
      int changed=memcmp(L.info,body,NI_SIZE)!=0; memcpy(L.info,body,NI_SIZE);
      if(!L.synced){ L.synced=1; set_state(ST_STATION_CONNECTED); }
      if(node_bits() || changed) signal_ev(); } }
}

static void udp_input(void){
  u8 buf[2048]; struct sockaddr_in from; socklen_t fl=sizeof from;
  int r=recvfrom(L.udp,(char*)buf,sizeof buf,0,(struct sockaddr*)&from,&fl);
  if(r<=0) return;
  u32 src=ntohl(from.sin_addr.s_addr);
  if(src==g_if.ip || !in_subnet(src)) return;
  int type,bn; u8 body[NI_SIZE];
  if(pk_parse(buf,r,&type,body,&bn)<=0) return;
  if(type==PK_SCAN && L.state==ST_AP_CREATED){
    u8 pk[2048]; int n=pk_build(pk,PK_SCANRESP,L.info,NI_SIZE);
    sendto(L.udp,(const char*)pk,n,0,(struct sockaddr*)&from,fl); }
  else if(type==PK_SCANRESP && bn==NI_SIZE && L.scanning){
    int k=0; for(;k<L.nscan;k++) if(!memcmp(L.scan[k]+0x20,body+0x20,6)) break;
    if(k<24){ memcpy(L.scan[k],body,NI_SIZE); if(k==L.nscan) L.nscan++; } }
}

static void net_thread(void* arg){ (void)arg;
  for(;;){
    fd_set rs; FD_ZERO(&rs); SOCKET mx=0; int any=0;
    lk();
#define ADD(s) do{ if((s)!=INVALID_SOCKET){ FD_SET((s),&rs); if((s)>mx) mx=(s); any=1; } }while(0)
    ADD(L.udp); ADD(L.listener); ADD(L.host.s); for(int i=1;i<8;i++) ADD(L.st[i].s);
    ulk();
    if(!any){ plat_sleep_ms(50); continue; }
    struct timeval tv={0,50000};
    int r=select((int)mx+1,&rs,NULL,NULL,&tv);
    if(r<=0) continue;
    lk();
    if(L.udp!=INVALID_SOCKET && FD_ISSET(L.udp,&rs)) udp_input();
    if(L.listener!=INVALID_SOCKET && FD_ISSET(L.listener,&rs)){
      struct sockaddr_in a; socklen_t al=sizeof a; SOCKET s=accept(L.listener,(struct sockaddr*)&a,&al);
      if(s!=INVALID_SOCKET){ int slot=0; for(int i=1;i<8 && !slot;i++) if(L.st[i].s==INVALID_SOCKET && i<L.info[0x66]) slot=i;
        if(!slot){ CLOSESOCK(s); } else { int one=1; setsockopt(s,IPPROTO_TCP,TCP_NODELAY,(const char*)&one,sizeof one); L.st[slot].s=s; L.st[slot].rxn=0; } } }
    if(L.host.s!=INVALID_SOCKET && FD_ISSET(L.host.s,&rs)) tcp_input(&L.host,0);
    for(int i=1;i<8;i++) if(L.st[i].s!=INVALID_SOCKET && FD_ISSET(L.st[i].s,&rs)) tcp_input(&L.st[i],i);
    ulk(); } }

static int open_udp(void){
  if(L.udp!=INVALID_SOCKET) return 1;
  SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP); if(s==INVALID_SOCKET) return 0;
  int one=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,(const char*)&one,sizeof one); setsockopt(s,SOL_SOCKET,SO_BROADCAST,(const char*)&one,sizeof one);
  sock_quiet_udp(s);
  struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_port=htons(LAN_PORT);
  /* a loopback test address binds to itself so two instances on one PC each get their own 11452 */
  a.sin_addr.s_addr= (g_if.ip>>24)==127? htonl(g_if.ip) : INADDR_ANY;
  if(bind(s,(struct sockaddr*)&a,sizeof a)){ LOG("lan: UDP %d is busy (error %d)\n",LAN_PORT,SOCK_ERR); CLOSESOCK(s); return 0; }
  L.udp=s;
  if(!L.thread){ L.thread=1; plat_thread(net_thread,NULL,1<<20); }
  return 1; }

static void close_udp(void){ if(L.udp!=INVALID_SOCKET){ CLOSESOCK(L.udp); L.udp=INVALID_SOCKET; } }

static void send_scan(void){
  u8 pk[16]; int n=pk_build(pk,PK_SCAN,NULL,0);
  struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_port=htons(LAN_PORT);
  if((g_if.ip>>24)!=127){ a.sin_addr.s_addr=htonl(lan_bcast()); sendto(L.udp,(const char*)pk,n,0,(struct sockaddr*)&a,sizeof a); }
  for(int i=0;i<g_npeers;i++){ a.sin_addr.s_addr=htonl(g_peers[i]); sendto(L.udp,(const char*)pk,n,0,(struct sockaddr*)&a,sizeof a); } }

/* ScanFilter at raw+8 (docs/LAN_SPEC.md 1.3) */
static int scan_match(const u8* ni, const u8* f){
  u32 flag; memcpy(&flag,f+0x5C,4);
  if((flag&1) && memcmp(ni,f,8)) return 0;                       /* LocalCommunicationId */
  if((flag&2) && memcmp(ni+0x10,f+0x10,16)) return 0;            /* SessionId */
  if(flag&4){ u32 t; memcpy(&t,f+0x20,4); if(t!=3 && ni[0x4B]!=t) return 0; }
  if((flag&0x10) && memcmp(ni+0x26,f+0x2A,0x22)) return 0;       /* Ssid */
  if((flag&0x20) && memcmp(ni+0xA,f+0xA,2)) return 0;            /* SceneId */
  if(!ni[NI_NODE(0)+0xC]) return 0;                                /* hosts without a name (R) */
  return 1; }

/* ============================================================ IUserLocalCommunicationService */
static int ldn_svc(SvcObj* o, IpcCtx* c){ (void)o;
  u64 a,s;
  lk();
  int st=L.state, rc=0;
  switch(c->cmd){
    case 0: out32(c,(u32)st); break;
    case 1: if(st!=ST_AP_CREATED && st!=ST_STATION_CONNECTED){ rc=LDN_RC(32); break; }
      buf_out(c,0,&a,&s); if(a && s){ memset(GP(a),0,s); memcpy(GP(a),L.info,s<NI_SIZE? s : NI_SIZE); } break;
    case 2: { u32 ip=lan_ip(), m=lan_mask(); out32(c,ip); out32(c,m); break; }
    case 3: out32(c,L.reason); break;
    case 4: { if(st!=ST_AP_CREATED && st!=ST_STATION_CONNECTED){ rc=LDN_RC(32); break; } u8 sp[0x20]={0}; memcpy(sp+0x10,L.info+0x10,16); outbytes(c,sp,0x20); break; }
    case 5: { if(st!=ST_AP_CREATED && st!=ST_STATION_CONNECTED){ rc=LDN_RC(32); break; }
      u8 nc[0x20]={0}; memcpy(nc,L.info,0x10); memcpy(nc+0x10,L.info+0x48,2); nc[0x12]=L.info[0x66]; memcpy(nc+0x14,L.info+NI_NODE(0)+0x2E,2); outbytes(c,nc,0x20); break; }
    case 100: if(!L.ev) L.ev=kevent_new("ldn state",1); out_copy_h(c,h_new(&L.ev->o)); break;
    case 101: { buf_out(c,0,&a,&s); if(a && s){ memset(GP(a),0,s); memcpy(GP(a),L.info,s<NI_SIZE? s : NI_SIZE); }
      u64 b,bs; buf_out(c,1,&b,&bs); for(u64 i=0;i<8 && b && (i+1)*8<=bs;i++){ memset(GP(b)+i*8,0,8); GP(b)[i*8]=L.upd[i]; }
      memset(L.upd,0,8); break; }
    case 102: case 103: {
      if(st==ST_STATION_CONNECTED || st==ST_ERROR || st==ST_NONE){ rc=LDN_RC(32); break; }
      u8 filt[0x60]; memcpy(filt,c->raw+8,c->raw_size>=0x68? 0x60 : 0);
      L.nscan=0; L.scanning=1; if(open_udp()) send_scan();
      ulk(); plat_sleep_ms(1000); lk();
      L.scanning=0;
      buf_out(c,0,&a,&s); u32 cap=(u32)(s/NI_SIZE), n=0;
      for(int i=0;i<L.nscan && n<cap;i++) if(scan_match(L.scan[i],filt)) memcpy(GP(a)+(u64)n++*NI_SIZE,L.scan[i],NI_SIZE);
      LOG("lan: scan found %d room(s), %u for this game\n",L.nscan,n);
      out32(c,n); break; }
    case 104: case 105: case 106: case 203: case 205: case 208: case 209: case 303: case 403: break;
    case 207: L.info[0x62]=in8(c,0); break;
    case 200: if(st!=ST_INIT){ rc=LDN_RC(32); break; } set_state(ST_AP); break;
    case 201: if(st!=ST_AP && st!=ST_AP_CREATED){ rc=LDN_RC(32); break; } destroy_network(); memset(L.info,0,NI_SIZE); set_state(ST_INIT); break;
    case 202: {
      if(st!=ST_AP){ rc=LDN_RC(32); break; }
      const u8* r=c->raw; u8 ncm=r[0x8A]; u16 ver; memcpy(&ver,r+0x8C,2);
      if(ncm>8 || (ver&0x8000)){ rc=LDN_RC(96); break; }
      memcpy(L.username,r+0x44,32); L.username[32]=0; L.lcver=ver;
      memset(L.info,0,NI_SIZE);
      memcpy(L.info,r+0x78,0x10);                         /* IntentId */
      i64 lcid; memcpy(&lcid,L.info,8); if(lcid==-1){ lcid=(i64)g_title_id; memcpy(L.info,&lcid,8); }
      for(int i=0;i<16;i++) L.info[0x10+i]=(u8)(rand()^(plat_ns()>>(i&7)));
      u32 ip=lan_ip(); fake_mac(L.info+0x20,ip);
      L.info[0x26]=32; memcpy(L.info+0x27,"12345678123456781234567812345678",32);
      u16 ch; memcpy(&ch,r+0x88,2); if(!ch) ch=6; memcpy(L.info+0x48,&ch,2); L.info[0x4A]=3; L.info[0x4B]=2;
      memcpy(L.info+0x60,r,2);                            /* SecurityMode */
      L.info[0x66]= ncm? ncm : 8;
      for(int i=0;i<8;i++) L.info[NI_NODE(i)+0xA]=(u8)i;
      my_node(L.info+NI_NODE(0)); L.info[0x67]=1;
      memcpy(L.info+0x26A,&L.advn,2); memcpy(L.info+0x26C,L.adv,L.advn);
      memset(L.prev_conn,0,8); memset(L.upd,0,8); node_bits();
      if(!open_udp()){ rc=LDN_RC(22); break; }
      SOCKET t=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); int one=1; setsockopt(t,SOL_SOCKET,SO_REUSEADDR,(const char*)&one,sizeof one);
      struct sockaddr_in sa={0}; sa.sin_family=AF_INET; sa.sin_port=htons(LAN_PORT); sa.sin_addr.s_addr= (ip>>24)==127? htonl(ip) : INADDR_ANY;
      if(bind(t,(struct sockaddr*)&sa,sizeof sa) || listen(t,10)){ LOG("lan: TCP %d is busy (error %d)\n",LAN_PORT,SOCK_ERR); CLOSESOCK(t); rc=LDN_RC(22); break; }
      L.listener=t;
      LOG("lan: room created by %.32s, up to %d players\n",L.username,L.info[0x66]);
      set_state(ST_AP_CREATED); break; }
    case 204: if(st!=ST_AP_CREATED){ rc=LDN_RC(32); break; } destroy_network(); memset(L.info,0,NI_SIZE); set_state(ST_AP); break;
    case 206: { buf_in(c,0,&a,&s); if(!s || s>0x180){ rc=LDN_RC(96); break; } if(st!=ST_AP && st!=ST_AP_CREATED){ rc=LDN_RC(32); break; }
      memcpy(L.adv,GP(a),s); L.advn=(u16)s;
      if(st==ST_AP_CREATED){ memset(L.info+0x26C,0,0x180); memcpy(L.info+0x26A,&L.advn,2); memcpy(L.info+0x26C,L.adv,L.advn); host_update(); }
      break; }
    case 300: if(st!=ST_INIT){ rc=LDN_RC(32); break; } set_state(ST_STATION); break;
    case 301: if(st!=ST_STATION && st!=ST_STATION_CONNECTED){ rc=LDN_RC(32); break; } close_peer(&L.host); L.synced=0; memset(L.info,0,NI_SIZE); set_state(ST_INIT); break;
    case 302: {
      if(st!=ST_STATION){ rc=LDN_RC(32); break; }
      const u8* r=c->raw; u32 ver,opt; memcpy(&ver,r+0x74,4); memcpy(&opt,r+0x78,4);
      if(opt>1 || ver>0x7FFF){ rc=LDN_RC(96); break; }
      buf_in(c,0,&a,&s); if(s<NI_SIZE){ rc=LDN_RC(96); break; }
      u8 ni[NI_SIZE]; memcpy(ni,GP(a),NI_SIZE);
      if(!ni[0x67]){ rc=LDN_RC(65); break; }
      memcpy(L.username,r+0x44,32); L.username[32]=0; L.lcver=(u16)ver;
      u32 hip; memcpy(&hip,ni+NI_NODE(0),4);
      SOCKET t=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
      if((lan_ip()>>24)==127 || lan_ip()){ struct sockaddr_in me={0}; me.sin_family=AF_INET; me.sin_addr.s_addr=htonl(lan_ip()); bind(t,(struct sockaddr*)&me,sizeof me); }
      struct sockaddr_in ha={0}; ha.sin_family=AF_INET; ha.sin_port=htons(LAN_PORT); ha.sin_addr.s_addr=htonl(hip);
      char hs[20]; ip_str(hip,hs);
      ulk();
      sock_nonblock(t,1); connect(t,(struct sockaddr*)&ha,sizeof ha);
      fd_set ws; FD_ZERO(&ws); FD_SET(t,&ws); struct timeval tv={3,0};
      int ok= select((int)t+1,NULL,&ws,NULL,&tv)==1;
      if(ok){ int err=0; socklen_t el=sizeof err; getsockopt(t,SOL_SOCKET,SO_ERROR,(char*)&err,&el); ok= !err; }
      sock_nonblock(t,0);
      lk();
      if(!ok){ LOG("lan: could not reach the host at %s:%d\n",hs,LAN_PORT); CLOSESOCK(t); rc=LDN_RC(64); break; }
      int one=1; setsockopt(t,IPPROTO_TCP,TCP_NODELAY,(const char*)&one,sizeof one);
      memcpy(L.info,ni,NI_SIZE); memset(L.prev_conn,0,8); memset(L.upd,0,8); L.synced=0;
      L.host.s=t; L.host.rxn=0;
      u8 me[0x40]; my_node(me); u8 pk[256]; int n=pk_build(pk,PK_CONNECT,me,0x40); send_all(t,pk,n);
      if(!open_udp()){ rc=LDN_RC(22); break; }
      LOG("lan: joining %.32s at %s\n",(char*)ni+NI_NODE(0)+0xC,hs);
      /* wait up to 4 s for the first SyncNetwork (R) */
      for(int i=0;i<80 && !L.synced && L.host.s!=INVALID_SOCKET;i++){ ulk(); plat_sleep_ms(50); lk(); }
      if(!L.synced){ close_peer(&L.host); rc=LDN_RC(66); LOG("lan: the host did not answer\n"); }
      break; }
    case 304: if(st!=ST_STATION_CONNECTED){ rc=LDN_RC(32); break; } station_drop(1); break;
    case 400: case 402:
      if(st!=ST_NONE){ rc=LDN_RC(32); break; }
      lan_pick();
      if(!lan_ip() || !open_udp()){ set_state(ST_ERROR); rc=LDN_RC(22); break; }
      L.reason=0; set_state(ST_INIT); break;
    case 401:
      if(st==ST_AP_CREATED || st==ST_AP) destroy_network();
      if(st==ST_STATION || st==ST_STATION_CONNECTED) close_peer(&L.host);
      close_udp(); memset(L.info,0,NI_SIZE); L.advn=0; set_state(ST_NONE); break;
    default: break; }
  ulk();
  return rc; }

/* ldn:u: 0 opens the service above, 1 the client monitor (18.0+, its RegisterClient is a stub) */
static int ldn_monitor(SvcObj* o, IpcCtx* c){ (void)o; (void)c; return 0; }
int lan_ldn_disp(SvcObj* o, IpcCtx* c){ (void)o;
  if(c->cmd==0){ lk(); ulk(); out_obj(c,svcobj_new("IUserLocalCommunicationService",ldn_svc,0)); return 0; }
  if(c->cmd==1){ out_obj(c,svcobj_new("IClientProcessMonitor",ldn_monitor,0)); return 0; }
  return 0; }

/* status for the menu */
void lan_status(char* out, int n){
  lk();
  const char* names[]={"desligado","pronto","abrindo sala","sala aberta","procurando","na sala","erro"};
  int players=L.info[0x67];
  if(L.state==ST_AP_CREATED || L.state==ST_STATION_CONNECTED) snprintf(out,n,"%s, %d jogador(es)",names[L.state],players);
  else snprintf(out,n,"%s",names[L.state<7? L.state : 6]);
  ulk(); }

/* ============================================================ bsd:u (docs/LAN_SPEC.md 3) */
#define NFD 64
static struct { SOCKET s, w; int type, nonblock, bound, online; } g_fd[NFD];   /* type 3 = eventfd: s reads, w writes (a connected UDP pair) */
static PMutex g_fdlk; static int g_fdinit;

static void fd_init(void){ if(!g_fdinit){ pm_init(&g_fdlk); for(int i=0;i<NFD;i++) g_fd[i].s=g_fd[i].w=INVALID_SOCKET; g_fdinit=1; net_startup(); } }
static int fd_ok(u32 fd){ return fd<NFD && g_fd[fd].s!=INVALID_SOCKET; }

/* host error -> Linux errno */
static u32 lx_errno(int e, int connecting){
#ifdef _WIN32
  switch(e){
    case WSAEWOULDBLOCK: return connecting? 115 : 11;
    case WSAEINPROGRESS: return 115; case WSAEALREADY: return 114; case WSAENOTSOCK: case WSAEBADF: return 9;
    case WSAEINVAL: return 22; case WSAEMSGSIZE: return 90; case WSAENOPROTOOPT: return 92; case WSAEOPNOTSUPP: return 95;
    case WSAEADDRINUSE: return 98; case WSAEADDRNOTAVAIL: return 99; case WSAENETUNREACH: return 101; case WSAECONNABORTED: return 103;
    case WSAECONNRESET: return 104; case WSAENOBUFS: return 105; case WSAEISCONN: return 106; case WSAENOTCONN: return 107;
    case WSAETIMEDOUT: return 110; case WSAECONNREFUSED: return 111; case WSAEHOSTUNREACH: return 113; case WSAEACCES: return 13;
  }
  return 22;
#else
  if(e==EINPROGRESS || (connecting && e==EAGAIN)) return 115;
  return (u32)e;   /* glibc uses the same numbers */
#endif
}

static void bsd_ret(IpcCtx* c, int ret, u32 err){ out32(c,(u32)ret); out32(c,err); }
static void bsd_fail(IpcCtx* c, int connecting){ int e=SOCK_ERR; u32 lx=lx_errno(e,connecting); if(lx==22){ static int said; if(said<20){ said++; LOG("lan: bsd cmd %u host error %d -> EINVAL\n",c->cmd,e); } } bsd_ret(c,-1,lx); }

/* FreeBSD sockaddr_in (u8 len, u8 family) <-> host */
static int sa_in(u64 a, u64 s, struct sockaddr_in* o){
  memset(o,0,sizeof *o); o->sin_family=AF_INET;
  if(!a || s<8) return 0;
  memcpy(&o->sin_port,GP(a)+2,2); memcpy(&o->sin_addr,GP(a)+4,4); return 1; }
static void sa_out(u64 a, u64 s, const struct sockaddr_in* i){
  if(!a || s<8) return; u8 b[16]={0}; b[0]=16; b[1]=2; memcpy(b+2,&i->sin_port,2); memcpy(b+4,&i->sin_addr,4); memcpy(GP(a),b,s<16? s : 16); }

/* outgoing traffic leaves through the chosen adapter (R: DefaultSocket pinning) */
static void pin(u32 fd){
  if(g_fd[fd].bound || !lan_ip()) return;
  struct sockaddr_in me={0}; me.sin_family=AF_INET; me.sin_addr.s_addr=htonl(lan_ip());
  if(!bind(g_fd[fd].s,(struct sockaddr*)&me,sizeof me)) g_fd[fd].bound=1; }

static int host_flags(u32 f){ int h=0; if(f&1) h|=MSG_OOB; if(f&2) h|=MSG_PEEK;
#ifdef MSG_DONTWAIT
  if(f&0x80) h|=MSG_DONTWAIT;
#endif
  return h; }

/* broadcasts also go to every lan_peers PC as unicast; 255.255.255.255 becomes the subnet broadcast */
static int send_to(u32 fd, const u8* p, int n, int fl, struct sockaddr_in* to){
  u32 dst=ntohl(to->sin_addr.s_addr), bc=lan_bcast();
  int is_bc= dst==0xFFFFFFFFu || (lan_ip() && dst==bc);
  int r=n;
  if(is_bc){
    if((lan_ip()>>24)!=127){ to->sin_addr.s_addr=htonl(bc); r=sendto(g_fd[fd].s,(const char*)p,n,fl,(struct sockaddr*)to,sizeof *to); }
    for(int i=0;i<g_npeers;i++){ struct sockaddr_in t=*to; t.sin_addr.s_addr=htonl(g_peers[i]); sendto(g_fd[fd].s,(const char*)p,n,fl,(struct sockaddr*)&t,sizeof t); }
    return r; }
  return sendto(g_fd[fd].s,(const char*)p,n,fl,(struct sockaddr*)to,sizeof *to); }

static int one_call_nonblock(u32 fd, u32 flags){
#ifdef _WIN32
  if((flags&0x80) && !g_fd[fd].nonblock){ sock_nonblock(g_fd[fd].s,1); return 1; }
#else
  (void)fd; (void)flags;
#endif
  return 0; }
/* keeps the error of the call it follows: ioctlsocket would reset WSAGetLastError and every failure would read as EINVAL */
static void restore_block(u32 fd, int t){ if(!t) return;
#ifdef _WIN32
  int e=WSAGetLastError(); sock_nonblock(g_fd[fd].s,0); WSASetLastError(e);
#else
  int e=errno; sock_nonblock(g_fd[fd].s,0); errno=e;
#endif
}

/* poll over select (Winsock's WSAPoll uses other bit values) */
static int do_poll(u8* pf, int n, int timeout_ms){
  fd_set rs,ws,es; FD_ZERO(&rs); FD_ZERO(&ws); FD_ZERO(&es); SOCKET mx=0; int any=0;
  for(int i=0;i<n;i++){ i32 fd; i16 ev; memcpy(&fd,pf+i*8,4); memcpy(&ev,pf+i*8+4,2); i16 z=0; memcpy(pf+i*8+6,&z,2);
    if(fd<0 || !fd_ok((u32)fd)) continue; SOCKET s=g_fd[fd].s; any=1; if(s>mx) mx=s;
    if(ev&(1|0x40|2)) FD_SET(s,&rs); if(ev&(4|0x100)) FD_SET(s,&ws); FD_SET(s,&es); }
  if(!any){ if(timeout_ms>0) plat_sleep_ms(timeout_ms<1000? timeout_ms : 1000); return 0; }
  struct timeval tv={timeout_ms/1000,(timeout_ms%1000)*1000};
  int r=select((int)mx+1,&rs,&ws,&es,timeout_ms<0? NULL : &tv);
  if(r<=0) return r;
  int cnt=0;
  for(int i=0;i<n;i++){ i32 fd; i16 ev,re=0; memcpy(&fd,pf+i*8,4); memcpy(&ev,pf+i*8+4,2);
    if(fd<0){ continue; } if(!fd_ok((u32)fd)){ re=0x20; } else { SOCKET s=g_fd[fd].s;
      if(FD_ISSET(s,&rs)) re|=ev&(1|0x40|2); if(FD_ISSET(s,&ws)) re|=ev&(4|0x100); if(FD_ISSET(s,&es)) re|=8; }
    if(re){ cnt++; memcpy(pf+i*8+6,&re,2); } }
  return cnt; }

int lan_bsd_disp(SvcObj* o, IpcCtx* c){ (void)o;
  fd_init();
  u64 a,s,b,bs; u32 fd=in32(c,0);
  switch(c->cmd){
    case 0: out32(c,0); return 0;                              /* RegisterClient: only errno */
    case 1: return 0;                                          /* StartMonitoring */
    case 2: case 3: {
      u32 dom=in32(c,0), type=in32(c,4), proto=in32(c,8); int nb=(type&0x20000000)!=0; type&=0x0FFFFFFF;
      if(dom!=2 || (type!=1 && type!=2)){ bsd_ret(c,-1,97); return 0; }
      pm_lock(&g_fdlk); int f=-1; for(int i=3;i<NFD && f<0;i++) if(g_fd[i].s==INVALID_SOCKET) f=i;
      if(f<0){ pm_unlock(&g_fdlk); bsd_ret(c,-1,24); return 0; }
      SOCKET h=socket(AF_INET,type==1? SOCK_STREAM : SOCK_DGRAM,proto? (int)proto : (type==1? IPPROTO_TCP : IPPROTO_UDP));
      if(h==INVALID_SOCKET){ pm_unlock(&g_fdlk); bsd_fail(c,0); return 0; }
      g_fd[f].s=h; g_fd[f].type=(int)type; g_fd[f].nonblock=nb; g_fd[f].bound=0; pm_unlock(&g_fdlk);
      if(type==2) sock_quiet_udp(h);
      if(nb) sock_nonblock(h,1);
      LOG("lan: socket %d (%s)\n",f,type==1? "TCP" : "UDP");
      bsd_ret(c,f,0); return 0; }
    case 5: {
      /* Select: fd_set buffers in/out; we answer through poll semantics */
      i32 nfds=(i32)in32(c,0); i64 sec=(i64)in64(c,8), usec=(i64)in64(c,0x10); int isnull=in8(c,0x18);
      int tmo= isnull? -1 : (int)(sec*1000+usec/1000);
      u64 ra,rsz,wa,wsz,ea,esz; buf_in(c,0,&ra,&rsz); buf_in(c,1,&wa,&wsz); buf_in(c,2,&ea,&esz);
      u64 ro,rosz,wo,wosz,eo,eosz; buf_out(c,0,&ro,&rosz); buf_out(c,1,&wo,&wosz); buf_out(c,2,&eo,&eosz);
      u8 pf[NFD*8]; int n=0; int map[NFD];
      for(int f=0;f<nfds && f<NFD;f++){ i16 ev=0;
        if(ra && (u64)f/8<rsz && (GP(ra)[f/8]>>(f%8)&1)) ev|=1; if(wa && (u64)f/8<wsz && (GP(wa)[f/8]>>(f%8)&1)) ev|=4; if(ea && (u64)f/8<esz && (GP(ea)[f/8]>>(f%8)&1)) ev|=2;
        if(!ev) continue; i32 fdv=f; memcpy(pf+n*8,&fdv,4); memcpy(pf+n*8+4,&ev,2); map[n]=f; n++; }
      int r=do_poll(pf,n,tmo);
      if(ro) memset(GP(ro),0,rosz); if(wo) memset(GP(wo),0,wosz); if(eo) memset(GP(eo),0,eosz);
      int cnt=0;
      for(int i=0;i<n;i++){ i16 re; memcpy(&re,pf+i*8+6,2); int f=map[i];
        if((re&1) && ro && (u64)f/8<rosz){ GP(ro)[f/8]|=1<<(f%8); cnt++; }
        if((re&4) && wo && (u64)f/8<wosz){ GP(wo)[f/8]|=1<<(f%8); cnt++; }
        if((re&8) && eo && (u64)f/8<eosz){ GP(eo)[f/8]|=1<<(f%8); cnt++; } }
      bsd_ret(c,r<0? -1 : cnt,r<0? 22 : 0); return 0; }
    case 6: {
      i32 n=(i32)in32(c,0), tmo=(i32)in32(c,4); buf_in(c,0,&a,&s); buf_out(c,0,&b,&bs);
      if(n<0 || n>NFD || (u64)n*8>s){ bsd_ret(c,-1,22); return 0; }
      u8 pf[NFD*8]; memcpy(pf,GP(a),(size_t)n*8);
      int r=do_poll(pf,n,tmo);
      if(b) memcpy(GP(b),pf,(size_t)(bs<(u64)n*8? bs : (u64)n*8));
      bsd_ret(c,r<0? -1 : r,r<0? 22 : 0); return 0; }
    case 8: case 25: {
      if(!fd_ok(fd)){ bsd_ret(c,-1,9); return 0; }
      if(g_fd[fd].type==3){ buf_out(c,0,&a,&s); u64 sum=0, v; int got=0;
        if(!g_fd[fd].nonblock){ fd_set rs; FD_ZERO(&rs); FD_SET(g_fd[fd].s,&rs); select((int)g_fd[fd].s+1,&rs,NULL,NULL,NULL); }
        sock_nonblock(g_fd[fd].s,1); while(recv(g_fd[fd].s,(char*)&v,8,0)==8){ sum+=v; got=1; } sock_nonblock(g_fd[fd].s,0);
        if(!got){ bsd_ret(c,-1,11); return 0; } if(a && s>=8) memcpy(GP(a),&sum,8); bsd_ret(c,8,0); return 0; }
      u32 fl= c->cmd==8? in32(c,4) : 0; buf_out(c,0,&a,&s); int t=one_call_nonblock(fd,fl);
      int r=recv(g_fd[fd].s,(char*)GP(a),(int)s,host_flags(fl)); restore_block(fd,t);
      if(r<0) bsd_fail(c,0); else bsd_ret(c,r,0); return 0; }
    case 9: {
      if(!fd_ok(fd)){ bsd_ret(c,-1,9); out32(c,0); return 0; }
      u32 fl=in32(c,4); buf_out(c,0,&a,&s); buf_out(c,1,&b,&bs); int t=one_call_nonblock(fd,fl);
      struct sockaddr_in from; socklen_t frl=sizeof from;
      int r=recvfrom(g_fd[fd].s,(char*)GP(a),(int)s,host_flags(fl),(struct sockaddr*)&from,&frl); restore_block(fd,t);
      if(r<0){ bsd_fail(c,0); out32(c,0); return 0; }
      sa_out(b,bs,&from); bsd_ret(c,r,0); out32(c,b? 16 : 0); return 0; }
    case 10: case 24: {
      if(!fd_ok(fd)){ bsd_ret(c,-1,9); return 0; }
      if(g_fd[fd].type==3){ buf_in(c,0,&a,&s); u64 v=0; if(a && s>=8) memcpy(&v,GP(a),8); send(g_fd[fd].w,(const char*)&v,8,0); bsd_ret(c,8,0); return 0; }
      u32 fl= c->cmd==10? in32(c,4) : 0; buf_in(c,0,&a,&s); pin(fd); int t=one_call_nonblock(fd,fl);
      int r=send(g_fd[fd].s,(const char*)GP(a),(int)s,host_flags(fl)); restore_block(fd,t);
      if(r<0) bsd_fail(c,0); else bsd_ret(c,r,0); return 0; }
    case 11: {
      if(!fd_ok(fd)){ bsd_ret(c,-1,9); return 0; }
      u32 fl=in32(c,4); buf_in(c,0,&a,&s); buf_in(c,1,&b,&bs); struct sockaddr_in to;
      if(!sa_in(b,bs,&to)){ bsd_ret(c,-1,22); return 0; }
      pin(fd); int t=one_call_nonblock(fd,fl);
      int r=send_to(fd,GP(a),(int)s,host_flags(fl),&to); restore_block(fd,t);
      if(r<0) bsd_fail(c,0); else bsd_ret(c,r,0); return 0; }
    case 12: {
      if(!fd_ok(fd)){ bsd_ret(c,-1,9); out32(c,0); return 0; }
      buf_out(c,0,&a,&s); struct sockaddr_in from; socklen_t frl=sizeof from;
      SOCKET h=accept(g_fd[fd].s,(struct sockaddr*)&from,&frl);
      if(h==INVALID_SOCKET){ bsd_fail(c,0); out32(c,0); return 0; }
      pm_lock(&g_fdlk); int f=-1; for(int i=3;i<NFD && f<0;i++) if(g_fd[i].s==INVALID_SOCKET) f=i;
      if(f<0){ pm_unlock(&g_fdlk); CLOSESOCK(h); bsd_ret(c,-1,24); out32(c,0); return 0; }
      g_fd[f].s=h; g_fd[f].type=1; g_fd[f].nonblock=0; g_fd[f].bound=1; pm_unlock(&g_fdlk);
      sa_out(a,s,&from); bsd_ret(c,f,0); out32(c,16); return 0; }
    case 13: {
      if(!fd_ok(fd)){ bsd_ret(c,-1,9); return 0; }
      buf_in(c,0,&a,&s); struct sockaddr_in me; if(!sa_in(a,s,&me)){ bsd_ret(c,-1,22); return 0; }
      /* a loopback test instance binds its own 127.x address so two games on one PC don't share ports */
      if(me.sin_addr.s_addr==INADDR_ANY && (lan_ip()>>24)==127) me.sin_addr.s_addr=htonl(lan_ip());
      if(g_fd[fd].type==2){ int one=1; setsockopt(g_fd[fd].s,SOL_SOCKET,SO_BROADCAST,(const char*)&one,sizeof one); }
      int r=bind(g_fd[fd].s,(struct sockaddr*)&me,sizeof me);
      if(r){ bsd_fail(c,0); return 0; }
      g_fd[fd].bound=1; LOG("lan: socket %u bound to port %u\n",fd,ntohs(me.sin_port)); bsd_ret(c,0,0); return 0; }
    case 14: {
      if(!fd_ok(fd)){ bsd_ret(c,-1,9); return 0; }
      buf_in(c,0,&a,&s); struct sockaddr_in to; if(!sa_in(a,s,&to)){ bsd_ret(c,-1,22); return 0; }
      pin(fd);
      /* online play: port 443 of the host goes to its built-in server (online.c), which listens on a high port */
      { extern u16 online_port(void); if(ntohs(to.sin_port)==443 && ntohl(to.sin_addr.s_addr)==online_host_ip()) to.sin_port=htons(online_port()); }
      { char t[20]; ip_str(ntohl(to.sin_addr.s_addr),t); LOG("lan: socket %u connects to %s:%u\n",fd,t,ntohs(to.sin_port)); extern u16 online_port(void); g_fd[fd].online= ntohs(to.sin_port)==online_port(); }
      int r=connect(g_fd[fd].s,(struct sockaddr*)&to,sizeof to);
      if(r) bsd_fail(c,1); else bsd_ret(c,0,0); return 0; }
    case 15: case 16: {
      if(!fd_ok(fd)){ bsd_ret(c,-1,9); out32(c,0); return 0; }
      buf_out(c,0,&a,&s); struct sockaddr_in me; socklen_t l=sizeof me;
      int r= c->cmd==15? getpeername(g_fd[fd].s,(struct sockaddr*)&me,&l) : getsockname(g_fd[fd].s,(struct sockaddr*)&me,&l);
      if(r){ bsd_fail(c,0); out32(c,0); return 0; }
      sa_out(a,s,&me); bsd_ret(c,0,0); out32(c,16); return 0; }
    case 17: {
      if(!fd_ok(fd)){ bsd_ret(c,-1,9); out32(c,0); return 0; }
      i32 lv=(i32)in32(c,4), on=(i32)in32(c,8); buf_out(c,0,&a,&s);
      int hl= lv==0xFFFF? SOL_SOCKET : lv; char v[64]={0}; socklen_t vl=sizeof v;
      if(lv==0xFFFF && on==0x1007){ int e=0; socklen_t el=sizeof e; getsockopt(g_fd[fd].s,SOL_SOCKET,SO_ERROR,(char*)&e,&el); u32 le= e? lx_errno(e,0) : 0; if(a && s>=4) memcpy(GP(a),&le,4); bsd_ret(c,0,0); out32(c,4); return 0; }
      if(getsockopt(g_fd[fd].s,hl,on,v,&vl)){ if(a && s) memset(GP(a),0,s); bsd_ret(c,0,0); out32(c,(u32)(s<4? s : 4)); return 0; }
      if(a) memcpy(GP(a),v,(size_t)(s<(u64)vl? s : (u64)vl)); bsd_ret(c,0,0); out32(c,(u32)vl); return 0; }
    case 18: if(!fd_ok(fd)){ bsd_ret(c,-1,9); return 0; } if(listen(g_fd[fd].s,(int)in32(c,4))) bsd_fail(c,0); else bsd_ret(c,0,0); return 0;
    case 19: {
      if(!fd_ok(fd)){ bsd_ret(c,-1,9); return 0; }
      u32 req=in32(c,4);
      if(req==0x4004667F){ unsigned long v=0;
#ifdef _WIN32
        u_long w=0; ioctlsocket(g_fd[fd].s,FIONREAD,&w); v=w;
#else
        int w=0; ioctl(g_fd[fd].s,FIONREAD,&w); v=(unsigned long)w;
#endif
        buf_out(c,0,&a,&s); u32 v32=(u32)v; if(a && s>=4) memcpy(GP(a),&v32,4); bsd_ret(c,0,0); return 0; }
      if(req==0x8004667E){ buf_in(c,0,&a,&s); u32 on=0; if(a && s>=4) memcpy(&on,GP(a),4); g_fd[fd].nonblock=on!=0; sock_nonblock(g_fd[fd].s,on!=0); bsd_ret(c,0,0); return 0; }
      if(req==0x40047307){ buf_out(c,0,&a,&s); if(a && s>=4) memset(GP(a),0,4); bsd_ret(c,0,0); return 0; }
      bsd_ret(c,-1,95); return 0; }
    case 20: {
      if(!fd_ok(fd)){ bsd_ret(c,-1,9); return 0; }
      u32 cmd=in32(c,4), arg=in32(c,8);
      if(cmd==3){ bsd_ret(c,g_fd[fd].nonblock? 0x800 : 0,0); return 0; }
      if(cmd==4){ int on=(arg&0x800)!=0 || (arg&4)!=0; g_fd[fd].nonblock=on; sock_nonblock(g_fd[fd].s,on); bsd_ret(c,0,0); return 0; }
      bsd_ret(c,0,0); return 0; }
    case 21: {
      if(!fd_ok(fd)){ bsd_ret(c,-1,9); return 0; }
      i32 lv=(i32)in32(c,4), on=(i32)in32(c,8); buf_in(c,0,&a,&s);
      int hl= lv==0xFFFF? SOL_SOCKET : lv;
      const char* v=(const char*)GP(a); int vl=(int)s;
#ifdef _WIN32
      DWORD ms;
      if(lv==0xFFFF && (on==0x1005 || on==0x1006) && s>=16){ i64 sec,us; memcpy(&sec,v,8); memcpy(&us,v+8,8); ms=(DWORD)(sec*1000+us/1000); v=(const char*)&ms; vl=4; }
#endif
      if(lv==0xFFFF && on==0x200) on=SO_REUSEADDR;     /* SO_REUSEPORT */
      if(setsockopt(g_fd[fd].s,hl,on,v,vl)){ static int said; if(said<20){ said++; LOG("lan: setsockopt %d/%d ignored (error %d)\n",lv,on,SOCK_ERR); } }
      bsd_ret(c,0,0); return 0; }
    case 22: if(!fd_ok(fd)){ bsd_ret(c,-1,9); return 0; }
      /* SWR_ONLINE_BT=1: who shuts a connection to the host server down (the game gives up on it) */
      if(g_fd[fd].online && getenv("SWR_ONLINE_BT")){ static int said; if(said<6){ said++; LOG("lan: socket %u to the server shut down by:\n",fd); extern void ipc_backtrace(IpcCtx*); ipc_backtrace(c); } }
      shutdown(g_fd[fd].s,(int)in32(c,4)); bsd_ret(c,0,0); return 0;
    case 23: bsd_ret(c,0,0); return 0;
    case 26: {
      if(!fd_ok(fd)){ bsd_ret(c,-1,9); return 0; }
      pm_lock(&g_fdlk); CLOSESOCK(g_fd[fd].s); g_fd[fd].s=INVALID_SOCKET; if(g_fd[fd].type==3 && g_fd[fd].w!=INVALID_SOCKET) CLOSESOCK(g_fd[fd].w); g_fd[fd].w=INVALID_SOCKET; pm_unlock(&g_fdlk);
      bsd_ret(c,0,0); return 0; }
    case 31: {
      /* EventFd (gRPC's wakeup fd): a counter that is readable while non-zero. here a pair of connected loopback UDP
         sockets: a write sends the 8-byte value, a read drains every datagram and returns their sum */
      u64 init=in64(c,8); u32 fl=in32(c,0);
      SOCKET r=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP), w=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
      struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(0x7F000001); socklen_t al=sizeof a;
      if(r==INVALID_SOCKET || w==INVALID_SOCKET || bind(r,(struct sockaddr*)&a,sizeof a) || getsockname(r,(struct sockaddr*)&a,&al) || connect(w,(struct sockaddr*)&a,sizeof a)){
        if(r!=INVALID_SOCKET) CLOSESOCK(r); if(w!=INVALID_SOCKET) CLOSESOCK(w); bsd_fail(c,0); return 0; }
      sock_quiet_udp(r); sock_quiet_udp(w);
      pm_lock(&g_fdlk); int f=-1; for(int i=3;i<NFD && f<0;i++) if(g_fd[i].s==INVALID_SOCKET) f=i;
      if(f<0){ pm_unlock(&g_fdlk); CLOSESOCK(r); CLOSESOCK(w); bsd_ret(c,-1,24); return 0; }
      g_fd[f].s=r; g_fd[f].w=w; g_fd[f].type=3; g_fd[f].nonblock=(fl&0x800)!=0 || (fl&4)!=0; g_fd[f].bound=1; pm_unlock(&g_fdlk);
      if(init) send(w,(const char*)&init,8,0);
      LOG("lan: eventfd %d\n",f); bsd_ret(c,f,0); return 0; }
    case 29: case 30: {
      /* RecvMMsg / SendMMsg (gRPC sends and reads through these). the out buffer is a serialized mmsghdr (Ryujinx
         BsdMMsgHdr): u8 header, then per message {u32 namelen, name, u32 iovcnt, {u64 len, data}[iovcnt],
         u32 ctrllen, ctrl, u32 flags, u32 msg_len}. data is sent/received in place and msg_len filled in */
      if(!fd_ok(fd)){ bsd_ret(c,-1,9); return 0; }
      u32 vlen=in32(c,4), fl=in32(c,8); buf_out(c,0,&a,&s); u8* b=GP(a); u64 o=1, end=s; int done_msgs=0;
      for(u32 m=0;m<vlen && m<16;m++){
        u32 nl,ic,cl; if(o+4>end) break; memcpy(&nl,b+o,4); o+=4+nl; if(o+4>end) break; memcpy(&ic,b+o,4); o+=4;
        u64 iv_off[16], iv_len[16]; u32 ni=0;
        for(u32 k=0;k<ic && o+8<=end;k++){ u64 l; memcpy(&l,b+o,8); o+=8; if(ni<16){ iv_off[ni]=o; iv_len[ni]=l; ni++; } o+=l; }
        if(o+4>end) break; memcpy(&cl,b+o,4); o+=4+cl; u64 lenpos=o+4; o+=8; if(o>end) break;
        int t=one_call_nonblock(fd,fl); i64 total=0; int err=0;
        for(u32 k=0;k<ni;k++){
          int r= c->cmd==30? send(g_fd[fd].s,(const char*)b+iv_off[k],(int)iv_len[k],host_flags(fl)) : recv(g_fd[fd].s,(char*)b+iv_off[k],(int)iv_len[k],host_flags(fl));
          if(r<0){ if(!total) err=1; break; } total+=r; if((u64)r<iv_len[k]) break; if(c->cmd==29 && r==0) break; }
        restore_block(fd,t);
        if(err){ if(!done_msgs){ bsd_fail(c,0); return 0; } break; }
        u32 ml=(u32)total; memcpy(b+lenpos,&ml,4); done_msgs++;
        if(c->cmd==29 && total==0) break; }
      bsd_ret(c,done_msgs,0); return 0; }
    case 7: bsd_ret(c,-1,95); return 0;
  }
  { static int said; if(said<20){ said++; LOG("lan: bsd cmd %u not handled\n",c->cmd); } }
  bsd_ret(c,-1,95); return 0; }

/* nifm answers that agree with the adapter LDN uses (network byte order here) */
u32 lan_nifm_ip_nbo(void){ return htonl(lan_ip()); }
u32 lan_nifm_mask_nbo(void){ return htonl(lan_mask()); }

/* adapters for the menu's network picker: "name (ip)" and the ip text */
int lan_menu_ifaces(char labels[][128], char ips[][20], int max){
  NetIf l[32]; int n=lan_list_ifaces(l,32); if(n>max) n=max;
  for(int i=0;i<n;i++){ ip_str(l[i].ip,ips[i]); snprintf(labels[i],128,"%s%s (%s)",l[i].zt? "ZeroTier: " : "",l[i].name,ips[i]); }
  return n; }

/* ============================================================ sfdnsres: DNS for online play (docs/ONLINE_SPEC.md)
   every *.nintendo.net name points at the host's game (online_host= in the .ini, empty = this PC), which plays
   Nintendo's servers for everyone on the ZeroTier network; any other name uses the PC's own DNS. answers use the
   packed addrinfo of the real service: big-endian header 0xBEEFCAFE, then the address as Ryujinx writes it */
static u32 online_host_ip(void){ const char* h=ini_value("online_host",""); u32 ip= *h? parse_ip(h) : 0; return ip? ip : (lan_ip()? lan_ip() : 0x7F000001u); }

static void be32w(u8* p, u32 v){ p[0]=(u8)(v>>24); p[1]=(u8)(v>>16); p[2]=(u8)(v>>8); p[3]=(u8)v; }

static u32 addrinfo_pack(u8* out, u64 cap, const char* name, u32* ips, int n, u16 port){
  u64 o=0; size_t nl=strlen(name)+1;
  for(int i=0;i<n;i++){
    if(o+24+16+nl+4>cap) break;
    be32w(out+o,0xBEEFCAFEu); be32w(out+o+4,0); be32w(out+o+8,2); be32w(out+o+12,0); be32w(out+o+16,0); be32w(out+o+20,16); o+=24;
    u8* a=out+o; memset(a,0,16); a[0]=4; a[1]=2; memcpy(a+2,&port,2);
    a[4]=(u8)ips[i]; a[5]=(u8)(ips[i]>>8); a[6]=(u8)(ips[i]>>16); a[7]=(u8)(ips[i]>>24); o+=16;
    memcpy(out+o,name,nl); o+=nl; }
  memset(out+o,0,4); o+=4;
  return (u32)o; }

static int resolve_name(const char* name, u32* ips, int max){
  if(strstr(name,"nintendo.net") || strstr(name,"nintendo.com")){ ips[0]=online_host_ip(); return 1; }
  struct addrinfo h={0},*r=NULL; h.ai_family=AF_INET;
  if(getaddrinfo(name,NULL,&h,&r) || !r) return 0;
  int n=0; for(struct addrinfo* p=r;p && n<max;p=p->ai_next) ips[n++]=ntohl(((struct sockaddr_in*)p->ai_addr)->sin_addr.s_addr);
  freeaddrinfo(r); return n; }

int lan_sfdnsres_disp(SvcObj* o, IpcCtx* c){ (void)o;
  net_startup();
  u64 a,s,b,bs,out,os;
  switch(c->cmd){
    case 6: case 12: {
      buf_in(c,0,&a,&s); buf_in(c,1,&b,&bs);
      char name[256]={0}, svc[64]={0}; if(a && s) memcpy(name,GP(a),s<255? s : 255); if(b && bs) memcpy(svc,GP(b),bs<63? bs : 63);
      buf_out(c,0,&out,&os);
      u32 ips[8]; int n=resolve_name(name,ips,8); u16 port=(u16)atoi(svc);
      char ipt[20]="-"; if(n) ip_str(ips[0],ipt);
      LOG("online: dns %s -> %s\n",name,ipt);
      u32 size= (n && out && os)? addrinfo_pack(GP(out),os,name,ips,n,port) : 0;
      int netdb= n? 0 : 1, gai= n? 0 : 7;   /* HOST_NOT_FOUND, EAI_NONAME-ish */
      if(c->cmd==12){ out32(c,size); out32(c,(u32)gai); out32(c,(u32)netdb); out32(c,0); }
      else { out32(c,(u32)netdb); out32(c,(u32)gai); out32(c,size); }
      return 0; }
    case 2: case 10: {   /* GetHostByName: not used by the traced paths yet */
      buf_in(c,0,&a,&s); char name[256]={0}; if(a && s) memcpy(name,GP(a),s<255? s : 255);
      LOG("online: gethostbyname %s (not answered)\n",name);
      if(c->cmd==10){ out32(c,0); out32(c,1); out32(c,0); out32(c,0); } else { out32(c,1); out32(c,0); out32(c,0); }
      return 0; }
    case 8: out32(c,0); return 0;   /* GetCancelHandleRequest */
  }
  return 0; }
