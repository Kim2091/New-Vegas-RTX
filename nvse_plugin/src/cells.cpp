#include "common.hpp"
#include "cells.hpp"
#include "camera.hpp"
#include "config.hpp"
#include "queue.hpp"
#include "owners.hpp"
#include "nvse_abi.h"

namespace fnvr::cells
{
	namespace
	{
		struct Cell
		{
			uintptr_t cell = 0;
			uintptr_t world = 0;              // the interior cell itself, or its worldspace
			int32_t x = 0, y = 0;
			bool interior = false;
			bool refs_loaded = false;
			uint32_t by = 0;                  // FnvRemixRefsLoadedBy
			ULONGLONG attach_ms = 0;
			uint32_t zero_frames = 0;         // main-loop frames with both queue counters at zero
		};

		std::vector<Cell> g_cells;
		uint32_t g_serial = 0;
		uintptr_t g_last_world = 0;           // world of the last cell that attached
		uint32_t g_last_resync = 0;
		uint32_t g_by_count[4] = {};          // refs-loaded verdicts by FnvRemixRefsLoadedBy, since start
		uint32_t g_attach_logs = 0;

		Cell* find(uintptr_t cell)
		{
			for (auto& c : g_cells)
				if (c.cell == cell) return &c;
			return nullptr;
		}

		// The cell's identity as the engine has it now. False when unreadable.
		bool describe(uintptr_t cell, Cell& out)
		{
			uint8_t type = 0;
			if (!guard::plausible(cell) || !guard::get(cell + engine::FORM_type, type) || type != engine::kFormType_CELL)
				return false;
			uint32_t ws = 0;
			if (!guard::get(cell + engine::CELL_worldSpace, ws)) return false;
			out.cell = cell;
			out.interior = ws == 0;
			out.world = out.interior ? cell : ws;
			out.x = out.y = 0;
			if (!out.interior)
			{
				uint32_t coords = 0;
				int32_t xy[2] = {};
				if (guard::get(cell + engine::CELL_coords, coords) && coords && guard::read(coords, xy, sizeof(xy)))
				{
					out.x = xy[0];
					out.y = xy[1];
				}
			}
			return true;
		}

		std::string name(const Cell& c)
		{
			return c.interior ? std::format("interior 0x{:X}", c.cell) : std::format("0x{:X} ({}, {})", c.cell, c.x, c.y);
		}

		void mark_loaded(Cell& c, uint32_t by)
		{
			if (c.refs_loaded) return;
			c.refs_loaded = true;
			c.by = by;
			g_by_count[by]++;
			// The wrapper hears of the loaded references with their owners known.
			owners::adopt_cell(c.cell, by == FNVR_RL_EVENT ? "refs loaded" : by == FNVR_RL_POLL ? "re-attach" : "timeout");
			queue::emit(FNVR_EV_CELL_REFS_LOADED, c.cell, by);
			const ULONGLONG ms = GetTickCount64() - c.attach_ms;
			if (by == FNVR_RL_TIMEOUT)
				log::f("CellMon", "cell {}: no refs-loaded event and references still queued after {} ms -- counted as "
					"loaded (timeout)", name(c), ms);
			else if (by == FNVR_RL_POLL)
				log::f("CellMon", "cell {}: references loaded after {} ms (queue counters at zero; no xNVSE event, "
					"a re-attached cell gets none)", name(c), ms);
		}

		void add(const Cell& info, const char* how)
		{
			Cell c = info;
			c.attach_ms = GetTickCount64();
			// A move into another interior or worldspace is a cut. A resync add
			// follows a cut (or a missed event) and requests none.
			if (std::strcmp(how, "(event)") == 0 && g_last_world && c.world != g_last_world)
				camera::request_cut(FNVR_CUT_WORLD_CHANGE, 0.0f);
			g_last_world = c.world;
			g_cells.push_back(c);
			g_serial++;
			queue::emit(FNVR_EV_CELL_ATTACHED, c.cell);
			owners::on_cell_attached(c.cell);
			// A cell found by a resync may have had its references for a while.
			if (std::strcmp(how, "(resync)") == 0) owners::adopt_cell(c.cell, "resync");
			if (g_attach_logs < 4000)
			{
				g_attach_logs++;
				log::f("CellMon", "attached {} {}, {} attached, serial {}", name(c), how, g_cells.size(), g_serial);
			}
		}

		void remove(uintptr_t cell, const char* how)
		{
			const auto it = std::find_if(g_cells.begin(), g_cells.end(), [cell](const Cell& c) { return c.cell == cell; });
			if (it == g_cells.end()) return;
			const std::string n = name(*it);
			g_cells.erase(it);
			g_serial++;
			queue::emit(FNVR_EV_CELL_DETACHED, cell);
			owners::on_cell_detached(cell);
			if (g_attach_logs < 4000)
			{
				g_attach_logs++;
				log::f("CellMon", "detached {} {}, {} attached, serial {}", n, how, g_cells.size(), g_serial);
			}
		}

		// The cells the engine has in residence: the grid and the interior.
		void engine_cells(std::vector<uintptr_t>& out)
		{
			out.clear();
			uint32_t tes = 0;
			if (!guard::get(fnv_exe::addr().tes, tes) || !tes) return;
			uint32_t grid = 0;
			if (guard::get(tes + engine::TES_gridCells, grid) && grid)
			{
				uint32_t dim = 0, cells = 0;
				if (guard::get(grid + engine::GCA_dim, dim) && guard::get(grid + engine::GCA_cells, cells) && cells &&
					dim && dim <= 16)
				{
					uint32_t arr[256];
					if (guard::read(cells, arr, dim * dim * sizeof(uint32_t)))
						for (uint32_t i = 0; i < dim * dim; i++)
							if (guard::plausible(arr[i])) out.push_back(arr[i]);
				}
			}
			uint32_t interior = 0;
			if (guard::get(tes + engine::TES_interior, interior) && guard::plausible(interior)) out.push_back(interior);
		}
	}

