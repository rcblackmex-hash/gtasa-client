// =====================================================================
// GTA SA Cliente Multijugador - Android (AML v1.2.4)
// v0.36-protocol (protocolo real del server descubierto y aplicado)
//
// VICTORIA EN v0.35:
//   - Cliente quieto cuando no hay remotos. Mundo se ve normal.
//   - 6+ minutos manejando confirmados sin glitches.
//
// DESCUBRIMIENTO v0.36 (sniffer en tools/sniffer.py):
//   El server NO manda PLAYERS|id|name|x|y|z como decia el contexto v0.33.
//   El formato REAL con 2+ clientes conectados es un STREAM INTERLEAVED:
//
//       PLAYERS|<id1>|<name1>                          (1er player, sin pos)
//       POS|<x1>|<y1>|<z1>[|<id2>|<name2>]             (pos del anterior +
//                                                       opcional anuncia next)
//       POS|<x2>|<y2>|<z2>[|<id3>|<name3>]             ...
//       POS|<xN>|<yN>|<zN>                             (ultimo, sin trailing)
//
//   El parser v0.33-v0.35 exigia >=6 campos en PLAYERS y NUNCA aceptaba
//   POS como input, asi que NUNCA podia ver remotos reales. Por eso el
//   campo "remotos" del log siempre dio 0 (excepto la sesion zombie de
//   v0.33 que tenia otro origen).
//
// PLAN v0.36 - PROTOCOL:
//   1. State machine con g_PendingId/g_PendingName entre mensajes.
//   2. HandlePlayersAnnounce: PLAYERS|id|name -> setea pending.
//   3. HandlePosUpdate: POS|x|y|z[|nid|nname] -> aplica pos al pending,
//      si hay trailing setea nuevo pending.
//   4. Filtros (id == g_MyId, name == PLAYER_NAME, pos en rango) en
//      RemotePassesFilters() reutilizable.
//   5. Reset del pending cuando llega QUIT del id pendiente.
//
// Sigue funcionando v0.34 (tether, repopulate, auto-echo filter) y
// v0.35 (modo IDLE sin remotos).
//
// Server: yamanote.proxy.rlwy.net:19365 (Railway TCP proxy)
// =====================================================================

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <mutex>
#include <pthread.h>
#include <setjmp.h>
#include <set>
#include <signal.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <vector>

#include <android/log.h>

#define LOG_TAG "GTASAClient"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------
static const char*    SERVER_HOST = "yamanote.proxy.rlwy.net";
static const uint16_t SERVER_PORT = 19365;
static const char*    PLAYER_NAME = "rcblackmex";

static const int   kHoverTickMs    = 100;
static const int   kInitialWaitS   = 20;
static const int64_t kRemoteTimeoutMs = 30000; // 30s sin update -> drop
static const size_t kSizeofCPed    = 0x9C8;

// v0.34: anti-streaming tether
static const float kTetherDistance   = 40.0f;  // si remoto > 40m del CJ -> tether
static const float kTetherTargetDist = 30.0f;  // colocar al ped a 30m del CJ
// v0.34: re-populate free pool si baja de este threshold
static const size_t kMinFreePeds = 3;

// ---------------------------------------------------------------------
// AML interface
// ---------------------------------------------------------------------
struct ModInfo {
    const char* szGUID;
    const char* szName;
    const char* szAuthor;
    const char* szVersion;
};

extern "C" {
    ModInfo modinfo = {
        "com.rcblackmex.gtasaclient",
        "GTA SA Cliente MP",
        "rcblackmex",
        "0.36-protocol"
    };
    ModInfo* __GetModInfo() { return &modinfo; }
}

// ---------------------------------------------------------------------
// Offsets confirmados
// ---------------------------------------------------------------------
#define OFF_CWORLD_PLAYERS    0x00BDC738
#define OFF_PED_MATRIX        0x18
#define OFF_MATRIX_POS        0x30

static uintptr_t g_GTASABase = 0;

// ---------------------------------------------------------------------
// Memoria helpers
// ---------------------------------------------------------------------
static uintptr_t FindLibBase(const char* name) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, name)) {
            uintptr_t addr;
            if (sscanf(line, "%lx-", &addr) == 1) { base = addr; break; }
        }
    }
    fclose(f);
    return base;
}

static inline uintptr_t StripTag(uintptr_t addr) {
    return addr & 0x00ffffffffffffffULL;
}

static inline bool IsValidPtr(uintptr_t addr) {
    uintptr_t real = StripTag(addr);
    return real >= 0x6000000000ULL && real <= 0x7fffffffffffULL;
}

