// ---------------------------------------------------------------------------------------------
// Pause menu "Crew Members": 5th row (below Slot 3)
// ---------------------------------------------------------------------------------------------
// The panel (WBP_PauseMenu_CrewMembers_C) has 4 designer-placed rows. Each row is a CanvasPanel in
// VerticalBox_171 holding ScaleBox > WBP_PauseMenu_PlayerInfos (name/mic/rank) and
// Overlay > WBP_PauseMenu_PlayerActions (mute/volume/kick); both have PlayerIndex 0..3.
// On Construct the panel builds PlayersInfos/PlayersActions arrays from the 4 rows and drives
// everything else (refresh, kick binding, lookup by lobby id) from those arrays.
//
// We hook the panel's Construct (by swapping the UFunction's native Func pointer, so our code runs
// on the game thread), build a 5th row with PlayerIndex 4 that copies Slot 1's layout, append it
// to the vertical box, and rewrite the two array-building statements to include the new widgets.

static uintptr_t g_funcFuncOff = 0;          // UFunction::Func
static uintptr_t g_processInternal = 0;      // UObject::ProcessInternal (the Func of every Blueprint function)
static int       g_peIndex = -1;            // vtable index of ProcessEvent (called through each object's own vtable)
using NativeFn = void (*)(void* ctx, void* stack, void* result);
using ProcessEventFn = void (*)(void* obj, void* fn, void* parms);

struct CrewCtx {
    bool ready = false;
    uintptr_t crewCls = 0, constructFn = 0, ubergraph = 0;
    NativeFn origConstruct = nullptr;
    int32_t offInfos1 = -1, offActions1 = -1;          // crew instance vars
    int32_t offInfosIdx = -1, offActionsIdx = -1;      // PlayerIndex in the row widget classes
    uintptr_t clsCanvas = 0, clsScaleBox = 0, clsOverlay = 0;
    uintptr_t fnCreate = 0, fnSpawnObject = 0, fnAddChild = 0, fnAddChildToCanvas = 0;
    uintptr_t fnVbSetPadding = 0, fnVbSetSize = 0, fnVbSetHAlign = 0, fnVbSetVAlign = 0;
    uintptr_t cdoWidgetLib = 0, cdoGameplayStatics = 0;
    int32_t offVisibility = -1;                         // UWidget::Visibility
    int32_t sbStretchOff = -1, sbStretchSize = 0;       // ScaleBox props to copy (contiguous block)
    int32_t csBlockOff = -1, csBlockSize = 0;           // CanvasPanelSlot LayoutData..ZOrder
    int32_t osBlockOff = -1, osBlockSize = 0;           // OverlaySlot Padding..VAlign
    int32_t sbsBlockOff = -1, sbsBlockSize = 0;         // ScaleBoxSlot HAlign..VAlign
    int32_t vbSizeOff = -1, vbPadOff = -1, vbHOff = -1, vbVOff = -1;
    int32_t offUberFrame = -1, offColorDefault = -1;   // PlayerInfos: persistent frame ptr / colour-switch fallback
    // runtime state (game thread only)
    uintptr_t lastCrew = 0, lastCanvas = 0, lastInfos = 0, lastActions = 0;
};
static CrewCtx g_crew;

static uintptr_t SlotOf(uintptr_t w) { return R<uintptr_t>(w + 0x30); }       // UWidget::Slot
static uintptr_t SlotParent(uintptr_t s) { return R<uintptr_t>(s + 0x28); }   // UPanelSlot::Parent
static uintptr_t ParentPanel(uintptr_t w) { uintptr_t s = SlotOf(w); return Plausible(s) ? SlotParent(s) : 0; }

