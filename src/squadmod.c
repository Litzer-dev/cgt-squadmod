// Conflict: Global Terror - AI Squad in Multiplayer (proxy dinput8.dll)
//
// Replaces the network squad spawner (FUN_004731b0 @ 0x004731b0) so that squad
// slots with no human player still get their soldier, owned by the host
// (machine 0) and therefore simulated by the host's AI and mirrored on clients.
//
// Also contains an optional loopback mode so two game instances can talk over
// LAN on the same PC (instance N binds 127.0.0.N, broadcasts go to the other).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

// ---------------------------------------------------------------- config ---
static struct {
    int enabled;        // master switch for the squad fill
    int fillCount;      // how many squad slots must exist (1..4)
    int hostOwnsAI;     // empty slots owned by host (1) or by slot index (0, vanilla debug behaviour)
    int log;
    int loopback;       // same-PC testing
    int freeMouse;      // DirectInput non-exclusive (don't lock the cursor)
    int debug;          // extra diagnostic hooks (order handler / net send logging)
    int steam;          // play over Steam (virtual LAN through a Steam lobby)
    int tunnelTest;     // with loopback: route the two copies through the tunnel (tests the Steam path without Steam)
} cfg;

// tunnel.c
int    Tun_Start(int mode, int instance, const char *dir, void (*log)(const char *, ...),
                 void *(*patch)(const char *, const char *, void *), u_long *pHostIp);
int    Tun_Active(void);
u_long Tun_SelfAddr(void);
const char *Tun_PlayerName(void);
int WINAPI Tun_Sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen);
#define RSendto(...) (Tun_Active() ? Tun_Sendto(__VA_ARGS__) : sendto(__VA_ARGS__))

static int  g_instance = 1;
static char g_dir[MAX_PATH];
static FILE *g_log;

