/*
 * Headless real draw-command oracle for openttdrs renderer parity (#307).
 *
 * `viewport.cpp` owns the actual tile scope and invokes the regular OpenTTD
 * draw procs. This file only owns configuration, stable JSONL serialization,
 * and the parent/combine bookkeeping needed to explain a divergent command.
 */

#include "world_draw_export.h"

#include "map_func.h"
#include "openttd.h"
#include "saveload/saveload.h"
#include "table/sprites.h"
#include "tile_map.h"
#include "timer/timer_game_tick.h"

#include "3rdparty/nlohmann/json.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>

using nlohmann::json;

namespace {

struct WorldDrawRequestedBounds : OpenttdrsWorldDrawBounds {
	bool filtered = false;
	uint32_t requested_min_x = 0;
	uint32_t requested_min_y = 0;
	uint32_t requested_max_x = 0;
	uint32_t requested_max_y = 0;
};

struct WorldDrawState {
	bool active = false;
	bool failed = false;
	bool in_tile = false;
	uint32_t width = 0;
	WorldDrawRequestedBounds bounds;
	std::ofstream out;
	uint32_t tile_x = 0;
	uint32_t tile_y = 0;
	uint64_t ordinal = 0;
	uint64_t tile_count = 0;
	uint64_t draw_count = 0;
	uint64_t combine_group = 0;
	std::optional<uint64_t> last_parent;
	std::optional<uint64_t> combine_parent;
};

WorldDrawState _openttdrs_world_draw;

bool ParseCoordinate(const char *&cursor, uint32_t &value)
{
	errno = 0;
	char *end = nullptr;
	const unsigned long long parsed = std::strtoull(cursor, &end, 10);
	if (end == cursor || errno == ERANGE || parsed > std::numeric_limits<uint32_t>::max()) return false;
	value = static_cast<uint32_t>(parsed);
	cursor = end;
	return true;
}

bool ParseBounds(uint width, uint height, WorldDrawRequestedBounds &bounds)
{
	bounds.end_x = width;
	bounds.end_y = height;
	const char *raw = std::getenv("OPENTTDRS_WORLD_DRAW_REGION");
	if (raw == nullptr || raw[0] == '\0') return true;

	bounds.filtered = true;
	const char *cursor = raw;
	if (!ParseCoordinate(cursor, bounds.requested_min_x) || *cursor++ != ',' ||
			!ParseCoordinate(cursor, bounds.requested_min_y) || *cursor++ != ',' ||
			!ParseCoordinate(cursor, bounds.requested_max_x) || *cursor++ != ',' ||
			!ParseCoordinate(cursor, bounds.requested_max_y) || *cursor != '\0' ||
			bounds.requested_min_x > bounds.requested_max_x ||
			bounds.requested_min_y > bounds.requested_max_y) {
		std::fprintf(stderr, "openttdrs world-draw: región inválida %s (usar x0,y0,x1,y1)\n", raw);
		return false;
	}

	bounds.begin_x = std::min<uint>(bounds.requested_min_x, width);
	bounds.begin_y = std::min<uint>(bounds.requested_min_y, height);
	bounds.end_x = bounds.requested_max_x >= width ? width : bounds.requested_max_x + 1;
	bounds.end_y = bounds.requested_max_y >= height ? height : bounds.requested_max_y + 1;
	return true;
}

int WorldDrawMinCall()
{
	const char *raw = std::getenv("OPENTTDRS_WORLD_DRAW_MIN_CALL");
	return raw != nullptr && raw[0] != '\0' ? std::atoi(raw) : 2;
}

json RegionJson(const WorldDrawRequestedBounds &bounds)
{
	if (!bounds.filtered) return nullptr;
	return {
		{"min_x", bounds.requested_min_x},
		{"min_y", bounds.requested_min_y},
		{"max_x", bounds.requested_max_x},
		{"max_y", bounds.requested_max_y},
	};
}

void Emit(json row)
{
	auto &state = _openttdrs_world_draw;
	if (!state.active || state.failed) return;
	state.out << row.dump() << '\n';
	if (!state.out) state.failed = true;
}

json SpriteJson(uint32_t image)
{
	return {
		{"source", "global"},
		{"id", image & SPRITE_MASK},
		{"raw_id", image},
	};
}

json BaseDrawRow(const char *primitive, const char *role)
{
	auto &state = _openttdrs_world_draw;
	const uint64_t ordinal = state.ordinal++;
	state.draw_count++;
	return {
		{"kind", "draw"},
		{"x", state.tile_x},
		{"y", state.tile_y},
		{"ordinal", ordinal},
		{"role", role},
		{"primitive", primitive},
		{"combine_group", state.combine_group == 0 ? json(nullptr) : json(state.combine_group)},
		{"fallback", false},
	};
}

uint64_t LastOrdinal() { return _openttdrs_world_draw.ordinal - 1; }

} // namespace