// Find UObject::ProcessEvent's vtable slot: we are called from ProcessEvent -> UFunction::Invoke -> our
// thunk, so one of our callers' function start addresses is UObject::ProcessEvent. We look it up in the
// vtable of UObject's own default object (Default__Object): that table is the shortest, so the first
// match is the real slot. (Scanning an actor's or widget's longer table can run past its end into the
// next class's table and match there - that gave a wrong slot.) Calls then go through each object's own
// vtable at that slot, so overrides such as AActor::ProcessEvent are respected.
static void FindProcessEvent(uintptr_t)
{
    if (g_peIndex >= 0 || !Plausible(g_objectCDO)) return;
    uintptr_t vt = R<uintptr_t>(g_objectCDO);
    void* frames[24];
    USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    for (USHORT i = 1; i < n && g_peIndex < 0; i++) {
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)frames[i], &imageBase, nullptr);
        if (!rf) continue;
        uintptr_t start = (uintptr_t)(imageBase + rf->BeginAddress);
        for (int k = 0; k < 160; k++) {
            uintptr_t e = R<uintptr_t>(vt + k * 8);
            if (!e) break;
            if (e == start) {
                g_peIndex = k;
                Log("  ProcessEvent = exe+0x%llx (vtable index %d)",
                    (unsigned long long)(start - (uintptr_t)GetModuleHandleW(nullptr)), k);
                break;
            }
        }
    }
}

static bool CallUFunction(uintptr_t obj, uintptr_t fn, void* parms)
{
    if (g_peIndex < 0 || !Plausible(obj) || !Plausible(fn)) return false;
    uintptr_t pe = R<uintptr_t>(R<uintptr_t>(obj) + 8 * g_peIndex);
    if (!Plausible(pe & ~7ull)) return false;
    ((ProcessEventFn)pe)((void*)obj, (void*)fn, parms);
    return true;
}

static uintptr_t CallCreate(uintptr_t worldCtx, uintptr_t cls)
{
    uint8_t p[0x20] = {};
    PutPtr(p + 0, worldCtx); PutPtr(p + 8, cls); PutPtr(p + 16, 0);
    if (!CallUFunction(g_crew.cdoWidgetLib, g_crew.fnCreate, p)) return 0;
    return GetPtr(p + 24);
}
static uintptr_t CallSpawnObject(uintptr_t cls, uintptr_t outer)
{
    uint8_t p[0x18] = {};
    PutPtr(p + 0, cls); PutPtr(p + 8, outer);
    if (!CallUFunction(g_crew.cdoGameplayStatics, g_crew.fnSpawnObject, p)) return 0;
    return GetPtr(p + 16);
}
static uintptr_t CallAddChild(uintptr_t fn, uintptr_t panel, uintptr_t child)
{
    uint8_t p[0x10] = {};
    PutPtr(p + 0, child);
    if (!CallUFunction(panel, fn, p)) return 0;
    return GetPtr(p + 8);
}

// Rewrite the two array-building statements of the panel's ubergraph:
//   @3214 K2Node_MakeArray_Array = [Actions0..3] (47)   @3261 PlayersActions = K2Node_MakeArray_Array (27)
//   @3288 K2Node_MakeArray_Array_1 = [Infos0..3] (47)   @3335 PlayersInfos = K2Node_MakeArray_Array_1 (27)
// into  PlayersActions = [Actions0..3, <new actions>]  and  PlayersInfos = [Infos0..3, <new infos>]
// (EX_SetArray directly on the member, 56 bytes + EX_Nothing padding). No jumps target this region.
static const int kCrewUgSize = 4871;
static int PatchCrewArrays(uintptr_t ug, uintptr_t newActions, uintptr_t newInfos, bool verifyOnly)
{
    ScriptArr s;
    if (!GetScript(ug, s) || s.num != kCrewUgSize) return kFail;
    for (int pass = 0; pass < 2; pass++) {
        int at = pass == 0 ? 3214 : 3288;
        uint8_t b[74];
        if (!SafeCopy(b, s.data + at, sizeof(b))) return kFail;
        uintptr_t elems[4], arrProp;
        bool patched = b[0] == 0x31 && b[1] == 0x01 && b[46] == 0x20 && b[55] == 0x32;
        if (patched) {
            arrProp = GetPtr(&b[2]);
            for (int i = 0; i < 4; i++) { if (b[10 + 9 * i] != 0x01) return kFail; elems[i] = GetPtr(&b[11 + 9 * i]); }
        } else {
            // original: 31 00 <tmp> (01 <e>)x4 32 | 0F <arr> 01 <arr> 00 <tmp>
            if (b[0] != 0x31 || b[1] != 0x00 || b[46] != 0x32 || b[47] != 0x0F || b[56] != 0x01 || b[65] != 0x00) return kFail;
            if (GetPtr(&b[2]) != GetPtr(&b[66]) || GetPtr(&b[48]) != GetPtr(&b[57])) return kFail;
            for (int i = 0; i < 4; i++) { if (b[10 + 9 * i] != 0x01) return kFail; elems[i] = GetPtr(&b[11 + 9 * i]); }
            arrProp = GetPtr(&b[48]);
        }
        if (verifyOnly) continue;
        uint8_t nb[74];
        memset(nb, 0x0B, sizeof(nb));
        int p = 0;
        nb[p++] = 0x31; nb[p++] = 0x01; PutPtr(nb + p, arrProp); p += 8;          // EX_SetArray self.<array>
        for (int i = 0; i < 4; i++) { nb[p++] = 0x01; PutPtr(nb + p, elems[i]); p += 8; }
        nb[p++] = 0x20; PutPtr(nb + p, pass == 0 ? newActions : newInfos); p += 8; // EX_ObjectConst <new row widget>
        nb[p++] = 0x32;                                                           // EX_EndArray
        if (!SafeWrite(s.data + at, nb, sizeof(nb))) return kFail;
    }
    return kDone;
}

