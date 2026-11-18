// Conflict: Global Terror - network tunnel ("virtual LAN") for SquadMod
//
// The game only knows LAN play (UDP broadcast discovery on 10010, TCP 4658 to join,
// UDP 4658 for the game). This module makes remote players look like LAN machines:
//
//   * every machine ("peer") gets a virtual address 127.NS.HI.LO
//     (NS = namespace, HI.LO = 16-bit peer key)
//   * the local game is bound to its own virtual address
//   * traffic the game sends to another peer's address (or as a broadcast) is
//     carried over a transport (Steam P2P, or plain UDP between two copies on
//     one PC for testing) and re-emitted on the other machine from a local
//     "proxy" socket bound to the sender's virtual address, so the remote game
//     sees a normal LAN packet / TCP connection from that address.
//
// Transports:
//   TUN_TEST  - two game copies on one PC, UDP 127.0.0.1:4700+instance
//   TUN_STEAM - SteamNetworkingMessages (relayed P2P) + a friends-only Steam lobby
//               that forms the "LAN". Uses App ID 480 (Spacewar).

#define FD_SETSIZE 256
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

enum { TUN_OFF = 0, TUN_TEST = 1, TUN_STEAM = 2 };

typedef void (*pfnLog)(const char *fmt, ...);
typedef void *(*pfnPatchIAT)(const char *dll, const char *func, void *repl);

static pfnLog   L = NULL;
#define LOG(...) do { if (L) L(__VA_ARGS__); } while (0)

static int      g_mode;
static int      g_inst;            // TUN_TEST: this copy's instance number (1/2)
static u_long  *g_pHostIp;         // squadmod.c: game host address (network order)
static CRITICAL_SECTION g_cs;

// ------------------------------------------------------------ peers -------
#define MAX_PEERS 8
typedef struct {
    int      used;
    uint64_t id;          // steam id, or instance number in test mode
    uint16_t key;         // virtual address low 16 bits
    char     name[48];
    DWORD    lastRx;
    DWORD    goneAt;      // steam: left the lobby at this tick (0 = here). Kept for a grace period.
} Peer;
#define PEER_GRACE_MS 90000   // keep a vanished peer's connections this long so a short drop can recover
static Peer     g_peers[MAX_PEERS];
static uint16_t g_selfKey;
static uint8_t  g_ns = 20;          // namespace (2nd octet)
static int      g_lastGamePeer = -1;   // peer we last heard game traffic from (fallback route)

static u_long VAddr(uint16_t key) { return htonl(0x7F000000u | ((u_long)g_ns << 16) | key); }
u_long Tun_SelfAddr(void) { return VAddr(g_selfKey); }