static void logf(const char *fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// ------------------------------------------------------- game addresses ---
// All from ConflictGlobal.exe (image base 0x400000, Dec 2005 build).
#define A_SPAWN_SQUAD      0x004731b0  // FUN_004731b0  (replaced)
#define A_PNETMGR          0x006ffe14  // -> net manager; +0xC0 = session
#define A_PENTMGR          0x008de7c0  // -> entity manager
#define A_LOCAL_SLOT       0x007017c4  // byte: this machine's slot (host = 0)
#define A_DIRTY_FLAG       0x008de720
#define A_CB_OWNER_LOCAL   0x004730f0  // entity callback (host)
#define A_CB_OWNER_HOST    0x00473110  // entity callback (client)
#define A_NETSYNC_CB       0x00473410

typedef int   (__thiscall *pfnSlotOccupied)(void *session, int slot);   // 0x0058f3a0
typedef int   (__cdecl    *pfnSlotToId)(int slot);                       // 0x0046b090
typedef void  (__cdecl    *pfnCreateSlot)(int slot);                     // 0x0046cee0
typedef void  (__cdecl    *pfnFinishSlot)(int slot);                     // 0x0046cf80
typedef void *(__thiscall *pfnGetEntity)(void *entmgr, int id);          // 0x0062f560
typedef void  (__thiscall *pfnForEachEnt)(void *entmgr, int mask, void *cb); // 0x0062ce80
typedef void  (__cdecl    *pfnResetSlots)(void);                         // 0x0046b0a0
typedef void  (__cdecl    *pfnNetReg1)(int netId, int a, void *cb, int b); // 0x00574a14
typedef void  (__cdecl    *pfnNetReg2)(int netId, int a);                // 0x00574a74
typedef void  (__thiscall *pfnActivate)(void *ent, int a, int b);        // vtable[2]

#define SlotOccupied ((pfnSlotOccupied)0x0058f3a0)
#define SlotToId     ((pfnSlotToId)    0x0046b090)
#define CreateSlot   ((pfnCreateSlot)  0x0046cee0)
#define FinishSlot   ((pfnFinishSlot)  0x0046cf80)
#define GetEntity    ((pfnGetEntity)   0x0062f560)
#define ForEachEnt   ((pfnForEachEnt)  0x0062ce80)
#define ResetSlots   ((pfnResetSlots)  0x0046b0a0)
#define NetReg1      ((pfnNetReg1)     0x00574a14)
#define NetReg2      ((pfnNetReg2)     0x00574a74)

#define SLOT_ID_TABLE ((int *)0x006f4314)   // slot -> entity id (read by 0x46b090)
// The game decides "this squad member is a human player" from the slot table.
// After a character switch it must point at the body the player now controls.
static void SetSlotBody(int slot, uint32_t id)
{
    if (slot < 0 || slot > 3 || SLOT_ID_TABLE[slot] == (int)id) return;
    logf("slot table: slot %d %d -> %u", slot, SLOT_ID_TABLE[slot], id);
    SLOT_ID_TABLE[slot] = (int)id;
}
#define U8(p,o)  (*(uint8_t  *)((uint8_t *)(p) + (o)))
#define U32(p,o) (*(uint32_t *)((uint8_t *)(p) + (o)))
#define I32(p,o) (*(int32_t  *)((uint8_t *)(p) + (o)))

// ------------------------------------------------ replacement spawner ---
static void __cdecl Hook_SpawnSquad(void)
{
    void *netmgr  = *(void **)A_PNETMGR;
    void *session = *(void **)((uint8_t *)netmgr + 0xC0);
    void *entmgr  = *(void **)A_PENTMGR;
    uint8_t localSlot = *(uint8_t *)A_LOCAL_SLOT;

    uint32_t f500c = U32(session, 0x500C);
    int isHost = (f500c & 2) != 0;
    int dbgFill = (U32(session, 0x5008) & 4) != 0;
    uint32_t dbgCount = U32(session, 0x5010);

    logf("SpawnSquad: %s localSlot=%u 500c=%08X 5008=%08X 5010=%u",
         isHost ? "HOST" : "CLIENT", localSlot, f500c, U32(session, 0x5008), dbgCount);
    for (int s = 0; s < 4; s++)
        logf("  slot%d flags=%02X name='%.32s' occupied=%d", s,
             U8(session, s * 0x8C + 0x85), (char *)session + s * 0x8C,
             SlotOccupied(session, s));

    for (int s = 0; s < 4; s++) {
        char hx[0x8C * 3 + 1]; uint8_t *rec = (uint8_t *)session + s * 0x8C;
        for (int i = 0; i < 0x8C; i++) sprintf(hx + i * 3, "%02X ", rec[i]);
        logf("  slot%d rec: %s", s, hx);
    }

    // --- original prologue
    ForEachEnt(entmgr, 3, (void *)(isHost ? A_CB_OWNER_LOCAL : A_CB_OWNER_HOST));
    ResetSlots();

    for (int slot = 0; slot < 4; slot++) {
        int occupied = SlotOccupied(session, slot) != 0;
        int vanilla  = occupied || (dbgFill && (uint32_t)slot < dbgCount);
        int modFill  = cfg.enabled && slot < cfg.fillCount;
        if (!vanilla && !modFill) {
            logf("  slot%d: skipped (empty)", slot);
            continue;
        }
        int aiSlot = !occupied;   // nobody is playing this slot

        if (SlotToId(slot) == -1) {
            CreateSlot(slot);
            if (SlotToId(slot) == -1 && aiSlot) {
                // attempt A: vanilla dev "fill slots" flags
                uint32_t s8 = U32(session, 0x5008), s10 = U32(session, 0x5010);
                U32(session, 0x5008) = s8 | 4; U32(session, 0x5010) = 4;
                CreateSlot(slot);
                U32(session, 0x5008) = s8; U32(session, 0x5010) = s10;
                logf("  slot%d: create with dev-fill flags -> id=%d", slot, SlotToId(slot));
            }
            if (SlotToId(slot) == -1 && aiSlot) {
                // attempt B: borrow the host's slot record, but with a character
                // (record +0x44) that no other slot uses yet
                uint8_t save[0x8C];
                uint8_t *rec = (uint8_t *)session + slot * 0x8C;
                memcpy(save, rec, sizeof save);
                for (int ch = 0; ch <= 4 && SlotToId(slot) == -1; ch++) {
                    memcpy(rec, (uint8_t *)session, 0x8C);
                    lstrcpyA((char *)rec, "AI");
                    U8(rec, 0x44) = (uint8_t)ch;
                    CreateSlot(slot);
                    int got = SlotToId(slot);
                    int dup = 0;
                    for (int o = 0; o < 4; o++)
                        if (o != slot && got != -1 && SlotToId(o) == got) dup = 1;
                    logf("  slot%d: character %d -> id=%d%s", slot, ch, got, dup ? " (taken)" : "");
                    if (dup) SLOT_ID_TABLE[slot] = -1;   // undo, try next character
                }
                memcpy(rec, save, sizeof save);
            }
        }
        int id = SlotToId(slot);
        void *ent = GetEntity(entmgr, id);
        if (!ent) { logf("  slot%d: id=%d -> no entity!", slot, id); continue; }

        uint8_t owner;
        if (aiSlot && cfg.hostOwnsAI) owner = 0;          // host simulates it
        else                          owner = (uint8_t)slot;

        void **vt = *(void ***)ent;
        U8(ent, 0x4A) = owner;
        *(int *)A_DIRTY_FLAG = 1;
        ((pfnActivate)vt[2])(ent, 1, 0);

        int netId = I32(ent, 0x3AC);
        if (owner != localSlot && netId != -1) {
            // remote-controlled on this machine: mirror the owner's state
            U8(ent, 0xF1C) = 3;
            NetReg1(netId, 1, (void *)A_NETSYNC_CB, 1);
            NetReg2(netId, 1);
        }
        FinishSlot(slot);

        logf("  slot%d: id=%d ent=%p owner=%u %s%s netId=%d", slot, id, ent, owner,
             aiSlot ? "AI" : "PLAYER",
             owner == localSlot ? " (local sim)" : " (mirrored)", netId);
    }
}

// ----------------------------------------------------------- patching ---

static int WriteJmp(uintptr_t at, void *to)
{
    DWORD old;
    uint8_t *p = (uint8_t *)at;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    p[0] = 0xE9;
    *(int32_t *)(p + 1) = (int32_t)((uintptr_t)to - (at + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}

static void *PatchIAT(const char *dll, const char *func, void *repl)
{
    HMODULE exe = GetModuleHandleA(NULL);
    uint8_t *base = (uint8_t *)exe;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(base + dd.VirtualAddress); d->Name; d++) {
        if (lstrcmpiA((char *)base + d->Name, dll)) continue;
        // match on the resolved address, so imports by ordinal work too
        uintptr_t target = (uintptr_t)GetProcAddress(GetModuleHandleA(dll), func);
        if (!target) return NULL;
        IMAGE_THUNK_DATA *addrs = (void *)(base + d->FirstThunk);
        for (; addrs->u1.Function; addrs++) {
            if (addrs->u1.Function != target) continue;
            DWORD old; void *orig = (void *)addrs->u1.Function;
            VirtualProtect(&addrs->u1.Function, 4, PAGE_READWRITE, &old);
            addrs->u1.Function = (uintptr_t)repl;
            VirtualProtect(&addrs->u1.Function, 4, old, &old);
            return orig;
        }
    }
    return NULL;
}


// ------------------------------------------- order handler diagnostics ---
// FUN_004fe770 (thiscall, no args) = per-local-player squad order input.
// this+0x7c -> player ctl (X); X+0x448 = controlled soldier; this+0x88 = squad
// squad+0x81 = member count, +0x7e = player's own index, +0xdc[4] = members,
// +0xf0 = selected member. Orders need soldier+0xd7c bit0 and an orderable member.
#define A_ORDER_HANDLER 0x004fe770
static uint8_t g_orderTramp[16];

static void LogSoldier(const char *tag, void *e)
{
    if (!e) { logf("    %s: (null)", tag); return; }
    logf("    %s: %p type=%u owner=%u d7c=%08X 790=%08X f1c=%u netId=%d hp=%.1f",
         tag, e, U32(e, 4), U8(e, 0x4A), U32(e, 0xD7C), U32(e, 0x790), U8(e, 0xF1C),
         I32(e, 0x3AC), *(float *)((uint8_t *)e + 0x13C));
}

static void LogNetState(const char *why);
static void RelayPump(void *handlerSelf);
static void *Session(void); static int IsNetGame(void); static int IsHost(void);
static void ClientRequestSwitch(void *cur, int dir);

// ---- character switching (Up / Down arrow) ---------------------------------
// Campaign switching = 0x422f60(ctl, soldier) (__thiscall), reached from the
// squad panel (handler+0x88, members +0xdc[cnt @+0x81]). The game's own keys
// do nothing in MP, so the mod drives it. Host only for now: AI squadmates
// are simulated by the host, so the host can take over one locally.
typedef int (__thiscall *pfnSwitchTo)(void *ctl, void *soldier);
typedef int (__thiscall *pfnSelectable)(void *squad, int idx);
#define SwitchTo   ((pfnSwitchTo)0x00422f60)
#define Selectable ((pfnSelectable)0x004f6d50)
static int GameHasFocus(void)
{
    DWORD pid = 0; HWND w = GetForegroundWindow();
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}
static void SquadSwitchPoll(void *self)
{
    static int prevUp, prevDn;
    int up = (GetAsyncKeyState(VK_UP) & 0x8000) != 0, dn = (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
    int dir = (up && !prevUp) ? -1 : (dn && !prevDn) ? 1 : 0;
    prevUp = up; prevDn = dn;
    if (!dir || !cfg.enabled || !GameHasFocus()) return;
    void *ctl = *(void **)((uint8_t *)self + 0x7C);
    uint8_t *sq = *(uint8_t **)((uint8_t *)self + 0x88);
    if (!ctl || !sq) return;
    void *cur0 = *(void **)((uint8_t *)ctl + 0x448);
    if (Session() && IsNetGame() && !IsHost()) { if (cur0) ClientRequestSwitch(cur0, dir); return; }
    int cnt = (int8_t)sq[0x81];
    if (cnt < 2 || cnt > 4) return;
    void *cur = *(void **)((uint8_t *)ctl + 0x448);
    int at = -1;
    for (int i = 0; i < cnt; i++) if (*(void **)(sq + 0xDC + i * 4) == cur) at = i;
    if (at < 0) at = (int8_t)sq[0x7E];
    for (int step = 1; step < cnt; step++) {
        int i = ((at + dir * step) % cnt + cnt) % cnt;
        void *m = *(void **)(sq + 0xDC + i * 4);
        if (!m || m == cur) continue;
        if (U8(m, 0x4A) != 0) continue;                 // driven by another machine (remote human)
        if (!(U32(m, 0xD7C) & 2)) continue;              // not switchable
        if (!Selectable(sq, i)) continue;                // dead / mission says no
        int r = SwitchTo(ctl, m);
        if (r) SetSlotBody(*(uint8_t *)A_LOCAL_SLOT, U32(m, 8));
        if (*(void **)((uint8_t *)ctl + 0x448) == m) sq[0x7E] = (uint8_t)i;
        logf("switch: %s -> member %d (id %u) result=%d now=%p", dir < 0 ? "up" : "down", i, U32(m, 8), r,
             *(void **)((uint8_t *)ctl + 0x448));
        return;
    }
    logf("switch: no other squadmate available");
}

static void __stdcall OrderProbe(void *self)
{
    RelayPump(self);
    SquadSwitchPoll(self);
    if (!cfg.debug) return;
    static uint32_t lastHash[4]; static void *lastSelf[4];
    void *x = *(void **)((uint8_t *)self + 0x7C);
    void *sq = *(void **)((uint8_t *)self + 0x88);
    if (!x || !sq) return;
    void *sol = *(void **)((uint8_t *)x + 0x448);
    uint32_t h = (uint32_t)(uintptr_t)sol ^ U32(x, 0x450) * 31 ^ U8(sq, 0x81) * 131 ^ U8(sq, 0x7E) * 7;
    for (int i = 0; i < 4; i++) {
        void *m = *(void **)((uint8_t *)sq + 0xDC + i * 4);
        h = h * 33 ^ (uint32_t)(uintptr_t)m;
        if (m) h ^= U32(m, 0xD7C) * 17;
    }
    if (sol) h ^= U32(sol, 0xD7C) * 3;
    int k = 0;
    for (; k < 4 && lastSelf[k] && lastSelf[k] != self; k++);
    if (k == 4) k = 0;
    if (lastSelf[k] == self && lastHash[k] == h) return;
    lastSelf[k] = self; lastHash[k] = h;

    LogNetState("order handler");
    logf("OrderHandler this=%p ctl=%p ctl+450=%u squad=%p count=%u selfIdx=%d selected=%p flags118=%08X",
         self, x, U32(x, 0x450), sq, U8(sq, 0x81), (int8_t)U8(sq, 0x7E),
         *(void **)((uint8_t *)sq + 0xF0), U32(sq, 0x118));
    LogSoldier("player", sol);
    for (int i = 0; i < 4; i++) {
        char t[16]; snprintf(t, sizeof t, "member%d", i);
        LogSoldier(t, *(void **)((uint8_t *)sq + 0xDC + i * 4));
    }
}

void *g_orderProbePtr = (void *)OrderProbe;
void *g_orderTrampPtr = (void *)g_orderTramp;
__attribute__((naked, used)) static void OrderStub(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "pushl %ecx\n\t"
        "call *_g_orderProbePtr\n\t"
        "popal\n\t"
        "jmp *_g_orderTrampPtr\n\t");
}

static void InstallOrderProbe(void)
{
    // original prologue: 83 EC 08 | 53 | 56 | 8B F1  (7 bytes)
    static const uint8_t expect[7] = { 0x83, 0xEC, 0x08, 0x53, 0x56, 0x8B, 0xF1 };
    if (memcmp((void *)A_ORDER_HANDLER, expect, 7)) { logf("order probe: unexpected bytes, skipped"); return; }
    DWORD old;
    VirtualProtect(g_orderTramp, sizeof g_orderTramp, PAGE_EXECUTE_READWRITE, &old);
    memcpy(g_orderTramp, expect, 7);
    g_orderTramp[7] = 0xE9;
    *(int32_t *)(g_orderTramp + 8) = (int32_t)((A_ORDER_HANDLER + 7) - ((uintptr_t)g_orderTramp + 12));
    uint8_t *p = (uint8_t *)A_ORDER_HANDLER;
    VirtualProtect(p, 7, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9; *(int32_t *)(p + 1) = (int32_t)((uintptr_t)OrderStub - (A_ORDER_HANDLER + 5));
    p[5] = 0x90; p[6] = 0x90;
    VirtualProtect(p, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 7);
    logf("order probe installed");
}


// ---- diagnostics: network message send (0x46d3a0: mov eax,[esp+8]; jmp [0x117d0d8])
uint8_t g_sendTramp[16];
void *g_sendProbePtr;
void *g_sendTrampPtr = (void *)g_sendTramp;

static void __cdecl SendProbe(uint32_t *args)   // args[0] = return addr
{
    static int n;
    if (n > 400) return;
    uint32_t type = args[1]; uint8_t *buf = (uint8_t *)(uintptr_t)args[2];
    char hx[3 * 32 + 1] = "";
    if (buf && !IsBadReadPtr(buf, 32)) for (int i = 0; i < 32; i++) sprintf(hx + i * 3, "%02X ", buf[i]);
    // skip the most common periodic traffic after a few samples
    static uint32_t cnt[64];
    if (type < 64 && ++cnt[type] > 6 && type != 3) return;
    n++;
    logf("NetSend type=%u a2=%08X a3=%08X ret=%08X buf: %s", type, args[3], args[4], args[0], hx);
}

__attribute__((naked, used)) static void SendStub(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "leal 32(%esp), %eax\n\t"
        "pushl %eax\n\t"
        "call *_g_sendProbePtr\n\t"
        "addl $4, %esp\n\t"
        "popal\n\t"
        "jmp *_g_sendTrampPtr\n\t");
}

static void InstallSendProbe(void)
{
    uint8_t *p = (uint8_t *)0x0046d3a0;
    static const uint8_t expect[10] = { 0x8B, 0x44, 0x24, 0x08, 0xFF, 0x25, 0xD8, 0xD0, 0x17, 0x01 };
    if (memcmp(p, expect, 10)) { logf("send probe: unexpected bytes"); return; }
    g_sendProbePtr = (void *)SendProbe;
    DWORD old;
    VirtualProtect(g_sendTramp, sizeof g_sendTramp, PAGE_EXECUTE_READWRITE, &old);
    memcpy(g_sendTramp, expect, 10);              // relocatable: absolute indirect jmp
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9; *(int32_t *)(p + 1) = (int32_t)((uintptr_t)SendStub - ((uintptr_t)p + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    logf("send probe installed");
}

typedef int (__cdecl *pfnInt0)(void);
static void LogNetState(const char *why)
{
    void *netmgr = *(void **)A_PNETMGR;
    void *session = netmgr ? *(void **)((uint8_t *)netmgr + 0xC0) : NULL;
    int a = ((pfnInt0)0x00471ec0)();
    int b = ((pfnInt0)0x00471ff0)();
    int c = session ? ((int (__thiscall *)(void *))0x0058fae0)(session) : -1;
    logf("NetState(%s): 471ec0=%d 471ff0=%d 58fae0=%d EBXconst[117dafc]=%d 500c=%08X 4200->%d",
         why, a, b, c, *(int *)0x0117dafc, session ? U32(session, 0x500C) : 0,
         session && *(void **)((uint8_t *)session + 0x4200) ? **(int **)((uint8_t *)session + 0x4200) : -1);
}

// --------------------------------------------------- loopback testing ---
typedef int (WINAPI *pfnBind)(SOCKET, const struct sockaddr *, int);
typedef int (WINAPI *pfnSendto)(SOCKET, const char *, int, int, const struct sockaddr *, int);
static pfnBind   real_bind;
static pfnSendto real_sendto;

static u_long LoopAddr(int inst) { return htonl(0x7F000000u | (u_long)inst); } // 127.0.0.N

static int WINAPI Hook_bind(SOCKET s, const struct sockaddr *name, int len)
{
    struct sockaddr_in a;
    if (name && len >= (int)sizeof(a) && name->sa_family == AF_INET) {
        memcpy(&a, name, sizeof(a));
        a.sin_addr.s_addr = LoopAddr(g_instance);
        int r = real_bind(s, (struct sockaddr *)&a, sizeof(a));
        logf("bind -> 127.0.0.%d:%u = %d (err %d)", g_instance, ntohs(a.sin_port), r,
             r ? WSAGetLastError() : 0);
        return r;
    }
    return real_bind(s, name, len);
}

static const char *IpStr(u_long nip, u_short nport, char *b)
{
    u_long ip = ntohl(nip);
    sprintf(b, "%lu.%lu.%lu.%lu:%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, ntohs(nport));
    return b;
}

// log each distinct (direction, address) pair only once
static int FirstTime(char dir, u_long ip, u_short port)
{
    static uint64_t seen[128]; static int n;
    uint64_t k = ((uint64_t)(uint8_t)dir << 48) | ((uint64_t)port << 32) | ip;
    for (int i = 0; i < n; i++) if (seen[i] == k) return 0;
    if (n < 128) seen[n++] = k;
    return 1;
}

// In loopback test mode every address that is not 127.x (broadcast, real LAN IP,
// 0.0.0.0) is meant for the other copy on this PC.
static int RedirectToOther(struct sockaddr_in *a)
{
    u_long ip = ntohl(a->sin_addr.s_addr);
    if ((ip >> 24) == 127 && a->sin_addr.s_addr != LoopAddr(g_instance)) return 0;
    a->sin_addr.s_addr = LoopAddr(g_instance == 1 ? 2 : 1);
    return 1;
}

static int WINAPI Hook_sendto(SOCKET s, const char *buf, int len, int flags,
                              const struct sockaddr *to, int tolen)
{
    struct sockaddr_in a;
    if (to && tolen >= (int)sizeof(a) && to->sa_family == AF_INET) {
        memcpy(&a, to, sizeof(a));
        u_long orig = a.sin_addr.s_addr;
        int red = RedirectToOther(&a);
        if (FirstTime('s', orig, a.sin_port)) {
            char b1[32], b2[32];
            logf("sendto %s%s%s (%d bytes)", IpStr(orig, a.sin_port, b1),
                 red ? " -> " : "", red ? IpStr(a.sin_addr.s_addr, a.sin_port, b2) : "", len);
        }
        return real_sendto(s, buf, len, flags, (struct sockaddr *)&a, sizeof(a));
    }
    return real_sendto(s, buf, len, flags, to, tolen);
}

typedef int (WINAPI *pfnConnect)(SOCKET, const struct sockaddr *, int);
typedef int (WINAPI *pfnRecvfrom)(SOCKET, char *, int, int, struct sockaddr *, int *);
typedef SOCKET (WINAPI *pfnAccept)(SOCKET, struct sockaddr *, int *);
typedef struct hostent *(WINAPI *pfnGethostbyname)(const char *);
static pfnConnect       real_connect;
static pfnRecvfrom      real_recvfrom;
static pfnAccept        real_accept;
static pfnGethostbyname real_gethostbyname;

static u_long g_hostIp;   // network order; game host address (learned from TCP connect to :4658)
static int WINAPI Hook_connect(SOCKET s, const struct sockaddr *to, int tolen)
{
    struct sockaddr_in a;
    if (to && tolen >= (int)sizeof(a) && to->sa_family == AF_INET) {
        memcpy(&a, to, sizeof(a));
        u_long orig = a.sin_addr.s_addr;
        if (!cfg.loopback) {
            if (ntohs(a.sin_port) == 4658) { g_hostIp = a.sin_addr.s_addr; logf("game connect: host address recorded"); }
            return real_connect(s, to, tolen);
        }
        int red = RedirectToOther(&a);
        // make the peer see this copy's own address (otherwise both look like 127.0.0.1)
        struct sockaddr_in me; int ml = sizeof me;
        if (getsockname(s, (struct sockaddr *)&me, &ml) != 0 || me.sin_addr.s_addr == 0) {
            memset(&me, 0, sizeof me);
            me.sin_family = AF_INET; me.sin_addr.s_addr = LoopAddr(g_instance); me.sin_port = 0;
            int br = real_bind(s, (struct sockaddr *)&me, sizeof me);
            logf("connect: pre-bound socket to 127.0.0.%d = %d", g_instance, br);
        }
        if (ntohs(a.sin_port) == 4658) g_hostIp = a.sin_addr.s_addr;
        int r = real_connect(s, (struct sockaddr *)&a, sizeof(a));
        int e = WSAGetLastError();
        char b1[32], b2[32];
        logf("connect %s%s%s = %d (err %d)", IpStr(orig, a.sin_port, b1), red ? " -> " : "",
             red ? IpStr(a.sin_addr.s_addr, a.sin_port, b2) : "", r, r ? e : 0);
        WSASetLastError(e);
        return r;
    }
    return real_connect(s, to, tolen);
}

static int WINAPI Hook_recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fl)
{
    int r = real_recvfrom(s, buf, len, flags, from, fl);
    if (r > 0 && from && fl && *fl >= (int)sizeof(struct sockaddr_in) && from->sa_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)from;
        if (FirstTime('r', a->sin_addr.s_addr, a->sin_port)) {
            char b[32]; logf("recvfrom %s (%d bytes)", IpStr(a->sin_addr.s_addr, a->sin_port, b), r);
        }
    }
    return r;
}

static SOCKET WINAPI Hook_accept(SOCKET s, struct sockaddr *from, int *fl)
{
    SOCKET c = real_accept(s, from, fl);
    if (c != INVALID_SOCKET && from && fl && from->sa_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)from;
        char b[32]; logf("accept from %s", IpStr(a->sin_addr.s_addr, a->sin_port, b));
    }
    return c;
}

// the game advertises its own IP from gethostbyname(gethostname()) -> report 127.0.0.N
static struct hostent *WINAPI Hook_gethostbyname(const char *name)
{
    struct hostent *h = real_gethostbyname(name);
    if (h && h->h_addrtype == AF_INET && h->h_addr_list && h->h_addr_list[0]) {
        u_long orig; memcpy(&orig, h->h_addr_list[0], 4);
        u_long mine = LoopAddr(g_instance);
        memcpy(h->h_addr_list[0], &mine, 4);
        h->h_addr_list[1] = NULL;
        char b[32]; logf("gethostbyname('%s') %s -> 127.0.0.%d", name ? name : "", IpStr(orig, 0, b), g_instance);
    }
    return h;
}

// ===================================================== client -> host order relay ===
// Most squad orders (follow/hold/fire) already have network code in the game, but
// ADVANCE ("go to" / context: shield, interact ...) and ALL_PRONE ("hit the dirt") only
// change the client's local mirror of the AI, which the host immediately overwrites.
// The client therefore sends these calls over a small UDP side channel and the host
// replays them on its own entities (temporarily loading the client's order cursor
// into the host's player controller for the controller-based calls).
#define RELAY_PORT   4659
#define RELAY_MAGIC  0x53544743u      // 'CGTS'
#define CTL_LO       0x440
#define CTL_HI       0x680
#define CTL_N        ((CTL_HI - CTL_LO) / 4)
#define IMG_LO       0x00400000u
#define IMG_HI       0x01200000u

enum { OP_ADV_ALL = 1, OP_ADV_ONE, OP_PRONE_ALL, OP_PRONE_ONE, OP_CTX_ALL, OP_CTX_ONE, OP_SWITCH, OP_SWITCH_OK, OP_SWITCH_NO,
       OP_HELLO, OP_HUMANS, OP_LAST = OP_HUMANS };
static const char *OpName(int op)
{
    static const char *n[] = { "?", "advance-all", "advance-one", "prone-all", "prone-one", "context-all", "context-one",
                               "switch-request", "switch-ok", "switch-refused", "hello", "humans" };
    return (op > 0 && op <= OP_LAST) ? n[op] : "?";
}
enum { K_RAW = 0, K_ENT = 1, K_KEEP = 2 };

#pragma pack(push, 1)
typedef struct {
    uint32_t magic, seq, op, ownId, memberId, arg;
    uint8_t  kind[CTL_N];
    uint32_t val[CTL_N];
} RelayPkt;
#pragma pack(pop)

typedef int (__thiscall *pfnThis0)(void *self);
typedef int (__thiscall *pfnThis1)(void *self, void *a);
typedef int (__thiscall *pfnThis1i)(void *self, int a);
typedef int (__thiscall *pfnThis2)(void *self, void *a, int b);
#define F_ADV_ALL    ((pfnThis0) 0x00424580)
#define F_ADV_ONE    ((pfnThis1) 0x00421bb0)
#define F_PRONE_ALL  ((pfnThis0) 0x0041b910)
#define F_PRONE_ONE  ((pfnThis1) 0x00473ef0)
#define F_CTX_ALL    ((pfnThis1i)0x004a3dc0)
#define F_CTX_ONE    ((pfnThis2) 0x0049fc50)

static SOCKET   g_relaySock = INVALID_SOCKET;
static void    *g_hostCtl;              // host: local player controller (from the order handler)
static uint8_t *g_squad;                // local squad panel (handler+0x88)
static void    *g_localCtl;             // local player controller (any side)
static volatile LONG g_inRelay;         // host: replaying (don't re-forward)

static void *EntMgr(void) { return *(void **)A_PENTMGR; }
static void *EntById(uint32_t id)
{
    void *m = EntMgr();
    if (!m || id == 0xFFFFFFFFu) return NULL;
    void **tbl = *(void ***)((uint8_t *)m + 0x2C);
    if (!tbl || id > U32(m, 0x30)) return NULL;
    return tbl[id];
}
static int IsEntity(uint32_t v, uint32_t *id)
{
    if (v < 0x10000 || v >= 0x80000000u || (v >= IMG_LO && v < IMG_HI)) return 0;
    if (IsBadReadPtr((void *)(uintptr_t)v, 12)) return 0;
    uint32_t i = U32((uintptr_t)v, 8);
    if (i > 0x100000 || EntById(i) != (void *)(uintptr_t)v) return 0;
    *id = i; return 1;
}
static uint32_t EntId(void *e) { uint32_t id; return (e && IsEntity((uint32_t)(uintptr_t)e, &id)) ? id : 0xFFFFFFFFu; }

static void *Session(void)
{
    void *nm = *(void **)A_PNETMGR;
    return nm ? *(void **)((uint8_t *)nm + 0xC0) : NULL;
}
static int IsNetGame(void)   { return ((pfnInt0)0x00471ec0)() != 0; }
static int IsHost(void)      { void *s = Session(); return s && (U32(s, 0x500C) & 2); }
static int IsNetClient(void) { return Session() && IsNetGame() && !IsHost(); }

static void RelayOpen(void)
{
    if (g_relaySock != INVALID_SOCKET) return;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) { logf("relay: socket failed %d", WSAGetLastError()); return; }
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(RELAY_PORT);
    a.sin_addr.s_addr = Tun_Active() ? Tun_SelfAddr() : cfg.loopback ? LoopAddr(g_instance) : INADDR_ANY;
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0)
        logf("relay: bind :%d failed %d (sending still works)", RELAY_PORT, WSAGetLastError());
    g_relaySock = s;
    logf("relay socket ready on port %d", RELAY_PORT);
}

