#include "std_include.hpp"
#include "tex_pool.hpp"

#include <atomic>

#include "api_textures.hpp"
#include "call_chain.hpp"
#include "shared/common/config.hpp"
#include "shared/fnv_exe.hpp"

namespace comp::game::tex_pool
{
	namespace
	{
		std::atomic<uint64_t> g_promoted{ 0 }, g_promoted_bytes{ 0 };
		std::atomic<uint64_t> g_kept{ 0 }, g_kept_bytes{ 0 };
		std::atomic<uint64_t> g_by_switch{ 0 }, g_by_format{ 0 }, g_by_autogen{ 0 }, g_by_import{ 0 }, g_by_facegen{ 0 };

		// The engine's D3DX texture file load (fnv_exe texture_from_file). A
		// thread-local flag says the texture it creates is a FaceGen face map.
		using TextureFromFile_t = bool(__fastcall*)(void* self, void* edx, const char* path, void* renderer, void* prefs,
			void* file);
		TextureFromFile_t g_orig_texture_from_file = nullptr;
		DWORD g_tls_facegen = TLS_OUT_OF_INDEXES;
		bool g_facegen_hooked = false;

		// `path` contains `folder` (lower case, '?' = either slash), in any case.
		bool in_folder(const char* path, const char* folder)
		{
			if (!path) return false;
			const size_t n = std::strlen(folder);
			for (const char* p = path; *p; p++)
			{
				size_t i = 0;
				for (; i < n && p[i]; i++)
				{
					const char c = (p[i] >= 'A' && p[i] <= 'Z') ? static_cast<char>(p[i] + ('a' - 'A')) : p[i];
					if (folder[i] == '?' ? (c != '\\' && c != '/') : c != folder[i]) break;
				}
				if (i == n) return true;
			}
			return false;
		}

		// An NPC's FaceGen face map:
		// "data\Textures\Characters\FaceMods\<plugin>\<form id>_<n>.dds"
		// (TESNPC's face texture path, 0x604310). The engine's FaceGen export
		// writes that folder, so mods ship their face maps there too.
		bool is_facegen_map(const char* path)
		{
			return in_folder(path, "characters?facemods?");
		}

		// A hair texture ("data\Textures\Characters\Hair\..."), with [RemixApi]
		// ShaderBakeHair on: the hair bake reads the BaseMap too
		// (api_hair_bake.cpp).
		bool is_baked_hair_map(const char* path)
		{
			return shared::common::config::get().remix_api.shader_bake_hair && in_folder(path, "characters?hair?");
		}

