/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file road_map.h Map accessors for roads. */

#ifndef ROAD_MAP_H
#define ROAD_MAP_H

#include "track_func.h"
#include "depot_type.h"
#include "rail_type.h"
#include "road_func.h"
#include "tile_map.h"


/** The different types of road tiles. */
enum RoadTileType : uint8_t {
	ROAD_TILE_NORMAL,   ///< Normal road
	ROAD_TILE_CROSSING, ///< Level crossing
	ROAD_TILE_DEPOT,    ///< Depot (one entrance)
};

/**
 * Test whether a tile can have road/tram types.
 * @param t Tile to query.
 * @return true if tile can be queried about road/tram types.
 */
inline bool MayHaveRoad(TileIndex t)
{
	switch (GetTileType(t)) {
		case MP_ROAD:
			return true;

		case MP_STATION:
			return true;

		case MP_TUNNELBRIDGE:
			return GB(_m[t].m5, 2, 2) == 1;

		default:
			return false;
	}
}

/**
 * Test whether a tile type can have road/tram types.
 * @param type Tile type.
 * @return true if tile type can have road/tram types.
 */
inline bool MayTileTypeHaveRoad(TileType type)
{
	switch (type) {
		case MP_ROAD:
		case MP_STATION:
		case MP_TUNNELBRIDGE:
			return true;

		default:
			return false;
	}
}

/**
 * Get the type of the road tile.
 * @param t Tile to query.
 * @pre IsTileType(t, MP_ROAD)
 * @return The road tile type.
 */
debug_inline static RoadTileType GetRoadTileType(TileIndex t)
{
	dbg_assert_tile(IsTileType(t, MP_ROAD), t);
	return (RoadTileType)GB(_m[t].m5, 6, 2);
}

/**
 * Return whether a tile is a normal road.
 * @param t Tile to query.
 * @pre IsTileType(t, MP_ROAD)
 * @return True if normal road.
 */
debug_inline static bool IsNormalRoad(TileIndex t)
{
	return GetRoadTileType(t) == ROAD_TILE_NORMAL;
}

/**
 * Return whether a tile is a normal road tile.
 * @param t Tile to query.
 * @return True if normal road tile.
 */
debug_inline static bool IsNormalRoadTile(TileIndex t)
{
	return IsTileType(t, MP_ROAD) && IsNormalRoad(t);
}

/**
 * Return whether a tile is a level crossing.
 * @param t Tile to query.
 * @pre IsTileType(t, MP_ROAD)
 * @return True if level crossing.
 */
inline bool IsLevelCrossing(TileIndex t)
{
	return GetRoadTileType(t) == ROAD_TILE_CROSSING;
}

/**
 * Return whether a tile is a level crossing tile.
 * @param t Tile to query.
 * @return True if level crossing tile.
 */
inline bool IsLevelCrossingTile(TileIndex t)
{
	return IsTileType(t, MP_ROAD) && IsLevelCrossing(t);
}

/**
 * Return whether a tile is a road depot.
 * @param t Tile to query.
 * @pre IsTileType(t, MP_ROAD)
 * @return True if road depot.
 */
debug_inline static bool IsRoadDepot(TileIndex t)
{
	return GetRoadTileType(t) == ROAD_TILE_DEPOT;
}

/**
 * Return whether a tile is a road depot tile.
 * @param t Tile to query.
 * @return True if road depot tile.
 */
debug_inline static bool IsRoadDepotTile(TileIndex t)
{
	return IsTileType(t, MP_ROAD) && IsRoadDepot(t);
}

/**
 * Get the present road bits for a specific road type.
 * @param t  The tile to query.
 * @param rt Road type.
 * @pre IsNormalRoad(t)
 * @return The present road bits for the road type.
 */