static bool CopyBlock(uintptr_t dst, uintptr_t src, int32_t off, int32_t size)
{
    if (off < 0 || size <= 0 || size > 256) return false;
    uint8_t tmp[256];
    return SafeCopy(tmp, src + off, size) && SafeWrite(dst + off, tmp, size);
}

static bool BuildFifthRow(uintptr_t crew)
{
    CrewCtx& c = g_crew;
    uintptr_t infos1 = R<uintptr_t>(crew + c.offInfos1), actions1 = R<uintptr_t>(crew + c.offActions1);
    if (!Plausible(infos1) || !Plausible(actions1)) { Log("  crew row: template rows missing"); return false; }
    if (R<int32_t>(infos1 + c.offInfosIdx) != 1 || R<int32_t>(actions1 + c.offActionsIdx) != 1) {
        Log("  crew row: unexpected template PlayerIndex"); return false;
    }
    uintptr_t sb1 = ParentPanel(infos1), ov1 = ParentPanel(actions1);
    uintptr_t cp1 = ParentPanel(sb1), vbox = ParentPanel(cp1);
    if (ObjClass(sb1) != c.clsScaleBox || ObjClass(ov1) != c.clsOverlay || ObjClass(cp1) != c.clsCanvas ||
        ParentPanel(ov1) != cp1 || !Plausible(vbox)) {
        Log("  crew row: unexpected widget hierarchy"); return false;
    }

    // Reuse a 5th row this panel already has (Construct runs again when the menu is rebuilt, and the
    // game may keep more than one pause menu instance around). PanelWidget::Slots is at +0x178.
    {
        ScriptArr slots;  // same TArray layout: data, num, max
        if (SafeCopy(&slots, vbox + 0x178, sizeof(slots)) && slots.num >= 4 && slots.num <= 16 &&
            R<uintptr_t>(R<uintptr_t>(slots.data + 8 * 1) + 0x30) == cp1) {
            for (int i = 4; i < slots.num; i++) {
                uintptr_t row = R<uintptr_t>(R<uintptr_t>(slots.data + 8 * i) + 0x30);
                if (ObjClass(row) != c.clsCanvas) continue;
                ScriptArr kids;
                if (!SafeCopy(&kids, row + 0x178, sizeof(kids)) || kids.num != 2) continue;
                uintptr_t sb = R<uintptr_t>(R<uintptr_t>(kids.data) + 0x30);
                uintptr_t ov = R<uintptr_t>(R<uintptr_t>(kids.data + 8) + 0x30);
                ScriptArr a, b;
                if (!SafeCopy(&a, sb + 0x178, sizeof(a)) || !SafeCopy(&b, ov + 0x178, sizeof(b)) || a.num != 1 || b.num != 1) continue;
                uintptr_t inf = R<uintptr_t>(R<uintptr_t>(a.data) + 0x30), act = R<uintptr_t>(R<uintptr_t>(b.data) + 0x30);
                if (ObjClass(inf) == ObjClass(infos1) && ObjClass(act) == ObjClass(actions1) &&
                    R<int32_t>(inf + c.offInfosIdx) == 4 && R<int32_t>(act + c.offActionsIdx) == 4) {
                    c.lastCrew = crew; c.lastCanvas = row; c.lastInfos = inf; c.lastActions = act;
                    return PatchCrewArrays(c.ubergraph, act, inf, false) == kDone;
                }
            }
        }
    }

    uintptr_t infos = CallCreate(crew, ObjClass(infos1));
    uintptr_t actions = CallCreate(crew, ObjClass(actions1));
    uintptr_t canvas = CallSpawnObject(c.clsCanvas, crew);
    uintptr_t scale = CallSpawnObject(c.clsScaleBox, crew);
    uintptr_t overlay = CallSpawnObject(c.clsOverlay, crew);
    if (!Plausible(infos) || !Plausible(actions) || !Plausible(canvas) || !Plausible(scale) || !Plausible(overlay)) {
        Log("  crew row: widget creation failed"); return false;
    }
    int32_t four = 4;
    SafeWrite(infos + c.offInfosIdx, &four, 4);
    SafeWrite(actions + c.offActionsIdx, &four, 4);
    // The original mute/volume/kick popups start hidden and are shown by the panel once a player takes
    // the slot; a freshly created widget defaults to Visible, so start ours hidden too (ESlateVisibility::Hidden).
    uint8_t hidden = 2;
    SafeWrite(actions + c.offVisibility, &hidden, 1);

    // Player colour bar: the row picks green/purple/yellow/red with a switch on PlayerIndex 0..3; any
    // other index uses the switch's fallback colour (all zero = invisible). Give the new row its own
    // fallback: a pinkish magenta at the same brightness as the other four.
    if (c.offUberFrame >= 0 && c.offColorDefault >= 0) {
        uintptr_t frame = R<uintptr_t>(infos + c.offUberFrame);
        float cur[4], magenta[4] = { 0.776f, 0.060f, 0.370f, 1.0f };
        if (Plausible(frame) && SafeCopy(cur, frame + c.offColorDefault, 16) && cur[0] == 0 && cur[1] == 0 && cur[2] == 0 && cur[3] == 0)
            SafeWrite(frame + c.offColorDefault, magenta, 16);
    }

    // Copy widget settings from Slot 1's row.
    CopyBlock(scale, sb1, c.sbStretchOff, c.sbStretchSize);
    CopyBlock(scale, sb1, c.offVisibility, 1);
    CopyBlock(overlay, ov1, c.offVisibility, 1);
    CopyBlock(canvas, cp1, c.offVisibility, 1);

    // Assemble the row while it is detached (slot settings are read when the row is built).
    uintptr_t sSb = CallAddChild(c.fnAddChild, scale, infos);
    uintptr_t sOv = CallAddChild(c.fnAddChild, overlay, actions);
    uintptr_t sC1 = CallAddChild(c.fnAddChildToCanvas, canvas, scale);
    uintptr_t sC2 = CallAddChild(c.fnAddChildToCanvas, canvas, overlay);
    if (!Plausible(sSb) || !Plausible(sOv) || !Plausible(sC1) || !Plausible(sC2)) { Log("  crew row: AddChild failed"); return false; }
    CopyBlock(sSb, SlotOf(infos1), c.sbsBlockOff, c.sbsBlockSize);
    CopyBlock(sOv, SlotOf(actions1), c.osBlockOff, c.osBlockSize);
    CopyBlock(sC1, SlotOf(sb1), c.csBlockOff, c.csBlockSize);
    CopyBlock(sC2, SlotOf(ov1), c.csBlockOff, c.csBlockSize);

    // Attach below Slot 3 and copy the vertical box slot settings through the setters (box is live).
    uintptr_t sVb = CallAddChild(c.fnAddChild, vbox, canvas);
    if (!Plausible(sVb)) { Log("  crew row: adding row to the list failed"); return false; }
    uintptr_t tmplVb = SlotOf(cp1);
    uint8_t p[16];
    if (SafeCopy(p, tmplVb + c.vbSizeOff, 8)) CallUFunction(sVb, c.fnVbSetSize, p);
    if (SafeCopy(p, tmplVb + c.vbPadOff, 16)) CallUFunction(sVb, c.fnVbSetPadding, p);
    if (SafeCopy(p, tmplVb + c.vbHOff, 1)) CallUFunction(sVb, c.fnVbSetHAlign, p);
    if (SafeCopy(p, tmplVb + c.vbVOff, 1)) CallUFunction(sVb, c.fnVbSetVAlign, p);

    if (PatchCrewArrays(c.ubergraph, actions, infos, false) != kDone) { Log("  crew row: array patch failed"); return false; }
    c.lastCrew = crew; c.lastCanvas = canvas; c.lastInfos = infos; c.lastActions = actions;
    Log("  crew row: 5th row added to the Crew Members panel");
    return true;
}

