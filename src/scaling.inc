// ---------------------------------------------------------------------------------------------
// Monster / mission scaling by player count: 5 players use the 4-player values
// ---------------------------------------------------------------------------------------------
// Many Blueprints pick a value with a "Select" on the number of players, with cases 0..4 only
// (monster speed/dodge/combat probabilities, spawn counts, keypad length, riddle and printer timings,
// genetics-lab counts). With 5 players they fell through to the Select's default value, which is never
// set (0) - e.g. monster speed x0, spawn counts 0, keypad code length 0.
//
// EX_SwitchValue layout: 0x69 | u16 NumCases | u32 EndOffset | IndexTerm | { CaseValue | u32 NextCase | CaseTerm }* | DefaultTerm
// Every switch listed here has a 9-byte local-variable default and a 9-byte local-variable case-4 term,
// so we make the default read the same variable as case 4.

struct CountSwitch {
    const char* cls; const char* fn; int fnSize; int stmt; int stmtLen; const char* indexVar;
};

static const CountSwitch kCountSwitches[] = {
    { "BP_ConsoleKeypass_C", "ExecuteUbergraph_BP_ConsoleKeypass", 4124, 322, 133, "Temp_int_Variable" },
    { "BP_BaseMonster_C", "BPI_Monster_CanDodge", 957, 687, 152, "Temp_int_Variable" },
    { "BP_BaseMonster_C", "Damage [Hit] DecreaseAccumulation", 803, 499, 152, "Temp_int_Variable" },
    { "BP_BaseMonster_C", "GetDecreaseFrostSpeed", 363, 181, 152, "Temp_int_Variable" },
    { "BP_BaseMonster_C", "GetSpeedDifficulty", 987, 778, 152, "Temp_int_Variable" },
    { "BP_BaseMonster_C", "PROBA_Combat", 3258, 630, 152, "Temp_int_Variable" },
    { "BP_BaseMonster_C", "PROBA_Combat", 3258, 2174, 152, "Temp_int_Variable" },
    { "BP_BaseMonster_C", "PROBA_Patrol", 2610, 2257, 152, "Temp_int_Variable" },
    { "BP_BigRobot_C", "BPI_BigRobot_CanDashAttack", 1572, 628, 152, "Temp_int_Variable" },
    { "BP_BigRobot_C", "CheckPlayersForSpawnSphere", 8687, 3607, 252, "Temp_int_Variable" },
    { "BP_BigRobot_C", "GetSpeedDifficulty", 987, 778, 152, "Temp_int_Variable" },
    { "BP_BigRobot_C", "PROBA_Patrol", 2562, 2083, 152, "Temp_int_Variable" },
    { "BP_BigRobot_C", "ServerOnlyFunction", 755, 509, 152, "Temp_int_Variable" },
    { "BP_Monster_Eye_C", "BPI_Monster_GetSpeedByType", 1762, 1024, 267, "Temp_int_Variable" },
    { "BP_Monster_Eye_C", "BPI_Monster_GetSpeedByType", 1762, 1024, 267, "Temp_int_Variable_1" },
    { "BP_Monster_Poulpi_C", "CheckTemporalDamage", 914, 277, 152, "Temp_int_Variable" },
    { "BP_Monster_ZombieMaster_C", "CheckPlayersForSpawnZombies", 13868, 2252, 252, "Temp_int_Variable" },
    { "BP_SnowMonster_Spawner_C", "AdjustNumber", 1145, 736, 152, "Temp_int_Variable" },
    { "BP_SnowMonster_Spawner_Electrical_C", "AdjustNumber", 725, 361, 152, "Temp_int_Variable" },
    { "BP_GeneticsLab_Manager_C", "CheckSpawnMorayLoop", 4355, 2080, 667, "Temp_int_Variable_15" },
    { "BP_GeneticsLab_Manager_C", "CheckSpawnMorayLoop", 4355, 2080, 667, "Temp_int_Variable_9" },
    { "BP_GeneticsLab_Manager_C", "CheckSpawnMorayLoop", 4355, 2080, 667, "Temp_int_Variable_3" },
    { "BP_GeneticsLab_Manager_C", "CheckSpawnMorayLoop", 4355, 2080, 667, "Temp_int_Variable_25" },
    { "BP_GeneticsLab_Manager_C", "CheckSpawnMorayLoop", 4355, 2080, 667, "Temp_int_Variable_29" },
    { "BP_GeneticsLab_Manager_C", "ExecuteUbergraph_BP_GeneticsLab_Manager", 15535, 7543, 648, "Temp_int_Variable_30" },
    { "BP_GeneticsLab_Manager_C", "ExecuteUbergraph_BP_GeneticsLab_Manager", 15535, 7543, 648, "Temp_int_Variable_1" },
    { "BP_GeneticsLab_Manager_C", "ExecuteUbergraph_BP_GeneticsLab_Manager", 15535, 7543, 648, "Temp_int_Variable_7" },
    { "BP_GeneticsLab_Manager_C", "ExecuteUbergraph_BP_GeneticsLab_Manager", 15535, 7543, 648, "Temp_int_Variable_13" },
    { "BP_GeneticsLab_Manager_C", "ExecuteUbergraph_BP_GeneticsLab_Manager", 15535, 7543, 648, "Temp_int_Variable_19" },
    { "BP_Laboratory_PuzzleContainer_C", "SpawnHelpersCannisters", 3341, 475, 140, "Temp_int_Variable_1" },
    { "BP_Laboratory_PuzzleContainer_C", "SpawnHelpersCannisters", 3341, 794, 148, "Temp_int_Variable_1" },
    { "BP_ERAccess_RiddleSystem_C", "ExecuteUbergraph_BP_ERAccess_RiddleSystem", 3844, 393, 133, "Temp_int_Variable_5" },
    { "BP_ERAccess_RiddleSystem_C", "ExecuteUbergraph_BP_ERAccess_RiddleSystem", 3844, 759, 133, "Temp_int_Variable_5" },
    { "BP_3DPrinterTerminal_C", "ExecuteUbergraph_BP_3DPrinterTerminal", 1818, 683, 133, "Temp_int_Variable" },
};

