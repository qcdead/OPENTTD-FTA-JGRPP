/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file game_info.cpp Functions to convert NetworkGameInfo to Packet and back.
 */

#include "../../stdafx.h"
#include "network_game_info.h"
#include "../../core/bitmath_func.hpp"
#include "../../company_base.h"
#include "../../date_func.h"
#include "../../debug.h"
#include "../../map_func.h"
#include "../../game/game.hpp"
#include "../../game/game_info.hpp"
#include "../../settings_type.h"
#include "../../string_func.h"
#include "../../rev.h"
#include "../../core/format.hpp"
#include "../network_func.h"
#include "../network.h"
#include "../network_internal.h"
#include "packet.h"

#include "../../safeguards.h"

extern const uint8_t _out_of_band_grf_md5[16];

/**
 * How many hex digits of the git hash to include in network revision string.
 * Determined as 10 hex digits + 2 characters for -g/-u/-m prefix.
 */
static const uint GITHASH_SUFFIX_LEN = 12;

NetworkServerGameInfo _network_game_info; ///< Information about our game.

/**
 * Get the network version string used by this build.
 * The returned string is guaranteed to be at most NETWORK_REVISON_LENGTH bytes.
 */
std::string_view GetNetworkRevisionString()
{
	static std::string network_revision;

	if (network_revision.empty()) {
#if !defined(NETWORK_INTERNAL_H)
#	error("network_internal.h must be included, otherwise the debug related preprocessor tokens won't be picked up correctly.")
#elif !defined(ENABLE_NETWORK_SYNC_EVERY_FRAME)
		/* Just a standard build. */
		network_revision = _openttd_revision;
#else
		/* Build for debugging that sends the first part of the seed every frame, practically syncing every frame. */
		network_revision = fmt::format("dbg_sync-{}", _openttd_revision);
#endif
		if (_openttd_revision_tagged) {
			/* Tagged; do not mangle further, though ensure it's not too long. */
			if (network_revision.size() >= NETWORK_REVISION_LENGTH) network_revision.resize(NETWORK_REVISION_LENGTH - 1);
		} else {
			/* Not tagged; add the githash suffix while ensuring the string does not become too long. */
			assert(_openttd_revision_modified < 3);
			std::string githash_suffix = fmt::format("-{}{}", "gum"[_openttd_revision_modified], _openttd_revision_hash);
			if (githash_suffix.size() > GITHASH_SUFFIX_LEN) githash_suffix.resize(GITHASH_SUFFIX_LEN);

			/* Where did the hash start in the original string? Overwrite from that position, unless that would create a too long string. */
			size_t hash_end = network_revision.find_last_of('-');
			if (hash_end == std::string::npos) hash_end = network_revision.size();
			if (hash_end + githash_suffix.size() >= NETWORK_REVISION_LENGTH) hash_end = NETWORK_REVISION_LENGTH - githash_suffix.size() - 1;

			/* Replace the git hash in revision string. */
			network_revision.replace(hash_end, std::string::npos, githash_suffix);
		}
		assert(network_revision.size() < NETWORK_REVISION_LENGTH); // size does not include terminator, constant does, hence strictly less than
		Debug(net, 3, "Network revision name: {}", network_revision);
	}

	return network_revision;
}

/**
 * Extract the git hash from the revision string.
 * @param revision_string The revision string (formatted as DATE-BRANCH-GITHASH).
 * @return The git has part of the revision.
 */
static std::string_view ExtractNetworkRevisionHash(std::string_view revision_string)
{
	size_t index = revision_string.find_last_of('-');
	if (index == std::string::npos) return {};
	return revision_string.substr(index);
}

/**
 * Checks whether the given version string is compatible with our version.
 * First tries to match the full string, if that fails, attempts to compare just git hashes.
 * @param other the version string to compare to
 */
bool IsNetworkCompatibleVersion(std::string_view other, bool extended)
{
	std::string_view our_revision = GetNetworkRevisionString();
	if (our_revision == other) return true;

	/* If this version is tagged, then the revision string must be a complete match,
	 * since there is no git hash suffix in it.
	 * This is needed to avoid situations like "1.9.0-beta1" comparing equal to "2.0.0-beta1".  */
	if (_openttd_revision_tagged) return false;

	/* One of the versions is for some sort of debugging, but not both. */
	if (other.starts_with("dbg_seed") != our_revision.starts_with("dbg_seed")) return false;
	if (other.starts_with("dbg_sync") != our_revision.starts_with("dbg_sync")) return false;

	std::string_view hash1 = ExtractNetworkRevisionHash(our_revision);
	std::string_view hash2 = ExtractNetworkRevisionHash(other);
	return hash1 == hash2;
}

