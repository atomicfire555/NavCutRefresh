# NavCutRefresh (xNVSE plugin) + Wasteland Forts - NavCut patch

Makes NPCs path around objects you build in Wasteland Forts, using New Vegas's own
runtime navmesh-cutting system (NavMeshObstacleManager). Nothing is regenerated; the
engine cuts the existing navmesh under each flagged object.

## Why it didn't work before

Found by disassembling FalloutNV.exe 1.4.0.525:

1. An object only cuts navmesh if its **base form has the Obstacle flag** (0x02000000) and
   its collision uses one of these layers: **ANIMSTATIC, CLUTTER, PROPS, DEBRIS_LARGE,
   TRANSPARENT_SMALL_ANIM**. STATIC never cuts (collector at 0x6C2210).
2. The cut is built when the object's 3D loads (or when its cell loads). **Moving the object
   afterwards (SetPos, SetAngle, MoveTo, JIP RotateAroundPoint) never updates it.** Only
   doors are re-cut on move. Wasteland Forts spawns each piece as a preview floating in
   front of you, then moves it into place, so the cut was built in mid-air and stayed there.
3. Calling `NavMeshObstacleManager::AddObstacle` (0x6C0C30) again on an already-registered
   object makes the engine undo its old cuts and rebuild them from where the object is now.

## What's in it

- **`Wasteland Forts - NavCut.esp`** (masters: the 5 DLCs + Wasteland Forts.esm). It adds the
  Obstacle flag to 588 buildable objects: 481 STAT, 55 CONT, 49 ACTI, 3 TERM.
  - 128 of them used STATIC-collision meshes; these point at copies under
    `meshes\WastelandForts\NavCut\` with the layer changed to ANIMSTATIC (2-4 bytes per mesh).
    The originals are untouched.
  - 455 use vanilla meshes, so they're flagged only. They cut if the vanilla mesh already has a
    valid layer; ones with STATIC collision are harmless no-ops until converted (needs your
    mesh BSAs).
  - Not flagged, on purpose: floors, pallets, stairs, rugs and other walk-on pieces; enterable
    buildings (tents, shacks, barracks, towers); furniture NPCs sit or sleep in; doors (the
    engine already handles them); wall-mounted or flat items; tiny clutter; and markers.
    `docs/object_list.csv` shows the decision for every object.
- **`NavCutRefresh.dll`**: every 10 frames it checks loaded cells for flagged objects that
  moved. Once a moved object holds still for 0.4 s, it asks the engine to rebuild that
  object's cut. It has no hooks or patches and only calls the engine's own function.
  Settings are in `NVSE\Plugins\NavCutRefresh.ini`.

## Building the DLL

GitHub Actions builds it on every push (`.github/workflows/build.yml`). The workflow
checks out xNVSE at a pinned commit, drops `src/main.cpp` into its example plugin project
and builds **Release | Win32** with MSVC v143. Download the **NavCutRefresh** artifact from
the run: it holds `NVSE\Plugins\NavCutRefresh.dll` and `NavCutRefresh.ini`. Copy them into
the patch mod's `NVSE\Plugins` folder.

To build locally instead, open xNVSE's `nvse_plugin_example.sln`, replace its `main.cpp`
with `src/main.cpp`, and build Release | Win32.

Requires xNVSE and the 1.4.0.525 runtime. The plugin refuses to load on any other exe.

## Install (MO2)

Install the patch folder as its own mod and enable `Wasteland Forts - NavCut.esp` after
`Wasteland Forts.esm`. It only overrides Wasteland Forts' own records, so it's safe to
uninstall: remove it and the flags and meshes revert.

## Testing

1. MCM -> Wasteland Forts -> turn ON "Turn OFF: Stuck watchdog", so it doesn't teleport
   NPCs and hide the result.
2. Build a wall or junk wall across a route a settler walks.
3. `NavCutRefresh.log` (in your game folder, next to `nvse.log`) should show
   `refreshed <refID> ...` about half a second after you place it.
4. Watch the settler go around it. If they still walk into it, send the log and tell me
   which object you used.
5. Set `bVerboseLog=0` when you're done.

## Addresses used (FalloutNV.exe 1.4.0.525)

| Address | What |
|---|---|
| 0x6C0720 | `NavMeshObstacleManager::GetSingleton()` (cdecl, returns 0x11D71E0) |
| 0x6C0C30 | `AddObstacle(TESObjectREFR*)` (thiscall); queues, or rebuilds if already registered |
| 0x6C0C80 | `RemoveObstacle(TESObjectREFR*)` (not used) |
| REFR vtbl +0x15C | `IsObstacle()`, checks the base form flag |
| REFR vtbl +0x1D0 | 3D root (the engine checks this before processing) |
| 0x11D73E4 | `bUseObstacleAvoidance:Pathfinding`, default 1 |