// ---- client side
static void RelaySend(int op, void *ctl, void *own, void *member, uint32_t arg)
{
    if (g_inRelay || !IsNetClient()) return;
    RelayOpen();
    if (!g_hostIp && cfg.loopback && !Tun_Active()) g_hostIp = LoopAddr(g_instance == 1 ? 2 : 1);
    if (g_relaySock == INVALID_SOCKET || !g_hostIp) { logf("relay: no host address yet, %s not sent", OpName(op)); return; }
    static uint32_t seq;
    RelayPkt p; memset(&p, 0, sizeof p);
    p.magic = RELAY_MAGIC; p.seq = ++seq; p.op = op; p.arg = arg;
    p.ownId = EntId(own); p.memberId = EntId(member);
    int nEnt = 0, nKeep = 0;
    if (ctl) {
        for (int i = 0; i < CTL_N; i++) {
            uint32_t v = U32(ctl, CTL_LO + i * 4), id;
            if (IsEntity(v, &id)) { p.kind[i] = K_ENT; p.val[i] = id; nEnt++; }
            else if (v >= 0x10000 && v < 0x30000000u && !(v >= IMG_LO && v < IMG_HI) &&
                     !IsBadReadPtr((void *)(uintptr_t)v, 4)) { p.kind[i] = K_KEEP; nKeep++; }
            else { p.kind[i] = K_RAW; p.val[i] = v; }
        }
    }
    struct sockaddr_in to; memset(&to, 0, sizeof to);
    to.sin_family = AF_INET; to.sin_port = htons(RELAY_PORT); to.sin_addr.s_addr = g_hostIp;
    int r = RSendto(g_relaySock, (const char *)&p, sizeof p, 0, (struct sockaddr *)&to, sizeof to);
    char b[32];
    logf("relay -> %s #%u %s own=%d member=%d arg=%u (ctl: %d entity, %d local ptr) = %d",
         IpStr(g_hostIp, to.sin_port, b), p.seq, OpName(op), (int)p.ownId, (int)p.memberId, arg, nEnt, nKeep, r);
    if (ctl) {
        logf("   ctl 4b0..4ec: %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X",
             U32(ctl,0x4b0),U32(ctl,0x4b4),U32(ctl,0x4b8),U32(ctl,0x4bc),U32(ctl,0x4c0),U32(ctl,0x4c4),U32(ctl,0x4c8),
             U32(ctl,0x4cc),U32(ctl,0x4d0),U32(ctl,0x4d4),U32(ctl,0x4d8),U32(ctl,0x4dc),U32(ctl,0x4e0),U32(ctl,0x4e4),
             U32(ctl,0x4e8),U32(ctl,0x4ec));
    }
}

static int __thiscall W_AdvAll(void *ctl)
{ RelaySend(OP_ADV_ALL, ctl, *(void **)((uint8_t *)ctl + 0x448), NULL, 0); return F_ADV_ALL(ctl); }
static int __thiscall W_AdvOne(void *ctl, void *m)
{ RelaySend(OP_ADV_ONE, ctl, *(void **)((uint8_t *)ctl + 0x448), m, 0); return F_ADV_ONE(ctl, m); }
static int __thiscall W_ProneAll(void *ctl)
{ RelaySend(OP_PRONE_ALL, ctl, *(void **)((uint8_t *)ctl + 0x448), NULL, 0); return F_PRONE_ALL(ctl); }
static int __thiscall W_ProneOne(void *sol, void *m)
{ RelaySend(OP_PRONE_ONE, NULL, sol, m, 0); return F_PRONE_ONE(sol, m); }
static int __thiscall W_CtxAll(void *sol, int a)
{ RelaySend(OP_CTX_ALL, NULL, sol, NULL, (uint32_t)a); return F_CTX_ALL(sol, a); }
static int __thiscall W_CtxOne(void *sol, void *m, int a)
{ RelaySend(OP_CTX_ONE, NULL, sol, m, (uint32_t)a); return F_CTX_ONE(sol, m, a); }

// ---- host side (runs on the game thread, from the order-handler probe)
static void RelayExecute(const RelayPkt *p)
{
    void *own = EntById(p->ownId);
    void *mem = p->memberId != 0xFFFFFFFFu ? EntById(p->memberId) : NULL;
    if (!own || U32(own, 4) != 1) { logf("relay: #%u %s - orderer entity %d not found", p->seq, OpName(p->op), (int)p->ownId); return; }
    if (p->memberId != 0xFFFFFFFFu && !mem) { logf("relay: #%u member %d not found", p->seq, (int)p->memberId); return; }
    int needCtl = p->op == OP_ADV_ALL || p->op == OP_ADV_ONE || p->op == OP_PRONE_ALL;
    void *ctl = g_hostCtl;
    if (needCtl && !ctl) { logf("relay: #%u no host controller yet", p->seq); return; }

    static uint8_t saved[CTL_HI - CTL_LO];
    int missing = 0;
    if (needCtl) {
        memcpy(saved, (uint8_t *)ctl + CTL_LO, sizeof saved);
        for (int i = 0; i < CTL_N; i++) {
            uint32_t *slot = (uint32_t *)((uint8_t *)ctl + CTL_LO + i * 4);
            if (p->kind[i] == K_RAW) *slot = p->val[i];
            else if (p->kind[i] == K_ENT) { void *e = EntById(p->val[i]); if (!e) missing++; *slot = (uint32_t)(uintptr_t)e; }
        }
        *(void **)((uint8_t *)ctl + 0x448) = own;
    }
    // ALL_PRONE skips soldiers flagged +0xF9A bit 4 (player-controlled). Remote human
    // players' soldiers lack that flag on the host, so protect every human player
    // except the one who gave the order while replaying.
    // keep every human except the one who gave the order out of the replayed order
    void *shielded[4] = { 0 }; int nShield = 0;
    if (Session()) {
        void *hostSol = g_hostCtl ? *(void **)((uint8_t *)g_hostCtl + 0x448) : NULL;
        int cnt = g_squad ? (int8_t)g_squad[0x81] : 0;
        for (int i = 0; i < cnt && i < 4; i++) {
            void *e = *(void **)(g_squad + 0xDC + i * 4);
            if (!e || e == own || (U8(e, 0xF9A) & 4)) continue;
            if (U8(e, 0x4A) == 0 && e != hostSol) continue;      // AI squadmate
            *((uint8_t *)e + 0xF9A) |= 4;
            shielded[nShield++] = e;
        }
    }
    InterlockedExchange(&g_inRelay, 1);
    switch (p->op) {
    case OP_ADV_ALL:   F_ADV_ALL(ctl); break;
    case OP_ADV_ONE:   F_ADV_ONE(ctl, mem); break;
    case OP_PRONE_ALL: F_PRONE_ALL(ctl); break;
    case OP_PRONE_ONE: F_PRONE_ONE(own, mem); break;
    case OP_CTX_ALL:   F_CTX_ALL(own, (int)p->arg); break;
    case OP_CTX_ONE:   F_CTX_ONE(own, mem, (int)p->arg); break;
    }
    InterlockedExchange(&g_inRelay, 0);
    for (int i = 0; i < nShield; i++) *((uint8_t *)shielded[i] + 0xF9A) &= ~4;
    if (nShield && p->op == OP_PRONE_ALL) logf("relay: kept %d human player(s) out of the squad-wide prone order", nShield);
    if (needCtl) memcpy((uint8_t *)ctl + CTL_LO, saved, sizeof saved);
    logf("relay: executed #%u %s for entity %d%s (unresolved entity refs: %d)", p->seq, OpName(p->op),
         (int)p->ownId, mem ? " on member" : "", missing);
}


// ---- human players are not squad members ------------------------------------
// The squad panel lists every squad soldier, so orders (and their highlight)
// also hit the other human player. A soldier is "another human" when:
//   host:   owner byte +0x4A != 0 (a client drives it)
//   client: its id is in the list the host sends (host soldier + client soldiers)
// The local player's own soldier is never counted.
static uint32_t g_humanIds[4]; static int g_nHumanIds; static DWORD g_humanIdsT;
static struct sockaddr_in g_clients[4]; static int g_nClients;

static void *LocalSoldier(void) { return g_localCtl ? *(void **)((uint8_t *)g_localCtl + 0x448) : NULL; }
static int IsOtherHuman(void *e)
{
    if (!e || e == LocalSoldier() || !Session() || !IsNetGame()) return 0;
    if (IsHost()) return U8(e, 0x4A) != 0;
    if (GetTickCount() - g_humanIdsT > 5000) return 0;          // stale list
    uint32_t id = U32(e, 8);
    for (int i = 0; i < g_nHumanIds; i++) if (g_humanIds[i] == id) return 1;
    return 0;
}
// mark other humans as "not taking squad orders" (+0xF9A bit 4, the flag the
// squad-wide order loops skip) for the duration of a call
typedef struct { void *e[4]; int n; } Shield;
static void ShieldOn(Shield *sh, void *except)
{
    sh->n = 0;
    uint8_t *sq = g_squad;
    int cnt = sq ? (int8_t)sq[0x81] : 0;
    for (int i = 0; i < cnt && i < 4; i++) {
        void *e = *(void **)(sq + 0xDC + i * 4);
        if (!e || e == except || (U8(e, 0xF9A) & 4) || !IsOtherHuman(e)) continue;
        *((uint8_t *)e + 0xF9A) |= 4;
        sh->e[sh->n++] = e;
    }
}
static void ShieldOff(Shield *sh)
{
    for (int i = 0; i < sh->n; i++) *((uint8_t *)sh->e[i] + 0xF9A) &= ~4;
    sh->n = 0;
}

// order handler call site (0x50a8fa): run the whole local order logic with other humans shielded
static void HumanDiag(const char *where)
{
    // temporary: log the panel whenever its human/selection picture changes
    static uint32_t last; uint8_t *sq = g_squad;
    if (!sq) return;
    int cnt = (int8_t)sq[0x81];
    void *ls = LocalSoldier();
    uint32_t h = U32(sq, 0x118) * 7 + (uint8_t)sq[0x7E] * 131 + (uint32_t)(uintptr_t)ls;
    for (int i = 0; i < cnt && i < 4; i++) {
        void *e = *(void **)(sq + 0xDC + i * 4);
        h = h * 31 + (uint32_t)(uintptr_t)e + IsOtherHuman(e) * 17 + U8(e, 0x4A);
    }
    if (h == last) return;
    last = h;
    char b[200]; int n = 0;
    for (int i = 0; i < cnt && i < 4; i++) {
        void *e = *(void **)(sq + 0xDC + i * 4);
        n += wsprintfA(b + n, " [%d id=%u own=%u %s%s]", i, e ? U32(e, 8) : 0, e ? U8(e, 0x4A) : 0,
                       e == ls ? "ME" : IsOtherHuman(e) ? "HUMAN" : "ai", e && (U8(e, 0xF9A) & 4) ? " f4" : "");
    }
    logf("HUMDBG %s: f118=%X sel=%d humansList=%d (age %lums)%s", where, U32(sq, 0x118), (int8_t)sq[0x7E],
         g_nHumanIds, (unsigned long)(GetTickCount() - g_humanIdsT), b);
}
static int __thiscall W_OrderHandler(void *self)
{
    // never let the order selection rest on another human (covers F1-F4 direct picks too)
    uint8_t *sq = *(uint8_t **)((uint8_t *)self + 0x88);
    static int8_t lastGood = -1;
    if (sq) {
        int cnt = (int8_t)sq[0x81], sel = (int8_t)sq[0x7E];
        if (sel >= 0 && sel < cnt && sel < 4 && IsOtherHuman(*(void **)(sq + 0xDC + sel * 4))) {
            int to = -1;
            if (lastGood >= 0 && lastGood < cnt && !IsOtherHuman(*(void **)(sq + 0xDC + lastGood * 4))) to = lastGood;
            for (int k = 1; to < 0 && k < cnt; k++) {
                int i = (sel + k) % cnt;
                if (!IsOtherHuman(*(void **)(sq + 0xDC + i * 4))) to = i;
            }
            logf("HUMDBG selection was on another human (member %d) -> moved to %d", sel, to);
            if (to >= 0) sq[0x7E] = (uint8_t)to;
        } else if (sel >= 0) lastGood = (int8_t)sel;
    }
    if (cfg.debug) HumanDiag("order");
    Shield sh; ShieldOn(&sh, NULL);
    int r = ((int (__thiscall *)(void *))0x004fe770)(self);
    ShieldOff(&sh);
    return r;
}
// squad panel vtable[31]: member selectable for an order / switch
static int (__thiscall *real_PanelSelectable)(void *panel, int idx);
static int __thiscall W_PanelSelectable(void *panel, int idx)
{
    if (idx >= 0 && idx < 4 && IsOtherHuman(*(void **)((uint8_t *)panel + 0xDC + idx * 4))) return 0;
    return real_PanelSelectable(panel, idx);
}
// squad panel vtable[30]: draw one member; drop the "whole squad" highlight for other humans
static int (__thiscall *real_PanelDrawMember)(void *panel, int idx);
static int __thiscall W_PanelDrawMember(void *panel, int idx)
{
    uint8_t *p = (uint8_t *)panel;
    if (idx < 0 || idx >= 4 || !IsOtherHuman(*(void **)(p + 0xDC + idx * 4)))
        return real_PanelDrawMember(panel, idx);
    uint32_t f = U32(p, 0x118);
    int8_t sel = (int8_t)p[0x7E];
    U32(p, 0x118) = f & ~1u;                 // not "all squad" for this portrait
    if (sel == idx) p[0x7E] = 0xFF;          // and never the selected one
    int r = real_PanelDrawMember(panel, idx);
    U32(p, 0x118) = (U32(p, 0x118) & ~1u) | (f & 1u);
    if (sel == idx) p[0x7E] = (uint8_t)sel;
    return r;
}