	void on_state(uintptr_t cell, uint32_t new_state, uint32_t prev_state)
	{
		using namespace nvse_abi;
		if (new_state == kCell_Attached)
		{
			if (find(cell)) return;
			Cell info;
			if (!describe(cell, info))
			{
				log::f("CellMon", "attach of 0x{:X}: not a readable cell, ignored", cell);
				return;
			}
			add(info, "(event)");
		}
		else if (prev_state == kCell_Attached)
			remove(cell, std::format("(event: state {} -> {})", prev_state, new_state).c_str());
	}

	void on_refs_loaded(uintptr_t cell)
	{
		if (Cell* c = find(cell)) mark_loaded(*c, FNVR_RL_EVENT);
	}

	void tick(uint32_t frame)
	{
		const auto& cfg = config::get();
		const ULONGLONG now = GetTickCount64();
		for (auto& c : g_cells)
		{
			if (c.refs_loaded) continue;
			uint32_t render = 0, critical = 1, queued = 1;
			const bool ok = guard::get(c.cell + engine::CELL_renderData, render) &&
				guard::get(c.cell + engine::CELL_criticalQueued, critical) && guard::get(c.cell + engine::CELL_queued, queued);
			if (ok && render && critical == 0 && queued == 0)
			{
				if (++c.zero_frames >= cfg.refs_loaded_poll_frames) mark_loaded(c, FNVR_RL_POLL);
			}
			else c.zero_frames = 0;
			if (!c.refs_loaded && now - c.attach_ms > cfg.refs_loaded_timeout_ms) mark_loaded(c, FNVR_RL_TIMEOUT);
		}
		if (frame - g_last_resync >= cfg.monitor_period)
		{
			g_last_resync = frame;
			resync("periodic");
		}
	}

	void resync(const char* why)
	{
		static std::vector<uintptr_t> s_engine;
		engine_cells(s_engine);
		// Tracked cells the engine no longer has attached.
		for (size_t i = 0; i < g_cells.size();)
		{
			uint8_t state = 0;
			if (guard::get(g_cells[i].cell + engine::CELL_state, state) && state == nvse_abi::kCell_Attached)
			{
				i++;
				continue;
			}
			log::f("CellMon", "resync ({}): cell 0x{:X} is not attached any more (state {}) and sent no event -- dropped",
				why, g_cells[i].cell, state);
			remove(g_cells[i].cell, "(resync)");
		}
		// Attached engine cells not tracked.
		for (const uintptr_t cell : s_engine)
		{
			if (find(cell)) continue;
			uint8_t state = 0;
			if (!guard::get(cell + engine::CELL_state, state) || state != nvse_abi::kCell_Attached) continue;
			Cell info;
			if (!describe(cell, info)) continue;
			log::f("CellMon", "resync ({}): cell {} is attached but sent no event -- added", why, name(info));
			add(info, "(resync)");
		}
	}

	bool all_refs_loaded()
	{
		for (const auto& c : g_cells)
			if (!c.refs_loaded) return false;
		return true;
	}

	void adopt_all(const char* why)
	{
		for (const auto& c : g_cells)
		{
			if (!c.refs_loaded) continue;
			owners::adopt_cell(c.cell, why);
			queue::emit(FNVR_EV_CELL_REFS_LOADED, c.cell, c.by);
		}
	}

	uint32_t serial() { return g_serial; }
	uint32_t count() { return static_cast<uint32_t>(g_cells.size()); }

	bool interior()
	{
		for (const auto& c : g_cells)
			if (c.interior) return true;
		return false;
	}

	bool grid(int32_t& cx, int32_t& cy, uint32_t& dim)
	{
		uint32_t tes = 0, grid = 0;
		if (!guard::get(fnv_exe::addr().tes, tes) || !tes || !guard::get(tes + engine::TES_gridCells, grid) || !grid) return false;
		int32_t x = 0, y = 0;
		uint32_t d = 0;
		if (!guard::get(grid + engine::GCA_centerX, x) || !guard::get(grid + engine::GCA_centerY, y) ||
			!guard::get(grid + engine::GCA_dim, d) || d > 16)
			return false;
		cx = x;
		cy = y;
		dim = d;
		return true;
	}

	uint32_t copy(FnvRemixCell* out, uint32_t max)
	{
		uint32_t n = 0;
		for (const auto& c : g_cells)
		{
			if (n >= max) break;
			FnvRemixCell& o = out[n++];
			o = {};
			o.cell = c.cell;
			uint32_t render = 0, root = 0;
			if (guard::get(c.cell + engine::CELL_renderData, render) && render && guard::get(render, root)) o.root = root;
			o.x = c.x;
			o.y = c.y;
			o.interior = c.interior ? 1 : 0;
			o.refs_loaded = c.refs_loaded ? 1 : 0;
		}
		return n;
	}

	uintptr_t cell_of_root(uintptr_t root)
	{
		if (!root) return 0;
		for (const auto& c : g_cells)
		{
			uint32_t render = 0, r = 0;
			if (guard::get(c.cell + engine::CELL_renderData, render) && render && guard::get(render, r) && r == root)
				return c.cell;
		}
		return 0;
	}

	bool attached(uintptr_t cell)
	{
		return find(cell) != nullptr;
	}
}