/**
 * Check if an game entry is compatible with our client.
 */
void CheckGameCompatibility(NetworkGameInfo &ngi, bool extended)
{
	/* Check if we are allowed on this server based on the revision-check. */
	ngi.version_compatible = IsNetworkCompatibleVersion(ngi.server_revision, extended);
	ngi.compatible = ngi.version_compatible;

	/* Check if we have all the GRFs on the client-system too. */
	for (const auto &c : ngi.grfconfig) {
		if (c->status == GCS_NOT_FOUND) ngi.compatible = false;
	}
}

/**
 * Fill a NetworkServerGameInfo structure with the static content, or things
 * that are so static they can be updated on request from a settings change.
 */
void FillStaticNetworkServerGameInfo()
{
	_network_game_info.use_password   = !_settings_client.network.server_password.empty();
	_network_game_info.calendar_start = CalTime::ConvertYMDToDate(_settings_game.game_creation.starting_year, 0, 1);
	_network_game_info.clients_max    = _settings_client.network.max_clients;
	_network_game_info.companies_max  = _settings_client.network.max_companies;
	_network_game_info.map_width      = Map::SizeX();
	_network_game_info.map_height     = Map::SizeY();
	_network_game_info.landscape      = _settings_game.game_creation.landscape;
	_network_game_info.dedicated      = _network_dedicated;
	CopyGRFConfigList(_network_game_info.grfconfig, _grfconfig, false);

	_network_game_info.server_name = _settings_client.network.server_name;
	_network_game_info.server_revision = GetNetworkRevisionString();
}

/**
 * Get the NetworkServerGameInfo structure with the latest information of the server.
 * @return The current NetworkServerGameInfo.
 */
const NetworkServerGameInfo &GetCurrentNetworkServerGameInfo()
{
	/* These variables are updated inside _network_game_info as if they are global variables:
	 *  - clients_on
	 *  - invite_code
	 * These don't need to be updated manually here.
	 */
	_network_game_info.companies_on  = (uint8_t)Company::GetNumItems();
	_network_game_info.spectators_on = NetworkSpectatorCount();
	_network_game_info.calendar_date = CalTime::CurDate();
	_network_game_info.ticks_playing = _scaled_tick_counter;
	return _network_game_info;
}

/**
 * Function that is called for every GRFConfig that is read when receiving
 * a NetworkGameInfo. Only grfid and md5sum are set, the rest is zero. This
 * function must set all appropriate fields. This GRF is later appended to
 * the grfconfig list of the NetworkGameInfo.
 * @param config The GRF to handle.
 * @param name The name of the NewGRF, empty when unknown.
 */
static void HandleIncomingNetworkGameInfoGRFConfig(GRFConfig &config, std::string_view name)
{
	/* Find the matching GRF file */
	const GRFConfig *f = FindGRFConfig(config.ident.grfid, FGCM_EXACT, &config.ident.md5sum);
	if (f == nullptr) {
		AddGRFTextToList(config.name, name.empty() ? GetString(STR_CONFIG_ERROR_INVALID_GRF_UNKNOWN) : name);
		config.status = GCS_NOT_FOUND;
	} else {
		config.filename = f->filename;
		config.name = f->name;
		config.info = f->info;
		config.url = f->url;
	}
	config.flags.Set(GRFConfigFlag::Copy);
}

/**
 * Serializes the NetworkGameInfo struct to the packet.
 * @param p    the packet to write the data to.
 * @param info the NetworkGameInfo struct to serialize from.
 */
