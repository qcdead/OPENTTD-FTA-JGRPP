/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file newgrf_station.cpp Functions for dealing with station classes and custom stations. */

#include "stdafx.h"
#include "debug.h"
#include "station_base.h"
#include "waypoint_base.h"
#include "roadstop_base.h"
#include "newgrf_badge.h"
#include "newgrf_cargo.h"
#include "newgrf_station.h"
#include "newgrf_spritegroup.h"
#include "newgrf_sound.h"
#include "newgrf_railtype.h"
#include "town.h"
#include "newgrf_town.h"
#include "company_func.h"
#include "tunnelbridge_map.h"
#include "newgrf_animation_base.h"
#include "newgrf_class_func.h"
#include "newgrf_extension.h"
#include "newgrf_dump.h"
#include "core/checksum_func.hpp"

#include "safeguards.h"

uint64_t _station_tile_cache_hash = 0;

template <>
/* static */ void StationClass::InsertDefaults()
{
	/* Set up initial data */
	StationClass::Get(StationClass::Allocate(STATION_CLASS_LABEL_DEFAULT))->name = STR_STATION_CLASS_DFLT;
	StationClass::Get(StationClass::Allocate(STATION_CLASS_LABEL_DEFAULT))->Insert(nullptr);
	StationClass::Get(StationClass::Allocate(STATION_CLASS_LABEL_WAYPOINT))->name = STR_STATION_CLASS_WAYP;
	StationClass::Get(StationClass::Allocate(STATION_CLASS_LABEL_WAYPOINT))->Insert(nullptr);
}

template <>
bool StationClass::IsUIAvailable(uint) const
{
	return true;
}

/* Instantiate StationClass. */
template class NewGRFClass<StationSpec, StationClassID, STAT_CLASS_MAX>;

static const uint NUM_STATIONSSPECS_PER_STATION = 255; ///< Maximum number of parts per station.

enum TriggerArea : uint8_t {
	TA_TILE,
	TA_PLATFORM,
	TA_WHOLE,
};

struct ETileArea : TileArea {
	ETileArea(const BaseStation *st, TileIndex tile, TriggerArea ta)
	{
		switch (ta) {
			default: NOT_REACHED();

			case TA_TILE:
				this->tile = tile;
				this->w    = 1;
				this->h    = 1;
				break;

			case TA_PLATFORM: {
				TileIndex start, end;
				Axis axis = GetRailStationAxis(tile);
				TileIndexDiff delta = TileOffsByAxis(axis);

				for (end = tile; IsRailStationTile(end + delta) && IsCompatibleTrainStationTile(end + delta, tile); end += delta) { /* Nothing */ }
				for (start = tile; IsRailStationTile(start - delta) && IsCompatibleTrainStationTile(start - delta, tile); start -= delta) { /* Nothing */ }

				this->tile = start;
				this->w = TileX(end) - TileX(start) + 1;
				this->h = TileY(end) - TileY(start) + 1;
				break;
			}

			case TA_WHOLE:
				st->GetTileArea(this, Station::IsExpected(st) ? StationType::Rail : StationType::RailWaypoint);
				break;
		}
	}
};


/**
 * Evaluate a tile's position within a station, and return the result in a bit-stuffed format.
 * if not centered: .TNLcCpP, if centered: .TNL..CP
 * - T = Tile layout number (#GetStationGfx)
 * - N = Number of platforms
 * - L = Length of platforms
 * - C = Current platform number from start, c = from end
 * - P = Position along platform from start, p = from end
 * .
 * if centered, C/P start from the centre and c/p are not available.
 * @return Platform information in bit-stuffed format.
 */
uint32_t GetPlatformInfo(Axis axis, uint8_t tile, int platforms, int length, int x, int y, bool centred)
{
	uint32_t retval = 0;

	if (axis == AXIS_X) {
		Swap(platforms, length);
		Swap(x, y);
	}

	if (centred) {
		x -= platforms / 2;
		y -= length / 2;
		x = Clamp(x, -8, 7);
		y = Clamp(y, -8, 7);
		SB(retval,  0, 4, y & 0xF);
		SB(retval,  4, 4, x & 0xF);
	} else {
		SB(retval,  0, 4, std::min(15, y));
		SB(retval,  4, 4, std::min(15, length - y - 1));
		SB(retval,  8, 4, std::min(15, x));
		SB(retval, 12, 4, std::min(15, platforms - x - 1));
	}
	SB(retval, 16, 4, std::min(15, length));
	SB(retval, 20, 4, std::min(15, platforms));
	SB(retval, 24, 8, tile);

	return retval;
}


/**
 * Find the end of a railway station, from the \a tile, in the direction of \a delta.
 * @param tile Start tile.
 * @param delta Movement direction.
 * @param check_type Stop when the custom station type changes.
 * @param check_axis Stop when the station direction changes.
 * @return Found end of the railway station.
 */
static TileIndex FindRailStationEnd(TileIndex tile, TileIndexDiff delta, bool check_type, bool check_axis)
{
	uint8_t orig_type = 0;
	Axis orig_axis = AXIS_X;
	StationID sid = GetStationIndex(tile);

	if (check_type) orig_type = GetCustomStationSpecIndex(tile);
	if (check_axis) orig_axis = GetRailStationAxis(tile);

	for (;;) {
		TileIndex new_tile = TileAdd(tile, delta);

		if (!IsTileType(new_tile, MP_STATION) || GetStationIndex(new_tile) != sid) break;
		if (!HasStationRail(new_tile)) break;
		if (check_type && GetCustomStationSpecIndex(new_tile) != orig_type) break;
		if (check_axis && GetRailStationAxis(new_tile) != orig_axis) break;

		tile = new_tile;
	}
	return tile;
}