static bool IsPageMapped(uintptr_t addr) {
    uintptr_t real = StripTag(addr);
    const size_t pageSize = 4096;
    uintptr_t pageStart = real & ~(uintptr_t)(pageSize - 1);
    unsigned char vec;
    return mincore((void*)pageStart, pageSize, &vec) == 0;
}

static bool SafeRead(uintptr_t addr, void* out, size_t len) {
    if (!IsValidPtr(addr)) return false;
    if (!IsPageMapped(addr)) return false;
    memcpy(out, (void*)addr, len);
    return true;
}

static bool ReadLocalPlayerPos(float* outX, float* outY, float* outZ) {
    if (g_GTASABase == 0) return false;
    uintptr_t playersAddr = g_GTASABase + OFF_CWORLD_PLAYERS;
    uintptr_t pInfo = 0;
    if (!SafeRead(playersAddr, &pInfo, sizeof(pInfo))) return false;
    if (!IsValidPtr(pInfo)) return false;
    uintptr_t pPed = 0;
    if (!SafeRead(pInfo + OFF_PED_MATRIX, &pPed, sizeof(pPed))) return false;
    if (!IsValidPtr(pPed)) return false;
    float v[3] = {0};
    if (!SafeRead(pPed + OFF_MATRIX_POS, v, sizeof(v))) return false;
    if (v[0] > -3500 && v[0] < 3500 &&
        v[1] > -3500 && v[1] < 3500 &&
        v[2] > -100  && v[2] < 1100 &&
        (v[0] != 0 || v[1] != 0 || v[2] != 0))
    {
        *outX = v[0]; *outY = v[1]; *outZ = v[2];
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------
// Signal handler
// ---------------------------------------------------------------------
static sigjmp_buf g_FaultBuf;
static volatile sig_atomic_t g_InProtectedScan = 0;
static struct sigaction g_OldSegv, g_OldBus;

static void ScanFaultHandler(int sig, siginfo_t*, void*) {
    if (g_InProtectedScan) siglongjmp(g_FaultBuf, 1);
    sigaction(SIGSEGV, &g_OldSegv, nullptr);
    sigaction(SIGBUS,  &g_OldBus,  nullptr);
    raise(sig);
}
static void InstallScanFaultHandler() {
    struct sigaction sa{};
    sa.sa_sigaction = ScanFaultHandler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_OldSegv);
    sigaction(SIGBUS,  &sa, &g_OldBus);
}
static void RestoreScanFaultHandler() {
    sigaction(SIGSEGV, &g_OldSegv, nullptr);
    sigaction(SIGBUS,  &g_OldBus,  nullptr);
}

// ---------------------------------------------------------------------
// Simbolos del engine
// ---------------------------------------------------------------------
typedef void* (*FnFindPlayerPed)(int);
typedef bool  (*FnIsPedPointerValid)(void*);

struct EngineSyms {
    FnFindPlayerPed       FindPlayerPed       = nullptr;
    FnIsPedPointerValid   IsPedPointerValid   = nullptr;
    void**                ms_pPedPool         = nullptr;
    uintptr_t             vt_CPed             = 0;
    uintptr_t             vt_CPlayerPed       = 0;
};
static EngineSyms g_Syms;

static bool ResolveSymbols() {
    LOGI("[SYM] resolviendo simbolos en libGTASA.so...");
    void* h = dlopen("libGTASA.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) {
        LOGE("[SYM] dlopen(libGTASA.so) NULL: %s", dlerror());
        return false;
    }
    g_Syms.FindPlayerPed     = (FnFindPlayerPed)    dlsym(h, "_Z13FindPlayerPedi");
    g_Syms.IsPedPointerValid = (FnIsPedPointerValid)dlsym(h, "_Z17IsPedPointerValidP4CPed");
    g_Syms.ms_pPedPool       = (void**)dlsym(h, "_ZN6CPools11ms_pPedPoolE");
    g_Syms.vt_CPed           = (uintptr_t)dlsym(h, "_ZTV4CPed");
    g_Syms.vt_CPlayerPed     = (uintptr_t)dlsym(h, "_ZTV10CPlayerPed");

    LOGI("[SYM] FindPlayerPed       = %p", (void*)g_Syms.FindPlayerPed);
    LOGI("[SYM] IsPedPointerValid   = %p", (void*)g_Syms.IsPedPointerValid);
    LOGI("[SYM] ms_pPedPool         = %p", (void*)g_Syms.ms_pPedPool);

    if (g_Syms.vt_CPed)       g_Syms.vt_CPed       += 0x10;
    if (g_Syms.vt_CPlayerPed) g_Syms.vt_CPlayerPed += 0x10;
    return g_Syms.FindPlayerPed && g_Syms.ms_pPedPool && g_Syms.IsPedPointerValid;
}

// ---------------------------------------------------------------------
// CPool helpers
// ---------------------------------------------------------------------
struct CPoolView {
    uintptr_t mObjects;
    uintptr_t mFlags;
    int       mSize;
    int       mFirstFree;
    bool      valid;
};

static CPoolView LoadCPedPool(void) {
    CPoolView v{};
    if (!g_Syms.ms_pPedPool) return v;

    uintptr_t poolPtr = 0;
    InstallScanFaultHandler();
    g_InProtectedScan = 1;
    if (sigsetjmp(g_FaultBuf, 1) == 0) {
        poolPtr = *(uintptr_t*)g_Syms.ms_pPedPool;
    }
    g_InProtectedScan = 0;
    RestoreScanFaultHandler();

    uintptr_t pool = StripTag(poolPtr);
    if (!IsValidPtr(pool) || !IsPageMapped(pool)) {
        LOGE("[POOL] poolPtr invalido");
        return v;
    }

    InstallScanFaultHandler();
    g_InProtectedScan = 1;
    if (sigsetjmp(g_FaultBuf, 1) == 0) {
        v.mObjects   = *(uintptr_t*)(pool + 0x00);
        v.mFlags     = *(uintptr_t*)(pool + 0x08);
        v.mSize      = *(int*)      (pool + 0x10);
        v.mFirstFree = *(int*)      (pool + 0x14);
        v.valid = true;
    }
    g_InProtectedScan = 0;
    RestoreScanFaultHandler();

    LOGI("[POOL] m_pObjects=0x%lx m_pFlags=0x%lx m_nSize=%d firstFree=%d",
         (unsigned long)v.mObjects, (unsigned long)v.mFlags, v.mSize, v.mFirstFree);
    return v;
}

// ---------------------------------------------------------------------
// Iterar pool y construir lista de peds disponibles (excluyendo CJ)
// ---------------------------------------------------------------------
struct PedSlot {
    uintptr_t pedPtr;       // strip tag
    uintptr_t matrixPtr;    // strip tag
};

static std::vector<PedSlot> IteratePool(const CPoolView& v, uintptr_t cjStrip) {
    std::vector<PedSlot> out;
    if (!v.valid) return out;
    uintptr_t objs  = StripTag(v.mObjects);
    uintptr_t flags = StripTag(v.mFlags);

    int activos = 0, libres = 0, validados = 0;
    InstallScanFaultHandler();
    for (int i = 0; i < v.mSize; i++) {
        uint8_t fl = 0xFF;
        g_InProtectedScan = 1;
        if (sigsetjmp(g_FaultBuf, 1) == 0) fl = *(uint8_t*)(flags + i);
        g_InProtectedScan = 0;
        if (fl & 0x80) { libres++; continue; }
        activos++;

        uintptr_t pedPtr = objs + i * kSizeofCPed;

        bool ok = false;
        g_InProtectedScan = 1;
        if (sigsetjmp(g_FaultBuf, 1) == 0) ok = g_Syms.IsPedPointerValid((void*)pedPtr);
        g_InProtectedScan = 0;
        if (!ok) continue;
        validados++;

        // Saltar al CJ
        if (StripTag(pedPtr) == cjStrip) continue;

        uintptr_t pMat = 0;
        g_InProtectedScan = 1;
        if (sigsetjmp(g_FaultBuf, 1) == 0) pMat = *(uintptr_t*)(pedPtr + OFF_PED_MATRIX);
        g_InProtectedScan = 0;
        uintptr_t pMs = StripTag(pMat);
        if (!IsValidPtr(pMs) || !IsPageMapped(pMs)) continue;

        PedSlot p{};
        p.pedPtr    = StripTag(pedPtr);
        p.matrixPtr = pMs;
        out.push_back(p);
    }
    RestoreScanFaultHandler();
    LOGI("[ITER] %d activos / %d libres / %d validados via API / %zu disponibles",
         activos, libres, validados, out.size());
    return out;
}

// ---------------------------------------------------------------------
// REMOTE PLAYERS state
// ---------------------------------------------------------------------
struct RemotePlayer {
    int         id;
    std::string name;
    float       pos[3];
    int64_t     lastUpdateMs;
    PedSlot     ped;        // {0,0} si no asignado
    bool        tethered = false; // v0.34: true si lo estamos manteniendo cerca por anti-streaming
};

static std::mutex                   g_RemoteMutex;
static std::map<int, RemotePlayer>  g_Remotes;
static std::vector<PedSlot>         g_FreePeds;  // peds del pool sin asignar
static int                          g_MyId = -1;

// v0.34: estado del pool para poder re-poblar el free pool cuando se vacia
static CPoolView                    g_PoolView;
static uintptr_t                    g_CjStrip = 0;

static int64_t NowMs() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void FreeRemotePed(RemotePlayer& r) {
    if (r.ped.pedPtr) {
        // v0.34: solo devolver al free pool si el ped sigue vivo (no murio
        // por streaming). Si murio, lo descartamos silenciosamente.
        bool alive = false;
        InstallScanFaultHandler();
        g_InProtectedScan = 1;
        if (sigsetjmp(g_FaultBuf, 1) == 0) {
            alive = g_Syms.IsPedPointerValid((void*)r.ped.pedPtr);
        }
        g_InProtectedScan = 0;
        RestoreScanFaultHandler();
        if (alive) {
            g_FreePeds.push_back(r.ped);
        } else {
            LOGW("[REMOTE] ped 0x%lx ya estaba muerto al liberar id=%d",
                 (unsigned long)r.ped.pedPtr, r.id);
        }
        r.ped = PedSlot{};
    }
}

static bool TryAssignPedToRemote(RemotePlayer& r) {
    if (r.ped.pedPtr) return true;
    if (g_FreePeds.empty()) return false;
    r.ped = g_FreePeds.back();
    g_FreePeds.pop_back();
    LOGI("[REMOTE] asignado ped 0x%lx a remoto id=%d name=%s",
         (unsigned long)r.ped.pedPtr, r.id, r.name.c_str());
    return true;
}

// v0.34: re-iterar el pool y agregar peds NUEVOS (no ya asignados ni ya
// presentes en free pool) cuando el free pool baja de kMinFreePeds.
// CALLER must hold g_RemoteMutex.
static void RepopulateFreePool() {
    if (!g_PoolView.valid || g_CjStrip == 0) return;

    auto fresh = IteratePool(g_PoolView, g_CjStrip);
    if (fresh.empty()) return;

    // Set de pedPtrs ya en uso (asignados a remotos o en free pool)
    std::set<uintptr_t> known;
    for (const auto& p : g_FreePeds) known.insert(p.pedPtr);
    for (const auto& kv : g_Remotes) {
        if (kv.second.ped.pedPtr) known.insert(kv.second.ped.pedPtr);
    }

    int added = 0;
    for (const auto& p : fresh) {
        if (known.count(p.pedPtr)) continue;
        g_FreePeds.push_back(p);
        added++;
    }
    LOGI("[POOL] repopulate: +%d nuevos peds, total free=%zu",
         added, g_FreePeds.size());
}

// ---------------------------------------------------------------------
// HOVER demo circulo (DEPRECATED en v0.35: ya no se usa el demo hover.
// El sistema queda IDLE cuando no hay remotos. Mantengo la struct y
// el array fuera del codigo para no perder la idea por si se quiere
// reactivar en debug.)
// ---------------------------------------------------------------------
// struct SlotPos { const char* label; float dx, dy; };
// static const SlotPos kCircle[8] = { ... };

// Escribe pos a la matrix de un ped, validando primero IsPedPointerValid.
// Retorna false si el ped ya no es valido (game lo libero).
static bool WritePedPos(const PedSlot& p, const float pos[3]) {
    bool ok = false;
    InstallScanFaultHandler();
    g_InProtectedScan = 1;
    if (sigsetjmp(g_FaultBuf, 1) == 0) {
        ok = g_Syms.IsPedPointerValid((void*)p.pedPtr);
    }
    g_InProtectedScan = 0;
    if (!ok) { RestoreScanFaultHandler(); return false; }

    g_InProtectedScan = 1;
    if (sigsetjmp(g_FaultBuf, 1) == 0) {
        memcpy((void*)(p.matrixPtr + OFF_MATRIX_POS), pos, 12);
    }
    g_InProtectedScan = 0;
    RestoreScanFaultHandler();
    return true;
}

static void TickAllPositions(float cjX, float cjY, float cjZ) {
    std::lock_guard<std::mutex> lk(g_RemoteMutex);
    int64_t now = NowMs();

    // 1. Remotos: cleanup timeout primero
    std::vector<int> toRemove;
    for (auto& kv : g_Remotes) {
        if (now - kv.second.lastUpdateMs > kRemoteTimeoutMs) {
            LOGI("[REMOTE] timeout id=%d, liberando ped", kv.first);
            FreeRemotePed(kv.second);
            toRemove.push_back(kv.first);
        }
    }
    for (int id : toRemove) g_Remotes.erase(id);

    // 2. v0.35: si NO hay remotos, no hacemos NADA mas. Modo IDLE.
    //    Asi el game maneja los peatones normalmente, no volamos a nadie.
    if (g_Remotes.empty()) {
        return;
    }

    // 3. Contar remotos sin ped asignado: si los hay y free pool esta bajo,
    //    re-poblar (throttle 5s). v0.35: solo si HAY demanda real.
    int waiting = 0;
    for (const auto& kv : g_Remotes) {
        if (!kv.second.ped.pedPtr) waiting++;
    }
    static int64_t lastRepop = 0;
    if (waiting > 0 && g_FreePeds.size() < kMinFreePeds && (now - lastRepop) > 5000) {
        LOGI("[POOL] free=%zu < %zu y hay %d remotos esperando ped, re-poblando...",
             g_FreePeds.size(), kMinFreePeds, waiting);
        RepopulateFreePool();
        lastRepop = now;
    }

    // 4. Asignar peds a remotos sin ped (si hay libres)
    for (auto& kv : g_Remotes) {
        if (!kv.second.ped.pedPtr) TryAssignPedToRemote(kv.second);
    }

    // 5. Escribir pos de cada remoto en su ped (con tether anti-streaming)
    //    v0.34: si remote.pos esta a >kTetherDistance del CJ, no escribir
    //    la pos real (el game soltaria el ped por streaming); colocar el
    //    ped a kTetherTargetDist del CJ en la direccion del remoto.
    for (auto& kv : g_Remotes) {
        RemotePlayer& r = kv.second;
        if (!r.ped.pedPtr) continue;

        float dx = r.pos[0] - cjX;
        float dy = r.pos[1] - cjY;
        float dz = r.pos[2] - cjZ;
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);

        float target[3];
        if (dist > kTetherDistance && dist > 0.01f) {
            float scale = kTetherTargetDist / dist;
            target[0] = cjX + dx * scale;
            target[1] = cjY + dy * scale;
            target[2] = cjZ + dz * scale;
            r.tethered = true;
        } else {
            target[0] = r.pos[0];
            target[1] = r.pos[1];
            target[2] = r.pos[2];
            r.tethered = false;
        }

        if (!WritePedPos(r.ped, target)) {
            LOGW("[REMOTE] ped 0x%lx (id=%d %s) murio, soltando referencia",
                 (unsigned long)r.ped.pedPtr, r.id, r.name.c_str());
            r.ped = PedSlot{};
        }
    }
    // v0.35: ya NO hay demo hover. g_FreePeds queda quieto, el game lo maneja.
}