		// A hair LayerMap: "<name>_hl.dds", vanilla and mods alike. The hair
		// material reads its alpha for the layer weights (api_material.cpp
		// hair_layer_textures).
		bool is_hair_layer_map(const char* path)
		{
			if (!path) return false;
			static constexpr char kSuffix[] = "_hl.dds";
			const size_t n = sizeof(kSuffix) - 1, len = std::strlen(path);
			if (len < n) return false;
			for (size_t i = 0; i < n; i++)
			{
				const char c = path[len - n + i];
				if (((c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c) != kSuffix[i]) return false;
			}
			return true;
		}

		bool __fastcall texture_from_file_hk(void* self, void* edx, const char* path, void* renderer, void* prefs, void* file)
		{
			const bool facegen = is_facegen_map(path) || is_hair_layer_map(path) || is_baked_hair_map(path);
			LPVOID prev = TlsGetValue(g_tls_facegen);
			if (facegen) TlsSetValue(g_tls_facegen, reinterpret_cast<LPVOID>(1));
			const bool r = g_orig_texture_from_file(self, edx, path, renderer, prefs, file);
			if (facegen) TlsSetValue(g_tls_facegen, prev);
			return r;
		}

		// The engine's in-memory FaceGen face map build (fnv_exe
		// facegen_image_texture): with bLoadFaceGenHeadEGTFiles the face map
		// is made from the EGT on the CPU, and its D3D9 texture is created
		// and filled inside this call, without the texture file load.
		using FaceGenImageTexture_t = void*(__fastcall*)(void* self, void* edx, float lo, float hi, float scale, void* format);
		FaceGenImageTexture_t g_orig_facegen_image_texture = nullptr;

		void* __fastcall facegen_image_texture_hk(void* self, void* edx, float lo, float hi, float scale, void* format)
		{
			LPVOID prev = TlsGetValue(g_tls_facegen);
			TlsSetValue(g_tls_facegen, reinterpret_cast<LPVOID>(1));
			void* r = g_orig_facegen_image_texture(self, edx, lo, hi, scale, format);
			TlsSetValue(g_tls_facegen, prev);
			return r;
		}

		bool loading_facegen_map()
		{
			return g_facegen_hooked && TlsGetValue(g_tls_facegen) != nullptr;
		}
		std::atomic<bool> g_logged{ false };
		bool g_probed = false;

		bool keep_default()
		{
			return shared::common::config::get().remix_api.keep_default_pool_textures;
		}

		// Bytes per pixel, or per 4x4 block for the block-compressed formats.
		struct Layout
		{
			bool block = false;
			uint32_t bytes = 0;
		};

		bool layout_of(D3DFORMAT f, Layout& l)
		{
			switch (static_cast<DWORD>(f))
			{
			case D3DFMT_DXT1:
			case MAKEFOURCC('A', 'T', 'I', '1'):
				l = { true, 8 }; return true;
			case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
			case MAKEFOURCC('A', 'T', 'I', '2'):
				l = { true, 16 }; return true;
			case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_A4L4: case D3DFMT_R3G3B2:
				l = { false, 1 }; return true;
			case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
			case D3DFMT_X4R4G4B4: case D3DFMT_A8R3G3B2: case D3DFMT_A8L8: case D3DFMT_A8P8:
			case D3DFMT_L16: case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_R16F:
				l = { false, 2 }; return true;
			case D3DFMT_R8G8B8:
				l = { false, 3 }; return true;
			case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
			case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10: case D3DFMT_G16R16: case D3DFMT_V16U16:
			case D3DFMT_Q8W8V8U8: case D3DFMT_X8L8V8U8: case D3DFMT_A2W10V10U10: case D3DFMT_G16R16F:
			case D3DFMT_R32F:
				l = { false, 4 }; return true;
			case D3DFMT_A16B16G16R16: case D3DFMT_Q16W16V16U16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F:
				l = { false, 8 }; return true;
			case D3DFMT_A32B32G32R32F:
				l = { false, 16 }; return true;
			default:
				return false;
			}
		}

		uint32_t row_bytes(const Layout& l, uint32_t w) { return l.block ? ((w + 3u) / 4u) * l.bytes : w * l.bytes; }
		uint32_t row_count(const Layout& l, uint32_t h) { return l.block ? (h + 3u) / 4u : h; }

		void copy_rows(const void* src, INT src_pitch, void* dst, INT dst_pitch, uint32_t rows, uint32_t bytes)
		{
			auto* s = static_cast<const uint8_t*>(src);
			auto* d = static_cast<uint8_t*>(dst);
			for (uint32_t r = 0; r < rows; r++)
				std::memcpy(d + static_cast<size_t>(r) * dst_pitch, s + static_cast<size_t>(r) * src_pitch, bytes);
		}

		// Copies `src_rect` of `s` (the whole surface when null) to `dst_point`
		// of `d` (the origin when null). `exact` asks for surfaces of the same
		// size, as UpdateTexture's level pairs are.
		HRESULT copy_surface(IDirect3DSurface9* s, const RECT* src_rect, IDirect3DSurface9* d, const POINT* dst_point, bool exact)
		{
			D3DSURFACE_DESC sd = {}, dd = {};
			if (FAILED(s->GetDesc(&sd)) || FAILED(d->GetDesc(&dd))) return D3DERR_INVALIDCALL;
			if (sd.Format != dd.Format) return D3DERR_INVALIDCALL;
			if (exact && (sd.Width != dd.Width || sd.Height != dd.Height)) return D3DERR_INVALIDCALL;
			Layout l;
			if (!layout_of(sd.Format, l)) return D3DERR_INVALIDCALL;

			const RECT r = src_rect ? *src_rect : RECT{ 0, 0, static_cast<LONG>(sd.Width), static_cast<LONG>(sd.Height) };
			const POINT p = dst_point ? *dst_point : POINT{ 0, 0 };
			if (r.left < 0 || r.top < 0 || r.right <= r.left || r.bottom <= r.top ||
				r.right > static_cast<LONG>(sd.Width) || r.bottom > static_cast<LONG>(sd.Height))
				return D3DERR_INVALIDCALL;
			const LONG w = r.right - r.left, h = r.bottom - r.top;
			if (p.x < 0 || p.y < 0 || p.x + w > static_cast<LONG>(dd.Width) || p.y + h > static_cast<LONG>(dd.Height))
				return D3DERR_INVALIDCALL;
			if (l.block && ((r.left | r.top | p.x | p.y) & 3)) return D3DERR_INVALIDCALL;

			const RECT dr = { p.x, p.y, p.x + w, p.y + h };
			const bool whole_src = r.left == 0 && r.top == 0 && w == static_cast<LONG>(sd.Width) && h == static_cast<LONG>(sd.Height);
			const bool whole_dst = p.x == 0 && p.y == 0 && w == static_cast<LONG>(dd.Width) && h == static_cast<LONG>(dd.Height);

			D3DLOCKED_RECT sl = {}, dl = {};
			HRESULT hr = s->LockRect(&sl, whole_src ? nullptr : &r, D3DLOCK_READONLY);
			if (FAILED(hr)) return hr;
			hr = d->LockRect(&dl, whole_dst ? nullptr : &dr, 0);
			if (FAILED(hr)) { s->UnlockRect(); return hr; }
			copy_rows(sl.pBits, sl.Pitch, dl.pBits, dl.Pitch, row_count(l, static_cast<uint32_t>(h)), row_bytes(l, static_cast<uint32_t>(w)));
			d->UnlockRect();
			s->UnlockRect();
			return D3D_OK;
		}

		HRESULT copy_volume(IDirect3DVolume9* s, IDirect3DVolume9* d)
		{
			D3DVOLUME_DESC sd = {}, dd = {};
			if (FAILED(s->GetDesc(&sd)) || FAILED(d->GetDesc(&dd))) return D3DERR_INVALIDCALL;
			if (sd.Format != dd.Format || sd.Width != dd.Width || sd.Height != dd.Height || sd.Depth != dd.Depth)
				return D3DERR_INVALIDCALL;
			Layout l;
			if (!layout_of(sd.Format, l)) return D3DERR_INVALIDCALL;

			D3DLOCKED_BOX sb = {}, db = {};
			HRESULT hr = s->LockBox(&sb, nullptr, D3DLOCK_READONLY);
			if (FAILED(hr)) return hr;
			hr = d->LockBox(&db, nullptr, 0);
			if (FAILED(hr)) { s->UnlockBox(); return hr; }
			for (UINT z = 0; z < sd.Depth; z++)
			{
				copy_rows(static_cast<const uint8_t*>(sb.pBits) + static_cast<size_t>(z) * sb.SlicePitch, sb.RowPitch,
					static_cast<uint8_t*>(db.pBits) + static_cast<size_t>(z) * db.SlicePitch, db.RowPitch,
					row_count(l, sd.Height), row_bytes(l, sd.Width));
			}
			d->UnlockBox();
			s->UnlockBox();
			return D3D_OK;
		}

		// The pool and usage of a texture's top level.
		bool top_desc(IDirect3DBaseTexture9* t, D3DPOOL& pool, DWORD& usage)
		{
			switch (t->GetType())
			{
			case D3DRTYPE_TEXTURE:
			{
				D3DSURFACE_DESC d = {};
				if (FAILED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d))) return false;
				pool = d.Pool; usage = d.Usage; return true;
			}
			case D3DRTYPE_CUBETEXTURE:
			{
				D3DSURFACE_DESC d = {};
				if (FAILED(static_cast<IDirect3DCubeTexture9*>(t)->GetLevelDesc(0, &d))) return false;
				pool = d.Pool; usage = d.Usage; return true;
			}
			case D3DRTYPE_VOLUMETEXTURE:
			{
				D3DVOLUME_DESC d = {};
				if (FAILED(static_cast<IDirect3DVolumeTexture9*>(t)->GetLevelDesc(0, &d))) return false;
				pool = d.Pool; usage = d.Usage; return true;
			}
			default:
				return false;
			}
		}