static int PatchCountSwitch(uintptr_t fn, const CountSwitch& cs)
{
    ScriptArr s;
    int r = ReadyScript(fn, cs.fnSize, s);
    if (r != kDone) return r;
    std::vector<uint8_t> b(cs.stmtLen);
    if (!SafeCopy(b.data(), s.data + cs.stmt, cs.stmtLen)) return kRetry;
    auto u16 = [&](int i) { uint16_t v; memcpy(&v, &b[i], 2); return (int)v; };
    auto u32 = [&](int i) { return GetI32(&b[i]); };
    bool unresolved = false;
    for (int i = 0; i + 17 <= cs.stmtLen; i++) {
        if (b[i] != 0x69 || u16(i + 1) != 5) continue;
        int end = u32(i + 3) - cs.stmt;                       // absolute -> statement-relative
        if (end <= i || end > cs.stmtLen) continue;
        if (b[i + 7] != 0x00) continue;                        // IndexTerm: EX_LocalVariable
        if (!PropResolved(&b[i + 8])) { unresolved = true; continue; }
        if (!PropIs(&b[i + 8], cs.indexVar)) continue;
        // Walk the 5 cases.
        int pos = i + 16, termStart[5], termEnd[5];
        bool ok = true;
        for (int c = 0; c < 5 && ok; c++) {
            int len, val;
            if (b[pos] == 0x1D) { len = 5; val = GetI32(&b[pos + 1]); }
            else if (b[pos] == 0x25) { len = 1; val = 0; }
            else if (b[pos] == 0x26) { len = 1; val = 1; }
            else { ok = false; break; }
            if (val != c) { ok = false; break; }
            int next = u32(pos + len) - cs.stmt;
            termStart[c] = pos + len + 4; termEnd[c] = next;
            if (next <= termStart[c] || next > end) { ok = false; break; }
            pos = next;
        }
        if (!ok) continue;
        int defStart = pos, defLen = end - pos, t4Len = termEnd[4] - termStart[4];
        if (defLen != 9 || t4Len != 9 || b[defStart] != 0x00 || b[termStart[4]] != 0x00) return kFail;
        if (!PropResolved(&b[defStart + 1]) || !PropResolved(&b[termStart[4] + 1])) return kRetry;
        if (memcmp(&b[defStart], &b[termStart[4]], 9) == 0) return kDone;   // already patched
        return SafeWrite(s.data + cs.stmt + defStart, &b[termStart[4]], 9) ? kDone : kFail;
    }
    return unresolved ? kRetry : kFail;
}