void SerializeNetworkGameInfo(Packet &p, const NetworkServerGameInfo &info, bool send_newgrf_names)
{
	p.Send_uint8 (NETWORK_GAME_INFO_VERSION);

	/*
	 *              Please observe the order.
	 * The parts must be read in the same order as they are sent!
	 */

	/* Update the documentation in game_info.h on changes
	 * to the NetworkGameInfo wire-protocol! */

	/* NETWORK_GAME_INFO_VERSION = 7 */
	p.Send_uint64(info.ticks_playing);

	/* NETWORK_GAME_INFO_VERSION = 6 */
	p.Send_uint8(send_newgrf_names ? NST_GRFID_MD5_NAME : NST_GRFID_MD5);

	/* NETWORK_GAME_INFO_VERSION = 5 */
	GameInfo *game_info = Game::GetInfo();
	p.Send_uint32(game_info == nullptr ? -1 : (uint32_t)game_info->GetVersion());
	p.Send_string(game_info == nullptr ? "" : game_info->GetName());

	/* NETWORK_GAME_INFO_VERSION = 4 */
	{
		/* Only send the GRF Identification (GRF_ID and MD5 checksum) of
		 * the GRFs that are needed, i.e. the ones that the server has
		 * selected in the NewGRF GUI and not the ones that are used due
		 * to the fact that they are in [newgrf-static] in openttd.cfg */
		uint count = std::ranges::count_if(info.grfconfig, [](const auto &c) { return !c->flags.Test(GRFConfigFlag::Static); });
		p.Send_uint8(ClampTo<uint8_t>(std::min<uint>(count, NETWORK_MAX_GRF_COUNT))); // Send number of GRFs

		/* Send actual GRF Identifications */
		for (const auto &c : info.grfconfig) {
			if (c->flags.Test(GRFConfigFlag::Static)) continue;

			SerializeGRFIdentifier(p, c->ident);
			if (send_newgrf_names) p.Send_string(c->GetName());
		}
	}

	/* NETWORK_GAME_INFO_VERSION = 3 */
	p.Send_uint32(info.calendar_date.base());
	p.Send_uint32(info.calendar_start.base());

	/* NETWORK_GAME_INFO_VERSION = 2 */
	p.Send_uint16 (info.companies_max);
	p.Send_uint16 (info.companies_on);
	p.Send_uint16 (info.clients_max); // Used to be max-spectators

	/* NETWORK_GAME_INFO_VERSION = 1 */
	p.Send_string(info.server_name);
	p.Send_string(info.server_revision);
	p.Send_bool  (info.use_password);
	p.Send_uint16 (info.clients_max);
	p.Send_uint16 (info.clients_on);
	p.Send_uint16 (info.spectators_on);

	auto encode_map_size = [&](uint32_t in) -> uint16_t {
		if (in < UINT16_MAX) {
			return in;
		} else {
			return 65000 + FindFirstBit(in);
		}
	};
	p.Send_uint16(encode_map_size(info.map_width));
	p.Send_uint16(encode_map_size(info.map_height));
	p.Send_uint8 (to_underlying(info.landscape));
	p.Send_bool  (info.dedicated);
}

/**
 * Serializes the NetworkGameInfo struct to the packet
 * @param p    the packet to write the data to
 * @param info the NetworkGameInfo struct to serialize
 */
void SerializeNetworkGameInfoExtended(Packet &p, const NetworkServerGameInfo &info, uint16_t flags, uint16_t version, bool send_newgrf_names)
{
	version = std::min<uint16_t>(version, 2); // Version 2 is the max supported

	p.Send_uint8(version); // version num

	p.Send_uint32(info.calendar_date.base());
	p.Send_uint32(info.calendar_start.base());
	p.Send_uint16 (info.companies_max);
	p.Send_uint16 (info.companies_on);
	p.Send_uint16 (info.clients_max); // Used to be max-spectators
	p.Send_string(info.server_name);
	p.Send_string(info.server_revision);
	p.Send_uint8 (0); // Used to be server-lang.
	p.Send_bool  (info.use_password);
	p.Send_uint16 (info.clients_max);
	p.Send_uint16 (info.clients_on);
	p.Send_uint16 (info.spectators_on);
	p.Send_string(""); // Used to be map-name.
	p.Send_uint32(info.map_width);
	p.Send_uint32(info.map_height);
	p.Send_uint8 (to_underlying(info.landscape));
	p.Send_bool  (info.dedicated);

	if (version >= 1) {
		GameInfo *game_info = Game::GetInfo();
		p.Send_uint32(game_info == nullptr ? -1 : (uint32_t)game_info->GetVersion());
		p.Send_string(game_info == nullptr ? "" : game_info->GetName());

		p.Send_uint8(send_newgrf_names ? NST_GRFID_MD5_NAME : NST_GRFID_MD5);
	}

	if (version >= 2) {
		p.Send_uint64(info.ticks_playing);
	}

	{
		/* Only send the GRF Identification (GRF_ID and MD5 checksum) of
		 * the GRFs that are needed, i.e. the ones that the server has
		 * selected in the NewGRF GUI and not the ones that are used due
		 * to the fact that they are in [newgrf-static] in openttd.cfg */
		uint count = 0;

		/* Count number of GRFs to send information about */
		for (const auto &c : info.grfconfig) {
			if (!c->flags.Test(GRFConfigFlag::Static)) count++;
		}
		p.Send_uint32(count); // Send number of GRFs

		/* Send actual GRF Identifications */
		for (const auto &c : info.grfconfig) {
			if (c->flags.Test(GRFConfigFlag::Static)) continue;

			SerializeGRFIdentifier(p, c->ident);
			if (send_newgrf_names && version >= 1) p.Send_string(c->GetName());
		}
	}
}

