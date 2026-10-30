#include "std_include.hpp"
#include "ui_boundary.hpp"
#include "call_chain.hpp"
#include "game.hpp"
#include "menu_scene.hpp"
#include "shared/fnv_exe.hpp"
#include "shared/common/ffp_state.hpp"

// See ui_boundary.hpp.

namespace comp::game::ui_boundary
{
	namespace
	{
		constexpr UINT kLogWindow = 600;

		// Written by the code caves on the game's main thread, which also
		// makes the D3D9 calls; cleared at Present. g_passed: the HUD's 3D render
		// was called. g_world: Main_Render rendered the world this frame.
		volatile uint8_t g_passed = 0;
		volatile uint8_t g_world = 0;

		bool g_installed = false;
		bool g_tried = false;
		bool g_logged_first = false;
		bool g_frame_injected = false;

		// Per-interval counts.
		UINT g_window = 0;
		uint32_t g_frames = 0, g_draws = 0, g_frame_draws = 0, g_max_draws = 0;
		uint32_t g_injections = 0, g_failures = 0, g_no_target = 0, g_menu_frames = 0;

		struct Redirect
		{
			const fnv_exe::Code* site;
			volatile uint8_t* flag;
			const char* what;
			uintptr_t thunk;       // the code the site calls once installed
		};
		Redirect g_sites[3] = {};

		// What a module that patches a site after the install means for the boundary.
		constexpr const char* kIfReplaced = "The UI boundary keeps working only if that code calls what it replaced; if "
			"it does not, frames that render the world are not recognised and UI draws rely on rtx.uiTextures.";
	}

