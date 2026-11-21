// ---------------------------------------------------------------------------------------------
// Seat 5 on other players' machines
// ---------------------------------------------------------------------------------------------
// The 4 original seats are placed in the map, so every machine has them with ParentShip filled in.
// Seat 5 is spawned by the host and replicated, but BP_Seat_C::ParentShip is not a replicated
// property, so on other players' machines seat 5 arrives without a ship link. ParentShip is only used
// by the seat to hook its screen to the ship's screen events (BeginPlay), so without it seat 5's screen
// stays blank for them.
//
// We hook BP_Seat_C::ReceiveBeginPlay (native Func swap, game thread). If the seat has no ParentShip we
// set it to the level's BP_PlayerShip before the seat's own BeginPlay runs, and afterwards ask the ship
// to rebuild its seat-screen list (SetUpChairLocal, see PatchChairLocal).

struct SeatCtx {
    bool ready = false;
    uintptr_t seatCls = 0, beginPlayFn = 0, shipCls = 0, setUpChairLocalFn = 0;
    int32_t offParentShip = -1;
    int32_t offRole = -1;                 // AActor::Role (3 = authority)
    uintptr_t fnAddLocalOffset = 0;       // AActor::K2_AddActorLocalOffset
    NativeFn origBeginPlay = nullptr;
};
// Where seat 5 goes, relative to the seats' shared anchor (front centre; seats 1/2 sit at y = -/+110,
// seats 3/4 at x = -90, y = -/+240). Default: behind the back row's centre. Overridable in
// FivePlayerMod.ini next to the DLL:  [Seat5]  X=-130  Y=0  Z=0   (Unreal units, x = forward)
static double kSeat5Offset[3] = { -130.0, 0.0, 0.0 };
static constexpr uint32_t RF_WasLoaded = 0x80000;
static SeatCtx g_seat;

static uintptr_t FindShipInstance(uintptr_t shipCls)
{
    int n = ObjCount();
    for (int i = 0; i < n; i++) {
        uintptr_t o = ObjAt(i);
        if (Plausible(o) && ObjClass(o) == shipCls && !(ObjFlags(o) & (RF_ClassDefaultObject | RF_ArchetypeObject)))
            return o;
    }
    return 0;
}