/**
 * Deserializes the NetworkGameInfo struct from the packet.
 * @param p    the packet to read the data from.
 * @param info the NetworkGameInfo to deserialize into.
 */
void DeserializeNetworkGameInfo(Packet &p, NetworkGameInfo &info, const GameInfoNewGRFLookupTable *newgrf_lookup_table)
{
	uint8_t game_info_version = p.Recv_uint8();
	NewGRFSerializationType newgrf_serialisation = NST_GRFID_MD5;

	/*
	 *              Please observe the order.
	 * The parts must be read in the same order as they are sent!
	 */

	/* Update the documentation in game_info.h on changes
	 * to the NetworkGameInfo wire-protocol! */

	switch (game_info_version) {
		case 7:
			info.ticks_playing = p.Recv_uint64();
			[[fallthrough]];

		case 6:
			newgrf_serialisation = (NewGRFSerializationType)p.Recv_uint8();
			if (newgrf_serialisation >= NST_END) return;
			[[fallthrough]];

		case 5: {
			info.gamescript_version = (int)p.Recv_uint32();
			info.gamescript_name = p.Recv_string(NETWORK_NAME_LENGTH);
			[[fallthrough]];
		}

		case 4: {
			/* Ensure that the maximum number of NewGRFs and the field in the network
			 * protocol are matched to each other. If that is not the case anymore a
			 * check must be added to ensure the received data is still valid. */
			static_assert(std::numeric_limits<uint8_t>::max() == NETWORK_MAX_GRF_COUNT);
			uint num_grfs = p.Recv_uint8();

			GRFConfigList &dst = info.grfconfig;
			for (uint i = 0; i < num_grfs; i++) {
				NamedGRFIdentifier grf;
				switch (newgrf_serialisation) {
					case NST_GRFID_MD5:
						DeserializeGRFIdentifier(p, grf.ident);
						break;

					case NST_GRFID_MD5_NAME:
						DeserializeGRFIdentifierWithName(p, grf);
						break;

					case NST_LOOKUP_ID: {
						if (newgrf_lookup_table == nullptr) return;
						auto it = newgrf_lookup_table->find(p.Recv_uint32());
						if (it == newgrf_lookup_table->end()) return;
						grf = it->second;
						break;
					}

					default:
						NOT_REACHED();
				}

				auto c = std::make_unique<GRFConfig>();
				c->ident = grf.ident;
				HandleIncomingNetworkGameInfoGRFConfig(*c, grf.name);

				/* Append GRFConfig to the list */
				dst.push_back(std::move(c));
			}
			[[fallthrough]];
		}

		case 3:
			info.calendar_date  = CalTime::DeserialiseDateClamped(p.Recv_uint32());
			info.calendar_start = CalTime::DeserialiseDateClamped(p.Recv_uint32());
			[[fallthrough]];

		case 2:
			info.companies_max  = p.Recv_uint16 ();
			info.companies_on   = p.Recv_uint16 ();
			p.Recv_uint16(); // Used to contain max-spectators.
			[[fallthrough]];

		case 1:
			info.server_name = p.Recv_string(NETWORK_NAME_LENGTH);
			info.server_revision = p.Recv_string(NETWORK_REVISION_LENGTH);
			if (game_info_version < 6) p.Recv_uint8 (); // Used to contain server-lang.
			info.use_password   = p.Recv_bool  ();
			info.clients_max    = p.Recv_uint8 ();
			info.clients_on     = p.Recv_uint8 ();
			info.spectators_on  = p.Recv_uint8 ();
			if (game_info_version < 3) { // 16 bits dates got scrapped and are read earlier
				info.calendar_date  = CalTime::DAYS_TILL_ORIGINAL_BASE_YEAR + p.Recv_uint16();
				info.calendar_start = CalTime::DAYS_TILL_ORIGINAL_BASE_YEAR + p.Recv_uint16();
			}
			if (game_info_version < 6) while (p.Recv_uint8() != 0) {} // Used to contain the map-name.

			auto decode_map_size = [&](uint16_t in) -> uint32_t {
				if (in >= 65000) {
					return 1 << (in - 65000);
				} else {
					return in;
				}
			};
			info.map_width      = decode_map_size(p.Recv_uint16());
			info.map_height     = decode_map_size(p.Recv_uint16());

			info.landscape      = LandscapeType{p.Recv_uint8()};
			info.dedicated      = p.Recv_bool  ();

			if (to_underlying(info.landscape) >= NUM_LANDSCAPE) info.landscape = LandscapeType::Temperate;
	}
}