// ---------------------------------------------------------------------
// Parser del protocolo REAL del server (descubierto en v0.36 con
// el sniffer tools/sniffer.py). El server NO manda PLAYERS|id|name|x|y|z
// como decia el contexto v0.33. Manda un stream INTERLEAVED:
//
//   PLAYERS|<id1>|<name1>                          <- anuncia 1er player
//   POS|<x1>|<y1>|<z1>[|<id2>|<name2>]             <- pos del anterior +
//                                                     opcionalmente anuncia
//                                                     al siguiente
//   POS|<x2>|<y2>|<z2>[|<id3>|<name3>]             ...
//   ...
//   POS|<xN>|<yN>|<zN>                             <- ultimo (sin trailing)
//
// Implementacion: state machine con "pending player" entre mensajes.
//
// Cuando llega PLAYERS|id|name -> setear pending = (id, name)
// Cuando llega POS|x|y|z[|next_id|next_name] ->
//   - aplicar (x,y,z) al pending (si existe)
//   - si trailing: pending = (next_id, next_name); sino: pending limpio
// ---------------------------------------------------------------------

// State machine del parser (solo accedido desde NetThread, no necesita mutex)
static int         g_PendingId   = -1;
static std::string g_PendingName;

// Filtros aplicados a un remoto antes de meterlo a g_Remotes. Devuelve
// true si pasa los filtros y debe procesarse.
static bool RemotePassesFilters(int id, const std::string& name,
                                float x, float y, float z) {
    // Filtro 1: NUNCA hijackearnos a nosotros mismos por id
    if (id == g_MyId) return false;

    // Filtro 2 (v0.34): NUNCA hijackearnos por nombre coincidente
    if (name == PLAYER_NAME) {
        static int64_t lastEchoLog = 0;
        int64_t nn = NowMs();
        if (nn - lastEchoLog > 10000) {
            LOGW("[REMOTE] auto-echo filtrado id=%d name=%s (mismo nombre que local)",
                 id, name.c_str());
            lastEchoLog = nn;
        }
        return false;
    }

    // Filtro 3: pos en rango razonable del mapa de San Andreas
    if (x < -3500 || x > 3500 || y < -3500 || y > 3500 ||
        z < -100  || z > 1100) {
        return false;
    }
    return true;
}

