/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file linkgraph_parity_fixtures.cpp Oracle dumps for openttdrs linkgraph_parity fixtures (#102). */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../linkgraph/demands.h"
#include "../linkgraph/flowmapper.h"
#include "../linkgraph/init.h"
#include "../linkgraph/linkgraph.h"
#include "../linkgraph/linkgraphjob.h"
#include "../linkgraph/linkgraphschedule.h"
#include "../linkgraph/linkgraph_type.h"
#include "../linkgraph/mcf.h"
#include "../map_func.h"
#include "../settings_type.h"
#include "../station_base.h"
#include "../timer/timer_game_economy.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

#include "../safeguards.h"

namespace {

struct FixtureNode {
	uint32_t station;
	uint32_t x;
	uint32_t y;
	uint32_t supply;
	uint32_t demand;
};

struct FixtureEdge {
	NodeID from;
	NodeID to;
	uint32_t capacity;
	uint32_t usage;
	uint32_t travel_time;
};

struct FixtureDemand {
	NodeID from;
	NodeID to;
	uint32_t demand;
};

struct FixtureFlow {
	uint32_t at;
	uint32_t origin;
	uint32_t via;
	uint32_t amount;
};

struct Fixture {
	const char *name;
	DistributionType distribution;
	uint8_t accuracy;
	uint8_t demand_size;
	uint8_t demand_distance;
	uint8_t short_path_saturation;
	uint32_t runtime;
	std::vector<FixtureNode> nodes;
	std::vector<FixtureEdge> edges;
};

bool DumpMode()
{
	const char *env = std::getenv("OPENTTD_DUMP_LINKGRAPH");
	return env != nullptr && env[0] != '\0' && env[0] != '0';
}

void ApplySettings(const Fixture &fx)
{
	auto &lg = _settings_game.linkgraph;
	lg.accuracy = fx.accuracy;
	lg.demand_size = fx.demand_size;
	lg.demand_distance = fx.demand_distance;
	lg.short_path_saturation = fx.short_path_saturation;
	/* Seconds (modern saves). 30 days * SECONDS_PER_DAY keeps join math sane. */
	lg.recalc_time = static_cast<uint16_t>(30 * CalendarTime::SECONDS_PER_DAY);
	lg.recalc_interval = lg.recalc_time;
	lg.distribution_pax = fx.distribution;
	lg.distribution_mail = fx.distribution;
	lg.distribution_armoured = fx.distribution;
	lg.distribution_default = fx.distribution;
}

LinkGraph *BuildGraph(const Fixture &fx)
{
	/* date=0 at construction → last_compression=0; then date=runtime-1 ⇒ scale runtime. */
	TimerGameEconomy::date = TimerGameEconomy::Date{0};
	REQUIRE(LinkGraph::CanAllocateItem());
	LinkGraph *graph = LinkGraph::Create(CargoType{1}); /* freight-ish index; all dist_* set equally */
	graph->Init(static_cast<uint>(fx.nodes.size()));
	for (size_t i = 0; i < fx.nodes.size(); ++i) {
		const auto &n = fx.nodes[i];
		auto &node = (*graph)[static_cast<NodeID>(i)];
		node.xy = TileXY(n.x, n.y);
		node.station = StationID(static_cast<uint16_t>(n.station));
		node.supply = n.supply;
		node.demand = n.demand;
	}
	for (const auto &e : fx.edges) {
		(*graph)[e.from].AddEdge(e.to, e.capacity, e.usage, e.travel_time, EdgeUpdateMode::Unrestricted);
	}
	TimerGameEconomy::date = TimerGameEconomy::Date{static_cast<int32_t>(fx.runtime) - 1};
	return graph;
}

struct ShareRow {
	uint32_t at;
	uint32_t origin;
	uint32_t via;
	uint32_t amount;
};

std::vector<FixtureDemand> CollectDemands(LinkGraphJob &job)
{
	std::vector<FixtureDemand> out;
	for (NodeID from = 0; from < job.Size(); ++from) {
		for (NodeID to = 0; to < job.Size(); ++to) {
			uint d = job[from].DemandTo(to);
			if (d == 0) continue;
			out.push_back({from, to, d});
		}
	}
	return out;
}

std::vector<ShareRow> CollectFlows(LinkGraphJob &job)
{
	std::vector<ShareRow> out;
	for (NodeID node_id = 0; node_id < job.Size(); ++node_id) {
		uint32_t at = job[node_id].base.station.base();
		for (const auto &origin_it : job[node_id].flows) {
			uint32_t origin = origin_it.first.base();
			const FlowStat &fs = origin_it.second;
			uint previous = 0;
			for (const auto &share_it : *fs.GetShares()) {
				uint amount = share_it.first - previous;
				previous = share_it.first;
				uint32_t via = share_it.second.base();
				if (amount == 0 || share_it.second == StationID::Invalid()) continue;
				out.push_back({at, origin, via, amount});
			}
		}
	}
	std::sort(out.begin(), out.end(), [](const ShareRow &a, const ShareRow &b) {
		return std::tie(a.at, a.origin, a.via, a.amount) < std::tie(b.at, b.origin, b.via, b.amount);
	});
	return out;
}

void PrintJsonDump(const Fixture &fx, const std::vector<FixtureDemand> &demands, const std::vector<ShareRow> &flows)
{
	std::cout << "===DUMP " << fx.name << "===\n";
	std::cout << "{\n";
	std::cout << "  \"name\": \"" << fx.name << "\",\n";
	std::cout << "  \"settings\": {\n";
	std::cout << "    \"accuracy\": " << unsigned(fx.accuracy) << ",\n";
	std::cout << "    \"demand_size\": " << unsigned(fx.demand_size) << ",\n";
	std::cout << "    \"demand_distance\": " << unsigned(fx.demand_distance) << ",\n";
	std::cout << "    \"short_path_saturation\": " << unsigned(fx.short_path_saturation) << ",\n";
	std::cout << "    \"distribution\": \"" << (fx.distribution == DistributionType::Symmetric ? "symmetric" : "asymmetric") << "\",\n";
	std::cout << "    \"map_max_x\": 255,\n";
	std::cout << "    \"map_max_y\": 255,\n";
	std::cout << "    \"runtime\": " << fx.runtime << "\n";
	std::cout << "  },\n";
	std::cout << "  \"nodes\": [\n";
	for (size_t i = 0; i < fx.nodes.size(); ++i) {
		const auto &n = fx.nodes[i];
		std::cout << "    { \"station\": " << n.station << ", \"x\": " << n.x << ", \"y\": " << n.y
		          << ", \"supply\": " << n.supply << ", \"demand\": " << n.demand << " }"
		          << (i + 1 < fx.nodes.size() ? "," : "") << "\n";
	}
	std::cout << "  ],\n";
	std::cout << "  \"edges\": [\n";
	for (size_t i = 0; i < fx.edges.size(); ++i) {
		const auto &e = fx.edges[i];
		std::cout << "    { \"from\": " << e.from << ", \"to\": " << e.to
		          << ", \"capacity\": " << e.capacity << ", \"usage\": " << e.usage
		          << ", \"travel_time\": " << e.travel_time << " }"
		          << (i + 1 < fx.edges.size() ? "," : "") << "\n";
	}
	std::cout << "  ],\n";
	std::cout << "  \"expected_demand\": [\n";
	for (size_t i = 0; i < demands.size(); ++i) {
		const auto &d = demands[i];
		std::cout << "    { \"from\": " << d.from << ", \"to\": " << d.to << ", \"demand\": " << d.demand << " }"
		          << (i + 1 < demands.size() ? "," : "") << "\n";
	}
	std::cout << "  ],\n";
	std::cout << "  \"expected_flows\": [\n";
	for (size_t i = 0; i < flows.size(); ++i) {
		const auto &f = flows[i];
		std::cout << "    { \"at\": " << f.at << ", \"origin\": " << f.origin << ", \"via\": " << f.via
		          << ", \"amount\": " << f.amount << " }"
		          << (i + 1 < flows.size() ? "," : "") << "\n";
	}
	std::cout << "  ],\n";
	std::cout << "  \"notes\": \"Golden dump from OpenTTD Catch2 linkgraph_parity_fixtures (OPENTTD_DUMP_LINKGRAPH).\"\n";
	std::cout << "}\n";
	std::cout << "===END " << fx.name << "===\n";
}

void RunFixture(const Fixture &fx)
{
	Map::Allocate(256, 256);
	ApplySettings(fx);
	LinkGraph *graph = BuildGraph(fx);
	REQUIRE(LinkGraphJob::CanAllocateItem());
	LinkGraphJob *job = LinkGraphJob::Create(*graph);
	if (std::string(fx.name) == "express_vs_local" && DumpMode()) {
		InitHandler().Run(*job);
		DemandHandler().Run(*job);
		std::cout << "AFTER_DEMAND unsatisfied=" << (*job)[0].UnsatisfiedDemandTo(2) << " demand=" << (*job)[0].DemandTo(2) << "\n";
		MCFHandler<MCF1stPass>().Run(*job);
		std::cout << "AFTER_MCF1 flows:";
		for (NodeID f = 0; f < job->Size(); ++f) {
			for (auto &e : (*job)[f].edges) {
				std::cout << " " << f << "->" << e.base.dest_node << "=" << e.Flow();
			}
		}
		std::cout << " unsat=" << (*job)[0].UnsatisfiedDemandTo(2) << "\n";
		FlowMapper(false).Run(*job);
		MCFHandler<MCF2ndPass>().Run(*job);
		std::cout << "AFTER_MCF2 flows:";
		for (NodeID f = 0; f < job->Size(); ++f) {
			for (auto &e : (*job)[f].edges) {
				std::cout << " " << f << "->" << e.base.dest_node << "=" << e.Flow();
			}
		}
		std::cout << " unsat=" << (*job)[0].UnsatisfiedDemandTo(2) << "\n";
		FlowMapper(true).Run(*job);
	} else {
		LinkGraphSchedule::Run(job);
	}

	auto demands = CollectDemands(*job);
	auto flows = CollectFlows(*job);

	if (DumpMode()) {
		PrintJsonDump(fx, demands, flows);
	}

	/* Always assert non-empty pipeline for non-manual fixtures. */
	REQUIRE_FALSE(demands.empty());
	REQUIRE_FALSE(flows.empty());

	/* Store last dump paths via INFO for ctest -V. */
	INFO("fixture=" << fx.name << " demands=" << demands.size() << " flows=" << flows.size());
}

const std::vector<Fixture> &AllFixtures()
{
	static const std::vector<Fixture> fixtures = {
		{
			"asymmetric_two_node",
			DistributionType::Asymmetric,
			16, 100, 100, 80, 30,
			{
				{0, 10, 10, 100, 0},
				{1, 20, 20, 0, 8},
			},
			{
				{0, 1, 50, 0, 0},
			},
		},
		{
			"symmetric_two_node",
			DistributionType::Symmetric,
			16, 100, 100, 80, 30,
			{
				{0, 10, 10, 50, 8},
				{1, 20, 20, 50, 8},
			},
			{
				{0, 1, 40, 0, 0},
				{1, 0, 40, 0, 0},
			},
		},
		{
			"three_node_linear",
			DistributionType::Asymmetric,
			16, 100, 100, 80, 30,
			{
				{0, 10, 10, 80, 0},
				{1, 20, 10, 0, 0},
				{2, 30, 10, 0, 8},
			},
			{
				{0, 1, 40, 0, 0},
				{1, 2, 40, 0, 0},
			},
		},
		{
			"three_node_cycle",
			DistributionType::Asymmetric,
			16, 100, 100, 80, 30,
			{
				{0, 10, 10, 60, 4},
				{1, 30, 10, 60, 4},
				{2, 20, 30, 60, 4},
			},
			{
				{0, 1, 30, 0, 0},
				{1, 2, 30, 0, 0},
				{2, 0, 30, 0, 0},
			},
		},
		{
			"express_vs_local",
			DistributionType::Asymmetric,
			16, 100, 100, 80, 30,
			{
				{0, 10, 10, 100, 0},
				{1, 20, 20, 0, 0},
				{2, 40, 10, 0, 8},
			},
			{
				{0, 1, 50, 0, 200},
				{1, 2, 50, 0, 200},
				{0, 2, 20, 0, 50},
			},
		},
	};
	return fixtures;
}

} // namespace

TEST_CASE("LinkGraphParityFixtures", "[linkgraph][parity]")
{
	for (const auto &fx : AllFixtures()) {
		SECTION(fx.name)
		{
			RunFixture(fx);
		}
	}
}