inline RoadBits GetRoadBits(TileIndex t, RoadTramType rtt)
{
	dbg_assert_tile(IsNormalRoad(t), t);
	if (rtt == RTT_TRAM) return (RoadBits)GB(_m[t].m3, 0, 4);
	return (RoadBits)GB(_m[t].m5, 0, 4);
}

/**
 * Get all set RoadBits on the given tile
 *
 * @param tile The tile from which we want to get the RoadBits
 * @return all set RoadBits of the tile
 */
inline RoadBits GetAllRoadBits(TileIndex tile)
{
	return GetRoadBits(tile, RTT_ROAD) | GetRoadBits(tile, RTT_TRAM);
}

/**
 * Set the present road bits for a specific road type.
 * @param t  The tile to change.
 * @param r  The new road bits.
 * @param rt Road type.
 * @pre IsNormalRoad(t)
 */
inline void SetRoadBits(TileIndex t, RoadBits r, RoadTramType rtt)
{
	assert_tile(IsNormalRoad(t), t); // XXX incomplete
	if (rtt == RTT_TRAM) {
		SB(_m[t].m3, 0, 4, r);
	} else {
		SB(_m[t].m5, 0, 4, r);
	}
}

inline RoadType GetRoadTypeRoad(TileIndex t)
{
	dbg_assert(MayHaveRoad(t));
	return (RoadType)GB(_m[t].m4, 0, 6);
}

inline RoadType GetRoadTypeTram(TileIndex t)
{
	dbg_assert(MayHaveRoad(t));
	return (RoadType)GB(_me[t].m8, 6, 6);
}

inline RoadType GetRoadType(TileIndex t, RoadTramType rtt)
{
	return (rtt == RTT_TRAM) ? GetRoadTypeTram(t) : GetRoadTypeRoad(t);
}

/**
 * Get the present road types of a tile.
 * @param t The tile to query.
 * @return Present road types.
 */
inline RoadTypes GetPresentRoadTypes(TileIndex t)
{
	RoadTypes result = ROADTYPES_NONE;
	if (MayHaveRoad(t)) {
		if (GetRoadTypeRoad(t) != INVALID_ROADTYPE) SetBit(result, GetRoadTypeRoad(t));
		if (GetRoadTypeTram(t) != INVALID_ROADTYPE) SetBit(result, GetRoadTypeTram(t));
	}
	return result;
}

inline bool HasRoadTypeRoad(TileIndex t)
{
	return GetRoadTypeRoad(t) != INVALID_ROADTYPE;
}

inline bool HasRoadTypeTram(TileIndex t)
{
	return GetRoadTypeTram(t) != INVALID_ROADTYPE;
}

/**
 * Get the present road types of a tile.
 * @param t The tile to query.
 * @return Present road types.
 */
inline RoadTramTypes GetPresentRoadTramTypes(TileIndex t)
{
	RoadTramTypes result = (RoadTramTypes)0;
	if (MayHaveRoad(t)) {
		if (GetRoadTypeRoad(t) != INVALID_ROADTYPE) result |= RTTB_ROAD;
		if (GetRoadTypeTram(t) != INVALID_ROADTYPE) result |= RTTB_TRAM;
	}
	return result;
}

/**
 * Check if a tile has a road or a tram road type.
 * @param t  The tile to check.
 * @param tram True to check tram, false to check road.
 * @return True if the tile has the specified road type.
 */
inline bool HasTileRoadType(TileIndex t, RoadTramType rtt)
{
	return GetRoadType(t, rtt) != INVALID_ROADTYPE;
}

/**
 * Check if a tile has one of the specified road types.
 * @param t  The tile to check.
 * @param rts Allowed road types.
 * @return True if the tile has one of the specified road types.
 */
inline bool HasTileAnyRoadType(TileIndex t, RoadTypes rts)
{
	if (!MayHaveRoad(t)) return false;
	return (GetPresentRoadTypes(t) & rts);
}

/**
 * Get the owner of a specific road type.
 * @param t  The tile to query.
 * @param rtt RoadTramType.
 * @return Owner of the given road type.
 */
