// Species Unknown - 5 player mod
//
// Runs on a background thread inside SpeciesUnknown-Win64-Shipping.exe (UE 5.7, Blueprint-only game).
// It locates the engine's global object array and name pool by scanning the exe's data sections,
// then:
//   * raises BP_MyGameInstance_C.playerLobbyCapacity (lobby / session size) from 4 to 5
//   * patches a handful of hard-coded "4 players" constants in Blueprint bytecode
//   * makes the shuttle spawn a 5th seat (centered between the two front seats)
//   * if the 5th seat could not be added, lets the shuttle launch when min(players, 4) are seated
//
// Every bytecode patch is verified byte-for-byte against the bytecode of the build this mod was
// written for before anything is written. If the game updates and the bytecode changes, that patch
// is skipped and logged instead of corrupting anything.

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

static constexpr int kTargetPlayers = 5;
static constexpr int kGIProbeSize = 89;  // BP_MyGameInstance_C::"OWNER Get Online Nb Players in Lobby"

// ---------------------------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------------------------
static FILE* g_log = nullptr;

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------------------------------------------------------------------------------------------
// Fault-tolerant memory access
// ---------------------------------------------------------------------------------------------
static bool SafeCopy(void* dst, uintptr_t src, size_t n)
{
    __try { memcpy(dst, (const void*)src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static volatile long g_writeCount = 0;   // every patch write goes through SafeWrite
static bool g_quiet = false;              // re-verification pass: don't repeat the per-step log lines

static bool SafeWrite(uintptr_t dst, const void* src, size_t n)
{
    InterlockedIncrement(&g_writeCount);
    __try { memcpy((void*)dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

template <class T> static T R(uintptr_t a, T def = T())
{
    T v;
    return SafeCopy(&v, a, sizeof(T)) ? v : def;
}

static bool Plausible(uintptr_t p) { return p >= 0x10000 && p < 0x7FFFFFFFFFFFull && (p & 7) == 0; }

// ---------------------------------------------------------------------------------------------
// Engine globals discovered at runtime
// ---------------------------------------------------------------------------------------------
static uintptr_t g_nameBlocks = 0;   // &FNamePool::Entries.Blocks[0]
static uintptr_t g_objObjects = 0;   // &GUObjectArray.ObjObjects (FChunkedFixedUObjectArray)
static int       g_itemStride = 0;   // sizeof(FUObjectItem)
static int       g_itemObjOff = 0;   // offset of the UObject* inside FUObjectItem (8 in UE 5.7)

// UObjectBase layout (stable across UE5)
static constexpr uintptr_t kObjFlags = 0x08, kObjIndex = 0x0C, kObjClass = 0x10, kObjName = 0x18, kObjOuter = 0x20;
static constexpr uint32_t RF_ClassDefaultObject = 0x10, RF_ArchetypeObject = 0x20, RF_NeedLoad = 0x400;

// Detected later
static uintptr_t g_scriptOff = 0;       // UStruct::Script (TArray<uint8>)
static uintptr_t g_fieldNameOff = 0;    // FField::NamePrivate
static uintptr_t g_fieldNextOff = 0;    // FField::Next

static std::unordered_map<uint32_t, std::string> g_nameCache;

static bool DecodeName(uint32_t id, std::string& out)
{
    auto it = g_nameCache.find(id);
    if (it != g_nameCache.end()) { out = it->second; return true; }
    uint32_t block = id >> 16, off = (id & 0xFFFF) * 2;
    if (block >= 8192) return false;
    uintptr_t b = R<uintptr_t>(g_nameBlocks + block * 8);
    if (!Plausible(b & ~7ull)) return false;
    uint16_t hdr;
    if (!SafeCopy(&hdr, b + off, 2)) return false;
    bool wide = hdr & 1;
    uint32_t len = hdr >> 6;
    if (len == 0 || len > 1023) return false;
    if (!wide) {
        char buf[1024];
        if (!SafeCopy(buf, b + off + 2, len)) return false;
        out.assign(buf, len);
    } else {
        wchar_t wb[1024];
        if (!SafeCopy(wb, b + off + 2, len * 2)) return false;
        out.clear();
        for (uint32_t i = 0; i < len; i++) out.push_back(wb[i] < 128 ? (char)wb[i] : '?');
    }
    g_nameCache.emplace(id, out);
    return true;
}

// Name of an FName stored at address a (ComparisonIndex + Number); empty if invalid.
static std::string NameAt(uintptr_t a)
{
    uint32_t v[2];
    if (!SafeCopy(v, a, 8)) return {};
    std::string s;
    if (!DecodeName(v[0], s)) return {};
    if (v[1] != 0) s += "_" + std::to_string(v[1] - 1);
    return s;
}

// FNames are case-insensitive and keep the casing first registered (e.g. "Amount", "min").
static std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static bool IEq(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; }

static std::string ObjName(uintptr_t o) { return Plausible(o) ? NameAt(o + kObjName) : std::string(); }
static uintptr_t   ObjClass(uintptr_t o) { return R<uintptr_t>(o + kObjClass); }
static uintptr_t   ObjOuter(uintptr_t o) { return R<uintptr_t>(o + kObjOuter); }
static uint32_t    ObjFlags(uintptr_t o) { return R<uint32_t>(o + kObjFlags); }

static int ObjCount() { return R<int32_t>(g_objObjects + 20); }

static uintptr_t ObjAt(int i)
{
    uintptr_t objects = R<uintptr_t>(g_objObjects);
    uintptr_t chunk = R<uintptr_t>(objects + (uintptr_t)(i >> 16) * 8);
    if (!Plausible(chunk)) return 0;
    return R<uintptr_t>(chunk + (uintptr_t)(i & 0xFFFF) * g_itemStride + g_itemObjOff);
}

// An object pointer we remembered is only safe to use while the engine's object table still holds it
// at its index (classes can be unloaded and reloaded, sometimes at the very same address).
static bool IsLive(uintptr_t o)
{
    if (!Plausible(o)) return false;
    int32_t idx = R<int32_t>(o + kObjIndex, -1);
    return idx >= 0 && idx < ObjCount() && ObjAt(idx) == o;
}

// ---------------------------------------------------------------------------------------------
// Locating FNamePool and GUObjectArray inside the exe's writable sections
// ---------------------------------------------------------------------------------------------
struct Range { uintptr_t begin, end; };

static std::vector<Range> WritableSections()
{
    std::vector<Range> out;
    auto base = (uintptr_t)GetModuleHandleW(nullptr);
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (sec->Characteristics & IMAGE_SCN_MEM_WRITE)
            out.push_back({ base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize });
    }
    return out;
}

static bool EntryIs(uintptr_t p, const char* s, uintptr_t* next)
{
    uint16_t hdr;
    if (!SafeCopy(&hdr, p, 2)) return false;
    size_t len = strlen(s);
    if ((hdr & 1) || (hdr >> 6) != len) return false;
    char buf[64];
    if (!SafeCopy(buf, p + 2, len) || memcmp(buf, s, len) != 0) return false;
    if (next) *next = p + ((2 + len + 1) & ~1ull);
    return true;
}

static bool FindNamePool()
{
    for (auto& r : WritableSections()) {
        for (uintptr_t a = r.begin; a + 8 <= r.end; a += 8) {
            uintptr_t blk = R<uintptr_t>(a);
            if (!Plausible(blk)) continue;
            uintptr_t n2;
            if (!EntryIs(blk, "None", &n2) || !EntryIs(n2, "ByteProperty", nullptr)) continue;
            uint32_t curBlock = R<uint32_t>(a - 8);
            if (curBlock >= 8192 || !Plausible(R<uintptr_t>(a + curBlock * 8))) continue;
            g_nameBlocks = a;
            Log("FNamePool blocks at exe+0x%llx (current block %u)",
                (unsigned long long)(a - (uintptr_t)GetModuleHandleW(nullptr)), curBlock);
            return true;
        }
    }
    return false;
}

static bool FindObjectArray()
{
    for (auto& r : WritableSections()) {
        for (uintptr_t a = r.begin; a + 32 <= r.end; a += 8) {
            uintptr_t objects = R<uintptr_t>(a);
            if (!Plausible(objects)) continue;
            int32_t maxEl = R<int32_t>(a + 16), numEl = R<int32_t>(a + 20);
            int32_t maxCh = R<int32_t>(a + 24), numCh = R<int32_t>(a + 28);
            if (maxEl < 0x10000 || maxEl > 0x8000000 || numEl < 0x1000 || numEl > maxEl) continue;
            // UE: MaxChunks = MaxElements / NumElementsPerChunk + 1 (older/other builds: rounded up)
            if (maxCh != maxEl / 0x10000 + 1 && maxCh != (maxEl + 0xFFFF) / 0x10000) continue;
            if (numCh < (numEl + 0xFFFF) / 0x10000 || numCh > maxCh) continue;
            uintptr_t chunk0 = R<uintptr_t>(objects);
            if (!Plausible(chunk0)) continue;
            for (int stride : { 24, 16, 32, 40 }) {
                for (int objOff : { 0, 8 }) {
                    bool ok = true;
                    for (int i = 0; i < 4 && ok; i++) {
                        uintptr_t o = R<uintptr_t>(chunk0 + (uintptr_t)i * stride + objOff);
                        ok = Plausible(o) && R<int32_t>(o + kObjIndex) == i;
                    }
                    if (ok) {
                        g_objObjects = a;
                        g_itemStride = stride;
                        g_itemObjOff = objOff;
                        Log("GUObjectArray.ObjObjects at exe+0x%llx, %d objects, item stride %d, object at +%d",
                            (unsigned long long)(a - (uintptr_t)GetModuleHandleW(nullptr)), numEl, stride, objOff);
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Reflection helpers (UStruct / FField)
// ---------------------------------------------------------------------------------------------
struct ScriptArr { uintptr_t data; int32_t num; int32_t max; };

static bool GetScript(uintptr_t fn, ScriptArr& s) { return g_scriptOff && SafeCopy(&s, fn + g_scriptOff, sizeof(s)); }

static bool DetectScriptOffset(uintptr_t fn, int expectedSize)
{
    for (uintptr_t off = 0x30; off <= 0x100; off += 8) {
        ScriptArr s;
        if (!SafeCopy(&s, fn + off, sizeof(s))) continue;
        if (s.num == expectedSize && s.max >= s.num && Plausible(s.data & ~7ull) &&
            R<uint8_t>(s.data + expectedSize - 1, 0) == 0x53 /*EX_EndOfScript*/) {
            g_scriptOff = off;
            Log("UStruct::Script offset = 0x%llx", (unsigned long long)off);
            return true;
        }
    }
    return false;
}

static uintptr_t ChildProps(uintptr_t ustruct) { return R<uintptr_t>(ustruct + g_scriptOff - 0x10); }

static bool LooksLikeIdent(const std::string& s)
{
    if (s.empty() || s.size() > 200) return false;
    for (char c : s) if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) return false;
    return true;
}

static bool DetectFieldLayout(uintptr_t ustruct)
{
    uintptr_t head = ChildProps(ustruct);
    if (!Plausible(head)) return false;
    for (uintptr_t nameOff : { 0x20, 0x28, 0x18, 0x30 }) {
        uintptr_t nextOff = nameOff - 8;
        int good = 0;
        uintptr_t f = head;
        for (int i = 0; i < 8 && Plausible(f); i++) {
            if (!LooksLikeIdent(NameAt(f + nameOff))) { good = -1; break; }
            good++;
            f = R<uintptr_t>(f + nextOff);
        }
        if (good >= 3 || (good >= 1 && f == 0)) {
            g_fieldNameOff = nameOff;
            g_fieldNextOff = nextOff;
            Log("FField::NamePrivate offset = 0x%llx", (unsigned long long)nameOff);
            return true;
        }
    }
    return false;
}

struct PropInfo { uintptr_t prop = 0; int32_t arrayDim = 0, elementSize = 0, offset = -1; };

static PropInfo FindProperty(uintptr_t ustruct, const char* name)
{
    PropInfo pi;
    if (!g_fieldNameOff) return pi;
    uintptr_t f = ChildProps(ustruct);
    for (int i = 0; i < 4000 && Plausible(f); i++) {
        if (IEq(NameAt(f + g_fieldNameOff), name)) {
            pi.prop = f;
            pi.arrayDim = R<int32_t>(f + g_fieldNameOff + 0x10);
            pi.elementSize = R<int32_t>(f + g_fieldNameOff + 0x14);
            pi.offset = R<int32_t>(f + g_fieldNameOff + 0x24, -1);
            return pi;
        }
        f = R<uintptr_t>(f + g_fieldNextOff);
    }
    return pi;
}

// ---------------------------------------------------------------------------------------------
// Object index (rebuilt by scanning GUObjectArray)
// ---------------------------------------------------------------------------------------------
static const char* kTargetClasses[] = {
    "BP_MyGameInstance_C",
    "BP_LobbyGameMode_C",
    "BP_Character_Lobby_C",
    "WBP_ScorePage_C",
    "BP_PlayerShip_C",
    "BP_MyPlayerController_C",
    "WBP_PauseMenu_CrewMembers_C",
    "WBP_PauseMenu_PlayerInfos_C",
    "WBP_PauseMenu_PlayerActions_C",
    "BP_Seat_C",
    // player-count scaling (scaling.inc)
    "Balancing_FunctionLibrary_C", "BP_ConsoleKeypass_C", "BP_BaseMonster_C", "BP_BigRobot_C", "BP_Monster_Eye_C",
    "BP_Monster_Poulpi_C", "BP_Monster_ZombieMaster_C", "BP_SnowMonster_Spawner_C", "BP_SnowMonster_Spawner_Electrical_C",
    "BP_GeneticsLab_Manager_C", "BP_Laboratory_PuzzleContainer_C", "BP_ERAccess_RiddleSystem_C", "BP_3DPrinterTerminal_C",
};

// Engine classes / libraries used to build the 5th crew row at runtime.
static const char* kEngineClasses[] = {
    "CanvasPanel", "ScaleBox", "Overlay", "Widget", "CanvasPanelSlot", "OverlaySlot", "ScaleBoxSlot", "VerticalBoxSlot",
    "Actor",
};
static const char* kEngineFuncOwners[] = {
    "WidgetBlueprintLibrary", "GameplayStatics", "PanelWidget", "CanvasPanel", "VerticalBoxSlot", "Actor", "Widget",
};
static std::unordered_map<std::string, uintptr_t> g_engineClasses;   // lower-case name -> UClass*
static std::unordered_map<std::string, uintptr_t> g_engineCDOs;      // lower-case name -> CDO

static std::unordered_map<std::string, std::string> g_targetCanon;  // lower-case name -> canonical name
static std::unordered_map<std::string, uintptr_t> g_classes;                                     // class name -> UClass*
static std::unordered_map<uintptr_t, std::unordered_map<std::string, uintptr_t>> g_funcs;       // outer -> name -> UFunction*
static uintptr_t g_objectCDO = 0;   // Default__Object (used to find ProcessEvent's vtable slot)
static uintptr_t g_kismetMath = 0;
static uintptr_t g_kismetArray = 0;
static std::vector<uintptr_t> g_gameInstances;  // includes the CDO
static int g_shipInstances = 0;
// 5-player mode is only for private lobbies: the host's GameInstance.isPlayerLobbyPrivate decides.
static volatile bool g_isPrivate = false;
static int g_scanned = 0;

static void ProcessObject(uintptr_t o)
{
    if (!Plausible(o)) return;
    uintptr_t cls = ObjClass(o);
    if (!Plausible(cls)) return;
    std::string clsName = ObjName(cls);
    if (clsName.empty()) return;

    if (IEq(clsName, "Function")) {
        uintptr_t outer = ObjOuter(o);
        std::string outerName = ObjName(outer);
        bool engineOwner = false;
        for (const char* e : kEngineFuncOwners) engineOwner |= IEq(outerName, e);
        if (engineOwner || IEq(outerName, "KismetMathLibrary") || IEq(outerName, "KismetArrayLibrary") ||
            g_targetCanon.count(Lower(outerName)))
            g_funcs[outer][Lower(ObjName(o))] = o;
        return;
    }
    if (IEq(clsName, "BlueprintGeneratedClass") || IEq(clsName, "WidgetBlueprintGeneratedClass")) {
        auto t = g_targetCanon.find(Lower(ObjName(o)));
        if (t != g_targetCanon.end()) g_classes[t->second] = o;
        return;
    }
    if (IEq(clsName, "Object") && (ObjFlags(o) & RF_ClassDefaultObject)) { g_objectCDO = o; return; }
    if (IEq(clsName, "Class")) {
        if (IEq(ObjName(o), "KismetMathLibrary")) g_kismetMath = o;
        if (IEq(ObjName(o), "KismetArrayLibrary")) g_kismetArray = o;
        std::string n = ObjName(o);
        for (const char* e : kEngineClasses) if (IEq(n, e)) g_engineClasses[Lower(n)] = o;
        for (const char* e : kEngineFuncOwners) if (IEq(n, e)) g_engineClasses[Lower(n)] = o;
        return;
    }
    if (IEq(clsName, "WidgetBlueprintLibrary") || IEq(clsName, "GameplayStatics")) {
        if (ObjFlags(o) & RF_ClassDefaultObject) g_engineCDOs[Lower(clsName)] = o;
        return;
    }
    if (IEq(clsName, "BP_MyGameInstance_C")) { g_gameInstances.push_back(o); return; }
    if (IEq(clsName, "BP_PlayerShip_C") && !(ObjFlags(o) & (RF_ClassDefaultObject | RF_ArchetypeObject))) g_shipInstances++;
}

static void FullScan()
{
    g_engineClasses.clear();
    g_engineCDOs.clear();
    g_classes.clear();
    g_funcs.clear();
    g_gameInstances.clear();
    g_shipInstances = 0;
    int n = ObjCount();
    for (int i = 0; i < n; i++) ProcessObject(ObjAt(i));
    g_scanned = n;
}

static void IncrementalScan()
{
    int n = ObjCount();
    for (int i = g_scanned; i < n; i++) ProcessObject(ObjAt(i));
    if (n > g_scanned) g_scanned = n;
}

static uintptr_t Func(uintptr_t outer, const char* name)
{
    auto it = g_funcs.find(outer);
    if (it == g_funcs.end()) return 0;
    auto jt = it->second.find(Lower(name));
    if (jt == it->second.end()) return 0;
    if (!IsLive(jt->second) || ObjOuter(jt->second) != outer) return 0;   // stale entry from an unloaded class
    return jt->second;
}

// Fingerprint of a class's loaded functions and their bytecode buffers: changes when the class is
// reloaded (even if the class object itself reappears at the same address).
static uint64_t ClassFingerprint(uintptr_t cls)
{
    uint64_t h = 1469598103934665603ull;
    auto it = g_funcs.find(cls);
    if (it == g_funcs.end()) return 0;
    for (auto& kv : it->second) {
        uintptr_t fn = kv.second;
        if (!IsLive(fn) || ObjOuter(fn) != cls) continue;
        ScriptArr sc{};
        if (g_scriptOff) SafeCopy(&sc, fn + g_scriptOff, sizeof(sc));
        h ^= (uint64_t)fn * 0x100000001b3ull;
        h ^= (uint64_t)sc.data * 0x9E3779B97F4A7C15ull;
        h = (h << 7) | (h >> 57);
    }
    return h;
}

static uintptr_t MathFunc(const char* name) { return g_kismetMath ? Func(g_kismetMath, name) : 0; }
static uintptr_t ArrayFunc(const char* name) { return g_kismetArray ? Func(g_kismetArray, name) : 0; }

// ---------------------------------------------------------------------------------------------
// Bytecode patch primitives
// ---------------------------------------------------------------------------------------------
enum Result { kRetry = 0, kDone = 1, kFail = -1 };

static const char* ResultStr(int r) { return r == kDone ? "ok" : r == kFail ? "FAILED" : "pending"; }

// Is the function's bytecode loaded and of the size we expect?
static int ReadyScript(uintptr_t fn, int expectedSize, ScriptArr& s)
{
    if (!IsLive(fn)) return kRetry;
    if (!g_scriptOff && !DetectScriptOffset(fn, expectedSize)) return kRetry;
    if (ObjFlags(fn) & RF_NeedLoad) return kRetry;
    if (!GetScript(fn, s) || s.num == 0) return kRetry;
    if (s.num != expectedSize) return kFail;
    if (R<uint8_t>(s.data + expectedSize - 1, 0) != 0x53) return kRetry;
    return kDone;
}

static void PutPtr(uint8_t* p, uintptr_t v) { memcpy(p, &v, 8); }
static uintptr_t GetPtr(const uint8_t* p) { uintptr_t v; memcpy(&v, p, 8); return v; }
static int32_t GetI32(const uint8_t* p) { int32_t v; memcpy(&v, p, 4); return v; }
static int memmem_like(const uint8_t* hay, int n, const char* needle)
{
    int m = (int)strlen(needle);
    for (int i = 0; i + m <= n; i++) if (memcmp(hay + i, needle, m) == 0) return i;
    return -1;
}

// Replace "EX_IntConst oldV, EX_EndFunctionParms" inside one statement.
static int PatchIntConst(uintptr_t fn, int size, int stmt, int len, int32_t oldV, int32_t newV)
{
    ScriptArr s;
    int r = ReadyScript(fn, size, s);
    if (r != kDone) return r;
    std::vector<uint8_t> b(len);
    if (!SafeCopy(b.data(), s.data + stmt, len)) return kRetry;
    int hitsOld = 0, hitsNew = 0, at = -1;
    for (int i = 0; i + 6 <= len; i++) {
        if (b[i] != 0x1D || b[i + 5] != 0x16) continue;
        int32_t v = GetI32(&b[i + 1]);
        if (v == oldV) { hitsOld++; at = i; }
        else if (v == newV) hitsNew++;
    }
    if (hitsOld == 0 && hitsNew == 1) return kDone;
    if (hitsOld != 1 || hitsNew != 0) return kFail;
    return SafeWrite(s.data + stmt + at + 1, &newV, 4) ? kDone : kFail;
}

// "Bool = KismetMathLibrary::NotEqual_IntInt(Local, 3)" -> "... Less_IntInt(Local, 3)"
// Used on Blueprint "switch on int" nodes so the case for 3 also catches 4 (the 5th player).
static int PatchSwapNotEqualToLess(uintptr_t fn, int size, int stmt)
{
    ScriptArr s;
    int r = ReadyScript(fn, size, s);
    if (r != kDone) return r;
    uintptr_t lessFn = MathFunc("Less_IntInt");
    if (!lessFn) return kRetry;
    uint8_t b[34];
    if (!SafeCopy(b, s.data + stmt, sizeof(b))) return kRetry;
    if (b[0] != 0x14 || b[1] != 0x00 || b[10] != 0x68 || b[19] != 0x00 || b[28] != 0x1D || b[33] != 0x16 ||
        GetI32(&b[29]) != 3)
        return kFail;
    uintptr_t cur = GetPtr(&b[11]);
    if (cur == lessFn) return kDone;
    if (!IEq(ObjName(cur), "NotEqual_IntInt")) return kFail;
    return SafeWrite(s.data + stmt + 11, &lessFn, 8) ? kDone : kFail;
}

// KismetArrayLibrary::Array_Set(..., bSizeToFit = false) -> true, so per-player arrays grow for player 5.
static int PatchArraySetSizeToFit(uintptr_t fn, int size, int stmt, int len)
{
    ScriptArr s;
    int r = ReadyScript(fn, size, s);
    if (r != kDone) return r;
    std::vector<uint8_t> b(len);
    if (!SafeCopy(b.data(), s.data + stmt, len)) return kRetry;
    if (b[0] != 0x19 || b[1] != 0x20 || b[22] != 0x1C || b[len - 1] != 0x16) return kFail;
    if (!IEq(ObjName(GetPtr(&b[23])), "Array_Set")) return kFail;
    if (b[len - 2] == 0x27) return kDone;
    if (b[len - 2] != 0x28) return kFail;
    uint8_t t = 0x27;
    return SafeWrite(s.data + stmt + len - 2, &t, 1) ? kDone : kFail;
}

// Property references inside bytecode are filled in by the engine *after* the bytecode itself is loaded
// (class members are resolved when the class is linked). The engine writes them at fixed offsets, so a
// rewrite that moves bytes around must only happen once every reference in the region is resolved,
// otherwise the engine later overwrites the rewritten code. These helpers check that.
static bool PropIs(const uint8_t* p, const char* expected)
{
    uintptr_t f = GetPtr(p);
    return Plausible(f) && IEq(NameAt(f + g_fieldNameOff), expected);
}
static bool PropResolved(const uint8_t* p)
{
    uintptr_t f = GetPtr(p);
    return Plausible(f) && LooksLikeIdent(NameAt(f + g_fieldNameOff));
}

// ---------------------------------------------------------------------------------------------
// Lobby creation: open member slot 4
// ---------------------------------------------------------------------------------------------
// BP_MyGameInstance_C::"Make Player Lobby Transaction" marks the lobby member slots as available with
// four unrolled calls (slot 0 = false for the host, slots 1..3 = true). Region [1566, 1818) is:
//   @1566 VariantDataBP_9.Type = 9        (29 bytes, redundant: already set @1440)
//   @1595 VariantDataBP_9.AsBool = true   (20 bytes, redundant: already set @1469)
//   @1615 SetMemberPublicMetadata(tx, 2, "IsSlotAvailable", VariantDataBP_9, self)   (77 bytes)
//   @1692 VariantDataBP_9.Type = 9        (29, redundant)
//   @1721 VariantDataBP_9.AsBool = true   (20, redundant)
//   @1741 SetMemberPublicMetadata(tx, 3, ...)                                        (77 bytes)
// Rewritten as: call(2), call(3), call(4), EX_Nothing padding.  No jumps target this region.
static int PatchOpenSlotFour(uintptr_t fn)
{
    const int size = 2390, start = 1566, end = 1818, callLen = 77;
    ScriptArr s;
    int r = ReadyScript(fn, size, s);
    if (r != kDone) return r;
    uint8_t b[end - start];
    if (!SafeCopy(b, s.data + start, sizeof(b))) return kRetry;
    const uint8_t* c2 = b + (1615 - start);
    const uint8_t* c3 = b + (1741 - start);
    // Call layout: 19 20 <CDO 8> <skip 4> <rvalue 8 = null> 45 <FScriptName 12> 00 <tx 8> 1D <slot 4>
    //              1F "IsSlotAvailable " 00 <variant 8> 17 16
    const int kSlot = 44;
    auto isCall = [&](const uint8_t* c, int slot) {
        return c[0] == 0x19 && c[1] == 0x20 && GetPtr(c + 14) == 0 && c[22] == 0x45 && c[35] == 0x00 &&
               c[kSlot] == 0x1D && GetI32(c + kSlot + 1) == slot && c[49] == 0x1F &&
               memcmp(c + 50, "IsSlotAvailable", 16) == 0 && c[66] == 0x00 && c[75] == 0x17 && c[76] == 0x16;
    };
    // Already patched? Layout is then call(2) @0, call(3) @77, call(4) @154.
    if (isCall(b, 2) && isCall(b + callLen, 3) && isCall(b + 2 * callLen, 4)) return kDone;
    // Verify the original.
    if (memcmp(b, b + (1692 - start), 49) != 0) return kFail;      // the two redundant setup pairs match
    if (!isCall(c2, 2) || !isCall(c3, 3)) return kFail;
    for (int i = 0; i < callLen; i++)                               // identical except the slot number
        if ((i < kSlot + 1 || i > kSlot + 4) && c2[i] != c3[i]) return kFail;
    // Every property reference in the region must be resolved before bytes are moved.
    for (int base : { 0, 1692 - 1566 }) {
        if (!PropResolved(b + base + 1) || !PropResolved(b + base + 10) || !PropResolved(b + base + 19) ||
            !PropResolved(b + base + 29 + 2) || !PropResolved(b + base + 29 + 11))
            return kRetry;
    }
    if (!PropResolved(c2 + 36) || !PropResolved(c2 + 67) || !PropResolved(c3 + 36) || !PropResolved(c3 + 67)) return kRetry;
    const int i3 = kSlot;

    uint8_t nb[end - start];
    memset(nb, 0x0B, sizeof(nb));
    memcpy(nb, c2, callLen);
    memcpy(nb + callLen, c3, callLen);
    memcpy(nb + 2 * callLen, c3, callLen);
    int32_t four = 4;
    memcpy(nb + 2 * callLen + i3 + 1, &four, 4);
    return SafeWrite(s.data + start, nb, sizeof(nb)) ? kDone : kFail;
}

// ---------------------------------------------------------------------------------------------
// Shuttle: 5th seat
// ---------------------------------------------------------------------------------------------
// Original BP_PlayerShip_C::SpawnChairs (172 bytes):
//   ChairArray.Clear();                       (41 bytes, EX_Context on KismetArrayLibrary CDO)
//   SpawnSingleChair(Chair1, SceneChair1);    (32 bytes each, EX_LocalVirtualFunction)
//   ... Chair2..Chair4 ...
//   return; end
// Rewritten (same 172 bytes):
//   SpawnSingleChair(Chair1, SceneChair1) ... SpawnSingleChair(Chair4, SceneChair4);   (EX_LocalFinalFunction, 28 bytes each)
//   SpawnSingleChair(<null temp>, SceneChair);   SceneChair is the parent anchor of the 4 seats:
//                                                 centered between the two front seats.
//   EX_Nothing padding; return; end
// ChairArray starts empty on a freshly spawned ship (SpawnChairs is only called once from BeginPlay),
// so dropping the Clear() does not change behaviour.
// The 5th call needs a writable "Chair" reference; it uses SpawnSingleChair's own 'Chair' parameter
// property as a local at offset 0 of SpawnChairs' frame, whose size is raised from 0 to 8 bytes.
// Original SpawnChairs, saved when we patch it, so public lobbies can get the original 4 seats back.
static uintptr_t g_seatOrigFn = 0;
static uint8_t g_seatOrig[172];
static int16_t g_seatOrigAlign = 0;

static int PatchFifthSeat(uintptr_t shipCls)
{
    uintptr_t spawnChairs = Func(shipCls, "SpawnChairs");
    uintptr_t spawnSingle = Func(shipCls, "SpawnSingleChair");
    if (!spawnChairs || !spawnSingle) return kRetry;
    if (!g_fieldNameOff && !DetectFieldLayout(shipCls)) return kRetry;
    ScriptArr s;
    int r = ReadyScript(spawnChairs, 172, s);
    if (r != kDone) return r;
    ScriptArr s2;
    r = ReadyScript(spawnSingle, 456, s2);
    if (r != kDone) return r;

    uint8_t b[172];
    if (!SafeCopy(b, s.data, sizeof(b))) return kRetry;
    if (b[0] == 0x46 && b[28] == 0x46 && b[112] == 0x46 && GetPtr(&b[1]) == spawnSingle) return kDone;  // already patched

    // Verify the original bytecode.
    if (b[0] != 0x19 || b[1] != 0x20 || b[22] != 0x1C || b[31] != 0x01 || b[40] != 0x16) return kFail;
    if (!IEq(ObjName(GetPtr(&b[23])), "Array_Clear")) return kFail;
    if (GetPtr(&b[14]) != 0) return kFail;
    if (!PropIs(&b[32], "ChairArray")) return kRetry;  // not resolved yet
    uintptr_t chairs[4], scenes[4];
    for (int k = 0; k < 4; k++) {
        const uint8_t* c = b + 41 + 32 * k;
        if (c[0] != 0x45 || c[13] != 0x01 || c[22] != 0x01 || c[31] != 0x16) return kFail;
        std::string fname;
        if (!DecodeName((uint32_t)GetI32(&c[1]), fname) || !IEq(fname, "SpawnSingleChair")) return kFail;
        chairs[k] = GetPtr(&c[14]);
        scenes[k] = GetPtr(&c[23]);
        if (!PropIs(&c[14], ("Chair" + std::to_string(k + 1)).c_str()) ||
            !PropIs(&c[23], ("SceneChair" + std::to_string(k + 1)).c_str()))
            return kRetry;  // property references not resolved yet
    }
    if (b[169] != 0x04 || b[170] != 0x0B || b[171] != 0x53) return kFail;

    // Properties we need.
    PropInfo anchor = FindProperty(shipCls, "SceneChair");
    PropInfo chairParam = FindProperty(spawnSingle, "Chair");
    PropInfo targetParam = FindProperty(spawnSingle, "TargetComponent");
    if (!anchor.prop || anchor.elementSize != 8 || anchor.arrayDim != 1) { Log("  seat: SceneChair property not usable"); return kFail; }
    if (!chairParam.prop || chairParam.offset != 0 || chairParam.elementSize != 8 || chairParam.arrayDim != 1 ||
        !targetParam.prop || targetParam.offset != 8) {
        Log("  seat: SpawnSingleChair parameter layout unexpected (Chair off %d size %d, Target off %d)",
            chairParam.offset, chairParam.elementSize, targetParam.offset);
        return kFail;
    }

    // SpawnChairs has no locals; give it an 8-byte frame for the temporary chair reference.
    uintptr_t propsSizeAddr = spawnChairs + g_scriptOff - 8, minAlignAddr = spawnChairs + g_scriptOff - 4;
    // UE 5.7: int32 PropertiesSize; int16 MinAlignment; (another 16-bit field follows)
    int32_t propsSize = R<int32_t>(propsSizeAddr, -1);
    int16_t minAlign = R<int16_t>(minAlignAddr, -1);
    if (propsSize != 0 || minAlign < 1 || minAlign > 16) {
        Log("  seat: SpawnChairs frame unexpected (PropertiesSize %d, MinAlignment %d)", propsSize, minAlign);
        return kFail;
    }

    memcpy(g_seatOrig, b, sizeof(b));
    g_seatOrigAlign = minAlign;
    g_seatOrigFn = spawnChairs;

    uint8_t nb[172];
    memset(nb, 0x0B, sizeof(nb));  // EX_Nothing
    int p = 0;
    auto emitCall = [&](uint8_t chairTok, uintptr_t chairProp, uintptr_t sceneProp) {
        nb[p++] = 0x46; PutPtr(nb + p, spawnSingle); p += 8;   // EX_LocalFinalFunction SpawnSingleChair
        nb[p++] = chairTok; PutPtr(nb + p, chairProp); p += 8;  // Chair (by ref)
        nb[p++] = 0x01; PutPtr(nb + p, sceneProp); p += 8;      // EX_InstanceVariable anchor
        nb[p++] = 0x16;                                          // EX_EndFunctionParms
    };
    for (int k = 0; k < 4; k++) emitCall(0x01, chairs[k], scenes[k]);
    emitCall(0x00 /*EX_LocalVariable*/, chairParam.prop, anchor.prop);
    // SetUpChairLocal(): rebuild the seat-screen list now that all 5 seats exist (see PatchChairLocal).
    uintptr_t setUpChairLocal = Func(shipCls, "SetUpChairLocal");
    if (setUpChairLocal) { nb[p++] = 0x46; PutPtr(nb + p, setUpChairLocal); p += 8; nb[p++] = 0x16; }
    memcpy(nb + 169, b + 169, 3);

    int32_t newSize = 8;
    int16_t newAlign = minAlign < 8 ? 8 : minAlign;
    if (!SafeWrite(minAlignAddr, &newAlign, 2) || !SafeWrite(propsSizeAddr, &newSize, 4)) return kFail;
    if (!SafeWrite(s.data, nb, sizeof(nb))) return kFail;
    return kDone;
}

// Put the original 4-seat SpawnChairs back (and its empty frame).
static int RestoreSpawnChairs(uintptr_t shipCls)
{
    uintptr_t spawnChairs = Func(shipCls, "SpawnChairs");
    uintptr_t spawnSingle = Func(shipCls, "SpawnSingleChair");
    if (!spawnChairs || !spawnSingle) return kRetry;
    ScriptArr s;
    int r = ReadyScript(spawnChairs, 172, s);
    if (r != kDone) return r;
    uint8_t b[172];
    if (!SafeCopy(b, s.data, sizeof(b))) return kRetry;
    bool patched = b[0] == 0x46 && b[28] == 0x46 && b[112] == 0x46 && GetPtr(&b[1]) == spawnSingle;
    if (!patched) return kDone;
    if (g_seatOrigFn != spawnChairs) { Log("  seat: original SpawnChairs not saved, cannot remove the 5th seat"); return kFail; }
    uintptr_t propsSizeAddr = spawnChairs + g_scriptOff - 8, minAlignAddr = spawnChairs + g_scriptOff - 4;
    int32_t zero = 0;
    if (!SafeWrite(s.data, g_seatOrig, sizeof(g_seatOrig))) return kFail;
    if (!SafeWrite(propsSizeAddr, &zero, 4) || !SafeWrite(minAlignAddr, &g_seatOrigAlign, 2)) return kFail;
    return kDone;
}

// ---------------------------------------------------------------------------------------------
// Shuttle: include the 5th seat in ChairLocal (used to drive the seat screens)
// ---------------------------------------------------------------------------------------------
// Original BP_PlayerShip_C::SetUpChairLocal (488 bytes):
//   @0    ChairLocal.Clear()                                              (41 bytes, kept)
//   @41, @152, @263, @374 (111 bytes each):
//          b = IsValid(ChairN); if (!b) goto next; Ret = ChairLocal.Add(ChairN);
//   @485  return; end
// Rewritten (same size). ChairLocal only drives the seat *screens* (alarm, player lists, custom screen):
//   ChairLocal.Clear()
//   if (ChairArray.Length() > 0) ChairLocal = ChairArray;                // host: all seats incl. #5
//   else GetAllActorsOfClass(self, BP_Seat_C, ChairLocal);              // other players: ChairArray is host-only
//   if (LastRowScreen != None) SetCustomScreen(LastRowScreen);           // bring a late seat up to date
//   padding; return; end
// SpawnChairs calls this again once all 5 seats exist, and the seat BeginPlay hook (seatfix.inc) calls it
// on other players' machines when seat 5 arrives.
static int PatchChairLocal(uintptr_t shipCls)
{
    const int size = 488;
    uintptr_t fn = Func(shipCls, "SetUpChairLocal");
    if (!fn) return kRetry;
    ScriptArr s;
    int r = ReadyScript(fn, size, s);
    if (r != kDone) return r;
    uintptr_t greaterFn = MathFunc("Greater_IntInt"), lengthFn = ArrayFunc("Array_Length");
    if (!greaterFn || !lengthFn) return kRetry;
    uint8_t b[size];
    if (!SafeCopy(b, s.data, size)) return kRetry;
    if (b[41] == 0x07 && b[46] == 0x68 && GetPtr(&b[47]) == greaterFn) return kDone;  // already patched

    // Verify the original.
    if (b[0] != 0x19 || b[1] != 0x20 || b[22] != 0x1C || b[31] != 0x01 || b[40] != 0x16) return kFail;
    if (!IEq(ObjName(GetPtr(&b[23])), "Array_Clear")) return kFail;
    if (b[485] != 0x04 || b[486] != 0x0B || b[487] != 0x53) return kFail;
    if (!PropIs(&b[32], "ChairLocal")) return kRetry;
    uintptr_t chairLocal = GetPtr(&b[32]);
    struct Blk { uintptr_t isValidFn, chair, addFn, retProp; } blk[4];
    for (int k = 0; k < 4; k++) {
        const uint8_t* c = b + 41 + 111 * k;
        const uint8_t* a = c + 43;
        if (c[0] != 0x14 || c[1] != 0x00 || c[10] != 0x68 || c[19] != 0x01 || c[28] != 0x16 ||
            c[29] != 0x07 || c[34] != 0x00 || GetPtr(&c[2]) != GetPtr(&c[35]) ||
            (int)GetI32(&c[30]) != 41 + 111 * (k + 1))
            return kFail;
        if (a[0] != 0x0F || a[9] != 0x00 || a[18] != 0x19 || a[19] != 0x20 || a[40] != 0x1C || a[49] != 0x01 ||
            a[58] != 0x01 || a[67] != 0x16 || GetPtr(&a[1]) != GetPtr(&a[10]) || GetPtr(&a[50]) != chairLocal ||
            GetPtr(&a[20]) != GetPtr(&a[20]) || GetPtr(&c[20]) != GetPtr(&a[59]))
            return kFail;
        if (!IEq(ObjName(GetPtr(&c[11])), "IsValid") || !IEq(ObjName(GetPtr(&a[41])), "Array_Add")) return kFail;
        std::string chairName = "Chair" + std::to_string(k + 1);
        if (!PropResolved(&c[2]) || !PropIs(&c[20], chairName.c_str()) || !PropResolved(&a[1])) return kRetry;
        blk[k] = { GetPtr(&c[11]), GetPtr(&c[20]), GetPtr(&a[41]), GetPtr(&a[1]) };
    }
    PropInfo chairArray = FindProperty(shipCls, "ChairArray");
    if (!chairArray.prop || chairArray.elementSize != 16) { Log("  chair list: ChairArray property unexpected"); return kFail; }
    PropInfo lastRowScreen = FindProperty(shipCls, "LastRowScreen");
    uintptr_t setCustomScreenFn = Func(shipCls, "SetCustomScreen"), notEqualNameFn = MathFunc("NotEqual_NameName");
    if (!lastRowScreen.prop || lastRowScreen.elementSize != 8 || !setCustomScreenFn) { Log("  chair list: LastRowScreen/SetCustomScreen unexpected"); return kFail; }
    if (!notEqualNameFn) return kRetry;
    auto seatIt = g_classes.find("BP_Seat_C");
    auto gsIt = g_engineClasses.find("gameplaystatics");
    if (seatIt == g_classes.end() || gsIt == g_engineClasses.end()) return kRetry;
    uintptr_t seatCls = seatIt->second, getAllActorsFn = Func(gsIt->second, "GetAllActorsOfClass");
    if (!getAllActorsFn) return kRetry;

    uint8_t nb[size];
    memset(nb, 0x0B, sizeof(nb));
    memcpy(nb, b, 41);                                     // ChairLocal.Clear()
    int p = 41;
    auto ptr = [&](uintptr_t v) { PutPtr(nb + p, v); p += 8; };
    auto i32 = [&](int32_t v) { memcpy(nb + p, &v, 4); p += 4; };
    // if (!(Array_Length(ChairArray) > 0)) goto CLIENT;
    nb[p++] = 0x07; int origJump = p; i32(0);
    nb[p++] = 0x68; ptr(greaterFn);
    nb[p++] = 0x68; ptr(lengthFn); nb[p++] = 0x01; ptr(chairArray.prop); nb[p++] = 0x16;
    nb[p++] = 0x1D; i32(0);
    nb[p++] = 0x16;
    // ChairLocal = ChairArray; goto SCREEN;            (host: every seat the ship spawned/attached)
    nb[p++] = 0x0F; ptr(chairLocal); nb[p++] = 0x01; ptr(chairLocal); nb[p++] = 0x01; ptr(chairArray.prop);
    nb[p++] = 0x06; int toScreen = p; i32(0);
    // CLIENT: GetAllActorsOfClass(self, BP_Seat_C, ChairLocal);   (other players: ChairArray is host-only)
    { int32_t here = p; memcpy(nb + origJump, &here, 4); }
    nb[p++] = 0x68; ptr(getAllActorsFn); nb[p++] = 0x17; nb[p++] = 0x20; ptr(seatCls); nb[p++] = 0x01; ptr(chairLocal); nb[p++] = 0x16;
    // SCREEN: if (LastRowScreen != None) SetCustomScreen(LastRowScreen);   -- re-send the current screen to every seat
    { int32_t here = p; memcpy(nb + toScreen, &here, 4); }
    nb[p++] = 0x07; int skipScreen = p; i32(0);
    nb[p++] = 0x68; ptr(notEqualNameFn); nb[p++] = 0x01; ptr(lastRowScreen.prop);
    nb[p++] = 0x21; for (int z = 0; z < 12; z++) nb[p++] = 0;   // EX_NameConst None
    nb[p++] = 0x16;
    nb[p++] = 0x46; ptr(setCustomScreenFn); nb[p++] = 0x01; ptr(lastRowScreen.prop); nb[p++] = 0x16;
    { int32_t here = p; memcpy(nb + skipScreen, &here, 4); }
    (void)blk;
    if (p > 485) return kFail;
    memcpy(nb + 485, b + 485, 3);
    return SafeWrite(s.data, nb, sizeof(nb)) ? kDone : kFail;
}

// ---------------------------------------------------------------------------------------------
// Shuttle: launch when min(players, 4) are seated (fallback if the 5th seat is not available)
// ---------------------------------------------------------------------------------------------
// CheckForLaunch @393 (+@431):  b = (Length(InGamePlayers) == NbPlayerSited); if (!b) pop_flow;
//        becomes            :  if (!(Min(Length(InGamePlayers), 4) == NbPlayerSited)) pop_flow;
// CheckForLaunch @628 (+@666):  b = (NbPlayerInShip == NbPlayerSited); if (!b) pop_flow;
//        becomes            :  if (!(Min(NbPlayerInShip, 4) == NbPlayerSited)) pop_flow;
static int PatchLaunchCompare(uintptr_t fn, int size, int stmt, uint8_t firstArgTok)
{
    ScriptArr s;
    int r = ReadyScript(fn, size, s);
    if (r != kDone) return r;
    uintptr_t minFn = MathFunc("Min");
    if (!minFn) return kRetry;
    uint8_t b[48];
    if (!SafeCopy(b, s.data + stmt, sizeof(b))) return kRetry;
    if (b[0] == 0x4F && b[1] == 0x68 && b[10] == 0x68 && GetPtr(&b[11]) == minFn) return kDone;
    if (b[0] != 0x14 || b[1] != 0x00 || b[10] != 0x68 || b[19] != firstArgTok || b[28] != 0x01 || b[37] != 0x16 ||
        b[38] != 0x4F || b[39] != 0x00 || GetPtr(&b[2]) != GetPtr(&b[40]))
        return kFail;
    uintptr_t eqFn = GetPtr(&b[11]);
    if (!IEq(ObjName(eqFn), "EqualEqual_IntInt")) return kFail;
    if (!PropResolved(&b[2]) || !PropIs(&b[20], firstArgTok == 0x00 ? "CallFunc_Array_Length_ReturnValue" : "NbPlayerInShip") ||
        !PropIs(&b[29], "NbPlayerSited"))
        return kRetry;  // property references not resolved yet
    uint8_t nb[48];
    memset(nb, 0x0B, sizeof(nb));
    int p = 0;
    nb[p++] = 0x4F;                                     // EX_PopExecutionFlowIfNot
    nb[p++] = 0x68; PutPtr(nb + p, eqFn); p += 8;       // EX_CallMath EqualEqual_IntInt
    nb[p++] = 0x68; PutPtr(nb + p, minFn); p += 8;      //   EX_CallMath Min
    memcpy(nb + p, b + 19, 9); p += 9;                  //     original first operand
    nb[p++] = 0x1D; int32_t four = 4; memcpy(nb + p, &four, 4); p += 4;  // 4
    nb[p++] = 0x16;                                     //   end Min
    memcpy(nb + p, b + 28, 9); p += 9;                  //   NbPlayerSited
    nb[p++] = 0x16;                                     // end EqualEqual
    return SafeWrite(s.data + stmt, nb, sizeof(nb)) ? kDone : kFail;
}

// Check Player Sit End @409 (+@447): b = (NbPlayerSited >= Length); if (!b) goto X;
//                     becomes      : if (!(NbPlayerSited >= Min(Length, 4))) goto X;
static int PatchSitEndCompare(uintptr_t fn)
{
    const int size = 853, stmt = 409;
    ScriptArr s;
    int r = ReadyScript(fn, size, s);
    if (r != kDone) return r;
    uintptr_t minFn = MathFunc("Min");
    if (!minFn) return kRetry;
    uint8_t b[52];
    if (!SafeCopy(b, s.data + stmt, sizeof(b))) return kRetry;
    if (b[0] == 0x07 && b[5] == 0x68 && b[23] == 0x68 && GetPtr(&b[24]) == minFn) return kDone;
    if (b[0] != 0x14 || b[1] != 0x00 || b[10] != 0x68 || b[19] != 0x01 || b[28] != 0x00 || b[37] != 0x16 ||
        b[38] != 0x07 || b[43] != 0x00 || GetPtr(&b[2]) != GetPtr(&b[44]))
        return kFail;
    uintptr_t geFn = GetPtr(&b[11]);
    if (!IEq(ObjName(geFn), "GreaterEqual_IntInt")) return kFail;
    if (!PropResolved(&b[2]) || !PropIs(&b[20], "NbPlayerSited") || !PropIs(&b[29], "CallFunc_Array_Length_ReturnValue"))
        return kRetry;  // property references not resolved yet
    uint8_t nb[52];
    memset(nb, 0x0B, sizeof(nb));
    int p = 0;
    nb[p++] = 0x07; memcpy(nb + p, b + 39, 4); p += 4;  // EX_JumpIfNot <same target>
    nb[p++] = 0x68; PutPtr(nb + p, geFn); p += 8;       // GreaterEqual_IntInt(
    memcpy(nb + p, b + 19, 9); p += 9;                  //   NbPlayerSited,
    nb[p++] = 0x68; PutPtr(nb + p, minFn); p += 8;      //   Min(
    memcpy(nb + p, b + 28, 9); p += 9;                  //     Length,
    nb[p++] = 0x1D; int32_t four = 4; memcpy(nb + p, &four, 4); p += 4;  // 4
    nb[p++] = 0x16;                                     //   )
    nb[p++] = 0x16;                                     // )
    return SafeWrite(s.data + stmt, nb, sizeof(nb)) ? kDone : kFail;
}

#include "crewrow.inc"
#include "seatfix.inc"

// ---------------------------------------------------------------------------------------------
// Per-class patch sets
// ---------------------------------------------------------------------------------------------
struct Step { const char* what; int result = kRetry; };

static int Combine(std::vector<Step>& steps, const char* cls)
{
    bool anyRetry = false, anyFail = false;
    for (auto& s : steps) { anyRetry |= s.result == kRetry; anyFail |= s.result == kFail; }
    if (anyRetry) return kRetry;
    if (!g_quiet) {
        for (auto& s : steps) Log("  %s: %s", s.what, ResultStr(s.result));
        Log("%s patched%s", cls, anyFail ? " (with failures)" : "");
    }
    return anyFail ? kFail : kDone;
}

#include "scaling.inc"

// BP_MyGameInstance_C::CheckAllowConnectionLobby @959 (37 bytes):
//   Count = Conv_Int64ToInt(self.playerLobbyCapacity)  ->  Conv_Int64ToInt(5)
// The joiner's own capacity follows its own (non-)private setting, but this pre-join check (the rejoin
// whitelist loop) must always look at all 5 slots of the lobby it is joining. Harmless for 4-slot lobbies.
static int PatchJoinCheckSlots(uintptr_t fn)
{
    const int size = 1802, stmt = 959;
    ScriptArr s;
    int r = ReadyScript(fn, size, s);
    if (r != kDone) return r;
    uint8_t b[37];
    if (!SafeCopy(b, s.data + stmt, sizeof(b))) return kRetry;
    if (b[0] != 0x0F || b[9] != 0x00 || b[18] != 0x68 || b[36] != 0x16) return kFail;
    if (!IEq(ObjName(GetPtr(&b[19])), "Conv_Int64ToInt")) return kFail;
    int64_t five = 5;
    if (b[27] == 0x35 && memcmp(&b[28], &five, 8) == 0) return kDone;
    if (b[27] != 0x01) return kFail;
    if (!PropIs(&b[28], "playerLobbyCapacity")) return kRetry;
    uint8_t nb[9]; nb[0] = 0x35; memcpy(nb + 1, &five, 8);       // EX_Int64Const 5
    return SafeWrite(s.data + stmt + 27, nb, 9) ? kDone : kFail;
}

static int PatchSimple(const char* clsName, uintptr_t cls)
{
    std::vector<Step> st;
    auto F = [&](const char* n) { return Func(cls, n); };
    std::string c = clsName;
    if (c == "BP_MyGameInstance_C") {
        st.push_back({ "lobby creation opens member slot 4", PatchOpenSlotFour(F("Make Player Lobby Transaction")) });
        st.push_back({ "joining/rejoining checks 5 slots", PatchJoinCheckSlots(F("CheckAllowConnectionLobby")) });
    } else if (c == "BP_LobbyGameMode_C") {
        st.push_back({ "lobby 'more than 4 players' check -> 5", PatchIntConst(F("Check More Than 4Players"), 2296, 435, 34, 4, 5) });
    } else if (c == "BP_Character_Lobby_C") {
        st.push_back({ "lobby spawn point for player 5", PatchSwapNotEqualToLess(F("_GetPlayerStartFromId"), 2031, 956) });
    } else if (c == "WBP_ScorePage_C") {
        st.push_back({ "score share (alive players)", PatchSwapNotEqualToLess(F("SetUpMaxShare"), 1839, 1265) });
        st.push_back({ "score share (other players)", PatchSwapNotEqualToLess(F("SetUpMaxShare"), 1839, 1487) });
    } else if (c == "WBP_PauseMenu_CrewMembers_C") {
        uintptr_t ug = F("ExecuteUbergraph_WBP_PauseMenu_CrewMembers");
        st.push_back({ "crew panel hides 5 action popups", PatchIntConst(ug, kCrewUgSize, 98, 34, 3, 4) });
        st.push_back({ "crew panel 5th row hook (shown in 5-player lobbies)", PrepareCrewRow(cls) });
    } else if (c == "BP_Seat_C") {
        st.push_back({ "seat 5 linked to the ship on other players' machines", PrepareSeatHook(cls) });
    } else if (c == "BP_MyPlayerController_C") {
        uintptr_t ug = F("ExecuteUbergraph_BP_MyPlayerController");
        st.push_back({ "voice volume array grows for player 5", PatchArraySetSizeToFit(ug, 43251, 40341, 82) });
        st.push_back({ "voice mute array grows for player 5", PatchArraySetSizeToFit(ug, 43251, 40469, 82) });
    } else if (!AddScalingSteps(c, cls, st)) {
        return kDone;
    }
    return Combine(st, clsName);
}

static int g_shipFallback = -1;   // -1 undecided, 0 not used, 1 used (decided on the first successful patch)
static int g_shipMode = -1;       // mode PatchShip last ran in: 1 = 5 seats (hosting a private lobby), 0 = 4 seats
static bool g_noReloadLog = false;
static int PatchShip(uintptr_t cls)
{
    bool priv = g_isPrivate;
    if (g_shipMode != (int)priv) {
        if (g_shipMode >= 0) {
            Log(priv ? "Shuttle: 5th seat on (hosting a private lobby)" : "Shuttle: original 4 seats (not hosting a private lobby)");
            g_noReloadLog = true;
        }
        g_shipMode = priv;
    }
    // The seat-screen list works with any number of seats; guests also need it in private lobbies
    // (they can't tell private from public), so it is applied in both modes.
    int cl = PatchChairLocal(cls);
    if (cl == kRetry) return kRetry;
    std::vector<Step> st;
    if (!priv) {
        // Public lobby, menus, or a guest (seats are spawned by the host only): original 4 seats.
        int r = RestoreSpawnChairs(cls);
        if (r == kRetry) return kRetry;
        st.push_back({ "original 4 shuttle seats (not hosting a private lobby)", r });
        st.push_back({ "seat screens use every spawned seat", cl });
        return Combine(st, "BP_PlayerShip_C");
    }
    int seat = PatchFifthSeat(cls);
    if (seat == kRetry) return kRetry;
    st.push_back({ "5th shuttle seat (private lobby)", seat });
    st.push_back({ "seat screens use every spawned seat", cl });
    bool needFallback = g_shipFallback >= 0 ? g_shipFallback == 1 : (seat != kDone || g_shipInstances > 0);
    if (g_shipFallback < 0 && seat == kDone && g_shipInstances > 0)
        Log("  a shuttle already existed before the seat patch; it only has 4 seats, enabling launch-with-4 fallback");
    if (needFallback) {
        uintptr_t cfl = Func(cls, "CheckForLaunch");
        st.push_back({ "launch when min(players,4) seated (departure)", PatchLaunchCompare(cfl, 1271, 393, 0x00) });
        st.push_back({ "launch when min(players,4) seated (escape)", PatchLaunchCompare(cfl, 1271, 628, 0x01) });
        st.push_back({ "score screen when min(players,4) seated", PatchSitEndCompare(Func(cls, "Check Player Sit End")) });
    }
    int res = Combine(st, "BP_PlayerShip_C");
    if (res != kRetry && g_shipFallback < 0) g_shipFallback = needFallback ? 1 : 0;
    return res;
}

// playerLobbyCapacity lives on the GameInstance: 5 while hosting/setting up a private lobby, else 4.
static uintptr_t g_capClass = 0;
static int32_t g_capOffset = -1;
static int32_t g_privOffset = -1;
static std::unordered_set<uintptr_t> g_capLogged;

static void PatchLobbyCapacity()
{
    auto it = g_classes.find("BP_MyGameInstance_C");
    if (it == g_classes.end()) return;
    uintptr_t cls = it->second;
    if (!g_scriptOff) return;  // needs UStruct layout, detected from the first patched function
    static int tries = 0;
    static bool gaveUp = false;
    if (cls != g_capClass) { g_capClass = cls; g_capOffset = -1; tries = 0; gaveUp = false; }
    if (g_capOffset < 0) {
        if (gaveUp) return;
        auto fail = [&](const char* why, int a1 = 0, int a2 = 0, int a3 = 0, int a4 = 0) {
            if (++tries < 2000) return;  // ~30 s of retries while the class finishes loading
            gaveUp = true;
            Log("playerLobbyCapacity NOT changed: %s (%d %d %d %d)", why, a1, a2, a3, a4);
        };
        if (!g_fieldNameOff && !DetectFieldLayout(cls)) return fail("could not detect FField layout");
        // Validate the FProperty layout on a function whose parameter offsets are known:
        // AddAndSave_MoneyMonunt(int amount) has 'amount' at 0 and 'CallFunc_Add_IntInt_ReturnValue' at 4.
        uintptr_t probe = Func(cls, "AddAndSave_MoneyMonunt");
        if (!probe) return fail("probe function not found");
        PropInfo a = FindProperty(probe, "amount"), b = FindProperty(probe, "CallFunc_Add_IntInt_ReturnValue");
        if (a.offset != 0 || a.elementSize != 4 || a.arrayDim != 1 || b.offset != 4 || b.elementSize != 4)
            return fail("FProperty layout check failed", a.offset, a.elementSize, b.offset, b.elementSize);
        PropInfo pi = FindProperty(cls, "playerLobbyCapacity");
        int32_t propsSize = R<int32_t>(cls + g_scriptOff - 8);
        if (!pi.prop || pi.elementSize != 8 || pi.arrayDim != 1 || pi.offset < 0x28 || pi.offset + 8 > propsSize)
            return fail("property missing or unexpected", pi.elementSize, pi.arrayDim, pi.offset, propsSize);
        PropInfo pv = FindProperty(cls, "isPlayerLobbyPrivate");
        if (!pv.prop || pv.elementSize != 1 || pv.offset < 0x28 || pv.offset >= propsSize)
            return fail("isPlayerLobbyPrivate missing or unexpected", pv.elementSize, pv.offset, propsSize, 0);
        g_capOffset = pi.offset;
        g_privOffset = pv.offset;
        Log("playerLobbyCapacity at offset 0x%x, isPlayerLobbyPrivate at 0x%x", g_capOffset, g_privOffset);
    }
    for (uintptr_t o : g_gameInstances) {
        if (ObjClass(o) != cls || (ObjFlags(o) & RF_ClassDefaultObject)) continue;   // the live GameInstance
        bool priv = R<uint8_t>(o + g_privOffset, 0) != 0;
        if (priv != g_isPrivate) {
            g_isPrivate = priv;
            Log(priv ? "Private lobby: 5-player mode ON" : "Not a private lobby: normal 4-player mode");
        }
        int64_t v = R<int64_t>(o + g_capOffset, -1);
        int64_t want = priv ? kTargetPlayers : 4;
        if ((v == 4 || v == kTargetPlayers) && v != want) {
            SafeWrite(o + g_capOffset, &want, 8);
            Log("  playerLobbyCapacity %lld -> %lld", (long long)v, (long long)want);
        } else if (v != 4 && v != kTargetPlayers && g_capLogged.insert(o).second) {
            Log("%s: playerLobbyCapacity is %lld (expected 4), left unchanged", ObjName(o).c_str(), (long long)v);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------------------------
struct ClassState { uintptr_t cls = 0; int result = kRetry; DWORD firstSeen = 0; uint64_t fp = 0; DWORD lastVerify = 0; };

static DWORD WINAPI ModThread(LPVOID)
{
    Log("Species Unknown 5-player mod starting (seat 5 offset %.1f, %.1f, %.1f)", kSeat5Offset[0], kSeat5Offset[1], kSeat5Offset[2]);
    for (const char* c : kTargetClasses) g_targetCanon[Lower(c)] = c;

    DWORD t0 = GetTickCount();
    while (!g_nameBlocks || !g_objObjects) {
        if (!g_nameBlocks) FindNamePool();
        if (g_nameBlocks && !g_objObjects) FindObjectArray();
        if (g_nameBlocks && g_objObjects) break;
        if (GetTickCount() - t0 > 120000) { Log("ERROR: engine globals not found; mod inactive"); return 0; }
        Sleep(50);
    }
    for (int i = 0; i < 3; i++) Log("object %d = %s", i, ObjName(ObjAt(i)).c_str());

    std::unordered_map<std::string, ClassState> state;
    DWORD lastFull = 0;
    for (;;) {
        DWORD now = GetTickCount();
        if (now - lastFull > 3000) { FullScan(); lastFull = now; }
        else IncrementalScan();

        for (auto& kv : g_classes) {
            ClassState& cs = state[kv.first];
            if (cs.cls != kv.second) { cs = ClassState(); cs.cls = kv.second; cs.firstSeen = now; }
            if (cs.result != kRetry) {
                // Re-patch if the class was reloaded in place (its functions/bytecode changed).
                uint64_t fp = ClassFingerprint(kv.second);
                if (fp != cs.fp && fp != 0) {
                    Log("%s reloaded, patching again", kv.first.c_str());
                    cs = ClassState(); cs.cls = kv.second; cs.firstSeen = now;
                } else {
                    // A class can also be unloaded and reloaded with every object at its old address, so
                    // the fingerprint can't see it. Every patch step detects "already applied", so simply
                    // re-run the steps quietly now and then; anything a reload undid gets re-applied.
                    if (cs.result != kDone || now - cs.lastVerify < 100 || !IsLive(kv.second)) continue;
                    cs.lastVerify = now;
                    long before = g_writeCount;
                    g_quiet = true;
                    int rr = kv.first == "BP_PlayerShip_C" ? PatchShip(kv.second) : PatchSimple(kv.first.c_str(), kv.second);
                    g_quiet = false;
                    if (g_writeCount != before && !g_noReloadLog) Log("%s was reloaded; patches re-applied (%s)", kv.first.c_str(), ResultStr(rr));
                    g_noReloadLog = false;
                    continue;
                }
            }
            if (!IsLive(kv.second)) continue;
            int r = kv.first == "BP_PlayerShip_C" ? PatchShip(kv.second)
                  : PatchSimple(kv.first.c_str(), kv.second);
            if (r == kRetry && now - cs.firstSeen > 30000) {
                Log("%s: gave up waiting for its bytecode to load", kv.first.c_str());
                r = kFail;
            }
            cs.result = r;
            if (r != kRetry) cs.fp = ClassFingerprint(kv.second);
        }
        // Script offset detection needs any loaded target function; the GameInstance is loaded at boot,
        // so fall back to detecting it from one of its known functions if nothing else has been patched yet.
        if (!g_scriptOff) {
            auto it = g_classes.find("BP_MyGameInstance_C");
            if (it != g_classes.end()) {
                uintptr_t f = Func(it->second, "OWNER Get Online Nb Players in Lobby");
                if (f) DetectScriptOffset(f, kGIProbeSize);
            }
        }
        PatchLobbyCapacity();
        Sleep(15);
    }
}

void StartFivePlayerMod(HMODULE self)
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) lstrcpyW(slash + 1, L"FivePlayerMod.log");
    g_log = _wfopen(path, L"w");
    // Optional settings: FivePlayerMod.ini next to the DLL.
    if (slash) {
        lstrcpyW(slash + 1, L"FivePlayerMod.ini");
        wchar_t buf[64];
        const wchar_t* keys[3] = { L"X", L"Y", L"Z" };
        for (int i = 0; i < 3; i++) {
            if (GetPrivateProfileStringW(L"Seat5", keys[i], L"", buf, 64, path) > 0)
                kSeat5Offset[i] = _wtof(buf);
        }
    }
    HANDLE h = CreateThread(nullptr, 0, ModThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}