static bool CrewConstructBody(uintptr_t ctx)
{
    __try {
        FindProcessEvent(ctx);
        if (g_peIndex < 0) { Log("  crew row: ProcessEvent not found"); return false; }
        return BuildFifthRow(ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("  crew row: exception 0x%08x while building the row", GetExceptionCode());
        return false;
    }
}

static void CrewConstructThunk(void* ctx, void* stack, void* result)
{
    if (g_crew.ready && ObjClass((uintptr_t)ctx) == g_crew.crewCls) CrewConstructBody((uintptr_t)ctx);
    g_crew.origConstruct(ctx, stack, result);
}

static int32_t PropOff(uintptr_t st, const char* name, int32_t expectSize = 0)
{
    PropInfo pi = FindProperty(st, name);
    if (!pi.prop || (expectSize && pi.elementSize != expectSize)) return -1;
    return pi.offset;
}

// UFunction::Func: every Blueprint function of a class holds the same pointer there
// (UObject::ProcessInternal, inside the exe's code section); native functions each hold their own
// exec thunk (also in the code section). Measured at 0xD8 on UE 5.7; only that layout is accepted,
// and only once at least 5 Blueprint functions of the class agree (i.e. the class is fully linked).
static bool EnsureFuncOffset(uintptr_t bpCls, uintptr_t anyBpFn)
{
    if (g_funcFuncOff) return true;
    const uintptr_t off = 0xD8;
    auto inText = [](uintptr_t p) {
        auto base = (uintptr_t)GetModuleHandleW(nullptr);
        auto nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        auto sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
            if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) &&
                p >= base + sec->VirtualAddress && p < base + sec->VirtualAddress + sec->Misc.VirtualSize)
                return true;
        return false;
    };
    uintptr_t n1 = MathFunc("Less_IntInt"), n2 = MathFunc("min");
    if (!n1 || !n2 || !anyBpFn) return false;
    uintptr_t pi = R<uintptr_t>(anyBpFn + off);
    uintptr_t k1 = R<uintptr_t>(n1 + off), k2 = R<uintptr_t>(n2 + off);
    int agree = 0, total = 0;
    auto it = g_funcs.find(bpCls);
    if (it != g_funcs.end())
        for (auto& kv : it->second) { total++; if (R<uintptr_t>(kv.second + off) == pi) agree++; }
    if (!inText(pi) || !inText(k1) || !inText(k2) || k1 == k2 || k1 == pi || k2 == pi || agree < 5 || agree != total)
        return false;
    g_funcFuncOff = off;
    g_processInternal = pi;
    Log("  UFunction::Func offset = 0x%llx (%d functions agree)", (unsigned long long)off, agree);
    return true;
}