inline Owner GetRoadOwner(TileIndex t, RoadTramType rtt)
{
	dbg_assert(MayHaveRoad(t));
	if (rtt == RTT_ROAD) {
		if (IsNormalRoadTile(t)) {
			return GetTileOwner(t);
		} else {
			return (Owner)(GB(_me[t].m7, 0, 5) | ((uint16_t)_me[t].m9&0xe0) | (((uint16_t)_me[t].m10&0x03)<<8));
		}
	}
	/* Trams don't need OWNER_TOWN, and remapping OWNER_NONE
	 * to OWNER_TOWN makes it use one bit less */
	Owner o = (Owner)(GB(_m[t].m3, 4, 4) | (((uint16_t)_me[t].m10&0xFC)<<3));
	return o == OWNER_TOWN ? OWNER_NONE : o;
}

/**
 * Set the owner of a specific road type.
 * @param t  The tile to change.
 * @param rtt RoadTramType.
 * @param o  New owner of the given road type.
 */
inline void SetRoadOwner(TileIndex t, RoadTramType rtt, Owner o)
{
	if (rtt == RTT_ROAD) {
		if (IsNormalRoadTile(t)) {
			SetTileOwner(t, o);
		} else {
			SB(_me[t].m7, 0, 5, o);
			uint8_t owner_add = ((o&0x3FF)>>5);
			_me[t].m9 &= ~(0xe0);
			_me[t].m9 |= (owner_add&0x1f)<<5;
			_me[t].m10 &= ~(0x03);
			_me[t].m10 |= (owner_add&0x1f)>>3;

		}
		} else {
		SB(_m[t].m3, 4, 4, o == OWNER_NONE ? OLD_OWNER_TOWN : o);
		uint8_t owner_add = ((o&0x3FF)>>5);
		_me[t].m10 &= ~(0xFC);
		_me[t].m10 |= (owner_add&0x1f)<<2;	}
}

/**
 * Check if a specific road type is owned by an owner.
 * @param t  The tile to query.
 * @param tram True to check tram, false to check road.
 * @param o  Owner to compare with.
 * @pre HasTileRoadType(t, rt)
 * @return True if the road type is owned by the given owner.
 */
inline bool IsRoadOwner(TileIndex t, RoadTramType rtt, Owner o)
{
	dbg_assert_tile(HasTileRoadType(t, rtt), t);
	return (GetRoadOwner(t, rtt) == o);
}

/**
 * Checks if given tile has town owned road
 * @param t tile to check
 * @pre IsTileType(t, MP_ROAD)
 * @return true iff tile has road and the road is owned by a town
 */
inline bool HasTownOwnedRoad(TileIndex t)
{
	return HasTileRoadType(t, RTT_ROAD) && IsRoadOwner(t, RTT_ROAD, OWNER_TOWN);
}

/**
 * Checks if a DisallowedRoadDirections is valid.
 *
 * @param wc The value to check
 * @return true if the given value is a valid DisallowedRoadDirections.
 */
inline bool IsValidDisallowedRoadDirections(DisallowedRoadDirections drt)
{
	return drt < DRD_END;
}

/**
 * Gets the disallowed directions
 * @param t the tile to get the directions from
 * @return the disallowed directions
 */
inline DisallowedRoadDirections GetDisallowedRoadDirections(TileIndex t)
{
	dbg_assert_tile(IsNormalRoad(t), t);
	return (DisallowedRoadDirections)GB(_m[t].m5, 4, 2);
}

/**
 * Sets the disallowed directions
 * @param t   the tile to set the directions for
 * @param drd the disallowed directions
 */
inline void SetDisallowedRoadDirections(TileIndex t, DisallowedRoadDirections drd)
{
	assert_tile(IsNormalRoad(t), t);
	assert(drd < DRD_END);
	SB(_m[t].m5, 4, 2, drd);
}

