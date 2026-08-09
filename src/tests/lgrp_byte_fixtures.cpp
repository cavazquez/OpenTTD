/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file lgrp_byte_fixtures.cpp Oracle dumps of LGRP wire bytes for openttdrs (#102). */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../linkgraph/linkgraph.h"
#include "../map_func.h"
#include "../timer/timer_game_economy.h"

#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <vector>

#include "../safeguards.h"

namespace {

bool DumpMode()
{
	const char *env = std::getenv("OPENTTD_DUMP_LGRP");
	return env != nullptr && env[0] != '\0' && env[0] != '0';
}

void WriteGamma(uint32_t v, std::vector<uint8_t> &buf)
{
	REQUIRE(v < (1u << 14));
	if (v < (1u << 7)) {
		buf.push_back(static_cast<uint8_t>(v));
	} else {
		buf.push_back(static_cast<uint8_t>(0x80 | (v >> 8)));
		buf.push_back(static_cast<uint8_t>(v & 0xFF));
	}
}

void WriteStr(const char *s, std::vector<uint8_t> &buf)
{
	WriteGamma(static_cast<uint32_t>(strlen(s)), buf);
	for (const char *p = s; *p != '\0'; ++p) buf.push_back(static_cast<uint8_t>(*p));
}

void WriteBe32(uint32_t v, std::vector<uint8_t> &buf)
{
	buf.push_back(static_cast<uint8_t>(v >> 24));
	buf.push_back(static_cast<uint8_t>(v >> 16));
	buf.push_back(static_cast<uint8_t>(v >> 8));
	buf.push_back(static_cast<uint8_t>(v));
}

void WriteBe16(uint16_t v, std::vector<uint8_t> &buf)
{
	buf.push_back(static_cast<uint8_t>(v >> 8));
	buf.push_back(static_cast<uint8_t>(v));
}

void WriteBe64(uint64_t v, std::vector<uint8_t> &buf)
{
	WriteBe32(static_cast<uint32_t>(v >> 32), buf);
	WriteBe32(static_cast<uint32_t>(v), buf);
}

/** Table header matching GetLinkGraphDesc / SlLinkgraphNode / SlLinkgraphEdge (SLV ≥ 304). */
std::vector<uint8_t> LgrpTableHeader()
{
	std::vector<uint8_t> header;
	header.push_back(5);
	WriteStr("last_compression", header);
	header.push_back(2);
	WriteStr("cargo", header);
	header.push_back(0x1B);
	WriteStr("nodes", header);
	header.push_back(0);
	header.push_back(6);
	WriteStr("xy", header);
	header.push_back(6);
	WriteStr("supply", header);
	header.push_back(6);
	WriteStr("demand", header);
	header.push_back(4);
	WriteStr("station", header);
	header.push_back(5);
	WriteStr("last_update", header);
	header.push_back(0x1B);
	WriteStr("edges", header);
	header.push_back(0);
	header.push_back(6);
	WriteStr("capacity", header);
	header.push_back(6);
	WriteStr("usage", header);
	header.push_back(8);
	WriteStr("travel_time_sum", header);
	header.push_back(5);
	WriteStr("last_unrestricted_update", header);
	header.push_back(5);
	WriteStr("last_restricted_update", header);
	header.push_back(4);
	WriteStr("dest_node", header);
	header.push_back(0);
	return header;
}

std::vector<uint8_t> EncodeLgrpRecord(const LinkGraph &lg)
{
	std::vector<uint8_t> rec;
	WriteBe32(static_cast<uint32_t>(lg.LastCompression().base()), rec);
	rec.push_back(lg.Cargo());
	WriteGamma(lg.Size(), rec);
	for (NodeID from = 0; from < lg.Size(); ++from) {
		const auto &node = lg[from];
		WriteBe32(node.xy.base(), rec);
		WriteBe32(node.supply, rec);
		WriteBe32(node.demand, rec);
		WriteBe16(node.station.base(), rec);
		WriteBe32(static_cast<uint32_t>(node.last_update.base()), rec);
		WriteGamma(static_cast<uint32_t>(node.edges.size()), rec);
		for (const auto &edge : node.edges) {
			WriteBe32(edge.capacity, rec);
			WriteBe32(edge.usage, rec);
			WriteBe64(edge.travel_time_sum, rec);
			WriteBe32(static_cast<uint32_t>(edge.last_unrestricted_update.base()), rec);
			WriteBe32(static_cast<uint32_t>(edge.last_restricted_update.base()), rec);
			WriteBe16(edge.dest_node, rec);
		}
	}
	return rec;
}

std::vector<uint8_t> EncodeLgrpChunk(const std::vector<const LinkGraph *> &graphs)
{
	std::vector<uint8_t> out;
	out.push_back('L');
	out.push_back('G');
	out.push_back('R');
	out.push_back('P');
	out.push_back(3); // CH_TABLE
	auto header = LgrpTableHeader();
	WriteGamma(static_cast<uint32_t>(header.size()) + 1, out);
	out.insert(out.end(), header.begin(), header.end());
	for (const LinkGraph *lg : graphs) {
		auto rec = EncodeLgrpRecord(*lg);
		WriteGamma(static_cast<uint32_t>(rec.size()) + 1, out);
		out.insert(out.end(), rec.begin(), rec.end());
	}
	WriteGamma(0, out);
	return out;
}

void PrintHexDump(const char *name, const std::vector<uint8_t> &bytes)
{
	std::cout << "===DUMP_LGRP " << name << "===\n";
	std::cout << std::hex << std::setfill('0');
	for (size_t i = 0; i < bytes.size(); ++i) {
		if (i && (i % 32) == 0) std::cout << '\n';
		std::cout << std::setw(2) << static_cast<unsigned>(bytes[i]);
	}
	std::cout << std::dec << "\n===END_LGRP " << name << " (" << bytes.size() << " bytes)===\n";
}

LinkGraph *BuildTwoNodeGoods()
{
	TimerGameEconomy::date = TimerGameEconomy::Date{0};
	REQUIRE(LinkGraph::CanAllocateItem());
	/* CT_GOODS temperate index = 5 */
	LinkGraph *lg = LinkGraph::Create(CargoType{5});
	lg->Init(2);
	(*lg)[0].xy = TileXY(10, 10);
	(*lg)[0].station = StationID(0);
	(*lg)[0].supply = 0;
	(*lg)[0].demand = 0;
	(*lg)[1].xy = TileXY(20, 20);
	(*lg)[1].station = StationID(1);
	(*lg)[1].supply = 0;
	(*lg)[1].demand = 0;
	(*lg)[0].AddEdge(1, 50, 7, 120, EdgeUpdateMode::Unrestricted);
	return lg;
}

} // namespace

TEST_CASE("LgrpByteFixtures", "[linkgraph][lgrp]")
{
	Map::Allocate(256, 256);

	SECTION("empty")
	{
		std::vector<const LinkGraph *> none;
		auto bytes = EncodeLgrpChunk(none);
		REQUIRE(bytes.size() > 8);
		REQUIRE(bytes[0] == 'L');
		REQUIRE(bytes[4] == 3);
		if (DumpMode()) PrintHexDump("lgrp_empty", bytes);
	}

	SECTION("two_node_goods")
	{
		LinkGraph *lg = BuildTwoNodeGoods();
		REQUIRE(lg->LastCompression().base() == 0);
		REQUIRE((*lg)[0].edges.size() == 1);
		REQUIRE((*lg)[0].edges[0].travel_time_sum == 50ull * 120ull);
		REQUIRE((*lg)[0].edges[0].last_unrestricted_update.base() == 0);
		REQUIRE((*lg)[0].edges[0].last_restricted_update.base() == -1);
		REQUIRE((*lg)[0].last_update.base() == -1);

		std::vector<const LinkGraph *> graphs{lg};
		auto bytes = EncodeLgrpChunk(graphs);
		if (DumpMode()) PrintHexDump("lgrp_two_node_goods", bytes);
		REQUIRE(bytes.size() > 64);
	}
}