bool OpenttdrsMaybeStartWorldDraw(const std::string &source_path)
{
	const char *out_path = std::getenv("OPENTTDRS_WORLD_DRAW_OUT");
	if (out_path == nullptr || out_path[0] == '\0') return true;

	static int call_count = 0;
	call_count++;
	if (call_count < WorldDrawMinCall()) return true;

	auto &state = _openttdrs_world_draw;
	if (state.active) return true;
	state = {};
	state.width = Map::SizeX();
	if (!ParseBounds(state.width, Map::SizeY(), state.bounds)) return false;
	state.out.open(out_path, std::ios::out | std::ios::trunc);
	if (!state.out) return false;
	state.active = true;

	const char *commit = std::getenv("OPENTTDRS_OPENTTD_COMMIT");
	const char *raw_source = std::getenv("OPENTTDRS_WORLD_DRAW_SOURCE");
	const char *save_hash = std::getenv("OPENTTDRS_WORLD_DRAW_SAVE_SHA256");
	extern SaveLoadVersion _sl_version;
	json metadata;
	metadata["kind"] = "metadata";
	metadata["schema_version"] = 1;
	metadata["contract"] = "world-draw";
	metadata["producer"] = "openttd";
	metadata["stage"] = "headless_tile_draw_proc";
	metadata["tick"] = static_cast<uint64_t>(TimerGameTick::counter);
	metadata["climate"] = static_cast<uint8_t>(_settings_game.game_creation.landscape);
	metadata["openttd_commit"] = commit != nullptr ? commit : "";
	metadata["source_path"] = raw_source != nullptr && raw_source[0] != '\0' ? raw_source : source_path;
	metadata["save_sha256"] = save_hash != nullptr ? save_hash : "";
	metadata["save_version"] = static_cast<uint16_t>(_sl_version);
	metadata["width"] = Map::SizeX();
	metadata["height"] = Map::SizeY();
	metadata["region"] = RegionJson(state.bounds);
	metadata["clipping"] = "disabled";
	metadata["includes"] = {"ground", "sortable", "child", "combine"};
	Emit(std::move(metadata));
	/* Dejar una cabecera diagnosticable incluso si un draw proc posterior falla. */
	state.out.flush();
	if (!state.out) state.failed = true;
	return !state.failed;
}

bool OpenttdrsWorldDrawCaptureActive()
{
	return _openttdrs_world_draw.active && !_openttdrs_world_draw.failed;
}

bool OpenttdrsWorldDrawCaptureBounds(OpenttdrsWorldDrawBounds &bounds)
{
	if (!OpenttdrsWorldDrawCaptureActive()) return false;
	bounds = _openttdrs_world_draw.bounds;
	return true;
}

