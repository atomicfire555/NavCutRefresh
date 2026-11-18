// NavCutRefresh - xNVSE plugin for Fallout: New Vegas 1.4.0.525
//
// Keeps runtime navmesh cuts (the "Obstacle" flag) in sync with objects that are moved after they load.
//
// Why: FalloutNV.exe registers an obstacle with NavMeshObstacleManager when a reference's 3D is set
// (TESObjectREFR vtable 0x1CC -> 0x5702E0) or when a cell attaches (0x5575D0). Moving a reference with
// SetPos / SetAngle / MoveTo / RotateAroundPoint never re-registers it (only doors are updated on move),
// so a building piece that was spawned and then positioned keeps the cut it had where it spawned, which
// for Wasteland Forts is the preview hovering in front of the player. Calling
// NavMeshObstacleManager::AddObstacle (0x6C0C30) on a reference that is already registered makes the
// manager undo its old cuts and rebuild them from the current 3D (0x6C45E0 -> 0x6C1920 -> 0x6C3520).
//
// What this plugin does: every few frames it walks the loaded cells around the player and, for each
// enabled reference whose base form has the Obstacle flag, remembers its position/rotation/scale. When
// one changes and then holds still for a moment, it asks the manager to rebuild that reference's cut.
// No hooks, no patches: it only calls the engine's own registration function.
//
// Addresses verified against FalloutNV.exe 1.4.0.525 (Steam), disassembled after Steamless:
//   0x6C0720  NavMeshObstacleManager *GetSingleton()           cdecl, no args, returns 0x11D71E0
//   0x6C0C30  void NavMeshObstacleManager::AddObstacle(REFR*)  thiscall, ret 4, gated by
//             bUseObstacleAvoidance:Pathfinding (default 1); queues the ref, processed next manager update
//   TESObjectREFR vtable +0x15C  bool IsObstacle()  -> base form has the 0x02000000 Obstacle flag
//   TESObjectREFR vtable +0x1D0  NiNode *GetNiNode() (what the manager itself uses before processing)
//   Collision layers that produce a cut (collector 0x6C2210): ANIMSTATIC, CLUTTER, PROPS, DEBRIS_LARGE,
//   TRANSPARENT_SMALL_ANIM. STATIC does not.

#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "nvse/Utilities.h"
#include <unordered_map>
#include <vector>
#include <cmath>
#include <algorithm>

IDebugLog gLog("NavCutRefresh.log");

namespace
{
	constexpr UInt32 kAddr_ObstacleManager_GetSingleton = 0x6C0720;
	constexpr UInt32 kAddr_ObstacleManager_AddObstacle = 0x6C0C30;
	constexpr UInt32 kVtbl_IsObstacle = 0x15C;
	constexpr UInt32 kVtbl_GetNiNode = 0x1D0;
	constexpr UInt32 kRefFlag_Disabled = 0x00000800;
	constexpr UInt32 kRefFlag_Deleted = 0x00000020;

	struct Config
	{
		bool enabled = true;
		int scanEveryFrames = 10;          // how often loaded cells are scanned
		int settleMs = 400;                // a moved object must hold still this long before its cut is rebuilt
		int maxRefreshPerScan = 24;        // spreads work out when many objects settle at once
		bool refreshCreatedOnSight = true; // rebuild cuts of player-placed (0xFF) objects once when first seen
		int cellRadius = 2;                // exterior cells around the player (2 = the default 5x5 grid)
		bool verbose = false;
	} g_cfg;

	struct Tracked
	{
		float pos[3];
		float rot[3];
		float scale;
		DWORD changedAt;
		UInt32 lastSeenScan;
		bool pending;
	};

	std::unordered_map<UInt32, Tracked> g_tracked;
	UInt32 g_frame = 0;
	UInt32 g_scan = 0;
	UInt32 g_totalRefreshed = 0;
	bool g_inGame = false;

	template <typename T>
	T VCall(const void *obj, UInt32 offset)
	{
		const UInt32 fn = (*reinterpret_cast<UInt32 *const *>(obj))[offset / 4];
		return ThisStdCall<T>(fn, obj);
	}

