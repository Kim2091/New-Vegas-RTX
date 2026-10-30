#include "std_include.hpp"
#include "d3d9ex.hpp"
#include "../d3d9_proxy.hpp"

#include "diagnostics.hpp"
#include "imgui.hpp"
#include "renderer.hpp"
#include "tracer.hpp"
#include "host_settings.hpp"
#include "comp/game/game_ini.hpp"
#include "shared/common/shader_cache.hpp"
#include "shared/common/ps_reflect.hpp"
#include "shared/common/vs_reflect.hpp"

using comp::tracer;
#include "tracer_dispatch.inc"
#include "shared/common/ffp_state.hpp"
#include "shared/common/config.hpp"
#include "shared/common/remix_protocol.hpp"
#include "comp/game/api_submit.hpp"
#include "comp/game/api_common.hpp"
#include "comp/game/api_camera.hpp"
#include "comp/game/scene_statics.hpp"
#include "comp/game/menu_bg.hpp"
#include "comp/game/menu_scene.hpp"
#include "comp/game/ui_boundary.hpp"
#include "comp/game/call_chain.hpp"
#include "comp/game/screen_space.hpp"
#include "comp/game/pipboy_cursor.hpp"
#include "comp/game/stable_origin.hpp"
#include "comp/game/nvse_link.hpp"
#include "comp/game/underwater.hpp"
#include "comp/game/fault_report.hpp"
#include "comp/game/frame_stats.hpp"
#include "comp/game/session_log.hpp"
#include "comp/game/session_report.hpp"
#include "comp/game/tex_pack.hpp"
#include "comp/game/tex_pool.hpp"
#include "comp/game/api_textures.hpp"
#include "comp/game/api_identity.hpp"

namespace comp
{
#pragma region D3D9Device

	HRESULT d3d9ex::D3D9Device::QueryInterface(REFIID riid, void** ppvObj)
	{
		TRACE_IF_ACTIVE(trace_QueryInterface, &riid, ppvObj);
		if (!ppvObj) return E_POINTER;
		*ppvObj = nullptr;

		// The proxy stands in only for interfaces whose vtable it implements;
		// handing it out for any other riid would give the caller a
		// D3D9Device under a foreign vtable, which crashes on first use.
		const bool wants_device9   = (riid == IID_IDirect3DDevice9) || (riid == IID_IUnknown);
		const bool wants_device9ex = (riid == IID_IDirect3DDevice9Ex);

		if (wants_device9 || (wants_device9ex && m_pIDirect3DDevice9Ex))
		{
			// Reference counting runs through the inner device (see AddRef /
			// Release), so take the ref there to keep the pairing balanced.
			m_pIDirect3DDevice9->AddRef();
			*ppvObj = wants_device9ex ? static_cast<void*>(static_cast<IDirect3DDevice9Ex*>(this))
			                          : static_cast<void*>(static_cast<IDirect3DDevice9*>(this));
			return S_OK;
		}

		// Everything else (including IID_IDirect3DDevice9Ex on a non-Ex device)
		// goes to the real device, and its answer is returned unaltered.
		return m_pIDirect3DDevice9->QueryInterface(riid, ppvObj);
	}