	void install()
	{
		if (g_tried) return;
		g_tried = true;
		if (!fnv_exe::known()) return;
		const auto& a = fnv_exe::addr();
		g_sites[0] = { &a.ui_boundary_call, &g_passed, "Main_Render's call to the HUD's 3D render", 0 };
		g_sites[1] = { &a.world_scene_call_a, &g_world, "Main_Render's call to RenderWorldScene (gameplay)", 0 };
		g_sites[2] = { &a.world_scene_call_c, &g_world, "Main_Render's call to RenderWorldScene (Pip-Boy)", 0 };

		// All three or none: the boundary means nothing without knowing
		// whether the frame rendered the world, and the reverse. A site that
		// holds another mod's call is chained: the thunk calls that mod's code.
		call_chain::Site found[std::size(g_sites)];
		bool ok = true;
		for (size_t i = 0; i < std::size(g_sites); ++i)
		{
			const Redirect& r = g_sites[i];
			if (!r.site->addr) return;
			found[i] = call_chain::inspect(*r.site);
			call_chain::log_site("UiBoundary", *r.site, r.what, found[i]);
			if (found[i].state == call_chain::State::Foreign) ok = false;
		}
		if (!ok)
		{
			shared::common::log("UiBoundary", "UI boundary OFF: UI draws rely on rtx.uiTextures to start path "
				"tracing; a UI element whose texture is not listed can come out transparent.",
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}

		// One thunk per site, which the site calls: mov byte [flag], 1 ; jmp
		// target. The return address the call pushed, ESP and ECX reach the
		// target as the site left them, so the arguments, a thiscall's object
		// and the return to the site are unchanged.
		auto* cave = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!cave)
		{
			shared::common::log("UiBoundary", "UI boundary OFF: no memory for the code cave",
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		for (size_t i = 0; i < std::size(g_sites); ++i)
		{
			uint8_t* c = cave + i * 16;
			g_sites[i].thunk = reinterpret_cast<uintptr_t>(c);
			*c++ = 0xC6; *c++ = 0x05;
			const auto flag = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_sites[i].flag));
			std::memcpy(c, &flag, sizeof(flag)); c += 4;
			*c++ = 1;
			call_chain::emit_rel32(c, 0xE9, found[i].target);
		}
		FlushInstructionCache(GetCurrentProcess(), cave, 4096);

		for (size_t i = 0; i < std::size(g_sites); ++i)
		{
			const Redirect& r = g_sites[i];
			if (!call_chain::patch_rel32(r.site->addr, 0xE8, r.thunk))
			{
				// Sites already written stay valid: each thunk ends in the site's previous target.
				shared::common::log("UiBoundary", std::format("UI boundary OFF: 0x{:X} could not be made writable",
					r.site->addr), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				return;
			}
			call_chain::track(call_chain::Kind::Call, r.site->addr, r.thunk, found[i], "UiBoundary", r.what, kIfReplaced);
		}
		g_installed = true;
		shared::common::log("UiBoundary", std::format("UI boundary active: Main_Render's call to the HUD's 3D render "
			"@0x{:X} and its calls to RenderWorldScene @0x{:X} / 0x{:X} redirected ({}). In a frame that renders the "
			"world, every draw from the HUD's 3D render (scope overlays) to Present is UI: path tracing is injected "
			"before the first one and all of them are rasterised as drawn, whatever their texture. Menu frames without "
			"the world keep the earlier routing.", a.ui_boundary_call.addr, a.world_scene_call_a.addr, a.world_scene_call_c.addr,
			std::ranges::all_of(found, [](const call_chain::Site& s) { return s.state == call_chain::State::Vanilla; })
				? "bytes verified" : "chained in front of another mod's hook where one was found"),
			shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
	}

	bool in_ui()
	{
		return g_passed != 0 && g_world != 0;
	}

	bool injected()
	{
		return g_frame_injected || menu_scene::raster_frame();
	}

	void before_draw(IDirect3DDevice9* dev)
	{
		++g_frame_draws;
		if (injected() || !dev) return;
		// The runtime only injects at a draw to a primary-sized target; a draw
		// to any other target is rasterised by it as it is. A screen-sized
		// off-screen target would receive the path-traced image instead of the
		// back buffer.
		if (!game::rendering_to_backbuffer || game::offscreen_screen_sized) return;
		if (!trigger_injection(dev))
		{
			++g_failures;
			return;
		}
		g_frame_injected = true;
		++g_injections;
		if (!g_logged_first)
		{
			g_logged_first = true;
			shared::common::log("UiBoundary", "first frame with the UI boundary: RTX injection triggered before the "
				"first HUD or interface draw", shared::common::LOG_TYPE::LOG_TYPE_STATUS);
		}
	}

	bool trigger_injection(IDirect3DDevice9* dev)
	{
		// The states the runtime checks before its UI test (colour writes on,
		// no alpha test or blend, no stencil) are set for the draw and given
		// back after. One triangle with all three vertices at one point: no
		// pixel is written.
		struct Vertex { float x, y, z, rhw; D3DCOLOR color; };
		const Vertex v[3] = {
			{ 0.0f, 0.0f, 0.0f, 1.0f, 0 }, { 0.0f, 0.0f, 0.0f, 1.0f, 0 }, { 0.0f, 0.0f, 0.0f, 1.0f, 0 } };

		IDirect3DVertexShader9* vs = nullptr;
		IDirect3DPixelShader9* ps = nullptr;
		IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DVertexBuffer9* vb = nullptr;
		UINT vb_offset = 0, vb_stride = 0;
		DWORD fvf = 0;
		D3DMATRIX proj = {};
		constexpr D3DRENDERSTATETYPE kStates[] = { D3DRS_ZWRITEENABLE, D3DRS_COLORWRITEENABLE, D3DRS_ALPHATESTENABLE,
			D3DRS_ALPHABLENDENABLE, D3DRS_STENCILENABLE };
		constexpr DWORD kValues[] = { FALSE, 0xFu, FALSE, FALSE, FALSE };
		DWORD saved[std::size(kStates)] = {};

		dev->GetVertexShader(&vs);
		dev->GetPixelShader(&ps);
		dev->GetVertexDeclaration(&decl);
		dev->GetFVF(&fvf);
		dev->GetStreamSource(0, &vb, &vb_offset, &vb_stride);
		dev->GetTransform(D3DTS_PROJECTION, &proj);
		for (size_t i = 0; i < std::size(kStates); ++i)
		{
			dev->GetRenderState(kStates[i], &saved[i]);
			dev->SetRenderState(kStates[i], kValues[i]);
		}

		const D3DMATRIX ortho = { { {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f } } };
		dev->SetTransform(D3DTS_PROJECTION, &ortho);
		dev->SetVertexShader(nullptr);
		dev->SetPixelShader(nullptr);
		dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
		const bool paused = game::pause_raster_only(dev);
		const HRESULT hr = dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vertex));
		if (paused) game::resume_raster_only(dev);

		// Given back; DrawPrimitiveUP leaves stream 0 unset.
		dev->SetTransform(D3DTS_PROJECTION, &proj);
		for (size_t i = 0; i < std::size(kStates); ++i)
			dev->SetRenderState(kStates[i], saved[i]);
		if (decl) dev->SetVertexDeclaration(decl);
		else dev->SetFVF(fvf);
		dev->SetStreamSource(0, vb, vb_offset, vb_stride);
		dev->SetVertexShader(vs);
		dev->SetPixelShader(ps);
		if (vs) vs->Release();
		if (ps) ps->Release();
		if (decl) decl->Release();
		if (vb) vb->Release();
		return SUCCEEDED(hr);
	}

	void on_present()
	{
		if (in_ui())
		{
			++g_frames;
			if (!injected()) ++g_no_target;   // no draw to the back buffer after the boundary
		}
		else if (g_passed)
			++g_menu_frames;
		g_draws += g_frame_draws;
		g_max_draws = (std::max)(g_max_draws, g_frame_draws);
		g_frame_draws = 0;
		g_passed = 0;
		g_world = 0;
		g_frame_injected = false;
		if (++g_window < kLogWindow) return;
		if (g_installed)
			shared::common::log("UiBoundary", std::format("last {} frames: UI boundary used in {} world frames; UI "
				"draws after it {} ({:.1f} per frame, max {}); injections triggered {} (failures {}); frames with no "
				"back-buffer draw after it {}; menu frames without the world (earlier routing) {}", g_window, g_frames,
				g_draws, g_frames ? static_cast<double>(g_draws) / g_frames : 0.0, g_max_draws, g_injections,
				g_failures, g_no_target, g_menu_frames), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		g_window = 0;
		g_frames = g_draws = g_max_draws = g_injections = g_failures = g_no_target = g_menu_frames = 0;
	}
}