	void *ObstacleManager() { return CdeclCall<void *>(kAddr_ObstacleManager_GetSingleton); }

	void RefreshObstacle(TESObjectREFR *ref)
	{
		ThisStdCall<void>(kAddr_ObstacleManager_AddObstacle, ObstacleManager(), ref);
	}

	bool Moved(const Tracked &t, const TESObjectREFR *r)
	{
		constexpr float kPos = 0.5f, kRot = 0.002f, kScale = 0.001f;
		return std::fabs(t.pos[0] - r->posX) > kPos || std::fabs(t.pos[1] - r->posY) > kPos ||
			   std::fabs(t.pos[2] - r->posZ) > kPos || std::fabs(t.rot[0] - r->rotX) > kRot ||
			   std::fabs(t.rot[1] - r->rotY) > kRot || std::fabs(t.rot[2] - r->rotZ) > kRot ||
			   std::fabs(t.scale - r->scale) > kScale;
	}

	void Snapshot(Tracked &t, const TESObjectREFR *r)
	{
		t.pos[0] = r->posX; t.pos[1] = r->posY; t.pos[2] = r->posZ;
		t.rot[0] = r->rotX; t.rot[1] = r->rotY; t.rot[2] = r->rotZ;
		t.scale = r->scale;
	}

	bool Candidate(TESObjectREFR *ref)
	{
		if (!ref || ref->typeID != kFormType_TESObjectREFR || !ref->baseForm)
			return false;
		if (ref->flags & (kRefFlag_Disabled | kRefFlag_Deleted))
			return false;
		if (!VCall<bool>(ref, kVtbl_IsObstacle))
			return false;
		return VCall<void *>(ref, kVtbl_GetNiNode) != nullptr;
	}

	void CollectCells(std::vector<TESObjectCELL *> &out)
	{
		PlayerCharacter *pc = *g_thePlayer;
		if (!pc || !pc->parentCell)
			return;
		TESObjectCELL *cell = pc->parentCell;
		TESWorldSpace *world = cell->worldSpace;
		if (!world || !cell->cellData || !world->cellMap)
		{
			out.push_back(cell);
			return;
		}
		const SInt32 cx = static_cast<SInt32>(cell->cellData->x);
		const SInt32 cy = static_cast<SInt32>(cell->cellData->y);
		for (SInt32 dx = -g_cfg.cellRadius; dx <= g_cfg.cellRadius; ++dx)
			for (SInt32 dy = -g_cfg.cellRadius; dy <= g_cfg.cellRadius; ++dy)
			{
				const SInt32 x = cx + dx, y = cy + dy;
				const UInt32 key = (static_cast<UInt32>(x) << 16) | (static_cast<UInt32>(y) & 0xFFFF);
				if (TESObjectCELL *c = world->cellMap->Lookup(key))
					out.push_back(c);
			}
	}

	void Scan()
	{
		++g_scan;
		const DWORD now = GetTickCount();
		int budget = g_cfg.maxRefreshPerScan;
		std::vector<TESObjectCELL *> cells;
		CollectCells(cells);

		for (TESObjectCELL *cell : cells)
		{
			for (auto *node = cell->objectList.Head(); node; node = node->next)
			{
				TESObjectREFR *ref = node->data;
				if (!Candidate(ref))
					continue;
				auto it = g_tracked.find(ref->refID);
				if (it == g_tracked.end())
				{
					Tracked t{};
					Snapshot(t, ref);
					t.changedAt = now;
					t.lastSeenScan = g_scan;
					t.pending = g_cfg.refreshCreatedOnSight && (ref->refID >> 24) == 0xFF;
					g_tracked.emplace(ref->refID, t);
					continue;
				}
				Tracked &t = it->second;
				t.lastSeenScan = g_scan;
				if (Moved(t, ref))
				{
					Snapshot(t, ref);
					t.changedAt = now;
					t.pending = true;
				}
				else if (t.pending && budget > 0 && now - t.changedAt >= static_cast<DWORD>(g_cfg.settleMs))
				{
					RefreshObstacle(ref);
					t.pending = false;
					--budget;
					++g_totalRefreshed;
					if (g_cfg.verbose)
						_MESSAGE("refreshed %08X (base %08X) at %.0f %.0f %.0f", ref->refID, ref->baseForm->refID,
								 ref->posX, ref->posY, ref->posZ);
				}
			}
		}

		if ((g_scan & 63) == 0) // forget refs that have been out of range for a while
		{
			for (auto it = g_tracked.begin(); it != g_tracked.end();)
				it = (g_scan - it->second.lastSeenScan > 64) ? g_tracked.erase(it) : std::next(it);
		}
	}