	ULONG d3d9ex::D3D9Device::AddRef()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_AddRef);
		return m_pIDirect3DDevice9->AddRef();
	}

	ULONG d3d9ex::D3D9Device::Release()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_Release);
		ULONG count = m_pIDirect3DDevice9->Release();
		if (!count) delete this;
		return count;
	}

	HRESULT d3d9ex::D3D9Device::TestCooperativeLevel()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_TestCooperativeLevel);
		return m_pIDirect3DDevice9->TestCooperativeLevel();
	}

	UINT d3d9ex::D3D9Device::GetAvailableTextureMem()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_GetAvailableTextureMem);
		return m_pIDirect3DDevice9->GetAvailableTextureMem();
	}

	HRESULT d3d9ex::D3D9Device::EvictManagedResources()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_EvictManagedResources);
		return m_pIDirect3DDevice9->EvictManagedResources();
	}

	HRESULT d3d9ex::D3D9Device::GetDirect3D(IDirect3D9** ppD3D9)
	{
		TRACE_IF_ACTIVE(trace_GetDirect3D, ppD3D9);
		return m_pIDirect3DDevice9->GetDirect3D(ppD3D9);
	}

	HRESULT d3d9ex::D3D9Device::GetDeviceCaps(D3DCAPS9* pCaps)
	{
		TRACE_IF_ACTIVE(trace_GetDeviceCaps, pCaps);
		return m_pIDirect3DDevice9->GetDeviceCaps(pCaps);
	}

	namespace
	{
		std::atomic<uint32_t> g_display_epoch = 0;

		// The first callers of GetDisplayMode, logged once each.
		constexpr size_t kModeCallers = 8;
		std::atomic<uintptr_t> g_mode_callers[kModeCallers] = {};

		void note_mode_caller(uintptr_t ret)
		{
			for (auto& slot : g_mode_callers)
			{
				uintptr_t seen = slot.load(std::memory_order_relaxed);
				if (seen == ret) return;
				if (seen) continue;
				if (!slot.compare_exchange_strong(seen, ret))
				{
					if (seen == ret) return;
					continue;
				}
				HMODULE mod = nullptr;
				char path[MAX_PATH] = "?";
				if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(ret), &mod) && mod)
					GetModuleFileNameA(mod, path, MAX_PATH);
				const char* name = strrchr(path, '\\');
				shared::common::log_event("D3D9", std::format("GetDisplayMode called from {}+0x{:X}; answered from the cached mode",
					name ? name + 1 : path, ret - reinterpret_cast<uintptr_t>(mod)), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT);
				return;
			}
		}
	}

	void d3d9ex::note_display_change()
	{
		g_display_epoch.fetch_add(1, std::memory_order_relaxed);
	}

	void d3d9ex::D3D9Device::clear_display_mode()
	{
		AcquireSRWLockExclusive(&m_mode_lock);
		m_mode_valid = false;
		ReleaseSRWLockExclusive(&m_mode_lock);
	}

	HRESULT d3d9ex::D3D9Device::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode)
	{
		TRACE_IF_ACTIVE(trace_GetDisplayMode, iSwapChain, pMode);
		if (iSwapChain != 0 || !pMode) return m_pIDirect3DDevice9->GetDisplayMode(iSwapChain, pMode);
		note_mode_caller(reinterpret_cast<uintptr_t>(_ReturnAddress()));

		const ULONGLONG now = GetTickCount64();
		const uint32_t epoch = g_display_epoch.load(std::memory_order_relaxed);
		AcquireSRWLockShared(&m_mode_lock);
		const bool hit = m_mode_valid && m_mode_epoch == epoch && now - m_mode_tick < 1000;
		if (hit) *pMode = m_mode;
		ReleaseSRWLockShared(&m_mode_lock);
		if (hit) return D3D_OK;

		D3DDISPLAYMODE mode = {};
		const HRESULT hr = m_pIDirect3DDevice9->GetDisplayMode(iSwapChain, &mode);
		if (FAILED(hr)) return hr;
		*pMode = mode;
		AcquireSRWLockExclusive(&m_mode_lock);
		m_mode = mode;
		m_mode_tick = now;
		m_mode_epoch = epoch;
		m_mode_valid = true;
		ReleaseSRWLockExclusive(&m_mode_lock);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters)
	{
		TRACE_IF_ACTIVE(trace_GetCreationParameters, pParameters);
		return m_pIDirect3DDevice9->GetCreationParameters(pParameters);
	}

	HRESULT d3d9ex::D3D9Device::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap)
	{
		TRACE_IF_ACTIVE(trace_SetCursorProperties, XHotSpot, YHotSpot, pCursorBitmap);
		return m_pIDirect3DDevice9->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap);
	}

	void d3d9ex::D3D9Device::SetCursorPosition(int X, int Y, DWORD Flags)
	{
		TRACE_IF_ACTIVE(trace_SetCursorPosition, X, Y, Flags);
		return m_pIDirect3DDevice9->SetCursorPosition(X, Y, Flags);
	}

	BOOL d3d9ex::D3D9Device::ShowCursor(BOOL bShow)
	{
		TRACE_IF_ACTIVE(trace_ShowCursor, bShow);
		return m_pIDirect3DDevice9->ShowCursor(bShow);
	}

	HRESULT d3d9ex::D3D9Device::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain)
	{
		TRACE_IF_ACTIVE(trace_CreateAdditionalSwapChain, pPresentationParameters, pSwapChain);
		return m_pIDirect3DDevice9->CreateAdditionalSwapChain(pPresentationParameters, pSwapChain);
	}

	HRESULT d3d9ex::D3D9Device::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain)
	{
		TRACE_IF_ACTIVE(trace_GetSwapChain, iSwapChain, pSwapChain);
		return m_pIDirect3DDevice9->GetSwapChain(iSwapChain, pSwapChain);
	}

	UINT d3d9ex::D3D9Device::GetNumberOfSwapChains()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_GetNumberOfSwapChains);
		return m_pIDirect3DDevice9->GetNumberOfSwapChains();
	}

	void d3d9ex::D3D9Device::pre_reset()
	{
		clear_display_mode();
		if (auto* t = tracer::get()) t->on_reset();
		shared::common::ffp_state::get().on_reset();
		remix_protocol::on_reset();
		game::on_reset();
		game::api_submit::on_reset();
		game::menu_bg::on_reset();
		game::frame_stats::on_reset();
		renderer_on_reset();
		game::stable_origin::on_reset();
		shared::common::g_shader_cache.clear_cache();
		tex_addons::init_texture_addons(true);
		ImGui_ImplDX9_InvalidateDeviceObjects();
	}

	void d3d9ex::D3D9Device::post_reset(HRESULT hr)
	{
		clear_display_mode();
		// Reset commonly returns D3DERR_DEVICELOST while the device is still
		// unavailable; the game retries next frame. Recreating device objects
		// against a device that is still lost fails and, for the ImGui backend,
		// leaves it believing it owns objects it doesn't.
		if (FAILED(hr)) return;
		game::note_default_render_target(m_pIDirect3DDevice9);
		tex_addons::init_texture_addons();
		ImGui_ImplDX9_CreateDeviceObjects();
	}

	HRESULT d3d9ex::D3D9Device::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters)
	{
		PROFILE_ZONE_N("d3d9::Reset");
		TRACE_IF_ACTIVE(trace_Reset, pPresentationParameters);
		pre_reset();
		const auto hr = m_pIDirect3DDevice9->Reset(pPresentationParameters);
		post_reset(hr);
		return hr;
	}

	// The overlay is drawn once per frame, in a scene of its own, after the
	// game's last draw (the 2D interface, on the back buffer). Drawn at every
	// EndScene it would also land mid-frame in the image-space source target,
	// where the runtime takes its orthographic draws as UI.
	void d3d9ex::D3D9Device::render_overlay()
	{
		if (!imgui::is_initialized()) return;
		PROFILE_ZONE_N("Overlay");
		if (FAILED(m_pIDirect3DDevice9->BeginScene())) return;
		imgui::get()->on_present();
		m_pIDirect3DDevice9->EndScene();
	}

	void d3d9ex::D3D9Device::pre_present()
	{
		// Each part's time goes to the [PerfSlow] breakdown (session_log.hpp).
		LARGE_INTEGER last;
		QueryPerformanceCounter(&last);
		const auto part = [&last](const char* name)
			{
				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				game::session_log::note_part(name, now.QuadPart - last.QuadPart);
				last = now;
			};
		{
			auto& ffp = shared::common::ffp_state::get();
			if (auto* d = diagnostics::get())
				d->on_present(ffp.frame_count(), ffp.draw_call_count(), ffp.scene_count());

			// Tracy plots before ffp_state::on_present(), which zeros the counters.
			PROFILE_PLOT_I64("DrawCalls/frame", ffp.draw_call_count());
			PROFILE_PLOT_I64("Scenes/frame",    ffp.scene_count());
		}

		// The engine monitor's events (cuts, cells, owners) reach their
		// consumers before the scene-graph ticks below read them.
		game::nvse_link::on_present();
		game::underwater::on_present();
		part("engine monitor");
		// The scene-graph statics ([RemixApi] SceneGraphStatics): their
		// creates, updates and destroys go out before the retained end of frame.
		game::scene_statics::tick();
		part("scene-graph statics");
		// Retained instances close the frame while frame_count() still names it.
		game::api_submit::end_frame_retained();
		part("retained instances");
		// The frame's world camera source ([RemixApi] FallbackCamera), while
		// frame_count() still names the frame.
		game::api_camera::on_present();
		// The Pip-Boy cursor's session lines, while frame_count() still names the frame.
		game::pipboy_cursor::on_present();
		// The UI boundary's frame flags, while frame_count() still names the frame.
		game::ui_boundary::on_present();
		// Every hooked engine site, at the first Present and every 600 frames.
		game::call_chain::on_present();
		part("camera, cursor, UI boundary, hook check");
		shared::common::ffp_state::get().on_present();
		remix_protocol::on_present();
		game::lights_updated_frame = false;
		game::on_frame_end();
		part("lights, sun, weather (frame end)");
		game::api_submit::on_present();
		part("API submit end of frame");
		game::frame_stats::on_present();
		game::session_log::on_present();
		game::session_report::on_present();
		game::fault_report::on_present();
		part("logs");
		game::menu_bg::on_present();
		game::menu_scene::on_present();
		game::screen_space::on_present();
		part("menu background and scenes");
		game::tex_pack::on_present(m_pIDirect3DDevice9);
		// The first Present asks whether D3DPOOL_DEFAULT textures can be imported.
		game::tex_pool::on_present();
		part("texture packs");
	}

	void d3d9ex::D3D9Device::post_present()
	{
		if (auto* t = tracer::get()) t->on_present();
		// Remix menu changes apply here, between frames: this frame is presented
		// and the next hasn't drawn yet, so a feature switched off (retained
		// instances, statics, the stable origin) tears down cleanly.
		host_settings::on_present(m_pIDirect3DDevice9);
		PROFILE_FRAME_MARK();
	}

	HRESULT d3d9ex::D3D9Device::Present(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion)
	{
		PROFILE_ZONE_N("Present");
		TRACE_IF_ACTIVE(trace_Present, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
		LARGE_INTEGER t0, t1, t2;
		QueryPerformanceCounter(&t0);
		render_overlay();
		pre_present();
		QueryPerformanceCounter(&t1);
		auto hr = m_pIDirect3DDevice9->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
		QueryPerformanceCounter(&t2);
		game::session_log::note_present(t1.QuadPart - t0.QuadPart, t2.QuadPart - t1.QuadPart);
		post_present();
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer)
	{
		TRACE_IF_ACTIVE(trace_GetBackBuffer, iSwapChain, iBackBuffer, Type, ppBackBuffer);
		return m_pIDirect3DDevice9->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer);
	}

	HRESULT d3d9ex::D3D9Device::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus)
	{
		TRACE_IF_ACTIVE(trace_GetRasterStatus, iSwapChain, pRasterStatus);
		return m_pIDirect3DDevice9->GetRasterStatus(iSwapChain, pRasterStatus);
	}

	HRESULT d3d9ex::D3D9Device::SetDialogBoxMode(BOOL bEnableDialogs)
	{
		TRACE_IF_ACTIVE(trace_SetDialogBoxMode, bEnableDialogs);
		return m_pIDirect3DDevice9->SetDialogBoxMode(bEnableDialogs);
	}

	void d3d9ex::D3D9Device::SetGammaRamp(UINT iSwapChain, DWORD Flags, CONST D3DGAMMARAMP* pRamp)
	{
		TRACE_IF_ACTIVE(trace_SetGammaRamp, iSwapChain, Flags, pRamp);
		return m_pIDirect3DDevice9->SetGammaRamp(iSwapChain, Flags, pRamp);
	}

	void d3d9ex::D3D9Device::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp)
	{
		TRACE_IF_ACTIVE(trace_GetGammaRamp, iSwapChain, pRamp);
		return m_pIDirect3DDevice9->GetGammaRamp(iSwapChain, pRamp);
	}

	HRESULT d3d9ex::D3D9Device::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle)
	{
		PROFILE_ZONE_N("d3d9::CreateTexture");
		TRACE_IF_ACTIVE(trace_CreateTexture, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
		// A plain DEFAULT request may be created MANAGED so the Remix API paths can read it (tex_pool.hpp).
		const auto hr = game::tex_pool::create(D3DRTYPE_TEXTURE, Width, Height, 1, Levels, Usage, Format, Pool, pSharedHandle,
			m_pIDirect3DDevice9Ex != nullptr, [&](D3DPOOL pool)
			{
				return m_pIDirect3DDevice9->CreateTexture(Width, Height, Levels, Usage, Format, pool, ppTexture, pSharedHandle);
			});
		if (SUCCEEDED(hr) && ppTexture)
		{
			game::tex_pack::on_texture_created(*ppTexture);
			game::api::identity::on_texture_created(*ppTexture);
			game::api::textures::forget_default(*ppTexture);
		}
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle)
	{
		PROFILE_ZONE_N("d3d9::CreateVolumeTexture");
		TRACE_IF_ACTIVE(trace_CreateVolumeTexture, Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle);
		const auto hr = game::tex_pool::create(D3DRTYPE_VOLUMETEXTURE, Width, Height, Depth, Levels, Usage, Format, Pool,
			pSharedHandle, m_pIDirect3DDevice9Ex != nullptr, [&](D3DPOOL pool)
			{
				return m_pIDirect3DDevice9->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, pool, ppVolumeTexture,
					pSharedHandle);
			});
		if (SUCCEEDED(hr) && ppVolumeTexture) game::tex_pack::on_texture_created(*ppVolumeTexture);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle)
	{
		PROFILE_ZONE_N("d3d9::CreateCubeTexture");
		TRACE_IF_ACTIVE(trace_CreateCubeTexture, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
		const auto hr = game::tex_pool::create(D3DRTYPE_CUBETEXTURE, EdgeLength, EdgeLength, 1, Levels, Usage, Format, Pool,
			pSharedHandle, m_pIDirect3DDevice9Ex != nullptr, [&](D3DPOOL pool)
			{
				return m_pIDirect3DDevice9->CreateCubeTexture(EdgeLength, Levels, Usage, Format, pool, ppCubeTexture, pSharedHandle);
			});
		if (SUCCEEDED(hr) && ppCubeTexture) game::tex_pack::on_texture_created(*ppCubeTexture);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle)
	{
		PROFILE_ZONE_N("d3d9::CreateVertexBuffer");
		TRACE_IF_ACTIVE(trace_CreateVertexBuffer, Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
		const auto hr = m_pIDirect3DDevice9->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
		// A new object: the statics' buffer record at this address starts
		// afresh (the address may be a released buffer's).
		if (SUCCEEDED(hr) && ppVertexBuffer && *ppVertexBuffer)
			game::api::identity::on_buffer_created(*ppVertexBuffer, Usage, static_cast<UINT>(Pool), Length, false);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle)
	{
		PROFILE_ZONE_N("d3d9::CreateIndexBuffer");
		TRACE_IF_ACTIVE(trace_CreateIndexBuffer, Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
		const auto hr = m_pIDirect3DDevice9->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
		if (SUCCEEDED(hr) && ppIndexBuffer && *ppIndexBuffer)
			game::api::identity::on_buffer_created(*ppIndexBuffer, Usage, static_cast<UINT>(Pool), Length, true);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
	{
		PROFILE_ZONE_N("d3d9::CreateRenderTarget");
		TRACE_IF_ACTIVE(trace_CreateRenderTarget, Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
		return m_pIDirect3DDevice9->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
	}

	HRESULT d3d9ex::D3D9Device::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
	{
		PROFILE_ZONE_N("d3d9::CreateDepthStencilSurface");
		TRACE_IF_ACTIVE(trace_CreateDepthStencilSurface, Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle);
		return m_pIDirect3DDevice9->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle);
	}

	HRESULT d3d9ex::D3D9Device::UpdateSurface(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestinationSurface, CONST POINT* pDestPoint)
	{
		PROFILE_ZONE_N("d3d9::UpdateSurface");
		TRACE_IF_ACTIVE(trace_UpdateSurface, pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint);
		// A MANAGED destination (tex_pool.hpp) is copied with locks; the runtime refuses it.
		if (HRESULT hr; game::tex_pool::update_surface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint, hr)) return hr;
		const HRESULT hr = m_pIDirect3DDevice9->UpdateSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint);
		game::tex_pool::after_update_surface(pDestinationSurface);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::UpdateTexture(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture)
	{
		PROFILE_ZONE_N("d3d9::UpdateTexture");
		TRACE_IF_ACTIVE(trace_UpdateTexture, pSourceTexture, pDestinationTexture);
		// A MANAGED destination (tex_pool.hpp) is copied with locks; the runtime refuses it.
		if (HRESULT hr; game::tex_pool::update_texture(pSourceTexture, pDestinationTexture, hr)) return hr;
		const HRESULT hr = m_pIDirect3DDevice9->UpdateTexture(pSourceTexture, pDestinationTexture);
		// A DEFAULT destination is hashed from the source's CPU copy (tex_pool.hpp).
		game::tex_pool::after_update_texture(pSourceTexture, pDestinationTexture, hr);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface)
	{
		PROFILE_ZONE_N("d3d9::GetRenderTargetData");
		TRACE_IF_ACTIVE(trace_GetRenderTargetData, pRenderTarget, pDestSurface);
		return m_pIDirect3DDevice9->GetRenderTargetData(pRenderTarget, pDestSurface);
	}

	HRESULT d3d9ex::D3D9Device::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface)
	{
		PROFILE_ZONE_N("d3d9::GetFrontBufferData");
		TRACE_IF_ACTIVE(trace_GetFrontBufferData, iSwapChain, pDestSurface);
		return m_pIDirect3DDevice9->GetFrontBufferData(iSwapChain, pDestSurface);
	}

	HRESULT d3d9ex::D3D9Device::StretchRect(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter)
	{
		PROFILE_ZONE_N("d3d9::StretchRect");
		TRACE_IF_ACTIVE(trace_StretchRect, pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);
		game::menu_bg::on_stretch_rect(pSourceSurface, pDestSurface);
		return m_pIDirect3DDevice9->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);
	}

	HRESULT d3d9ex::D3D9Device::ColorFill(IDirect3DSurface9* pSurface, CONST RECT* pRect, D3DCOLOR color)
	{
		PROFILE_ZONE_N("d3d9::ColorFill");
		TRACE_IF_ACTIVE(trace_ColorFill, pSurface, pRect, color);
		return m_pIDirect3DDevice9->ColorFill(pSurface, pRect, color);
	}

	HRESULT d3d9ex::D3D9Device::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
	{
		PROFILE_ZONE_N("d3d9::CreateOffscreenPlainSurface");
		TRACE_IF_ACTIVE(trace_CreateOffscreenPlainSurface, Width, Height, Format, Pool, ppSurface, pSharedHandle);
		return m_pIDirect3DDevice9->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle);
	}

	HRESULT d3d9ex::D3D9Device::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget)
	{
		PROFILE_ZONE_N("d3d9::SetRenderTarget");
		TRACE_IF_ACTIVE(trace_SetRenderTarget, RenderTargetIndex, pRenderTarget);
		// The Pip-Boy's live background may bind the backbuffer in place of the
		// menu-background target during its capture pass; everything downstream
		// classifies what is actually bound.
		auto* const bind = game::menu_bg::filter_render_target(m_pIDirect3DDevice9, RenderTargetIndex, pRenderTarget);
		game::on_set_render_target(m_pIDirect3DDevice9, RenderTargetIndex, bind);
		game::menu_bg::post_render_target(RenderTargetIndex);
		if (RenderTargetIndex == 0) game::frame_stats::note_render_target(bind);
		return m_pIDirect3DDevice9->SetRenderTarget(RenderTargetIndex, bind);
	}

	HRESULT d3d9ex::D3D9Device::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget)
	{
		TRACE_IF_ACTIVE(trace_GetRenderTarget, RenderTargetIndex, ppRenderTarget);
		return m_pIDirect3DDevice9->GetRenderTarget(RenderTargetIndex, ppRenderTarget);
	}

	HRESULT d3d9ex::D3D9Device::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil)
	{
		PROFILE_ZONE_N("d3d9::SetDepthStencilSurface");
		TRACE_IF_ACTIVE(trace_SetDepthStencilSurface, pNewZStencil);
		return m_pIDirect3DDevice9->SetDepthStencilSurface(game::menu_bg::filter_depth_stencil(pNewZStencil));
	}

	HRESULT d3d9ex::D3D9Device::GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface)
	{
		TRACE_IF_ACTIVE(trace_GetDepthStencilSurface, ppZStencilSurface);
		return m_pIDirect3DDevice9->GetDepthStencilSurface(ppZStencilSurface);
	}

	HRESULT d3d9ex::D3D9Device::BeginScene()
	{
		PROFILE_ZONE_N("d3d9::BeginScene");
		TRACE_IF_ACTIVE_NOARGS(trace_BeginScene);
		shared::common::ffp_state::get().on_begin_scene();
		remix_protocol::on_begin_scene();
		// The engine monitor plugin, found once (its facts are read lazily,
		// once per frame, by the first consumer).
		game::nvse_link::on_begin_scene();
		game::frame_device = m_pIDirect3DDevice9;
		// This frame's render origin, before anything below sets a transform
		// or a light (once per frame; no-op while StableOrigin is off).
		game::stable_origin::begin_frame(m_pIDirect3DDevice9);
		// The frame's retained replay, with the O just computed, before any
		// draw of the frame (and so before the HUD draw that makes Remix
		// trace it). Once per frame; [RemixApi] RetainedReplayAtFrameStart.
		game::api_submit::begin_frame_retained();

		if (renderer::is_initialized()) {
			on_begin_scene_cb();
		}

		return m_pIDirect3DDevice9->BeginScene();
	}

	HRESULT d3d9ex::D3D9Device::EndScene()
	{
		PROFILE_ZONE_N("EndScene");
		TRACE_IF_ACTIVE_NOARGS(trace_EndScene);
		return m_pIDirect3DDevice9->EndScene();
	}

	HRESULT d3d9ex::D3D9Device::Clear(DWORD Count, CONST D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
	{
		PROFILE_ZONE_N("d3d9::Clear");
		TRACE_IF_ACTIVE(trace_Clear, Count, pRects, Flags, Color, Z, Stencil);

		return m_pIDirect3DDevice9->Clear(Count, pRects, Flags, Color, Z, Stencil);
	}

	HRESULT d3d9ex::D3D9Device::SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix)
	{
		PROFILE_ZONE_N("d3d9::SetTransform");
		TRACE_IF_ACTIVE(trace_SetTransform, State, pMatrix);
		shared::common::ffp_state::get().on_set_transform(State);
		// WORLD / VIEW into the stable render origin's frame (a plain
		// SetTransform while it is off).
		return game::stable_origin::set_transform(m_pIDirect3DDevice9, State, pMatrix);
	}

	HRESULT d3d9ex::D3D9Device::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix)
	{
		TRACE_IF_ACTIVE(trace_GetTransform, State, pMatrix);
		// With the stable origin on, hand back what the caller set, not the
		// shifted copy on the device, so a save / restore doesn't shift twice.
		if (game::stable_origin::get_raw_transform(State, pMatrix))
			return D3D_OK;
		return m_pIDirect3DDevice9->GetTransform(State, pMatrix);
	}

	HRESULT d3d9ex::D3D9Device::MultiplyTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix)
	{
		TRACE_IF_ACTIVE(trace_MultiplyTransform, State, pMatrix);
		D3DMATRIX raw;
		if (pMatrix && game::stable_origin::get_raw_transform(State, &raw))
		{
			// D3D9 order: M * current. Set through the origin like any other
			// WORLD / VIEW.
			D3DXMATRIX product;
			D3DXMatrixMultiply(&product, reinterpret_cast<const D3DXMATRIX*>(pMatrix), reinterpret_cast<const D3DXMATRIX*>(&raw));
			return game::stable_origin::set_transform(m_pIDirect3DDevice9, State, &product);
		}
		return m_pIDirect3DDevice9->MultiplyTransform(State, pMatrix);
	}

	HRESULT d3d9ex::D3D9Device::SetViewport(CONST D3DVIEWPORT9* pViewport)
	{
		PROFILE_ZONE_N("d3d9::SetViewport");
		TRACE_IF_ACTIVE(trace_SetViewport, pViewport);
		return m_pIDirect3DDevice9->SetViewport(pViewport);
	}

	HRESULT d3d9ex::D3D9Device::GetViewport(D3DVIEWPORT9* pViewport)
	{
		TRACE_IF_ACTIVE(trace_GetViewport, pViewport);
		return m_pIDirect3DDevice9->GetViewport(pViewport);
	}

	HRESULT d3d9ex::D3D9Device::SetMaterial(CONST D3DMATERIAL9* pMaterial)
	{
		PROFILE_ZONE_N("d3d9::SetMaterial");
		TRACE_IF_ACTIVE(trace_SetMaterial, pMaterial);
		return m_pIDirect3DDevice9->SetMaterial(pMaterial);
	}

	HRESULT d3d9ex::D3D9Device::GetMaterial(D3DMATERIAL9* pMaterial)
	{
		TRACE_IF_ACTIVE(trace_GetMaterial, pMaterial);
		return m_pIDirect3DDevice9->GetMaterial(pMaterial);
	}

	HRESULT d3d9ex::D3D9Device::SetLight(DWORD Index, CONST D3DLIGHT9* pLight)
	{
		PROFILE_ZONE_N("d3d9::SetLight");
		TRACE_IF_ACTIVE(trace_SetLight, Index, pLight);
		return m_pIDirect3DDevice9->SetLight(Index, pLight);
	}

	HRESULT d3d9ex::D3D9Device::GetLight(DWORD Index, D3DLIGHT9* pLight)
	{
		TRACE_IF_ACTIVE(trace_GetLight, Index, pLight);
		return m_pIDirect3DDevice9->GetLight(Index, pLight);
	}

	HRESULT d3d9ex::D3D9Device::LightEnable(DWORD Index, BOOL Enable)
	{
		PROFILE_ZONE_N("d3d9::LightEnable");
		TRACE_IF_ACTIVE(trace_LightEnable, Index, Enable);
		return m_pIDirect3DDevice9->LightEnable(Index, Enable);
	}

	HRESULT d3d9ex::D3D9Device::GetLightEnable(DWORD Index, BOOL* pEnable)
	{
		TRACE_IF_ACTIVE(trace_GetLightEnable, Index, pEnable);
		return m_pIDirect3DDevice9->GetLightEnable(Index, pEnable);
	}

	HRESULT d3d9ex::D3D9Device::SetClipPlane(DWORD Index, CONST float* pPlane)
	{
		TRACE_IF_ACTIVE(trace_SetClipPlane, Index, pPlane);
		return m_pIDirect3DDevice9->SetClipPlane(Index, pPlane);
	}

	HRESULT d3d9ex::D3D9Device::GetClipPlane(DWORD Index, float* pPlane)
	{
		TRACE_IF_ACTIVE(trace_GetClipPlane, Index, pPlane);
		return m_pIDirect3DDevice9->GetClipPlane(Index, pPlane);
	}

	HRESULT d3d9ex::D3D9Device::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
	{
		PROFILE_ZONE_N("d3d9::SetRenderState");
		TRACE_IF_ACTIVE(trace_SetRenderState, State, Value);
		remix_protocol::on_external_set(State, Value);
		// Fog distances into render units, like the view space they apply to.
		return m_pIDirect3DDevice9->SetRenderState(State, game::stable_origin::set_fog_state(State, Value));
	}

	HRESULT d3d9ex::D3D9Device::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue)
	{
		TRACE_IF_ACTIVE(trace_GetRenderState, State, pValue);
		if (pValue && game::stable_origin::get_raw_fog_state(State, pValue))
			return D3D_OK;
		return m_pIDirect3DDevice9->GetRenderState(State, pValue);
	}

	HRESULT d3d9ex::D3D9Device::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB)
	{
		PROFILE_ZONE_N("d3d9::CreateStateBlock");
		TRACE_IF_ACTIVE(trace_CreateStateBlock, Type, ppSB);
		return m_pIDirect3DDevice9->CreateStateBlock(Type, ppSB);
	}

	HRESULT d3d9ex::D3D9Device::BeginStateBlock()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_BeginStateBlock);
		return m_pIDirect3DDevice9->BeginStateBlock();
	}

	HRESULT d3d9ex::D3D9Device::EndStateBlock(IDirect3DStateBlock9** ppSB)
	{
		TRACE_IF_ACTIVE(trace_EndStateBlock, ppSB);
		return m_pIDirect3DDevice9->EndStateBlock(ppSB);
	}

	HRESULT d3d9ex::D3D9Device::SetClipStatus(CONST D3DCLIPSTATUS9* pClipStatus)
	{
		TRACE_IF_ACTIVE(trace_SetClipStatus, pClipStatus);
		return m_pIDirect3DDevice9->SetClipStatus(pClipStatus);
	}

	HRESULT d3d9ex::D3D9Device::GetClipStatus(D3DCLIPSTATUS9* pClipStatus)
	{
		TRACE_IF_ACTIVE(trace_GetClipStatus, pClipStatus);
		return m_pIDirect3DDevice9->GetClipStatus(pClipStatus);
	}

	HRESULT d3d9ex::D3D9Device::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture)
	{
		TRACE_IF_ACTIVE(trace_GetTexture, Stage, ppTexture);
		return m_pIDirect3DDevice9->GetTexture(Stage, ppTexture);
	}

	HRESULT d3d9ex::D3D9Device::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture)
	{
		PROFILE_ZONE_N("d3d9::SetTexture");
		TRACE_IF_ACTIVE(trace_SetTexture, Stage, pTexture);
		shared::common::ffp_state::get().on_set_texture(Stage, pTexture);
		return m_pIDirect3DDevice9->SetTexture(Stage, pTexture);
	}

	HRESULT d3d9ex::D3D9Device::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue)
	{
		TRACE_IF_ACTIVE(trace_GetTextureStageState, Stage, Type, pValue);
		return m_pIDirect3DDevice9->GetTextureStageState(Stage, Type, pValue);
	}

	HRESULT d3d9ex::D3D9Device::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
	{
		PROFILE_ZONE_N("d3d9::SetTextureStageState");
		TRACE_IF_ACTIVE(trace_SetTextureStageState, Stage, Type, Value);
		shared::common::ffp_state::get().on_set_texture_stage_state(Stage, Type, Value);
		return m_pIDirect3DDevice9->SetTextureStageState(Stage, Type, Value);
	}

	HRESULT d3d9ex::D3D9Device::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue)
	{
		TRACE_IF_ACTIVE(trace_GetSamplerState, Sampler, Type, pValue);
		return m_pIDirect3DDevice9->GetSamplerState(Sampler, Type, pValue);
	}

	HRESULT d3d9ex::D3D9Device::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value)
	{
		PROFILE_ZONE_N("d3d9::SetSamplerState");
		TRACE_IF_ACTIVE(trace_SetSamplerState, Sampler, Type, Value);
		return m_pIDirect3DDevice9->SetSamplerState(Sampler, Type, Value);
	}

	HRESULT d3d9ex::D3D9Device::ValidateDevice(DWORD* pNumPasses)
	{
		TRACE_IF_ACTIVE(trace_ValidateDevice, pNumPasses);
		return m_pIDirect3DDevice9->ValidateDevice(pNumPasses);
	}

	HRESULT d3d9ex::D3D9Device::SetPaletteEntries(UINT PaletteNumber, CONST PALETTEENTRY* pEntries)
	{
		TRACE_IF_ACTIVE(trace_SetPaletteEntries, PaletteNumber, pEntries);
		return m_pIDirect3DDevice9->SetPaletteEntries(PaletteNumber, pEntries);
	}

	HRESULT d3d9ex::D3D9Device::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries)
	{
		TRACE_IF_ACTIVE(trace_GetPaletteEntries, PaletteNumber, pEntries);
		return m_pIDirect3DDevice9->GetPaletteEntries(PaletteNumber, pEntries);
	}

	HRESULT d3d9ex::D3D9Device::SetCurrentTexturePalette(UINT PaletteNumber)
	{
		TRACE_IF_ACTIVE(trace_SetCurrentTexturePalette, PaletteNumber);
		return m_pIDirect3DDevice9->SetCurrentTexturePalette(PaletteNumber);
	}

	HRESULT d3d9ex::D3D9Device::GetCurrentTexturePalette(UINT *PaletteNumber)
	{
		TRACE_IF_ACTIVE(trace_GetCurrentTexturePalette, PaletteNumber);
		return m_pIDirect3DDevice9->GetCurrentTexturePalette(PaletteNumber);
	}

	HRESULT d3d9ex::D3D9Device::SetScissorRect(CONST RECT* pRect)
	{
		PROFILE_ZONE_N("d3d9::SetScissorRect");
		TRACE_IF_ACTIVE(trace_SetScissorRect, pRect);
		return m_pIDirect3DDevice9->SetScissorRect(pRect);
	}

	HRESULT d3d9ex::D3D9Device::GetScissorRect(RECT* pRect)
	{
		TRACE_IF_ACTIVE(trace_GetScissorRect, pRect);
		return m_pIDirect3DDevice9->GetScissorRect(pRect);
	}

	HRESULT d3d9ex::D3D9Device::SetSoftwareVertexProcessing(BOOL bSoftware)
	{
		TRACE_IF_ACTIVE(trace_SetSoftwareVertexProcessing, bSoftware);
		return m_pIDirect3DDevice9->SetSoftwareVertexProcessing(bSoftware);
	}

	BOOL d3d9ex::D3D9Device::GetSoftwareVertexProcessing()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_GetSoftwareVertexProcessing);
		return m_pIDirect3DDevice9->GetSoftwareVertexProcessing();
	}

	HRESULT d3d9ex::D3D9Device::SetNPatchMode(float nSegments)
	{
		TRACE_IF_ACTIVE(trace_SetNPatchMode, nSegments);
		return m_pIDirect3DDevice9->SetNPatchMode(nSegments);
	}

	float d3d9ex::D3D9Device::GetNPatchMode()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_GetNPatchMode);
		return m_pIDirect3DDevice9->GetNPatchMode();
	}

	HRESULT d3d9ex::D3D9Device::DrawPrimitive([[maybe_unused]] D3DPRIMITIVETYPE PrimitiveType, [[maybe_unused]] UINT StartVertex, [[maybe_unused]] UINT PrimitiveCount)
	{
		PROFILE_ZONE_N("d3d9::DrawPrimitive");
		const game::session_log::draw_timer timer;
		TRACE_IF_ACTIVE(trace_DrawPrimitive, PrimitiveType, StartVertex, PrimitiveCount);
		if (game::menu_bg::should_skip_draw(m_pIDirect3DDevice9)) { game::frame_stats::note_route("MENU_BG_SKIP"); return S_OK; }
		// An orthographic draw into a screen-sized off-screen target never triggers the injection (game.hpp).
		const bool raster_only = game::begin_raster_only(m_pIDirect3DDevice9);
		const auto hr = renderer::get()->on_draw_primitive(m_pIDirect3DDevice9, PrimitiveType, StartVertex, PrimitiveCount);
		if (raster_only) game::end_raster_only(m_pIDirect3DDevice9);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount)
	{
		PROFILE_ZONE_N("d3d9::DrawIndexedPrimitive");
		const game::session_log::draw_timer timer;
		TRACE_IF_ACTIVE(trace_DrawIndexedPrimitive, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
		if (game::menu_bg::should_skip_draw(m_pIDirect3DDevice9)) { game::frame_stats::note_route("MENU_BG_SKIP"); return S_OK; }
		// An orthographic draw into a screen-sized off-screen target never triggers the injection (game.hpp).
		const bool raster_only = game::begin_raster_only(m_pIDirect3DDevice9);
		const auto hr = renderer::get()->on_draw_indexed_prim(m_pIDirect3DDevice9, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
		if (raster_only) game::end_raster_only(m_pIDirect3DDevice9);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
	{
		PROFILE_ZONE_N("d3d9::DrawPrimitiveUP");
		const game::session_log::draw_timer timer;
		TRACE_IF_ACTIVE(trace_DrawPrimitiveUP, PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
		if (game::menu_bg::should_skip_draw(m_pIDirect3DDevice9)) { game::frame_stats::note_route("MENU_BG_SKIP"); return S_OK; }
		// Not routed through the renderer: the draw must not inherit a sticky
		// material-protocol payload from whatever the renderer drew last.
		game::frame_stats::note_route("UP");
		// A frame rasterised for a menu scene injects before its first draw (menu_scene.hpp).
		game::menu_scene::before_draw(m_pIDirect3DDevice9, false);
		// After the frame's UI boundary the injection runs before the first 2D draw (ui_boundary.hpp).
		if (game::ui_boundary::in_ui()) game::ui_boundary::before_draw(m_pIDirect3DDevice9);
		remix_protocol::none(m_pIDirect3DDevice9);
		shared::common::ffp_state::get().flush_extra_textures(m_pIDirect3DDevice9);
		const bool raster_only = game::begin_raster_only(m_pIDirect3DDevice9);
		const auto hr = m_pIDirect3DDevice9->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
		if (raster_only) game::end_raster_only(m_pIDirect3DDevice9);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::DrawIndexedPrimitiveUP(
		[[maybe_unused]] D3DPRIMITIVETYPE PrimitiveType,
		[[maybe_unused]] UINT MinVertexIndex,
		[[maybe_unused]] UINT NumVertices,
		[[maybe_unused]] UINT PrimitiveCount,
		[[maybe_unused]] CONST void* pIndexData,
		[[maybe_unused]] D3DFORMAT IndexDataFormat,
		[[maybe_unused]] CONST void* pVertexStreamZeroData,
		[[maybe_unused]] UINT VertexStreamZeroStride)
	{
		PROFILE_ZONE_N("d3d9::DrawIndexedPrimitiveUP");
		const game::session_log::draw_timer timer;
		TRACE_IF_ACTIVE(trace_DrawIndexedPrimitiveUP, PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
		if (game::menu_bg::should_skip_draw(m_pIDirect3DDevice9)) { game::frame_stats::note_route("MENU_BG_SKIP"); return S_OK; }
		// See DrawPrimitiveUP: clear any sticky protocol payload first.
		game::frame_stats::note_route("UP");
		// A frame rasterised for a menu scene injects before its first draw (menu_scene.hpp).
		game::menu_scene::before_draw(m_pIDirect3DDevice9, false);
		// After the frame's UI boundary the injection runs before the first 2D draw (ui_boundary.hpp).
		if (game::ui_boundary::in_ui()) game::ui_boundary::before_draw(m_pIDirect3DDevice9);
		remix_protocol::none(m_pIDirect3DDevice9);
		shared::common::ffp_state::get().flush_extra_textures(m_pIDirect3DDevice9);
		const bool raster_only = game::begin_raster_only(m_pIDirect3DDevice9);
		const auto hr = m_pIDirect3DDevice9->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
		if (raster_only) game::end_raster_only(m_pIDirect3DDevice9);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags)
	{
		PROFILE_ZONE_N("d3d9::ProcessVertices");
		TRACE_IF_ACTIVE(trace_ProcessVertices, SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags);
		return m_pIDirect3DDevice9->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags);
	}

	HRESULT d3d9ex::D3D9Device::CreateVertexDeclaration(CONST D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl)
	{
		PROFILE_ZONE_N("d3d9::CreateVertexDeclaration");
		TRACE_IF_ACTIVE(trace_CreateVertexDeclaration, pVertexElements, ppDecl);
		auto hr = m_pIDirect3DDevice9->CreateVertexDeclaration(pVertexElements, ppDecl);
		if (SUCCEEDED(hr) && ppDecl && *ppDecl)
			if (auto* t = tracer::get(); t && t->is_capturing()) t->record_created_handle(*ppDecl);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl)
	{
		PROFILE_ZONE_N("d3d9::SetVertexDeclaration");
		TRACE_IF_ACTIVE(trace_SetVertexDeclaration, pDecl);
		shared::common::ffp_state::get().on_set_vertex_declaration(pDecl);
		return m_pIDirect3DDevice9->SetVertexDeclaration(pDecl);
	}

	HRESULT d3d9ex::D3D9Device::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl)
	{
		TRACE_IF_ACTIVE(trace_GetVertexDeclaration, ppDecl);
		return m_pIDirect3DDevice9->GetVertexDeclaration(ppDecl);
	}

	HRESULT d3d9ex::D3D9Device::SetFVF(DWORD FVF)
	{
		PROFILE_ZONE_N("d3d9::SetFVF");
		TRACE_IF_ACTIVE(trace_SetFVF, FVF);
		return m_pIDirect3DDevice9->SetFVF(FVF);
	}

	HRESULT d3d9ex::D3D9Device::GetFVF(DWORD* pFVF)
	{
		TRACE_IF_ACTIVE(trace_GetFVF, pFVF);
		return m_pIDirect3DDevice9->GetFVF(pFVF);
	}

	HRESULT d3d9ex::D3D9Device::CreateVertexShader(CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader)
	{
		PROFILE_ZONE_N("d3d9::CreateVertexShader");
		TRACE_IF_ACTIVE(trace_CreateVertexShader, pFunction, ppShader);
		auto hr = m_pIDirect3DDevice9->CreateVertexShader(pFunction, ppShader);
		if (SUCCEEDED(hr) && ppShader && *ppShader)
			if (auto* t = tracer::get(); t && t->is_capturing()) t->record_created_handle(*ppShader);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::SetVertexShader(IDirect3DVertexShader9* pShader)
	{
		PROFILE_ZONE_N("d3d9::SetVertexShader");
		TRACE_IF_ACTIVE(trace_SetVertexShader, pShader);
		// Classifies the VS on first sight (CTAB parse), so the per-draw lookup
		// in the vertex expanders is a pointer-keyed hit.
		shared::common::g_vs_classifier.classify(pShader);
		shared::common::ffp_state::get().on_set_vertex_shader(pShader);
		return m_pIDirect3DDevice9->SetVertexShader(pShader);
	}

	HRESULT d3d9ex::D3D9Device::GetVertexShader(IDirect3DVertexShader9** ppShader)
	{
		TRACE_IF_ACTIVE(trace_GetVertexShader, ppShader);
		return m_pIDirect3DDevice9->GetVertexShader(ppShader);
	}

	HRESULT d3d9ex::D3D9Device::SetVertexShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount)
	{
		PROFILE_ZONE_N("d3d9::SetVSConstF");
		TRACE_IF_ACTIVE(trace_SetVertexShaderConstantF, StartRegister, pConstantData, Vector4fCount);

		// Redundant-write filter for non-bone constants. count==3 is the bone
		// upload signature consumed by game::on_set_vs_const_f's num_bones /
		// bones_drawn state machine; those must always fire, even with
		// identical data, to keep per-object bone tracking correct.
		auto& ffp = shared::common::ffp_state::get();
		if (Vector4fCount != 3 && ffp.vs_const_matches(StartRegister, pConstantData, Vector4fCount))
			return D3D_OK;

		ffp.on_set_vs_const_f(StartRegister, pConstantData, Vector4fCount);
		if (auto* d = diagnostics::get())
			d->on_set_vs_const_f(StartRegister, pConstantData, Vector4fCount);

		// When the upload is consumed as a skinned bone it is captured for the
		// Remix-API skinned path, and no traced draw runs the game's skinning
		// VS. Skip the forward (one fewer bridge crossing per bone);
		// passthrough draws that do (offscreen targets) flush it back.
		if (game::on_set_vs_const_f(m_pIDirect3DDevice9, StartRegister, pConstantData, Vector4fCount))
			return D3D_OK;

		return m_pIDirect3DDevice9->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}

	HRESULT d3d9ex::D3D9Device::GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount)
	{
		TRACE_IF_ACTIVE(trace_GetVertexShaderConstantF, StartRegister, pConstantData, Vector4fCount);
		return m_pIDirect3DDevice9->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}

	HRESULT d3d9ex::D3D9Device::SetVertexShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount)
	{
		TRACE_IF_ACTIVE(trace_SetVertexShaderConstantI, StartRegister, pConstantData, Vector4iCount);
		return m_pIDirect3DDevice9->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}

	HRESULT d3d9ex::D3D9Device::GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount)
	{
		TRACE_IF_ACTIVE(trace_GetVertexShaderConstantI, StartRegister, pConstantData, Vector4iCount);
		return m_pIDirect3DDevice9->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}

	HRESULT d3d9ex::D3D9Device::SetVertexShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT  BoolCount)
	{
		TRACE_IF_ACTIVE(trace_SetVertexShaderConstantB, StartRegister, pConstantData, BoolCount);
		return m_pIDirect3DDevice9->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
	}

	HRESULT d3d9ex::D3D9Device::GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount)
	{
		TRACE_IF_ACTIVE(trace_GetVertexShaderConstantB, StartRegister, pConstantData, BoolCount);
		return m_pIDirect3DDevice9->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
	}

	HRESULT d3d9ex::D3D9Device::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride)
	{
		PROFILE_ZONE_N("d3d9::SetStreamSource");
		TRACE_IF_ACTIVE(trace_SetStreamSource, StreamNumber, pStreamData, OffsetInBytes, Stride);
		shared::common::ffp_state::get().on_set_stream_source(StreamNumber, pStreamData, OffsetInBytes, Stride);
		return m_pIDirect3DDevice9->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride);
	}

	HRESULT d3d9ex::D3D9Device::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* OffsetInBytes, UINT* pStride)
	{
		TRACE_IF_ACTIVE(trace_GetStreamSource, StreamNumber, ppStreamData, OffsetInBytes, pStride);
		return m_pIDirect3DDevice9->GetStreamSource(StreamNumber, ppStreamData, OffsetInBytes, pStride);
	}

	HRESULT d3d9ex::D3D9Device::SetStreamSourceFreq(UINT StreamNumber, UINT Divider)
	{
		TRACE_IF_ACTIVE(trace_SetStreamSourceFreq, StreamNumber, Divider);
		return m_pIDirect3DDevice9->SetStreamSourceFreq(StreamNumber, Divider);
	}

	HRESULT d3d9ex::D3D9Device::GetStreamSourceFreq(UINT StreamNumber, UINT* Divider)
	{
		TRACE_IF_ACTIVE(trace_GetStreamSourceFreq, StreamNumber, Divider);
		return m_pIDirect3DDevice9->GetStreamSourceFreq(StreamNumber, Divider);
	}

	HRESULT d3d9ex::D3D9Device::SetIndices(IDirect3DIndexBuffer9* pIndexData)
	{
		PROFILE_ZONE_N("d3d9::SetIndices");
		TRACE_IF_ACTIVE(trace_SetIndices, pIndexData);
		return m_pIDirect3DDevice9->SetIndices(pIndexData);
	}

	HRESULT d3d9ex::D3D9Device::GetIndices(IDirect3DIndexBuffer9** ppIndexData)
	{
		TRACE_IF_ACTIVE(trace_GetIndices, ppIndexData);
		return m_pIDirect3DDevice9->GetIndices(ppIndexData);
	}

	HRESULT d3d9ex::D3D9Device::CreatePixelShader(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader)
	{
		PROFILE_ZONE_N("d3d9::CreatePixelShader");
		TRACE_IF_ACTIVE(trace_CreatePixelShader, pFunction, ppShader);
		auto hr = m_pIDirect3DDevice9->CreatePixelShader(pFunction, ppShader);
		if (SUCCEEDED(hr) && ppShader && *ppShader)
			if (auto* t = tracer::get(); t && t->is_capturing()) t->record_created_handle(*ppShader);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::SetPixelShader(IDirect3DPixelShader9* pShader)
	{
		PROFILE_ZONE_N("d3d9::SetPixelShader");
		TRACE_IF_ACTIVE(trace_SetPixelShader, pShader);
		// Classifies the PS on first sight. The draw paths look it up per draw
		// for the sampler roles and the RS-protocol payload.
		shared::common::g_ps_classifier.classify(pShader);
		if (shared::common::ffp_state::get().on_set_pixel_shader(pShader))
			return S_OK; // swallowed while FFP is active
		return m_pIDirect3DDevice9->SetPixelShader(pShader);
	}

	HRESULT d3d9ex::D3D9Device::GetPixelShader(IDirect3DPixelShader9** ppShader)
	{
		TRACE_IF_ACTIVE(trace_GetPixelShader, ppShader);
		return m_pIDirect3DDevice9->GetPixelShader(ppShader);
	}

	HRESULT d3d9ex::D3D9Device::SetPixelShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount)
	{
		PROFILE_ZONE_N("d3d9::SetPSConstF");
		TRACE_IF_ACTIVE(trace_SetPixelShaderConstantF, StartRegister, pConstantData, Vector4fCount);

		// Redundant-write filter. PS constants have no game-side state
		// machine, only ffp_state's cache update, which does nothing when the
		// data is unchanged.
		auto& ffp = shared::common::ffp_state::get();
		if (ffp.ps_const_matches(StartRegister, pConstantData, Vector4fCount))
			return D3D_OK;

		ffp.on_set_ps_const_f(StartRegister, pConstantData, Vector4fCount);
		return m_pIDirect3DDevice9->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}

	HRESULT d3d9ex::D3D9Device::GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount)
	{
		TRACE_IF_ACTIVE(trace_GetPixelShaderConstantF, StartRegister, pConstantData, Vector4fCount);
		return m_pIDirect3DDevice9->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}

	HRESULT d3d9ex::D3D9Device::SetPixelShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount)
	{
		TRACE_IF_ACTIVE(trace_SetPixelShaderConstantI, StartRegister, pConstantData, Vector4iCount);
		return m_pIDirect3DDevice9->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}

	HRESULT d3d9ex::D3D9Device::GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount)
	{
		TRACE_IF_ACTIVE(trace_GetPixelShaderConstantI, StartRegister, pConstantData, Vector4iCount);
		return m_pIDirect3DDevice9->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}

	HRESULT d3d9ex::D3D9Device::SetPixelShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT  BoolCount)
	{
		TRACE_IF_ACTIVE(trace_SetPixelShaderConstantB, StartRegister, pConstantData, BoolCount);
		return m_pIDirect3DDevice9->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
	}

	HRESULT d3d9ex::D3D9Device::GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount)
	{
		TRACE_IF_ACTIVE(trace_GetPixelShaderConstantB, StartRegister, pConstantData, BoolCount);
		return m_pIDirect3DDevice9->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
	}

	HRESULT d3d9ex::D3D9Device::DrawRectPatch(UINT Handle, CONST float* pNumSegs, CONST D3DRECTPATCH_INFO* pRectPatchInfo)
	{
		TRACE_IF_ACTIVE(trace_DrawRectPatch, Handle, pNumSegs, pRectPatchInfo);
		return m_pIDirect3DDevice9->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo);
	}

	HRESULT d3d9ex::D3D9Device::DrawTriPatch(UINT Handle, CONST float* pNumSegs, CONST D3DTRIPATCH_INFO* pTriPatchInfo)
	{
		TRACE_IF_ACTIVE(trace_DrawTriPatch, Handle, pNumSegs, pTriPatchInfo);
		return m_pIDirect3DDevice9->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo);
	}

	HRESULT d3d9ex::D3D9Device::DeletePatch(UINT Handle)
	{
		TRACE_IF_ACTIVE(trace_DeletePatch, Handle);
		return m_pIDirect3DDevice9->DeletePatch(Handle);
	}

	HRESULT d3d9ex::D3D9Device::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery)
	{
		PROFILE_ZONE_N("d3d9::CreateQuery");
		TRACE_IF_ACTIVE(trace_CreateQuery, Type, ppQuery);
		return m_pIDirect3DDevice9->CreateQuery(Type, ppQuery);
	}

	// IDirect3DDevice9Ex. Reached only when the device was created through
	// CreateDeviceEx. On a plain CreateDevice device m_pIDirect3DDevice9Ex is
	// null and these report D3DERR_INVALIDCALL, as QueryInterface tells callers.