void OpenttdrsWorldDrawBeginTile(
	uint32_t x,
	uint32_t y,
	uint8_t tile_type,
	uint8_t tileh,
	uint32_t base_z,
	uint8_t foundation_tileh,
	uint32_t foundation_base_z
)
{
	auto &state = _openttdrs_world_draw;
	if (!OpenttdrsWorldDrawCaptureActive()) return;
	state.in_tile = true;
	state.tile_x = x;
	state.tile_y = y;
	state.ordinal = 0;
	state.tile_count++;
	state.combine_group = 0;
	state.last_parent.reset();
	state.combine_parent.reset();
	Emit({
		{"kind", "tile"},
		{"index", static_cast<uint64_t>(y) * state.width + x},
		{"x", x},
		{"y", y},
		{"tile_type", tile_type},
		{"tileh", tileh},
		{"base_z", base_z},
		{"foundation_tileh", foundation_tileh},
		{"foundation_base_z", foundation_base_z},
	});
}

void OpenttdrsWorldDrawEndTile()
{
	if (!OpenttdrsWorldDrawCaptureActive()) return;
	_openttdrs_world_draw.in_tile = false;
}

void OpenttdrsWorldDrawRecordFoundation(
	uint8_t foundation,
	uint8_t foundation_tileh,
	uint32_t foundation_base_z,
	uint8_t sprite_block,
	bool has_nw,
	bool has_ne,
	int32_t nw_w_here,
	int32_t nw_n_here,
	int32_t nw_w_neighbour,
	int32_t nw_n_neighbour,
	int32_t ne_e_here,
	int32_t ne_n_here,
	int32_t ne_e_neighbour,
	int32_t ne_n_neighbour
)
{
	auto &state = _openttdrs_world_draw;
	if (!OpenttdrsWorldDrawCaptureActive() || !state.in_tile) return;
	Emit({
		{"kind", "foundation"},
		{"x", state.tile_x},
		{"y", state.tile_y},
		{"foundation", foundation},
		{"foundation_tileh", foundation_tileh},
		{"foundation_base_z", foundation_base_z},
		{"sprite_block", sprite_block},
		{"has_nw", has_nw},
		{"has_ne", has_ne},
		{"nw_w_here", nw_w_here},
		{"nw_n_here", nw_n_here},
		{"nw_w_neighbour", nw_w_neighbour},
		{"nw_n_neighbour", nw_n_neighbour},
		{"ne_e_here", ne_e_here},
		{"ne_n_here", ne_n_here},
		{"ne_e_neighbour", ne_e_neighbour},
		{"ne_n_neighbour", ne_n_neighbour},
	});
}

void OpenttdrsWorldDrawRecordTileSprite(uint32_t image, uint32_t palette, int32_t x, int32_t y, int32_t z)
{
	auto &state = _openttdrs_world_draw;
	if (!OpenttdrsWorldDrawCaptureActive() || !state.in_tile) return;
	json row = BaseDrawRow("ground", "ground");
	row["sprite"] = SpriteJson(image);
	row["palette"] = palette;
	row["world"] = {{"x", x}, {"y", y}, {"z", z}};
	row["bounds"] = nullptr;
	row["offset"] = {{"x", 0}, {"y", 0}, {"z", 0}};
	row["parent_ordinal"] = nullptr;
	row["transparent"] = false;
	Emit(std::move(row));
}