// Aplica una pos al remoto pendiente actual. Hace upsert en g_Remotes.
static void ApplyPosToPending(float x, float y, float z) {
    if (g_PendingId < 0) return;  // sin pending

    if (!RemotePassesFilters(g_PendingId, g_PendingName, x, y, z)) {
        return;
    }

    std::lock_guard<std::mutex> lk(g_RemoteMutex);
    int64_t now = NowMs();
    auto it = g_Remotes.find(g_PendingId);
    bool isNew = (it == g_Remotes.end());
    RemotePlayer& r = g_Remotes[g_PendingId];
    r.id   = g_PendingId;
    r.name = g_PendingName;
    r.pos[0] = x; r.pos[1] = y; r.pos[2] = z;
    r.lastUpdateMs = now;

    if (isNew) {
        LOGI("[REMOTE] NUEVO id=%d name=%s pos=(%.1f,%.1f,%.1f)",
             g_PendingId, g_PendingName.c_str(), x, y, z);
    }
}

// Handler de PLAYERS|id|name (siempre 3 tokens en el protocolo real)
static void HandlePlayersAnnounce(const std::vector<std::string>& parts) {
    if (parts.size() < 3) {
        LOGW("[PLAYERS] formato raro: %zu campos", parts.size());
        return;
    }
    // Si quedaba un pending sin pos, lo descartamos (server bug o split raro)
    g_PendingId   = atoi(parts[1].c_str());
    g_PendingName = parts[2];
}