enum RoadCachedOneWayState {
	RCOWS_NORMAL = 0,             ///< Road is not one-way
	RCOWS_NON_JUNCTION_A,         ///< Road is one-way in 'A' direction (Trackdir 8-13 is allowed, same as DRD_SOUTHBOUND for straight road pieces)
	RCOWS_NON_JUNCTION_B,         ///< Road is one-way in 'B' direction (Trackdir 0-5 is allowed, same as DRD_NORTHBOUND for straight road pieces)
	RCOWS_NO_ACCESS,              ///< Road is disallowed in both directions
	RCOWS_SIDE_JUNCTION,          ///< Road is a one-way side junction
	RCOWS_SIDE_JUNCTION_NO_EXIT,  ///< Road is a one-way side junction, with no side exit
};

/**
 * Get the road cached one-way state
 * @param t tile to get the state from
 * @pre MayHaveRoad(t)
 * @return road cached one way state
 */
inline RoadCachedOneWayState GetRoadCachedOneWayState(TileIndex t)
{
	dbg_assert(MayHaveRoad(t));
	return (RoadCachedOneWayState)GB(_me[t].m8, 12, 3);
}

/**
 * Set the road cached one-way state
 * @param t tile to set the state of
 * @param rcows road cached one way state
 * @pre MayHaveRoad(t)
 */
inline void SetRoadCachedOneWayState(TileIndex t, RoadCachedOneWayState rcows)
{
	assert(MayHaveRoad(t));
	SB(_me[t].m8, 12, 3, rcows);
}

/**
 * Get the road axis of a level crossing.
 * @param t The tile to query.
 * @pre IsLevelCrossing(t)
 * @return The axis of the road.
 */
inline Axis GetCrossingRoadAxis(TileIndex t)
{
	dbg_assert_tile(IsLevelCrossing(t), t);
	return (Axis)GB(_m[t].m5, 0, 1);
}

/**
 * Get the rail axis of a level crossing.
 * @param t The tile to query.
 * @pre IsLevelCrossing(t)
 * @return The axis of the rail.
 */
inline Axis GetCrossingRailAxis(TileIndex t)
{
	dbg_assert_tile(IsLevelCrossing(t), t);
	return OtherAxis((Axis)GetCrossingRoadAxis(t));
}

/**
 * Get the road bits of a level crossing.
 * @param tile The tile to query.
 * @return The present road bits.
 */
inline RoadBits GetCrossingRoadBits(TileIndex tile)
{
	return GetCrossingRoadAxis(tile) == AXIS_X ? ROAD_X : ROAD_Y;
}

/**
 * Get the rail track of a level crossing.
 * @param tile The tile to query.
 * @return The rail track.
 */
inline Track GetCrossingRailTrack(TileIndex tile)
{
	return AxisToTrack(GetCrossingRailAxis(tile));
}

/**
 * Get the rail track bits of a level crossing.
 * @param tile The tile to query.
 * @return The rail track bits.
 */
inline TrackBits GetCrossingRailBits(TileIndex tile)
{
	return AxisToTrackBits(GetCrossingRailAxis(tile));
}


/**
 * Get the reservation state of the rail crossing
 * @param t the crossing tile
 * @return reservation state
 * @pre IsLevelCrossingTile(t)
 */
inline bool HasCrossingReservation(TileIndex t)
{
	dbg_assert_tile(IsLevelCrossingTile(t), t);
	return HasBit(_m[t].m5, 4);
}

/**
 * Set the reservation state of the rail crossing
 * @note Works for both waypoints and rail depots
 * @param t the crossing tile
 * @param b the reservation state
 * @pre IsLevelCrossingTile(t)
 */
inline void SetCrossingReservation(TileIndex t, bool b)
{
	assert_tile(IsLevelCrossingTile(t), t);
	AssignBit(_m[t].m5, 4, b);
}