void OpenttdrsWorldDrawRecordSortable(
	uint32_t image,
	uint32_t palette,
	int32_t x,
	int32_t y,
	int32_t z,
	int32_t origin_x,
	int32_t origin_y,
	int32_t origin_z,
	int32_t extent_x,
	int32_t extent_y,
	int32_t extent_z,
	int32_t offset_x,
	int32_t offset_y,
	int32_t offset_z,
	bool transparent,
	uint8_t combine_mode
)
{
	auto &state = _openttdrs_world_draw;
	if (!OpenttdrsWorldDrawCaptureActive() || !state.in_tile) return;
	const bool combined = combine_mode == 2;
	const bool starts_combine = combine_mode == 1;
	const bool empty_bounds = (image & SPRITE_MASK) == SPR_EMPTY_BOUNDING_BOX;
	/* El padre que abre un bloque también pertenece a ese bloque. */
	if (starts_combine) state.combine_group++;
	json row = BaseDrawRow(
		empty_bounds ? "empty_bounds" : (combined ? "combined" : "sortable"),
		empty_bounds ? "empty_bounds" : (combined ? "overlay" : "sortable")
	);
	const uint64_t ordinal = LastOrdinal();
	if (combined) {
		row["parent_ordinal"] = state.combine_parent ? json(*state.combine_parent) : json(nullptr);
	} else {
		row["parent_ordinal"] = nullptr;
		state.last_parent = ordinal;
		if (starts_combine) {
			state.combine_parent = ordinal;
		}
	}
	row["sprite"] = SpriteJson(image);
	row["palette"] = palette;
	row["resolved_palette"] = transparent ? PALETTE_TO_TRANSPARENT : palette;
	row["world"] = {{"x", x}, {"y", y}, {"z", z}};
	row["bounds"] = {
		{"ox", origin_x}, {"oy", origin_y}, {"oz", origin_z},
		{"ex", extent_x}, {"ey", extent_y}, {"ez", extent_z},
	};
	row["offset"] = {{"x", offset_x}, {"y", offset_y}, {"z", offset_z}};
	row["transparent"] = transparent;
	Emit(std::move(row));
}

void OpenttdrsWorldDrawRecordChild(
	uint32_t image,
	uint32_t palette,
	int32_t x,
	int32_t y,
	bool transparent,
	bool scale,
	bool relative
)
{
	auto &state = _openttdrs_world_draw;
	if (!OpenttdrsWorldDrawCaptureActive() || !state.in_tile) return;
	json row = BaseDrawRow("child", "child");
	row["sprite"] = SpriteJson(image);
	row["palette"] = palette;
	row["resolved_palette"] = transparent ? PALETTE_TO_TRANSPARENT : palette;
	row["world"] = nullptr;
	row["bounds"] = nullptr;
	row["offset"] = {{"x", x}, {"y", y}, {"z", 0}};
	row["parent_ordinal"] = state.combine_parent ? json(*state.combine_parent) : state.last_parent ? json(*state.last_parent) : json(nullptr);
	row["transparent"] = transparent;
	row["scale"] = scale;
	row["relative"] = relative;
	Emit(std::move(row));
}

void OpenttdrsWorldDrawRecordCombineStart()
{
	auto &state = _openttdrs_world_draw;
	if (!OpenttdrsWorldDrawCaptureActive() || !state.in_tile) return;
	json row = BaseDrawRow("combine_start", "combine");
	row["sprite"] = nullptr;
	row["palette"] = nullptr;
	row["world"] = nullptr;
	row["bounds"] = nullptr;
	row["offset"] = {{"x", 0}, {"y", 0}, {"z", 0}};
	row["parent_ordinal"] = nullptr;
	row["transparent"] = false;
	Emit(std::move(row));
}

void OpenttdrsWorldDrawRecordCombineEnd()
{
	auto &state = _openttdrs_world_draw;
	if (!OpenttdrsWorldDrawCaptureActive() || !state.in_tile) return;
	json row = BaseDrawRow("combine_end", "combine");
	row["sprite"] = nullptr;
	row["palette"] = nullptr;
	row["world"] = nullptr;
	row["bounds"] = nullptr;
	row["offset"] = {{"x", 0}, {"y", 0}, {"z", 0}};
	row["parent_ordinal"] = state.combine_parent ? json(*state.combine_parent) : json(nullptr);
	row["transparent"] = false;
	Emit(std::move(row));
	state.combine_parent.reset();
}

bool OpenttdrsFinishWorldDraw()
{
	auto &state = _openttdrs_world_draw;
	if (!state.active) return true;
	if (!state.failed) {
		Emit({
			{"kind", "complete"},
			{"tiles", state.tile_count},
			{"draws", state.draw_count},
		});
	}
	state.out.flush();
	const bool ok = !state.failed && static_cast<bool>(state.out);
	state.out.close();
	state.active = false;
	_exit_game = true;
	return ok;
}