// Handler de POS|x|y|z[|next_id|next_name]
// - los 3 numeros son la pos del player anunciado anteriormente (pending)
// - los 2 trailing fields (si existen) anuncian al SIGUIENTE player
static void HandlePosUpdate(const std::vector<std::string>& parts) {
    if (parts.size() < 4) {
        // POS sin x/y/z? Probablemente garbage del server. Reset.
        g_PendingId = -1; g_PendingName.clear();
        return;
    }
    float x = (float)atof(parts[1].c_str());
    float y = (float)atof(parts[2].c_str());
    float z = (float)atof(parts[3].c_str());

    // Aplicar pos al pending actual
    ApplyPosToPending(x, y, z);

    // Si hay trailing (id+name del siguiente), setear nuevo pending
    if (parts.size() >= 6) {
        g_PendingId   = atoi(parts[4].c_str());
        g_PendingName = parts[5];
    } else {
        // Fin de frame, sin mas players
        g_PendingId = -1; g_PendingName.clear();
    }
}

// ---------------------------------------------------------------------
// Estado de red
// ---------------------------------------------------------------------
static int g_Socket = -1;
static volatile bool g_Running = false;
static pthread_t g_NetThread;
static pthread_t g_AttackThread;
static std::string g_RecvBuffer;
static std::mutex g_SendMutex;
static float g_LocalX = 2495.0f;
static float g_LocalY = -1666.0f;
static float g_LocalZ = 13.3f;