static uint32_t GetPlatformInfoHelper(TileIndex tile, bool check_type, bool check_axis, bool centred)
{
	int tx = TileX(tile);
	int ty = TileY(tile);
	int sx = TileX(FindRailStationEnd(tile, TileDiffXY(-1,  0), check_type, check_axis));
	int sy = TileY(FindRailStationEnd(tile, TileDiffXY( 0, -1), check_type, check_axis));
	int ex = TileX(FindRailStationEnd(tile, TileDiffXY( 1,  0), check_type, check_axis)) + 1;
	int ey = TileY(FindRailStationEnd(tile, TileDiffXY( 0,  1), check_type, check_axis)) + 1;

	tx -= sx; ex -= sx;
	ty -= sy; ey -= sy;

	return GetPlatformInfo(GetRailStationAxis(tile), GetStationGfx(tile), ex, ey, tx, ty, centred);
}


static uint32_t GetRailContinuationInfo(TileIndex tile)
{
	/* Tile offsets and exit dirs for X axis */
	static const Direction x_dir[8] = { DIR_SW, DIR_NE, DIR_SE, DIR_NW, DIR_S, DIR_E, DIR_W, DIR_N };
	static const DiagDirection x_exits[8] = { DIAGDIR_SW, DIAGDIR_NE, DIAGDIR_SE, DIAGDIR_NW, DIAGDIR_SW, DIAGDIR_NE, DIAGDIR_SW, DIAGDIR_NE };

	/* Tile offsets and exit dirs for Y axis */
	static const Direction y_dir[8] = { DIR_SE, DIR_NW, DIR_SW, DIR_NE, DIR_S, DIR_W, DIR_E, DIR_N };
	static const DiagDirection y_exits[8] = { DIAGDIR_SE, DIAGDIR_NW, DIAGDIR_SW, DIAGDIR_NE, DIAGDIR_SE, DIAGDIR_NW, DIAGDIR_SE, DIAGDIR_NW };

	Axis axis = GetRailStationAxis(tile);

	/* Choose appropriate lookup table to use */
	const Direction *dir = axis == AXIS_X ? x_dir : y_dir;
	const DiagDirection *diagdir = axis == AXIS_X ? x_exits : y_exits;

	uint32_t res = 0;
	uint i;

	for (i = 0; i < lengthof(x_dir); i++, dir++, diagdir++) {
		TileIndex neighbour_tile = tile + TileOffsByDir(*dir);
		TrackBits trackbits = TrackdirBitsToTrackBits(GetTileTrackdirBits(neighbour_tile, TRANSPORT_RAIL, 0));
		if (trackbits != TRACK_BIT_NONE) {
			/* If there is any track on the tile, set the bit in the second byte */
			SetBit(res, i + 8);

			/* With tunnels and bridges the tile has tracks, but they are not necessarily connected
			 * with the next tile because the ramp is not going in the right direction. */
			if (IsTileType(neighbour_tile, MP_TUNNELBRIDGE) && GetTunnelBridgeDirection(neighbour_tile) == ReverseDiagDir(*diagdir)) {
				continue;
			}

			/* If any track reaches our exit direction, set the bit in the lower byte */
			if (trackbits & DiagdirReachesTracks(*diagdir)) SetBit(res, i);
		}
	}

	return res;
}


/* Station Resolver Functions */
/* virtual */ uint32_t StationScopeResolver::GetRandomBits() const
{
	return (this->st == nullptr ? 0 : this->st->random_bits) | (this->tile == INVALID_TILE ? 0 : GetStationTileRandomBits(this->tile) << 16);
}


/* virtual */ uint32_t StationScopeResolver::GetTriggers() const
{
	return this->st == nullptr ? 0 : this->st->waiting_triggers;
}


/**
 * Station variable cache
 * This caches 'expensive' station variable lookups which iterate over
 * several tiles that may be called multiple times per Resolve().
 */
static struct {
	uint32_t v40;
	uint32_t v41;
	uint32_t v45;
	uint32_t v46;
	uint32_t v47;
	uint32_t v49;
	uint8_t valid; ///< Bits indicating what variable is valid (for each bit, \c 0 is invalid, \c 1 is valid).
} _svc;

/**
 * Get the town scope associated with a station, if it exists.
 * On the first call, the town scope is created (if possible).
 * @return Town scope, if available.
 */
TownScopeResolver *StationResolverObject::GetTown()
{
	if (!this->town_scope.has_value()) {
		Town *t = nullptr;
		if (this->station_scope.st != nullptr) {
			t = this->station_scope.st->town;
		} else if (this->station_scope.tile != INVALID_TILE) {
			t = ClosestTownFromTile(this->station_scope.tile, UINT_MAX);
		}
		if (t == nullptr) return nullptr;
		this->town_scope.emplace(*this, t, this->station_scope.st == nullptr);
	}
	return &*this->town_scope;
}

uint32_t StationScopeResolver::GetNearbyStationInfo(uint32_t parameter, StationScopeResolver::NearbyStationInfoMode mode) const
{
	TileIndex nearby_tile = GetNearbyTile(parameter, this->tile);

	if (!HasStationTileRail(nearby_tile)) return 0xFFFFFFFF;

	uint32_t grfid = this->st->speclist[GetCustomStationSpecIndex(this->tile)].grfid;
	bool perpendicular = GetRailStationAxis(this->tile) != GetRailStationAxis(nearby_tile);
	bool same_station = this->st->TileBelongsToRailStation(nearby_tile);
	uint32_t res = GB(GetStationGfx(nearby_tile), 1, 2) << 12 | !!perpendicular << 11 | !!same_station << 10;

	uint16_t localidx = 0;
	if (IsCustomStationSpecIndex(nearby_tile)) {
		const StationSpecList &ssl = BaseStation::GetByTile(nearby_tile)->speclist[GetCustomStationSpecIndex(nearby_tile)];
		localidx = ssl.localidx;
		res |= 1 << (ssl.grfid != grfid ? 9 : 8);
	}

	switch (mode) {
		case NearbyStationInfoMode::Standard:
		default:
			return res | ClampTo<uint8_t>(localidx);

		case NearbyStationInfoMode::V2:
			return (res << 8) | localidx;
	}
}

