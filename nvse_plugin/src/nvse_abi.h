#pragma once
#include <cstdint>

// The part of xNVSE's plugin binary interface this plugin uses, declared
// here from its documented layout: the two entry points, PluginInfo, the
// head of NVSEInterface, the messaging interface and the message numbers.
// Nothing else of xNVSE is used, and no xNVSE file is included or compiled.
//
// Layouts are 32-bit, in declaration order, and must not change: xNVSE
// passes these structures across the module boundary.

namespace nvse_abi
{
	using PluginHandle = uint32_t;
	constexpr PluginHandle kInvalidHandle = 0xFFFFFFFFu;

	// Packed versions: major << 24 | minor << 16 | build << 4 | sub.
	constexpr uint32_t make_version(uint32_t major, uint32_t minor, uint32_t build, uint32_t sub = 0)
	{
		return ((major & 0xFF) << 24) | ((minor & 0xFF) << 16) | ((build & 0xFFF) << 4) | (sub & 0xF);
	}
	constexpr uint32_t kRuntime_1_4_0_525 = make_version(4, 0, 525);   // 0x040020D0 (FalloutNV.exe 1.4.0.525)
	constexpr uint32_t kXnvse_6_4_8 = make_version(6, 4, 8);           // the first version with the ref / cell events

	// QueryInterface ids (only the messaging interface is asked for).
	constexpr uint32_t kInterface_Messaging = 2;

	struct PluginInfo
	{
		static constexpr uint32_t kInfoVersion = 1;
		uint32_t infoVersion;
		const char* name;
		uint32_t version;
	};

	// The head of NVSEInterface. Only these members are read; the structure
	// continues in xNVSE with members this plugin never touches.
	struct NVSEInterface
	{
		uint32_t nvseVersion;
		uint32_t runtimeVersion;
		uint32_t editorVersion;
		uint32_t isEditor;
		void* RegisterCommand;
		void* SetOpcodeBase;
		void* (*QueryInterface)(uint32_t id);
		PluginHandle (*GetPluginHandle)();
	};

	struct Message
	{
		const char* sender;
		uint32_t type;
		uint32_t dataLen;
		void* data;
	};

	using EventCallback = void (*)(Message* msg);

	struct MessagingInterface
	{
		uint32_t version;
		bool (*RegisterListener)(PluginHandle listener, const char* sender, EventCallback handler);
		bool (*Dispatch)(PluginHandle sender, uint32_t type, void* data, uint32_t dataLen, const char* receiver);
	};

	// Message numbers sent by "NVSE". Data layouts, as dispatched:
	//   PreLoadGame / LoadGame     data = save path (char*)
	//   PostLoadGame               data = bool success (by value in the pointer's low byte)
	//   OnFramePresent             data = int* isLoadingScreen
	//   OnRefSet3D                 data = { TESObjectREFR* ref; NiAVObject* new3D }*, sent after the 3D is set
	//   OnRefUnset3D               data = TESObjectREFR* itself, sent before the 3D is released
	//   OnRefAttach                data = TESObjectREFR* itself (end of the cell's node attach)
	//   OnCellStateChange          data = { TESObjectCELL* cell; uint32 newState; uint32 prevState }*
	//   OnCellRefsLoaded           data = TESObjectCELL* itself
	//   OnNonPersistentFormUnload  data = TESForm* itself
	enum MessageType : uint32_t
	{
		kMsg_PostLoad = 0,
		kMsg_ExitGame = 1,
		kMsg_ExitToMainMenu = 2,
		kMsg_LoadGame = 3,
		kMsg_SaveGame = 4,
		kMsg_ScriptPrecompile = 5,
		kMsg_PreLoadGame = 6,
		kMsg_ExitGame_Console = 7,
		kMsg_PostLoadGame = 8,
		kMsg_PostPostLoad = 9,
		kMsg_RuntimeScriptError = 10,
		kMsg_DeleteGame = 11,
		kMsg_RenameGame = 12,
		kMsg_RenameNewGame = 13,
		kMsg_NewGame = 14,
		kMsg_DeleteGameName = 15,
		kMsg_RenameGameName = 16,
		kMsg_RenameNewGameName = 17,
		kMsg_DeferredInit = 18,
		kMsg_ClearScriptDataCache = 19,
		kMsg_MainGameLoop = 20,
		kMsg_ScriptCompile = 21,
		kMsg_EventListDestroyed = 22,
		kMsg_PostQueryPlugins = 23,
		kMsg_OnFramePresent = 24,
		kMsg_ReloadConfig = 25,
		kMsg_OnRefSet3D = 26,
		kMsg_OnRefUnset3D = 27,
		kMsg_OnRefAttach = 28,
		kMsg_OnCellStateChange = 29,
		kMsg_OnCellRefsLoaded = 30,
		kMsg_OnNonPersistentFormLoad = 31,
		kMsg_OnNonPersistentFormUnload = 32,
	};

	// TESObjectCELL load states (the OnCellStateChange values).
	enum CellState : uint32_t
	{
		kCell_NotLoaded = 0, kCell_Unloading = 1, kCell_Loading = 2, kCell_Loaded = 3,
		kCell_Detaching = 4, kCell_Attaching = 5, kCell_Attached = 6,
	};
}