// ---- character switching across machines -----------------------------------
// The client asks the host (OP_SWITCH, ownId = its current soldier, arg = direction).
// The host picks the next free AI squadmate, hands it to the client (owner byte
// +0x4A = client slot, mirrored on the host) and takes the client's old body
// back as a host-simulated AI; then it answers OP_SWITCH_OK and the client
// performs the same ownership swap locally and switches its controller.
static void RelaySendTo(int op, uint32_t ownId, uint32_t memberId, uint32_t arg, u_long ip, u_short portN)
{
    RelayOpen();
    if (g_relaySock == INVALID_SOCKET || !ip) { logf("relay: cannot send %s (no address)", OpName(op)); return; }
    static uint32_t seq;
    RelayPkt p; memset(&p, 0, sizeof p);
    p.magic = RELAY_MAGIC; p.seq = ++seq; p.op = op; p.ownId = ownId; p.memberId = memberId; p.arg = arg;
    struct sockaddr_in to; memset(&to, 0, sizeof to);
    to.sin_family = AF_INET; to.sin_port = portN; to.sin_addr.s_addr = ip;
    int r = RSendto(g_relaySock, (const char *)&p, sizeof p, 0, (struct sockaddr *)&to, sizeof to);
    char b[32]; logf("relay -> %s #%u %s own=%d member=%d arg=%d = %d", IpStr(ip, portN, b), p.seq, OpName(op),
                     (int)ownId, (int)memberId, (int)arg, r);
}
static void MakeMirrored(void *e)
{
    U8(e, 0xF1C) = 3;
    int netId = I32(e, 0x3AC);
    if (netId != -1) { NetReg1(netId, 1, (void *)A_NETSYNC_CB, 1); NetReg2(netId, 1); }
}
static void MakeLocal(void *e)
{
    // the game's own "simulated here" registration (see 0x4b3155): mode 1, no sync callback
    U8(e, 0xF1C) = 1;
    int netId = I32(e, 0x3AC);
    if (netId != -1) NetReg1(netId, 0, NULL, 1);
}
static void ClientRequestSwitch(void *cur, int dir)
{
    if (!g_hostIp && cfg.loopback && !Tun_Active()) g_hostIp = LoopAddr(g_instance == 1 ? 2 : 1);
    RelaySendTo(OP_SWITCH, EntId(cur), 0xFFFFFFFFu, (uint32_t)dir, g_hostIp, htons(RELAY_PORT));
}
static void HostHandleSwitch(const RelayPkt *p, const struct sockaddr_in *from)
{
    void *old = EntById(p->ownId);
    int dir = (int)p->arg < 0 ? -1 : 1;
    uint8_t *sq = g_squad;
    void *hostSol = g_hostCtl ? *(void **)((uint8_t *)g_hostCtl + 0x448) : NULL;
    int cnt = sq ? (int8_t)sq[0x81] : 0;
    int at = -1;
    for (int i = 0; i < cnt && i < 4; i++) if (*(void **)(sq + 0xDC + i * 4) == old) at = i;
    if (!old || at < 0 || U8(old, 0x4A) == 0) {
        logf("switch: request from client with unknown soldier %d", (int)p->ownId);
        RelaySendTo(OP_SWITCH_NO, p->ownId, 0xFFFFFFFFu, 0, from->sin_addr.s_addr, from->sin_port);
        return;
    }
    for (int step = 1; step < cnt; step++) {
        int i = ((at + dir * step) % cnt + cnt) % cnt;
        void *m = *(void **)(sq + 0xDC + i * 4);
        if (!m || m == old || m == hostSol) continue;
        if (U8(m, 0x4A) != 0) continue;                  // another human
        if (!(U32(m, 0xD7C) & 2) || !Selectable(sq, i)) continue;
        uint8_t clientSlot = U8(old, 0x4A);
        uint8_t localF1C = U8(m, 0xF1C);
        U8(m, 0x4A) = clientSlot;                        // client drives the new body
        MakeMirrored(m);
        U8(old, 0x4A) = 0;                               // host simulates the old body as AI
        MakeLocal(old);
        logf("switch: client (slot %u) takes member %d id %u, its old body id %u becomes AI (f1c %u)",
             clientSlot, i, U32(m, 8), U32(old, 8), localF1C);
        SetSlotBody(clientSlot, U32(m, 8));
        RelaySendTo(OP_SWITCH_OK, p->ownId, U32(m, 8), 0, from->sin_addr.s_addr, from->sin_port);
        return;
    }
    logf("switch: no free squadmate for the client");
    RelaySendTo(OP_SWITCH_NO, p->ownId, 0xFFFFFFFFu, 0, from->sin_addr.s_addr, from->sin_port);
}
static void ClientHandleSwitch(const RelayPkt *p)
{
    if (p->op == OP_SWITCH_NO) { logf("switch: host refused (no free squadmate)"); return; }
    void *old = EntById(p->ownId), *m = EntById(p->memberId);
    void *ctl = g_localCtl;
    if (!old || !m || !ctl) { logf("switch: ok from host but entities/controller missing"); return; }
    uint8_t mySlot = U8(old, 0x4A);
    U8(m, 0x4A) = mySlot;
    MakeLocal(m);
    U8(old, 0x4A) = 0;
    MakeMirrored(old);
    int r = SwitchTo(ctl, m);
    SetSlotBody(mySlot, U32(m, 8));
    if (g_squad) {
        int cnt = (int8_t)g_squad[0x81];
        for (int i = 0; i < cnt && i < 4; i++) if (*(void **)(g_squad + 0xDC + i * 4) == m) g_squad[0x7E] = (uint8_t)i;
    }
    logf("switch: now controlling id %u (was %u) result=%d ctlSoldier=%p", U32(m, 8), U32(old, 8), r,
         *(void **)((uint8_t *)ctl + 0x448));
}

// ---- diagnostics: squad inventories (soldier +0x814 -> 40 item pointers, +0x808 current)
static void ItemName(void *item, char *out, int n)
{
    out[0] = 0;
    void *st = *(void **)((uint8_t *)item + 0x24);
    if (!st || IsBadReadPtr(st, 0x100)) return;
    for (int off = 4; off < 0x100; off += 4) {            // first printable string in the stat block
        const char *c = (const char *)st + off;
        int k = 0;
        while (k < n - 1 && c[k] >= 0x20 && c[k] < 0x7f) k++;
        if (k >= 3 && c[k] == 0) { memcpy(out, c, k); out[k] = 0; return; }
        void *ptr = *(void **)c;                          // or a pointer to one
        if ((uintptr_t)ptr > 0x10000 && !IsBadReadPtr(ptr, 4)) {
            const char *q = (const char *)ptr; k = 0;
            while (k < n - 1 && !IsBadReadPtr(q + k, 1) && q[k] >= 0x20 && q[k] < 0x7f) k++;
            if (k >= 3 && q[k] == 0) { memcpy(out, q, k); out[k] = 0; return; }
        }
    }
}
static void InventoryWatch(void)
{
    static uint32_t last[4]; static DWORD nextT;
    DWORD now = GetTickCount();
    if (now < nextT) return;
    nextT = now + 2000;
    if (!Session()) return;
    for (int slot = 0; slot < 4; slot++) {
        int id = SlotToId(slot);
        void *e = id >= 0 ? EntById((uint32_t)id) : NULL;
        if (!e || U32(e, 4) != 1) continue;
        void **inv = *(void ***)((uint8_t *)e + 0x814);
        if (!inv || IsBadReadPtr(inv, 40 * 4)) continue;
        uint32_t h = 0; int cnt = 0;
        for (int i = 0; i < 40; i++) { h = h * 31 + (uint32_t)(uintptr_t)inv[i]; if (inv[i]) cnt++; }
        if (h == last[slot]) continue;
        last[slot] = h;
        logf("INV slot%d id=%d %s owner=%u: %d items, current=%p", slot, id,
             SlotOccupied(Session(), slot) ? "PLAYER" : "AI", U8(e, 0x4A), cnt, *(void **)((uint8_t *)e + 0x808));
        for (int i = 0; i < 40; i++) if (inv[i]) {
            void *it = inv[i]; void *st = *(void **)((uint8_t *)it + 0x24);
            char nm[48]; ItemName(it, nm, sizeof nm);
            logf("    [%2d] item=%p id=%d stat=%p type=%d sub=%d '%s'", i, it, I32(it, 8), st,
                 st ? I32(st, 0) : -1, st ? I32(st, 0xEC) : -1, nm);
        }
    }
}

static void RelayPump(void *handlerSelf)
{
    if (cfg.debug) InventoryWatch();
    void *ctl = *(void **)((uint8_t *)handlerSelf + 0x7C);
    if (ctl) g_localCtl = ctl;
    g_squad = *(uint8_t **)((uint8_t *)handlerSelf + 0x88);
    if (IsNetClient()) {
        RelayOpen();
        if (g_relaySock == INVALID_SOCKET) return;
        for (int n = 0; n < 4; n++) {
            RelayPkt p; struct sockaddr_in from; int fl = sizeof from;
            int r = recvfrom(g_relaySock, (char *)&p, sizeof p, 0, (struct sockaddr *)&from, &fl);
            if (r <= 0) break;
            if (r != (int)sizeof p || p.magic != RELAY_MAGIC) continue;
            if (p.op == OP_SWITCH_OK || p.op == OP_SWITCH_NO) ClientHandleSwitch(&p);
            else if (p.op == OP_HUMANS) {
                int n = (int)p.arg; if (n > 4) n = 4;
                for (int i = 0; i < n; i++) {
                    g_humanIds[i] = p.val[i] & 0xFFFFFF;
                    int slot = (int)(p.val[i] >> 24);
                    if (slot != *(uint8_t *)A_LOCAL_SLOT) SetSlotBody(slot, g_humanIds[i]);   // other players' bodies
                }
                g_nHumanIds = n; g_humanIdsT = GetTickCount();
            }
        }
        static DWORD helloT;
        if (GetTickCount() - helloT > 1000) {
            helloT = GetTickCount();
            if (!g_hostIp && cfg.loopback && !Tun_Active()) g_hostIp = LoopAddr(g_instance == 1 ? 2 : 1);
            if (g_hostIp) {   // quiet send (no log spam)
                RelayPkt h; memset(&h, 0, sizeof h); h.magic = RELAY_MAGIC; h.op = OP_HELLO;
                struct sockaddr_in to; memset(&to, 0, sizeof to);
                to.sin_family = AF_INET; to.sin_port = htons(RELAY_PORT); to.sin_addr.s_addr = g_hostIp;
                RSendto(g_relaySock, (const char *)&h, sizeof h, 0, (struct sockaddr *)&to, sizeof to);
            }
        }
        return;
    }
    if (!IsHost()) return;
    if (ctl) g_hostCtl = ctl;
    RelayOpen();
    if (g_relaySock == INVALID_SOCKET) return;
    for (int n = 0; n < 8; n++) {
        RelayPkt p; struct sockaddr_in from; int fl = sizeof from;
        int r = recvfrom(g_relaySock, (char *)&p, sizeof p, 0, (struct sockaddr *)&from, &fl);
        if (r <= 0) break;
        if (r != (int)sizeof p || p.magic != RELAY_MAGIC || p.op < 1 || p.op > OP_LAST) continue;
        {   int k = 0;
            for (; k < g_nClients; k++)
                if (g_clients[k].sin_addr.s_addr == from.sin_addr.s_addr && g_clients[k].sin_port == from.sin_port) break;
            if (k == g_nClients && g_nClients < 4) {
                g_clients[g_nClients++] = from;
                char b[32]; logf("relay: client %s registered", IpStr(from.sin_addr.s_addr, from.sin_port, b));
            }
        }
        if (p.op == OP_HELLO) continue;
        char b[32]; logf("relay <- %s #%u %s", IpStr(from.sin_addr.s_addr, from.sin_port, b), p.seq, OpName(p.op));
        if (p.op == OP_SWITCH) HostHandleSwitch(&p, &from);
        else if (p.op <= OP_CTX_ONE) RelayExecute(&p);
    }
    static DWORD humT;
    if (g_nClients && GetTickCount() - humT > 300) {
        humT = GetTickCount();
        RelayPkt h; memset(&h, 0, sizeof h); h.magic = RELAY_MAGIC; h.op = OP_HUMANS;
        int n = 0;
        void *hs = g_hostCtl ? *(void **)((uint8_t *)g_hostCtl + 0x448) : NULL;
        if (hs) h.val[n++] = U32(hs, 8);            // slot 0 = host
        int cnt = g_squad ? (int8_t)g_squad[0x81] : 0;
        for (int i = 0; i < cnt && i < 4 && n < 4; i++) {
            void *e = *(void **)(g_squad + 0xDC + i * 4);
            if (e && e != hs && U8(e, 0x4A) != 0) h.val[n++] = U32(e, 8) | ((uint32_t)U8(e, 0x4A) << 24);
        }
        h.arg = (uint32_t)n;
        for (int k = 0; k < g_nClients; k++)
            RSendto(g_relaySock, (const char *)&h, sizeof h, 0, (struct sockaddr *)&g_clients[k], sizeof g_clients[k]);
    }
}

static int PatchCall(uintptr_t at, uintptr_t expectTarget, void *to)
{
    uint8_t *p = (uint8_t *)at;
    if (p[0] != 0xE8 || at + 5 + *(int32_t *)(p + 1) != expectTarget) {
        logf("relay: call @%08X unexpected, not hooked", (unsigned)at); return 0;
    }
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(int32_t *)(p + 1) = (int32_t)((uintptr_t)to - (at + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}

static void InstallRelay(void)
{
    int n = 0;
    n += PatchCall(0x004feb67, 0x00424580, (void *)W_AdvAll);
    n += PatchCall(0x004feb72, 0x00421bb0, (void *)W_AdvOne);
    n += PatchCall(0x004febf9, 0x0041b910, (void *)W_ProneAll);
    n += PatchCall(0x004fedc6, 0x0041b910, (void *)W_ProneAll);
    n += PatchCall(0x004fec06, 0x00473ef0, (void *)W_ProneOne);
    n += PatchCall(0x004fed76, 0x004a3dc0, (void *)W_CtxAll);
    n += PatchCall(0x004fed7e, 0x0049fc50, (void *)W_CtxOne);
    logf("order relay: %d/7 call sites hooked", n);
}

// ---------------------------------------------------- dinput8 forward ---
typedef HRESULT (WINAPI *pfnDI8Create)(HINSTANCE, DWORD, const void *, LPVOID *, void *);
static pfnDI8Create real_DI8Create;

// ---- FreeMouse: force DirectInput devices to non-exclusive so the cursor is not locked
typedef HRESULT (WINAPI *pfnCreateDevice)(void *self, const void *guid, void **dev, void *outer);
typedef HRESULT (WINAPI *pfnSetCoopLevel)(void *self, HWND hwnd, DWORD flags);
static pfnCreateDevice real_CreateDevice;
static pfnSetCoopLevel real_SetCoopLevel;

static void PatchVtbl(void **vtbl, int idx, void *repl, void **orig)
{
    if (vtbl[idx] == repl) return;
    DWORD old;
    VirtualProtect(&vtbl[idx], sizeof(void *), PAGE_READWRITE, &old);
    if (!*orig) *orig = vtbl[idx];
    vtbl[idx] = repl;
    VirtualProtect(&vtbl[idx], sizeof(void *), old, &old);
}


// ---- mission start items for AI squadmates ---------------------------------
// Engine table entry 0xdb0 ("is this character a human-played participant").
// Mission scripts delete a character's personal kit (e.g. Mission01 backpacks)
// when it answers 0. We answer 1 for AI-filled characters, but ONLY at call
// sites that immediately delete an object (call [reg+0x45c]), so objective
// logic that also uses 0xdb0 keeps treating the AI as non-players.
typedef int (__cdecl *pfnCharIsPlayer)(int ch);
static pfnCharIsPlayer real_CharIsPlayer;
#define ENGINE_TABLE 0x006f1da0
static int CallerDeletesObject(const uint8_t *ret)
{
    if (IsBadReadPtr(ret, 24)) return 0;
    for (int i = 0; i + 6 <= 24; i++)
        if (ret[i] == 0xFF && (ret[i+1] & 0xF8) == 0x90 &&
            ret[i+2] == 0x5C && ret[i+3] == 0x04 && ret[i+4] == 0 && ret[i+5] == 0)
            return 1;
    return 0;
}
static int CharIsAIFill(int ch)
{
    void *ses = Session();
    if (!ses || !IsNetGame()) return 0;
    for (int slot = 0; slot < cfg.fillCount; slot++)
        if (!SlotOccupied(ses, slot) && SlotToId(slot) == ch) return 1;
    return 0;
}
static int __cdecl Hook_CharIsPlayer(int ch)
{
    int r = real_CharIsPlayer(ch);
    if (r == 0 && CallerDeletesObject((const uint8_t *)__builtin_return_address(0)) && CharIsAIFill(ch)) {
        logf("mission kit: keeping items of AI squadmate character %d", ch);
        return 1;
    }
    return r;
}
static int __thiscall W_OrderHandler(void *self);
static int __thiscall W_PanelSelectable(void *panel, int idx);
static int __thiscall W_PanelDrawMember(void *panel, int idx);
static void InstallHumanFilter(void)
{
    int ok = PatchCall(0x0050a8fa, 0x004fe770, (void *)W_OrderHandler);
    PatchVtbl((void **)0x006cc998, 31, (void *)W_PanelSelectable, (void **)&real_PanelSelectable);
    PatchVtbl((void **)0x006cc998, 30, (void *)W_PanelDrawMember, (void **)&real_PanelDrawMember);
    logf("human filter installed (order call %s, panel select %p, panel draw %p)", ok ? "hooked" : "NOT hooked",
         (void *)real_PanelSelectable, (void *)real_PanelDrawMember);
}
static void InstallKitFix(void)
{
    PatchVtbl((void **)ENGINE_TABLE, 0xdb0 / 4, (void *)Hook_CharIsPlayer, (void **)&real_CharIsPlayer);
    logf("mission kit fix installed (engine[0xdb0] %p)", (void *)real_CharIsPlayer);
}

static HRESULT WINAPI Hook_SetCoopLevel(void *self, HWND hwnd, DWORD flags)
{
    DWORD nf = flags;
    if (nf & 0x1) nf = (nf & ~0x1u) | 0x2;   // DISCL_EXCLUSIVE -> DISCL_NONEXCLUSIVE
    nf &= ~0x10u;                            // drop DISCL_NOWINKEY
    logf("SetCooperativeLevel %08X -> %08X", flags, nf);
    return real_SetCoopLevel(self, hwnd, nf);
}

static HRESULT WINAPI Hook_CreateDevice(void *self, const void *guid, void **dev, void *outer)
{
    HRESULT hr = real_CreateDevice(self, guid, dev, outer);
    if (SUCCEEDED(hr) && dev && *dev)        // IDirectInputDevice8::SetCooperativeLevel = slot 13
        PatchVtbl(*(void ***)*dev, 13, (void *)Hook_SetCoopLevel, (void **)&real_SetCoopLevel);
    return hr;
}

// ---- FreeMouse: user32 cursor locks
typedef BOOL (WINAPI *pfnClipCursor)(const RECT *);
typedef BOOL (WINAPI *pfnSetCursorPos)(int, int);
typedef HWND (WINAPI *pfnSetCapture)(HWND);
static pfnClipCursor   real_ClipCursor;
static pfnSetCursorPos real_SetCursorPos;
static pfnSetCapture   real_SetCapture;

static int GameIsForeground(void)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// The cursor behaves normally while this window is active (so mouse-look works),
// and is released as soon as another window is activated (Alt+Tab / click).
static BOOL WINAPI Hook_ClipCursor(const RECT *r)
{
    if (!GameIsForeground()) return real_ClipCursor(NULL);
    return real_ClipCursor(r);
}

static BOOL WINAPI Hook_SetCursorPos(int x, int y)
{
    if (!GameIsForeground()) return TRUE;    // background instance must not steal the cursor
    return real_SetCursorPos(x, y);
}

static HWND WINAPI Hook_SetCapture(HWND h)
{
    if (!GameIsForeground()) return NULL;
    return real_SetCapture(h);
}

// ---- KeepRunning: the game must not notice it lost focus (otherwise it hides /
// minimises itself and pauses, which kills the host in a 2-instance test)
typedef ATOM (WINAPI *pfnRegisterClassA)(const WNDCLASSA *);
typedef BOOL (WINAPI *pfnShowWindow)(HWND, int);
typedef BOOL (WINAPI *pfnSetWindowPos)(HWND, HWND, int, int, int, int, UINT);
static pfnRegisterClassA real_RegisterClassA;
static pfnShowWindow     real_ShowWindow;
static pfnSetWindowPos   real_SetWindowPos;
static WNDPROC           game_WndProc;
static HWND              g_gameWnd;

#define WM_SQUADMOD_RESTORE (WM_APP + 0x5D)

static LRESULT CALLBACK Hook_WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (!g_gameWnd) g_gameWnd = h;
    switch (m) {
    case WM_SQUADMOD_RESTORE:
        if (IsIconic(h)) { logf("restoring minimised game window"); real_ShowWindow(h, SW_SHOWNOACTIVATE); }
        return 0;
    case WM_ACTIVATEAPP:
        if (!w) { logf("WM_ACTIVATEAPP(0) swallowed"); real_ClipCursor(NULL); ReleaseCapture(); return 0; }
        break;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) { logf("WM_ACTIVATE(inactive) swallowed"); real_ClipCursor(NULL); return 0; }
        break;
    case WM_KILLFOCUS:
        return 0;
    case WM_SIZE:
        if (w == SIZE_MINIMIZED) {
            logf("WM_SIZE(minimized) swallowed, will restore");
            PostMessageA(h, WM_SQUADMOD_RESTORE, 0, 0);
            return 0;
        }
        break;
    case WM_SYSCOMMAND:
        if ((w & 0xFFF0) == SC_MINIMIZE) { logf("SC_MINIMIZE blocked"); return 0; }
        break;
    }
    return CallWindowProcA(game_WndProc, h, m, w, l);
}

