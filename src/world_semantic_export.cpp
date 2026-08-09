/*
 * Per-tile semantic oracle for openttdrs SAV rendering parity (#306).
 *
 * The raw dump proves MAP* bytes. This export intentionally records the
 * interpretation OpenTTD gives those bytes, including the official other-end
 * lookup for each tunnel/bridge ramp.
 */

#include "world_semantic_export.h"

#include "bridge_map.h"
#include "map_func.h"
#include "openttd.h"
#include "saveload/saveload.h"
#include "tile_map.h"
#include "timer/timer_game_tick.h"
#include "tunnelbridge_map.h"

#include "3rdparty/nlohmann/json.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>

namespace {

using json = nlohmann::json;

struct WorldSemanticBounds {
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

bool ParseWorldSemanticCoordinate(const char *&cursor, uint32_t &value)
{
	errno = 0;
	char *end = nullptr;
	const unsigned long long parsed = std::strtoull(cursor, &end, 10);
	if (end == cursor || errno == ERANGE || parsed > std::numeric_limits<uint32_t>::max()) return false;
	value = static_cast<uint32_t>(parsed);
	cursor = end;
	return true;
}

bool ParseWorldSemanticBounds(uint width, uint height, WorldSemanticBounds &bounds)
{
	bounds.end_x = width;
	bounds.end_y = height;
	const char *raw = std::getenv("OPENTTDRS_WORLD_SEMANTIC_REGION");
	if (raw == nullptr || raw[0] == '\0') return true;

	bounds.filtered = true;
	const char *cursor = raw;
	if (!ParseWorldSemanticCoordinate(cursor, bounds.requested_min_x) || *cursor++ != ',' ||
			!ParseWorldSemanticCoordinate(cursor, bounds.requested_min_y) || *cursor++ != ',' ||
			!ParseWorldSemanticCoordinate(cursor, bounds.requested_max_x) || *cursor++ != ',' ||
			!ParseWorldSemanticCoordinate(cursor, bounds.requested_max_y) || *cursor != '\0' ||
			bounds.requested_min_x > bounds.requested_max_x ||
			bounds.requested_min_y > bounds.requested_max_y) {
		std::fprintf(stderr, "openttdrs world-semantic: región inválida %s (usar x0,y0,x1,y1)\n", raw);
		return false;
	}

	bounds.begin_x = std::min<uint>(bounds.requested_min_x, width);
	bounds.begin_y = std::min<uint>(bounds.requested_min_y, height);
	bounds.end_x = bounds.requested_max_x >= width ? width : bounds.requested_max_x + 1;
	bounds.end_y = bounds.requested_max_y >= height ? height : bounds.requested_max_y + 1;
	return true;
}

uint64_t WorldSemanticTileCount(const WorldSemanticBounds &bounds)
{
	return static_cast<uint64_t>(bounds.end_x - bounds.begin_x) *
		static_cast<uint64_t>(bounds.end_y - bounds.begin_y);
}

json WorldSemanticRegionJson(const WorldSemanticBounds &bounds)
{
	if (!bounds.filtered) return nullptr;
	return {
		{"min_x", bounds.requested_min_x},
		{"min_y", bounds.requested_min_y},
		{"max_x", bounds.requested_max_x},
		{"max_y", bounds.requested_max_y},
	};
}

int WorldSemanticMinCall()
{
	const char *raw = std::getenv("OPENTTDRS_WORLD_SEMANTIC_MIN_CALL");
	if (raw == nullptr || raw[0] == '\0') raw = std::getenv("OPENTTDRS_WORLD_RAW_MIN_CALL");
	if (raw == nullptr || raw[0] == '\0') raw = std::getenv("OPENTTDRS_SNAPSHOT_MIN_CALL");
	return raw != nullptr && raw[0] != '\0' ? std::atoi(raw) : 2;
}

const char *SemanticClass(uint8_t tile_type)
{
	switch (tile_type) {
		case 0: return "clear";
		case 1: return "railway";
		case 2: return "road";
		case 3: return "house";
		case 4: return "trees";
		case 5: return "station";
		case 6: return "water";
		case 7: return "void";
		case 8: return "industry";
		case 9: return "tunnel_bridge";
		case 10: return "object";
		default: return "unknown";
	}
}

json RawTile(Tile tile)
{
	return {
		{"height", static_cast<uint32_t>(TileHeight(tile))},
		{"type", static_cast<uint32_t>(tile.type())},
		{"m1", static_cast<uint32_t>(tile.m1())},
		{"m2", static_cast<uint32_t>(tile.m2())},
		{"m3", static_cast<uint32_t>(tile.m3())},
		{"m4", static_cast<uint32_t>(tile.m4())},
		{"m5", static_cast<uint32_t>(tile.m5())},
		{"m6", static_cast<uint32_t>(tile.m6())},
		{"m7", static_cast<uint32_t>(tile.m7())},
		{"m8", static_cast<uint32_t>(tile.m8())},
	};
}

json ClearDetails(Tile tile)
{
	const uint8_t ground = (tile.m5() >> 2) & 0x07;
	return {
		{"family", "clear"},
		{"ground", ground},
		{"density", tile.m5() & 0x03},
		{"counter", (tile.m5() >> 5) & 0x07},
		{"field_type", ground == 3 ? json(tile.m3() & 0x0f) : json(nullptr)},
		{"snow", (tile.m3() & 0x10) != 0},
	};
}

json RailwayDetails(Tile tile)
{
	const uint8_t subtype = (tile.m5() >> 6) & 0x03;
	const bool plain = subtype == 0 || subtype == 1;
	const bool signals = subtype == 1;
	const uint16_t m2 = tile.m2();
	uint8_t reservation = 0;
	if (plain) {
		const uint8_t saved_track = (m2 >> 8) & 0x07;
		if (saved_track != 0) {
			reservation = static_cast<uint8_t>(1U << (saved_track - 1));
			if ((m2 & (1U << 11)) != 0) {
				switch (reservation) {
					case 0x04: reservation |= 0x08; break;
					case 0x08: reservation |= 0x04; break;
					case 0x10: reservation |= 0x20; break;
					case 0x20: reservation |= 0x10; break;
					default: break;
				}
			}
		}
	}
	return {
		{"family", "railway"},
		{"rail_tile_type", subtype},
		{"track_bits", plain ? json(tile.m5() & 0x3f) : json(nullptr)},
		{"rail_type", tile.m8() & 0x3f},
		{"depot_direction", subtype == 3 ? json(tile.m5() & 0x03) : json(nullptr)},
		{"signal_present", signals ? json(tile.m3() >> 4) : json(nullptr)},
		{"signal_state", signals ? json(tile.m4() >> 4) : json(nullptr)},
		{"reservation_track_bits", plain ? json(reservation) : json(nullptr)},
	};
}

json RoadDetails(Tile tile)
{
	const uint8_t subtype = (tile.m5() >> 6) & 0x03;
	const bool normal = subtype == 0;
	const uint8_t tram_type = (tile.m8() >> 6) & 0x3f;
	return {
		{"family", "road"},
		{"road_tile_type", subtype},
		{"road_bits", normal ? json(tile.m5() & 0x0f) : json(nullptr)},
		{"tram_bits", normal ? json(tile.m3() & 0x0f) : json(nullptr)},
		{"road_type", tile.m4() & 0x3f},
		{"tram_type", tram_type == 0x3f ? json(nullptr) : json(tram_type)},
		{"crossing_road_axis", subtype == 1 ? json(tile.m5() & 0x01) : json(nullptr)},
		{"crossing_rail_axis", subtype == 1 ? json((tile.m5() & 0x01) ^ 1) : json(nullptr)},
		{"depot_direction", subtype == 2 ? json(tile.m5() & 0x03) : json(nullptr)},
		{"roadside", (tile.m6() >> 3) & 0x07},
	};
}

json HouseDetails(Tile tile)
{
	const bool completed = (tile.m3() & 0x80) != 0;
	return {
		{"family", "house"},
		{"town_id", static_cast<uint32_t>(tile.m2())},
		{"house_type", tile.m8() & 0x0fff},
		{"completed", completed},
		{"building_stage", completed ? 3 : ((tile.m5() >> 3) & 0x03)},
	};
}

json TreeDetails(Tile tile)
{
	return {
		{"family", "trees"},
		{"tree_type", static_cast<uint32_t>(tile.m3())},
		{"ground", (tile.m2() >> 6) & 0x07},
		{"density", (tile.m2() >> 4) & 0x03},
		{"count", ((tile.m5() >> 6) & 0x03) + 1},
		{"growth", tile.m5() & 0x07},
		{"water_class", (tile.m1() >> 5) & 0x03},
	};
}

json StationDetails(Tile tile)
{
	const uint8_t station_type = (tile.m6() >> 3) & 0x0f;
	const uint8_t gfx = tile.m5();
	const bool has_rail = station_type == 0 || station_type == 7;
	const bool road_stop = station_type == 2 || station_type == 3 || station_type == 8;
	const bool bay = (station_type == 2 || station_type == 3) && gfx < 4;
	const bool drive_through = road_stop && (gfx == 4 || gfx == 5);
	const bool dock = station_type == 5;
	return {
		{"family", "station"},
		{"station_id", static_cast<uint32_t>(tile.m2())},
		{"station_type", station_type},
		{"station_gfx", gfx},
		{"rail_type", has_rail ? json(tile.m8() & 0x3f) : json(nullptr)},
		{"rail_axis", has_rail ? json((gfx & 0x01) != 0 ? 1 : 0) : json(nullptr)},
		{"catenary_wires", has_rail ? json((tile.m3() & 0x02) != 0) : json(nullptr)},
		{"catenary_pylons", has_rail ? json((tile.m3() & 0x04) != 0) : json(nullptr)},
		{"station_custom_spec", has_rail ? json(tile.m4()) : json(nullptr)},
		{"road_stop_layout", bay ? json("bay") : (drive_through ? json("drive_through") : json(nullptr))},
		{"road_stop_bay_direction", bay ? json(gfx) : json(nullptr)},
		{"road_stop_drive_through_axis", drive_through ? json(gfx == 5 ? 1 : 0) : json(nullptr)},
		{"road_stop_custom_spec", road_stop ? json(tile.m8() & 0x003f) : json(nullptr)},
		{"dock_water_part", dock ? json(gfx >= 4) : json(nullptr)},
		{"dock_direction", dock && gfx < 4 ? json(gfx) : json(nullptr)},
	};
}

json WaterDetails(Tile tile)
{
	const uint8_t type = (tile.m5() >> 4) & 0x0f;
	const bool depot = type == 3;
	const bool lock = type == 2;
	const uint8_t depot_axis = (tile.m5() >> 1) & 0x01;
	const uint8_t depot_part = tile.m5() & 0x01;
	return {
		{"family", "water"},
		{"water_tile_type", type},
		{"water_class", (tile.m1() >> 5) & 0x03},
		{"ship_depot_axis", depot ? json(depot_axis) : json(nullptr)},
		{"ship_depot_part", depot ? json(depot_part) : json(nullptr)},
		{"ship_depot_direction", depot ? json((depot_axis * 3) ^ (depot_part * 2)) : json(nullptr)},
		{"lock_direction", lock ? json(tile.m5() & 0x03) : json(nullptr)},
		{"lock_part", lock ? json((tile.m5() >> 2) & 0x03) : json(nullptr)},
	};
}

json IndustryDetails(Tile tile)
{
	const bool completed = (tile.m1() & 0x80) != 0;
	return {
		{"family", "industry"},
		{"industry_id", static_cast<uint32_t>(tile.m2())},
		{"completed", completed},
		{"construction_stage", completed ? 3 : (tile.m1() & 0x03)},
		{"gfx", tile.m5() | (((tile.m6() >> 2) & 0x01) << 8)},
	};
}

json TunnelBridgeDetails(Tile tile)
{
	const bool tunnel = (tile.m5() & 0x80) == 0;
	const uint8_t transport = (tile.m5() >> 2) & 0x03;
	const TileIndex other = GetOtherTunnelBridgeEnd(tile);
	const uint8_t tram_type = (tile.m8() >> 6) & 0x3f;
	return {
		{"family", "tunnel_bridge"},
		{"is_tunnel", tunnel},
		{"transport_type", transport},
		{"direction", tile.m5() & 0x03},
		{"other_end", {{"x", TileX(other)}, {"y", TileY(other)}}},
		{"bridge_type", !tunnel ? json((tile.m6() >> 2) & 0x0f) : json(nullptr)},
		{"rail_type", transport == 0 ? json(tile.m8() & 0x3f) : json(nullptr)},
		{"road_type", transport != 0 ? json(tile.m4() & 0x3f) : json(nullptr)},
		{"tram_type", transport != 0 && tram_type != 0x3f ? json(tram_type) : json(nullptr)},
		{"rail_reserved", transport == 0 ? json((tile.m5() & 0x10) != 0) : json(nullptr)},
	};
}

json ObjectDetails(Tile tile)
{
	const uint32_t object_id = static_cast<uint32_t>(tile.m2()) | (static_cast<uint32_t>(tile.m5()) << 16);
	return {
		{"family", "object"},
		{"object_id", object_id},
		{"random", static_cast<uint32_t>(tile.m3())},
	};
}

json Details(Tile tile, uint8_t tile_type)
{
	switch (tile_type) {
		case 0: return ClearDetails(tile);
		case 1: return RailwayDetails(tile);
		case 2: return RoadDetails(tile);
		case 3: return HouseDetails(tile);
		case 4: return TreeDetails(tile);
		case 5: return StationDetails(tile);
		case 6: return WaterDetails(tile);
		case 7: return {{"family", "void"}};
		case 8: return IndustryDetails(tile);
		case 9: return TunnelBridgeDetails(tile);
		case 10: return ObjectDetails(tile);
		default: return {{"family", "unknown"}};
	}
}

json SemanticTile(uint64_t index, uint x, uint y, Tile tile)
{
	const uint8_t tile_type = tile.type() >> 4;
	const bool supported = tile_type <= 10;
	auto [tileh, base_z] = GetTileSlopeZ(tile);
	const uint8_t bridge_bits = (tile.type() >> 2) & 0x03;
	json owner = (tile_type == 3 || tile_type == 7 || tile_type == 8)
		? json(nullptr)
		: json(tile.m1() & 0x1f);
	json bridge_axis = bridge_bits == 1 ? json(0) : (bridge_bits == 2 ? json(1) : json(nullptr));
	return {
		{"kind", "tile_semantic"},
		{"index", index},
		{"x", x},
		{"y", y},
		{"tile_type", tile_type},
		{"class", SemanticClass(tile_type)},
		{"tileh", static_cast<uint32_t>(tileh)},
		{"base_z", base_z},
		{"owner", owner},
		{"bridge_above_axis", bridge_axis},
		{"supported", supported},
		{"unsupported_reason", supported ? json(nullptr) : json("tile_type")},
		{"raw", RawTile(tile)},
		{"details", Details(tile, tile_type)},
	};
}

} // namespace

bool OpenttdrsMaybeExportWorldSemantic(const std::string &source_path)
{
	const char *out_path = std::getenv("OPENTTDRS_WORLD_SEMANTIC_OUT");
	if (out_path == nullptr || out_path[0] == '\0') return true;

	static int call_count = 0;
	call_count++;
	if (call_count < WorldSemanticMinCall()) return true;

	const uint width = Map::SizeX();
	const uint height = Map::SizeY();
	WorldSemanticBounds bounds;
	if (!ParseWorldSemanticBounds(width, height, bounds)) return false;

	std::ofstream out(out_path, std::ios::out | std::ios::trunc);
	if (!out) return false;

	const char *commit = std::getenv("OPENTTDRS_OPENTTD_COMMIT");
	const char *raw_source = std::getenv("OPENTTDRS_WORLD_SEMANTIC_SOURCE");
	const char *save_hash = std::getenv("OPENTTDRS_WORLD_SEMANTIC_SAVE_SHA256");
	extern SaveLoadVersion _sl_version;
	json metadata;
	metadata["kind"] = "metadata";
	metadata["schema_version"] = 1;
	metadata["contract"] = "world-semantic";
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
	metadata["emitted_tile_count"] = WorldSemanticTileCount(bounds);
	metadata["region"] = WorldSemanticRegionJson(bounds);
	out << metadata.dump() << '\n';
	if (!out) return false;

	for (uint y = bounds.begin_y; y < bounds.end_y; y++) {
		for (uint x = bounds.begin_x; x < bounds.end_x; x++) {
			Tile tile(TileXY(x, y));
			out << SemanticTile(static_cast<uint64_t>(y) * width + x, x, y, tile).dump() << '\n';
			if (!out) return false;
		}
	}
	out.flush();
	if (!out) return false;

	_exit_game = true;
	return true;
}