// Balancing_FunctionLibrary_C::GetNbPlayersFloatValue(row, nb_players) reads the column for 1..4
// players from a data table (sentry guns). Its case-4 term is a table column, so instead of the default
// we clamp the player count:
//   @101 Temp_real_Variable = 1.0;  @128 Temp_int_Variable = nb_players;     (27 + 27 bytes)
//   -> Temp_int_Variable = Clamp(nb_players, 1, 4);  + padding               (the 1.0 only served "0 players")
static int PatchBalancingClamp(uintptr_t fn)
{
    const int size = 327, at = 101, len = 54;
    ScriptArr s;
    int r = ReadyScript(fn, size, s);
    if (r != kDone) return r;
    uintptr_t clampFn = MathFunc("Clamp");
    if (!clampFn) return kRetry;
    uint8_t b[len];
    if (!SafeCopy(b, s.data + at, len)) return kRetry;
    if (b[0] == 0x0F && b[18] == 0x68 && GetPtr(&b[19]) == clampFn) return kDone;
    // original: 0F <Temp_real> 00 <Temp_real> 37 <1.0> | 0F <Temp_int> 00 <Temp_int> 00 <nb_players>
    double one; memcpy(&one, &b[19], 8);
    if (b[0] != 0x0F || b[9] != 0x00 || b[18] != 0x37 || one != 1.0 || GetPtr(&b[1]) != GetPtr(&b[10]) ||
        b[27] != 0x0F || b[36] != 0x00 || b[45] != 0x00 || GetPtr(&b[28]) != GetPtr(&b[37]))
        return kFail;
    if (!PropIs(&b[28], "Temp_int_Variable") || !PropIs(&b[46], "nb_players")) return kRetry;
    uint8_t nb[len];
    memset(nb, 0x0B, sizeof(nb));
    int p = 0;
    nb[p++] = 0x0F; memcpy(nb + p, &b[28], 8); p += 8;          // Temp_int_Variable =
    nb[p++] = 0x00; memcpy(nb + p, &b[37], 8); p += 8;
    nb[p++] = 0x68; PutPtr(nb + p, clampFn); p += 8;            //   Clamp(
    nb[p++] = 0x00; memcpy(nb + p, &b[46], 8); p += 8;          //     nb_players,
    nb[p++] = 0x26;                                             //     1,
    nb[p++] = 0x1D; int32_t four = 4; memcpy(nb + p, &four, 4); p += 4;   // 4
    nb[p++] = 0x16;                                             //   )
    return SafeWrite(s.data + at, nb, sizeof(nb)) ? kDone : kFail;
}

// Returns true if this class has scaling patches (and fills the steps).
static bool AddScalingSteps(const std::string& cls, uintptr_t clsPtr, std::vector<Step>& st)
{
    bool any = false;
    if (cls == "Balancing_FunctionLibrary_C") {
        st.push_back({ "sentry-gun balancing uses the 4-player column for 5 players",
                       PatchBalancingClamp(Func(clsPtr, "GetNbPlayersFloatValue")) });
        return true;
    }
    for (const auto& cs : kCountSwitches) {
        if (cls != cs.cls) continue;
        any = true;
        st.push_back({ cs.fn, PatchCountSwitch(Func(clsPtr, cs.fn), cs) });
    }
    if (any) st.insert(st.begin(), { "5 players use the 4-player values in:", kDone });
    return any;
}
