/*
 * Snapshot export for openttdrs parity (#110).
 * Produces JSON without invoking openttdrs parsers.
 */

#include "snapshot_export.h"

#include "aircraft.h"
#include "company_func.h"
#include "core/random_func.hpp"
#include "direction_type.h"
#include "engine_base.h"
#include "fileio_type.h"
#include "group_type.h"
#include "map_func.h"
#include "openttd.h"
#include "rail_map.h"
#include "saveload/saveload.h"
#include "station_base.h"
#include "table/sprites.h"
#include "tile_map.h"
#include "tile_type.h"
#include "timer/timer_game_tick.h"
#include "train.h"
#include "vehicle_base.h"
#include "vehicle_func.h"

#include "3rdparty/nlohmann/json.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <queue>
#include <string>
#include <utility>
#include <vector>

/* From train_cmd.cpp — spacing wagons after attach. */
bool TrainController(Train *v, Vehicle *nomove, bool reverse = true);

namespace {

constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
constexpr uint64_t FNV_PRIME = 0x100000001b3ULL;

struct Fnv1a64 {
	uint64_t h = FNV_OFFSET;
	void write_u8(uint8_t v)
	{
		h ^= v;
		h *= FNV_PRIME;
	}
	void write_u16(uint16_t v)
	{
		write_u8(static_cast<uint8_t>(v & 0xFF));
		write_u8(static_cast<uint8_t>((v >> 8) & 0xFF));
	}
	std::string hex() const
	{
		char buf[17];
		std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
		return buf;
	}
};

/** Same mapping as openttdrs `ottd_tile_kind` + snapshot_dumper kind codes. */
uint8_t KindCode(Tile tile)
{
	const uint8_t ottd_type = static_cast<uint8_t>(GetTileType(tile));
	const uint8_t m5 = tile.m5();
	const uint8_t transport_subtype = (m5 >> 6) & 0x3;
	switch (ottd_type) {
		case MP_CLEAR:
		case MP_OBJECT:
			return 1; /* Grass */
		case MP_RAILWAY:
			return transport_subtype == 3 ? 11 : 4; /* RailDepot / Rail */
		case MP_ROAD:
			return transport_subtype == 2 ? 10 : 3; /* RoadDepot / Road */
		case MP_HOUSE:
			return 5;
		case MP_TREES:
			return 8; /* Forest */
		case MP_STATION:
			/* Alineado con openttdrs `ottd_tile_kind`: no distingue aeropuerto. */
			return 7;
		case MP_WATER:
			/* Alineado con openttdrs: depósitos navales cuentan como Water. */
			return 2;
		case MP_VOID:
			return 0;
		case MP_INDUSTRY:
			return 6;
		case MP_TUNNELBRIDGE: {
			const bool is_bridge = (m5 & 0x80) != 0;
			const uint8_t transport = (m5 >> 2) & 0x3;
			if (is_bridge) return transport == 0 ? 15 : 14; /* RailBridge / RoadBridge */
			return transport == 0 ? 13 : 12; /* RailTunnel / RoadTunnel */
		}
		default:
			return static_cast<uint8_t>(128u + ottd_type);
	}
}

const char *KindName(uint8_t code)
{
	switch (code) {
		case 0: return "Void";
		case 1: return "Grass";
		case 2: return "Water";
		case 3: return "Road";
		case 4: return "Rail";
		case 5: return "House";
		case 6: return "Industry";
		case 7: return "Station";
		case 8: return "Forest";
		case 9: return "CoalField";
		case 10: return "RoadDepot";
		case 11: return "RailDepot";
		case 12: return "RoadTunnel";
		case 13: return "RailTunnel";
		case 14: return "RoadBridge";
		case 15: return "RailBridge";
		case 16: return "ShipDepot";
		case 17: return "Airport";
		default: return "Unknown";
	}
}

bool IsKind(Tile tile, uint8_t want)
{
	return KindCode(tile) == want;
}

size_t CountComponents(uint8_t want)
{
	const uint w = Map::SizeX();
	const uint h = Map::SizeY();
	std::vector<uint8_t> visited(static_cast<size_t>(w) * h, 0);
	size_t comps = 0;
	auto idx = [w](uint x, uint y) { return static_cast<size_t>(y) * w + x; };

	for (uint y = 0; y < h; y++) {
		for (uint x = 0; x < w; x++) {
			const size_t i = idx(x, y);
			if (visited[i]) continue;
			Tile t(TileXY(x, y));
			if (!IsKind(t, want)) {
				visited[i] = 1;
				continue;
			}
			comps++;
			visited[i] = 1;
			std::queue<std::pair<uint, uint>> q;
			q.push({x, y});
			while (!q.empty()) {
				auto [cx, cy] = q.front();
				q.pop();
				const int nbs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
				for (auto &d : nbs) {
					const int nx = static_cast<int>(cx) + d[0];
					const int ny = static_cast<int>(cy) + d[1];
					if (nx < 0 || ny < 0 || static_cast<uint>(nx) >= w || static_cast<uint>(ny) >= h) continue;
					const size_t ni = idx(static_cast<uint>(nx), static_cast<uint>(ny));
					if (visited[ni]) continue;
					visited[ni] = 1;
					if (IsKind(Tile(TileXY(static_cast<uint>(nx), static_cast<uint>(ny))), want)) {
						q.push({static_cast<uint>(nx), static_cast<uint>(ny)});
					}
				}
			}
		}
	}
	return comps;
}

struct PbsTraceState {
	std::ofstream out;
	std::string source_path;
	uint64_t rows = 0;
	uint64_t max_rows = 40;
	bool armed = false;
};

PbsTraceState _openttdrs_pbs_trace;

uint64_t ParseTraceTicks()
{
	const char *raw = std::getenv("OPENTTDRS_PBS_TRACE_TICKS");
	if (raw == nullptr || raw[0] == '\0') return 40;
	const long long parsed = std::atoll(raw);
	return parsed > 0 ? static_cast<uint64_t>(parsed) : 40;
}

/** Misma convención que openttdrs `rail_pixel_from_openttd_pos`. */
uint8_t RailPixelFromPos(int x_pos, int y_pos, Direction direction)
{
	const uint8_t xf = static_cast<uint8_t>(((x_pos % 16) + 16) % 16);
	const uint8_t yf = static_cast<uint8_t>(((y_pos % 16) + 16) % 16);
	switch (direction) {
		case DIR_SW: return xf;
		case DIR_SE: return yf;
		case DIR_NW: return static_cast<uint8_t>(15 - yf);
		case DIR_N: return std::min(static_cast<uint8_t>(15 - xf), static_cast<uint8_t>(15 - yf));
		case DIR_S: return std::min(xf, yf);
		case DIR_E: return std::min(static_cast<uint8_t>(15 - xf), yf);
		case DIR_W: return std::min(xf, static_cast<uint8_t>(15 - yf));
		default: return static_cast<uint8_t>(15 - xf); /* DIR_NE */
	}
}

nlohmann::json UnitsForTrain(const Train *head)
{
	nlohmann::json units = nlohmann::json::array();
	int index = 0;
	for (const Train *u = head; u != nullptr; u = u->Next(), index++) {
		nlohmann::json unit;
		unit["index"] = index;
		unit["x"] = TileX(u->tile);
		unit["y"] = TileY(u->tile);
		unit["rail_pixel"] = RailPixelFromPos(u->x_pos, u->y_pos, u->direction);
		unit["direction"] = static_cast<uint8_t>(u->direction);
		units.push_back(unit);
	}
	return units;
}

/**
 * Engancha N vagones Goods Van (engine 32) detrás del primer tren y los
 * espacia con TrainController. Opcionalmente guarda el .sav resultante.
 *
 * Activación:
 *   OPENTTDRS_FIXTURE_ATTACH_WAGONS=2
 *   OPENTTDRS_FIXTURE_SAVE_OUT=/ruta/absoluta/salida.sav
 */
void MaybeAttachWagonsForFixture()
{
	const char *raw = std::getenv("OPENTTDRS_FIXTURE_ATTACH_WAGONS");
	if (raw == nullptr || raw[0] == '\0') return;
	const int count = std::atoi(raw);
	if (count <= 0) return;

	Train *head = nullptr;
	for (Vehicle *v : Vehicle::Iterate()) {
		if (v->type != VEH_TRAIN || !v->IsPrimaryVehicle()) continue;
		head = Train::From(v);
		break;
	}
	if (head == nullptr) {
		std::fprintf(stderr, "openttdrs fixture: no hay tren primario\n");
		return;
	}

	const Engine *wagon_engine = Engine::GetIfValid(EngineID{32}); /* Goods Van temperate */
	if (wagon_engine == nullptr || wagon_engine->type != VEH_TRAIN) {
		std::fprintf(stderr, "openttdrs fixture: engine 32 (Goods Van) no disponible\n");
		return;
	}
	const RailVehicleInfo *rvi = &wagon_engine->VehInfo<RailVehicleInfo>();
	_current_company = head->owner;

	for (int n = 0; n < count; n++) {
		Train *last = head->Last();
		Train *wagon = new Train();
		wagon->spritenum = rvi->image_index;
		wagon->engine_type = wagon_engine->index;
		wagon->gcache.first_engine = EngineID::Invalid();
		wagon->direction = last->direction;
		wagon->tile = last->tile;
		wagon->x_pos = last->x_pos;
		wagon->y_pos = last->y_pos;
		wagon->z_pos = last->z_pos;
		wagon->owner = head->owner;
		wagon->track = last->track;
		wagon->vehstatus = last->vehstatus;
		wagon->vehstatus.Reset(VehState::Stopped);
		wagon->vehstatus.Reset(VehState::Hidden);
		wagon->SetWagon();
		wagon->cargo_type = wagon_engine->GetDefaultCargoType();
		wagon->cargo_cap = rvi->capacity;
		wagon->refit_cap = 0;
		wagon->railtypes = rvi->railtypes;
		wagon->date_of_last_service = head->date_of_last_service;
		wagon->date_of_last_service_newgrf = head->date_of_last_service_newgrf;
		wagon->build_year = head->build_year;
		wagon->sprite_cache.sprite_seq.Set(SPR_IMG_QUERY);
		wagon->random_bits = Random();
		wagon->group_id = DEFAULT_GROUP;
		wagon->UpdatePosition();

		last->SetNext(wagon);
		head->ConsistChanged(CCF_ARRANGE);

		const int steps = last->CalcNextVehicleOffset();
		for (int i = 0; i < steps; i++) {
			if (!TrainController(head, wagon, false)) break;
		}
		head->ConsistChanged(CCF_TRACK);
	}

	std::fprintf(stderr, "openttdrs fixture: enganchados %d vagón(es); unidades=%d\n",
			count, CountVehiclesInChain(head));

	const char *save_out = std::getenv("OPENTTDRS_FIXTURE_SAVE_OUT");
	if (save_out != nullptr && save_out[0] != '\0') {
		if (SaveOrLoad(save_out, SLO_SAVE, DFT_GAME_FILE, NO_DIRECTORY, false) != SL_OK) {
			std::fprintf(stderr, "openttdrs fixture: falló el save en %s\n", save_out);
		} else {
			std::fprintf(stderr, "openttdrs fixture: guardado %s\n", save_out);
		}
	}
}

void WritePbsTraceRow(const char *kind)
{
	nlohmann::json row;
	row["kind"] = kind;
	row["tick"] = TimerGameTick::counter;
	row["trains"] = nlohmann::json::array();
	row["rail_reservations"] = nlohmann::json::array();

	for (const Vehicle *v : Vehicle::Iterate()) {
		if (v->type != VEH_TRAIN || !v->IsPrimaryVehicle() || v->tile == INVALID_TILE) continue;
		const Train *train_v = Train::From(v);
		nlohmann::json train;
		train["vehicle"] = v->index.base();
		train["x"] = TileX(v->tile);
		train["y"] = TileY(v->tile);
		train["progress"] = v->progress;
		train["speed"] = v->cur_speed;
		train["subspeed"] = v->subspeed;
		train["direction"] = static_cast<uint8_t>(v->direction);
		train["units"] = UnitsForTrain(train_v);
		row["trains"].push_back(train);
	}

	for (uint y = 0; y < Map::SizeY(); y++) {
		for (uint x = 0; x < Map::SizeX(); x++) {
			Tile tile(TileXY(x, y));
			if (!IsPlainRailTile(tile)) continue;
			const TrackBits reserved = GetRailReservationTrackBits(tile);
			if (reserved == TRACK_BIT_NONE) continue;
			nlohmann::json reservation;
			reservation["x"] = x;
			reservation["y"] = y;
			reservation["track_bits"] = static_cast<uint32_t>(reserved);
			row["rail_reservations"].push_back(reservation);
		}
	}

	_openttdrs_pbs_trace.out << row.dump() << '\n';
	_openttdrs_pbs_trace.out.flush();
}

} // namespace