static const char *A2S(u_long nip, u_short nport, char *b)
{
    u_long ip = ntohl(nip);
    sprintf(b, "%lu.%lu.%lu.%lu:%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, ntohs(nport));
    return b;
}

static int PeerByKey(uint16_t key)
{
    for (int i = 0; i < MAX_PEERS; i++) if (g_peers[i].used && g_peers[i].key == key) return i;
    return -1;
}
static int PeerById(uint64_t id)
{
    for (int i = 0; i < MAX_PEERS; i++) if (g_peers[i].used && g_peers[i].id == id) return i;
    return -1;
}
static int PeerCount(void)
{
    int n = 0;
    for (int i = 0; i < MAX_PEERS; i++) n += g_peers[i].used && !g_peers[i].goneAt;
    return n;
}

// Classify a destination the game wants to reach.
//   returns peer index, or -1 = local/self (leave alone), -2 = broadcast, -3 = unknown peer
static int Route(u_long nip)
{
    u_long ip = ntohl(nip);
    if (ip == 0xFFFFFFFFu) return -2;                                   // broadcast
    if ((ip >> 24) == 127) {
        uint8_t ns = (uint8_t)(ip >> 16);
        if (ns < 20 || ns > 29) return -1;                              // plain loopback
        uint16_t key = (uint16_t)ip;
        if (key == g_selfKey) return -1;
        int p = PeerByKey(key);
        return p >= 0 ? p : -3;
    }
    if (ip == 0) return -1;
    if ((ip & 0xFF) == 0xFF) return -2;                                 // subnet broadcast
    // a real IP the game picked up from a packet (e.g. the host advertising its LAN
    // address). Send it to the peer we are talking to.
    if (g_lastGamePeer >= 0 && g_peers[g_lastGamePeer].used) return g_lastGamePeer;
    if (PeerCount() == 1) for (int i = 0; i < MAX_PEERS; i++) if (g_peers[i].used) return i;
    return -3;
}

// --------------------------------------------------------- transport -------
// message layout: [type u8] ...
enum { M_UDP = 1, M_OPEN = 2, M_DATA = 3, M_CLOSE = 4 };
#pragma pack(push, 1)
typedef struct { uint8_t t; uint16_t src, dst; } HdrUdp;
typedef struct { uint8_t t; uint32_t id; uint16_t dst; } HdrOpen;
typedef struct { uint8_t t; uint32_t id; uint8_t fromOpener; } HdrData;   // also CLOSE
typedef struct { uint32_t magic; uint8_t inst; } HdrTest;
#pragma pack(pop)
#define TEST_MAGIC 0x54544743u   // 'CGTT'
#define TEST_PORT  4700

// ---- test transport
static SOCKET g_testSock = INVALID_SOCKET;

// ---- steam transport (flat API, loaded at run time)
typedef int      (__cdecl *pfnInitFlat)(char *err);
typedef void     (__cdecl *pfnVoid)(void);
typedef int      (__cdecl *pfnGetPipe)(void);
typedef void     (__cdecl *pfnMDRunFrame)(int pipe);
typedef uint8_t  (__cdecl *pfnMDGetNext)(int pipe, void *msg);
typedef void     (__cdecl *pfnMDFree)(int pipe);
typedef uint8_t  (__cdecl *pfnMDCallResult)(int pipe, uint64_t call, void *buf, int cb, int expected, uint8_t *failed);
typedef void    *(__cdecl *pfnAccessor)(void);
typedef uint64_t (__cdecl *pfnU64Self)(void *self);
typedef const char *(__cdecl *pfnStrSelf)(void *self);
typedef const char *(__cdecl *pfnFriendName)(void *self, uint64_t id);
typedef uint64_t (__cdecl *pfnCreateLobby)(void *self, int type, int max);
typedef uint64_t (__cdecl *pfnJoinLobby)(void *self, uint64_t lobby);
typedef void     (__cdecl *pfnLeaveLobby)(void *self, uint64_t lobby);
typedef int      (__cdecl *pfnNumMembers)(void *self, uint64_t lobby);
typedef uint64_t (__cdecl *pfnMemberByIdx)(void *self, uint64_t lobby, int i);
typedef uint64_t (__cdecl *pfnLobbyOwner)(void *self, uint64_t lobby);
typedef uint8_t  (__cdecl *pfnSetLobbyData)(void *self, uint64_t lobby, const char *k, const char *v);
typedef void     (__cdecl *pfnInviteDlg)(void *self, uint64_t lobby);
typedef uint8_t  (__cdecl *pfnSetRP)(void *self, const char *k, const char *v);
typedef int      (__cdecl *pfnSendMsg)(void *self, const void *ident, const void *data, uint32_t cb, int flags, int ch);
typedef int      (__cdecl *pfnRecvMsgs)(void *self, int ch, void **out, int max);
typedef uint8_t  (__cdecl *pfnAcceptSession)(void *self, const void *ident);
typedef void     (__cdecl *pfnMsgRelease)(void *msg);
typedef void     (__cdecl *pfnSelfVoid)(void *self);

static struct {
    HMODULE dll;
    int pipe;
    pfnMDRunFrame RunFrame; pfnMDGetNext GetNext; pfnMDFree FreeLast; pfnMDCallResult CallResult;
    void *user, *friends, *mm, *msgs, *utils;
    pfnU64Self GetSteamID; pfnStrSelf PersonaName; pfnFriendName FriendName;
    pfnCreateLobby CreateLobby; pfnJoinLobby JoinLobby; pfnLeaveLobby LeaveLobby;
    pfnNumMembers NumMembers; pfnMemberByIdx MemberByIdx; pfnLobbyOwner LobbyOwner; pfnSetLobbyData SetLobbyData;
    pfnInviteDlg InviteDlg; pfnSetRP SetRP;
    pfnSendMsg Send; pfnRecvMsgs Recv; pfnAcceptSession Accept; pfnMsgRelease Release;
    uint64_t me, lobby;
    char myName[48];
} S;

// SteamNetworkingIdentity (pack 1): int type; int size; union { uint64 steamid; ... 128 bytes }
typedef struct { int type, size; uint8_t u[128]; } SteamIdent;
static void Ident(SteamIdent *id, uint64_t sid)
{
    memset(id, 0, sizeof *id);
    id->type = 16;            // k_ESteamNetworkingIdentityType_SteamID
    id->size = 8;
    memcpy(id->u, &sid, 8);
}

static void TransportSend(int peer, const void *buf, int len, int reliable)
{
    if (peer < 0 || !g_peers[peer].used) return;
    if (g_mode == TUN_TEST) {
        static char pkt[70000];
        if (len > (int)sizeof pkt - 8) return;
        HdrTest h = { TEST_MAGIC, (uint8_t)g_inst };
        memcpy(pkt, &h, sizeof h); memcpy(pkt + sizeof h, buf, len);
        struct sockaddr_in to; memset(&to, 0, sizeof to);
        to.sin_family = AF_INET; to.sin_addr.s_addr = htonl(0x7F000001u);
        to.sin_port = htons((u_short)(TEST_PORT + (int)g_peers[peer].id));
        sendto(g_testSock, pkt, (int)sizeof h + len, 0, (struct sockaddr *)&to, sizeof to);
    } else if (g_mode == TUN_STEAM && S.msgs) {
        SteamIdent id; Ident(&id, g_peers[peer].id);
        // reliable: 8 | no nagle 1 ; unreliable: 0 ; + auto-restart broken session 32
        int flags = (reliable ? 9 : 0) | 32;
        int r = S.Send(S.msgs, &id, buf, (uint32_t)len, flags, 0);
        if (r != 1) {
            static DWORD lastErr;
            if (GetTickCount() - lastErr > 2000) { lastErr = GetTickCount(); LOG("steam: send to %s failed (result %d)", g_peers[peer].name, r); }
        }
    }
}

// ------------------------------------------------------ proxy sockets -------
typedef int (WINAPI *pfnBind)(SOCKET, const struct sockaddr *, int);
typedef int (WINAPI *pfnSendto)(SOCKET, const char *, int, int, const struct sockaddr *, int);
typedef int (WINAPI *pfnConnect)(SOCKET, const struct sockaddr *, int);
typedef struct hostent *(WINAPI *pfnGethostbyname)(const char *);
static pfnBind          real_bind;
static pfnSendto        real_sendto;
static pfnConnect       real_connect;
static pfnGethostbyname real_gethostbyname;

#define MAX_UDP 64
#define MAX_LISTEN 16
#define MAX_STREAMS 64
typedef struct { int used, peer; u_short port; SOCKET s; } UdpProxy;        // bound peerVA:port
typedef struct { int used, peer; u_short port; SOCKET s; } Listener;        // bound peerVA:port, listening
typedef struct { int used, peer, opener; uint32_t id; SOCKET s; } Stream;   // opener: the local side accepted the game's connect
static UdpProxy g_udp[MAX_UDP];
static Listener g_lis[MAX_LISTEN];
static Stream   g_str[MAX_STREAMS];
static uint32_t g_nextStream = 1;

static SOCKET BoundSocket(int type, u_long nip, u_short nport)
{
    SOCKET s = socket(AF_INET, type, 0);
    if (s == INVALID_SOCKET) return s;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = nip; a.sin_port = nport;
    if (real_bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        char b[32]; LOG("tunnel: bind %s failed (%d)", A2S(nip, nport, b), WSAGetLastError());
        closesocket(s); return INVALID_SOCKET;
    }
    return s;
}

static UdpProxy *UdpFor(int peer, u_short nport)
{
    for (int i = 0; i < MAX_UDP; i++) if (g_udp[i].used && g_udp[i].peer == peer && g_udp[i].port == nport) return &g_udp[i];
    for (int i = 0; i < MAX_UDP; i++) if (!g_udp[i].used) {
        SOCKET s = BoundSocket(SOCK_DGRAM, VAddr(g_peers[peer].key), nport);
        if (s == INVALID_SOCKET) return NULL;
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
        g_udp[i].used = 1; g_udp[i].peer = peer; g_udp[i].port = nport; g_udp[i].s = s;
        char b[32]; LOG("tunnel: udp proxy for %s at %s", g_peers[peer].name, A2S(VAddr(g_peers[peer].key), nport, b));
        return &g_udp[i];
    }
    return NULL;
}

static int EnsureListener(int peer, u_short nport)
{
    for (int i = 0; i < MAX_LISTEN; i++) if (g_lis[i].used && g_lis[i].peer == peer && g_lis[i].port == nport) return 1;
    for (int i = 0; i < MAX_LISTEN; i++) if (!g_lis[i].used) {
        SOCKET s = BoundSocket(SOCK_STREAM, VAddr(g_peers[peer].key), nport);
        if (s == INVALID_SOCKET) return 0;
        if (listen(s, 8) != 0) { closesocket(s); return 0; }
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
        g_lis[i].used = 1; g_lis[i].peer = peer; g_lis[i].port = nport; g_lis[i].s = s;
        char b[32]; LOG("tunnel: tcp listener for %s at %s", g_peers[peer].name, A2S(VAddr(g_peers[peer].key), nport, b));
        return 1;
    }
    return 0;
}

static Stream *FindStream(int peer, uint32_t id, int opener)
{
    for (int i = 0; i < MAX_STREAMS; i++)
        if (g_str[i].used && g_str[i].peer == peer && g_str[i].id == id && g_str[i].opener == opener) return &g_str[i];
    return NULL;
}
static Stream *NewStream(void)
{
    for (int i = 0; i < MAX_STREAMS; i++) if (!g_str[i].used) { memset(&g_str[i], 0, sizeof g_str[i]); g_str[i].used = 1; return &g_str[i]; }
    return NULL;
}
static void CloseStream(Stream *st, int notify)
{
    if (!st->used) return;
    if (notify) {
        HdrData h = { M_CLOSE, st->id, (uint8_t)st->opener };
        TransportSend(st->peer, &h, sizeof h, 1);
    }
    closesocket(st->s);
    LOG("tunnel: tcp stream %u with %s closed", st->id, g_peers[st->peer].used ? g_peers[st->peer].name : "?");
    st->used = 0;
}

static void DropPeerSockets(int peer)
{
    for (int i = 0; i < MAX_UDP; i++) if (g_udp[i].used && g_udp[i].peer == peer) { closesocket(g_udp[i].s); g_udp[i].used = 0; }
    for (int i = 0; i < MAX_LISTEN; i++) if (g_lis[i].used && g_lis[i].peer == peer) { closesocket(g_lis[i].s); g_lis[i].used = 0; }
    for (int i = 0; i < MAX_STREAMS; i++) if (g_str[i].used && g_str[i].peer == peer) CloseStream(&g_str[i], 0);
    if (g_lastGamePeer == peer) g_lastGamePeer = -1;
}

// ------------------------------------------------- incoming messages -------
static void OnMessage(int peer, const uint8_t *m, int len)
{
    if (len < 1 || peer < 0) return;
    g_peers[peer].lastRx = GetTickCount();
    switch (m[0]) {
    case M_UDP: {
        if (len < (int)sizeof(HdrUdp)) return;
        HdrUdp h; memcpy(&h, m, sizeof h);
        UdpProxy *u = UdpFor(peer, h.src);
        if (!u) return;
        struct sockaddr_in to; memset(&to, 0, sizeof to);
        to.sin_family = AF_INET; to.sin_addr.s_addr = Tun_SelfAddr(); to.sin_port = h.dst;
        real_sendto(u->s, (const char *)m + sizeof h, len - (int)sizeof h, 0, (struct sockaddr *)&to, sizeof to);
        if (ntohs(h.dst) != 4659) g_lastGamePeer = peer;
        break;
    }
    case M_OPEN: {
        if (len < (int)sizeof(HdrOpen)) return;
        HdrOpen h; memcpy(&h, m, sizeof h);
        Stream *st = NewStream();
        if (!st) { HdrData c = { M_CLOSE, h.id, 0 }; TransportSend(peer, &c, sizeof c, 1); return; }
        SOCKET s = BoundSocket(SOCK_STREAM, VAddr(g_peers[peer].key), 0);
        struct sockaddr_in to; memset(&to, 0, sizeof to);
        to.sin_family = AF_INET; to.sin_addr.s_addr = Tun_SelfAddr(); to.sin_port = h.dst;
        if (s == INVALID_SOCKET || real_connect(s, (struct sockaddr *)&to, sizeof to) != 0) {
            char b[32]; LOG("tunnel: %s opened tcp to %s but the game refused (%d)", g_peers[peer].name,
                            A2S(to.sin_addr.s_addr, to.sin_port, b), WSAGetLastError());
            if (s != INVALID_SOCKET) closesocket(s);
            st->used = 0;
            HdrData c = { M_CLOSE, h.id, 0 }; TransportSend(peer, &c, sizeof c, 1);
            return;
        }
        BOOL nd = TRUE; setsockopt(s, IPPROTO_TCP, 0x0001 /*TCP_NODELAY*/, (const char *)&nd, sizeof nd);
        st->peer = peer; st->id = h.id; st->opener = 0; st->s = s;
        g_lastGamePeer = peer;
        char b[32]; LOG("tunnel: tcp stream %u from %s -> game %s", h.id, g_peers[peer].name, A2S(to.sin_addr.s_addr, to.sin_port, b));
        break;
    }
    case M_DATA: case M_CLOSE: {
        if (len < (int)sizeof(HdrData)) return;
        HdrData h; memcpy(&h, m, sizeof h);
        Stream *st = FindStream(peer, h.id, !h.fromOpener);
        if (!st) return;
        if (m[0] == M_CLOSE) { CloseStream(st, 0); return; }
        const char *p = (const char *)m + sizeof h; int n = len - (int)sizeof h;
        while (n > 0) {                       // blocking socket: loopback, delivers quickly
            int w = send(st->s, p, n, 0);
            if (w <= 0) { CloseStream(st, 1); break; }
            p += w; n -= w;
        }
        break;
    }
    }
}

// ------------------------------------------------------- steam glue -------
// Lobby state for reconnecting. g_lastLobby = the lobby we lost (rejoin it first);
// g_needCreate = make a fresh lobby when g_retryAt comes.
static uint64_t g_lastLobby;
static int      g_rejoinTries, g_needCreate;
static DWORD    g_retryAt;
static uint64_t g_lobbyCall;

static void LobbyLost(const char *why)
{
    if (!S.lobby) return;
    LOG("steam: lost the lobby (%s) - rejoining", why);
    g_lastLobby = S.lobby; S.lobby = 0;
    g_rejoinTries = 0; g_retryAt = GetTickCount();
    // keep everyone's connections for the grace period; they come back with the lobby
    for (int p = 0; p < MAX_PEERS; p++) if (g_peers[p].used && !g_peers[p].goneAt) g_peers[p].goneAt = GetTickCount() | 1;
}

static void SteamRefreshMembers(void)
{
    if (!S.lobby) return;
    int n = S.NumMembers(S.mm, S.lobby);
    uint64_t ids[16]; int cnt = 0, meIn = 0;
    for (int i = 0; i < n && cnt < 16; i++) { ids[cnt] = S.MemberByIdx(S.mm, S.lobby, i); meIn |= ids[cnt] == S.me; cnt++; }
    // Steam dropped us from the lobby without telling us (network or Steam hiccup):
    // the member list comes back empty. Rejoin instead of showing LOBBY (0).
    if (!meIn) { LobbyLost(n ? "not a member any more" : "lobby is empty"); return; }
    // peers that left: keep their sockets for a while, they may be back (disconnect)
    for (int p = 0; p < MAX_PEERS; p++) if (g_peers[p].used) {
        int still = 0;
        for (int i = 0; i < cnt; i++) if (ids[i] == g_peers[p].id) still = 1;
        if (!still && !g_peers[p].goneAt) { LOG("steam: %s left the lobby (connection kept %ds in case they come back)", g_peers[p].name, PEER_GRACE_MS / 1000); g_peers[p].goneAt = GetTickCount() | 1; }
        if (still && g_peers[p].goneAt) { LOG("steam: %s is back", g_peers[p].name); g_peers[p].goneAt = 0; }
    }
    // add new peers
    for (int i = 0; i < cnt; i++) {
        if (ids[i] == S.me || PeerById(ids[i]) >= 0) continue;
        for (int p = 0; p < MAX_PEERS; p++) if (!g_peers[p].used) {
            Peer *pp = &g_peers[p]; memset(pp, 0, sizeof *pp);
            pp->used = 1; pp->id = ids[i];
            uint32_t h = (uint32_t)ids[i] * 2654435761u;
            pp->key = (uint16_t)(0x0100 + (h >> 16) % 0xFE00);
            snprintf(pp->name, sizeof pp->name, "%s", S.FriendName(S.friends, ids[i]));
            char b[32]; LOG("steam: %s joined, virtual address %s", pp->name, A2S(VAddr(pp->key), 0, b));
            if (pp->key == g_selfKey) LOG("steam: WARNING virtual address clash with %s", pp->name);
            break;
        }
    }
}

static void SteamOnCallback(int id, const uint8_t *d, int cb)
{
    (void)cb;
    switch (id) {
    case 513: {   // LobbyCreated_t { EResult; uint64 lobby @8 }
        int res; memcpy(&res, d, 4);
        uint64_t lob; memcpy(&lob, d + 8, 8);
        static uint64_t done;
        if (lob == done) break;
        done = lob;
        LOG("steam: lobby created (result %d)", res);
        if (res == 1) S.SetLobbyData(S.mm, lob, "game", "cgt-squadmod");
        else { g_needCreate = 1; g_retryAt = GetTickCount() + 10000; }   // Steam offline? try again
        break;
    }
    case 504: {   // LobbyEnter_t { uint64 lobby; uint32 perms; bool locked; uint32 response @16 }
        uint64_t lob; memcpy(&lob, d, 8);
        uint32_t resp; memcpy(&resp, d + 16, 4);
        if (resp != 1) {
            // the answer arrives twice (callback + call result); report it once
            static DWORD lastFail; if (GetTickCount() - lastFail > 500) LOG("steam: could not enter lobby (response %u)", resp);
            lastFail = GetTickCount();
            // 2 = the lobby doesn't exist any more (everyone left): stop retrying, make a new one now
            if (resp == 2 && lob == g_lastLobby) { g_rejoinTries = 99; g_retryAt = GetTickCount(); }
            break;
        }
        if (S.lobby && S.lobby != lob) S.LeaveLobby(S.mm, S.lobby);   // a late answer for an old lobby
        S.lobby = lob; g_lastLobby = 0; g_needCreate = 0;
        LOG("steam: in lobby %llu (host %s)", (unsigned long long)lob,
            S.LobbyOwner(S.mm, lob) == S.me ? "me" : S.FriendName(S.friends, S.LobbyOwner(S.mm, lob)));
        SteamRefreshMembers();
        break;
    }
    case 506:     // LobbyChatUpdate_t
        SteamRefreshMembers();
        break;
    case 512: {   // LobbyKicked_t { uint64 lobby; uint64 admin; uint8 disconnected }
        uint64_t lob; memcpy(&lob, d, 8);
        if (lob == S.lobby) LobbyLost("removed by Steam");
        break;
    }
    case 333: {   // GameLobbyJoinRequested_t { lobby; friend }
        uint64_t lob; memcpy(&lob, d, 8);
        if (lob == S.lobby) break;
        LOG("steam: joining friend's lobby %llu", (unsigned long long)lob);
        if (S.lobby) { S.LeaveLobby(S.mm, S.lobby); S.lobby = 0; }
        g_lastLobby = 0; g_needCreate = 0;
        for (int p = 0; p < MAX_PEERS; p++) if (g_peers[p].used) { DropPeerSockets(p); g_peers[p].used = 0; g_peers[p].goneAt = 0; }
        S.JoinLobby(S.mm, lob);
        break;
    }
    case 1251: {  // SteamNetworkingMessagesSessionRequest_t { identity }
        uint64_t sid; memcpy(&sid, d + 8, 8);
        SteamRefreshMembers();
        if (PeerById(sid) >= 0) { SteamIdent i; Ident(&i, sid); S.Accept(S.msgs, &i); LOG("steam: accepted connection from %s", S.FriendName(S.friends, sid)); }
        else LOG("steam: ignored connection from non-member %llu", (unsigned long long)sid);
        break;
    }
    case 1252:
        LOG("steam: a peer connection failed");
        break;
    }
}

// Once a second: notice a lobby Steam dropped us from, rejoin it, or make a new one;
// forget peers whose grace period ran out.
static void SteamTick(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 1000) return;
    last = now;
    if (S.lobby) SteamRefreshMembers();
    for (int p = 0; p < MAX_PEERS; p++)
        if (g_peers[p].used && g_peers[p].goneAt && now - g_peers[p].goneAt > PEER_GRACE_MS) {
            LOG("steam: %s did not come back, closing their connections", g_peers[p].name);
            DropPeerSockets(p); g_peers[p].used = 0; g_peers[p].goneAt = 0;
        }
    if (S.lobby || (int)(now - g_retryAt) < 0) return;
    if (g_lastLobby && g_rejoinTries < 6) {
        g_rejoinTries++;
        LOG("steam: rejoining lobby %llu (try %d)", (unsigned long long)g_lastLobby, g_rejoinTries);
        S.JoinLobby(S.mm, g_lastLobby);
        g_retryAt = now + 5000;
    } else if (g_lastLobby || g_needCreate) {
        LOG("steam: could not get back into the old lobby - creating a new one (invite your friends again)");
        g_lastLobby = 0;
        g_lobbyCall = S.CreateLobby(S.mm, 1 /*friends only*/, 4);
        g_retryAt = now + 15000;
        g_needCreate = 1;        // cleared when we are in the new lobby
    }
}

