#pragma once

// Save-game screenshots ([SaveScreenshots] in remix-comp.ini), and the
// game's own Print Screen capture, which is turned off: it reads the back
// buffer (ScreenShotN.bmp), which under Remix is half black, and Remix's
// screenshot takes the key. Nothing in the executable reads
// bAllowScreenShot, so the call is taken (fnv_exe screenshot_key_call).
//
// The save header writer (0x84D4B0) calls the save screenshot capture
// (0x878F60), which draws the world again into a private render target and
// reads it back. Under Remix the world is path-traced from API submissions
// and never rasterised, so the capture is blank. The hook takes that call
// (fnv_exe save_screenshot_call) and answers it with the runtime's thumbnail
// instead: during play the wrapper asks for one every RefreshMs
// (rtx.screenshot.thumbnailRequest), and the runtime writes
// rtx-remix\thumbnail.rgb from the path-traced frame before the game's
// interface is drawn into it. A save uses the latest thumbnail asked for
// since the last load or cell change, resampled to the size the engine
// expects; without one, the engine's own capture runs. Main thread.
namespace comp::game::save_thumbnail
{
	// The runtime's thumbnail: RGB rows from the top, and the request it answers.
	struct Thumbnail
	{
		uint32_t width = 0, height = 0, request = 0;
		std::vector<uint8_t> rgb;
	};

	// Asks the runtime for a thumbnail of its latest path-traced frame,
	// answered at its next present. Returns the request's number, 0 when it
	// could not be sent.
	uint32_t request_thumbnail();

	// Reads the latest thumbnail of this session from rtx-remix\thumbnail.rgb.
	bool read_thumbnail(Thumbnail& t, std::string& why);

	// The thumbnail shows the place the player is in now: the periodic
	// refresh asked for it since the last load or cell change.
	bool is_current(const Thumbnail& t);

	// Game-code patch time: takes the save screenshot call when
	// [SaveScreenshots] Enabled=1, and the Print Screen capture's call.
	void install();

	// Each Present: asks the runtime for a new thumbnail when one is due.
	void on_present();
}