namespace {

/** Requested and effective bounds for OPENTTDRS_WORLD_RAW_REGION. */
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

bool OpenttdrsMaybeExportSnapshot(const std::string &source_path)
{
	const char *out_path = std::getenv("OPENTTDRS_SNAPSHOT_OUT");
	if (out_path == nullptr || out_path[0] == '\0') return true;

	/* Dedicated + `-g` genera un new-game (1er AfterLoadGame) y luego carga el .sav
	 * (2º). Por defecto saltamos el primero. OPENTTDRS_SNAPSHOT_MIN_CALL=1 fuerza el 1º. */
	static int call_count = 0;
	call_count++;
	const char *min_s = std::getenv("OPENTTDRS_SNAPSHOT_MIN_CALL");
	const int min_call = (min_s != nullptr && min_s[0] != '\0') ? std::atoi(min_s) : 2;
	if (call_count < min_call) return true;

	const char *commit = std::getenv("OPENTTDRS_OPENTTD_COMMIT");
	const uint w = Map::SizeX();
	const uint h = Map::SizeY();

	Fnv1a64 h_height, h_kind, h_mapt, h_rail, h_road;
	std::map<std::string, uint64_t> counts;
	uint8_t min_h = 255, max_h = 0;

	for (uint y = 0; y < h; y++) {
		for (uint x = 0; x < w; x++) {
			Tile t(TileXY(x, y));
			const uint8_t height = static_cast<uint8_t>(TileHeight(t));
			const uint8_t mapt = t.type(); /* full MAPT byte */
			const uint8_t m5 = t.m5();
			const uint8_t kind = KindCode(t);
			const char *name = KindName(kind);
			counts[name]++;
			min_h = std::min(min_h, height);
			max_h = std::max(max_h, height);
			h_height.write_u8(height);
			h_kind.write_u8(kind);
			h_mapt.write_u8(mapt);
			if (kind == 4) { /* Rail (plain / signals share m5 track bits) */
				h_rail.write_u8(m5 & 0x3F);
				h_rail.write_u8(t.m3());
				h_rail.write_u8(t.m4()); /* ottdmap m3hi */
			}
			if (kind == 3) { /* Road */
				h_road.write_u8(m5 & 0x0F);
				h_road.write_u16(t.m8());
			}
		}
	}

	nlohmann::json j;
	j["schema_version"] = 1;
	j["producer"] = "openttd";
	j["openttd_commit"] = commit != nullptr ? commit : "";
	j["source_path"] = source_path;
	j["map"] = {
		{"width", w},
		{"height", h},
		{"tile_count", static_cast<uint64_t>(w) * h},
		{"tile_kind_counts", counts},
		{"max_height", max_h},
		{"min_height", min_h == 255 ? 0 : min_h},
	};
	j["hashes"] = {
		{"height_hash_fnv1a64", h_height.hex()},
		{"kind_hash_fnv1a64", h_kind.hex()},
		{"mapt_hash_fnv1a64", h_mapt.hex()},
		{"rail_bits_hash_fnv1a64", h_rail.hex()},
		{"road_bits_hash_fnv1a64", h_road.hex()},
	};
	/* ottdmap footers do not exist in the live engine; leave zeros. */
	j["extras"] = {
		{"dense_payload_end", 0},
		{"footer_industry_pairs", 0},
		{"footer_station_xy", 0},
		{"footer_tnbp_blob_len", 0},
	};
	j["components"] = {
		{"industry_components", CountComponents(6)},
		{"station_components", CountComponents(7)},
	};

	std::ofstream out(out_path);
	if (!out) return false;
	out << j.dump(2) << '\n';
	if (!out) return false;

	/* Dedicated: salir tras exportar (evita servidor colgado). */
	_exit_game = true;
	return true;
}

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

