/*
 * Lightweight per-tile oracle for openttdrs SAV rendering parity (#305).
 * Kept separate from snapshot_export.cpp so it can compile against newer
 * OpenTTD APIs without porting the unrelated PBS/FTA fixture machinery.
 */

#include "world_raw_export.h"

#include "map_func.h"
#include "openttd.h"
#include "saveload/saveload.h"
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

namespace {

struct WorldRawBounds {
	bool filtered = false;
	uint32_t requested_min_x = 0;
	uint32_t requested_min_y = 0;
	uint32_t requested_max_x = 0;
	uint32_t requested_max_y = 0;
	uint begin_x = 0;
	uint begin_y = 0;
	uint end_x = 0; /* exclusive */
	uint end_y = 0; /* exclusive */
};

bool ParseWorldRawCoordinate(const char *&cursor, uint32_t &value)
{
	errno = 0;
	char *end = nullptr;
	const unsigned long long parsed = std::strtoull(cursor, &end, 10);
	if (end == cursor || errno == ERANGE || parsed > std::numeric_limits<uint32_t>::max()) return false;
	value = static_cast<uint32_t>(parsed);
	cursor = end;
	return true;
}

bool ParseWorldRawBounds(uint width, uint height, WorldRawBounds &bounds)
{
	bounds.end_x = width;
	bounds.end_y = height;
	const char *raw = std::getenv("OPENTTDRS_WORLD_RAW_REGION");
	if (raw == nullptr || raw[0] == '\0') return true;

	bounds.filtered = true;
	const char *cursor = raw;
	if (!ParseWorldRawCoordinate(cursor, bounds.requested_min_x) || *cursor++ != ',' ||
			!ParseWorldRawCoordinate(cursor, bounds.requested_min_y) || *cursor++ != ',' ||
			!ParseWorldRawCoordinate(cursor, bounds.requested_max_x) || *cursor++ != ',' ||
			!ParseWorldRawCoordinate(cursor, bounds.requested_max_y) || *cursor != '\0' ||
			bounds.requested_min_x > bounds.requested_max_x ||
			bounds.requested_min_y > bounds.requested_max_y) {
		std::fprintf(stderr, "openttdrs world-raw: región inválida %s (usar x0,y0,x1,y1)\n", raw);
		return false;
	}

	bounds.begin_x = std::min<uint>(bounds.requested_min_x, width);
	bounds.begin_y = std::min<uint>(bounds.requested_min_y, height);
	bounds.end_x = bounds.requested_max_x >= width ? width : bounds.requested_max_x + 1;
	bounds.end_y = bounds.requested_max_y >= height ? height : bounds.requested_max_y + 1;
	return true;
}

uint64_t WorldRawTileCount(const WorldRawBounds &bounds)
{
	return static_cast<uint64_t>(bounds.end_x - bounds.begin_x) *
		static_cast<uint64_t>(bounds.end_y - bounds.begin_y);
}

nlohmann::json WorldRawRegionJson(const WorldRawBounds &bounds)
{
	if (!bounds.filtered) return nullptr;
	return {
		{"min_x", bounds.requested_min_x},
		{"min_y", bounds.requested_min_y},
		{"max_x", bounds.requested_max_x},
		{"max_y", bounds.requested_max_y},
	};
}

int WorldRawMinCall()
{
	const char *raw = std::getenv("OPENTTDRS_WORLD_RAW_MIN_CALL");
	if (raw == nullptr || raw[0] == '\0') raw = std::getenv("OPENTTDRS_SNAPSHOT_MIN_CALL");
	return raw != nullptr && raw[0] != '\0' ? std::atoi(raw) : 2;
}

} // namespace

bool OpenttdrsMaybeExportWorldRaw(const std::string &source_path)
{
	const char *out_path = std::getenv("OPENTTDRS_WORLD_RAW_OUT");
	if (out_path == nullptr || out_path[0] == '\0') return true;

	/* Dedicated + -g loads a new game before the requested save. */
	static int call_count = 0;
	call_count++;
	if (call_count < WorldRawMinCall()) return true;

	const uint width = Map::SizeX();
	const uint height = Map::SizeY();
	WorldRawBounds bounds;
	if (!ParseWorldRawBounds(width, height, bounds)) return false;

	std::ofstream out(out_path, std::ios::out | std::ios::trunc);
	if (!out) return false;

	const char *commit = std::getenv("OPENTTDRS_OPENTTD_COMMIT");
	const char *raw_source = std::getenv("OPENTTDRS_WORLD_RAW_SOURCE");
	const char *save_hash = std::getenv("OPENTTDRS_WORLD_RAW_SAVE_SHA256");
	extern SaveLoadVersion _sl_version;
	nlohmann::json metadata;
	metadata["kind"] = "metadata";
	metadata["schema_version"] = 2;
	metadata["contract"] = "world-raw";
	metadata["producer"] = "openttd";
	metadata["stage"] = "after_load_game";
	metadata["tick"] = static_cast<uint64_t>(TimerGameTick::counter);
	metadata["climate"] = static_cast<uint8_t>(_settings_game.game_creation.landscape);
	metadata["openttd_commit"] = commit != nullptr ? commit : "";
	metadata["source_path"] = raw_source != nullptr && raw_source[0] != '\0' ? raw_source : source_path;
	metadata["save_sha256"] = save_hash != nullptr ? save_hash : "";
	metadata["save_version"] = static_cast<uint16_t>(_sl_version);
	metadata["width"] = width;
	metadata["height"] = height;
	metadata["tile_count"] = static_cast<uint64_t>(width) * height;
	metadata["emitted_tile_count"] = WorldRawTileCount(bounds);
	metadata["region"] = WorldRawRegionJson(bounds);
	out << metadata.dump() << '\n';
	if (!out) return false;

	for (uint y = bounds.begin_y; y < bounds.end_y; y++) {
		for (uint x = bounds.begin_x; x < bounds.end_x; x++) {
			Tile tile(TileXY(x, y));
			nlohmann::json row;
			row["kind"] = "tile_raw";
			row["index"] = static_cast<uint64_t>(y) * width + x;
			row["x"] = x;
			row["y"] = y;
			row["height"] = static_cast<uint8_t>(TileHeight(tile));
			row["type"] = tile.type();
			row["m1"] = tile.m1();
			row["m2"] = tile.m2();
			row["m3"] = tile.m3();
			row["m4"] = tile.m4();
			row["m5"] = tile.m5();
			row["m6"] = tile.m6();
			row["m7"] = tile.m7();
			row["m8"] = tile.m8();
			out << row.dump() << '\n';
			if (!out) return false;
		}
	}
	out.flush();
	if (!out) return false;

	_exit_game = true;
	return true;
}