static uintptr_t SeatPreBeginPlay(uintptr_t seat)
{
    __try {
        if (R<uintptr_t>(seat + g_seat.offParentShip) != 0) return 0;  // host / map-placed seats: already linked
        uintptr_t ship = FindShipInstance(g_seat.shipCls);
        if (!Plausible(ship)) return 0;
        SafeWrite(seat + g_seat.offParentShip, &ship, 8);
        return ship;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void SeatPostBeginPlay(uintptr_t seat, uintptr_t ship)
{
    __try {
        FindProcessEvent(seat);
        if (g_peIndex < 0) { Log("  seat 5: ProcessEvent not found"); return; }
        uint8_t parms[16] = {};
        CallUFunction(ship, g_seat.setUpChairLocalFn, parms);
        Log("  seat 5: linked to the ship and added to its seat screens");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("  seat 5: exception 0x%08x while linking", GetExceptionCode());
    }
}

// Host only: the seat the ship spawned (not one placed in the map) is moved to the back row centre
// before the ship attaches it; its position then replicates to everyone.
static void MoveSpawnedSeat(uintptr_t seat)
{
    __try {
        if (ObjFlags(seat) & RF_WasLoaded) return;                 // map-placed seats 1-4
        if (!g_seat.fnAddLocalOffset) { Log("  seat 5: not moved (offset function unavailable)"); return; }
        uint8_t role = R<uint8_t>(seat + g_seat.offRole);
        if (role != 3) { Log("  seat 5: not moved here (role %u, the host moves it)", role); return; }  // only the host moves it
        FindProcessEvent(seat);
        if (g_peIndex < 0) { Log("  seat 5: not moved (ProcessEvent slot unknown)"); return; }
        alignas(16) uint8_t parms[320] = {};
        memcpy(parms + 0, kSeat5Offset, 24);  // DeltaLocation
        parms[24] = 0;                        // bSweep
        parms[288] = 1;                       // bTeleport
        CallUFunction(seat, g_seat.fnAddLocalOffset, parms);
        Log("  seat 5: moved to the back row, centre");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("  seat 5: exception 0x%08x while moving it", GetExceptionCode());
    }
}

static void SeatBeginPlayThunk(void* ctx, void* stack, void* result)
{
    uintptr_t ship = 0;
    if (g_seat.ready && ObjClass((uintptr_t)ctx) == g_seat.seatCls) {
        MoveSpawnedSeat((uintptr_t)ctx);
        ship = SeatPreBeginPlay((uintptr_t)ctx);
    }
    g_seat.origBeginPlay(ctx, stack, result);
    if (ship) SeatPostBeginPlay((uintptr_t)ctx, ship);
}

static int PrepareSeatHook(uintptr_t seatCls)
{
    if (g_seat.ready && g_seat.seatCls == seatCls &&
        R<uintptr_t>(g_seat.beginPlayFn + g_funcFuncOff) == (uintptr_t)&SeatBeginPlayThunk)
        return kDone;
    SeatCtx c;
    c.seatCls = seatCls;
    c.beginPlayFn = Func(seatCls, "ReceiveBeginPlay");
    uintptr_t ug = Func(seatCls, "ExecuteUbergraph_BP_Seat");
    auto shipIt = g_classes.find("BP_PlayerShip_C");
    if (!c.beginPlayFn || !ug || shipIt == g_classes.end() || !g_fieldNameOff) return kRetry;
    c.shipCls = shipIt->second;
    c.setUpChairLocalFn = Func(c.shipCls, "SetUpChairLocal");
    if (!c.setUpChairLocalFn) return kRetry;
    PropInfo ps = FindProperty(seatCls, "ParentShip");
    if (!ps.prop || ps.elementSize != 8 || ps.arrayDim != 1 || ps.offset < 0x28) { Log("  seat 5: ParentShip property unexpected"); return kFail; }
    c.offParentShip = ps.offset;
    {
        auto it = g_engineClasses.find("actor");
        if (it == g_engineClasses.end()) return kRetry;
        uintptr_t actorCls = it->second;
        PropInfo role = FindProperty(actorCls, "Role");
        uintptr_t fn = Func(actorCls, "K2_AddActorLocalOffset");
        if (!fn) return kRetry;
        PropInfo dl = FindProperty(fn, "DeltaLocation"), sw = FindProperty(fn, "bSweep"),
                 hr = FindProperty(fn, "SweepHitResult"), tp = FindProperty(fn, "bTeleport");
        if (!role.prop || role.elementSize != 1 || dl.offset != 0 || dl.elementSize != 24 || sw.offset != 24 ||
            hr.offset != 32 || hr.elementSize > 256 || tp.offset != 288) {
            Log("  seat 5: AddActorLocalOffset/Role layout unexpected; seat stays at the front centre");
        } else {
            c.offRole = role.offset;
            c.fnAddLocalOffset = fn;
        }
    }
    if (!EnsureFuncOffset(seatCls, ug)) return kRetry;
    uintptr_t cur = R<uintptr_t>(c.beginPlayFn + g_funcFuncOff);
    if (cur != g_processInternal || R<uintptr_t>(ug + g_funcFuncOff) != g_processInternal) return kRetry;  // not linked yet
    c.origBeginPlay = (NativeFn)cur;
    c.ready = true;
    g_seat = c;
    uintptr_t thunk = (uintptr_t)&SeatBeginPlayThunk;
    return SafeWrite(c.beginPlayFn + g_funcFuncOff, &thunk, 8) ? kDone : kFail;
}