static void SteamPump(void)
{
    S.RunFrame(S.pipe);

    struct { int user; int id; uint8_t *param; int cb; } msg;
    while (S.GetNext(S.pipe, &msg)) {

        if (msg.id == 703) {              // SteamAPICallCompleted_t { uint64 call; int id; uint32 cb }
            uint64_t call; int cid; uint32_t ccb;
            memcpy(&call, msg.param, 8); memcpy(&cid, msg.param + 8, 4); memcpy(&ccb, msg.param + 12, 4);
            static uint8_t buf[4096]; uint8_t failed = 0;
            if (ccb <= sizeof buf && S.CallResult(S.pipe, call, buf, (int)ccb, cid, &failed) && !failed)
                SteamOnCallback(cid, buf, (int)ccb);
        } else SteamOnCallback(msg.id, msg.param, msg.cb);
        S.FreeLast(S.pipe);
    }
    void *in[32];
    int n = S.Recv(S.msgs, 0, in, 32);
    for (int i = 0; i < n; i++) {
        uint8_t *m = (uint8_t *)in[i];
        void *data; int size; uint64_t sid;
        memcpy(&data, m, 4); memcpy(&size, m + 4, 4); memcpy(&sid, m + 12 + 8, 8);
        int p = PeerById(sid);
        if (p >= 0) OnMessage(p, (const uint8_t *)data, size);
        S.Release(in[i]);
    }
}