static std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out; size_t start = 0, pos;
    while ((pos = s.find(sep, start)) != std::string::npos) {
        out.emplace_back(s.substr(start, pos - start)); start = pos + 1;
    }
    out.emplace_back(s.substr(start)); return out;
}

static bool SendLine(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_SendMutex);
    if (g_Socket < 0) return false;
    std::string out = line + "\n";
    ssize_t n = send(g_Socket, out.data(), out.size(), MSG_NOSIGNAL);
    if (n < 0) { LOGE("send fallo: %s", strerror(errno)); return false; }
    return true;
}

static void HandleMessage(const std::string& line) {
    if (line.empty()) return;
    auto parts = Split(line, '|');
    const std::string& cmd = parts[0];

    if (cmd == "ID" && parts.size() >= 2) {
        g_MyId = atoi(parts[1].c_str());
        LOGI("======== ID ASIGNADO: %d ========", g_MyId);
    }
    else if (cmd == "PLAYERS") {
        // v0.36: anuncia al 1er player de un frame, formato real
        // PLAYERS|<id>|<name>
        HandlePlayersAnnounce(parts);
    }
    else if (cmd == "POS") {
        // v0.36: contiene la pos del player anunciado anteriormente y
        // opcionalmente anuncia al siguiente. Formato real:
        //   POS|<x>|<y>|<z>            (ultimo del frame)
        //   POS|<x>|<y>|<z>|<id>|<nm>  (no es ultimo)
        HandlePosUpdate(parts);
    }
    else if (cmd == "QUIT" && parts.size() >= 2) {
        int id = atoi(parts[1].c_str());
        std::lock_guard<std::mutex> lk(g_RemoteMutex);
        auto it = g_Remotes.find(id);
        if (it != g_Remotes.end()) {
            LOGI("[REMOTE] QUIT id=%d, liberando ped", id);
            FreeRemotePed(it->second);
            g_Remotes.erase(it);
        }
        // Reset del parser pending si era este id
        if (g_PendingId == id) { g_PendingId = -1; g_PendingName.clear(); }
    }
    else if (cmd == "CHAT" && parts.size() >= 2) {
        LOGI("<- CHAT: %s", parts[1].c_str());
    }
    else if (cmd == "PING") {}
    else {
        // Loggear desconocidos solo la primera vez
        static std::set<std::string> seen;
        if (seen.insert(cmd).second) {
            LOGW("<- comando nuevo (no manejado): %s (line=%s)",
                 cmd.c_str(), line.c_str());
        }
    }
}