	/* Dedicated: exit after the dump instead of serving forever. */
	_exit_game = true;
	return true;
}

void OpenttdrsMaybeStartPbsTrace(const std::string &source_path)
{
	const char *out_path = std::getenv("OPENTTDRS_PBS_TRACE_OUT");
	const char *fixture_wagons = std::getenv("OPENTTDRS_FIXTURE_ATTACH_WAGONS");
	const bool want_fixture = fixture_wagons != nullptr && fixture_wagons[0] != '\0';
	if ((out_path == nullptr || out_path[0] == '\0') && !want_fixture) return;

	/* Dedicated + -g loads a new game before the requested save. Match the
	 * snapshot exporter and arm only after the requested AfterLoadGame call. */
	static int call_count = 0;
	call_count++;
	const char *min_s = std::getenv("OPENTTDRS_SNAPSHOT_MIN_CALL");
	const int min_call = (min_s != nullptr && min_s[0] != '\0') ? std::atoi(min_s) : 2;
	if (call_count < min_call || _openttdrs_pbs_trace.armed) return;

	MaybeAttachWagonsForFixture();

	/* Solo generar el .sav del fixture, sin traza. */
	if (out_path == nullptr || out_path[0] == '\0') {
		_exit_game = true;
		return;
	}

	_openttdrs_pbs_trace.out.open(out_path, std::ios::out | std::ios::trunc);
	if (!_openttdrs_pbs_trace.out.is_open()) {
		std::fprintf(stderr, "openttdrs PBS trace cannot open %s\n", out_path);
		return;
	}
	const char *trace_source = std::getenv("OPENTTDRS_PBS_TRACE_SOURCE");
	_openttdrs_pbs_trace.source_path =
		trace_source != nullptr && trace_source[0] != '\0' ? trace_source : source_path;
	_openttdrs_pbs_trace.rows = 0;
	_openttdrs_pbs_trace.max_rows = ParseTraceTicks();
	_openttdrs_pbs_trace.armed = true;

	nlohmann::json metadata;
	metadata["kind"] = "metadata";
	metadata["schema_version"] = 2;
	metadata["producer"] = "openttd";
	const char *commit = std::getenv("OPENTTDRS_OPENTTD_COMMIT");
	metadata["openttd_commit"] = commit != nullptr ? commit : "";
	metadata["source_path"] = _openttdrs_pbs_trace.source_path;
	metadata["initial_sample_point"] = "after_load_game";
	metadata["tick_sample_point"] = "after_state_game_loop";
	metadata["max_ticks"] = _openttdrs_pbs_trace.max_rows;
	_openttdrs_pbs_trace.out << metadata.dump() << '\n';
	_openttdrs_pbs_trace.out.flush();
	WritePbsTraceRow("initial");
}