		HRESULT copy_level(IDirect3DBaseTexture9* src, UINT src_level, IDirect3DBaseTexture9* dst, UINT dst_level)
		{
			switch (dst->GetType())
			{
			case D3DRTYPE_TEXTURE:
			{
				IDirect3DSurface9* s = nullptr;
				IDirect3DSurface9* d = nullptr;
				HRESULT hr = static_cast<IDirect3DTexture9*>(src)->GetSurfaceLevel(src_level, &s);
				if (SUCCEEDED(hr)) hr = static_cast<IDirect3DTexture9*>(dst)->GetSurfaceLevel(dst_level, &d);
				if (SUCCEEDED(hr)) hr = copy_surface(s, nullptr, d, nullptr, true);
				if (d) d->Release();
				if (s) s->Release();
				return hr;
			}
			case D3DRTYPE_CUBETEXTURE:
			{
				for (UINT face = 0; face < 6; face++)
				{
					IDirect3DSurface9* s = nullptr;
					IDirect3DSurface9* d = nullptr;
					const auto f = static_cast<D3DCUBEMAP_FACES>(face);
					HRESULT hr = static_cast<IDirect3DCubeTexture9*>(src)->GetCubeMapSurface(f, src_level, &s);
					if (SUCCEEDED(hr)) hr = static_cast<IDirect3DCubeTexture9*>(dst)->GetCubeMapSurface(f, dst_level, &d);
					if (SUCCEEDED(hr)) hr = copy_surface(s, nullptr, d, nullptr, true);
					if (d) d->Release();
					if (s) s->Release();
					if (FAILED(hr)) return hr;
				}
				return D3D_OK;
			}
			case D3DRTYPE_VOLUMETEXTURE:
			{
				IDirect3DVolume9* s = nullptr;
				IDirect3DVolume9* d = nullptr;
				HRESULT hr = static_cast<IDirect3DVolumeTexture9*>(src)->GetVolumeLevel(src_level, &s);
				if (SUCCEEDED(hr)) hr = static_cast<IDirect3DVolumeTexture9*>(dst)->GetVolumeLevel(dst_level, &d);
				if (SUCCEEDED(hr)) hr = copy_volume(s, d);
				if (d) d->Release();
				if (s) s->Release();
				return hr;
			}
			default:
				return D3DERR_INVALIDCALL;
			}
		}