// ---------------------------------------------------------------------
// Attack thread: setup + lock loop
// ---------------------------------------------------------------------
static void* AttackThreadFn(void*) {
    LOGI("[ATK] thread iniciado, espera %ds para que cargue todo...", kInitialWaitS);
    for (int i = 0; i < kInitialWaitS && g_Running; i++) usleep(1000 * 1000);
    if (!g_Running) return nullptr;

    g_GTASABase = FindLibBase("libGTASA.so");
    if (!ResolveSymbols()) {
        LOGE("[ATK] simbolos fallaron, ABORT");
        return nullptr;
    }

    float cjX=0, cjY=0, cjZ=0;
    int retries = 0;
    while (!ReadLocalPlayerPos(&cjX, &cjY, &cjZ) && retries < 20) {
        usleep(500 * 1000); retries++;
    }
    if (retries >= 20) { LOGW("[ATK] no pos CJ"); return nullptr; }
    LOGI("[ATK] CJ en (%.1f, %.1f, %.1f)", cjX, cjY, cjZ);

    CPoolView pool = LoadCPedPool();
    if (!pool.valid) { LOGE("[ATK] pool invalido"); return nullptr; }

    uintptr_t cjPed = 0;
    InstallScanFaultHandler();
    g_InProtectedScan = 1;
    if (sigsetjmp(g_FaultBuf, 1) == 0) cjPed = (uintptr_t)g_Syms.FindPlayerPed(0);
    g_InProtectedScan = 0;
    RestoreScanFaultHandler();
    uintptr_t cjStrip = StripTag(cjPed);
    LOGI("[ATK] CJ ped = 0x%lx (strip=0x%lx)", (unsigned long)cjPed, (unsigned long)cjStrip);

    auto peds = IteratePool(pool, cjStrip);
    if (peds.empty()) { LOGW("[ATK] sin peds disponibles"); return nullptr; }

    {
        std::lock_guard<std::mutex> lk(g_RemoteMutex);
        g_FreePeds = peds;
        // v0.34: guardar pool y CJ ptr globalmente para repopulate
        g_PoolView = pool;
        g_CjStrip  = cjStrip;
    }

    LOGI("================================================================");
    LOGI("[ATK] >>> SISTEMA MULTIPLAYER LISTO (modo QUIETDEMO) <<<");
    LOGI("[ATK] %zu peds en cache. Sin remotos: cliente IDLE (no toca peds).", peds.size());
    LOGI("[ATK] Cuando llegue un remoto: hijack + tether anti-streaming.");
    LOGI("================================================================");

    int64_t lastLog = NowMs();
    while (g_Running) {
        float nx, ny, nz;
        if (ReadLocalPlayerPos(&nx, &ny, &nz)) {
            cjX = nx; cjY = ny; cjZ = nz;
            g_LocalX = nx; g_LocalY = ny; g_LocalZ = nz;
        }

        TickAllPositions(cjX, cjY, cjZ);

        int64_t now = NowMs();
        if (now - lastLog > 15000) {
            std::lock_guard<std::mutex> lk(g_RemoteMutex);
            int withPed = 0, tethered = 0;
            for (auto& kv : g_Remotes) {
                if (kv.second.ped.pedPtr) withPed++;
                if (kv.second.tethered)   tethered++;
            }
            const char* mode = g_Remotes.empty() ? "IDLE" : "ACTIVE";
            LOGI("[ATK] [state-%s] %zu remotos (%d con ped, %d tethered), %zu peds en cache, CJ=(%.1f,%.1f,%.1f)",
                 mode, g_Remotes.size(), withPed, tethered, g_FreePeds.size(), cjX, cjY, cjZ);
            lastLog = now;
        }
        usleep(kHoverTickMs * 1000);
    }
    return nullptr;
}