/**
 * Deserializes the NetworkGameInfo struct from the packet
 * @param p    the packet to read the data from
 * @param info the NetworkGameInfo to deserialize into
 */
void DeserializeNetworkGameInfoExtended(Packet &p, NetworkGameInfo &info)
{
	const uint8_t version = p.Recv_uint8();
	if (version > SERVER_GAME_INFO_EXTENDED_MAX_VERSION) return; // Unknown version

	NewGRFSerializationType newgrf_serialisation = NST_GRFID_MD5;

	info.calendar_date  = CalTime::DeserialiseDateClamped(p.Recv_uint32());
	info.calendar_start = CalTime::DeserialiseDateClamped(p.Recv_uint32());
	info.companies_max  = p.Recv_uint8 ();
	info.companies_on   = p.Recv_uint8 ();
	p.Recv_uint8(); // Used to contain max-spectators.
	info.server_name = p.Recv_string(NETWORK_NAME_LENGTH);
	info.server_revision = p.Recv_string(NETWORK_LONG_REVISION_LENGTH);
	p.Recv_uint8 (); // Used to contain server-lang.
	info.use_password   = p.Recv_bool  ();
	info.clients_max    = p.Recv_uint8 ();
	info.clients_on     = p.Recv_uint8 ();
	info.spectators_on  = p.Recv_uint8 ();
	while (p.Recv_uint8() != 0) {} // Used to contain the map-name.
	info.map_width      = p.Recv_uint32();
	info.map_height     = p.Recv_uint32();
	info.landscape      = LandscapeType{p.Recv_uint8()};
	if (to_underlying(info.landscape) >= NUM_LANDSCAPE) info.landscape = LandscapeType::Temperate;
	info.dedicated      = p.Recv_bool  ();

	if (version >= 1) {
		info.gamescript_version = (int)p.Recv_uint32();
		info.gamescript_name = p.Recv_string(NETWORK_NAME_LENGTH);

		newgrf_serialisation = (NewGRFSerializationType)p.Recv_uint8();
		if (newgrf_serialisation >= NST_END) return;
	}

	if (version >= 2) {
		info.ticks_playing = p.Recv_uint64();
	}

	{
		GRFConfigList &dst = info.grfconfig;
		uint num_grfs = p.Recv_uint32();

		/* Broken/bad data. It cannot have that many NewGRFs. */
		if (num_grfs > MAX_NON_STATIC_GRF_COUNT) return;

		for (uint i = 0; i < num_grfs; i++) {
			NamedGRFIdentifier grf;
			switch (newgrf_serialisation) {
				case NST_GRFID_MD5:
					DeserializeGRFIdentifier(p, grf.ident);
					break;

				case NST_GRFID_MD5_NAME:
					DeserializeGRFIdentifierWithName(p, grf);
					break;

				case NST_LOOKUP_ID: {
					Debug(net, 0, "Unexpected NST_LOOKUP_ID in DeserializeNetworkGameInfoExtended");
					return;
				}

				default:
					NOT_REACHED();
			}

			auto c = std::make_unique<GRFConfig>();
			c->ident = grf.ident;
			HandleIncomingNetworkGameInfoGRFConfig(*c, grf.name);

			/* Append GRFConfig to the list */
			dst.push_back(std::move(c));
		}
	}
}

/**
 * Serializes the GRFIdentifier (GRF ID and MD5 checksum) to the packet
 * @param p    the packet to write the data to.
 * @param grf  the GRFIdentifier to serialize.
 */
void SerializeGRFIdentifier(Packet &p, const GRFIdentifier &grf)
{
	p.Send_uint32(grf.grfid);
	p.Send_bytes(grf.md5sum);
}

/**
 * Deserializes the GRFIdentifier (GRF ID and MD5 checksum) from the packet
 * @param p    the packet to read the data from.
 * @param grf  the GRFIdentifier to deserialize.
 */
void DeserializeGRFIdentifier(Packet &p, GRFIdentifier &grf)
{
	grf.grfid = p.Recv_uint32();
	p.Recv_bytes(grf.md5sum);
}

/**
 * Deserializes the NamedGRFIdentifier (GRF ID, MD5 checksum and name) from the packet
 * @param p    the packet to read the data from.
 * @param grf  the NamedGRFIdentifier to deserialize.
 */
void DeserializeGRFIdentifierWithName(Packet &p, NamedGRFIdentifier &grf)
{
	DeserializeGRFIdentifier(p, grf.ident);
	grf.name = p.Recv_string(NETWORK_GRF_NAME_LENGTH);
}