static int PrepareCrewRow(uintptr_t crewCls)
{
    if (g_crew.ready && g_crew.crewCls == crewCls &&
        R<uintptr_t>(g_crew.constructFn + g_funcFuncOff) == (uintptr_t)&CrewConstructThunk)
        return kDone;
    CrewCtx c;
    c.crewCls = crewCls;
    c.constructFn = Func(crewCls, "Construct");
    c.ubergraph = Func(crewCls, "ExecuteUbergraph_WBP_PauseMenu_CrewMembers");
    auto infosIt = g_classes.find("WBP_PauseMenu_PlayerInfos_C");
    auto actionsIt = g_classes.find("WBP_PauseMenu_PlayerActions_C");
    if (!c.constructFn || !c.ubergraph || infosIt == g_classes.end() || actionsIt == g_classes.end()) return kRetry;
    ScriptArr s;
    int r = ReadyScript(c.ubergraph, kCrewUgSize, s);
    if (r != kDone) return r;
    if (PatchCrewArrays(c.ubergraph, 0, 0, true) != kDone) { Log("  crew row: panel bytecode unexpected"); return kFail; }
    if (!g_fieldNameOff) return kRetry;

    auto E = [&](const char* n) -> uintptr_t { auto it = g_engineClasses.find(Lower(n)); return it == g_engineClasses.end() ? 0 : it->second; };
    auto F = [&](const char* owner, const char* n) -> uintptr_t { uintptr_t o = E(owner); return o ? Func(o, n) : 0; };
    c.clsCanvas = E("CanvasPanel"); c.clsScaleBox = E("ScaleBox"); c.clsOverlay = E("Overlay");
    uintptr_t clsWidget = E("Widget"), clsCps = E("CanvasPanelSlot"), clsOs = E("OverlaySlot"),
              clsSbs = E("ScaleBoxSlot"), clsVbs = E("VerticalBoxSlot");
    c.fnCreate = F("WidgetBlueprintLibrary", "Create");
    c.fnSpawnObject = F("GameplayStatics", "SpawnObject");
    c.fnAddChild = F("PanelWidget", "AddChild");
    c.fnAddChildToCanvas = F("CanvasPanel", "AddChildToCanvas");
    c.fnVbSetPadding = F("VerticalBoxSlot", "SetPadding");
    c.fnVbSetSize = F("VerticalBoxSlot", "SetSize");
    c.fnVbSetHAlign = F("VerticalBoxSlot", "SetHorizontalAlignment");
    c.fnVbSetVAlign = F("VerticalBoxSlot", "SetVerticalAlignment");
    auto cdo = [&](const char* n) -> uintptr_t { auto it = g_engineCDOs.find(Lower(n)); return it == g_engineCDOs.end() ? 0 : it->second; };
    c.cdoWidgetLib = cdo("WidgetBlueprintLibrary"); c.cdoGameplayStatics = cdo("GameplayStatics");
    if (!c.clsCanvas || !c.clsScaleBox || !c.clsOverlay || !clsWidget || !clsCps || !clsOs || !clsSbs || !clsVbs ||
        !c.fnCreate || !c.fnSpawnObject || !c.fnAddChild || !c.fnAddChildToCanvas || !c.fnVbSetPadding ||
        !c.fnVbSetSize || !c.fnVbSetHAlign || !c.fnVbSetVAlign || !c.cdoWidgetLib || !c.cdoGameplayStatics)
        return kRetry;

    // Parameter layouts of the engine functions we call (verified, not assumed).
    bool ok =
        PropOff(c.fnCreate, "WorldContextObject", 8) == 0 && PropOff(c.fnCreate, "WidgetType", 8) == 8 &&
        PropOff(c.fnCreate, "OwningPlayer", 8) == 16 && PropOff(c.fnCreate, "ReturnValue", 8) == 24 &&
        PropOff(c.fnSpawnObject, "ObjectClass", 8) == 0 && PropOff(c.fnSpawnObject, "Outer", 8) == 8 &&
        PropOff(c.fnSpawnObject, "ReturnValue", 8) == 16 &&
        PropOff(c.fnAddChild, "Content", 8) == 0 && PropOff(c.fnAddChild, "ReturnValue", 8) == 8 &&
        PropOff(c.fnAddChildToCanvas, "Content", 8) == 0 && PropOff(c.fnAddChildToCanvas, "ReturnValue", 8) == 8 &&
        PropOff(c.fnVbSetPadding, "InPadding", 16) == 0 && PropOff(c.fnVbSetSize, "InSize", 8) == 0 &&
        PropOff(c.fnVbSetHAlign, "InHorizontalAlignment", 1) == 0 && PropOff(c.fnVbSetVAlign, "InVerticalAlignment", 1) == 0;
    if (!ok) { Log("  crew row: engine function parameters unexpected"); return kFail; }

    c.offInfos1 = PropOff(crewCls, "WBP_PauseMenu_PlayerInfos_1", 8);
    c.offActions1 = PropOff(crewCls, "WBP_PauseMenu_PlayerActions_1", 8);
    c.offInfosIdx = PropOff(infosIt->second, "PlayerIndex", 4);
    c.offActionsIdx = PropOff(actionsIt->second, "PlayerIndex", 4);
    c.offVisibility = PropOff(clsWidget, "Visibility", 1);
    {
        uintptr_t infosUg = Func(infosIt->second, "ExecuteUbergraph_WBP_PauseMenu_PlayerInfos");
        c.offUberFrame = PropOff(infosIt->second, "UberGraphFrame", 8);
        c.offColorDefault = infosUg ? PropOff(infosUg, "K2Node_Select_Default_8", 16) : -1;
        if (c.offUberFrame < 0 || c.offColorDefault < 0) Log("  crew row: colour fallback not found (row will have no colour bar)");
    }
    int32_t st = PropOff(c.clsScaleBox, "Stretch", 1), sd = PropOff(c.clsScaleBox, "StretchDirection", 1),
            us = PropOff(c.clsScaleBox, "UserSpecifiedScale", 4);
    int32_t ld = PropOff(clsCps, "LayoutData"), az = PropOff(clsCps, "bAutoSize"), zo = PropOff(clsCps, "ZOrder", 4);
    int32_t op = PropOff(clsOs, "Padding", 16), oh = PropOff(clsOs, "HorizontalAlignment", 1), ov = PropOff(clsOs, "VerticalAlignment", 1);
    int32_t sh = PropOff(clsSbs, "HorizontalAlignment", 1), sv = PropOff(clsSbs, "VerticalAlignment", 1);
    c.vbSizeOff = PropOff(clsVbs, "Size", 8); c.vbPadOff = PropOff(clsVbs, "Padding", 16);
    c.vbHOff = PropOff(clsVbs, "HorizontalAlignment", 1); c.vbVOff = PropOff(clsVbs, "VerticalAlignment", 1);
    if (c.offInfos1 < 0 || c.offActions1 < 0 || c.offInfosIdx < 0 || c.offActionsIdx < 0 || c.offVisibility < 0 ||
        st < 0 || sd < 0 || us < 0 || ld < 0 || az < 0 || zo < 0 || op < 0 || oh < 0 || ov < 0 || sh < 0 || sv < 0 ||
        c.vbSizeOff < 0 || c.vbPadOff < 0 || c.vbHOff < 0 || c.vbVOff < 0) {
        Log("  crew row: widget property missing"); return kFail;
    }
    int32_t lo = st < sd ? st : sd, hi = us + 4;
    if (st + 1 > hi) hi = st + 1;
    if (sd + 1 > hi) hi = sd + 1;
    c.sbStretchOff = lo; c.sbStretchSize = hi - lo;
    c.csBlockOff = ld; c.csBlockSize = zo + 4 - ld;
    c.osBlockOff = op; c.osBlockSize = (oh > ov ? oh : ov) + 1 - op;
    c.sbsBlockOff = sh < sv ? sh : sv; c.sbsBlockSize = (sh > sv ? sh : sv) + 1 - c.sbsBlockOff;
    if (c.sbStretchSize <= 0 || c.sbStretchSize > 16 || c.csBlockSize != 72 || c.osBlockSize != 18 || c.sbsBlockSize != 2) {
        Log("  crew row: widget property layout unexpected (%d %d %d %d)", c.sbStretchSize, c.csBlockSize, c.osBlockSize, c.sbsBlockSize);
        return kFail;
    }

    if (!EnsureFuncOffset(crewCls, c.ubergraph)) return kRetry;
    uintptr_t cur = R<uintptr_t>(c.constructFn + g_funcFuncOff);
    if (cur != g_processInternal || R<uintptr_t>(c.ubergraph + g_funcFuncOff) != g_processInternal) return kRetry;  // not linked yet
    c.origConstruct = (NativeFn)cur;
    c.ready = true;
    g_crew = c;
    uintptr_t thunk = (uintptr_t)&CrewConstructThunk;
    return SafeWrite(c.constructFn + g_funcFuncOff, &thunk, 8) ? kDone : kFail;
}