	void ReadConfig()
	{
		char path[MAX_PATH];
		GetModuleFileNameA(nullptr, path, MAX_PATH);
		char *slash = strrchr(path, '\\');
		if (slash)
			*(slash + 1) = 0;
		strcat_s(path, "Data\\NVSE\\Plugins\\NavCutRefresh.ini");
		auto get = [&](const char *key, int def) { return static_cast<int>(GetPrivateProfileIntA("Main", key, def, path)); };
		g_cfg.enabled = get("bEnabled", 1) != 0;
		g_cfg.scanEveryFrames = (std::max)(1, get("iScanEveryFrames", g_cfg.scanEveryFrames));
		g_cfg.settleMs = (std::max)(0, get("iSettleMs", g_cfg.settleMs));
		g_cfg.maxRefreshPerScan = (std::max)(1, get("iMaxRefreshPerScan", g_cfg.maxRefreshPerScan));
		g_cfg.refreshCreatedOnSight = get("bRefreshPlayerPlacedOnLoad", 1) != 0;
		g_cfg.cellRadius = (std::clamp)(get("iCellRadius", g_cfg.cellRadius), 0, 5);
		g_cfg.verbose = get("bVerboseLog", 0) != 0;
		_MESSAGE("config: enabled=%d scanEvery=%d settleMs=%d maxPerScan=%d refreshOnLoad=%d radius=%d verbose=%d",
				 g_cfg.enabled, g_cfg.scanEveryFrames, g_cfg.settleMs, g_cfg.maxRefreshPerScan,
				 g_cfg.refreshCreatedOnSight, g_cfg.cellRadius, g_cfg.verbose);
	}

	void MessageHandler(NVSEMessagingInterface::Message *msg)
	{
		switch (msg->type)
		{
		case NVSEMessagingInterface::kMessage_PostLoadGame:
		case NVSEMessagingInterface::kMessage_NewGame:
			g_tracked.clear();
			g_inGame = true;
			break;
		case NVSEMessagingInterface::kMessage_ExitToMainMenu:
		case NVSEMessagingInterface::kMessage_PreLoadGame:
			g_tracked.clear();
			g_inGame = false;
			if (g_cfg.verbose)
				_MESSAGE("session total refreshed: %u", g_totalRefreshed);
			break;
		case NVSEMessagingInterface::kMessage_MainGameLoop:
			if (g_cfg.enabled && g_inGame && ++g_frame % g_cfg.scanEveryFrames == 0)
				Scan();
			break;
		default:
			break;
		}
	}
}

// Exported through exports.def, same as xNVSE's nvse_plugin_example.
bool NVSEPlugin_Query(const NVSEInterface *nvse, PluginInfo *info)
{
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "NavCutRefresh";
	info->version = 1;
	if (nvse->isEditor)
		return false;
	if (nvse->runtimeVersion != RUNTIME_VERSION_1_4_0_525)
	{
		_ERROR("unsupported runtime %08X (addresses are for 1.4.0.525 only)", nvse->runtimeVersion);
		return false;
	}
	return true;
}

bool NVSEPlugin_Load(NVSEInterface *nvse)
{
	ReadConfig();
	auto *messaging = static_cast<NVSEMessagingInterface *>(nvse->QueryInterface(kInterface_Messaging));
	messaging->RegisterListener(nvse->GetPluginHandle(), "NVSE", MessageHandler);
	_MESSAGE("NavCutRefresh loaded");
	return true;
}