void OpenttdrsMaybeExportPbsTraceTick()
{
	if (!_openttdrs_pbs_trace.armed) return;

	WritePbsTraceRow("tick");
	_openttdrs_pbs_trace.rows++;
	if (_openttdrs_pbs_trace.rows >= _openttdrs_pbs_trace.max_rows) {
		_openttdrs_pbs_trace.armed = false;
		_openttdrs_pbs_trace.out.close();
		_exit_game = true;
	}
}

namespace {

struct AirportFtaTraceState {
	std::ofstream out;
	std::string source_path;
	uint64_t rows = 0;
	uint64_t max_rows = 80;
	bool armed = false;
};

AirportFtaTraceState _openttdrs_airport_fta_trace;

uint64_t ParseAirportFtaTraceTicks()
{
	const char *raw = std::getenv("OPENTTDRS_AIRPORT_FTA_TRACE_TICKS");
	if (raw == nullptr || raw[0] == '\0') return 80;
	const long long parsed = std::atoll(raw);
	return parsed > 0 ? static_cast<uint64_t>(parsed) : 80;
}

void WriteAirportFtaTraceRow(const char *kind)
{
	nlohmann::json row;
	row["kind"] = kind;
	row["tick"] = TimerGameTick::counter;
	row["aircraft"] = nlohmann::json::array();
	row["airports"] = nlohmann::json::array();

	for (const Vehicle *v : Vehicle::Iterate()) {
		if (v->type != VEH_AIRCRAFT) continue;
		const Aircraft *a = Aircraft::From(v);
		if (!a->IsNormalAircraft()) continue;

		nlohmann::json ac;
		ac["vehicle"] = v->index.base();
		ac["engine"] = a->engine_type.base();
		ac["x"] = TileX(v->tile);
		ac["y"] = TileY(v->tile);
		ac["x_pos"] = v->x_pos;
		ac["y_pos"] = v->y_pos;
		ac["z_pos"] = v->z_pos;
		ac["direction"] = static_cast<uint8_t>(v->direction);
		ac["pos"] = a->pos;
		ac["previous_pos"] = a->previous_pos;
		ac["state"] = a->state;
		ac["targetairport"] = a->targetairport.base();
		ac["speed"] = v->cur_speed;
		ac["running"] = !v->vehstatus.Test(VehState::Stopped);
		row["aircraft"].push_back(ac);
	}

	for (const Station *st : Station::Iterate()) {
		if (!st->facilities.Test(StationFacility::Airport) || st->airport.tile == INVALID_TILE) {
			continue;
		}
		nlohmann::json ap;
		ap["station"] = st->index.base();
		ap["x"] = TileX(st->airport.tile);
		ap["y"] = TileY(st->airport.tile);
		ap["w"] = st->airport.w;
		ap["h"] = st->airport.h;
		ap["type"] = st->airport.type;
		ap["layout"] = st->airport.layout;
		ap["blocks"] = st->airport.blocks.base();
		row["airports"].push_back(ap);
	}

	_openttdrs_airport_fta_trace.out << row.dump() << '\n';
	_openttdrs_airport_fta_trace.out.flush();
}

} // namespace