#define GP(var, name) do { *(FARPROC *)&(var) = GetProcAddress(S.dll, name); if (!(var)) { LOG("steam: missing %s", name); return 0; } } while (0)
static int SteamInit(const char *dir)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%ssteam_appid.txt", dir);
    if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) { FILE *f = fopen(p, "w"); if (f) { fputs("480", f); fclose(f); } }
    SetEnvironmentVariableA("SteamAppId", "480");
    SetEnvironmentVariableA("SteamGameId", "480");
    snprintf(p, sizeof p, "%ssteam_api.dll", dir);
    S.dll = LoadLibraryA(p);
    if (!S.dll) { LOG("steam: steam_api.dll not found next to the game (%lu)", GetLastError()); return 0; }

    pfnVoid MDInit; pfnInitFlat InitFlat; pfnGetPipe GetPipe;
    pfnAccessor aUser, aFriends, aMM, aMsgs, aUtils; pfnSelfVoid InitRelay;
    GP(MDInit, "SteamAPI_ManualDispatch_Init");
    GP(InitFlat, "SteamAPI_InitFlat");
    GP(GetPipe, "SteamAPI_GetHSteamPipe");
    GP(S.RunFrame, "SteamAPI_ManualDispatch_RunFrame");
    GP(S.GetNext, "SteamAPI_ManualDispatch_GetNextCallback");
    GP(S.FreeLast, "SteamAPI_ManualDispatch_FreeLastCallback");
    GP(S.CallResult, "SteamAPI_ManualDispatch_GetAPICallResult");
    GP(aUser, "SteamAPI_SteamUser_v023");
    GP(aFriends, "SteamAPI_SteamFriends_v018");
    GP(aMM, "SteamAPI_SteamMatchmaking_v009");
    GP(aMsgs, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
    GP(aUtils, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
    GP(InitRelay, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
    GP(S.GetSteamID, "SteamAPI_ISteamUser_GetSteamID");
    GP(S.PersonaName, "SteamAPI_ISteamFriends_GetPersonaName");
    GP(S.FriendName, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    GP(S.CreateLobby, "SteamAPI_ISteamMatchmaking_CreateLobby");
    GP(S.JoinLobby, "SteamAPI_ISteamMatchmaking_JoinLobby");
    GP(S.LeaveLobby, "SteamAPI_ISteamMatchmaking_LeaveLobby");
    GP(S.NumMembers, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    GP(S.MemberByIdx, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    GP(S.LobbyOwner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    GP(S.SetLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData");
    GP(S.InviteDlg, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog");
    GP(S.SetRP, "SteamAPI_ISteamFriends_SetRichPresence");
    GP(S.Send, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    GP(S.Recv, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    GP(S.Accept, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    GP(S.Release, "SteamAPI_SteamNetworkingMessage_t_Release");

    char err[1024] = { 0 };
    int r = InitFlat(err);
    if (r != 0) { LOG("steam: init failed (%d) %s - is Steam running?", r, err); return 0; }
    MDInit();                     // must come after SteamAPI_Init
    S.pipe = GetPipe();
    S.user = aUser(); S.friends = aFriends(); S.mm = aMM(); S.msgs = aMsgs(); S.utils = aUtils();
    if (!S.user || !S.friends || !S.mm || !S.msgs) { LOG("steam: interfaces missing"); return 0; }
    InitRelay(S.utils);
    S.me = S.GetSteamID(S.user);
    snprintf(S.myName, sizeof S.myName, "%s", S.PersonaName(S.friends));
    uint32_t h = (uint32_t)S.me * 2654435761u;
    g_selfKey = (uint16_t)(0x0100 + (h >> 16) % 0xFE00);
    S.SetRP(S.friends, "status", "Conflict: Global Terror co-op (SquadMod)");
    g_lobbyCall = S.CreateLobby(S.mm, 1 /*friends only*/, 4);
    char b[32];
    LOG("steam: logged in as %s, virtual address %s; creating friends-only lobby", S.myName, A2S(Tun_SelfAddr(), 0, b));
    return 1;
}

// ------------------------------------------------------ game hooks -------
static void GamePortOf(SOCKET s, u_short *nport)
{
    struct sockaddr_in me; int ml = sizeof me;
    *nport = (getsockname(s, (struct sockaddr *)&me, &ml) == 0) ? me.sin_port : 0;
}

static void SendUdpToPeer(int peer, u_short src, u_short dst, const char *buf, int len)
{
    static char pkt[66000];
    if (len < 0 || len > 65000) return;
    HdrUdp h = { M_UDP, src, dst };
    memcpy(pkt, &h, sizeof h); memcpy(pkt + sizeof h, buf, len);
    TransportSend(peer, pkt, (int)sizeof h + len, 0);
}

static int FirstSeen(u_long ip, u_short port)
{
    static uint64_t seen[64]; static int n;
    uint64_t k = ((uint64_t)port << 32) | ip;
    for (int i = 0; i < n; i++) if (seen[i] == k) return 0;
    if (n < 64) seen[n++] = k;
    return 1;
}

static int WINAPI T_bind(SOCKET s, const struct sockaddr *name, int len)
{
    if (name && len >= (int)sizeof(struct sockaddr_in) && name->sa_family == AF_INET) {
        struct sockaddr_in a; memcpy(&a, name, sizeof a);
        a.sin_addr.s_addr = Tun_SelfAddr();
        int r = real_bind(s, (struct sockaddr *)&a, sizeof a);
        char b[32]; LOG("bind -> %s = %d (err %d)", A2S(a.sin_addr.s_addr, a.sin_port, b), r, r ? WSAGetLastError() : 0);
        return r;
    }
    return real_bind(s, name, len);
}

int WINAPI Tun_Sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
    if (g_mode && to && tolen >= (int)sizeof(struct sockaddr_in) && to->sa_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)to;
        EnterCriticalSection(&g_cs);
        int r = Route(a->sin_addr.s_addr);
        if (r == -1) { LeaveCriticalSection(&g_cs); return real_sendto(s, buf, len, flags, to, tolen); }
        u_short src; GamePortOf(s, &src);
        if (r == -2) {
            for (int p = 0; p < MAX_PEERS; p++) if (g_peers[p].used) SendUdpToPeer(p, src, a->sin_port, buf, len);
        } else if (r >= 0) {
            SendUdpToPeer(r, src, a->sin_port, buf, len);
        }
        if (FirstSeen(a->sin_addr.s_addr, a->sin_port)) {
            char b[32];
            LOG("sendto %s -> %s (%d bytes)", A2S(a->sin_addr.s_addr, a->sin_port, b),
                r == -2 ? "all peers" : r >= 0 ? g_peers[r].name : "nobody (unknown address)", len);
        }
        LeaveCriticalSection(&g_cs);
        return len;
    }
    return real_sendto(s, buf, len, flags, to, tolen);
}

static int WINAPI T_connect(SOCKET s, const struct sockaddr *to, int tolen)
{
    if (to && tolen >= (int)sizeof(struct sockaddr_in) && to->sa_family == AF_INET) {
        struct sockaddr_in a; memcpy(&a, to, sizeof a);
        u_long orig = a.sin_addr.s_addr;
        EnterCriticalSection(&g_cs);
        int r = Route(a.sin_addr.s_addr);
        if (r >= 0) {
            EnsureListener(r, a.sin_port);
            a.sin_addr.s_addr = VAddr(g_peers[r].key);
            if (ntohs(a.sin_port) == 4658 && g_pHostIp) *g_pHostIp = a.sin_addr.s_addr;
        }
        LeaveCriticalSection(&g_cs);
        // make sure the game's socket comes from our own virtual address
        struct sockaddr_in me; int ml = sizeof me;
        if (getsockname(s, (struct sockaddr *)&me, &ml) != 0 || me.sin_addr.s_addr == 0) {
            memset(&me, 0, sizeof me); me.sin_family = AF_INET; me.sin_addr.s_addr = Tun_SelfAddr();
            real_bind(s, (struct sockaddr *)&me, sizeof me);
        }
        int res = real_connect(s, (struct sockaddr *)&a, sizeof a);
        int e = WSAGetLastError();
        char b1[32], b2[32];
        LOG("connect %s -> %s (%s) = %d (err %d)", A2S(orig, a.sin_port, b1), A2S(a.sin_addr.s_addr, a.sin_port, b2),
            r >= 0 ? g_peers[r].name : r == -1 ? "local" : "UNKNOWN", res, res ? e : 0);
        WSASetLastError(e);
        return res;
    }
    return real_connect(s, to, tolen);
}

static struct hostent *WINAPI T_gethostbyname(const char *name)
{
    char me[256] = { 0 };
    gethostname(me, sizeof me);
    int own = !name || !*name || !_stricmp(name, me) || !_stricmp(name, "localhost");
    struct hostent *h = real_gethostbyname(name);
    if (own && h && h->h_addrtype == AF_INET && h->h_addr_list && h->h_addr_list[0]) {
        u_long mine = Tun_SelfAddr();
        memcpy(h->h_addr_list[0], &mine, 4);
        h->h_addr_list[1] = NULL;
    }
    return h;
}

// -------------------------------------------------------- pump thread -------
static HWND g_wnd;
static BOOL CALLBACK FindGameWnd(HWND w, LPARAM lp)
{
    DWORD pid; GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) { *(HWND *)lp = w; return FALSE; }
    return TRUE;
}
static void UpdateTitle(void)
{
    static DWORD t; static char last[160];
    if (GetTickCount() - t < 1000) return;
    t = GetTickCount();
    if (!g_wnd || !IsWindow(g_wnd)) { g_wnd = NULL; EnumWindows(FindGameWnd, (LPARAM)&g_wnd); }
    if (!g_wnd) return;
    char title[160]; int n;
    if (g_mode == TUN_STEAM)
        n = snprintf(title, sizeof title, "Conflict: Global Terror - Steam %s (%d player%s) - F7 invite friends",
                     S.lobby ? "lobby" : "connecting", PeerCount() + 1, PeerCount() ? "s" : "");
    else
        n = snprintf(title, sizeof title, "Conflict: Global Terror [copy %d, tunnel test]", g_inst);
    (void)n;
    if (strcmp(title, last)) { strcpy(last, title); SetWindowTextA(g_wnd, title); }
}

static void PollSockets(int timeoutMs)
{
    fd_set rd; FD_ZERO(&rd);
    if (g_testSock != INVALID_SOCKET) FD_SET(g_testSock, &rd);
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < MAX_UDP; i++) if (g_udp[i].used) FD_SET(g_udp[i].s, &rd);
    for (int i = 0; i < MAX_LISTEN; i++) if (g_lis[i].used) FD_SET(g_lis[i].s, &rd);
    for (int i = 0; i < MAX_STREAMS; i++) if (g_str[i].used) FD_SET(g_str[i].s, &rd);
    LeaveCriticalSection(&g_cs);
    if (rd.fd_count == 0) { Sleep(timeoutMs); return; }
    struct timeval tv = { 0, timeoutMs * 1000 };
    if (select(0, &rd, NULL, NULL, &tv) <= 0) return;

    static char buf[66000];
    EnterCriticalSection(&g_cs);
    if (g_testSock != INVALID_SOCKET && FD_ISSET(g_testSock, &rd)) {
        for (;;) {
            int r = recv(g_testSock, buf, sizeof buf, 0);
            if (r < (int)sizeof(HdrTest)) break;
            HdrTest h; memcpy(&h, buf, sizeof h);
            if (h.magic != TEST_MAGIC) continue;
            int p = PeerById(h.inst);
            if (p >= 0) OnMessage(p, (uint8_t *)buf + sizeof h, r - (int)sizeof h);
        }
    }
    // the game sent something to a proxy directly (connected UDP socket)
    for (int i = 0; i < MAX_UDP; i++) if (g_udp[i].used && FD_ISSET(g_udp[i].s, &rd)) {
        struct sockaddr_in from; int fl = sizeof from;
        int r = recvfrom(g_udp[i].s, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (r >= 0) SendUdpToPeer(g_udp[i].peer, from.sin_port, g_udp[i].port, buf, r);
    }
    for (int i = 0; i < MAX_LISTEN; i++) if (g_lis[i].used && FD_ISSET(g_lis[i].s, &rd)) {
        SOCKET c = accept(g_lis[i].s, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        Stream *st = NewStream();
        if (!st) { closesocket(c); continue; }
        u_long nb = 0; ioctlsocket(c, FIONBIO, &nb);
        BOOL nd = TRUE; setsockopt(c, IPPROTO_TCP, 0x0001, (const char *)&nd, sizeof nd);
        st->peer = g_lis[i].peer; st->opener = 1; st->id = g_nextStream++; st->s = c;
        HdrOpen h = { M_OPEN, st->id, g_lis[i].port };
        TransportSend(st->peer, &h, sizeof h, 1);
        LOG("tunnel: game opened tcp stream %u to %s port %u", st->id, g_peers[st->peer].name, ntohs(g_lis[i].port));
    }
    for (int i = 0; i < MAX_STREAMS; i++) if (g_str[i].used && FD_ISSET(g_str[i].s, &rd)) {
        Stream *st = &g_str[i];
        HdrData h = { M_DATA, st->id, (uint8_t)st->opener };
        memcpy(buf, &h, sizeof h);
        int r = recv(st->s, buf + sizeof h, 16000, 0);
        if (r <= 0) { CloseStream(st, 1); continue; }
        TransportSend(st->peer, buf, (int)sizeof h + r, 1);
    }
    LeaveCriticalSection(&g_cs);
}

static const char *g_dirStr;
static int g_dbgKeys;
static volatile LONG g_inviteReq;
static DWORD WINAPI PumpThread(LPVOID unused)
{
    (void)unused;
    if (g_mode == TUN_STEAM && !SteamInit(g_dirStr)) {
        LOG("steam: tunnel disabled; LAN play only");
        g_mode = TUN_OFF;
        return 0;
    }
    int f7 = 0;
    for (;;) {
        if (g_mode == TUN_STEAM) {
            EnterCriticalSection(&g_cs);
            SteamPump();
            SteamTick();
            LeaveCriticalSection(&g_cs);
            if (g_dbgKeys && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_F8) & 1)) {
                // Debug=1 test: leave the lobby behind the game's back, as a network drop would
                EnterCriticalSection(&g_cs);
                if (S.lobby) { LOG("steam: DEBUG leaving the lobby to test reconnecting"); S.LeaveLobby(S.mm, S.lobby); }
                LeaveCriticalSection(&g_cs);
            }
            int k = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
            HWND fg = GetForegroundWindow(); DWORD pid = 0; if (fg) GetWindowThreadProcessId(fg, &pid);
            if (k && !f7 && pid == GetCurrentProcessId() && S.lobby) { LOG("steam: opening invite dialog"); S.InviteDlg(S.friends, S.lobby); }
            f7 = k;
            if (InterlockedExchange(&g_inviteReq, 0) && S.lobby) { LOG("steam: opening invite dialog (menu)"); S.InviteDlg(S.friends, S.lobby); }
        }
        UpdateTitle();
        PollSockets(g_mode == TUN_STEAM ? 2 : 5);
    }
}

// Steam name for the game's player name: printable ASCII only (the game font has
// nothing else), at most 15 characters. NULL until Steam is up.
const char *Tun_PlayerName(void)
{
    static char out[16];
    if (g_mode != TUN_STEAM || !S.myName[0]) return NULL;
    int n = 0;
    for (const unsigned char *c = (const unsigned char *)S.myName; *c && n < 15; c++)
        if (*c >= 0x20 && *c < 0x7F) out[n++] = (char)*c;
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
    return n ? out : NULL;
}

// For the in-game STEAM screen.
// Status: -1 = Steam play off, 0 = connecting, 1 = in lobby, 2 = reconnecting.
int Tun_SteamStatus(void)
{
    if (g_mode != TUN_STEAM) return -1;
    return S.lobby ? 1 : (g_lastLobby || g_needCreate) ? 2 : 0;
}
void Tun_Invite(void) { InterlockedExchange(&g_inviteReq, 1); }
// Lobby member names, comma separated, printable ASCII. Returns the member count.
int Tun_LobbyNames(char *out, int cap)
{
    int n = 0, len = 0;
    out[0] = 0;
    if (g_mode != TUN_STEAM || !S.dll) return 0;
    EnterCriticalSection(&g_cs);
    if (S.lobby) {
        n = S.NumMembers(S.mm, S.lobby);
        for (int i = 0; i < n; i++) {
            const char *nm = S.FriendName(S.friends, S.MemberByIdx(S.mm, S.lobby, i));
            if (i && len < cap - 3) { out[len++] = ','; out[len++] = ' '; }
            for (const unsigned char *c = (const unsigned char *)(nm ? nm : "?"); *c && len < cap - 1; c++)
                if (*c >= 0x20 && *c < 0x7F) out[len++] = (char)*c;
        }
        out[len] = 0;
    }
    LeaveCriticalSection(&g_cs);
    return n;
}

// ------------------------------------------------------------- start -------
int Tun_Active(void) { return g_mode != TUN_OFF; }

int Tun_Start(int mode, int instance, const char *dir, pfnLog log, pfnPatchIAT patch, u_long *pHostIp)
{
    L = log; g_mode = mode; g_inst = instance; g_pHostIp = pHostIp; g_dirStr = dir;
    { char ini[MAX_PATH]; snprintf(ini, sizeof ini, "%ssquadmod.ini", dir); g_dbgKeys = GetPrivateProfileIntA("SquadMod", "Debug", 0, ini); }
    InitializeCriticalSection(&g_cs);
    HMODULE ws = LoadLibraryA("wsock32.dll");
    real_bind = (pfnBind)patch("WSOCK32.dll", "bind", (void *)T_bind);
    real_sendto = (pfnSendto)patch("WSOCK32.dll", "sendto", (void *)Tun_Sendto);
    real_connect = (pfnConnect)patch("WSOCK32.dll", "connect", (void *)T_connect);
    real_gethostbyname = (pfnGethostbyname)patch("WSOCK32.dll", "gethostbyname", (void *)T_gethostbyname);
    if (!real_bind) real_bind = (pfnBind)GetProcAddress(ws, "bind");
    if (!real_sendto) real_sendto = (pfnSendto)GetProcAddress(ws, "sendto");
    if (!real_connect) real_connect = (pfnConnect)GetProcAddress(ws, "connect");
    if (!real_gethostbyname) real_gethostbyname = (pfnGethostbyname)GetProcAddress(ws, "gethostbyname");

    WSADATA wd; WSAStartup(MAKEWORD(1, 1), &wd);
    if (mode == TUN_TEST) {
        g_ns = (uint8_t)(20 + instance);       // each copy sees its own namespace on the shared PC
        g_selfKey = (uint16_t)instance;
        int other = instance == 1 ? 2 : 1;
        Peer *p = &g_peers[0]; p->used = 1; p->id = (uint64_t)other; p->key = (uint16_t)other;
        snprintf(p->name, sizeof p->name, "copy %d", other);
        g_testSock = BoundSocket(SOCK_DGRAM, htonl(0x7F000001u), htons((u_short)(TEST_PORT + instance)));
        if (g_testSock == INVALID_SOCKET) { LOG("tunnel test: transport socket failed"); g_mode = TUN_OFF; return 0; }
        u_long nb = 1; ioctlsocket(g_testSock, FIONBIO, &nb);
        char b[32], b2[32]; LOG("tunnel test: me = %s, copy %d = %s", A2S(Tun_SelfAddr(), 0, b), other, A2S(VAddr(p->key), 0, b2));
    }
    CreateThread(NULL, 0, PumpThread, NULL, 0, NULL);
    LOG("tunnel started (%s)", mode == TUN_STEAM ? "steam" : "test");
    return 1;
}