static ATOM WINAPI Hook_RegisterClassA(const WNDCLASSA *wc)
{
    if (wc && !game_WndProc && wc->lpfnWndProc) {
        WNDCLASSA c = *wc;
        game_WndProc = c.lpfnWndProc;
        c.lpfnWndProc = Hook_WndProc;
        logf("RegisterClassA '%s': window proc wrapped", HIWORD((uintptr_t)c.lpszClassName) ? c.lpszClassName : "#atom");
        return real_RegisterClassA(&c);
    }
    return real_RegisterClassA(wc);
}

static BOOL WINAPI Hook_ShowWindow(HWND h, int cmd)
{
    if (h == g_gameWnd && (cmd == SW_HIDE || cmd == SW_MINIMIZE || cmd == SW_SHOWMINIMIZED ||
                           cmd == SW_SHOWMINNOACTIVE || cmd == SW_FORCEMINIMIZE)) {
        logf("ShowWindow(%d) on game window blocked", cmd);
        return TRUE;
    }
    return real_ShowWindow(h, cmd);
}

static BOOL WINAPI Hook_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT f)
{
    if (h == g_gameWnd && (f & SWP_HIDEWINDOW)) {
        logf("SetWindowPos(HIDE) on game window blocked");
        f &= ~SWP_HIDEWINDOW;
    }
    return real_SetWindowPos(h, after, x, y, cx, cy, f);
}

// ---- MultiInstance: give the 2nd copy its own kernel object names so the
// "A version of Conflict ... is currently in use" check does not trigger
typedef HANDLE (WINAPI *pfnCreateMutexA)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
typedef HANDLE (WINAPI *pfnCreateEventA)(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCSTR);
typedef HANDLE (WINAPI *pfnOpenEventA)(DWORD, BOOL, LPCSTR);
typedef HWND   (WINAPI *pfnGetTopWindow)(HWND);
static pfnCreateMutexA real_CreateMutexA;
static pfnCreateEventA real_CreateEventA;
static pfnOpenEventA   real_OpenEventA;
static pfnGetTopWindow real_GetTopWindow;

static LPCSTR InstName(LPCSTR name, char *buf, size_t n)
{
    if (!name || g_instance == 1) return name;
    snprintf(buf, n, "%s_SquadModCopy%d", name, g_instance);
    return buf;
}

static HANDLE WINAPI Hook_CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCSTR name)
{
    char b[300]; LPCSTR nn = InstName(name, b, sizeof b);
    HANDLE h = real_CreateMutexA(sa, own, nn);
    DWORD e = GetLastError();
    logf("CreateMutexA('%s') -> %p%s", nn ? nn : "(null)", h, e == ERROR_ALREADY_EXISTS ? " ALREADY_EXISTS" : "");
    SetLastError(e);
    return h;
}

static HANDLE WINAPI Hook_CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL man, BOOL init, LPCSTR name)
{
    char b[300]; LPCSTR nn = InstName(name, b, sizeof b);
    HANDLE h = real_CreateEventA(sa, man, init, nn);
    DWORD e = GetLastError();
    if (name) logf("CreateEventA('%s') -> %p%s", nn, h, e == ERROR_ALREADY_EXISTS ? " ALREADY_EXISTS" : "");
    SetLastError(e);
    return h;
}

static HANDLE WINAPI Hook_OpenEventA(DWORD acc, BOOL inh, LPCSTR name)
{
    char b[300]; LPCSTR nn = InstName(name, b, sizeof b);
    HANDLE h = real_OpenEventA(acc, inh, nn);
    DWORD e = GetLastError();
    logf("OpenEventA('%s') -> %p", nn ? nn : "(null)", h);
    SetLastError(e);
    return h;
}

// window scan: hide the other copy's game window from GetTopWindow walks
static HWND WINAPI Hook_GetTopWindow(HWND parent)
{
    static int logged;
    if (!logged++) logf("GetTopWindow called (window scan)");
    HWND h = real_GetTopWindow(parent);
    char cls[32];
    while (h && GetClassNameA(h, cls, sizeof cls) && !lstrcmpA(cls, "GLOBAL_STORM")) {
        DWORD pid; GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) break;
        h = GetWindow(h, GW_HWNDNEXT);       // skip other copy
    }
    return h;
}

// ---- Windowed: make the game ask Direct3D for a real window instead of
// exclusive fullscreen (dgVoodoo otherwise minimises it on focus loss)
typedef void *(WINAPI *pfnD3DCreate9)(UINT);
typedef HRESULT (WINAPI *pfnD3DCreateDevice)(void *self, UINT ad, int type, HWND wnd, DWORD flags, void *pp, void **dev);
typedef HRESULT (WINAPI *pfnD3DReset)(void *self, void *pp);
static pfnD3DCreate9      real_D3DCreate9;
static pfnD3DCreateDevice real_D3DCreateDevice;
static pfnD3DReset        real_D3DReset;

// Display mode follows the game's own VIDEO screen (RESOLUTION + WINDOWED):
// D3D always runs windowed underneath (alt-tab safe, works with dgVoodoo);
//   WINDOWED on  -> normal window sized to the chosen resolution
//   WINDOWED off -> borderless window covering the monitor (dgVoodoo scales)
// The two-copies test (LoopbackTest) always uses normal side-by-side windows.
static HWND g_gameWnd;

// Display settings chosen in the VIDEO screen, kept in squadmod.ini [Display]
// (the game itself always starts at 800x600 fullscreen).
static char     g_ini[MAX_PATH];
static int      g_dispWindowed;          // WINDOWED toggle: 1 = window, 0 = borderless fullscreen
static uint32_t g_saveW, g_saveH;        // saved resolution (0 = none yet)
static uint32_t g_curW, g_curH;          // current back buffer size
static int      g_resetSeen;

static void LoadDisplay(void)
{
    g_dispWindowed = GetPrivateProfileIntA("Display", "Windowed", 0, g_ini) != 0;
    g_saveW = (uint32_t)GetPrivateProfileIntA("Display", "Width", 0, g_ini);
    g_saveH = (uint32_t)GetPrivateProfileIntA("Display", "Height", 0, g_ini);
}
static void SaveDisplay(void)
{
    char v[16];
    snprintf(v, sizeof v, "%d", g_dispWindowed); WritePrivateProfileStringA("Display", "Windowed", v, g_ini);
    if (g_curW && g_curH) {
        snprintf(v, sizeof v, "%u", g_curW); WritePrivateProfileStringA("Display", "Width", v, g_ini);
        snprintf(v, sizeof v, "%u", g_curH); WritePrivateProfileStringA("Display", "Height", v, g_ini);
        g_saveW = g_curW; g_saveH = g_curH;
    }
    logf("display settings saved: %ux%u %s", g_curW, g_curH, g_dispWindowed ? "windowed" : "fullscreen");
}