/* virtual */ uint32_t StationScopeResolver::GetVariable(uint16_t variable, uint32_t parameter, GetVariableExtra &extra) const
{
	if (this->st == nullptr) {
		/* Station does not exist, so we're in a purchase list or the land slope check callback. */
		switch (variable) {
			case 0x40:
			case 0x41:
			case 0x46:
			case 0x47:
			case 0x49: return 0x2110000;        // Platforms, tracks & position
			case 0x42: return GetReverseRailTypeTranslation (this->rt, this->statspec->grf_prop.grffile) << 8; // Rail type
			case 0x43: return GetCompanyInfo(_current_company); // Station owner
			case 0x44: return 2;                // PBS status
			case 0x67: // Land info of nearby tile
				if (this->axis != INVALID_AXIS && this->tile != INVALID_TILE) {
					TileIndex tile = this->tile;
					if (parameter != 0) tile = GetNearbyTile(parameter, tile, true, this->axis); // only perform if it is required

					uint32_t result = GetNearbyTileInformation(tile, this->ro.grffile->grf_version >= 8, extra.mask);
					if (extra.mask & SLOPE_EW) {
						Slope tileh = GetTileSlope(tile);
						if (this->axis == AXIS_Y && HasBit(tileh, CORNER_W) != HasBit(tileh, CORNER_E)) result ^= SLOPE_EW;
					}
					return result;
				}
				break;

			case 0x7A: return GetBadgeVariableResult(*this->ro.grffile, this->statspec->badges, parameter);

			case 0xFA: return ClampTo<uint16_t>(CalTime::CurDate() - CalTime::DAYS_TILL_ORIGINAL_BASE_YEAR); // Build date, clamped to a 16 bit value
		}

		extra.available = false;
		return UINT_MAX;
	}

	switch (variable) {
		/* Calculated station variables */
		case 0x40:
			if (!HasBit(_svc.valid, 0)) { _svc.v40 = GetPlatformInfoHelper(this->tile, false, false, false); SetBit(_svc.valid, 0); }
			return _svc.v40;

		case 0x41:
			if (!HasBit(_svc.valid, 1)) { _svc.v41 = GetPlatformInfoHelper(this->tile, true,  false, false); SetBit(_svc.valid, 1); }
			return _svc.v41;

		case 0x42: return GetTerrainType(this->tile) | (GetReverseRailTypeTranslation(GetRailType(this->tile), this->statspec->grf_prop.grffile) << 8);
		case 0x43: return GetCompanyInfo(this->st->owner); // Station owner
		case 0x44: return HasStationReservation(this->tile) ? 7 : 4; // PBS status
		case 0x45:
			if (!HasBit(_svc.valid, 2)) { _svc.v45 = GetRailContinuationInfo(this->tile); SetBit(_svc.valid, 2); }
			return _svc.v45;

		case 0x46:
			if (!HasBit(_svc.valid, 3)) { _svc.v46 = GetPlatformInfoHelper(this->tile, false, false, true); SetBit(_svc.valid, 3); }
			return _svc.v46;

		case 0x47:
			if (!HasBit(_svc.valid, 4)) { _svc.v47 = GetPlatformInfoHelper(this->tile, true,  false, true); SetBit(_svc.valid, 4); }
			return _svc.v47;

		case 0x49:
			if (!HasBit(_svc.valid, 5)) { _svc.v49 = GetPlatformInfoHelper(this->tile, false, true, false); SetBit(_svc.valid, 5); }
			return _svc.v49;

		case 0x4A: // Animation frame of tile
			return GetAnimationFrame(this->tile);

		/* Variables which use the parameter */
		/* Variables 0x60 to 0x65 and 0x69 are handled separately below */
		case 0x66: { // Animation frame of nearby tile
			TileIndex tile = this->tile;
			if (parameter != 0) tile = GetNearbyTile(parameter, tile);
			return this->st->TileBelongsToRailStation(tile) ? GetAnimationFrame(tile) : UINT_MAX;
		}

		case 0x67: { // Land info of nearby tile
			Axis axis = GetRailStationAxis(this->tile);
			TileIndex tile = this->tile;
			if (parameter != 0) tile = GetNearbyTile(parameter, tile); // only perform if it is required

			uint32_t result = GetNearbyTileInformation(tile, this->ro.grffile->grf_version >= 8, extra.mask);
			if (extra.mask & SLOPE_EW) {
				Slope tileh = GetTileSlope(tile);
				if (axis == AXIS_Y && HasBit(tileh, CORNER_W) != HasBit(tileh, CORNER_E)) result ^= SLOPE_EW;
			}
			return result;
		}

		/* Station info of nearby tiles */
		case 0x68: {
			return this->GetNearbyStationInfo(parameter, NearbyStationInfoMode::Standard);
		}

		/* Station info of nearby tiles: v2 */
		case A2VRI_STATION_INFO_NEARBY_TILES_V2: {
			return this->GetNearbyStationInfo(parameter, NearbyStationInfoMode::V2);
		}

		case 0x6A: { // GRFID of nearby station tiles
			TileIndex nearby_tile = GetNearbyTile(parameter, this->tile);

			if (!HasStationTileRail(nearby_tile)) return 0xFFFFFFFF;
			if (!IsCustomStationSpecIndex(nearby_tile)) return 0;

			const StationSpecList &sm = BaseStation::GetByTile(nearby_tile)->speclist[GetCustomStationSpecIndex(nearby_tile)];
			return sm.grfid;
		}

		case 0x6B: { // 16 bit Station ID of nearby tiles
			TileIndex nearby_tile = GetNearbyTile(parameter, this->tile);

			if (!HasStationTileRail(nearby_tile)) return 0xFFFFFFFF;
			if (!IsCustomStationSpecIndex(nearby_tile)) return 0xFFFE;

			uint32_t grfid = this->st->speclist[GetCustomStationSpecIndex(this->tile)].grfid;

			const StationSpecList &sm = BaseStation::GetByTile(nearby_tile)->speclist[GetCustomStationSpecIndex(nearby_tile)];
			if (sm.grfid == grfid) {
				return sm.localidx;
			}

			return 0xFFFE;
		}

		case 0x7A: return GetBadgeVariableResult(*this->ro.grffile, this->statspec->badges, parameter);

		/* General station variables */
		case 0x82: return 50;
		case 0x84: return this->st->string_id;
		case 0x86: return 0;
		case 0xF0: return this->st->facilities;
		case 0xFA: return ClampTo<uint16_t>(this->st->build_date - CalTime::DAYS_TILL_ORIGINAL_BASE_YEAR);
	}

	return this->st->GetNewGRFVariable(this->ro, variable, parameter, extra.available);
}