/**
 * Get the reserved track bits for a rail crossing
 * @param t the tile
 * @pre IsLevelCrossingTile(t)
 * @return reserved track bits
 */
inline TrackBits GetCrossingReservationTrackBits(TileIndex t)
{
	return HasCrossingReservation(t) ? GetCrossingRailBits(t) : TRACK_BIT_NONE;
}

/**
 * Check if the level crossing is barred.
 * @param t The tile to query.
 * @pre IsLevelCrossing(t)
 * @return True if the level crossing is barred.
 */
inline bool IsCrossingBarred(TileIndex t)
{
	dbg_assert_tile(IsLevelCrossing(t), t);
	return HasBit(_m[t].m5, 5);
}

/**
 * Set the bar state of a level crossing.
 * @param t The tile to modify.
 * @param barred True if the crossing should be barred, false otherwise.
 * @pre IsLevelCrossing(t)
 */
inline void SetCrossingBarred(TileIndex t, bool barred)
{
	assert_tile(IsLevelCrossing(t), t);
	AssignBit(_m[t].m5, 5, barred);
}

/**
 * Check if the level crossing is possibly occupied by road vehicle(s).
 * @param t The tile to query.
 * @pre IsLevelCrossing(t)
 * @return True if the level crossing is marked as occupied. This may return false positives.
 */
inline bool IsCrossingPossiblyOccupiedByRoadVehicle(TileIndex t)
{
	dbg_assert_tile(IsLevelCrossing(t), t);
	return HasBit(_m[t].m5, 1);
}

/**
 * Set whether the level crossing is occupied by road vehicle(s).
 * @param t The tile to modify.
 * @param barred True if the crossing should be marked as occupied, false otherwise.
 * @pre IsLevelCrossing(t)
 */
inline void SetCrossingOccupiedByRoadVehicle(TileIndex t, bool occupied)
{
	assert_tile(IsLevelCrossing(t), t);
	AssignBit(_m[t].m5, 1, occupied);
}

/** Check if a road tile has snow/desert. */
#define IsOnDesert IsOnSnow
/**
 * Check if a road tile has snow/desert.
 * @param t The tile to query.
 * @return True if the tile has snow/desert.
 */
inline bool IsOnSnow(TileIndex t)
{
	return HasBit(_me[t].m7, 5);
}

/** Toggle the snow/desert state of a road tile. */
#define ToggleDesert ToggleSnow
/**
 * Toggle the snow/desert state of a road tile.
 * @param t The tile to change.
 */
inline void ToggleSnow(TileIndex t)
{
	ToggleBit(_me[t].m7, 5);
}


/** The possible road side decorations. */
enum Roadside {
	ROADSIDE_BARREN           = 0, ///< Road on barren land
	ROADSIDE_GRASS            = 1, ///< Road on grass
	ROADSIDE_PAVED            = 2, ///< Road with paved sidewalks
	ROADSIDE_STREET_LIGHTS    = 3, ///< Road with street lights on paved sidewalks
	// 4 is unused for historical reasons
	ROADSIDE_TREES            = 5, ///< Road with trees on paved sidewalks
	ROADSIDE_GRASS_ROAD_WORKS = 6, ///< Road on grass with road works
	ROADSIDE_PAVED_ROAD_WORKS = 7, ///< Road with sidewalks and road works
};

/**
 * Get the decorations of a road.
 * @param tile The tile to query.
 * @return The road decoration of the tile.
 */
inline Roadside GetRoadside(TileIndex tile)
{
	return (Roadside)GB(_me[tile].m6, 3, 3);
}

/**
 * Set the decorations of a road.
 * @param tile The tile to change.
 * @param s    The new road decoration of the tile.
 */
inline void SetRoadside(TileIndex tile, Roadside s)
{
	SB(_me[tile].m6, 3, 3, s);
}

/**
 * Check if a tile has road works.
 * @param t The tile to check.
 * @return True if the tile has road works in progress.
 */
