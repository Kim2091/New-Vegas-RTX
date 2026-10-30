#pragma once

// Problems that most likely come from another mod or a broken install. Each
// kind is written once per session as a [Problem] line in remix-comp.log and
// sent to the runtime's problem notice (rtx.faultNotice.hostReport, one
// "id|text" line per kind), which shows it on top of the game with the log
// and crash dump folders and the help link from rtx.conf.
//
// The code that detects a problem calls report() where it detects it.
// on_present() checks the per-frame signals (an empty scene, the plugin's
// read faults), FalloutNV.exe's 4GB patch and a crash left by the previous
// session, which is reported once that session's zip is made, naming it as
// the one file to upload (rtx.faultNotice.uploadFile). The notice lists
// the kinds in the order of Fault: xNVSE and the 4GB patch first.
namespace comp::game::fault_report
{
	enum class Fault : uint8_t
	{
		XnvseMissing, NoLargeAddress, XnvseOld, PluginMissing, PluginMismatch, FeatureOff, UnknownExe, D3D9ExDevice,
		IncompatibleDll, EmptyScene, PluginReadFaults, GameCrashed, Count
	};

	// Logs the problem and sends it to the runtime. A kind already reported
	// only takes a detail it does not hold yet, added to its line.
	void report(Fault fault, const std::string& detail = {});

	// Reports a mod that is known not to work with this one
	// (docs/INCOMPATIBLE_MODS.md), by a loaded DLL's or plugin file's name
	// or a DLL's product name (either may be null). Returns the mod's name,
	// or null when the file is not on the list.
	const char* check_module(const char* file, const char* product);

	// Present: the per-frame checks, and the report sent once the bridge is up.
	void on_present();

	// The crash filter: leaves a marker that the next session reports, when
	// the game was still presenting frames. `crash_file` is the crash log's
	// file name in rtx-remix\logs, or null. Allocation free.
	void on_crash(const char* header, const char* crash_file);
}