uint32_t Station::GetNewGRFVariable(const ResolverObject &object, uint16_t variable, uint8_t parameter, bool &available) const
{
	switch (variable) {
		case 0x48: { // Accepted cargo types
			uint32_t value = GetAcceptanceMask(this);
			return value;
		}

		case 0x8A: return this->had_vehicle_of_type;
		case 0xF1: return (this->airport.tile != INVALID_TILE) ? this->airport.GetSpec()->ttd_airport_type : ATP_TTDP_LARGE;
		case 0xF2: return (this->truck_stops != nullptr) ? this->truck_stops->status : 0;
		case 0xF3: return (this->bus_stops != nullptr)   ? this->bus_stops->status   : 0;
		case 0xF6: return this->airport.flags;
		case 0xF7: return GB(this->airport.flags, 8, 8);
	}

	/* Handle cargo variables with parameter, 0x60 to 0x65 and 0x69 */
	if ((variable >= 0x60 && variable <= 0x65) || variable == 0x69) {
		CargoType c = GetCargoTranslation(parameter, object.grffile);

		if (c == INVALID_CARGO) {
			switch (variable) {
				case 0x62: return 0xFFFFFFFF;
				case 0x64: return 0xFF00;
				default:   return 0;
			}
		}
		const GoodsEntry *ge = &this->goods[c];

		switch (variable) {
			case 0x60: return std::min<uint32_t>(ge->CargoTotalCount(), 4095);
			case 0x61: return ge->HasVehicleEverTriedLoading() && ge->IsSupplyAllowed() ? ge->time_since_pickup : 0;
			case 0x62: return ge->HasRating() ? ge->rating : 0xFFFFFFFF;
			case 0x63: return ge->data != nullptr ? ge->data->cargo.PeriodsInTransit() : 0;
			case 0x64: return ge->HasVehicleEverTriedLoading() && ge->IsSupplyAllowed() ? ge->last_speed | (ge->last_age << 8) : 0xFF00;
			case 0x65: return GB(ge->status, GoodsEntry::GES_ACCEPTANCE, 1) << 3;
			case 0x69: {
				static_assert((int)GoodsEntry::GES_EVER_ACCEPTED + 1 == (int)GoodsEntry::GES_LAST_MONTH);
				static_assert((int)GoodsEntry::GES_EVER_ACCEPTED + 2 == (int)GoodsEntry::GES_CURRENT_MONTH);
				static_assert((int)GoodsEntry::GES_EVER_ACCEPTED + 3 == (int)GoodsEntry::GES_ACCEPTED_BIGTICK);
				return GB(ge->status, GoodsEntry::GES_EVER_ACCEPTED, 4);
			}
		}
	}

	/* Handle cargo variables (deprecated) */
	if (variable >= 0x8C && variable <= 0xEC) {
		const GoodsEntry *g = &this->goods[GB(variable - 0x8C, 3, 4)];
		switch (GB(variable - 0x8C, 0, 3)) {
			case 0: return g->CargoTotalCount();
			case 1: return GB(std::min(g->CargoTotalCount(), 4095u), 0, 4) | (GB(g->status, GoodsEntry::GES_ACCEPTANCE, 1) << 7);
			case 2: return g->time_since_pickup;
			case 3: return g->rating;
			case 4: return g->data != nullptr ? g->data->cargo.GetFirstStation() : INVALID_STATION;
			case 5: return g->data != nullptr ? g->data->cargo.PeriodsInTransit() : 0;
			case 6: return g->last_speed;
			case 7: return g->last_age;
		}
	}

	Debug(grf, 1, "Unhandled station variable 0x{:X}", variable);

	available = false;
	return UINT_MAX;
}

uint32_t Waypoint::GetNewGRFVariable(const ResolverObject &object, uint16_t variable, uint8_t parameter, bool &available) const
{
	switch (variable) {
		case 0x48: return 0; // Accepted cargo types
		case 0x8A: return HVOT_WAYPOINT;
		case 0xF1: return 0; // airport type
		case 0xF2: return 0; // truck stop status
		case 0xF3: return 0; // bus stop status
		case 0xF6: return 0; // airport flags
		case 0xF7: return 0; // airport flags cont.
	}

	/* Handle cargo variables with parameter, 0x60 to 0x65 */
	if (variable >= 0x60 && variable <= 0x65) {
		return 0;
	}

	/* Handle cargo variables (deprecated) */
	if (variable >= 0x8C && variable <= 0xEC) {
		switch (GB(variable - 0x8C, 0, 3)) {
			case 3: return INITIAL_STATION_RATING;
			case 4: return INVALID_STATION;
			default: return 0;
		}
	}

	Debug(grf, 1, "Unhandled station variable 0x{:X}", variable);

	available = false;
	return UINT_MAX;
}