inline bool HasRoadWorks(TileIndex t)
{
	return GetRoadside(t) >= ROADSIDE_GRASS_ROAD_WORKS;
}

/**
 * Increase the progress counter of road works.
 * @param t The tile to modify.
 * @return True if the road works are in the last stage.
 */
inline bool IncreaseRoadWorksCounter(TileIndex t)
{
	AB(_me[t].m7, 0, 4, 1);

	return GB(_me[t].m7, 0, 4) == 15;
}

/**
 * Start road works on a tile.
 * @param t The tile to start the work on.
 * @pre !HasRoadWorks(t)
 */
inline void StartRoadWorks(TileIndex t)
{
	assert_tile(!HasRoadWorks(t), t);
	/* Remove any trees or lamps in case or roadwork */
	switch (GetRoadside(t)) {
		case ROADSIDE_BARREN:
		case ROADSIDE_GRASS:  SetRoadside(t, ROADSIDE_GRASS_ROAD_WORKS); break;
		default:              SetRoadside(t, ROADSIDE_PAVED_ROAD_WORKS); break;
	}
}

/**
 * Terminate road works on a tile.
 * @param t Tile to stop the road works on.
 * @pre HasRoadWorks(t)
 */
inline void TerminateRoadWorks(TileIndex t)
{
	assert_tile(HasRoadWorks(t), t);
	SetRoadside(t, (Roadside)(GetRoadside(t) - ROADSIDE_GRASS_ROAD_WORKS + ROADSIDE_GRASS));
	/* Stop the counter */
	SB(_me[t].m7, 0, 4, 0);
}


/**
 * Get the direction of the exit of a road depot.
 * @param t The tile to query.
 * @return Diagonal direction of the depot exit.
 */
inline DiagDirection GetRoadDepotDirection(TileIndex t)
{
	dbg_assert_tile(IsRoadDepot(t), t);
	return (DiagDirection)GB(_m[t].m5, 0, 2);
}


RoadBits GetAnyRoadBits(TileIndex tile, RoadTramType rtt, bool straight_tunnel_bridge_entrance = false);

/**
 * Set the road road type of a tile.
 * @param t The tile to change.
 * @param rt The road type to set.
 */
inline void SetRoadTypeRoad(TileIndex t, RoadType rt)
{
	assert(MayHaveRoad(t));
	assert(rt == INVALID_ROADTYPE || RoadTypeIsRoad(rt));
	SB(_m[t].m4, 0, 6, rt);
}

/**
 * Set the tram road type of a tile.
 * @param t The tile to change.
 * @param rt The road type to set.
 */
inline void SetRoadTypeTram(TileIndex t, RoadType rt)
{
	assert(MayHaveRoad(t));
	assert(rt == INVALID_ROADTYPE || RoadTypeIsTram(rt));
	SB(_me[t].m8, 6, 6, rt);
}

/**
 * Set the road type of a tile.
 * @param t The tile to change.
 * @param rtt Set road or tram type.
 * @param rt The road type to set.
 */
inline void SetRoadType(TileIndex t, RoadTramType rtt, RoadType rt)
{
	if (rtt == RTT_TRAM) {
		SetRoadTypeTram(t, rt);
	} else {
		SetRoadTypeRoad(t, rt);
	}
}

/**
 * Set the present road types of a tile.
 * @param t  The tile to change.
 * @param road_rt The road roadtype to set for the tile.
 * @param tram_rt The tram roadtype to set for the tile.
 */
inline void SetRoadTypes(TileIndex t, RoadType road_rt, RoadType tram_rt)
{
	SetRoadTypeRoad(t, road_rt);
	SetRoadTypeTram(t, tram_rt);
}

