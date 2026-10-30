#pragma once

// remix-comp.ini settings as live controls in the Remix developer menu's
// "Host Settings" tab (runtime exports remixapi_*HostSetting*, forwarded by the
// bridge client; see remix_c.h). Every exposed key is registered with its
// current value, grouped by its ini section. A menu change updates the config
// (settings read only at startup are saved and flagged as needing a restart)
// and is written back to the game folder's remix-comp.ini, keeping its
// comments and layout.
//
// FNV runs 32-bit: the runtime lives in NvRemixBridge.exe and every call is
// IPC. Registration is async (one message per setting). While the Remix menu
// is open (and once as it closes), changes are polled every kPollFrames
// frames with the bridge's remixapi_PollHostSettings, one
// round trip that returns values only when the menu changed something; a
// bridge without it falls back to one Get per setting. A bridge or runtime
// without the exports is logged once and nothing is done.
namespace comp::host_settings
{
	// After the game's Present, on the render thread: registers everything once
	// the bridge has settled, then polls and applies menu changes between frames.
	void on_present(IDirect3DDevice9* dev);
}