		// NVTF's own settings, when its ini is in the usual place.
		std::string nvtf_note()
		{
			const std::filesystem::path ini = std::filesystem::path(shared::globals::root_path) / "Data" / "NVSE" / "Plugins" / "nvtf.ini";
			std::error_code ec;
			if (!std::filesystem::exists(ini, ec)) return {};
			const std::string p = ini.string();
			const int dx = static_cast<int>(GetPrivateProfileIntA("Main", "bModifyDirectXBehavior", -1, p.c_str()));
			const int pool = static_cast<int>(GetPrivateProfileIntA("DirectX", "bUseDefaultPoolForTextures", -1, p.c_str()));
			auto v = [](int x) { return x < 0 ? std::string("unset") : std::to_string(x); };
			return std::format(" | nvtf.ini: bModifyDirectXBehavior={} bUseDefaultPoolForTextures={}", v(dx), v(pool));
		}
	}

	bool plain_default(D3DPOOL pool, DWORD usage, const HANDLE* shared_handle, bool ex_device)
	{
		// Only plain sampled textures: any usage flag but AUTOGENMIPMAP (render
		// target, depth stencil, dynamic, displacement map) keeps the pool.
		return pool == D3DPOOL_DEFAULT && !ex_device && !shared_handle &&
			!(usage & ~static_cast<DWORD>(D3DUSAGE_AUTOGENMIPMAP));
	}

	D3DPOOL texture_pool(D3DRESOURCETYPE type, D3DPOOL pool, DWORD usage, D3DFORMAT format,
		const HANDLE* shared_handle, bool ex_device)
	{
		if (!plain_default(pool, usage, shared_handle, ex_device)) return pool;
		if (!keep_default())
		{
			g_by_switch.fetch_add(1, std::memory_order_relaxed);
			return D3DPOOL_MANAGED;
		}
		if (type != D3DRTYPE_TEXTURE) return pool;
		// FaceGen face maps and hair LayerMaps (every hair texture with
		// ShaderBakeHair): the skin tint, the hair layer weights and the hair
		// bake read them (api_material.cpp facegen_layer, hair_layer_textures,
		// api_hair_bake.cpp), so they keep a CPU copy.
		if (loading_facegen_map())
		{
			g_by_facegen.fetch_add(1, std::memory_order_relaxed);
			return D3DPOOL_MANAGED;
		}
		if (usage & D3DUSAGE_AUTOGENMIPMAP)
		{
			g_by_autogen.fetch_add(1, std::memory_order_relaxed);
			return D3DPOOL_MANAGED;
		}
		if (!api::textures::importable(format))
		{
			g_by_format.fetch_add(1, std::memory_order_relaxed);
			return D3DPOOL_MANAGED;
		}
		if (!api::textures::import_available() || api::textures::capture_failed())
		{
			g_by_import.fetch_add(1, std::memory_order_relaxed);
			return D3DPOOL_MANAGED;
		}
		return pool;
	}

	void note_created(D3DPOOL created, uint64_t bytes)
	{
		if (created == D3DPOOL_MANAGED)
		{
			g_promoted.fetch_add(1, std::memory_order_relaxed);
			g_promoted_bytes.fetch_add(bytes, std::memory_order_relaxed);
		}
		else
		{
			g_kept.fetch_add(1, std::memory_order_relaxed);
			g_kept_bytes.fetch_add(bytes, std::memory_order_relaxed);
		}
	}

	uint64_t chain_bytes(D3DRESOURCETYPE type, UINT width, UINT height, UINT depth, UINT levels, D3DFORMAT format)
	{
		Layout l;
		if (!layout_of(format, l) || !width || !height) return 0;
		if (type != D3DRTYPE_VOLUMETEXTURE) depth = 1;
		uint64_t total = 0;
		UINT w = width, h = height, d = (std::max)(1u, depth);
		for (UINT i = 0; levels == 0 || i < levels; i++)
		{
			total += static_cast<uint64_t>(row_bytes(l, w)) * row_count(l, h) * d;
			if (w == 1 && h == 1 && d == 1) break;
			w = (std::max)(1u, w / 2); h = (std::max)(1u, h / 2); d = (std::max)(1u, d / 2);
		}
		return type == D3DRTYPE_CUBETEXTURE ? total * 6 : total;
	}

	uint64_t promoted_count() { return g_promoted.load(std::memory_order_relaxed); }

	Stats stats()
	{
		Stats s;
		s.kept = g_kept.load(std::memory_order_relaxed);
		s.kept_bytes = g_kept_bytes.load(std::memory_order_relaxed);
		s.promoted = g_promoted.load(std::memory_order_relaxed);
		s.promoted_bytes = g_promoted_bytes.load(std::memory_order_relaxed);
		s.by_switch = g_by_switch.load(std::memory_order_relaxed);
		s.by_format = g_by_format.load(std::memory_order_relaxed);
		s.by_autogen = g_by_autogen.load(std::memory_order_relaxed);
		s.by_import = g_by_import.load(std::memory_order_relaxed);
		s.by_facegen = g_by_facegen.load(std::memory_order_relaxed);
		return s;
	}

	void install_hook()
	{
		if (!keep_default()) return;
		if (!fnv_exe::known())
		{
			shared::common::log("Textures", "FaceGen face maps: hook SKIPPED (unknown executable) -- with NVTF's "
				"bUseDefaultPoolForTextures their skin tint is lost", shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		g_tls_facegen = TlsAlloc();
		if (g_tls_facegen == TLS_OUT_OF_INDEXES) return;
		const fnv_exe::Code& fn = fnv_exe::addr().texture_from_file;
		if (!call_chain::hook_entry(fn, reinterpret_cast<void*>(&texture_from_file_hk),
			reinterpret_cast<void**>(&g_orig_texture_from_file), "Textures", "NiDX9SourceTextureData D3DX file load",
			"FaceGen face maps are told apart only if that code still calls the engine's texture file load"))
		{
			shared::common::log("Textures", "FaceGen face maps: the texture file load is not hooked -- with NVTF's "
				"bUseDefaultPoolForTextures their skin tint is lost", shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		g_facegen_hooked = true;
		shared::common::log("Textures", std::format("FaceGen face maps: hooked the texture file load @0x{:X}; they are "
			"created MANAGED", fn.addr));

		const fnv_exe::Code& gen = fnv_exe::addr().facegen_image_texture;
		if (!call_chain::hook_entry(gen, reinterpret_cast<void*>(&facegen_image_texture_hk),
			reinterpret_cast<void**>(&g_orig_facegen_image_texture), "Textures", "BSFaceGenImage texture build",
			"face maps built from EGT files are told apart only if that code still builds them there"))
		{
			shared::common::log("Textures", "FaceGen face maps built from EGT files: the build is not hooked -- with "
				"NVTF's bUseDefaultPoolForTextures their skin tint is lost", shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		shared::common::log("Textures", std::format("FaceGen face maps built from EGT files: hooked @0x{:X}; they are "
			"created MANAGED", gen.addr));
	}

	void after_update_texture(IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst, HRESULT hr)
	{
		if (!src || !dst || !keep_default() || dst->GetType() != D3DRTYPE_TEXTURE || src->GetType() != D3DRTYPE_TEXTURE)
			return;
		auto* d = static_cast<IDirect3DTexture9*>(dst);
		if (FAILED(hr))
		{
			api::textures::forget_default(d);
			return;
		}
		api::textures::capture_default(static_cast<IDirect3DTexture9*>(src), d);
	}

	void after_update_surface(IDirect3DSurface9* dst)
	{
		if (!dst || !keep_default()) return;
		D3DSURFACE_DESC dd = {};
		if (FAILED(dst->GetDesc(&dd)) || dd.Pool != D3DPOOL_DEFAULT) return;
		IDirect3DTexture9* t = nullptr;
		if (FAILED(dst->GetContainer(IID_IDirect3DTexture9, reinterpret_cast<void**>(&t))) || !t) return;
		api::textures::forget_default(t);
		t->Release();
	}

	void on_present()
	{
		if (g_probed || !keep_default()) return;
		g_probed = true;
		api::textures::resolve_import();
	}

	bool bridge_memory(BridgeMemory& out)
	{
		// The bridge client's export (bridge/src/client/d3d9_lss.cpp).
		struct ClientMemoryStats
		{
			uint32_t size;
			uint32_t reserved;
			uint64_t surfaceShadowBytes;
			uint64_t surfaceShadows;
			uint64_t bufferShadowBytes;
			uint64_t bufferShadows;
		};
		using Fn = BOOL(__stdcall*)(ClientMemoryStats*);
		static std::atomic<Fn> s_fn{ nullptr };
		static std::atomic<bool> s_resolved{ false };
		Fn fn = s_fn.load(std::memory_order_acquire);
		if (!fn && !s_resolved.load(std::memory_order_acquire))
		{
			if (HMODULE m = shared::globals::d3d9_chain_module)
				fn = reinterpret_cast<Fn>(GetProcAddress(m, "BridgeClient_GetMemoryStats"));
			s_fn.store(fn, std::memory_order_release);
			s_resolved.store(true, std::memory_order_release);
		}
		if (!fn) return false;
		ClientMemoryStats s = {};
		s.size = sizeof(s);
		if (!fn(&s)) return false;
		out.surface_bytes = s.surfaceShadowBytes;
		out.surfaces = s.surfaceShadows;
		out.buffer_bytes = s.bufferShadowBytes;
		out.buffers = s.bufferShadows;
		return true;
	}

	bool update_texture(IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst, HRESULT& hr)
	{
		if (!src || !dst || src->GetType() != dst->GetType()) return false;
		D3DPOOL src_pool, dst_pool;
		DWORD src_usage = 0, dst_usage = 0;
		if (!top_desc(src, src_pool, src_usage) || !top_desc(dst, dst_pool, dst_usage)) return false;
		if (src_pool != D3DPOOL_SYSTEMMEM || dst_pool != D3DPOOL_MANAGED) return false;

		// The destination's levels match the source's smallest ones.
		const DWORD src_levels = src->GetLevelCount();
		const DWORD dst_levels = dst->GetLevelCount();
		if (src_levels < dst_levels) { hr = D3DERR_INVALIDCALL; return true; }
		const DWORD skip = src_levels - dst_levels;
		hr = D3D_OK;
		for (DWORD i = 0; i < dst_levels && SUCCEEDED(hr); i++)
			hr = copy_level(src, skip + i, dst, i);
		if (SUCCEEDED(hr) && (dst_usage & D3DUSAGE_AUTOGENMIPMAP)) dst->GenerateMipSubLevels();
		return true;
	}

	bool update_surface(IDirect3DSurface9* src, const RECT* src_rect, IDirect3DSurface9* dst, const POINT* dst_point, HRESULT& hr)
	{
		if (!src || !dst) return false;
		D3DSURFACE_DESC sd = {}, dd = {};
		if (FAILED(src->GetDesc(&sd)) || FAILED(dst->GetDesc(&dd))) return false;
		if (sd.Pool != D3DPOOL_SYSTEMMEM || dd.Pool != D3DPOOL_MANAGED) return false;
		hr = copy_surface(src, src_rect, dst, dst_point, false);
		return true;
	}

	void log_once()
	{
		const Stats s = stats();
		if (!s.promoted && !s.kept) return;
		if (g_logged.exchange(true)) return;
		if (!keep_default())
		{
			shared::common::log_event("Textures", std::format(
				"{} textures requested in D3DPOOL_DEFAULT were created MANAGED ({:.0f} MiB of bridge-client CPU copies; "
				"[RemixApi] KeepDefaultPoolTextures=0){}",
				s.promoted, static_cast<double>(s.promoted_bytes) / (1024.0 * 1024.0), nvtf_note()));
			return;
		}
		shared::common::log_event("Textures", std::format(
			"{} textures requested in D3DPOOL_DEFAULT stay DEFAULT ({:.0f} MiB the bridge client keeps no CPU copy of) and "
			"reach Remix by import; {} were created MANAGED ({:.0f} MiB: FaceGen face maps {}, format {}, autogen mips {}, "
			"import unavailable or not yet asked {}){}",
			s.kept, static_cast<double>(s.kept_bytes) / (1024.0 * 1024.0), s.promoted,
			static_cast<double>(s.promoted_bytes) / (1024.0 * 1024.0), s.by_facegen, s.by_format, s.by_autogen, s.by_import,
			nvtf_note()));
	}
}