/**
 * Make a normal road tile.
 * @param t       Tile to make a normal road.
 * @param bits    Road bits to set for all present road types.
 * @param road_rt The road roadtype to set for the tile.
 * @param tram_rt The tram roadtype to set for the tile.
 * @param town    Town ID if the road is a town-owned road.
 * @param road    New owner of road.
 * @param tram    New owner of tram tracks.
 */
inline void MakeRoadNormal(TileIndex t, RoadBits bits, RoadType road_rt, RoadType tram_rt, TownID town, Owner road, Owner tram)
{
	SetTileType(t, MP_ROAD);
	SetTileOwner(t, road);
	_m[t].m2 = town;
	_m[t].m3 = (tram_rt != INVALID_ROADTYPE ? bits : 0);
	_m[t].m5 = (road_rt != INVALID_ROADTYPE ? bits : 0) | ROAD_TILE_NORMAL << 6;
	SB(_me[t].m6, 2, 4, 0);
	_me[t].m7 = 0;
	SetRoadTypes(t, road_rt, tram_rt);
	SetRoadOwner(t, RTT_TRAM, tram);
}

/**
 * Make a level crossing.
 * @param t       Tile to make a level crossing.
 * @param road    New owner of road.
 * @param tram    New owner of tram tracks.
 * @param rail    New owner of the rail track.
 * @param roaddir Axis of the road.
 * @param rat     New rail type.
 * @param road_rt The road roadtype to set for the tile.
 * @param tram_rt The tram roadtype to set for the tile.
 * @param town    Town ID if the road is a town-owned road.
 */
inline void MakeRoadCrossing(TileIndex t, Owner road, Owner tram, Owner rail, Axis roaddir, RailType rat, RoadType road_rt, RoadType tram_rt, TownID town)
{
	SetTileType(t, MP_ROAD);
	SetTileOwner(t, rail);
	_m[t].m2 = town;
	_m[t].m3 = 0;
	_m[t].m4 = INVALID_ROADTYPE;
	_m[t].m5 = ROAD_TILE_CROSSING << 6 | roaddir;
	SB(_me[t].m6, 2, 4, 0);
// 	_me[t].m7 = road;
	SB(_me[t].m7, 0, 5, road);
	uint8_t owner_add = ((road&0x3FF)>>5);
	_me[t].m9 &= ~(0xe0);
	_me[t].m9 |= (owner_add&0x1f)<<5;
	_me[t].m10 &= ~(0x03);
	_me[t].m10 |= (owner_add&0x1f)>>3;
	_me[t].m8 = INVALID_ROADTYPE << 6 | rat;
	SetRoadTypes(t, road_rt, tram_rt);
	SetRoadOwner(t, RTT_TRAM, tram);
}

/**
 * Make a road depot.
 * @param t     Tile to make a level crossing.
 * @param owner New owner of the depot.
 * @param did   New depot ID.
 * @param dir   Direction of the depot exit.*
 * @param rt    Road type of the depot.
 */
inline void MakeRoadDepot(TileIndex t, Owner owner, DepotID did, DiagDirection dir, RoadType rt)
{
	SetTileType(t, MP_ROAD);
	SetTileOwner(t, owner);
	_m[t].m2 = did;
	_m[t].m3 = 0;
	_m[t].m4 = INVALID_ROADTYPE;
	_m[t].m5 = ROAD_TILE_DEPOT << 6 | dir;
	SB(_me[t].m6, 2, 4, 0);
	SB(_me[t].m7, 0, 5, owner);
	uint8_t owner_add = ((owner&0x3FF)>>5);
	_me[t].m9 &= ~(0xe0);
	_me[t].m9 |= (owner_add&0x1f)<<5;
	_me[t].m10 &= ~(0x03);
	_me[t].m10 |= (owner_add&0x1f)>>3;	_me[t].m8 = INVALID_ROADTYPE << 6;
	SetRoadType(t, GetRoadTramType(rt), rt);
	SetRoadOwner(t, RTT_TRAM, owner);
}

#endif /* ROAD_MAP_H */