static void ApplyWindow(HWND wnd, uint32_t w, uint32_t h, int windowed)
{
    if (!wnd || !w || !h) return;
    if (!windowed && !cfg.loopback) {
        HMONITOR mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = { sizeof mi };
        GetMonitorInfoA(mon, &mi);
        SetWindowLongA(wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS);
        SetWindowLongA(wnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
        real_SetWindowPos(wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                          mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                          SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        logf("display: %ux%u borderless fullscreen", w, h);
    } else {
        LONG style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE | WS_CLIPSIBLINGS;
        SetWindowLongA(wnd, GWL_STYLE, style);
        SetWindowLongA(wnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
        RECT r = { 0, 0, (LONG)w, (LONG)h };
        AdjustWindowRect(&r, style, FALSE);
        int ww = r.right - r.left, wh = r.bottom - r.top;
        int sw = GetSystemMetrics(SM_CXSCREEN), shh = GetSystemMetrics(SM_CYSCREEN);
        int x, y;
        if (cfg.loopback) { x = g_instance == 1 ? 0 : (sw - ww > 0 ? sw - ww : 40); y = 0; }
        else { x = (sw - ww) / 2; y = (shh - wh) / 2; if (x < 0) x = 0; if (y < 0) y = 0; }
        real_SetWindowPos(wnd, HWND_NOTOPMOST, x, y, ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        logf("display: %ux%u window", w, h);
    }
    if (cfg.loopback) {
        char title[64];
        snprintf(title, sizeof title, "Conflict: Global Terror [copy %d]", g_instance);
        SetWindowTextA(wnd, title);
    }
}

// Rewrites the present parameters to windowed and returns the game's requested mode.
static int TakeDisplayPP(void *pp, const char *who)
{
    uint32_t w = U32(pp, 0x00), h = U32(pp, 0x04);
    int windowed = U32(pp, 0x20) != 0;
    logf("%s: game asks %ux%u %s", who, w, h, windowed ? "windowed" : "fullscreen");
    U32(pp, 0x20) = 1;   // Windowed
    U32(pp, 0x30) = 0;   // FullScreen_RefreshRateInHz must be 0 when windowed
    return windowed;
}

typedef HRESULT (WINAPI *pfnD3DPresent)(void *self, const void *src, const void *dst, HWND wnd, const void *rgn);
static pfnD3DPresent real_D3DPresent;
static void FixVideoScreen(void);
static void RestoreDisplay(void);
static void MenuFrame(void);
static HRESULT WINAPI Hook_D3DPresent(void *self, const void *src, const void *dst, HWND wnd, const void *rgn)
{
    FixVideoScreen();
    MenuFrame();
    HRESULT hr = real_D3DPresent(self, src, dst, wnd, rgn);
    RestoreDisplay();   // once, between frames: re-apply the saved resolution
    return hr;
}

static HRESULT WINAPI Hook_D3DReset(void *self, void *pp)
{
    if (!pp) return real_D3DReset(self, pp);
    TakeDisplayPP(pp, "Reset");
    int windowed = g_dispWindowed;
    HWND wnd = U32(pp, 0x1C) ? (HWND)(uintptr_t)U32(pp, 0x1C) : g_gameWnd;
    ApplyWindow(wnd, U32(pp, 0), U32(pp, 4), windowed);
    HRESULT hr = real_D3DReset(self, pp);
    logf("Reset hr=%08X", (unsigned)hr);
    if (SUCCEEDED(hr)) { g_curW = U32(pp, 0); g_curH = U32(pp, 4); g_resetSeen = 1; }
    return hr;
}

static HRESULT WINAPI Hook_D3DCreateDevice(void *self, UINT ad, int type, HWND wnd, DWORD flags, void *pp, void **dev)
{
    if (pp) { TakeDisplayPP(pp, "CreateDevice"); g_curW = U32(pp, 0); g_curH = U32(pp, 4); }
    int windowed = g_dispWindowed;
    HWND target = (pp && U32(pp, 0x1C)) ? (HWND)(uintptr_t)U32(pp, 0x1C) : wnd;
    g_gameWnd = target;
    if (pp) ApplyWindow(target, U32(pp, 0), U32(pp, 4), windowed);
    HRESULT hr = real_D3DCreateDevice(self, ad, type, wnd, flags, pp, dev);
    logf("CreateDevice hr=%08X", (unsigned)hr);
    if (SUCCEEDED(hr) && dev && *dev)        // IDirect3DDevice9::Reset = slot 16
    {
        PatchVtbl(*(void ***)*dev, 16, (void *)Hook_D3DReset, (void **)&real_D3DReset);
        PatchVtbl(*(void ***)*dev, 17, (void *)Hook_D3DPresent, (void **)&real_D3DPresent);   // Present
    }
    return hr;
}

typedef UINT (WINAPI *pfnModeCount)(void *self, UINT ad, DWORD fmt);
typedef HRESULT (WINAPI *pfnEnumModes)(void *self, UINT ad, DWORD fmt, UINT i, uint32_t *mode);
static pfnModeCount real_ModeCount;
static pfnEnumModes real_EnumModes;
static UINT WINAPI Hook_ModeCount(void *self, UINT ad, DWORD fmt)
{
    UINT n = real_ModeCount(self, ad, fmt);
    logf("modes: adapter %u format %u -> %u modes", ad, (unsigned)fmt, n);
    return n;
}
static HRESULT WINAPI Hook_EnumModes(void *self, UINT ad, DWORD fmt, UINT i, uint32_t *mode)
{
    HRESULT hr = real_EnumModes(self, ad, fmt, i, mode);
    if (SUCCEEDED(hr) && mode) logf("modes:   #%u %ux%u @%u fmt %u", i, mode[0], mode[1], mode[2], mode[3]);
    return hr;
}

static void *WINAPI Hook_D3DCreate9(UINT sdk)
{
    void *d3d = real_D3DCreate9(sdk);
    if (d3d)                                  // IDirect3D9::CreateDevice = slot 16
        PatchVtbl(*(void ***)d3d, 16, (void *)Hook_D3DCreateDevice, (void **)&real_D3DCreateDevice);
    if (d3d && cfg.debug) {                   // GetAdapterModeCount = 6, EnumAdapterModes = 7
        PatchVtbl(*(void ***)d3d, 6, (void *)Hook_ModeCount, (void **)&real_ModeCount);
        PatchVtbl(*(void ***)d3d, 7, (void *)Hook_EnumModes, (void **)&real_EnumModes);
    }
    return d3d;
}

HRESULT WINAPI DirectInput8Create(HINSTANCE h, DWORD v, const void * r, LPVOID *o, void * u)
{
    if (!real_DI8Create) return E_FAIL;
    HRESULT hr = real_DI8Create(h, v, r, o, u);
    if (SUCCEEDED(hr) && cfg.freeMouse && o && *o)   // IDirectInput8::CreateDevice = slot 3
        PatchVtbl(*(void ***)*o, 3, (void *)Hook_CreateDevice, (void **)&real_CreateDevice);
    return hr;
}

static void LoadRealDinput8(void)
{
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    lstrcatA(path, "\\dinput8.dll");
    HMODULE m = LoadLibraryA(path);
    if (m) real_DI8Create = (pfnDI8Create)GetProcAddress(m, "DirectInput8Create");
}

// ----------------------------------------------------------- startup ---
// 0x58f300 (session thiscall): copy the local player name (session+0x5018, set from the
// save profile by 0x58f2e0) into this machine's slot record and mark the session
// dirty so it is sent to the other players. Reimplemented to use the Steam name.
static int g_useSteamName = 1;   // squadmod.ini SteamName (STEAM screen)
static void __fastcall Hook_ApplyLocalName(uint8_t *session)
{
    uint8_t slot = *(uint8_t *)A_LOCAL_SLOT;
    if (slot > 0xFD) return;
    const char *steam = g_useSteamName ? Tun_PlayerName() : NULL;
    if (steam && strcmp((char *)session + 0x5018, steam)) {
        logf("player name: '%s' -> Steam name '%s'", (char *)session + 0x5018, steam);
        strcpy((char *)session + 0x5018, steam);
    }
    U32(session, 0x230) = 1;
    strcpy((char *)session + slot * 0x8C, (char *)session + 0x5018);
    U32(session, 0x230) = 0;
    U32(session, 0x234) = 1;
}

// ---- game VIDEO screen (SCR_VIDEO) -------------------------------------------
// 0x417cf0 (cdecl, w, h) decides which display modes the VIDEO screen lists. The
// list holds at most 8 entries (tables at 0x739600 / 0x739620), so allow a short
// set of common sizes plus the desktop size.
static int ModeAllowed(int w, int h)
{
    static const int std[][2] = { {800,600}, {1024,768}, {1280,720}, {1280,1024},
                                  {1366,768}, {1600,900}, {1920,1080} };
    int dw = GetSystemMetrics(SM_CXSCREEN), dh = GetSystemMetrics(SM_CYSCREEN);
    if (w > dw || h > dh) return 0;
    if (w == dw && h == dh) return 1;
    for (unsigned i = 0; i < sizeof std / sizeof std[0]; i++)
        if (std[i][0] == w && std[i][1] == h) return 1;
    return 0;
}
static int __cdecl Hook_ModeFilter(int w, int h) { return ModeAllowed(w, h); }

// SCR_VIDEO event 0x1e (screen opened) ends with call 0x51f090 at 0x520e73. The PC
// build hides TOG_WINDOWED there (widget flag 0x8000 = visible); show it again.
typedef void  (__fastcall *pfnScreenFn)(void *self, void *edx);
typedef void *(__fastcall *pfnFindWidget)(void *container, void *edx, uint32_t id);
#define FindWidget ((pfnFindWidget)0x005a0160)
// Rows of SCR_VIDEO in display order (TOG_DEVICE and BN_RESET_WINDOW stay hidden).
static const uint32_t kVideoRows[] = {
    0x256da361,   // TOG_RESOLUTION
    0x96109e70,   // TOG_COLOUR_DEPTH
    0xf03fcd4a,   // TOG_WINDOWED   (hidden by the PC build, half-overlapping slot)
    0xacf936f4,   // BN_DEFAULT
    0x991ebb3f,   // BN_SCAN_HARDWARE
    0xcd8e30b8,   // BN_ADVANCED_SETTINGS
    0x3a21c6fd,   // BN_APPLY
};
#define VIDEO_ROWS (sizeof kVideoRows / sizeof kVideoRows[0])
static uint8_t *g_videoRow[VIDEO_ROWS];
static uint8_t *g_togWindowed;   // = g_videoRow[2]; widgets are built once at startup
static uint8_t *g_videoScreen;

// Toggle widget: value byte +0x74, min +0x75, max +0x76 (0 = NO, 1 = YES).
static void SetWindowedToggle(void)
{
    if (!g_togWindowed) return;
    U8(g_togWindowed, 0x75) = 0; U8(g_togWindowed, 0x76) = 1;
    U8(g_togWindowed, 0x74) = (uint8_t)(g_dispWindowed != 0);
}

static void __fastcall Hook_VideoScreenOpened(uint8_t *screen, void *edx)
{
    ((pfnScreenFn)0x0051f090)(screen, edx);
    uint8_t *group = FindWidget(screen + 0x24, 0, 0x4b5da259);
    if (!group) return;              // also called for screens without the video rows
    uint8_t *rows[VIDEO_ROWS];
    for (unsigned i = 0; i < VIDEO_ROWS; i++)
        if (!(rows[i] = FindWidget(group + 0x40, 0, kVideoRows[i]))) return;
    if (!g_togWindowed) {            // widgets live for the whole session
        for (unsigned i = 0; i < VIDEO_ROWS; i++) g_videoRow[i] = rows[i];
        g_togWindowed = rows[2];
        g_videoScreen = screen;
        logf("video screen: WINDOWED option found");
    }
    SetWindowedToggle();
}

// Called every frame (Present): the game hides TOG_WINDOWED again whenever the screen
// is entered, and its rows are rescaled on resolution changes. Re-show it and lay the
// rows out evenly (rect at +0x24: x, y, w, h in the parent's coordinates).
static void FixVideoScreen(void)
{
    if (!g_togWindowed) return;
    U32(g_togWindowed, 0x20) |= 0x8000;
    U8(g_togWindowed, 0x75) = 0; U8(g_togWindowed, 0x76) = 1;
    if (U8(g_togWindowed, 0x74) > 1) U8(g_togWindowed, 0x74) = 1;
    float *y0 = (float *)(g_videoRow[0] + 0x28), *y1 = (float *)(g_videoRow[1] + 0x28);
    float step = *y1 - *y0;
    if (step <= 0.0f) return;
    for (unsigned i = 2; i < VIDEO_ROWS; i++)
        *(float *)(g_videoRow[i] + 0x28) = *y1 + step * (float)(i - 1);
}
// 0x520200 refreshes SCR_VIDEO (on open, SET TO DEFAULTS, device change) and hides
// TOG_WINDOWED each time; called from 6 places.
static void __fastcall W_VideoRefresh(void *screen, void *edx)
{
    ((pfnScreenFn)0x00520200)(screen, edx);
    if (g_togWindowed) U32(g_togWindowed, 0x20) |= 0x8000;
    SetWindowedToggle();
}
// OPTIONS > VIDEO: the game selects the size from its own settings (800x600 after a
// restart), so select the size the game is running at.
static void __fastcall W_VideoEnter(void *screen, void *edx)
{
    W_VideoRefresh(screen, edx);
    if (!g_videoRow[0] || !g_curW) return;
    char cur[32];
    snprintf(cur, sizeof cur, "%ux%u", g_curW, g_curH);
    for (int i = 0; i <= U8(g_videoRow[0], 0x76) && i < 8; i++)
        if (!strcmp((const char *)(uintptr_t)(0x00739620 + 48 * i), cur)) { U8(g_videoRow[0], 0x74) = (uint8_t)i; break; }
}

// APPLY: the game's own code (protected, 0x520cc0) changes the resolution via
// D3D Reset; the mod adds the WINDOWED toggle and saves both to squadmod.ini.
static void __fastcall W_VideoApply(uint8_t *screen, void *edx)
{
    int want = g_togWindowed ? (U8(g_togWindowed, 0x74) != 0) : g_dispWindowed;
    int changed = want != g_dispWindowed;
    g_dispWindowed = want;
    g_resetSeen = 0;
    ((pfnScreenFn)0x00520cc0)(screen, edx);
    if (changed && !g_resetSeen) ApplyWindow(g_gameWnd, g_curW, g_curH, want);
    SaveDisplay();
}

// Once after startup: select the saved resolution in the VIDEO screen and run APPLY.
static void RestoreDisplay(void)
{
    static int done;
    static DWORD readyAt;
    if (done || !g_videoScreen) return;
    // APPLY opens follow-up screens that are only built once the front end is up,
    // so wait until the menus have been running for a while.
    if (!readyAt) { readyAt = GetTickCount() + 6000; return; }
    if ((int)(GetTickCount() - readyAt) < 0) return;
    // ...and until the "keep these settings?" dialog APPLY shows has its widgets
    // (0x50fec0 uses them unchecked).
    uint8_t *dlg = *(uint8_t **)0x0072fc70;          // message box screen (vt 006CECD8)
    int built = dlg != NULL;
    for (int o = 0x74; built && o <= 0x8c; o += 4) {   // unbuilt slots hold 0 or 0xFAFAFAFA
        uint32_t w = U32(dlg, o);
        built = w >= 0x10000 && w != 0xFAFAFAFA && !IsBadReadPtr((void *)(uintptr_t)w, 4)
             && *(uint32_t *)(uintptr_t)w - 0x00600000u < 0x00100000u;   // has a game vtable
    }
    if (!built) {
        if ((int)(GetTickCount() - readyAt) < 60000) return;
        done = 1; logf("display restore: settings dialog never built, skipped"); return;
    }
    done = 1;
    if (!g_saveW || !g_saveH || (g_saveW == g_curW && g_saveH == g_curH)) return;
    ((pfnScreenFn)0x00520200)(g_videoScreen, 0);           // builds the resolution list
    char want[32];
    snprintf(want, sizeof want, "%ux%u", g_saveW, g_saveH);
    uint8_t *res = g_videoRow[0];
    int max = U8(res, 0x76), found = -1;
    for (int i = 0; i <= max && i < 8; i++)
        if (!strcmp((const char *)(uintptr_t)(0x00739620 + 48 * i), want)) { found = i; break; }
    if (found < 0) { logf("display restore: %s not in the mode list", want); return; }
    U8(res, 0x74) = (uint8_t)found;
    SetWindowedToggle();
    logf("display restore: applying %s", want);
    ((pfnScreenFn)0x00520cc0)(g_videoScreen, 0);
    // APPLY opens the "keep these settings?" dialog (layer 4, object at 0x72fb4c) whose
    // countdown reverts the change; close it the way the video screen's timeout does,
    // without the revert: the saved settings are already known good.
    typedef void *(__fastcall *pfnLayerGet)(void *mgr, void *edx, int layer);
    typedef void  (__fastcall *pfnLayerClose)(void *mgr, void *edx, int layer);
    void *ui = *(void **)0x00736b20;
    if (ui && ((pfnLayerGet)0x00599980)(ui, 0, 4) == *(void **)0x0072fb4c) {
        ((pfnLayerClose)0x00599690)(*(void **)((uint8_t *)ui + 0x28), 0, 4);
        logf("display restore: confirmation dialog closed");
    }
}

static void InstallVideoScreen(void)
{
    static const uint8_t thunk[] = { 0xFF, 0x25, 0x8C, 0xED, 0x17, 0x01 };   // jmp [0117ED8C]
    if (!memcmp((void *)0x00417cf0, thunk, sizeof thunk) && WriteJmp(0x00417cf0, (void *)Hook_ModeFilter))
        logf("video screen: resolution list widened");
    else logf("video screen: unexpected bytes at 00417CF0, resolution list unchanged");
    PatchCall(0x00520e73, 0x0051f090, (void *)Hook_VideoScreenOpened);
    PatchCall(0x00513591, 0x00520200, (void *)W_VideoEnter);    // OPTIONS > VIDEO
    static const uintptr_t refresh[] = { 0x00520699, 0x005206d8, 0x0052079e, 0x00520e6c, 0x00520eae };
    for (unsigned i = 0; i < sizeof refresh / sizeof refresh[0]; i++)
        PatchCall(refresh[i], 0x00520200, (void *)W_VideoRefresh);
    PatchCall(0x00520e96, 0x00520cc0, (void *)W_VideoApply);   // BN_APPLY
}


// ---- STEAM and GRAPHICS screens in the game's OPTIONS menu --------------------------
// The PC build hides two OPTIONS buttons (REFRESH RATE, CENTRE SCREEN) and never uses two
// console screens (ONLINE OPTIONS, NETWORK OPTIONS). They become:
//   OPTIONS > STEAM     (ONLINE OPTIONS:  3 buttons)  invite, player name, lobby
//   OPTIONS > GRAPHICS  (NETWORK OPTIONS: 3 toggles + APPLY)  dgVoodoo.conf settings
// Text: label widgets hold a string hash (+0x48) that the game looks up through
// 0x686a10 when drawing; the mod answers its own hashes there.
int  Tun_SteamStatus(void);
void Tun_Invite(void);
int  Tun_LobbyNames(char *out, int cap);

#define UI_ROOT          (*(uint8_t **)0x00736b20)
#define SCR_OPTIONS_ID   0x215067a4
#define SCR_STEAM_ID     0x2ac24a2b   // SCR_ONLINE_OPTIONS
#define SCR_GRAPHICS_ID  0x80862421   // SCR_NETWORK_OPTIONS
#define SCR_MULTI_ID     0x50dc7ff1   // SCR_MULTIPLAYER_MENU (LAN / INTERNET)
#define BN_INTERNET_ID   0xb6478360   // BN_ONLINE: GameSpy, servers are gone
#define SCR_MAIN_ID      0xf23ea323   // main menu
#define BN_GAMESPY_ID    0x053949b1   // BN_SIGNIN_OUT "GAMESPY ID LOGIN"
#define BN_STEAM_ID      0x40919218   // OPTIONS: BN_OPTIONS_REFRESH_RATE
#define BN_GRAPHICS_ID   0x835b97d7   // OPTIONS: BN_OPTIONS_CENTRE_SCREEN
#define BN_INVITE_ID     0x72e30658   // BN_ONLINE_OPTIONS_FRIENDS
#define BN_NAME_ID       0x33c9dd57   // BN_ONLINE_OPTIONS_OPTIONS
#define BN_LOBBY_ID      0x1dfaca0c   // BN_ONLINE_OPTIONS_SCOREBOARDS
#define BN_GFX_APPLY_ID  0x4304c950   // BTN_NET_OPTIONS_APPLY
#define TYPE_TEXT        0x1c6e1bbb
#define W_SHOWN          0x00108000   // 0x8000 visible, 0x00100000 present on PC

enum { T_STEAM, T_GRAPHICS, T_INVITE, T_NAME, T_LOBBY, T_AA, T_FILTER, T_FPS,
       T_AA_V, T_FILTER_V, T_FPS_V, T_APPLY, T_COOP, T_NOTICE, T_COUNT };
#define T_HASH(i) (0x5D5D0000u + (uint32_t)(i))
static uint16_t g_txt[T_COUNT][96];
static void SetTxt(int i, const char *str)
{
    int n = 0;
    for (; str[n] && n < 95; n++) g_txt[i][n] = (uint8_t)str[n];
    g_txt[i][n] = 0;
}

typedef const uint16_t *(__fastcall *pfnLoc)(void *tbl, void *edx, uint32_t hash);
static pfnLoc real_Loc;
#define HASH_PLAY_LAN     0x0ec98d0f   // "LAN": MULTIPLAYER button + title of the LAN screen
#define HASH_ESRB_NOTICE  0x8b3807d0   // "GAME EXPERIENCE MAY CHANGE DURING ONLINE PLAY"
static const uint16_t *__fastcall Hook_Loc(void *tbl, void *edx, uint32_t hash)
{
    if (hash - T_HASH(0) < T_COUNT) return g_txt[hash - T_HASH(0)];
    if ((hash == HASH_PLAY_LAN || hash == HASH_ESRB_NOTICE) && Tun_SteamStatus() >= 0)   // Steam play on
        return g_txt[hash == HASH_PLAY_LAN ? T_COOP : T_NOTICE];
    return real_Loc(tbl, edx, hash);
}

typedef void *(__fastcall *pfnGetScreen)(void *mgr, void *edx, uint32_t id);
typedef void  (__fastcall *pfnGoScreen)(void *ui, void *edx, uint32_t id, int layer);
typedef uint8_t *(__fastcall *pfnFindDeep)(void *container, void *edx, uint32_t id);
typedef uint8_t *(__fastcall *pfnFindType)(void *container, void *edx, uint32_t type, int *idx);
typedef int  (__fastcall *pfnEvent)(void *self, void *edx, int ev, void *arg);
#define FindDeep ((pfnFindDeep)0x005a0500)
#define FindType ((pfnFindType)0x005a01a0)

static void GoScreen(uint32_t id) { ((pfnGoScreen)0x00599e20)(UI_ROOT, 0, id, 4); }
static uint8_t *GetScreen(uint32_t id)
{
    uint8_t *ui = UI_ROOT;
    if (!ui || !U32(ui, 0x28)) return NULL;
    return ((pfnGetScreen)0x00597540)((void *)(uintptr_t)U32(ui, 0x28), 0, id);
}
static uint8_t *Find(uint8_t *scr, uint32_t id) { return scr ? FindDeep(scr + 0x24, 0, id) : NULL; }
static void LabelRow(uint8_t *row, int t)       // a row's caption text widget
{
    uint8_t *tx = row ? FindType(row + 0x40, 0, TYPE_TEXT, NULL) : NULL;
    if (tx) U32(tx, 0x48) = T_HASH(t);
}
static void LabelId(uint8_t *scr, uint32_t id, int t)
{
    uint8_t *tx = Find(scr, id);
    if (tx) U32(tx, 0x48) = T_HASH(t);
}
static void LabelTitle(uint8_t *scr, uint32_t titleId, uint32_t oldHash, int t)
{
    uint8_t *w = Find(scr, titleId);
    for (int o = 0x24; w && o < 0x100; o += 4)
        if (U32(w, o) == oldHash) { U32(w, o) = T_HASH(t); return; }
    logf("menus: title %08X not relabelled", titleId);
}
static void ShowRow(uint8_t *row) { if (row) U32(row, 0x20) |= W_SHOWN; }
static float *RowY(uint8_t *row) { return (float *)(row + 0x28); }   // rect x, y, w, h at +0x24
static float  RowH(uint8_t *row) { return *(float *)(row + 0x30); }

static uint8_t *g_scrOpt, *g_scrSteam, *g_scrGfx, *g_mpInternet, *g_mainGamespy;
static uint8_t *g_optVideo, *g_optAudio, *g_optSteam, *g_optGfx;

// ---- GRAPHICS: dgVoodoo.conf (read by dgVoodoo when the game starts) ----
static const char *const kAAVal[]   = { "off", "2x", "4x", "8x" };
static const char *const kAALbl[]   = { "OFF", "2X", "4X", "8X" };
static const char *const kFiltVal[] = { "appdriven", "bilinear", "trilinear", "4", "8", "16" };
static const char *const kFiltLbl[] = { "GAME DEFAULT", "BILINEAR", "TRILINEAR", "ANISOTROPIC 4X", "ANISOTROPIC 8X", "ANISOTROPIC 16X" };
static const char *const kFpsVal[]  = { "30", "60", "120", "144", "0" };
static const char *const kFpsLbl[]  = { "30", "60", "120", "144", "UNLIMITED" };
typedef struct {
    uint32_t togId, valueId; const char *section, *key;
    const char *const *vals, *const *lbls; int n, label, valueT;
    uint8_t *tog, *value;
} GfxOpt;
static GfxOpt g_gfx[3] = {
    { 0x214631f9, 0x5ea43dea, "DirectX",    "Antialiasing", kAAVal,   kAALbl,   4, T_AA,     T_AA_V },
    { 0x8e0547aa, 0x733802e1, "DirectX",    "Filtering",    kFiltVal, kFiltLbl, 6, T_FILTER, T_FILTER_V },
    { 0x8e45ca5d, 0x35102b0d, "GeneralExt", "FPSLimit",     kFpsVal,  kFpsLbl,  5, T_FPS,    T_FPS_V },
};
static uint8_t *g_gfxApply;

static char *ReadFileAll(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc(*len + 1);
    if (b) { *len = (long)fread(b, 1, *len, f); b[*len] = 0; }
    fclose(f);
    return b;
}
// Finds "key = value" in [section]; returns the value start and its length.
static const char *ConfFind(const char *txt, const char *section, const char *key, int *vlen)
{
    int in = 0; size_t kl = strlen(key), sl = strlen(section);
    for (const char *ln = txt; *ln; ) {
        const char *end = strchr(ln, '\n'); if (!end) end = ln + strlen(ln);
        const char *c = ln; while (c < end && (*c == ' ' || *c == '\t')) c++;
        if (*c == '[') in = !_strnicmp(c + 1, section, sl) && c[1 + sl] == ']';
        else if (in && !_strnicmp(c, key, kl) && (c[kl] == ' ' || c[kl] == '\t' || c[kl] == '=')) {
            const char *eq = memchr(c, '=', end - c);
            if (eq) {
                const char *v = eq + 1; while (v < end && (*v == ' ' || *v == '\t')) v++;
                const char *ve = v; while (ve < end && *ve != '\r' && *ve != ' ' && *ve != '\t' && *ve != ';') ve++;
                *vlen = (int)(ve - v);
                return v;
            }
        }
        ln = *end ? end + 1 : end;
    }
    return NULL;
}
static void GfxLoad(void)
{
    char path[MAX_PATH]; long len;
    snprintf(path, sizeof path, "%sdgVoodoo.conf", g_dir);
    char *txt = ReadFileAll(path, &len);
    for (int i = 0; i < 3; i++) {
        GfxOpt *o = &g_gfx[i];
        int v = 0, vl = 0;
        const char *cur = txt ? ConfFind(txt, o->section, o->key, &vl) : NULL;
        for (int k = 0; cur && k < o->n; k++)
            if ((int)strlen(o->vals[k]) == vl && !_strnicmp(cur, o->vals[k], vl)) v = k;
        if (o->tog) U8(o->tog, 0x74) = (uint8_t)v;
    }
    free(txt);
    if (!txt) logf("menus: %s not found", path);
}
static int GfxSave(void)
{
    char path[MAX_PATH], bak[MAX_PATH]; long len;
    snprintf(path, sizeof path, "%sdgVoodoo.conf", g_dir);
    snprintf(bak, sizeof bak, "%sdgVoodoo.conf.squadmod.bak", g_dir);
    char *txt = ReadFileAll(path, &len);
    if (!txt) { logf("menus: cannot read %s", path); return 0; }
    CopyFileA(path, bak, TRUE);                     // first save keeps the original
    for (int i = 0; i < 3; i++) {
        GfxOpt *o = &g_gfx[i];
        if (!o->tog) continue;
        const char *val = o->vals[U8(o->tog, 0x74) % o->n];
        int vl; const char *cur = ConfFind(txt, o->section, o->key, &vl);
        if (!cur) { logf("menus: [%s] %s not in dgVoodoo.conf", o->section, o->key); continue; }
        size_t off = cur - txt, nl = strlen(val);
        char *nt = (char *)malloc(len - vl + nl + 1);
        memcpy(nt, txt, off); memcpy(nt + off, val, nl); memcpy(nt + off + nl, cur + vl, len - off - vl + 1);
        free(txt); txt = nt; len = (long)(len - vl + nl);
        logf("menus: dgVoodoo [%s] %s = %s", o->section, o->key, val);
    }
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(txt, 1, len, f) == (size_t)len;
    if (f) fclose(f);
    free(txt);
    return ok;
}

// ---- screen handlers (vtable slot 7 = event, slot 13 = per-frame update) ----
static pfnEvent real_OptionsEvent, real_SteamEvent, real_GfxEvent, real_NetTogEvent;
typedef void (__fastcall *pfnUpdate)(void *self, void *edx);
#define BaseUpdate ((pfnUpdate)0x0059e010)

static int __fastcall W_OptionsEvent(uint8_t *scr, void *edx, int ev, uint8_t *w)
{
    if (ev == 4 && w) {
        if (U32(w, 0x14) == BN_STEAM_ID)    { GoScreen(SCR_STEAM_ID);    return 1; }
        if (U32(w, 0x14) == BN_GRAPHICS_ID) { GoScreen(SCR_GRAPHICS_ID); return 1; }
    }
    return real_OptionsEvent(scr, edx, ev, w);
}

static void SteamText(void)
{
    int st = Tun_SteamStatus();
    char names[160], line[96];
    SetTxt(T_INVITE, st == 1 ? "INVITE FRIENDS" : st == 0 ? "INVITE FRIENDS (CONNECTING...)" : st == 2 ? "INVITE FRIENDS (RECONNECTING...)" : "INVITE FRIENDS (STEAM OFF)");
    SetTxt(T_NAME, g_useSteamName ? "PLAYER NAME: STEAM NAME" : "PLAYER NAME: PROFILE NAME");
    int n = Tun_LobbyNames(names, sizeof names);
    if (st < 0)       snprintf(line, sizeof line, "STEAM PLAY IS OFF (STEAM=1 IN SQUADMOD.INI)");
    else if (st == 0) snprintf(line, sizeof line, "LOBBY: CONNECTING TO STEAM...");
    else if (st == 2) snprintf(line, sizeof line, "LOBBY: CONNECTION LOST - REJOINING...");
    else              snprintf(line, sizeof line, "LOBBY (%d): %s", n, names);
    SetTxt(T_LOBBY, line);
    if (st == 1) snprintf(line, sizeof line, "STEAM LOBBY (%d): %s  -  INVITE FRIENDS WITH F7 OR OPTIONS > STEAM", n, names);
    else         snprintf(line, sizeof line, st == 2 ? "STEAM LOBBY LOST - REJOINING..." : "CONNECTING TO STEAM...");
    SetTxt(T_NOTICE, line);
}
static int __fastcall W_SteamEvent(uint8_t *scr, void *edx, int ev, uint8_t *w)
{
    if (ev == 4) {
        if (!w) return 0;
        if (U32(w, 0x14) == BN_INVITE_ID) {
            if (Tun_SteamStatus() == 1) Tun_Invite();
            return 1;
        }
        if (U32(w, 0x14) == BN_NAME_ID) {
            g_useSteamName = !g_useSteamName;
            WritePrivateProfileStringA("SquadMod", "SteamName", g_useSteamName ? "1" : "0", g_ini);
            logf("menus: player name = %s", g_useSteamName ? "Steam" : "profile");
            SteamText();
            return 1;
        }
        return 1;                                     // LOBBY: nothing to do
    }
    if (ev == 5) { GoScreen(SCR_OPTIONS_ID); return 1; }   // back
    if (ev == 0x15) { SteamText(); return 0; }              // entered (skip the GameSpy code)
    return real_SteamEvent(scr, edx, ev, w);
}
static void __fastcall W_SteamUpdate(uint8_t *scr, void *edx)
{
    BaseUpdate(scr, edx);
    static DWORD t;
    if (GetTickCount() - t > 500) { t = GetTickCount(); SteamText(); }
}

static void GfxText(void)
{
    for (int i = 0; i < 3; i++) {
        GfxOpt *o = &g_gfx[i];
        if (!o->tog) continue;
        U8(o->tog, 0x75) = 0; U8(o->tog, 0x76) = (uint8_t)(o->n - 1);
        if (U8(o->tog, 0x74) >= o->n) U8(o->tog, 0x74) = 0;
        SetTxt(o->valueT, o->lbls[U8(o->tog, 0x74)]);
        if (o->value) U32(o->value, 0x48) = T_HASH(o->valueT);   // the game resets it on entry
    }
}
static int __fastcall W_GfxEvent(uint8_t *scr, void *edx, int ev, uint8_t *w)
{
    if (ev == 4) {
        // the event carries the screen, not the button: check the group's focused row (+0x50)
        uint8_t *grp = g_gfxApply ? (uint8_t *)(uintptr_t)U32(g_gfxApply, 0x0c) : NULL;
        if ((w && U32(w, 0x14) == BN_GFX_APPLY_ID) || (grp && (uint8_t *)(uintptr_t)U32(grp, 0x50) == g_gfxApply))
            SetTxt(T_APPLY, GfxSave() ? "SAVED - RESTART THE GAME TO USE" : "COULD NOT SAVE DGVOODOO.CONF");
        return 1;
    }
    if (ev == 5) { GoScreen(SCR_OPTIONS_ID); return 1; }
    if (ev == 0x15) {
        int r = real_GfxEvent(scr, edx, ev, w);
        GfxLoad(); GfxText();
        SetTxt(T_APPLY, "APPLY (AFTER RESTART)");
        return r;
    }
    return real_GfxEvent(scr, edx, ev, w);
}
// The three toggles belong to a voice-chat toggle class; keep its code away from them.
static int __fastcall W_NetTogEvent(uint8_t *tog, void *edx, int ev, void *arg)
{
    for (int i = 0; i < 3; i++) if (U32(tog, 0x14) == g_gfx[i].togId) return 0;
    return real_NetTogEvent(tog, edx, ev, arg);
}

// ...and its per-frame update (0x510820 = base 0x5a5e70 + 0x510240, which rewrites
// the value text and sets the GameSpy online status).
static pfnUpdate real_NetTogUpdate;
static void __fastcall W_NetTogUpdate(uint8_t *tog, void *edx)
{
    for (int i = 0; i < 3; i++)
        if (U32(tog, 0x14) == g_gfx[i].togId) { ((pfnUpdate)0x005a5e70)(tog, edx); return; }
    real_NetTogUpdate(tog, edx);
}

static int MenusSetup(void)
{
    g_scrOpt = GetScreen(SCR_OPTIONS_ID);
    g_scrSteam = GetScreen(SCR_STEAM_ID);
    g_scrGfx = GetScreen(SCR_GRAPHICS_ID);
    if (!g_scrOpt || !g_scrSteam || !g_scrGfx) return 0;
    if (U32(g_scrOpt, 0x14) != SCR_OPTIONS_ID || U32(g_scrSteam, 0x14) != SCR_STEAM_ID || U32(g_scrGfx, 0x14) != SCR_GRAPHICS_ID) {
        logf("menus: screen lookup mismatch");
        return -1;
    }
    g_optVideo = Find(g_scrOpt, 0x46016b2c);
    g_optAudio = Find(g_scrOpt, 0xd0d96e32);
    g_optSteam = Find(g_scrOpt, BN_STEAM_ID);
    g_optGfx   = Find(g_scrOpt, BN_GRAPHICS_ID);
    LabelRow(g_optSteam, T_STEAM);
    LabelRow(g_optGfx, T_GRAPHICS);
    // The group lays out and navigates its visible rows in child-array order, which
    // puts these two between GAMMA and VIDEO. (Reordering the array makes the game
    // quit when the VIDEO screen applies settings.)

    LabelTitle(g_scrSteam, 0x08736add, 0x7e74ba26, T_STEAM);
    LabelRow(Find(g_scrSteam, BN_INVITE_ID), T_INVITE);
    LabelRow(Find(g_scrSteam, BN_NAME_ID), T_NAME);
    LabelRow(Find(g_scrSteam, BN_LOBBY_ID), T_LOBBY);

    LabelTitle(g_scrGfx, 0x0873dc9d, 0x7e74ba26, T_GRAPHICS);
    for (int i = 0; i < 3; i++) {
        g_gfx[i].tog = Find(g_scrGfx, g_gfx[i].togId);
        LabelRow(g_gfx[i].tog, g_gfx[i].label);
        g_gfx[i].value = Find(g_scrGfx, g_gfx[i].valueId);
    }
    g_gfxApply = Find(g_scrGfx, BN_GFX_APPLY_ID);
    uint8_t *mp = GetScreen(SCR_MULTI_ID);
    if (mp && U32(mp, 0x14) == SCR_MULTI_ID) g_mpInternet = Find(mp, BN_INTERNET_ID);
    if (!g_mpInternet) logf("menus: MULTIPLAYER > INTERNET not found, left as is");
    uint8_t *mm = GetScreen(SCR_MAIN_ID);
    if (mm && U32(mm, 0x14) == SCR_MAIN_ID) g_mainGamespy = Find(mm, BN_GAMESPY_ID);
    if (!g_mainGamespy) logf("menus: main menu GAMESPY ID LOGIN not found, left as is");
    LabelRow(g_gfxApply, T_APPLY);
    logf("menus: STEAM and GRAPHICS screens ready (%p %p %p %p)", g_optSteam, g_optGfx, g_gfx[0].tog, g_gfxApply);
    return 1;
}

// Every frame (Present): keep the rows shown and laid out (positions scale with the
// resolution, so they are derived from rows the game placed itself).
static void MenuFrame(void)
{
    static int state;              // 0 = not yet, 1 = ready, -1 = gave up
    static DWORD next;
    if (state == 0) {
        if (GetTickCount() < next) return;
        next = GetTickCount() + 1000;
        state = MenusSetup();
        if (state <= 0) return;
    }
    if (state < 0) return;
    if (g_mpInternet) U32(g_mpInternet, 0x20) &= ~0x8000u;
    if (g_mainGamespy) U32(g_mainGamespy, 0x20) &= ~0x8000u;   // hidden rows are skipped by layout + navigation
    static DWORD tText;
    if (GetTickCount() - tText > 500) { tText = GetTickCount(); SteamText(); }   // lobby line in MULTIPLAYER
    ShowRow(g_optSteam); ShowRow(g_optGfx);
    ShowRow(Find(g_scrSteam, BN_INVITE_ID)); ShowRow(Find(g_scrSteam, BN_NAME_ID)); ShowRow(Find(g_scrSteam, BN_LOBBY_ID));
    uint8_t *pic = Find(g_scrSteam, 0xfeab8365);         // GameSpy friend-message icon
    if (pic) U32(pic, 0x20) &= ~0x8000u;
    if (g_gfx[0].tog && g_gfxApply) {
        float y0 = *RowY(g_gfx[0].tog), step = RowH(g_gfx[0].tog);
        for (int i = 0; i < 3; i++) if (g_gfx[i].tog) { ShowRow(g_gfx[i].tog); *RowY(g_gfx[i].tog) = y0 + step * i; }
        ShowRow(g_gfxApply); *RowY(g_gfxApply) = y0 + step * 3;
        GfxText();
    }
}

static void InstallMenus(void)
{
    static const uint8_t locExpect[] = { 0x8B, 0x41, 0x38, 0x85, 0xC0, 0x76 };
    if (memcmp((void *)0x00686a10, locExpect, sizeof locExpect)) { logf("menus: unexpected bytes at 00686A10, menus off"); return; }
    uint8_t *t = (uint8_t *)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return;
    memcpy(t, (void *)0x00686a10, 5);                    // mov eax,[ecx+38h]; test eax,eax
    t[5] = 0xE9; *(int32_t *)(t + 6) = (int32_t)(0x00686a15 - (uintptr_t)(t + 10));
    real_Loc = (pfnLoc)t;
    if (!WriteJmp(0x00686a10, (void *)Hook_Loc)) return;
    SetTxt(T_STEAM, "STEAM"); SetTxt(T_GRAPHICS, "GRAPHICS");
    SetTxt(T_AA, "ANTI-ALIASING"); SetTxt(T_FILTER, "TEXTURE FILTERING"); SetTxt(T_FPS, "FRAME RATE LIMIT");
    SetTxt(T_APPLY, "APPLY (AFTER RESTART)");
    SetTxt(T_COOP, "STEAM CO-OP");
    SteamText();
    for (int i = 0; i < 3; i++) SetTxt(g_gfx[i].valueT, "");
    PatchVtbl((void **)0x006cd730, 7, (void *)W_OptionsEvent, (void **)&real_OptionsEvent);
    PatchVtbl((void **)0x006ce4f0, 7, (void *)W_SteamEvent, (void **)&real_SteamEvent);
    static void *origSteamUpdate;
    PatchVtbl((void **)0x006ce4f0, 13, (void *)W_SteamUpdate, &origSteamUpdate);
    PatchVtbl((void **)0x006ce558, 7, (void *)W_GfxEvent, (void **)&real_GfxEvent);
    PatchVtbl((void **)0x006cd620, 7, (void *)W_NetTogEvent, (void **)&real_NetTogEvent);
    PatchVtbl((void **)0x006cd620, 13, (void *)W_NetTogUpdate, (void **)&real_NetTogUpdate);
    logf("menus: hooks installed");
}

static int IniInt(const char *ini, const char *key, int def)
{
    return GetPrivateProfileIntA("SquadMod", key, def, ini);
}

// ---- diagnostics (Debug=1): why the game quits ----
static LONG CALLBACK DiagVeh(EXCEPTION_POINTERS *e)
{
    DWORD c = e->ExceptionRecord->ExceptionCode;
    if (c == 0xC0000005 || c == 0xC000001D || c == 0xC0000096 || c == 0xC00000FD || c == 0xC0000409) {
        static int n;
        if (n++ < 20) logf("diag: exception %08X at %08X (ecx %08X esi %08X)", (unsigned)c,
            (unsigned)e->ContextRecord->Eip, (unsigned)e->ContextRecord->Ecx, (unsigned)e->ContextRecord->Esi);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
typedef void (WINAPI *pfnExitProcess)(UINT);
static pfnExitProcess real_ExitProcess;
static void WINAPI Hook_ExitProcess(UINT code)
{
    void *ret = __builtin_return_address(0);
    uint32_t *sp = (uint32_t *)__builtin_frame_address(0);
    logf("diag: ExitProcess(%u) from %p, stack %08X %08X %08X %08X %08X %08X %08X %08X", code, ret,
         sp[2], sp[3], sp[4], sp[5], sp[6], sp[7], sp[8], sp[9]);
    real_ExitProcess(code);
}

static void Init(HMODULE self)
{
    GetModuleFileNameA(self, g_dir, MAX_PATH);
    char *slash = strrchr(g_dir, '\\'); if (slash) slash[1] = 0;

    char ini[MAX_PATH];
    snprintf(ini, sizeof ini, "%ssquadmod.ini", g_dir);
    snprintf(g_ini, sizeof g_ini, "%s", ini);
    LoadDisplay();
    cfg.enabled    = IniInt(ini, "Enabled", 1);
    cfg.fillCount  = IniInt(ini, "FillCount", 4);
    cfg.hostOwnsAI = IniInt(ini, "HostOwnsAI", 1);
    cfg.log        = IniInt(ini, "Log", 1);
    cfg.loopback   = IniInt(ini, "LoopbackTest", 0);
    cfg.freeMouse  = IniInt(ini, "FreeMouse", 1);
    cfg.debug      = IniInt(ini, "Debug", 0);
    cfg.steam      = IniInt(ini, "Steam", 0);
    cfg.tunnelTest = IniInt(ini, "TunnelTest", 0);
    if (cfg.fillCount < 1) cfg.fillCount = 1;
    if (cfg.fillCount > 4) cfg.fillCount = 4;

    // instance number: first process to grab the mutex is #1
    CreateMutexA(NULL, FALSE, "Local\\CGTSquadMod_Instance1");
    g_instance = (GetLastError() == ERROR_ALREADY_EXISTS) ? 2 : 1;

    if (cfg.log) {
        char lp[MAX_PATH];
        snprintf(lp, sizeof lp, "%ssquadmod_%d.log", g_dir, g_instance);
        g_log = fopen(lp, "w");
    }
    logf("SquadMod loaded. instance=%d enabled=%d fill=%d hostOwnsAI=%d loopback=%d",
         g_instance, cfg.enabled, cfg.fillCount, cfg.hostOwnsAI, cfg.loopback);

    // sanity check: make sure this is the build we analysed
    static const uint8_t expect[] = { 0x83, 0xEC, 0x0C, 0xA1, 0x14, 0xFE, 0x6F, 0x00 };
    if (memcmp((void *)A_SPAWN_SQUAD, expect, sizeof expect) != 0) {
        logf("ERROR: unexpected bytes at %08X - wrong game version, squad hook NOT installed",
             A_SPAWN_SQUAD);
    } else if (WriteJmp(A_SPAWN_SQUAD, (void *)Hook_SpawnSquad)) {
        logf("squad spawner hooked");
    }
    if (cfg.enabled) { InstallOrderProbe(); InstallRelay(); InstallKitFix(); InstallHumanFilter(); }
    if (cfg.debug) InstallSendProbe();
    if (cfg.enabled) {
        // Squad-order input in the per-player update (FUN around 0x50a360) is skipped in
        // network games by CALL 0x471ec0 ("is network game") -> TEST/JNZ. Neutralise the
        // call at each site (XOR EAX,EAX) so order mode can be entered AND left again.
        // The order functions themselves already forward client orders to the host.
        static const struct { uintptr_t at; uint8_t rel[4]; const char *what; } gates[] = {
            { 0x0050a751, { 0x6A, 0x77, 0xF6, 0xFF }, "order button release (leave order mode)" },
            { 0x0050a8e0, { 0xDB, 0x75, 0xF6, 0xFF }, "order handler call" },
        };
        for (int i = 0; i < (int)(sizeof gates / sizeof gates[0]); i++) {
            uint8_t *p = (uint8_t *)gates[i].at;
            if (p[0] == 0xE8 && !memcmp(p + 1, gates[i].rel, 4)) {
                DWORD old;
                VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
                p[0] = 0x31; p[1] = 0xC0; p[2] = 0x90; p[3] = 0x90; p[4] = 0x90;   // xor eax,eax; nop x3
                VirtualProtect(p, 5, old, &old);
                FlushInstructionCache(GetCurrentProcess(), p, 5);
                logf("MP gate removed @%08X: %s", (unsigned)gates[i].at, gates[i].what);
            } else logf("MP gate @%08X: unexpected bytes, not patched", (unsigned)gates[i].at);
        }
    }

    if (cfg.debug) {
        AddVectoredExceptionHandler(1, DiagVeh);
        real_ExitProcess = (pfnExitProcess)PatchIAT("KERNEL32.dll", "ExitProcess", (void *)Hook_ExitProcess);
        logf("diag: exception + ExitProcess logging %s", real_ExitProcess ? "on" : "partly on");
    }
    real_CreateMutexA = (pfnCreateMutexA)PatchIAT("KERNEL32.dll", "CreateMutexA", (void *)Hook_CreateMutexA);
    real_CreateEventA = (pfnCreateEventA)PatchIAT("KERNEL32.dll", "CreateEventA", (void *)Hook_CreateEventA);
    real_OpenEventA   = (pfnOpenEventA)  PatchIAT("KERNEL32.dll", "OpenEventA",   (void *)Hook_OpenEventA);
    real_GetTopWindow = (pfnGetTopWindow)PatchIAT("USER32.dll",   "GetTopWindow", (void *)Hook_GetTopWindow);
    if (!real_CreateMutexA) real_CreateMutexA = CreateMutexA;
    if (!real_CreateEventA) real_CreateEventA = CreateEventA;
    if (!real_OpenEventA)   real_OpenEventA   = OpenEventA;
    if (!real_GetTopWindow) real_GetTopWindow = GetTopWindow;
    logf("multi-instance hooks installed");

    if (cfg.freeMouse) {
        real_ClipCursor  = (pfnClipCursor)  PatchIAT("USER32.dll", "ClipCursor",   (void *)Hook_ClipCursor);
        real_SetCursorPos = (pfnSetCursorPos)PatchIAT("USER32.dll", "SetCursorPos", (void *)Hook_SetCursorPos);
        real_SetCapture   = (pfnSetCapture)  PatchIAT("USER32.dll", "SetCapture",   (void *)Hook_SetCapture);
        if (!real_ClipCursor)   real_ClipCursor   = ClipCursor;
        if (!real_SetCursorPos) real_SetCursorPos = SetCursorPos;
        if (!real_SetCapture)   real_SetCapture   = SetCapture;
        logf("free mouse: user32 cursor hooks installed");

        real_RegisterClassA = (pfnRegisterClassA)PatchIAT("USER32.dll", "RegisterClassA", (void *)Hook_RegisterClassA);
        real_ShowWindow     = (pfnShowWindow)    PatchIAT("USER32.dll", "ShowWindow",     (void *)Hook_ShowWindow);
        real_SetWindowPos   = (pfnSetWindowPos)  PatchIAT("USER32.dll", "SetWindowPos",   (void *)Hook_SetWindowPos);
        if (!real_RegisterClassA) real_RegisterClassA = RegisterClassA;
        if (!real_ShowWindow)     real_ShowWindow     = ShowWindow;
        if (!real_SetWindowPos)   real_SetWindowPos   = SetWindowPos;
        logf("keep-running hooks installed");

        real_D3DCreate9 = (pfnD3DCreate9)PatchIAT("d3d9.dll", "Direct3DCreate9", (void *)Hook_D3DCreate9);
        logf("Direct3DCreate9 hook: %s", real_D3DCreate9 ? "ok" : "FAILED");
    }

    InstallVideoScreen();
    g_useSteamName = IniInt(ini, "SteamName", 1);
    InstallMenus();
    int tunMode = cfg.steam ? 2 : (cfg.loopback && cfg.tunnelTest) ? 1 : 0;
    if (tunMode == 2) {
        static const uint8_t nameExpect[] = { 0x80, 0x3D, 0xC4, 0x17, 0x70, 0x00, 0xFD };
        if (!memcmp((void *)0x0058f300, nameExpect, sizeof nameExpect) && WriteJmp(0x0058f300, (void *)Hook_ApplyLocalName))
            logf("player name: Steam name will be used in game");
        else logf("player name: unexpected bytes at 0058F300, keeping profile name");
    }
    if (tunMode) {
        Tun_Start(tunMode, g_instance, g_dir, logf, PatchIAT, &g_hostIp);
    } else if (!cfg.loopback && cfg.enabled) {
        real_connect = (pfnConnect)PatchIAT("WSOCK32.dll", "connect", (void *)Hook_connect);
        if (!real_connect) real_connect = connect;
    }
    if (cfg.loopback && !tunMode) {
        real_bind   = (pfnBind)  PatchIAT("WSOCK32.dll", "bind",   (void *)Hook_bind);
        real_sendto = (pfnSendto)PatchIAT("WSOCK32.dll", "sendto", (void *)Hook_sendto);
        real_connect       = (pfnConnect)      PatchIAT("WSOCK32.dll", "connect",       (void *)Hook_connect);
        real_recvfrom      = (pfnRecvfrom)     PatchIAT("WSOCK32.dll", "recvfrom",      (void *)Hook_recvfrom);
        real_accept        = (pfnAccept)       PatchIAT("WSOCK32.dll", "accept",        (void *)Hook_accept);
        real_gethostbyname = (pfnGethostbyname)PatchIAT("WSOCK32.dll", "gethostbyname", (void *)Hook_gethostbyname);
        logf("net hooks: connect=%p recvfrom=%p accept=%p gethostbyname=%p",
             real_connect, real_recvfrom, real_accept, real_gethostbyname);
        logf("loopback mode: bind=%p sendto=%p (this instance = 127.0.0.%d)",
             real_bind, real_sendto, g_instance);
        if (!real_bind)   real_bind   = (pfnBind)  GetProcAddress(LoadLibraryA("wsock32.dll"), "bind");
        if (!real_sendto) real_sendto = (pfnSendto)GetProcAddress(LoadLibraryA("wsock32.dll"), "sendto");
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        LoadRealDinput8();
        Init(h);
    }
    return TRUE;
}