// ---------------------------------------------------------------------
// Net thread
// ---------------------------------------------------------------------
static void* NetThreadFn(void*) {
    LOGI("[net] hilo iniciado");
    char buf[4096];
    int64_t lastHeartbeat = NowMs();
    if (g_GTASABase == 0) {
        for (int i = 0; i < 30 && g_GTASABase == 0; i++) {
            g_GTASABase = FindLibBase("libGTASA.so");
            if (g_GTASABase) break;
            usleep(200 * 1000);
        }
    }
    while (g_Running) {
        ssize_t n = recv(g_Socket, buf, sizeof(buf), 0);
        if (n > 0) {
            g_RecvBuffer.append(buf, (size_t)n);
            size_t pos;
            while ((pos = g_RecvBuffer.find('\n')) != std::string::npos) {
                std::string line = g_RecvBuffer.substr(0, pos);
                g_RecvBuffer.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                HandleMessage(line);
            }
        }
        else if (n == 0) { LOGE("[net] server cerro"); break; }
        else if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(20 * 1000); }
        else { LOGE("recv: %s", strerror(errno)); break; }

        int64_t now = NowMs();
        if (now - lastHeartbeat >= 1000) {     // 1Hz POS heartbeat
            float rx, ry, rz;
            if (ReadLocalPlayerPos(&rx, &ry, &rz)) {
                g_LocalX = rx; g_LocalY = ry; g_LocalZ = rz;
            }
            char hb[128];
            snprintf(hb, sizeof(hb), "POS|%.2f|%.2f|%.2f", g_LocalX, g_LocalY, g_LocalZ);
            SendLine(hb);
            lastHeartbeat = now;
        }
    }
    return nullptr;
}

static bool ConnectTCP() {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char portStr[8]; snprintf(portStr, sizeof(portStr), "%u", SERVER_PORT);
    int rc = getaddrinfo(SERVER_HOST, portStr, &hints, &res);
    if (rc != 0 || !res) { LOGE("getaddrinfo: %s", gai_strerror(rc)); return false; }
    char ipStr[64];
    sockaddr_in* sin = (sockaddr_in*)res->ai_addr;
    inet_ntop(AF_INET, &sin->sin_addr, ipStr, sizeof(ipStr));
    LOGI("[net] resuelto %s -> %s:%u", SERVER_HOST, ipStr, SERVER_PORT);
    g_Socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_Socket < 0) { LOGE("socket: %s", strerror(errno)); freeaddrinfo(res); return false; }
    timeval tv{10, 0};
    setsockopt(g_Socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(g_Socket, res->ai_addr, res->ai_addrlen) < 0) {
        LOGE("connect: %s", strerror(errno));
        close(g_Socket); g_Socket = -1; freeaddrinfo(res); return false;
    }
    freeaddrinfo(res);
    int yes = 1;
    setsockopt(g_Socket, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
    setsockopt(g_Socket, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    int fl = fcntl(g_Socket, F_GETFL, 0);
    fcntl(g_Socket, F_SETFL, fl | O_NONBLOCK);
    LOGI("======== CONECTADO a %s:%u ========", SERVER_HOST, SERVER_PORT);
    return true;
}

// ---------------------------------------------------------------------
// AML entrypoints
// ---------------------------------------------------------------------
extern "C" void OnModLoad(void*) {
    LOGI("===========================================");
    LOGI(" %s v%s by %s", modinfo.szName, modinfo.szVersion, modinfo.szAuthor);
    LOGI(" Hito 3 COMPLETO + Hito 4: jugadores remotos reales");
    LOGI(" Server: %s:%u   Nombre: %s", SERVER_HOST, SERVER_PORT, PLAYER_NAME);
    LOGI("===========================================");

    g_GTASABase = FindLibBase("libGTASA.so");
    if (g_GTASABase) LOGI("[mem] libGTASA.so base = 0x%lx", g_GTASABase);

    if (!ConnectTCP()) { LOGE("No se pudo conectar"); return; }
    std::string j = std::string("JOIN|") + PLAYER_NAME + "\n";
    send(g_Socket, j.data(), j.size(), MSG_NOSIGNAL);
    LOGI("-> JOIN|%s", PLAYER_NAME);

    g_Running = true;
    pthread_create(&g_NetThread,    nullptr, NetThreadFn,    nullptr);
    pthread_create(&g_AttackThread, nullptr, AttackThreadFn, nullptr);

    LOGI("Cliente listo. En %ds arranca el sistema multiplayer.", kInitialWaitS);
    LOGI("Sin remotos: cliente IDLE. Con remotos: aparecen como peatones.");
}

extern "C" void OnModUnload() {
    LOGI("Descargando mod...");
    g_Running = false;
    if (g_Socket >= 0) { shutdown(g_Socket, SHUT_RDWR); close(g_Socket); g_Socket = -1; }
    if (g_NetThread)    { pthread_join(g_NetThread,    nullptr); g_NetThread    = 0; }
    if (g_AttackThread) { pthread_join(g_AttackThread, nullptr); g_AttackThread = 0; }
}