/* virtual */ const SpriteGroup *StationResolverObject::ResolveReal(const RealSpriteGroup *group) const
{
	if (this->station_scope.st == nullptr || !Station::IsExpected(this->station_scope.st)) {
		return group->loading[0];
	}

	uint cargo = 0;
	const Station *st = Station::From(this->station_scope.st);

	switch (this->station_scope.cargo_type) {
		case INVALID_CARGO:
		case SpriteGroupCargo::SG_DEFAULT_NA:
		case SpriteGroupCargo::SG_PURCHASE:
			cargo = 0;
			break;

		case SpriteGroupCargo::SG_DEFAULT:
			for (const GoodsEntry &ge : st->goods) {
				cargo += ge.CargoTotalCount();
			}
			break;

		default:
			cargo = st->goods[this->station_scope.cargo_type].CargoTotalCount();
			break;
	}

	if (this->station_scope.statspec->flags.Test(StationSpecFlag::DivByStationSize)) cargo /= (st->train_station.w + st->train_station.h);
	cargo = std::min(0xfffu, cargo);

	if (cargo > this->station_scope.statspec->cargo_threshold) {
		if (!group->loading.empty()) {
			uint set = ((cargo - this->station_scope.statspec->cargo_threshold) * (uint)group->loading.size()) / (4096 - this->station_scope.statspec->cargo_threshold);
			return group->loading[set];
		}
	} else {
		if (!group->loaded.empty()) {
			uint set = (cargo * (uint)group->loaded.size()) / (this->station_scope.statspec->cargo_threshold + 1);
			return group->loaded[set];
		}
	}

	return group->loading[0];
}

GrfSpecFeature StationResolverObject::GetFeature() const
{
	return GSF_STATIONS;
}

uint32_t StationResolverObject::GetDebugID() const
{
	return this->station_scope.statspec->grf_prop.local_id;
}

/**
 * Resolver for stations.
 * @param statspec Station (type) specification.
 * @param base_station Instance of the station.
 * @param tile %Tile of the station.
 * @param rt %RailType of the station (unbuilt stations only).
 * @param callback Callback ID.
 * @param callback_param1 First parameter (var 10) of the callback.
 * @param callback_param2 Second parameter (var 18) of the callback.
 */
StationResolverObject::StationResolverObject(const StationSpec *statspec, BaseStation *base_station, TileIndex tile, RailType rt,
		CallbackID callback, uint32_t callback_param1, uint32_t callback_param2)
	: ResolverObject(statspec->grf_prop.grffile, callback, callback_param1, callback_param2),
	station_scope(*this, statspec, base_station, tile, rt)
{
	/* Invalidate all cached vars */
	_svc.valid = 0;

	CargoType ctype = SpriteGroupCargo::SG_DEFAULT_NA;

	if (this->station_scope.st == nullptr) {
		/* No station, so we are in a purchase list */
		ctype = SpriteGroupCargo::SG_PURCHASE;
	} else if (Station::IsExpected(this->station_scope.st)) {
		const Station *st = Station::From(this->station_scope.st);
		/* Pick the first cargo that we have waiting */
		for (const auto &[cargo, spritegroup] : statspec->grf_prop) {
			if (cargo < NUM_CARGO && st->goods[cargo].CargoTotalCount() > 0) {
				ctype = cargo;
				break;
			}
		}
	}

	this->root_spritegroup = this->station_scope.statspec->grf_prop.GetSpriteGroup(ctype);
	if (this->root_spritegroup == nullptr) {
		ctype = SpriteGroupCargo::SG_DEFAULT;
		this->root_spritegroup = this->station_scope.statspec->grf_prop.GetSpriteGroup(ctype);
	}

	/* Remember the cargo type we've picked */
	this->station_scope.cargo_type = ctype;
}

/**
 * Resolve sprites for drawing a station tile.
 * @param statspec Station spec
 * @param st Station (nullptr in GUI)
 * @param tile Station tile being drawn (INVALID_TILE in GUI)
 * @param rt %RailType of the station (unbuilt stations only).
 * @param var10 Value to put in variable 10; normally 0; 1 when resolving the groundsprite and StationSpecFlag::SeparateGround is set.
 * @return First sprite of the Action 1 spriteset to use, minus an offset of 0x42D to accommodate for weird NewGRF specs.
 */
SpriteID GetCustomStationRelocation(const StationSpec *statspec, BaseStation *st, TileIndex tile, RailType rt, uint32_t var10)
{
	StationResolverObject object(statspec, st, tile, rt, CBID_NO_CALLBACK, var10);
	const SpriteGroup *group = object.Resolve();
	if (group == nullptr || group->type != SGT_RESULT) return 0;
	return group->GetResult() - 0x42D;
}

/**
 * Resolve the sprites for custom station foundations.
 * @param statspec Station spec
 * @param st Station
 * @param tile Station tile being drawn
 * @param layout Spritelayout as returned by previous callback
 * @param edge_info Information about northern tile edges; whether they need foundations or merge into adjacent tile's foundations.
 * @return First sprite of a set of foundation sprites for various slopes, or 0 if default foundations shall be drawn.
 */
SpriteID GetCustomStationFoundationRelocation(const StationSpec *statspec, BaseStation *st, TileIndex tile, uint layout, uint edge_info)
{
	/* callback_param1 == 2 means  we are resolving the foundation sprites. */
	StationResolverObject object(statspec, st, tile, INVALID_RAILTYPE, CBID_NO_CALLBACK, 2, layout | (edge_info << 16));

	const SpriteGroup *group = object.Resolve();
	if (group == nullptr || group->type != SGT_RESULT) return 0;

	/* Note: SpriteGroup::Resolve zeroes all registers, so register 0x100 is initialised to 0. (compatibility) */
	return group->GetResult() + GetRegister(0x100);
}


uint16_t GetStationCallback(CallbackID callback, uint32_t param1, uint32_t param2, const StationSpec *statspec, BaseStation *st, TileIndex tile, RailType rt)
{
	StationResolverObject object(statspec, st, tile, rt, callback, param1, param2);
	return object.ResolveCallback();
}

/**
 * Check the slope of a tile of a new station.
 * @param north_tile Norther tile of the station rect.
 * @param cur_tile Tile to check.
 * @param statspec Station spec.
 * @param axis Axis of the new station.
 * @param plat_len Platform length.
 * @param numtracks Number of platforms.
 * @return Succeeded or failed command.
 */
