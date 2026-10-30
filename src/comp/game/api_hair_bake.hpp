#pragma once
#include "api_common.hpp"

// [RemixApi] ShaderBakeHair: the hair material's albedo baked from the game's
// own hair PS (shader_bake.hpp) instead of the LayerMap terrain layer
// (api_material.cpp hair_layer_textures).
//
// Threads:
//   * render thread: albedo() per hair draw. A new (PS, BaseMap, LayerMap,
//     material constants) pointer key reads a sampled hash of both textures
//     (two READONLY locks of the bridge client's CPU copy, 64 spots each);
//     a new content key also copies the PS bytecode, the draw's PS constants
//     and the one or two mip levels the bake reads of each texture (READONLY
//     locks and memcpy, no decode) into a job. At most one capture per frame
//     and kMaxPending bakes queued or waiting for their upload.
//   * one worker thread (THREAD_PRIORITY_LOWEST): decode, interpreter,
//     probes, mips and BC3 encode. It makes no D3D9 or bridge call and logs
//     nothing.
//   * render thread, on_present(): takes the finished bakes, logs them and
//     uploads at most one per frame (acquire_generated, while the frame's
//     upload budget is not spent).
// Until its bake is uploaded a draw gets 0 and keeps the LayerMap material.
// The baked texture's hash is in the material key (Desc::hair_bake), so the
// next draw after the upload builds a new material and mesh, and the old ones
// age out with their caches' eviction.
//
// Bakes are cached by content key (PS hash, both textures' shapes and sampled
// hashes, the PS constants the bake keeps): every NPC with the same hair
// shares one. The cache holds one reference on each uploaded bake and drops
// it TextureEvictFrames after the last draw that asked for it. A refused
// bake is logged once per (shader, reason) and its draws keep the LayerMap
// material.

namespace comp::game::api::hair_bake
{
	// The baked albedo of a hair draw: its PS (`psmap`, `ps`), BaseMap and
	// LayerMap. The API texture hash, or 0 (option off, not baked yet,
	// refused). Render thread.
	uint64_t albedo(const shared::common::PsSlotMap* psmap, IDirect3DPixelShader9* ps, IDirect3DBaseTexture9* base,
		IDirect3DBaseTexture9* layer);

	// Once per frame, before api::textures::on_present: finished bakes, at
	// most one upload, eviction.
	void on_present();

	// The status line, empty while no bake was ever asked for.
	std::string status();
}