void OpenttdrsMaybeStartAirportFtaTrace(const std::string &source_path)
{
	const char *out_path = std::getenv("OPENTTDRS_AIRPORT_FTA_TRACE_OUT");
	if (out_path == nullptr || out_path[0] == '\0') return;

	static int call_count = 0;
	call_count++;
	const char *min_s = std::getenv("OPENTTDRS_SNAPSHOT_MIN_CALL");
	const int min_call = (min_s != nullptr && min_s[0] != '\0') ? std::atoi(min_s) : 2;
	if (call_count < min_call || _openttdrs_airport_fta_trace.armed) return;

	_openttdrs_airport_fta_trace.out.open(out_path, std::ios::out | std::ios::trunc);
	if (!_openttdrs_airport_fta_trace.out.is_open()) {
		std::fprintf(stderr, "openttdrs airport FTA trace cannot open %s\n", out_path);
		return;
	}
	const char *trace_source = std::getenv("OPENTTDRS_AIRPORT_FTA_TRACE_SOURCE");
	_openttdrs_airport_fta_trace.source_path =
		trace_source != nullptr && trace_source[0] != '\0' ? trace_source : source_path;
	_openttdrs_airport_fta_trace.rows = 0;
	_openttdrs_airport_fta_trace.max_rows = ParseAirportFtaTraceTicks();
	_openttdrs_airport_fta_trace.armed = true;

	nlohmann::json metadata;
	metadata["kind"] = "metadata";
	metadata["schema_version"] = 1;
	metadata["producer"] = "openttd";
	metadata["trace"] = "airport_fta";
	const char *commit = std::getenv("OPENTTDRS_OPENTTD_COMMIT");
	metadata["openttd_commit"] = commit != nullptr ? commit : "";
	metadata["source_path"] = _openttdrs_airport_fta_trace.source_path;
	metadata["initial_sample_point"] = "after_load_game";
	metadata["tick_sample_point"] = "after_state_game_loop";
	metadata["max_ticks"] = _openttdrs_airport_fta_trace.max_rows;
	_openttdrs_airport_fta_trace.out << metadata.dump() << '\n';
	_openttdrs_airport_fta_trace.out.flush();
	WriteAirportFtaTraceRow("initial");
}

void OpenttdrsMaybeExportAirportFtaTraceTick()
{
	if (!_openttdrs_airport_fta_trace.armed) return;

	WriteAirportFtaTraceRow("tick");
	_openttdrs_airport_fta_trace.rows++;
	if (_openttdrs_airport_fta_trace.rows >= _openttdrs_airport_fta_trace.max_rows) {
		_openttdrs_airport_fta_trace.armed = false;
		_openttdrs_airport_fta_trace.out.close();
		_exit_game = true;
	}
}