CommandCost PerformStationTileSlopeCheck(TileIndex north_tile, TileIndex cur_tile, RailType rt, const StationSpec *statspec, Axis axis, uint8_t plat_len, uint8_t numtracks)
{
	TileIndexDiffCUnsigned diff = TileIndexToTileIndexDiffCUnsigned(cur_tile, north_tile);
	Slope slope = GetTileSlope(cur_tile);

	StationResolverObject object(statspec, nullptr, cur_tile, rt, CBID_STATION_LAND_SLOPE_CHECK,
			(slope << 4) | (slope ^ (axis == AXIS_Y && HasBit(slope, CORNER_W) != HasBit(slope, CORNER_E) ? SLOPE_EW : 0)),
			(numtracks << 24) | (plat_len << 16) | (axis == AXIS_Y ? diff.x << 8 | diff.y : diff.y << 8 | diff.x));
	object.station_scope.axis = axis;

	uint16_t cb_res = object.ResolveCallback();

	/* Failed callback means success. */
	if (cb_res == CALLBACK_FAILED) return CommandCost();

	/* The meaning of bit 10 is inverted for a grf version < 8. */
	if (statspec->grf_prop.grffile->grf_version < 8) ToggleBit(cb_res, 10);
	return GetErrorMessageFromLocationCallbackResult(cb_res, statspec->grf_prop.grffile, STR_ERROR_LAND_SLOPED_IN_WRONG_DIRECTION);
}


/**
 * Allocate a StationSpec to a Station. This is called once per build operation.
 * @param statspec StationSpec to allocate.
 * @param st Station to allocate it to.
 * @param exec Whether to actually allocate the spec.
 * @return Index within the Station's spec list, or -1 if the allocation failed.
 */
int AllocateSpecToStation(const StationSpec *statspec, BaseStation *st, bool exec)
{
	uint i;

	if (statspec == nullptr || st == nullptr) return 0;

	for (i = 1; i < st->speclist.size() && i < NUM_STATIONSSPECS_PER_STATION; i++) {
		if (st->speclist[i].spec == nullptr && st->speclist[i].grfid == 0) break;
	}

	if (i == NUM_STATIONSSPECS_PER_STATION) {
		/* As final effort when the spec list is already full...
		 * try to find the same spec and return that one. This might
		 * result in slightly "wrong" (as per specs) looking stations,
		 * but it's fairly unlikely that one reaches the limit anyways.
		 */
		for (i = 1; i < st->speclist.size() && i < NUM_STATIONSSPECS_PER_STATION; i++) {
			if (st->speclist[i].spec == statspec) return i;
		}

		return -1;
	}

	if (exec) {
		if (i >= st->speclist.size()) st->speclist.resize(i + 1);
		st->speclist[i].spec     = statspec;
		st->speclist[i].grfid    = statspec->grf_prop.grfid;
		st->speclist[i].localidx = statspec->grf_prop.local_id;

		StationUpdateCachedTriggers(st);
	}

	return i;
}


/**
 * Deallocate a StationSpec from a Station. Called when removing a single station tile.
 * @param st Station to work with.
 * @param specindex Index of the custom station within the Station's spec list.
 * @return Indicates whether the StationSpec was deallocated.
 */
void DeallocateSpecFromStation(BaseStation *st, uint8_t specindex)
{
	/* specindex of 0 (default) is never freeable */
	if (specindex == 0) return;

	ETileArea area = ETileArea(st, INVALID_TILE, TA_WHOLE);
	/* Check all tiles over the station to check if the specindex is still in use */
	for (TileIndex tile : area) {
		if (st->TileBelongsToRailStation(tile) && GetCustomStationSpecIndex(tile) == specindex) {
			return;
		}
	}

	/* This specindex is no longer in use, so deallocate it */
	st->speclist[specindex].spec     = nullptr;
	st->speclist[specindex].grfid    = 0;
	st->speclist[specindex].localidx = 0;

	/* If this was the highest spec index, reallocate */
	if (specindex == st->speclist.size() - 1) {
		size_t num_specs;
		for (num_specs = st->speclist.size() - 1; num_specs > 0; num_specs--) {
			if (st->speclist[num_specs].grfid != 0) break;
		}

		if (num_specs > 0) {
			st->speclist.resize(num_specs + 1);
		} else {
			st->speclist.clear();
			st->cached_anim_triggers = 0;
			st->cached_cargo_triggers = 0;
			return;
		}
	}

	StationUpdateCachedTriggers(st);
}

/**
 * Draw representation of a station tile for GUI purposes.
 * @param x Position x of image.
 * @param y Position y of image.
 * @param axis Axis.
 * @param railtype Rail type.
 * @param sclass, station Type of station.
 * @param station station ID
 * @return True if the tile was drawn (allows for fallback to default graphic)
 */