#define REQUIRE_DEVICE9EX() if (!m_pIDirect3DDevice9Ex) return D3DERR_INVALIDCALL

	HRESULT d3d9ex::D3D9Device::SetConvolutionMonoKernel(UINT width, UINT height, float* rows, float* columns)
	{
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->SetConvolutionMonoKernel(width, height, rows, columns);
	}

	HRESULT d3d9ex::D3D9Device::ComposeRects(IDirect3DSurface9* pSrc, IDirect3DSurface9* pDst, IDirect3DVertexBuffer9* pSrcRectDescs, UINT NumRects, IDirect3DVertexBuffer9* pDstRectDescs, D3DCOMPOSERECTSOP Operation, int Xoffset, int Yoffset)
	{
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->ComposeRects(pSrc, pDst, pSrcRectDescs, NumRects, pDstRectDescs, Operation, Xoffset, Yoffset);
	}

	HRESULT d3d9ex::D3D9Device::PresentEx(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion, DWORD dwFlags)
	{
		PROFILE_ZONE_N("PresentEx");
		REQUIRE_DEVICE9EX();
		TRACE_IF_ACTIVE(trace_Present, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
		LARGE_INTEGER t0, t1, t2;
		QueryPerformanceCounter(&t0);
		render_overlay();
		pre_present();
		QueryPerformanceCounter(&t1);
		auto hr = m_pIDirect3DDevice9Ex->PresentEx(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
		QueryPerformanceCounter(&t2);
		game::session_log::note_present(t1.QuadPart - t0.QuadPart, t2.QuadPart - t1.QuadPart);
		post_present();
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::GetGPUThreadPriority(INT* pPriority)
	{
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->GetGPUThreadPriority(pPriority);
	}

	HRESULT d3d9ex::D3D9Device::SetGPUThreadPriority(INT Priority)
	{
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->SetGPUThreadPriority(Priority);
	}

	HRESULT d3d9ex::D3D9Device::WaitForVBlank(UINT iSwapChain)
	{
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->WaitForVBlank(iSwapChain);
	}

	HRESULT d3d9ex::D3D9Device::CheckResourceResidency(IDirect3DResource9** pResourceArray, UINT32 NumResources)
	{
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->CheckResourceResidency(pResourceArray, NumResources);
	}

	HRESULT d3d9ex::D3D9Device::SetMaximumFrameLatency(UINT MaxLatency)
	{
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->SetMaximumFrameLatency(MaxLatency);
	}

	HRESULT d3d9ex::D3D9Device::GetMaximumFrameLatency(UINT* pMaxLatency)
	{
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->GetMaximumFrameLatency(pMaxLatency);
	}

	HRESULT d3d9ex::D3D9Device::CheckDeviceState(HWND hDestinationWindow)
	{
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->CheckDeviceState(hDestinationWindow);
	}

	HRESULT d3d9ex::D3D9Device::CreateRenderTargetEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage)
	{
		PROFILE_ZONE_N("d3d9::CreateRenderTargetEx");
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->CreateRenderTargetEx(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle, Usage);
	}

	HRESULT d3d9ex::D3D9Device::CreateOffscreenPlainSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage)
	{
		PROFILE_ZONE_N("d3d9::CreateOffscreenPlainSurfaceEx");
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->CreateOffscreenPlainSurfaceEx(Width, Height, Format, Pool, ppSurface, pSharedHandle, Usage);
	}

	HRESULT d3d9ex::D3D9Device::CreateDepthStencilSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage)
	{
		PROFILE_ZONE_N("d3d9::CreateDepthStencilSurfaceEx");
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->CreateDepthStencilSurfaceEx(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle, Usage);
	}

	HRESULT d3d9ex::D3D9Device::ResetEx(D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode)
	{
		PROFILE_ZONE_N("d3d9::ResetEx");
		REQUIRE_DEVICE9EX();
		TRACE_IF_ACTIVE(trace_Reset, pPresentationParameters);
		pre_reset();
		const auto hr = m_pIDirect3DDevice9Ex->ResetEx(pPresentationParameters, pFullscreenDisplayMode);
		post_reset(hr);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::GetDisplayModeEx(UINT iSwapChain, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation)
	{
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->GetDisplayModeEx(iSwapChain, pMode, pRotation);
	}

#undef REQUIRE_DEVICE9EX

#pragma endregion

#pragma region _D3D9

	HRESULT __stdcall d3d9ex::_d3d9::QueryInterface(REFIID riid, void** ppvObj)
	{
		*ppvObj = nullptr;

		HRESULT hRes = m_pIDirect3D9->QueryInterface(riid, ppvObj);

		if (hRes == NOERROR) {
			*ppvObj = this;
		}

		return hRes;
	}

	ULONG __stdcall d3d9ex::_d3d9::AddRef()
	{
		return m_pIDirect3D9->AddRef();
	}

	ULONG __stdcall d3d9ex::_d3d9::Release()
	{
		ULONG count = m_pIDirect3D9->Release();
		if (!count) delete this;
		return count;
	}

	HRESULT __stdcall d3d9ex::_d3d9::RegisterSoftwareDevice(void* pInitializeFunction)
	{
		return m_pIDirect3D9->RegisterSoftwareDevice(pInitializeFunction);
	}

	UINT __stdcall d3d9ex::_d3d9::GetAdapterCount()
	{
		return m_pIDirect3D9->GetAdapterCount();
	}

	HRESULT __stdcall d3d9ex::_d3d9::GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier)
	{
		return m_pIDirect3D9->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
	}

	UINT __stdcall d3d9ex::_d3d9::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format)
	{
		return m_pIDirect3D9->GetAdapterModeCount(Adapter, Format);
	}

	HRESULT __stdcall d3d9ex::_d3d9::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode)
	{
		return m_pIDirect3D9->EnumAdapterModes(Adapter, Format, Mode, pMode);
	}

	HRESULT __stdcall d3d9ex::_d3d9::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode)
	{
		return m_pIDirect3D9->GetAdapterDisplayMode(Adapter, pMode);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CheckDeviceType(UINT iAdapter, D3DDEVTYPE DevType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed)
	{
		return m_pIDirect3D9->CheckDeviceType(iAdapter, DevType, DisplayFormat, BackBufferFormat, bWindowed);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat)
	{
		return m_pIDirect3D9->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels)
	{
		return m_pIDirect3D9->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat)
	{
		return m_pIDirect3D9->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat)
	{
		return m_pIDirect3D9->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps)
	{
		return m_pIDirect3D9->GetDeviceCaps(Adapter, DeviceType, pCaps);
	}

	HMONITOR __stdcall d3d9ex::_d3d9::GetAdapterMonitor(UINT Adapter)
	{
		return m_pIDirect3D9->GetAdapterMonitor(Adapter);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice9** ppReturnedDeviceInterface)
	{
		HRESULT hres = m_pIDirect3D9->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
		shared::common::log("d3d9", "m_pIDirect3D9->CreateDevice", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);

		// D3D9 nulls the out-pointer on failure, and games retry CreateDevice
		// with different parameters. A proxy around the failed result would
		// wrap a null device and crash on the first call through it.
		if (FAILED(hres) || !ppReturnedDeviceInterface || !*ppReturnedDeviceInterface)
		{
			shared::common::log("d3d9",
				std::format("CreateDevice failed (hr=0x{:08X}); not wrapping device", static_cast<unsigned>(hres)),
				shared::common::LOG_TYPE::LOG_TYPE_WARN);
			return hres;
		}

		game::note_default_render_target(*ppReturnedDeviceInterface);
		*ppReturnedDeviceInterface = new d3d9ex::D3D9Device(*ppReturnedDeviceInterface);
		shared::globals::d3d_device = *ppReturnedDeviceInterface;

		return hres;
	}


#pragma endregion

#pragma region _D3D9Ex

	namespace
	{
		// Who called Direct3DCreate9Ex last ("module (return address)") and
		// whether that was FalloutNV.exe, for the problem notice.
		std::string g_ex_requester;
		bool g_ex_requester_is_game = false;

		// True when `addr` lies in FalloutNV.exe's image.
		bool in_game_exe(uintptr_t addr)
		{
			const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
			if (!base) return false;
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			return addr >= base && addr < base + nt->OptionalHeader.SizeOfImage;
		}

		// True when `hwnd` is the game's main window (its class name holds
		// "Fallout: New Vegas").
		bool is_game_window(HWND hwnd)
		{
			char name[256] = {};
			return hwnd && GetClassNameA(hwnd, name, sizeof(name)) && std::string_view(name).contains("Fallout: New Vegas");
		}

		// The problem notice's detail: who asked for D3D9Ex and what to do.
		std::string d3d9ex_cause()
		{
			if (g_ex_requester_is_game)
				return std::format("FalloutNV.exe asked for it ({}): the game does this in fullscreen on a PC with more "
					"than one monitor when the launcher resolution is not the main monitor's own, and this mod could "
					"not turn that off (the remix-comp log says why). Set the launcher resolution to the main "
					"monitor's resolution, or play windowed.", g_ex_requester);
			if (g_ex_requester.empty())
				return "Another mod or overlay asked for it. Remove mods that change how the game creates its "
					"Direct3D device.";
			return std::format("{} asked for it. Turn off that mod's DirectX or D3D9Ex option, or remove the mod.",
				g_ex_requester);
		}
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::QueryInterface(REFIID riid, void** ppvObj)
	{
		*ppvObj = nullptr;
		HRESULT hRes = m_pIDirect3D9Ex->QueryInterface(riid, ppvObj);

		if (hRes == NOERROR) {
			*ppvObj = this;
		}

		return hRes;
	}

	ULONG __stdcall d3d9ex::_d3d9ex::AddRef()
	{
		return m_pIDirect3D9Ex->AddRef();
	}

	ULONG __stdcall d3d9ex::_d3d9ex::Release()
	{
		ULONG count = m_pIDirect3D9Ex->Release();
		if (!count) delete this;
		return count;
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::RegisterSoftwareDevice(void* pInitializeFunction)
	{
		return m_pIDirect3D9Ex->RegisterSoftwareDevice(pInitializeFunction);
	}

	UINT __stdcall d3d9ex::_d3d9ex::GetAdapterCount()
	{
		return m_pIDirect3D9Ex->GetAdapterCount();
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier)
	{
		return m_pIDirect3D9Ex->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
	}

	UINT __stdcall d3d9ex::_d3d9ex::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format)
	{
		return m_pIDirect3D9Ex->GetAdapterModeCount(Adapter, Format);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode)
	{
		return m_pIDirect3D9Ex->EnumAdapterModes(Adapter, Format, Mode, pMode);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode)
	{
		return m_pIDirect3D9Ex->GetAdapterDisplayMode(Adapter, pMode);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CheckDeviceType(UINT iAdapter, D3DDEVTYPE DevType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed)
	{
		return m_pIDirect3D9Ex->CheckDeviceType(iAdapter, DevType, DisplayFormat, BackBufferFormat, bWindowed);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat)
	{
		return m_pIDirect3D9Ex->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels)
	{
		return m_pIDirect3D9Ex->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat)
	{
		return m_pIDirect3D9Ex->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat)
	{
		return m_pIDirect3D9Ex->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps)
	{
		return m_pIDirect3D9Ex->GetDeviceCaps(Adapter, DeviceType, pCaps);
	}

	HMONITOR __stdcall d3d9ex::_d3d9ex::GetAdapterMonitor(UINT Adapter)
	{
		return m_pIDirect3D9Ex->GetAdapterMonitor(Adapter);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice9** ppReturnedDeviceInterface)
	{
		HRESULT hres = m_pIDirect3D9Ex->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
		shared::common::log("d3d9", "m_pIDirect3D9Ex->CreateDevice", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);

		if (FAILED(hres) || !ppReturnedDeviceInterface || !*ppReturnedDeviceInterface)
		{
			shared::common::log("d3d9",
				std::format("CreateDevice failed (hr=0x{:08X}); not wrapping device", static_cast<unsigned>(hres)),
				shared::common::LOG_TYPE::LOG_TYPE_WARN);
			return hres;
		}

		game::note_default_render_target(*ppReturnedDeviceInterface);
		*ppReturnedDeviceInterface = new d3d9ex::D3D9Device(*ppReturnedDeviceInterface);
		shared::globals::d3d_device = *ppReturnedDeviceInterface;

		return hres;
	}

	UINT __stdcall d3d9ex::_d3d9ex::GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter)
	{
		return (m_pIDirect3D9Ex->GetAdapterModeCountEx(Adapter, pFilter));
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::EnumAdapterModesEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter, UINT Mode, D3DDISPLAYMODEEX* pMode)
	{
		return (m_pIDirect3D9Ex->EnumAdapterModesEx(Adapter, pFilter, Mode, pMode));
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation)
	{
		return (m_pIDirect3D9Ex->GetAdapterDisplayModeEx(Adapter, pMode, pRotation));
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CreateDeviceEx(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode, IDirect3DDevice9Ex** ppReturnedDeviceInterface)
	{
		HRESULT hres = m_pIDirect3D9Ex->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, pFullscreenDisplayMode, ppReturnedDeviceInterface);
		shared::common::log("d3d9", "m_pIDirect3D9Ex->CreateDeviceEx", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);

		if (FAILED(hres) || !ppReturnedDeviceInterface || !*ppReturnedDeviceInterface)
		{
			shared::common::log("d3d9",
				std::format("CreateDeviceEx failed (hr=0x{:08X}); not wrapping device", static_cast<unsigned>(hres)),
				shared::common::LOG_TYPE::LOG_TYPE_WARN);
			return hres;
		}

		// Ex devices are wrapped like plain ones (D3D9Device implements
		// IDirect3DDevice9Ex); unwrapped, none of the hooks would fire and
		// globals::d3d_device would stay null.
		auto* proxy = new d3d9ex::D3D9Device(*ppReturnedDeviceInterface);
		*ppReturnedDeviceInterface = proxy;
		shared::globals::d3d_device = proxy;

		// Only the game's own device matters for the notice; an overlay's or
		// another mod's private Ex device is logged.
		const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
		const HWND window = pPresentationParameters && pPresentationParameters->hDeviceWindow ?
			pPresentationParameters->hDeviceWindow : hFocusWindow;
		const bool game_device = is_game_window(window) || in_game_exe(caller);
		shared::common::log("d3d9", std::format("D3D9Ex device created by {} (0x{:X}) on an interface from {}; {}",
			game::call_chain::owner_of(caller), caller, g_ex_requester.empty() ? std::string("?") : g_ex_requester,
			game_device ? "the game's device" : "not the game's window"),
			game_device ? shared::common::LOG_TYPE::LOG_TYPE_WARN : shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, true);
		if (game_device)
			game::fault_report::report(game::fault_report::Fault::D3D9ExDevice, d3d9ex_cause());

		return hres;
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::GetAdapterLUID(UINT Adapter, LUID* pLUID)
	{
		return (m_pIDirect3D9Ex->GetAdapterLUID(Adapter, pLUID));
	}
#pragma endregion

	d3d9ex::d3d9ex() {}
}

// ---- Exported entry points (linked via d3d9.def) ----

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion)
{
	if (d3d9_proxy::passthrough())
	{
		const auto fn = d3d9_proxy::get_Direct3DCreate9();
		return fn ? fn(SDKVersion) : nullptr;
	}
	shared::common::log("d3d9", "Direct3DCreate9 called. Creating proxy interface.");
	comp::game::session_log::on_d3d_create();
	// The game has read its INI files and reads none of these settings yet.
	comp::game::game_ini::apply();
	auto real_fn = d3d9_proxy::get_Direct3DCreate9();
	if (!real_fn) return nullptr;

	IDirect3D9* real_d3d9 = real_fn(SDKVersion);
	if (!real_d3d9) return nullptr;

	shared::globals::d3d9_interface = new comp::d3d9ex::_d3d9(real_d3d9);
	return shared::globals::d3d9_interface;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D)
{
	if (d3d9_proxy::passthrough())
	{
		const auto fn = d3d9_proxy::get_Direct3DCreate9Ex();
		return fn ? fn(SDKVersion, ppD3D) : D3DERR_NOTAVAILABLE;
	}

	// The engine asks for D3D9Ex in fullscreen on a PC with more than one
	// monitor when iSize W/H differs from the primary desktop size (renderer
	// init 0x4DA985). A D3D9Ex device has no D3DPOOL_MANAGED, so every game
	// texture ends up in D3DPOOL_DEFAULT with no CPU copy and the Remix API
	// paths refuse all world geometry: only the sky renders. Declining makes
	// NiDX9Renderer::CreateDirect3D9 (0xE69460) fall back to Direct3DCreate9
	// and a plain fullscreen device at the same resolution. That function
	// also marks its result failed when the Ex call fails (`or esi, -1`),
	// and its caller 0xE76210 then gives up with "Could not initialize
	// DirectX9", so the mark is written as `or esi, 0` first. When the mark
	// holds other bytes the request is served, and the problem notice names
	// the game as the requester.
	const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
	const std::string owner = comp::game::call_chain::owner_of(caller);
	const std::string requester = std::format("{} (0x{:X})",
		owner == "allocated memory" ? "Another mod's hook code outside any DLL" : owner, caller);
	if (comp::in_game_exe(caller))
	{
		const fnv_exe::Code& mark = fnv_exe::addr().d3d9ex_failed_mark;
		const auto* now = reinterpret_cast<const uint8_t*>(mark.addr);
		const bool cleared = mark.addr && std::memcmp(now, mark.bytes, mark.len - 1u) == 0 && now[mark.len - 1u] == 0;
		const bool vanilla = mark.addr && std::memcmp(now, mark.bytes, mark.len) == 0;
		if (cleared || (vanilla && comp::game::call_chain::patch_byte(mark.addr + mark.len - 1u, 0)))
		{
			shared::common::log("d3d9", "Direct3DCreate9Ex called by the game (fullscreen, several monitors, "
				"iSize W/H not the primary desktop size): declined, so the game creates a plain D3D9 device "
				"(D3D9Ex has no MANAGED textures and path tracing needs their CPU copy)",
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
			if (ppD3D) *ppD3D = nullptr;
			return D3DERR_NOTAVAILABLE;
		}
		shared::common::log("d3d9", std::format("Direct3DCreate9Ex called by the game, which cannot be declined: "
			"0x{:X} holds [{}], expected the engine's [{}]. The game gets a D3D9Ex device.", mark.addr,
			mark.addr ? comp::game::call_chain::hex(now, mark.len) : std::string("nothing"),
			comp::game::call_chain::hex(mark.bytes, mark.len)), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
	}

	shared::common::log("d3d9", std::format("Direct3DCreate9Ex called by {}. Creating proxy interface.", requester));
	comp::g_ex_requester = requester;
	comp::g_ex_requester_is_game = comp::in_game_exe(caller);
	comp::game::game_ini::apply();
	auto real_fn = d3d9_proxy::get_Direct3DCreate9Ex();
	if (!real_fn || !ppD3D) return D3DERR_NOTAVAILABLE;

	IDirect3D9Ex* real_d3d9ex = nullptr;
	HRESULT hr = real_fn(SDKVersion, &real_d3d9ex);
	if (FAILED(hr) || !real_d3d9ex) return hr;

	auto* proxy = new comp::d3d9ex::_d3d9ex(real_d3d9ex);
	shared::globals::d3d9_interface = proxy;
	*ppD3D = proxy;
	return hr;
}