bool DrawStationTile(int x, int y, RailType railtype, Axis axis, StationClassID sclass, uint station)
{
	const DrawTileSprites *sprites = nullptr;
	const RailTypeInfo *rti = GetRailTypeInfo(railtype);
	PaletteID palette = COMPANY_SPRITE_COLOUR(_local_company);
	uint tile = 2;

	const StationSpec *statspec = StationClass::Get(sclass)->GetSpec(station);
	if (statspec == nullptr) return false;

	if (statspec->callback_mask.Test(StationCallbackMask::DrawTileLayout)) {
		uint16_t callback = GetStationCallback(CBID_STATION_DRAW_TILE_LAYOUT, 0, 0, statspec, nullptr, INVALID_TILE, railtype);
		if (callback != CALLBACK_FAILED) tile = callback & ~1;
	}

	uint32_t total_offset = rti->GetRailtypeSpriteOffset();
	uint32_t relocation = 0;
	uint32_t ground_relocation = 0;
	const NewGRFSpriteLayout *layout = nullptr;
	DrawTileSprites tmp_rail_layout;

	if (statspec->renderdata.empty()) {
		sprites = GetStationTileLayout(StationType::Rail, tile + axis);
	} else {
		layout = &statspec->renderdata[(tile < statspec->renderdata.size()) ? tile + axis : (uint)axis];
		if (!layout->NeedsPreprocessing()) {
			sprites = layout;
			layout = nullptr;
		}
	}

	if (layout != nullptr) {
		/* Sprite layout which needs preprocessing */
		bool separate_ground = statspec->flags.Test(StationSpecFlag::SeparateGround);
		uint32_t var10_values = layout->PrepareLayout(total_offset, rti->fallback_railtype, 0, 0, separate_ground);
		for (uint8_t var10 : SetBitIterator(var10_values)) {
			uint32_t var10_relocation = GetCustomStationRelocation(statspec, nullptr, INVALID_TILE, railtype, var10);
			layout->ProcessRegisters(var10, var10_relocation, separate_ground);
		}

		tmp_rail_layout.seq = layout->GetLayout(&tmp_rail_layout.ground);
		sprites = &tmp_rail_layout;
		total_offset = 0;
	} else {
		/* Simple sprite layout */
		ground_relocation = relocation = GetCustomStationRelocation(statspec, nullptr, INVALID_TILE, railtype, 0);
		if (HasBit(sprites->ground.sprite, SPRITE_MODIFIER_CUSTOM_SPRITE)) {
			ground_relocation = GetCustomStationRelocation(statspec, nullptr, INVALID_TILE, railtype, 1);
		}
		ground_relocation += rti->fallback_railtype;
	}

	SpriteID image = sprites->ground.sprite;
	PaletteID pal = sprites->ground.pal;
	RailTrackOffset overlay_offset;
	if (rti->UsesOverlay() && SplitGroundSpriteForOverlay(nullptr, &image, &overlay_offset)) {
		SpriteID ground = GetCustomRailSprite(rti, INVALID_TILE, RTSG_GROUND);
		DrawSprite(image, PAL_NONE, x, y);
		DrawSprite(ground + overlay_offset, PAL_NONE, x, y);
	} else {
		image += HasBit(image, SPRITE_MODIFIER_CUSTOM_SPRITE) ? ground_relocation : total_offset;
		if (HasBit(pal, SPRITE_MODIFIER_CUSTOM_SPRITE)) pal += ground_relocation;
		DrawSprite(image, GroundSpritePaletteTransform(image, pal, palette), x, y);
	}

	DrawRailTileSeqInGUI(x, y, sprites, total_offset, relocation, palette);

	return true;
}


const StationSpec *GetStationSpec(TileIndex t)
{
	if (!IsCustomStationSpecIndex(t)) return nullptr;

	const BaseStation *st = BaseStation::GetByTile(t);
	uint specindex = GetCustomStationSpecIndex(t);
	return specindex < st->speclist.size() ? st->speclist[specindex].spec : nullptr;
}

/** Wrapper for animation control, see GetStationCallback. */
uint16_t GetAnimStationCallback(CallbackID callback, uint32_t param1, uint32_t param2, const StationSpec *statspec, BaseStation *st, TileIndex tile, int extra_data)
{
	return GetStationCallback(callback, param1, param2, statspec, st, tile, INVALID_RAILTYPE);
}

/** Helper class for animation control. */
struct StationAnimationBase : public AnimationBase<StationAnimationBase, StationSpec, BaseStation, int, GetAnimStationCallback, TileAnimationFrameAnimationHelper<BaseStation> > {
	static constexpr CallbackID cb_animation_speed      = CBID_STATION_ANIMATION_SPEED;
	static constexpr CallbackID cb_animation_next_frame = CBID_STATION_ANIM_NEXT_FRAME;

	static constexpr StationCallbackMask cbm_animation_speed      = StationCallbackMask::AnimationSpeed;
	static constexpr StationCallbackMask cbm_animation_next_frame = StationCallbackMask::AnimationNextFrame;
};

void AnimateStationTile(TileIndex tile)
{
	const StationSpec *ss = GetStationSpec(tile);
	if (ss == nullptr) return;

	StationAnimationBase::AnimateTile(ss, BaseStation::GetByTile(tile), tile, ss->flags.Test(StationSpecFlag::Cb141RandomBits));
}

uint8_t GetStationTileAnimationSpeed(TileIndex tile)
{
	const StationSpec *ss = GetStationSpec(tile);
	if (ss == nullptr) return 0;

	return StationAnimationBase::GetAnimationSpeed(ss);
}

void TriggerStationAnimation(BaseStation *st, TileIndex trigger_tile, StationAnimationTrigger trigger, CargoType cargo_type)
{
	/* List of coverage areas for each animation trigger */
	static const TriggerArea tas[] = {
		TA_TILE, TA_WHOLE, TA_WHOLE, TA_PLATFORM, TA_PLATFORM, TA_PLATFORM, TA_WHOLE
	};

	/* Get Station if it wasn't supplied */
	if (st == nullptr) st = BaseStation::GetByTile(trigger_tile);

	/* Check the cached animation trigger bitmask to see if we need
	 * to bother with any further processing. */
	if (!HasBit(st->cached_anim_triggers, trigger)) return;

	uint16_t random_bits = Random();
	ETileArea area = ETileArea(st, trigger_tile, tas[trigger]);

	/* Check all tiles over the station to check if the specindex is still in use */
	for (TileIndex tile : area) {
		if (st->TileBelongsToRailStation(tile)) {
			const StationSpec *ss = GetStationSpec(tile);
			if (ss != nullptr && HasBit(ss->animation.triggers, trigger)) {
				CargoType cargo;
				if (cargo_type == INVALID_CARGO) {
					cargo = INVALID_CARGO;
				} else {
					cargo = ss->grf_prop.grffile->cargo_map[cargo_type];
				}
				StationAnimationBase::ChangeAnimationFrame(CBID_STATION_ANIM_START_STOP, ss, st, tile, (random_bits << 16) | GB(Random(), 0, 16), (uint8_t)trigger | (cargo << 8));
			}
		}
	}
}

/**
 * Trigger station randomisation
 * @param st station being triggered
 * @param trigger_tile specific tile of platform to trigger
 * @param trigger trigger type
 * @param cargo_type cargo type causing trigger
 */
void TriggerStationRandomisation(Station *st, TileIndex trigger_tile, StationRandomTrigger trigger, CargoType cargo_type)
{
	/* List of coverage areas for each animation trigger */
	static const TriggerArea tas[] = {
		TA_WHOLE, TA_WHOLE, TA_PLATFORM, TA_PLATFORM, TA_PLATFORM, TA_PLATFORM
	};

	/* Get Station if it wasn't supplied */
	if (st == nullptr) st = Station::GetByTile(trigger_tile);

	/* Check the cached cargo trigger bitmask to see if we need
	 * to bother with any further processing. */
	if (st->cached_cargo_triggers == 0) return;
	if (cargo_type != INVALID_CARGO && !HasBit(st->cached_cargo_triggers, cargo_type)) return;

	uint32_t whole_reseed = 0;
	ETileArea area = ETileArea(st, trigger_tile, tas[trigger]);

	/* Bitmask of completely empty cargo types to be matched. */
	CargoTypes empty_mask = (trigger == SRT_CARGO_TAKEN) ? GetEmptyMask(st) : 0;

	/* Store triggers now for var 5F */
	SetBit(st->waiting_triggers, trigger);
	uint32_t used_triggers = 0;

	/* Check all tiles over the station to check if the specindex is still in use */
	for (TileIndex tile : area) {
		if (st->TileBelongsToRailStation(tile)) {
			const StationSpec *ss = GetStationSpec(tile);
			if (ss == nullptr) continue;

			/* Cargo taken "will only be triggered if all of those
			 * cargo types have no more cargo waiting." */
			if (trigger == SRT_CARGO_TAKEN) {
				if ((ss->cargo_triggers & ~empty_mask) != 0) continue;
			}

			if (cargo_type == INVALID_CARGO || HasBit(ss->cargo_triggers, cargo_type)) {
				StationResolverObject object(ss, st, tile, INVALID_RAILTYPE, CBID_RANDOM_TRIGGER, 0);
				object.waiting_triggers = st->waiting_triggers;

				const SpriteGroup *group = object.Resolve();
				if (group == nullptr) continue;

				used_triggers |= object.used_triggers;

				uint32_t reseed = object.GetReseedSum();
				if (reseed != 0) {
					whole_reseed |= reseed;
					reseed >>= 16;

					/* Set individual tile random bits */
					uint8_t random_bits = GetStationTileRandomBits(tile);
					random_bits &= ~reseed;
					random_bits |= Random() & reseed;
					SetStationTileRandomBits(tile, random_bits);

					MarkTileDirtyByTile(tile, VMDF_NOT_MAP_MODE);
				}
			}
		}
	}

	/* Update whole station random bits */
	st->waiting_triggers &= ~used_triggers;
	if ((whole_reseed & 0xFFFF) != 0) {
		st->random_bits &= ~whole_reseed;
		st->random_bits |= Random() & whole_reseed;
	}
}

/**
 * Update the cached animation trigger bitmask for a station.
 * @param st Station to update.
 */
void StationUpdateCachedTriggers(BaseStation *st)
{
	st->cached_anim_triggers = 0;
	st->cached_cargo_triggers = 0;

	/* Combine animation trigger bitmask for all station specs
	 * of this station. */
	for (const auto &sm : GetStationSpecList<StationSpec>(st)) {
		if (sm.spec == nullptr) continue;
		st->cached_anim_triggers |= sm.spec->animation.triggers;
		st->cached_cargo_triggers |= sm.spec->cargo_triggers;
	}
}

void DumpStationSpriteGroup(const StationSpec *statspec, BaseStation *st, SpriteGroupDumper &dumper)
{
	StationResolverObject ro(statspec, st, INVALID_TILE, INVALID_RAILTYPE);

	switch (ro.station_scope.cargo_type) {
		case SpriteGroupCargo::SG_DEFAULT:
			dumper.Print("SG_DEFAULT");
			break;
		case SpriteGroupCargo::SG_PURCHASE:
			dumper.Print("SG_PURCHASE");
			break;
		case SpriteGroupCargo::SG_DEFAULT_NA:
			dumper.Print("SG_DEFAULT_NA");
			break;
		default:
			dumper.Print(fmt::format("Cargo: {}", ro.station_scope.cargo_type));
			break;
	}

	dumper.DumpSpriteGroup(ro.root_spritegroup, 0);

	for (const auto &[cargo, spritegroup] : statspec->grf_prop) {
		if (spritegroup != ro.root_spritegroup) {
			dumper.Print("");
			switch (cargo) {
				case SpriteGroupCargo::SG_DEFAULT:
					dumper.Print("OTHER SPRITE GROUP: SG_DEFAULT");
					break;
				case SpriteGroupCargo::SG_PURCHASE:
					dumper.Print("OTHER SPRITE GROUP: SG_PURCHASE");
					break;
				case SpriteGroupCargo::SG_DEFAULT_NA:
					dumper.Print("OTHER SPRITE GROUP: SG_DEFAULT_NA");
					break;
				default:
					dumper.Print(fmt::format("OTHER SPRITE GROUP: Cargo: {}", cargo));
					break;
			}
			dumper.DumpSpriteGroup(spritegroup, 0);
		}
	}
}

void UpdateStationTileCacheFlags(bool force_update)
{
	SimpleChecksum64 checksum;
	for (const StationClass &cls : StationClass::Classes()) {
		checksum.Update(cls.GetSpecCount());
		for (uint j = 0; j < cls.GetSpecCount(); j++) {
			const StationSpec *statspec = cls.GetSpec(j);
			if (statspec == nullptr) continue;

			checksum.Update(j);
			for (StationSpec::TileFlags flags : statspec->tileflags) {
				checksum.Update(static_cast<uint64_t>(flags.base()));
			}
		}
	}

	if (checksum.state != _station_tile_cache_hash || force_update) {
		_station_tile_cache_hash = checksum.state;

		for (TileIndex t(0); t < Map::Size(); t++) {
			if (HasStationTileRail(t)) SetRailStationTileFlags(t, GetStationSpec(t));
		}
	}
}
