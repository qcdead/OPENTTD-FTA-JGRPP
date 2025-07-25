/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file rail_cmd.cpp Handling of rail tiles. */

#include "stdafx.h"
#include "viewport_func.h"
#include "command_func.h"
#include "depot_base.h"
#include "pathfinder/yapf/yapf_cache.h"
#include "newgrf_debug.h"
#include "newgrf_railtype.h"
#include "train.h"
#include "autoslope.h"
#include "water.h"
#include "tunnelbridge_map.h"
#include "bridge_signal_map.h"
#include "vehicle_func.h"
#include "sound_func.h"
#include "tunnelbridge.h"
#include "elrail_func.h"
#include "town.h"
#include "pbs.h"
#include "company_base.h"
#include "core/backup_type.hpp"
#include "date_func.h"
#include "core/container_func.hpp"
#include "strings_func.h"
#include "company_gui.h"
#include "object_map.h"
#include "tracerestrict.h"
#include "programmable_signals.h"
#include "spritecache.h"
#include "core/container_func.hpp"
#include "news_func.h"
#include "scope.h"
#include "newgrf_newsignals.h"
#include "pathfinder/water_regions.h"
#include "landscape_cmd.h"
#include "rail_cmd.h"

#include "table/strings.h"
#include "table/railtypes.h"
#include "table/track_land.h"

#include <vector>

#include "safeguards.h"

/** Helper type for lists/vectors of trains */
typedef std::vector<Train *> TrainList;

RailTypeInfo _railtypes[RAILTYPE_END];
std::vector<RailType> _sorted_railtypes;
TileIndex _rail_track_endtile; ///< The end of a rail track; as hidden return from the rail build/remove command for GUI purposes.
RailTypes _railtypes_hidden_mask;

/**
 * Reset all rail type information to its default values.
 */
void ResetRailTypes()
{
	static_assert(lengthof(_original_railtypes) <= lengthof(_railtypes));

	auto insert = std::copy(std::begin(_original_railtypes), std::end(_original_railtypes), std::begin(_railtypes));
	std::fill(insert, std::end(_railtypes), RailTypeInfo{});

	_railtypes_hidden_mask = RAILTYPES_NONE;
}

void ResolveRailTypeGUISignalSprites(RailTypeInfo *rti, uint8_t style, PalSpriteID signals[SIGTYPE_END][2][2])
{
	/* Array of default GUI signal sprite numbers. */
	const SpriteID _signal_lookup[2][SIGTYPE_END] = {
		{SPR_IMG_SIGNAL_ELECTRIC_NORM,  SPR_IMG_SIGNAL_ELECTRIC_ENTRY, SPR_IMG_SIGNAL_ELECTRIC_EXIT,
		 SPR_IMG_SIGNAL_ELECTRIC_COMBO, SPR_IMG_SIGNAL_ELECTRIC_PBS,   SPR_IMG_SIGNAL_ELECTRIC_PBS_OWAY,
		 SPR_IMG_SIGNAL_ELECTRIC_PROG,  SPR_IMG_SIGNAL_ELECTRIC_NO_ENTRY},

		{SPR_IMG_SIGNAL_SEMAPHORE_NORM,  SPR_IMG_SIGNAL_SEMAPHORE_ENTRY, SPR_IMG_SIGNAL_SEMAPHORE_EXIT,
		 SPR_IMG_SIGNAL_SEMAPHORE_COMBO, SPR_IMG_SIGNAL_SEMAPHORE_PBS,   SPR_IMG_SIGNAL_SEMAPHORE_PBS_OWAY,
		 SPR_IMG_SIGNAL_SEMAPHORE_PROG,  SPR_IMG_SIGNAL_SEMAPHORE_NO_ENTRY},
	};

	auto default_sprite = [&](SignalVariant var, SignalType type) -> SpriteID {
		SpriteID spr = _signal_lookup[var][type];
		if (_settings_client.gui.show_all_signal_default == SSDM_ON) {
			if (type == SIGTYPE_PROG) {
				spr += SPR_DUP_PROGSIGNAL_BASE - SPR_PROGSIGNAL_BASE;
			} else if (type == SIGTYPE_NO_ENTRY) {
				spr += SPR_DUP_EXTRASIGNAL_BASE - SPR_EXTRASIGNAL_BASE;
			} else if (var == SIG_ELECTRIC && type == SIGTYPE_BLOCK) {
				spr += SPR_DUP_ORIGINAL_SIGNALS_BASE - SPR_ORIGINAL_SIGNALS_BASE;
			} else {
				spr += SPR_DUP_SIGNALS_BASE - SPR_SIGNALS_BASE;
			}
		}
		return spr;
	};

	for (SignalType type = SIGTYPE_BLOCK; type < SIGTYPE_END; type = (SignalType)(type + 1)) {
		for (SignalVariant var = SIG_ELECTRIC; var <= SIG_SEMAPHORE; var = (SignalVariant)(var + 1)) {
			PalSpriteID red   = GetCustomSignalSprite(rti, INVALID_TILE, type, var, 0, { CSSC_GUI }, style).sprite;
			if (red.sprite != 0) {
				signals[type][var][0] = { red.sprite + SIGNAL_TO_SOUTH, red.pal };
			} else {
				signals[type][var][0] = { default_sprite(var, type), PAL_NONE };
			}
			if (type == SIGTYPE_NO_ENTRY) {
				signals[type][var][1] = signals[type][var][0];
				continue;
			}
			PalSpriteID green = GetCustomSignalSprite(rti, INVALID_TILE, type, var, 255, { CSSC_GUI }, style).sprite;
			if (green.sprite != 0) {
				signals[type][var][1] = { green.sprite + SIGNAL_TO_SOUTH, green.pal };
			} else {
				signals[type][var][1] = { default_sprite(var, type) + 1, PAL_NONE };
			}
		}
	}
}

void ResolveRailTypeGUISprites(RailTypeInfo *rti)
{
	SpriteID cursors_base = GetCustomRailSprite(rti, INVALID_TILE, RTSG_CURSORS);
	if (cursors_base != 0) {
		rti->gui_sprites.build_ns_rail = cursors_base +  0;
		rti->gui_sprites.build_x_rail  = cursors_base +  1;
		rti->gui_sprites.build_ew_rail = cursors_base +  2;
		rti->gui_sprites.build_y_rail  = cursors_base +  3;
		rti->gui_sprites.auto_rail     = cursors_base +  4;
		rti->gui_sprites.build_depot   = cursors_base +  5;
		rti->gui_sprites.build_tunnel  = cursors_base +  6;
		rti->gui_sprites.convert_rail  = cursors_base +  7;
		rti->cursor.rail_ns   = cursors_base +  8;
		rti->cursor.rail_swne = cursors_base +  9;
		rti->cursor.rail_ew   = cursors_base + 10;
		rti->cursor.rail_nwse = cursors_base + 11;
		rti->cursor.autorail  = cursors_base + 12;
		rti->cursor.depot     = cursors_base + 13;
		rti->cursor.tunnel    = cursors_base + 14;
		rti->cursor.convert   = cursors_base + 15;
	}

	ResolveRailTypeGUISignalSprites(rti, 0, rti->gui_sprites.signals);
}

/**
 * Compare railtypes based on their sorting order.
 * @param first  The railtype to compare to.
 * @param second The railtype to compare.
 * @return True iff the first should be sorted before the second.
 */
static bool CompareRailTypes(const RailType &first, const RailType &second)
{
	if (_settings_client.gui.sort_track_types_by_speed) {
		RailType rt[2] = { first, second };
		uint sort_value[2];

		for (int i = 0; i < 2; ++i) {
			// Last sort by speed
			sort_value[i] = (GetRailTypeInfo(rt[i])->max_speed != 0) ? GetRailTypeInfo(rt[i])->max_speed : UINT16_MAX;

			// Inside those categories filter by compatibility with each other.
			if (!HasPowerOnRail(rt[i], rt[(i + 1) % 2])) {
				sort_value[i] += (1 << 16);
			}

			// We sort by Rail, Electric and others
			if (!HasPowerOnRail(rt[i], RAILTYPE_RAIL)) {
				sort_value[i] += (1 << 17);

				if (!HasPowerOnRail(rt[i], RAILTYPE_ELECTRIC)) {
					sort_value[i] += (1 << 18);

					if (!HasPowerOnRail(rt[i], RAILTYPE_MONO) && HasPowerOnRail(rt[i], RAILTYPE_MAGLEV)) {
						sort_value[i] += (1 << 19);
					}
				}
			}

			// Then Mono
			if (HasPowerOnRail(rt[i], RAILTYPE_MONO)) {
				sort_value[i] += (1 << 20);
			}

			// Maglev is second last
			if (HasPowerOnRail(rt[i], RAILTYPE_MAGLEV)) {
				sort_value[i] += (1 << 21);
			}

			// All no-speed tracks (like planning and lifted) go to the end
			if (GetRailTypeInfo(rt[i])->max_speed == 0) {
				sort_value[i] += (1 << 22);
			}
		}

		return std::tie(sort_value[0], first) < std::tie(sort_value[1], second);
	} else {
		return std::tie(GetRailTypeInfo(first)->sorting_order, first) < std::tie(GetRailTypeInfo(second)->sorting_order, second);
	}
}

void SortRailTypes()
{
	std::sort(_sorted_railtypes.begin(), _sorted_railtypes.end(), CompareRailTypes);
}

void UpdateRailGuiSprites()
{
	for (RailType rt = RAILTYPE_BEGIN; rt != RAILTYPE_END; rt++) {
		ResolveRailTypeGUISprites(&_railtypes[rt]);
	}

	for (uint8_t style = 0; style < _num_new_signal_styles; style++) {
		ResolveRailTypeGUISignalSprites(nullptr, style + 1, _new_signal_styles[style].signals);
	}
}

/**
 * Resolve sprites of custom rail types
 */
void InitRailTypes()
{
	UpdateRailGuiSprites();

	_sorted_railtypes.clear();
	for (RailType rt = RAILTYPE_BEGIN; rt != RAILTYPE_END; rt++) {
		bool hidden = _railtypes[rt].flags.Test(RailTypeFlag::Hidden);
		if (hidden) SetBit(_railtypes_hidden_mask, rt);
		if (_railtypes[rt].label != 0 && !hidden) {
			_sorted_railtypes.push_back(rt);
		}
	}
	SortRailTypes();

	for (RailType rt = RAILTYPE_BEGIN; rt != RAILTYPE_END; rt++) {
		_railtypes[rt].all_compatible_railtypes = _railtypes[rt].compatible_railtypes;
	}
	for (RailType rt = RAILTYPE_BEGIN; rt != RAILTYPE_END; rt++) {
		RailTypes compatible = _railtypes[rt].all_compatible_railtypes;
		RailTypes to_check = compatible;
		while (to_check) {
			RailType i = (RailType)FindFirstBit(to_check);
			to_check = KillFirstBit(to_check);
			RailTypes new_types = _railtypes[i].compatible_railtypes & (~compatible);
			to_check |= new_types;
			compatible |= new_types;
		}
		RailTypes to_update = compatible;
		while (to_update) {
			RailType i = (RailType)FindFirstBit(to_update);
			to_update = KillFirstBit(to_update);
			_railtypes[i].all_compatible_railtypes = compatible;
		}
	}
}

/**
 * Allocate a new rail type label
 */
RailType AllocateRailType(RailTypeLabel label)
{
	for (RailType rt = RAILTYPE_BEGIN; rt != RAILTYPE_END; rt++) {
		RailTypeInfo *rti = &_railtypes[rt];

		if (rti->label == 0) {
			/* Set up new rail type */
			*rti = _original_railtypes[RAILTYPE_RAIL];
			rti->label = label;
			rti->alternate_labels.clear();

			/* Make us compatible with ourself. */
			rti->powered_railtypes    = (RailTypes)(1LL << rt);
			rti->compatible_railtypes = (RailTypes)(1LL << rt);

			/* We also introduce ourself. */
			rti->introduces_railtypes = (RailTypes)(1LL << rt);

			/* Default sort order; order of allocation, but with some
			 * offsets so it's easier for NewGRF to pick a spot without
			 * changing the order of other (original) rail types.
			 * The << is so you can place other railtypes in between the
			 * other railtypes, the 7 is to be able to place something
			 * before the first (default) rail type. */
			rti->sorting_order = rt << 4 | 7;

			if (label == 'TELE' || label == 'PIPE' || label == 'WIRE') rti->ctrl_flags.Set(RailTypeCtrlFlag::NoRealisticBraking);
			return rt;
		}
	}

	return INVALID_RAILTYPE;
}

static const uint8_t _track_sloped_sprites[14] = {
	14, 15, 22, 13,
	 0, 21, 17, 12,
	23,  0, 18, 20,
	19, 16
};


/*         4
 *     ---------
 *    |\       /|
 *    | \    1/ |
 *    |  \   /  |
 *    |   \ /   |
 *  16|    \    |32
 *    |   / \2  |
 *    |  /   \  |
 *    | /     \ |
 *    |/       \|
 *     ---------
 *         8
 */



/* MAP2 byte:    abcd???? => Signal On? Same coding as map3lo
 * MAP3LO byte:  abcd???? => Signal Exists?
 *               a and b are for diagonals, upper and left,
 *               one for each direction. (ie a == NE->SW, b ==
 *               SW->NE, or v.v., I don't know. b and c are
 *               similar for lower and right.
 * MAP2 byte:    ????abcd => Type of ground.
 * MAP3LO byte:  ????abcd => Type of rail.
 * MAP5:         00abcdef => rail
 *               01abcdef => rail w/ signals
 *               10uuuuuu => unused
 *               11uuuudd => rail depot
 */

/**
 * Tests if a vehicle interacts with the specified track.
 * All track bits interact except parallel #TRACK_BIT_HORZ or #TRACK_BIT_VERT.
 *
 * @param tile The tile.
 * @param track The track.
 * @return Succeeded command (no train found), or a failed command (a train was found).
 */
static CommandCost EnsureNoTrainOnTrack(TileIndex tile, Track track)
{
	TrackBits rail_bits = TrackToTrackBits(track);
	return EnsureNoTrainOnTrackBits(tile, rail_bits);
}

struct CheckTrackCombinationRailTypeChanges {
	RailType convert_to = INVALID_RAILTYPE;
	RailType primary = INVALID_RAILTYPE;
	RailType secondary = INVALID_RAILTYPE;
};

/**
 * Check that the new track bits may be built.
 * @param tile %Tile to build on.
 * @param to_build New track bits.
 * @param railtype New rail type.
 * @param disable_dual_rail_type Whether dual rail types are disabled.
 * @param flags    Flags of the operation.
 * @return Succeeded or failed command.
 */
static CommandCost CheckTrackCombination(TileIndex tile, TrackBits to_build, RailType railtype, bool disable_dual_rail_type,
		DoCommandFlag flags, bool auto_remove_signals, CheckTrackCombinationRailTypeChanges &changes)
{
	if (!IsPlainRail(tile)) return CommandCost(STR_ERROR_IMPOSSIBLE_TRACK_COMBINATION);

	/* So, we have a tile with tracks on it (and possibly signals). Let's see
	 * what tracks first */
	TrackBits current = GetTrackBits(tile); // The current track layout.
	TrackBits future = current | to_build;  // The track layout we want to build.

	/* Are we really building something new? */
	if (current == future) {
		/* Nothing new is being built */
		if (IsCompatibleRail(GetTileRailTypeByTrackBit(tile, to_build), railtype)) {
			return CommandCost(STR_ERROR_ALREADY_BUILT);
		} else {
			return CommandCost(STR_ERROR_IMPOSSIBLE_TRACK_COMBINATION);
		}
	}

	/* These combinations are always allowed, unless disable_dual_rail_type is set */
	if ((future == TRACK_BIT_HORZ || future == TRACK_BIT_VERT) && !disable_dual_rail_type) {
		if (flags & DC_EXEC) {
			if (to_build & TRACK_BIT_RT_1) {
				RailType current_rt = GetRailType(tile);
				changes.primary = railtype;
				changes.secondary = current_rt;
			} else {
				changes.secondary = railtype;
			}
		}
		return CommandCost();
	}

	/* Let's see if we may build this */
	if (HasSignals(tile) && !auto_remove_signals) {
		/* If we are not allowed to overlap (flag is on for ai companies or we have
		 * signals on the tile), check that */
		if (future != TRACK_BIT_HORZ && future != TRACK_BIT_VERT) {
			return CommandCost(STR_ERROR_MUST_REMOVE_SIGNALS_FIRST);
		}
	}

	RailType rt = INVALID_RAILTYPE;
	if (current == TRACK_BIT_HORZ || current == TRACK_BIT_VERT) {
		RailType rt1 = GetRailType(tile);
		if (!IsCompatibleRail(rt1, railtype)) return CommandCost(STR_ERROR_IMPOSSIBLE_TRACK_COMBINATION);

		RailType rt2 = GetSecondaryRailType(tile);
		if (!IsCompatibleRail(rt2, railtype)) return CommandCost(STR_ERROR_IMPOSSIBLE_TRACK_COMBINATION);

		if (rt1 != rt2) {
			/* Two different railtypes present */
			if ((railtype == rt1 || HasPowerOnRail(rt1, railtype)) && (railtype == rt2 || HasPowerOnRail(rt2, railtype))) {
				rt = railtype;
			} else if ((railtype == rt1 || HasPowerOnRail(railtype, rt1)) && HasPowerOnRail(rt2, rt1)) {
				rt = railtype = rt1;
			} else if ((railtype == rt2 || HasPowerOnRail(railtype, rt2)) && HasPowerOnRail(rt1, rt2)) {
				rt = railtype = rt2;
			} else {
				return CommandCost(STR_ERROR_IMPOSSIBLE_TRACK_COMBINATION);
			}
		} else if (railtype == rt1) {
			/* Nothing to do */
			rt = INVALID_RAILTYPE;
		} else if (HasPowerOnRail(railtype, rt1)) {
			/* Try to keep existing railtype */
			railtype = rt1;
			rt = INVALID_RAILTYPE;
		} else if (HasPowerOnRail(rt1, railtype)) {
			rt = railtype;
		} else {
			return CommandCost(STR_ERROR_IMPOSSIBLE_TRACK_COMBINATION);
		}
	} else {
		rt = GetRailType(tile);

		if (railtype == rt) {
			/* Nothing to do */
			rt = INVALID_RAILTYPE;
		} else if (!IsCompatibleRail(rt, railtype)) {
			return CommandCost(STR_ERROR_IMPOSSIBLE_TRACK_COMBINATION);
		} else if (HasPowerOnRail(railtype, rt)) {
			/* Try to keep existing railtype */
			railtype = rt;
			rt = INVALID_RAILTYPE;
		} else if (HasPowerOnRail(rt, railtype)) {
			rt = railtype;
		} else {
			return CommandCost(STR_ERROR_IMPOSSIBLE_TRACK_COMBINATION);
		}
	}

	CommandCost ret;
	if (rt != INVALID_RAILTYPE) {
		ret = Command<CMD_CONVERT_RAIL>::Do(flags & ~DC_EXEC, tile, tile, rt, false);
		if (ret.Failed()) return ret;
		changes.convert_to = rt;
	}

	if (flags & DC_EXEC) {
		changes.primary = railtype;
		changes.secondary = railtype;
	}

	return ret;
}


/** Valid TrackBits on a specific (non-steep)-slope without foundation */
static const TrackBits _valid_tracks_without_foundation[15] = {
	TRACK_BIT_ALL,
	TRACK_BIT_RIGHT,
	TRACK_BIT_UPPER,
	TRACK_BIT_X,

	TRACK_BIT_LEFT,
	TRACK_BIT_NONE,
	TRACK_BIT_Y,
	TRACK_BIT_LOWER,

	TRACK_BIT_LOWER,
	TRACK_BIT_Y,
	TRACK_BIT_NONE,
	TRACK_BIT_LEFT,

	TRACK_BIT_X,
	TRACK_BIT_UPPER,
	TRACK_BIT_RIGHT,
};

/** Valid TrackBits on a specific (non-steep)-slope with leveled foundation */
static const TrackBits _valid_tracks_on_leveled_foundation[15] = {
	TRACK_BIT_NONE,
	TRACK_BIT_LEFT,
	TRACK_BIT_LOWER,
	TRACK_BIT_Y | TRACK_BIT_LOWER | TRACK_BIT_LEFT,

	TRACK_BIT_RIGHT,
	TRACK_BIT_ALL,
	TRACK_BIT_X | TRACK_BIT_LOWER | TRACK_BIT_RIGHT,
	TRACK_BIT_ALL,

	TRACK_BIT_UPPER,
	TRACK_BIT_X | TRACK_BIT_UPPER | TRACK_BIT_LEFT,
	TRACK_BIT_ALL,
	TRACK_BIT_ALL,

	TRACK_BIT_Y | TRACK_BIT_UPPER | TRACK_BIT_RIGHT,
	TRACK_BIT_ALL,
	TRACK_BIT_ALL
};

/**
 * Checks if a track combination is valid on a specific slope and returns the needed foundation.
 *
 * @param tileh Tile slope.
 * @param bits  Trackbits.
 * @return Needed foundation or FOUNDATION_INVALID if track/slope combination is not allowed.
 */
Foundation GetRailFoundation(Slope tileh, TrackBits bits)
{
	if (bits == TRACK_BIT_NONE) return FOUNDATION_NONE;

	if (IsSteepSlope(tileh)) {
		/* Test for inclined foundations */
		if (bits == TRACK_BIT_X) return FOUNDATION_INCLINED_X;
		if (bits == TRACK_BIT_Y) return FOUNDATION_INCLINED_Y;

		/* Get higher track */
		Corner highest_corner = GetHighestSlopeCorner(tileh);
		TrackBits higher_track = CornerToTrackBits(highest_corner);

		/* Only higher track? */
		if (bits == higher_track) return HalftileFoundation(highest_corner);

		/* Overlap with higher track? */
		if (TracksOverlap(bits | higher_track)) return FOUNDATION_INVALID;

		/* either lower track or both higher and lower track */
		return ((bits & higher_track) != 0 ? FOUNDATION_STEEP_BOTH : FOUNDATION_STEEP_LOWER);
	} else {
		if ((~_valid_tracks_without_foundation[tileh] & bits) == 0) return FOUNDATION_NONE;

		bool valid_on_leveled = ((~_valid_tracks_on_leveled_foundation[tileh] & bits) == 0);

		Corner track_corner;
		switch (bits) {
			case TRACK_BIT_LEFT:  track_corner = CORNER_W; break;
			case TRACK_BIT_LOWER: track_corner = CORNER_S; break;
			case TRACK_BIT_RIGHT: track_corner = CORNER_E; break;
			case TRACK_BIT_UPPER: track_corner = CORNER_N; break;

			case TRACK_BIT_HORZ:
				if (tileh == SLOPE_N) return HalftileFoundation(CORNER_N);
				if (tileh == SLOPE_S) return HalftileFoundation(CORNER_S);
				return (valid_on_leveled ? FOUNDATION_LEVELED : FOUNDATION_INVALID);

			case TRACK_BIT_VERT:
				if (tileh == SLOPE_W) return HalftileFoundation(CORNER_W);
				if (tileh == SLOPE_E) return HalftileFoundation(CORNER_E);
				return (valid_on_leveled ? FOUNDATION_LEVELED : FOUNDATION_INVALID);

			case TRACK_BIT_X:
				if (IsSlopeWithOneCornerRaised(tileh)) return FOUNDATION_INCLINED_X;
				return (valid_on_leveled ? FOUNDATION_LEVELED : FOUNDATION_INVALID);

			case TRACK_BIT_Y:
				if (IsSlopeWithOneCornerRaised(tileh)) return FOUNDATION_INCLINED_Y;
				return (valid_on_leveled ? FOUNDATION_LEVELED : FOUNDATION_INVALID);

			default:
				return (valid_on_leveled ? FOUNDATION_LEVELED : FOUNDATION_INVALID);
		}
		/* Single diagonal track */

		/* Track must be at least valid on leveled foundation */
		if (!valid_on_leveled) return FOUNDATION_INVALID;

		/* If slope has three raised corners, build leveled foundation */
		if (IsSlopeWithThreeCornersRaised(tileh)) return FOUNDATION_LEVELED;

		/* If neighboured corners of track_corner are lowered, build halftile foundation */
		if ((tileh & SlopeWithThreeCornersRaised(OppositeCorner(track_corner))) == SlopeWithOneCornerRaised(track_corner)) return HalftileFoundation(track_corner);

		/* else special anti-zig-zag foundation */
		return SpecialRailFoundation(track_corner);
	}
}


/**
 * Tests if a track can be build on a tile.
 *
 * @param tileh Tile slope.
 * @param rail_bits Tracks to build.
 * @param existing Tracks already built.
 * @param tile Tile (used for water test)
 * @return Error message or cost for foundation building.
 */
static CommandCost CheckRailSlope(Slope tileh, TrackBits rail_bits, TrackBits existing, TileIndex tile)
{
	/* don't allow building on the lower side of a coast */
	if (GetFloodingBehaviour(tile) != FLOOD_NONE) {
		if (!IsSteepSlope(tileh) && ((~_valid_tracks_on_leveled_foundation[tileh] & (rail_bits | existing)) != 0)) return CommandCost(STR_ERROR_CAN_T_BUILD_ON_WATER);
	}

	Foundation f_new = GetRailFoundation(tileh, rail_bits | existing);

	/* check track/slope combination */
	if ((f_new == FOUNDATION_INVALID) ||
			((f_new != FOUNDATION_NONE) && (!_settings_game.construction.build_on_slopes))) {
		return CommandCost(STR_ERROR_LAND_SLOPED_IN_WRONG_DIRECTION);
	}

	Foundation f_old = GetRailFoundation(tileh, existing);
	return CommandCost(EXPENSES_CONSTRUCTION, f_new != f_old ? _price[PR_BUILD_FOUNDATION] : (Money)0);
}

bool IsValidFlatRailBridgeHeadTrackBits(Slope normalised_slope, DiagDirection bridge_direction, TrackBits tracks)
{
	/* bridge_direction  c1  c2
	 *                0   0   1
	 *                1   0   3
	 *                2   2   3
	 *                3   2   1
	 */
	const Corner c1 = (Corner) (bridge_direction & 2);
	const Corner c2 = (Corner) (((bridge_direction + 1) & 2) + 1);
	auto test_corner = [&](Corner c) -> bool {
		if (normalised_slope & SlopeWithOneCornerRaised(c)) return true;
		Slope effective_slope = normalised_slope | SlopeWithOneCornerRaised(OppositeCorner(c));
		assert(effective_slope < lengthof(_valid_tracks_on_leveled_foundation));
		return (_valid_tracks_on_leveled_foundation[effective_slope] & tracks) == tracks;
	};
	return test_corner(c1) && test_corner(c2);
}

/* Validate functions for rail building */
static inline bool ValParamTrackOrientation(Track track)
{
	return IsValidTrack(track);
}

static void ReReserveTrainPath(Train *v)
{
	const bool consider_stopped = (((v->vehstatus & VS_STOPPED) && v->cur_speed == 0) || v->current_order.IsAnyLoadingType());
	const bool at_safe_waiting_position = IsSafeWaitingPosition(v, v->tile, v->GetVehicleTrackdir(), true, _settings_game.pf.forbid_90_deg);

	/* Don't extend the train's path if it's stopped or loading, and at a safe position. */
	if (consider_stopped && at_safe_waiting_position) return;

	if (!at_safe_waiting_position || (v->UsingRealisticBraking() && !v->StopFoundAtVehiclePosition())) {
		TryPathReserve(v, true);
	}
}

/**
 * Build a single piece of rail
 * @param flags operation to perform
 * @param tile tile  to build on
 * @param railtype railtype of being built piece (normal, mono, maglev)
 * @param track track-orientation
 * @param build_flags Build flags
 * @return the cost of this operation or an error
 */
CommandCost CmdBuildSingleRail(DoCommandFlag flags, TileIndex tile, RailType railtype, Track track, BuildRailTrackFlags build_flags)
{
	CommandCost cost(EXPENSES_CONSTRUCTION);

	_rail_track_endtile = INVALID_TILE;

	if (!ValParamRailType(railtype) || !ValParamTrackOrientation(track)) return CMD_ERROR;

	Slope tileh = GetTileSlope(tile);
	TrackBits trackbit = TrackToTrackBits(track);

	switch (GetTileType(tile)) {
		case MP_RAILWAY: {
			CommandCost ret = CheckTileOwnership(tile);
			if (ret.Failed()) return ret;

			if (!IsPlainRail(tile)) return Command<CMD_LANDSCAPE_CLEAR>::Do(flags, tile); // just get appropriate error message

			CheckTrackCombinationRailTypeChanges changes;
			ret = CheckTrackCombination(tile, trackbit, railtype, HasFlag(build_flags, BuildRailTrackFlags::NoDualRailType), flags, HasFlag(build_flags, BuildRailTrackFlags::AutoRemoveSignals), changes);
			if (ret.Succeeded()) {
				cost.AddCost(ret);
				ret = EnsureNoTrainOnTrack(tile, track);
			}
			if (ret.Failed()) {
				if (ret.GetErrorMessage() == STR_ERROR_ALREADY_BUILT) _rail_track_endtile = tile;
				return ret;
			}

			ret = CheckRailSlope(tileh, trackbit, GetTrackBits(tile), tile);
			if (ret.Failed()) return ret;
			cost.AddCost(ret);

			if (HasSignals(tile) && TracksOverlap(GetTrackBits(tile) | TrackToTrackBits(track))) {
				/* If adding the new track causes any overlap, all signals must be removed first */
				if (!HasFlag(build_flags, BuildRailTrackFlags::AutoRemoveSignals)) return CommandCost(STR_ERROR_MUST_REMOVE_SIGNALS_FIRST);

				for (Track track_it = TRACK_BEGIN; track_it < TRACK_END; track_it++) {
					if (HasTrack(tile, track_it) && HasSignalOnTrack(tile, track_it)) {
						CommandCost ret_remove_signals = Command<CMD_REMOVE_SINGLE_SIGNAL>::Do(flags, tile, track_it, RemoveSignalFlags::None);
						if (ret_remove_signals.Failed()) return ret_remove_signals;
						cost.AddCost(ret_remove_signals);
					}
				}
			}

			if (flags & DC_EXEC) {
				if (changes.convert_to != INVALID_RAILTYPE) {
					/* The cost is already accounted for and a test already done in CheckTrackCombination */
					CommandCost ret = Command<CMD_CONVERT_RAIL>::Do(flags, tile, tile, changes.convert_to, false);
					assert(ret.Succeeded());
				}
				if (changes.primary != INVALID_RAILTYPE) SetRailType(tile, changes.primary);
				if (changes.secondary != INVALID_RAILTYPE) SetSecondaryRailType(tile, changes.secondary);

				SetRailGroundType(tile, RAIL_GROUND_BARREN);
				TrackBits bits = GetTrackBits(tile);
				TrackBits newbits = bits | trackbit;
				SetTrackBits(tile, newbits);
				if (newbits == TRACK_BIT_HORZ || newbits == TRACK_BIT_VERT) {
					Company::Get(GetTileOwner(tile))->infrastructure.rail[GetPlainRailParallelTrackRailTypeByTrackBit(tile, trackbit)]++;
				} else {
					/* Subtract old infrastructure count. */
					uint pieces = CountBits(bits);
					if (TracksOverlap(bits)) pieces *= pieces;
					Company::Get(GetTileOwner(tile))->infrastructure.rail[GetRailType(tile)] -= pieces;
					/* Add new infrastructure count. */
					pieces = CountBits(newbits);
					if (TracksOverlap(newbits)) pieces *= pieces;
					Company::Get(GetTileOwner(tile))->infrastructure.rail[GetRailType(tile)] += pieces;
				}
				DirtyCompanyInfrastructureWindows(GetTileOwner(tile));
			}
			break;
		}

		case MP_TUNNELBRIDGE: {
			CommandCost ret = CheckTileOwnership(tile);
			if (ret.Failed()) return ret;

			if (HasFlag(build_flags, BuildRailTrackFlags::NoCustomBridgeHeads) || !_settings_game.construction.rail_custom_bridge_heads || !IsFlatRailBridgeHeadTile(tile)) {
				return Command<CMD_LANDSCAPE_CLEAR>::Do(flags, tile); // just get appropriate error message
			}

			const DiagDirection entrance_dir = GetTunnelBridgeDirection(tile);
			const TrackBits axial_track = DiagDirToDiagTrackBits(entrance_dir);
			const TrackBits existing = GetCustomBridgeHeadTrackBits(tile);
			const TrackBits future = existing | trackbit;

			const bool secondary_piece = ((future == TRACK_BIT_HORZ || future == TRACK_BIT_VERT) && (future != existing));

			if (!secondary_piece && !HasFlag(build_flags, BuildRailTrackFlags::NoDualRailType)) {
				if (!IsCompatibleRail(GetRailType(tile), railtype)) return CommandCost(STR_ERROR_IMPOSSIBLE_TRACK_COMBINATION);
				if (GetRailType(tile) != railtype && !HasPowerOnRail(railtype, GetRailType(tile))) return CommandCost(STR_ERROR_CAN_T_CONVERT_RAIL);
				if (GetSecondaryTunnelBridgeTrackBits(tile) != TRACK_BIT_NONE) {
					if (!IsCompatibleRail(GetSecondaryRailType(tile), railtype)) return CommandCost(STR_ERROR_IMPOSSIBLE_TRACK_COMBINATION);
					if (GetRailType(tile) != railtype && !HasPowerOnRail(railtype, GetSecondaryRailType(tile))) return CommandCost(STR_ERROR_CAN_T_CONVERT_RAIL);
				}
			}

			if (existing == future) return CommandCost(STR_ERROR_ALREADY_BUILT);

			if (IsTunnelBridgeWithSignalSimulation(tile)) {
				if (future != TRACK_BIT_HORZ && future != TRACK_BIT_VERT) {
					return CommandCost(STR_ERROR_MUST_REMOVE_SIGNALS_FIRST);
				}
			}

			if ((trackbit & ~axial_track) && !_settings_game.construction.build_on_slopes) {
				return CommandCost(STR_ERROR_LAND_SLOPED_IN_WRONG_DIRECTION);
			}

			/* Steep slopes behave the same as slopes with one corner raised. */
			const Slope normalised_tileh = IsSteepSlope(tileh) ? SlopeWithOneCornerRaised(GetHighestSlopeCorner(tileh)) : tileh;

			if (!IsValidFlatRailBridgeHeadTrackBits(normalised_tileh, GetTunnelBridgeDirection(tile), future)) {
				return CommandCost(STR_ERROR_LAND_SLOPED_IN_WRONG_DIRECTION);
			}

			const TileIndex other_end = GetOtherTunnelBridgeEnd(tile);
			if (!secondary_piece) {
				ret = TunnelBridgeIsFree(tile, other_end);
				if (ret.Failed()) return ret;
			}

			if (flags & DC_EXEC) {
				SubtractRailTunnelBridgeInfrastructure(tile, other_end);
				SetCustomBridgeHeadTrackBits(tile, future);
				SetTunnelBridgeGroundBits(tile, IsRailCustomBridgeHead(tile) ? 2 : 0);
				if (secondary_piece) {
					SetSecondaryRailType(tile, railtype);
				}
				AddRailTunnelBridgeInfrastructure(tile, other_end);
				DirtyCompanyInfrastructureWindows(_current_company);
			}

			break;
		}

		case MP_ROAD: {
			/* Level crossings may only be built on these slopes */
			if (!HasBit(VALID_LEVEL_CROSSING_SLOPES, tileh)) return CommandCost(STR_ERROR_LAND_SLOPED_IN_WRONG_DIRECTION);

			if (!_settings_game.construction.crossing_with_competitor && _current_company != OWNER_DEITY) {
				CommandCost ret = CheckTileOwnership(tile);
				if (ret.Failed()) return ret;
			}

			CommandCost ret = EnsureNoVehicleOnGround(tile);
			if (ret.Failed()) return ret;

			if (IsNormalRoad(tile)) {
				if (HasRoadWorks(tile)) return CommandCost(STR_ERROR_ROAD_WORKS_IN_PROGRESS);

				if (GetDisallowedRoadDirections(tile) != DRD_NONE) return CommandCost(STR_ERROR_CROSSING_ON_ONEWAY_ROAD);

				if (RailNoLevelCrossings(railtype)) return CommandCost(STR_ERROR_CROSSING_DISALLOWED_RAIL);

				RoadType roadtype_road = GetRoadTypeRoad(tile);
				RoadType roadtype_tram = GetRoadTypeTram(tile);

				if (roadtype_road != INVALID_ROADTYPE && RoadNoLevelCrossing(roadtype_road)) return CommandCost(STR_ERROR_CROSSING_DISALLOWED_ROAD);
				if (roadtype_tram != INVALID_ROADTYPE && RoadNoLevelCrossing(roadtype_tram)) return CommandCost(STR_ERROR_CROSSING_DISALLOWED_ROAD);

				RoadBits road = GetRoadBits(tile, RTT_ROAD);
				RoadBits tram = GetRoadBits(tile, RTT_TRAM);
				if ((track == TRACK_X && ((road | tram) & ROAD_X) == 0) ||
						(track == TRACK_Y && ((road | tram) & ROAD_Y) == 0)) {
					Owner road_owner = GetRoadOwner(tile, RTT_ROAD);
					Owner tram_owner = GetRoadOwner(tile, RTT_TRAM);
					/* Disallow breaking end-of-line of someone else
					 * so trams can still reverse on this tile. */
					if (Company::IsValidID(tram_owner) && HasExactlyOneBit(tram)) {
						ret = CheckOwnership(tram_owner);
						if (ret.Failed()) return ret;
					}

					uint num_new_road_pieces = (road != ROAD_NONE) ? 2 - CountBits(road) : 0;
					if (num_new_road_pieces > 0) {
						cost.AddCost(num_new_road_pieces * RoadBuildCost(roadtype_road));
					}

					uint num_new_tram_pieces = (tram != ROAD_NONE) ? 2 - CountBits(tram) : 0;
					if (num_new_tram_pieces > 0) {
						cost.AddCost(num_new_tram_pieces * RoadBuildCost(roadtype_tram));
					}

					if (flags & DC_EXEC) {
						MakeRoadCrossing(tile, road_owner, tram_owner, _current_company, (track == TRACK_X ? AXIS_Y : AXIS_X), railtype, roadtype_road, roadtype_tram, GetTownIndex(tile));
						UpdateLevelCrossing(tile, false);
						MarkDirtyAdjacentLevelCrossingTilesOnAdd(tile, GetCrossingRoadAxis(tile));
						Company::Get(_current_company)->infrastructure.rail[railtype] += LEVELCROSSING_TRACKBIT_FACTOR;
						DirtyCompanyInfrastructureWindows(_current_company);
						if (num_new_road_pieces > 0 && Company::IsValidID(road_owner)) {
							assert(roadtype_road != INVALID_ROADTYPE);
							Company::Get(road_owner)->infrastructure.road[roadtype_road] += num_new_road_pieces;
							DirtyCompanyInfrastructureWindows(road_owner);
						}
						if (num_new_tram_pieces > 0 && Company::IsValidID(tram_owner)) {
							assert(roadtype_tram != INVALID_ROADTYPE);
							Company::Get(tram_owner)->infrastructure.road[roadtype_tram] += num_new_tram_pieces;
							DirtyCompanyInfrastructureWindows(tram_owner);
						}
						UpdateRoadCachedOneWayStatesAroundTile(tile);
					}
					break;
				}
			}

			if (IsLevelCrossing(tile) && GetCrossingRailBits(tile) == trackbit) {
				_rail_track_endtile = tile;
				return CommandCost(STR_ERROR_ALREADY_BUILT);
			}
			[[fallthrough]];
		}

		default: {
			/* Will there be flat water on the lower halftile? */
			bool water_ground = IsTileType(tile, MP_WATER) && IsSlopeWithOneCornerRaised(tileh);

			CommandCost ret = CheckRailSlope(tileh, trackbit, TRACK_BIT_NONE, tile);
			if (ret.Failed()) return ret;
			cost.AddCost(ret);

			ret = Command<CMD_LANDSCAPE_CLEAR>::Do(flags | DC_ALLOW_REMOVE_WATER, tile);
			if (ret.Failed()) return ret;
			cost.AddCost(ret);

			if (water_ground) {
				cost.AddCost(-_price[PR_CLEAR_WATER]);
				cost.AddCost(_price[PR_CLEAR_ROUGH]);
			}

			if (flags & DC_EXEC) {
				MakeRailNormal(tile, _current_company, trackbit, railtype);
				if (water_ground) {
					SetRailGroundType(tile, RAIL_GROUND_WATER);
					if (IsPossibleDockingTile(tile)) CheckForDockingTile(tile);
				}
				Company::Get(_current_company)->infrastructure.rail[railtype]++;
				DirtyCompanyInfrastructureWindows(_current_company);
			}
			break;
		}
	}

	if (flags & DC_EXEC) {
		MarkTileDirtyByTile(tile);
		AddTrackToSignalBuffer(tile, track, _current_company);
		YapfNotifyTrackLayoutChange(tile, track);
	}

	cost.AddCost(RailBuildCost(railtype));
	_rail_track_endtile = tile;
	return cost;
}

/**
 * Remove a single piece of track
 * @param flags operation to perform
 * @param tile tile to remove track from
 * @param track rail orientation
 * @return the cost of this operation or an error
 */
CommandCost CmdRemoveSingleRail(DoCommandFlag flags, TileIndex tile, Track track)
{
	CommandCost cost(EXPENSES_CONSTRUCTION);
	bool crossing = false;

	_rail_track_endtile = INVALID_TILE;

	if (!ValParamTrackOrientation(track)) return CMD_ERROR;
	TrackBits trackbit = TrackToTrackBits(track);

	/* Need to read tile owner now because it may change when the rail is removed
	 * Also, in case of floods, _current_company != owner
	 * There may be invalid tiletype even in exec run (when removing long track),
	 * so do not call GetTileOwner(tile) in any case here */
	Owner owner = INVALID_OWNER;

	Train *v = nullptr;

	switch (GetTileType(tile)) {
		case MP_ROAD: {
			if (!IsLevelCrossing(tile) || GetCrossingRailBits(tile) != trackbit) return CommandCost(STR_ERROR_THERE_IS_NO_RAILROAD_TRACK);

			if (_current_company != OWNER_WATER) {
				CommandCost ret = CheckTileOwnership(tile);
				if (ret.Failed()) return ret;
			}

			if (!(flags & DC_BANKRUPT)) {
				CommandCost ret = EnsureNoVehicleOnGround(tile);
				if (ret.Failed()) return ret;
			}

			cost.AddCost(RailClearCost(GetRailType(tile)));

			if (HasReservedTracks(tile, trackbit)) {
				v = GetTrainForReservation(tile, track);
				if (v != nullptr) {
					CommandCost ret = CheckTrainReservationPreventsTrackModification(v);
					if (ret.Failed()) return ret;
				}
			}

			if (flags & DC_EXEC) {
				UpdateAdjacentLevelCrossingTilesOnRemove(tile, GetCrossingRoadAxis(tile));
				if (v != nullptr) FreeTrainTrackReservation(v);

				owner = GetTileOwner(tile);
				Company::Get(owner)->infrastructure.rail[GetRailType(tile)] -= LEVELCROSSING_TRACKBIT_FACTOR;
				DirtyCompanyInfrastructureWindows(owner);
				MakeRoadNormal(tile, GetCrossingRoadBits(tile), GetRoadTypeRoad(tile), GetRoadTypeTram(tile), GetTownIndex(tile), GetRoadOwner(tile, RTT_ROAD), GetRoadOwner(tile, RTT_TRAM));
				DeleteNewGRFInspectWindow(GSF_RAILTYPES, tile.base());
				UpdateRoadCachedOneWayStatesAroundTile(tile);
			}
			break;
		}

		case MP_RAILWAY: {
			TrackBits present;
			/* There are no rails present at depots. */
			if (!IsPlainRail(tile)) return CommandCost(STR_ERROR_THERE_IS_NO_RAILROAD_TRACK);

			if (_current_company != OWNER_WATER) {
				CommandCost ret = CheckTileOwnership(tile);
				if (ret.Failed()) return ret;
			}

			CommandCost ret = EnsureNoTrainOnTrack(tile, track);
			if (ret.Failed()) return ret;

			present = GetTrackBits(tile);
			if ((present & trackbit) == 0) return CommandCost(STR_ERROR_THERE_IS_NO_RAILROAD_TRACK);
			if (present == (TRACK_BIT_X | TRACK_BIT_Y)) crossing = true;

			cost.AddCost(RailClearCost(GetTileRailTypeByTrackBit(tile, trackbit)));

			if (HasReservedTracks(tile, trackbit)) {
				v = GetTrainForReservation(tile, track);
				if (v != nullptr) {
					CommandCost ret = CheckTrainReservationPreventsTrackModification(v);
					if (ret.Failed()) return ret;
				}
			}

			/* Charge extra to remove signals on the track, if they are there */
			if (HasSignalOnTrack(tile, track)) {
				CommandCost ret_remove_signals = Command<CMD_REMOVE_SINGLE_SIGNAL>::Do(flags, tile, track, RemoveSignalFlags::None);
				if (ret_remove_signals.Failed()) return ret_remove_signals;
				cost.AddCost(ret_remove_signals);
			}

			if (flags & DC_EXEC) {
				if (v != nullptr) FreeTrainTrackReservation(v);

				owner = GetTileOwner(tile);

				if (present == TRACK_BIT_HORZ || present == TRACK_BIT_VERT) {
					Company::Get(owner)->infrastructure.rail[GetTileRailTypeByTrackBit(tile, trackbit)]--;
					present ^= trackbit;
					SetRailType(tile, GetTileRailTypeByTrackBit(tile, present));
				} else {
					/* Subtract old infrastructure count. */
					uint pieces = CountBits(present);
					if (TracksOverlap(present)) pieces *= pieces;
					Company::Get(owner)->infrastructure.rail[GetRailType(tile)] -= pieces;
					/* Add new infrastructure count. */
					present ^= trackbit;
					pieces = CountBits(present);
					if (TracksOverlap(present)) pieces *= pieces;
					Company::Get(owner)->infrastructure.rail[GetRailType(tile)] += pieces;
				}
				DirtyCompanyInfrastructureWindows(owner);

				if (present == 0) {
					Slope tileh = GetTileSlope(tile);
					/* If there is flat water on the lower halftile, convert the tile to shore so the water remains */
					if (GetRailGroundType(tile) == RAIL_GROUND_WATER && IsSlopeWithOneCornerRaised(tileh)) {
						bool docking = IsDockingTile(tile);
						MakeShore(tile);
						SetDockingTile(tile, docking);
					} else {
						DoClearSquare(tile);
					}
					DeleteNewGRFInspectWindow(GSF_RAILTYPES, tile.base());
				} else {
					SetTrackBits(tile, present);
					SetTrackReservation(tile, GetRailReservationTrackBits(tile) & present);

					if (present == TRACK_BIT_HORZ || present == TRACK_BIT_VERT) {
						SetSecondaryRailType(tile, GetRailType(tile));
					}
				}
			}
			break;
		}

		case MP_TUNNELBRIDGE: {
			CommandCost ret = CheckTileOwnership(tile);
			if (ret.Failed()) return ret;

			if (!IsFlatRailBridgeHeadTile(tile) || GetCustomBridgeHeadTrackBits(tile) == DiagDirToDiagTrackBits(GetTunnelBridgeDirection(tile))) {
				return Command<CMD_LANDSCAPE_CLEAR>::Do(flags, tile); // just get appropriate error message
			}

			const TrackBits present = GetCustomBridgeHeadTrackBits(tile);
			if ((present & trackbit) == 0) return CommandCost(STR_ERROR_THERE_IS_NO_RAILROAD_TRACK);
			if (present == (TRACK_BIT_X | TRACK_BIT_Y)) crossing = true;

			const TrackBits future = present ^ trackbit;

			if ((GetAcrossBridgePossibleTrackBits(tile) & future) == 0) return Command<CMD_LANDSCAPE_CLEAR>::Do(flags, tile); // just get appropriate error message

			const TileIndex other_end = GetOtherTunnelBridgeEnd(tile);
			if (present == TRACK_BIT_HORZ || present == TRACK_BIT_VERT) {
				ret = EnsureNoTrainOnTrack(tile, track);
			} else {
				ret = TunnelBridgeIsFree(tile, other_end);
			}
			if (ret.Failed()) return ret;

			if (HasReservedTracks(tile, trackbit)) {
				v = GetTrainForReservation(tile, track);
				if (v != nullptr) {
					CommandCost ret = CheckTrainReservationPreventsTrackModification(v);
					if (ret.Failed()) return ret;
				}
			}

			cost.AddCost(RailClearCost(GetTileRailTypeByTrackBit(tile, trackbit)));

			if (flags & DC_EXEC) {
				SubtractRailTunnelBridgeInfrastructure(tile, other_end);
				owner = GetTileOwner(tile);

				if (v != nullptr) FreeTrainTrackReservation(v);

				if (future == TRACK_BIT_HORZ || future == TRACK_BIT_VERT) {
					// Changing to two separate tracks with separate rail types
					SetSecondaryRailType(tile, GetRailType(tile));
				}

				SetCustomBridgeHeadTrackBits(tile, future);
				SetTunnelBridgeGroundBits(tile, IsRailCustomBridgeHead(tile) ? 2 : 0);
				AddRailTunnelBridgeInfrastructure(tile, other_end);
				DirtyCompanyInfrastructureWindows(_current_company);
			}

			break;
		}

		default: return CommandCost(STR_ERROR_THERE_IS_NO_RAILROAD_TRACK);
	}

	if (flags & DC_EXEC) {
		/* if we got that far, 'owner' variable is set correctly */
		assert(Company::IsValidID(owner));

		MarkTileDirtyByTile(tile);
		if (crossing) {
			/* crossing is set when only TRACK_BIT_X and TRACK_BIT_Y are set. As we
			 * are removing one of these pieces, we'll need to update signals for
			 * both directions explicitly, as after the track is removed it won't
			 * 'connect' with the other piece. */
			AddTrackToSignalBuffer(tile, TRACK_X, owner);
			AddTrackToSignalBuffer(tile, TRACK_Y, owner);
			YapfNotifyTrackLayoutChange(tile, TRACK_X);
			YapfNotifyTrackLayoutChange(tile, TRACK_Y);
		} else {
			AddTrackToSignalBuffer(tile, track, owner);
			YapfNotifyTrackLayoutChange(tile, track);
		}

		if (v != nullptr) ReReserveTrainPath(v);
	}

	_rail_track_endtile = tile;
	return cost;
}


/**
 * Called from water_cmd if a non-flat rail-tile gets flooded and should be converted to shore.
 * The function floods the lower halftile, if the tile has a halftile foundation.
 *
 * @param t The tile to flood.
 * @return true if something was flooded.
 */
bool FloodHalftile(TileIndex t)
{
	assert_tile(IsPlainRailTile(t), t);

	bool flooded = false;
	if (GetRailGroundType(t) == RAIL_GROUND_WATER) return flooded;

	Slope tileh = GetTileSlope(t);
	TrackBits rail_bits = GetTrackBits(t);

	if (IsSlopeWithOneCornerRaised(tileh)) {
		TrackBits lower_track = CornerToTrackBits(OppositeCorner(GetHighestSlopeCorner(tileh)));

		TrackBits to_remove = lower_track & rail_bits;
		if (to_remove != 0) {
			Backup<CompanyID> cur_company(_current_company, OWNER_WATER, FILE_LINE);
			flooded = Command<CMD_REMOVE_SINGLE_RAIL>::Do(DC_EXEC, t, FindFirstTrack(to_remove)).Succeeded();
			cur_company.Restore();
			if (!flooded) return flooded; // not yet floodable
			rail_bits = rail_bits & ~to_remove;
			if (rail_bits == 0) {
				MakeShore(t);
				MarkTileDirtyByTile(t);
				return flooded;
			}
		}

		if (IsNonContinuousFoundation(GetRailFoundation(tileh, rail_bits))) {
			flooded = true;
			SetRailGroundType(t, RAIL_GROUND_WATER);
			MarkTileDirtyByTile(t);
		}
	} else {
		/* Make shore on steep slopes and 'three-corners-raised'-slopes. */
		if (ApplyFoundationToSlope(GetRailFoundation(tileh, rail_bits), tileh) == 0) {
			if (IsSteepSlope(tileh) || IsSlopeWithThreeCornersRaised(tileh)) {
				flooded = true;
				SetRailGroundType(t, RAIL_GROUND_WATER);
				MarkTileDirtyByTile(t, VMDF_NOT_MAP_MODE);
			}
		}
	}
	return flooded;
}

static const TileIndexDiffC _trackdelta[] = {
	{ -1,  0 }, {  0,  1 }, { -1,  0 }, {  0,  1 }, {  1,  0 }, {  0,  1 },
	{  0,  0 },
	{  0,  0 },
	{  1,  0 }, {  0, -1 }, {  0, -1 }, {  1,  0 }, {  0, -1 }, { -1,  0 },
	{  0,  0 },
	{  0,  0 }
};


static CommandCost ValidateAutoDrag(Trackdir *trackdir, TileIndex start, TileIndex end)
{
	int x = TileX(start);
	int y = TileY(start);
	int ex = TileX(end);
	int ey = TileY(end);

	if (!ValParamTrackOrientation(TrackdirToTrack(*trackdir))) return CMD_ERROR;

	/* calculate delta x,y from start to end tile */
	int dx = ex - x;
	int dy = ey - y;

	/* calculate delta x,y for the first direction */
	int trdx = _trackdelta[*trackdir].x;
	int trdy = _trackdelta[*trackdir].y;

	if (!IsDiagonalTrackdir(*trackdir)) {
		trdx += _trackdelta[*trackdir ^ 1].x;
		trdy += _trackdelta[*trackdir ^ 1].y;
	}

	/* validate the direction */
	while ((trdx <= 0 && dx > 0) ||
			(trdx >= 0 && dx < 0) ||
			(trdy <= 0 && dy > 0) ||
			(trdy >= 0 && dy < 0)) {
		if (!HasBit(*trackdir, 3)) { // first direction is invalid, try the other
			SetBit(*trackdir, 3); // reverse the direction
			trdx = -trdx;
			trdy = -trdy;
		} else { // other direction is invalid too, invalid drag
			return CMD_ERROR;
		}
	}

	/* (for diagonal tracks, this is already made sure of by above test), but:
	 * for non-diagonal tracks, check if the start and end tile are on 1 line */
	if (!IsDiagonalTrackdir(*trackdir)) {
		trdx = _trackdelta[*trackdir].x;
		trdy = _trackdelta[*trackdir].y;
		if (abs(dx) != abs(dy) && abs(dx) + abs(trdy) != abs(dy) + abs(trdx)) return CMD_ERROR;
	}

	return CommandCost();
}

/**
 * Build or remove a stretch of railroad tracks.
 * @param flags operation to perform
 * @param tile start tile of drag
 * @param end_tile end tile of drag
 * @param railtype railroad type normal/maglev (0 = normal, 1 = mono, 2 = maglev), only used for building
 * @param track track-orientation
 * @param remove remove tracks?
 * @param auto_remove_signals false = error on signal in the way, true = auto remove signals when in the way, only used for building
 * @param fail_on_obstacle false = build starting from and up to an obstacle, true = fail if an obstacle is found (used for AIs)
 * @return the cost of this operation or an error
 */
static CommandCost CmdRailTrackHelper(DoCommandFlag flags, TileIndex tile, TileIndex end_tile, RailType railtype, Track track, bool remove, BuildRailTrackFlags build_flags, bool fail_on_obstacle)
{
	CommandCost total_cost(EXPENSES_CONSTRUCTION);

	_rail_track_endtile = INVALID_TILE;

	if ((!remove && !ValParamRailType(railtype)) || !ValParamTrackOrientation(track)) return CMD_ERROR;
	if (end_tile >= Map::Size()) return CMD_ERROR;
	Trackdir trackdir = TrackToTrackdir(track);

	CommandCost ret = ValidateAutoDrag(&trackdir, tile, end_tile);
	if (ret.Failed()) return ret;

	bool had_success = false;
	CommandCost last_error = CMD_ERROR;
	for (;;) {
		TileIndex last_endtile = _rail_track_endtile;
		CommandCost ret;
		if (remove) {
			ret = Command<CMD_REMOVE_SINGLE_RAIL>::Do(flags, tile, TrackdirToTrack(trackdir));
		} else {
			ret = Command<CMD_BUILD_SINGLE_RAIL>::Do(flags, tile, railtype, TrackdirToTrack(trackdir), build_flags);
		}

		if (ret.Failed()) {
			last_error = ret;
			if (_rail_track_endtile == INVALID_TILE) _rail_track_endtile = last_endtile;
			if (last_error.GetErrorMessage() != STR_ERROR_ALREADY_BUILT && !remove) {
				if (fail_on_obstacle) return last_error;
				if (had_success) break; // Keep going if we haven't constructed any rail yet, skipping the start of the drag
			}

			/* Ownership errors are more important. */
			if (last_error.GetErrorMessage() == STR_ERROR_OWNED_BY && remove) break;
		} else {
			had_success = true;
			total_cost.AddCost(ret);
		}

		if (tile == end_tile) break;

		tile += ToTileIndexDiff(_trackdelta[trackdir]);

		/* toggle railbit for the non-diagonal tracks */
		if (!IsDiagonalTrackdir(trackdir)) ToggleBit(trackdir, 0);
	}

	if (had_success) return total_cost;
	return last_error;
}

/**
 * Build rail on a stretch of track.
 * Stub for the unified rail builder/remover
 * @param flags operation to perform
 * @param end_tile end tile of drag
 * @param start_tile start tile of drag
 * @param railtype railroad type normal/maglev (0 = normal, 1 = mono, 2 = maglev), only used for building
 * @param track track-orientation
 * @param build_flags build flags.
 * @param fail_on_obstacle false = error on signal in the way, true = auto remove signals when in the way

 * @see CmdRailTrackHelper
 */
CommandCost CmdBuildRailroadTrack(DoCommandFlag flags, TileIndex end_tile, TileIndex start_tile, RailType railtype, Track track, BuildRailTrackFlags build_flags, bool fail_on_obstacle)
{
	return CmdRailTrackHelper(flags, start_tile, end_tile, railtype, track, false, build_flags, fail_on_obstacle);
}

/**
 * Remove rail from a stretch of track.
 * Stub for the unified rail builder/remover
 * @param flags operation to perform
 * @param end_tile end tile of drag
 * @param start_tile start tile of drag
 * @param track track-orientation
 * @return the cost of this operation or an error
 * @see CmdRailTrackHelper
 */
CommandCost CmdRemoveRailroadTrack(DoCommandFlag flags, TileIndex end_tile, TileIndex start_tile, Track track)
{
	return CmdRailTrackHelper(flags, start_tile, end_tile, INVALID_RAILTYPE, track, true, BuildRailTrackFlags::None, false);
}

/**
 * Build a train depot
 * @param flags operation to perform
 * @param tile position of the train depot
 * @param railtype rail type
 * @param dir entrance direction
 * @return the cost of this operation or an error
 *
 * @todo When checking for the tile slope,
 * distinguish between "Flat land required" and "land sloped in wrong direction"
 */
CommandCost CmdBuildTrainDepot(DoCommandFlag flags, TileIndex tile, RailType railtype, DiagDirection dir)
{
	/* check railtype and valid direction for depot (0 through 3), 4 in total */
	if (!ValParamRailType(railtype) || !IsValidDiagDirection(dir)) return CMD_ERROR;

	Slope tileh = GetTileSlope(tile);

	CommandCost cost(EXPENSES_CONSTRUCTION);

	/* Prohibit construction if
	 * The tile is non-flat AND
	 * 1) build-on-slopes is disabled
	 * 2) the tile is steep i.e. spans two height levels
	 * 3) the exit points in the wrong direction
	 */

	if (tileh != SLOPE_FLAT) {
		if (!_settings_game.construction.build_on_slopes || !CanBuildDepotByTileh(dir, tileh)) {
			return CommandCost(STR_ERROR_FLAT_LAND_REQUIRED);
		}
		cost.AddCost(_price[PR_BUILD_FOUNDATION]);
	}

	cost.AddCost(Command<CMD_LANDSCAPE_CLEAR>::Do(flags, tile));
	if (cost.Failed()) return cost;

	if (IsBridgeAbove(tile)) return CommandCost(STR_ERROR_MUST_DEMOLISH_BRIDGE_FIRST);

	if (!Depot::CanAllocateItem()) return CMD_ERROR;

	if (flags & DC_EXEC) {
		Depot *d = new Depot(tile);
		d->build_date = CalTime::CurDate();

		MakeRailDepot(tile, _current_company, d->index, dir, railtype);
		MarkTileDirtyByTile(tile);
		MakeDefaultName(d);

		Company::Get(_current_company)->infrastructure.rail[railtype]++;
		DirtyCompanyInfrastructureWindows(_current_company);

		AddSideToSignalBuffer(tile, INVALID_DIAGDIR, _current_company);
		YapfNotifyTrackLayoutChange(tile, DiagDirToDiagTrack(dir));
	}

	cost.AddCost(_price[PR_BUILD_DEPOT_TRAIN]);
	cost.AddCost(RailBuildCost(railtype));
	return cost;
}

static void ClearBridgeTunnelSignalSimulation(TileIndex entrance, TileIndex exit)
{
	if (IsBridge(entrance)) ClearBridgeEntranceSimulatedSignals(entrance);
	ClrTunnelBridgeSignalSimulationEntrance(entrance);
	ClrTunnelBridgeSignalSimulationExit(exit);
	SetTunnelBridgeSignalSpecialPropagationFlag(entrance, false);
	SetTunnelBridgeSignalSpecialPropagationFlag(exit, false);
}

static void SetupBridgeTunnelSignalSimulation(TileIndex entrance, TileIndex exit)
{
	SetTunnelBridgeSignalSimulationEntrance(entrance);
	SetTunnelBridgeEntranceSignalState(entrance, SIGNAL_STATE_GREEN);
	SetTunnelBridgeSignalSimulationExit(exit);
	UpdateTunnelBridgeSignalSpecialPropagationFlag(entrance, false);
	UpdateTunnelBridgeSignalSpecialPropagationFlag(exit, false);
	if (_extra_aspects > 0) {
		SetTunnelBridgeEntranceSignalAspect(entrance, 0);
		UpdateAspectDeferred(entrance, GetTunnelBridgeEntranceTrackdir(entrance));
	}
}

/**
 * Build signals, alternate between double/single, signal/semaphore,
 * pre/exit/combo-signals, and what-else not. If the rail piece does not
 * have any signals, signal cycling is ignored
 * @param flags operation to perform
 * @param tile tile where to build the signals
 * @param track track-orientation
 * @param sigtype type of the signal
 * @param sigvar variant of signal type (normal/semaphore)
 * @param signal_style signal style
 * @param signal_spacing signal spacing (for tunnel/bridge)
 * @param build_flags build flags
 * @param which_signals cycle through which signal sets
 * @param num_dir_cycle cycle the signal direction this many times
 * @param signals_copy used for CmdBuildManySignals() to copy direction of first signal
 * @return the cost of this operation or an error
 * @todo p2 should be replaced by two bits for "along" and "against" the track.
 */
CommandCost CmdBuildSingleSignal(DoCommandFlag flags, TileIndex tile, Track track, SignalType sigtype, SignalVariant sigvar, uint8_t signal_style,
		uint8_t signal_spacing, BuildSignalFlags build_flags, SignalCycleGroups which_signals, uint8_t num_dir_cycle, uint8_t signals_copy)
{
	const bool ctrl_pressed = HasFlag(build_flags, BuildSignalFlags::CtrlPressed); // was the CTRL button pressed
	const bool convert_signal = HasFlag(build_flags, BuildSignalFlags::Convert); // convert button pressed

	if (signal_style > _num_new_signal_styles || !HasBit(_enabled_new_signal_styles_mask, signal_style)) return CMD_ERROR;

	if (_settings_game.vehicle.train_braking_model == TBM_REALISTIC && IsSignalTypeUnsuitableForRealisticBraking(sigtype)) return CMD_ERROR;

	/* You can only build signals on plain rail tiles or tunnel/bridges, and the selected track must exist */
	if (IsTileType(tile, MP_TUNNELBRIDGE)) {
		if (GetTunnelBridgeTransportType(tile) != TRANSPORT_RAIL) return CMD_ERROR;
		if (!ValParamTrackOrientation(track) || !IsTrackAcrossTunnelBridge(tile, track)) {
			return CommandCost(STR_ERROR_THERE_IS_NO_RAILROAD_TRACK);
		}
		CommandCost ret = TunnelBridgeIsFree(tile, GetOtherTunnelBridgeEnd(tile), nullptr, TBIFM_ACROSS_ONLY);
		if (ret.Failed()) return ret;
	} else if (!ValParamTrackOrientation(track) || !IsPlainRailTile(tile) || !HasTrack(tile, track)) {
		return CommandCost(STR_ERROR_THERE_IS_NO_RAILROAD_TRACK);
	}
	/* Protect against invalid signal copying */
	if (signals_copy != 0 && (signals_copy & SignalOnTrack(track)) == 0) return CMD_ERROR;

	CommandCost ret = CheckTileOwnership(tile);
	if (ret.Failed()) return ret;

	auto is_style_usable = [](SignalVariant sigvar, uint8_t style_id, uint8_t mask) {
		if (style_id == 0) return true;

		const NewSignalStyle &style = _new_signal_styles[style_id - 1];
		return ((sigvar == SIG_SEMAPHORE ? style.semaphore_mask : style.electric_mask) & mask) == mask;
	};

	CommandCost cost;
	/* handle signals simulation on tunnel/bridge. */
	if (IsTileType(tile, MP_TUNNELBRIDGE)) {
		if (signal_spacing == 0) return CommandCost(STR_ERROR_THERE_IS_NO_RAILROAD_TRACK);
		signal_spacing = Clamp<uint8_t>(signal_spacing, 1, 16);

		const TileIndex tile_exit = GetOtherTunnelBridgeEnd(tile);

		auto get_entrance_signal_count = [&]() -> uint {
			uint spacing;
			if (IsTunnelBridgeWithSignalSimulation(tile)) {
				spacing = GetTunnelBridgeSignalSimulationSpacing(tile);
			} else {
				spacing = GetBestTunnelBridgeSignalSimulationSpacing(tile, tile_exit, signal_spacing);
			}
			return 1 + (GetTunnelBridgeLength(tile, tile_exit) / spacing);
		};
		auto get_one_way_signal_count = [&]() -> uint {
			return get_entrance_signal_count() + 1;
		};

		if (TracksOverlap(GetTunnelBridgeTrackBits(tile)) || TracksOverlap(GetTunnelBridgeTrackBits(tile_exit))) return CommandCost(STR_ERROR_NO_SUITABLE_RAILROAD_TRACK);
		const bool bidirectional = HasFlag(build_flags, BuildSignalFlags::PermitBidiTunnelBridge) && (sigtype == SIGTYPE_PBS);
		cost = CommandCost();
		bool change_variant = false;              ///< Whether to change the semaphore/normal variant state
		bool change_variant_to_semaphore = false; ///< Semaphore/normal variant state to change to, true = semaphore
		bool change_style = false;
		const bool is_pbs = (sigtype == SIGTYPE_PBS) || (sigtype == SIGTYPE_PBS_ONEWAY);
		const Trackdir entrance_td = TrackExitdirToTrackdir(track, GetTunnelBridgeDirection(tile));
		const bool p2_signal_in = signals_copy & SignalAlongTrackdir(entrance_td);
		const bool p2_signal_out = signals_copy & SignalAgainstTrackdir(entrance_td);
		const bool p2_active = p2_signal_in || p2_signal_out;
		if (bidirectional && p2_active) return CMD_ERROR;
		if (p2_signal_in && p2_signal_out) return CommandCost();
		uint8_t other_end_signal_style = signal_style;
		bool change_both_ends = false;

		if (!IsTunnelBridgeWithSignalSimulation(tile)) {
			/* Previously unsignalled tunnel/bridge */
			change_both_ends = true;
			if (convert_signal) return CommandCost(STR_ERROR_THERE_ARE_NO_SIGNALS);
			cost = CommandCost(EXPENSES_CONSTRUCTION, _price[PR_BUILD_SIGNALS] * get_one_way_signal_count() * (bidirectional ? 2 : 1)); // minimal 1
			if (bidirectional) {
				if (HasBit(_signal_style_masks.no_tunnel_bridge_entrance | _signal_style_masks.no_tunnel_bridge_exit, signal_style)) {
					/* Bidirectional: both ends must be the same style */
					return CommandCost(STR_ERROR_UNSUITABLE_SIGNAL_TYPE);
				}
				if (!is_style_usable(sigvar, signal_style, 0x10)) return CommandCost(STR_ERROR_UNSUITABLE_SIGNAL_TYPE);
			} else {
				if (HasBit(_signal_style_masks.no_tunnel_bridge_entrance & _signal_style_masks.no_tunnel_bridge_exit, signal_style)) {
					/* Style is unusable for both ends */
					return CommandCost(STR_ERROR_UNSUITABLE_SIGNAL_TYPE);
				}
				if (!is_style_usable(sigvar, signal_style, is_pbs ? 0x20 : 0x1)) {
					/* Signal type unusable for this style */
					return CommandCost(STR_ERROR_UNSUITABLE_SIGNAL_TYPE);
				}
				if (HasBit(_signal_style_masks.no_tunnel_bridge_entrance, signal_style)) {
					signal_style = 0;
				}
				if (HasBit(_signal_style_masks.no_tunnel_bridge_exit, other_end_signal_style)) {
					other_end_signal_style = 0;
				}
				if (p2_signal_out) {
					/* Setting up tunnel/bridge in reverse direction */
					std::swap(signal_style, other_end_signal_style);
				}
			}
		} else {
			/* Previously signalled tunnel/bridge */

			if (HasFlag(build_flags, BuildSignalFlags::SkipExisting)) return CommandCost(); // don't modify existing signal flag

			const bool is_bidi = IsTunnelBridgeSignalSimulationBidirectional(tile);
			bool will_be_bidi = is_bidi;

			/* Common checks for both ends and bidi state */
			if (p2_active) {
				change_both_ends = true;
				will_be_bidi = false;
				if (HasBit(_signal_style_masks.no_tunnel_bridge_entrance | _signal_style_masks.no_tunnel_bridge_exit, signal_style)) return CommandCost(STR_ERROR_UNSUITABLE_SIGNAL_TYPE);
			} else {
				if (convert_signal) {
					will_be_bidi = bidirectional && !ctrl_pressed;
				} else if (ctrl_pressed) {
					will_be_bidi = false;
				} else {
					/* Swap direction */
					change_both_ends = true;
				}
			}
			if (is_bidi || will_be_bidi) change_both_ends = true;

			auto check_tile = [&](TileIndex t) -> CommandCost {
				const bool is_semaphore = IsTunnelBridgeSemaphore(t);
				bool will_be_semaphore = is_semaphore;
				bool will_be_pbs = IsTunnelBridgePBS(t);
				const uint8_t is_style = GetTunnelBridgeSignalStyle(t);
				uint8_t will_be_style = is_style;
				if (p2_active) {
					will_be_style = signal_style;
				} else if (convert_signal) {
					change_style = true;
					will_be_style = signal_style;
					will_be_pbs = is_pbs;
					will_be_semaphore = (sigvar == SIG_SEMAPHORE);
					if (HasBit(_signal_style_masks.no_tunnel_bridge_entrance, signal_style) && (will_be_bidi || IsTunnelBridgeSignalSimulationEntrance(t))) {
						return CommandCost(STR_ERROR_UNSUITABLE_SIGNAL_TYPE);
					}
					if (HasBit(_signal_style_masks.no_tunnel_bridge_exit, signal_style) && (will_be_bidi || IsTunnelBridgeSignalSimulationExit(t))) {
						return CommandCost(STR_ERROR_UNSUITABLE_SIGNAL_TYPE);
					}
				} else if (ctrl_pressed) {
					will_be_pbs = !will_be_pbs;
				} else if (!is_bidi) {
					/* Swap direction, check signal style compatibility */
					if (IsTunnelBridgeSignalSimulationEntrance(t)) {
						if (HasBit(_signal_style_masks.no_tunnel_bridge_exit, is_style)) return CommandCost(STR_ERROR_UNSUITABLE_SIGNAL_TYPE);
					}
					if (IsTunnelBridgeSignalSimulationExit(t)) {
						if (HasBit(_signal_style_masks.no_tunnel_bridge_entrance, is_style)) return CommandCost(STR_ERROR_UNSUITABLE_SIGNAL_TYPE);
					}
				}

				if (change_variant) {
					will_be_semaphore = change_variant_to_semaphore;
				} else if ((p2_active && (sigvar == SIG_SEMAPHORE) != is_semaphore) ||
						(convert_signal && (ctrl_pressed || (sigvar == SIG_SEMAPHORE) != is_semaphore))) {
					will_be_semaphore = !is_semaphore;
					change_variant = true;
					change_variant_to_semaphore = will_be_semaphore;
				}

				CommandCost subcost = CommandCost(EXPENSES_CONSTRUCTION, 0);
				if ((is_semaphore != will_be_semaphore) || (will_be_style != is_style)) {
					uint signal_count = 0;
					if (IsTunnelBridgeSignalSimulationEntrance(t)) signal_count += get_entrance_signal_count();
					if (IsTunnelBridgeSignalSimulationExit(t)) signal_count += 1;

					subcost.AddCost(_price[PR_CLEAR_SIGNALS] * signal_count);

					if (will_be_bidi) {
						signal_count = get_one_way_signal_count();
					}

					subcost.AddCost(_price[PR_BUILD_SIGNALS] * signal_count);
				} else if (is_bidi != will_be_bidi) {
					if (will_be_bidi != IsTunnelBridgeSignalSimulationEntrance(t)) {
						subcost.AddCost(_price[will_be_bidi ? PR_BUILD_SIGNALS : PR_CLEAR_SIGNALS] * get_one_way_signal_count());
					}
				}
				if (!is_style_usable(will_be_semaphore ? SIG_SEMAPHORE : SIG_ELECTRIC, will_be_style, will_be_bidi ? 0x10 : (will_be_pbs ? 0x20 : 0x1))) {
					return CommandCost(STR_ERROR_UNSUITABLE_SIGNAL_TYPE);
				}
				return subcost;
			};

			cost = CommandCost(EXPENSES_CONSTRUCTION, 0);

			CommandCost subcost = check_tile(tile);
			if (subcost.Failed()) return subcost;
			cost.AddCost(subcost);

			if (change_both_ends) {
				subcost = check_tile(tile_exit);
				if (subcost.Failed()) return subcost;
				cost.AddCost(subcost);
			}
		}

		if (_settings_game.vehicle.train_braking_model == TBM_REALISTIC) {
			for (TileIndex t : { tile, tile_exit }) {
				if (HasAcrossTunnelBridgeReservation(t)) {
					CommandCost ret = CheckTrainReservationPreventsTrackModification(t, FindFirstTrack(GetAcrossTunnelBridgeReservationTrackBits(t)));
					if (ret.Failed()) return ret;
				}
			}
		}

		if (flags & DC_EXEC) {
			auto remove_pbs_bidi = [&]() {
				if (IsTunnelBridgeSignalSimulationBidirectional(tile)) {
					ClrTunnelBridgeSignalSimulationExit(tile);
					ClrTunnelBridgeSignalSimulationEntrance(tile_exit);
				}
			};
			auto set_bidi = [&](TileIndex t) {
				SetTunnelBridgeSignalSimulationEntrance(t);
				SetTunnelBridgeEntranceSignalState(t, SIGNAL_STATE_GREEN);
				SetTunnelBridgeSignalSimulationExit(t);
				if (_extra_aspects > 0) {
					SetTunnelBridgeEntranceSignalAspect(t, 0);
					UpdateAspectDeferred(t, GetTunnelBridgeEntranceTrackdir(t));
				}
			};

			Company * const c = Company::Get(GetTileOwner(tile));
			std::vector<Train *> re_reserve_trains;
			for (TileIndex t : { tile, tile_exit }) {
				if (HasAcrossTunnelBridgeReservation(t)) {
					Train *re_reserve_train = GetTrainForReservation(t, FindFirstTrack(GetAcrossTunnelBridgeReservationTrackBits(t)));
					if (re_reserve_train != nullptr) {
						FreeTrainTrackReservation(re_reserve_train);
						re_reserve_trains.push_back(re_reserve_train);
					}
				}
			}
			if (IsTunnelBridgeWithSignalSimulation(tile)) {
				c->infrastructure.signal -= GetTunnelBridgeSignalSimulationSignalCount(tile, tile_exit);
			} else {
				uint spacing = GetBestTunnelBridgeSignalSimulationSpacing(tile, tile_exit, signal_spacing);
				SetTunnelBridgeSignalSimulationSpacing(tile, spacing);
				SetTunnelBridgeSignalSimulationSpacing(tile_exit, spacing);
			}
			if (!p2_active && IsTunnelBridgeWithSignalSimulation(tile)) { // Toggle signal if already signals present.
				if (convert_signal) {
					if (change_variant) {
						SetTunnelBridgeSemaphore(tile, change_variant_to_semaphore);
						if (change_both_ends) SetTunnelBridgeSemaphore(tile_exit, change_variant_to_semaphore);
					}
					if (!ctrl_pressed) {
						SetTunnelBridgePBS(tile, is_pbs);
						if (change_both_ends) SetTunnelBridgePBS(tile_exit, is_pbs);
						if (bidirectional) {
							set_bidi(tile);
							set_bidi(tile_exit);
						} else {
							remove_pbs_bidi();
						}
					}
					if (change_style) {
						SetTunnelBridgeSignalStyle(tile, signal_style);
					}
					if (change_style && change_both_ends) {
						SetTunnelBridgeSignalStyle(tile_exit, other_end_signal_style);
					}
				} else if (ctrl_pressed) {
					SetTunnelBridgePBS(tile, !IsTunnelBridgePBS(tile));
					if (change_both_ends) SetTunnelBridgePBS(tile_exit, IsTunnelBridgePBS(tile));
					if (!IsTunnelBridgePBS(tile)) remove_pbs_bidi();
				} else if (!IsTunnelBridgeSignalSimulationBidirectional(tile)) {
					if (IsTunnelBridgeSignalSimulationEntrance(tile)) {
						ClearBridgeTunnelSignalSimulation(tile, tile_exit);
						SetupBridgeTunnelSignalSimulation(tile_exit, tile);
					} else {
						ClearBridgeTunnelSignalSimulation(tile_exit, tile);
						SetupBridgeTunnelSignalSimulation(tile, tile_exit);
					}
				}
			} else {
				/* Create one direction tunnel/bridge if required. */
				if (!p2_active) {
					if (bidirectional) {
						set_bidi(tile);
						set_bidi(tile_exit);
					} else {
						SetupBridgeTunnelSignalSimulation(tile, tile_exit);
					}
				} else if (p2_signal_in != p2_signal_out) {
					/* If signal only on one side build accordingly one-way tunnel/bridge. */
					if (p2_signal_in) {
						ClearBridgeTunnelSignalSimulation(tile_exit, tile);
						SetupBridgeTunnelSignalSimulation(tile, tile_exit);
					} else {
						ClearBridgeTunnelSignalSimulation(tile, tile_exit);
						SetupBridgeTunnelSignalSimulation(tile_exit, tile);
					}
				}

				SetTunnelBridgeSemaphore(tile, sigvar == SIG_SEMAPHORE);
				SetTunnelBridgePBS(tile, is_pbs);
				SetTunnelBridgeSignalStyle(tile, signal_style);
				if (change_both_ends) {
					SetTunnelBridgeSemaphore(tile_exit, sigvar == SIG_SEMAPHORE);
					SetTunnelBridgePBS(tile_exit, is_pbs);
					SetTunnelBridgeSignalStyle(tile_exit, other_end_signal_style);
				}
				if (!IsTunnelBridgePBS(tile)) remove_pbs_bidi();
			}

			if (IsTunnelBridgeSignalSimulationExit(tile) && IsTunnelBridgeEffectivelyPBS(tile) && !HasAcrossTunnelBridgeReservation(tile)) {
				SetTunnelBridgeExitSignalState(tile, SIGNAL_STATE_RED);
			}
			if (IsTunnelBridgeSignalSimulationExit(tile_exit) && IsTunnelBridgeEffectivelyPBS(tile_exit) && !HasAcrossTunnelBridgeReservation(tile_exit)) {
				SetTunnelBridgeExitSignalState(tile_exit, SIGNAL_STATE_RED);
			}
			MarkBridgeOrTunnelDirty(tile, tile_exit);
			auto update_signal_side = [](TileIndex t) {
				AddSideToSignalBuffer(t, INVALID_DIAGDIR, GetTileOwner(t));
				if (IsTunnelBridgeSignalSimulationEntrance(t)) {
					SetTunnelBridgeEntranceSignalAspect(t, 0);
					UpdateAspectDeferred(t, GetTunnelBridgeEntranceTrackdir(t));
				}
				UpdateSignalsInBuffer();
			};
			update_signal_side(tile);
			update_signal_side(tile_exit);
			YapfNotifyTrackLayoutChange(tile, track);
			YapfNotifyTrackLayoutChange(tile_exit, track);
			if (IsTunnelBridgeWithSignalSimulation(tile)) {
				c->infrastructure.signal += GetTunnelBridgeSignalSimulationSignalCount(tile, tile_exit);
				if (IsTunnelBridgeRestrictedSignal(tile) || IsTunnelBridgeRestrictedSignal(tile_exit)) {
					InvalidateWindowClassesData(WC_TRACE_RESTRICT);
				}
			}
			DirtyCompanyInfrastructureWindows(GetTileOwner(tile));
			for (Train *re_reserve_train : re_reserve_trains) {
				ReReserveTrainPath(re_reserve_train);
			}
		}
		return cost;
	}

	/* See if this is a valid track combination for signals (no overlap) */
	if (TracksOverlap(GetTrackBits(tile))) return CommandCost(STR_ERROR_NO_SUITABLE_RAILROAD_TRACK);

	/* In case we don't want to change an existing signal, return without error. */
	if (HasFlag(build_flags, BuildSignalFlags::SkipExisting) && HasSignalOnTrack(tile, track)) return CommandCost();

	/* you can not convert a signal if no signal is on track */
	if (convert_signal && !HasSignalOnTrack(tile, track)) return CommandCost(STR_ERROR_THERE_ARE_NO_SIGNALS);

	if (!HasSignalOnTrack(tile, track)) {
		/* build new signals */
		cost = CommandCost(EXPENSES_CONSTRUCTION, _price[PR_BUILD_SIGNALS]);
		if (!is_style_usable(sigvar, signal_style, 1 << sigtype)) return CommandCost(STR_ERROR_UNSUITABLE_SIGNAL_TYPE);
	} else {
		if (signals_copy != 0 && (sigvar != GetSignalVariant(tile, track) || signal_style != GetSignalStyle(tile, track))) {
			/* convert signals <-> semaphores and/or change style */
			cost = CommandCost(EXPENSES_CONSTRUCTION, _price[PR_BUILD_SIGNALS] + _price[PR_CLEAR_SIGNALS]);
			if (!is_style_usable(sigvar, signal_style, 1 << sigtype)) return CommandCost(STR_ERROR_UNSUITABLE_SIGNAL_TYPE);

		} else if (convert_signal) {
			/* convert button pressed */
			if (ctrl_pressed || GetSignalVariant(tile, track) != sigvar || signal_style != GetSignalStyle(tile, track)) {
				/* it costs money to change signal variant (light or semaphore) */
				cost = CommandCost(EXPENSES_CONSTRUCTION, _price[PR_BUILD_SIGNALS] + _price[PR_CLEAR_SIGNALS]);
			} else {
				/* it is free to change signal type (block, exit, entry, combo, path, etc) */
				cost = CommandCost();
			}

			if (ctrl_pressed) {
				if (!is_style_usable((GetSignalVariant(tile, track) == SIG_ELECTRIC) ? SIG_SEMAPHORE : SIG_ELECTRIC, GetSignalStyle(tile, track), 1 << GetSignalType(tile, track))) return CommandCost(STR_ERROR_UNSUITABLE_SIGNAL_TYPE);
			} else {
				if (!is_style_usable(sigvar, signal_style, 1 << sigtype)) return CommandCost(STR_ERROR_UNSUITABLE_SIGNAL_TYPE);
			}

		} else {
			if (ctrl_pressed && GetSignalStyle(tile, track) != 0) {
				SignalType new_sigtype = NextSignalType(GetSignalType(tile, track), which_signals);
				if (_settings_game.vehicle.train_braking_model == TBM_REALISTIC && IsSignalTypeUnsuitableForRealisticBraking(new_sigtype)) return CMD_ERROR;
				if (!is_style_usable(GetSignalVariant(tile, track), GetSignalStyle(tile, track), 1 << new_sigtype)) return CommandCost(STR_ERROR_UNSUITABLE_SIGNAL_TYPE);
			}

			/* it is free to change orientation or number of signals on the tile (for block/presignals which allow signals in both directions) */
			cost = CommandCost();
		}
	}

	Train *v = nullptr;
	/* The new/changed signal could block our path. As this can lead to
	 * stale reservations, we clear the path reservation here and try
	 * to redo it later on. */
	if (HasReservedTracks(tile, TrackToTrackBits(track))) {
		v = GetTrainForReservation(tile, track);
		if (v != nullptr) {
			CommandCost ret = CheckTrainReservationPreventsTrackModification(v);
			if (ret.Failed()) return ret;
			if (flags & DC_EXEC) FreeTrainTrackReservation(v);
		}
	}

	if (flags & DC_EXEC) {
		if (!HasSignals(tile)) {
			/* there are no signals at all on this tile yet */
			SetHasSignals(tile, true);
			SetSignalStates(tile, 0xF); // all signals are on
			SetPresentSignals(tile, 0); // no signals built by default
			SetSignalType(tile, track, sigtype);
			SetSignalVariant(tile, track, sigvar);
			SetSignalStyle(tile, track, signal_style);
			UpdateSignalReserveThroughBit(tile, track, false);
			SetSignalSpecialPropagationFlag(tile, track, false);
		}

		/* Subtract old signal infrastructure count. */
		Company::Get(GetTileOwner(tile))->infrastructure.signal -= CountBits(GetPresentSignals(tile));

		if (signals_copy == 0) {
			if (!HasSignalOnTrack(tile, track)) {
				/* build new signals */
				SetPresentSignals(tile, GetPresentSignals(tile) | ((IsPbsSignal(sigtype) || _settings_game.vehicle.train_braking_model == TBM_REALISTIC) ? KillFirstBit(SignalOnTrack(track)) : SignalOnTrack(track)));
				SetSignalType(tile, track, sigtype);
				SetSignalVariant(tile, track, sigvar);
				SetSignalStyle(tile, track, signal_style);
				UpdateSignalReserveThroughBit(tile, track, false);
				SetSignalSpecialPropagationFlag(tile, track, false);
				while (num_dir_cycle-- > 0) CycleSignalSide(tile, track);
			} else {
				if (convert_signal) {
					/* convert signal button pressed */
					if (ctrl_pressed) {
						/* toggle the present signal variant: SIG_ELECTRIC <-> SIG_SEMAPHORE */
						SetSignalVariant(tile, track, (GetSignalVariant(tile, track) == SIG_ELECTRIC) ? SIG_SEMAPHORE : SIG_ELECTRIC);
						/* Query current signal type so the check for PBS signals below works. */
						sigtype = GetSignalType(tile, track);
					} else {
						if (GetSignalType(tile, track) == SIGTYPE_NO_ENTRY) CycleSignalSide(tile, track);

						/* convert the present signal to the chosen type and variant */
						if (IsPresignalProgrammable(tile, track)) {
							FreeSignalProgram(SignalReference(tile, track));
						}
						SetSignalType(tile, track, sigtype);
						SetSignalVariant(tile, track, sigvar);
						SetSignalStyle(tile, track, signal_style);
						if (IsPbsSignal(sigtype) && (GetPresentSignals(tile) & SignalOnTrack(track)) == SignalOnTrack(track)) {
							SetPresentSignals(tile, (GetPresentSignals(tile) & ~SignalOnTrack(track)) | KillFirstBit(SignalOnTrack(track)));
						}

						if (sigtype == SIGTYPE_NO_ENTRY) CycleSignalSide(tile, track);

						UpdateSignalReserveThroughBit(tile, track, false);
						UpdateRailSignalSpecialPropagationFlag(tile, track, GetExistingTraceRestrictProgram(tile, track), false);
					}

				} else if (ctrl_pressed) {
					/* cycle through signal types */
					sigtype = (SignalType)(GetSignalType(tile, track));
					if (IsProgrammableSignal(sigtype)) {
						FreeSignalProgram(SignalReference(tile, track));
					}

					if (sigtype == SIGTYPE_NO_ENTRY) CycleSignalSide(tile, track);

					sigtype = NextSignalType(sigtype, which_signals);
					if (_settings_game.vehicle.train_braking_model == TBM_REALISTIC && IsSignalTypeUnsuitableForRealisticBraking(sigtype)) return CMD_ERROR;

					SetSignalType(tile, track, sigtype);
					if (IsPbsSignal(sigtype) && (GetPresentSignals(tile) & SignalOnTrack(track)) == SignalOnTrack(track)) {
						SetPresentSignals(tile, (GetPresentSignals(tile) & ~SignalOnTrack(track)) | KillFirstBit(SignalOnTrack(track)));
					}

					if (sigtype == SIGTYPE_NO_ENTRY) CycleSignalSide(tile, track);
				} else {
					/* programmable pre-signal dependencies are invalidated when the signal direction is changed */
					CheckRemoveSignal(tile, track);
					/* cycle the signal side: both -> left -> right -> both -> ... */
					CycleSignalSide(tile, track);
					/* Query current signal type so the check for PBS signals below works. */
					sigtype = GetSignalType(tile, track);
				}
			}
		} else {
			/* If CmdBuildManySignals is called with copying signals, just copy the
			 * direction of the first signal given as parameter by CmdBuildManySignals */
			SetPresentSignals(tile, (GetPresentSignals(tile) & ~SignalOnTrack(track)) | (signals_copy & SignalOnTrack(track)));
			SetSignalVariant(tile, track, sigvar);
			if (IsPresignalProgrammable(tile, track)) {
				FreeSignalProgram(SignalReference(tile, track));
			}
			SetSignalType(tile, track, sigtype);
			SetSignalStyle(tile, track, signal_style);
			UpdateSignalReserveThroughBit(tile, track, false);
			UpdateRailSignalSpecialPropagationFlag(tile, track, GetExistingTraceRestrictProgram(tile, track), false);
		}

		/* Add new signal infrastructure count. */
		Company::Get(GetTileOwner(tile))->infrastructure.signal += CountBits(GetPresentSignals(tile));
		DirtyCompanyInfrastructureWindows(GetTileOwner(tile));

		if (IsPbsSignalNonExtended(sigtype) || (_settings_game.vehicle.train_braking_model == TBM_REALISTIC && HasBit(GetRailReservationTrackBits(tile), track))) {
			/* PBS signals should show red unless they are on reserved tiles without a train. */
			uint mask = GetPresentSignals(tile) & SignalOnTrack(track);
			SetSignalStates(tile, (GetSignalStates(tile) & ~mask) | ((HasBit(GetRailReservationTrackBits(tile), track) && EnsureNoVehicleOnGround(tile).Succeeded() ? UINT_MAX : 0) & mask));
		}
		MarkTileDirtyByTile(tile, VMDF_NOT_MAP_MODE);
		AddTrackToSignalBuffer(tile, track, _current_company);
		YapfNotifyTrackLayoutChange(tile, track);
		if (v != nullptr && v->track != TRACK_BIT_DEPOT) {
			ReReserveTrainPath(v);
		}
	}

	return cost;
}

static bool CheckSignalAutoFill(TileIndex &tile, Trackdir &trackdir, int &signal_ctr, bool remove, bool allow_station)
{
	tile = AddTileIndexDiffCWrap(tile, _trackdelta[trackdir]);
	if (tile == INVALID_TILE) return false;

	/* Check for track bits on the new tile */
	TrackdirBits trackdirbits = GetTileTrackdirBits(tile, TRANSPORT_RAIL, 0);

	if (TracksOverlap(TrackdirBitsToTrackBits(trackdirbits))) return false;
	trackdirbits &= TrackdirReachesTrackdirs(trackdir);

	/* No track bits, must stop */
	if (trackdirbits == TRACKDIR_BIT_NONE) return false;

	/* Get the first track dir */
	trackdir = RemoveFirstTrackdir(&trackdirbits);

	/* Any left? It's a junction so we stop */
	if (trackdirbits != TRACKDIR_BIT_NONE) return false;

	switch (GetTileType(tile)) {
		case MP_RAILWAY:
			if (IsRailDepot(tile)) return false;
			if (!remove && HasSignalOnTrack(tile, TrackdirToTrack(trackdir))) return false;
			signal_ctr++;
			if (IsDiagonalTrackdir(trackdir)) {
				signal_ctr++;
				/* Ensure signal_ctr even so X and Y pieces get signals */
				ClrBit(signal_ctr, 0);
			}
			return true;

		case MP_ROAD:
			if (!IsLevelCrossing(tile)) return false;
			signal_ctr += 2;
			return true;

		case MP_TUNNELBRIDGE: {
			if (!remove && IsTunnelBridgeWithSignalSimulation(tile)) return false;
			TileIndex orig_tile = tile; // backup old value

			if (GetTunnelBridgeTransportType(tile) != TRANSPORT_RAIL) return false;
			signal_ctr += IsDiagonalTrackdir(trackdir) ? 2 : 1;
			if (GetTunnelBridgeDirection(tile) == TrackdirToExitdir(trackdir)) {
				/* Skip to end of tunnel or bridge
				 * note that tile is a parameter by reference, so it must be updated */
				tile = GetOtherTunnelBridgeEnd(tile);
				signal_ctr += GetTunnelBridgeLength(orig_tile, tile) * 2;

				/* Check for track bits on the new tile */
				trackdirbits = GetTileTrackdirBits(tile, TRANSPORT_RAIL, 0);

				if (TracksOverlap(TrackdirBitsToTrackBits(trackdirbits))) return false;
				trackdirbits &= TrackdirReachesTrackdirs(trackdir);

				/* Get the first track dir */
				trackdir = RemoveFirstTrackdir(&trackdirbits);

				/* Any left? It's a junction so we stop */
				if (trackdirbits != TRACKDIR_BIT_NONE) return false;

				signal_ctr += IsDiagonalTrackdir(trackdir) ? 2 : 1;
			}
			return true;
		}

		case MP_STATION: {
			if (!allow_station) return false;
			signal_ctr += 2;
			return true;
		}

		default: return false;
	}
}

struct SignalTrackHelperParamsBase {};

struct SignalTrackHelperBuildParams : public SignalTrackHelperParamsBase {
	SignalType sigtype;      ///< default signal type
	SignalVariant sigvar;    ///< signal variant to build
	uint8_t signal_style;    ///< default signal style
	bool mode;               ///< true = override signal/semaphore, or pre/exit/combo signal (CTRL-toggle)
};

struct SignalTrackHelperRemoveParams : public SignalTrackHelperParamsBase {
	RemoveSignalFlags remove_flags;
};

/**
 * Build many signals by dragging; AutoSignals
 * @param flags operation to perform
 * @param tile start tile of drag
 * @param end_tile end tile of drag
 * @param track track-orientation
 * @param remove remove signals?
 * @param drag_flags signal drag flags
 * @param signal_density user defined signals_density
 * @param subcommand_params SignalTrackHelperBuildParams for build, SignalTrackHelperRemoveParams for remove
 * @return the cost of this operation or an error
 */
static CommandCost CmdSignalTrackHelper(DoCommandFlag flags, TileIndex tile, TileIndex end_tile, Track track, bool remove, SignalDragFlags drag_flags, uint8_t signal_density, SignalTrackHelperParamsBase &subcommand_params)
{
	CommandCost total_cost(EXPENSES_CONSTRUCTION);

	if (end_tile >= Map::Size() || !ValParamTrackOrientation(track)) return CMD_ERROR;
	if (signal_density == 0 || signal_density > MAX_SIGNAL_DRAG_DISTANCE) return CMD_ERROR;

	if (!remove) {
		auto &params = static_cast<SignalTrackHelperBuildParams &>(subcommand_params);
		if (params.sigtype > SIGTYPE_LAST || params.sigvar > SIG_SEMAPHORE) return CMD_ERROR;
	}

	TileIndex start_tile = tile;

	if (!IsPlainRailTile(tile)) return CommandCost(STR_ERROR_THERE_IS_NO_RAILROAD_TRACK);

	/* for vertical/horizontal tracks, double the given signals density
	 * since the original amount will be too dense (shorter tracks) */
	signal_density *= 2;

	Trackdir trackdir = TrackToTrackdir(track);
	CommandCost ret = ValidateAutoDrag(&trackdir, tile, end_tile);
	if (ret.Failed()) return ret;

	track = TrackdirToTrack(trackdir); // trackdir might have changed, keep track in sync
	Trackdir start_trackdir = trackdir;

	/* Must start on a valid track to be able to avoid loops */
	if (!HasTrack(tile, track)) return CMD_ERROR;

	uint8_t signals = 0;
	/* copy the signal-style of the first rail-piece if existing */
	if (!remove) {
		auto &params = static_cast<SignalTrackHelperBuildParams &>(subcommand_params);

		if (HasSignalOnTrack(tile, track)) {
			signals = GetPresentSignals(tile) & SignalOnTrack(track);
			assert(signals != 0);

			/* copy signal/semaphores style (independent of CTRL) */
			params.sigvar = GetSignalVariant(tile, track);

			params.sigtype = GetSignalType(tile, track);
			/* Don't but copy entry or exit-signal type */
			if (params.sigtype == SIGTYPE_ENTRY || params.sigtype == SIGTYPE_EXIT) params.sigtype = SIGTYPE_BLOCK;

			params.signal_style = GetSignalStyle(tile, track);
		} else { // no signals exist, drag a two-way signal stretch
			signals = IsPbsSignal(params.sigtype) ? SignalAlongTrackdir(trackdir) : SignalOnTrack(track);
		}
	}

	uint8_t signal_dir = 0;
	if (signals & SignalAlongTrackdir(trackdir))   SetBit(signal_dir, 0);
	if (signals & SignalAgainstTrackdir(trackdir)) SetBit(signal_dir, 1);

	/* signal_ctr         - amount of tiles already processed
	 * last_used_ctr      - amount of tiles before previously placed signal
	 * signals_density    - setting to put signal on every Nth tile (double space on |, -- tracks)
	 * last_suitable_ctr  - amount of tiles before last possible signal place
	 * last_suitable_tile - last tile where it is possible to place a signal
	 * last_suitable_trackdir - trackdir of the last tile
	 **********
	 * trackdir   - trackdir to build with autorail
	 * semaphores - semaphores or signals
	 * signals    - is there a signal/semaphore on the first tile, copy its style (two-way/single-way)
	 *              and convert all others to semaphore/signal
	 * remove     - 1 remove signals, 0 build signals */
	int signal_ctr = 0;
	int last_used_ctr = INT_MIN; // initially INT_MIN to force building/removing at the first tile
	int last_suitable_ctr = 0;
	TileIndex last_suitable_tile = INVALID_TILE;
	Trackdir last_suitable_trackdir = INVALID_TRACKDIR;
	CommandCost last_error = CMD_ERROR;
	bool had_success = false;
	std::vector<TileIndex> tunnel_bridge_blacklist;

	auto build_signal = [&](TileIndex tile, Trackdir trackdir, bool test_only, bool skip_existing_signals) -> CommandCost {
		/* Pick the correct orientation for the track direction */
		uint8_t signals = 0;
		if (HasBit(signal_dir, 0)) signals |= SignalAlongTrackdir(trackdir);
		if (HasBit(signal_dir, 1)) signals |= SignalAgainstTrackdir(trackdir);

		DoCommandFlag do_flags = test_only ? flags & ~DC_EXEC : flags;
		CommandCost ret;
		if (remove) {
			auto &params = static_cast<SignalTrackHelperRemoveParams &>(subcommand_params);
			ret = Command<CMD_REMOVE_SINGLE_SIGNAL>::Do(do_flags, tile, TrackdirToTrack(trackdir), params.remove_flags);
		} else {
			auto &params = static_cast<SignalTrackHelperBuildParams &>(subcommand_params);
			BuildSignalFlags build_flags{};
			if (skip_existing_signals) build_flags |= BuildSignalFlags::SkipExisting;
			if (params.mode) build_flags |= BuildSignalFlags::CtrlPressed;
			ret = Command<CMD_BUILD_SINGLE_SIGNAL>::Do(do_flags, tile, TrackdirToTrack(trackdir), params.sigtype, params.sigvar, params.signal_style,
					Clamp<uint8_t>(signal_density / 2, 1, 16), build_flags, SCG_CURRENT_GROUP, 0, signals);
		}
		if (!test_only && ret.Succeeded() && IsTileType(tile, MP_TUNNELBRIDGE) && GetTunnelBridgeDirection(tile) == TrackdirToExitdir(trackdir)) {
			/* Blacklist far end of tunnel if we just actioned the near end */
			tunnel_bridge_blacklist.push_back(GetOtherTunnelBridgeEnd(tile));
		}
		return ret;
	};

	for (;;) {
		bool tile_ok = true;
		if (IsTileType(tile, MP_TUNNELBRIDGE)) {
			if (container_unordered_remove(tunnel_bridge_blacklist, tile) > 0) {
				/* This tile is blacklisted, skip tile and remove from blacklist.
				 * Mark last used counter as current tile.
				 */
				tile_ok = false;
				last_used_ctr = signal_ctr;
				last_suitable_tile = INVALID_TILE;
			}
		}

		/* only build/remove signals with the specified density */
		if (tile_ok && (remove || HasFlag(drag_flags, SignalDragFlags::MinimiseGaps) || signal_ctr % signal_density == 0 || IsTileType(tile, MP_TUNNELBRIDGE))) {
			/* Test tiles in between for suitability as well if minimising gaps. */
			bool test_only = !remove && HasFlag(drag_flags, SignalDragFlags::MinimiseGaps) && signal_ctr < (last_used_ctr + signal_density);
			CommandCost ret = build_signal(tile, trackdir, test_only, signal_ctr == 0);

			if (ret.Succeeded()) {
				/* Remember last track piece where we can place a signal. */
				last_suitable_ctr = signal_ctr;
				last_suitable_tile = tile;
				last_suitable_trackdir = trackdir;
			} else if (ret.GetErrorMessage() == STR_ERROR_RESTRICTED_SIGNAL) {
				last_error = ret;
				break;
			} else if (!test_only && last_suitable_tile != INVALID_TILE && ret.GetErrorMessage() != STR_ERROR_CANNOT_MODIFY_TRACK_TRAIN_APPROACHING) {
				/* If a signal can't be placed, place it at the last possible position. */
				ret = build_signal(last_suitable_tile, last_suitable_trackdir, false, false);
			}

			/* Collect cost. */
			if (!test_only) {
				/* Be user-friendly and try placing signals as much as possible */
				if (ret.Succeeded()) {
					had_success = true;
					total_cost.AddCost(ret);
					last_used_ctr = last_suitable_ctr;
					last_suitable_tile = INVALID_TILE;
				} else {
					/* The "No railway" error is the least important one. */
					if (ret.GetErrorMessage() != STR_ERROR_THERE_IS_NO_RAILROAD_TRACK ||
							last_error.GetErrorMessage() == INVALID_STRING_ID) {
						last_error = ret;
					}
				}
			}
		}

		if (HasFlag(drag_flags, SignalDragFlags::Autofill)) {
			if (!CheckSignalAutoFill(tile, trackdir, signal_ctr, remove, HasFlag(drag_flags, SignalDragFlags::SkipOverStations))) break;

			/* Prevent possible loops */
			if (tile == start_tile && trackdir == start_trackdir) break;
		} else {
			if (tile == end_tile) break;

			tile += ToTileIndexDiff(_trackdelta[trackdir]);
			signal_ctr++;

			/* toggle railbit for the non-diagonal tracks (|, -- tracks) */
			if (IsDiagonalTrackdir(trackdir)) {
				signal_ctr++;
			} else {
				ToggleBit(trackdir, 0);
			}
		}
	}

	return had_success ? total_cost : last_error;
}

/**
 * Build signals on a stretch of track.
 * Stub for the unified signal builder/remover
 * @param flags operation to perform
 * @param tile start tile of drag
 * @param end_tile end tile of drag
 * @param track track-orientation
 * @param sigtype default signal type
 * @param sigvar signal variant to build
 * @param signal_style default signal style
 * @param mode true = override signal/semaphore, or pre/exit/combo signal (CTRL-toggle)
 * @param drag_flags signal drag flags
 * @param signal_density user defined signals_density
 * @return the cost of this operation or an error
 * @see CmdSignalTrackHelper
 */
CommandCost CmdBuildSignalTrack(DoCommandFlag flags, TileIndex tile, TileIndex end_tile, Track track, SignalType sigtype, SignalVariant sigvar, uint8_t signal_style, bool mode, SignalDragFlags drag_flags, uint8_t signal_density)
{
	SignalTrackHelperBuildParams params{ {}, sigtype, sigvar, signal_style, mode };
	return CmdSignalTrackHelper(flags, tile, end_tile, track, false, drag_flags, signal_density, params);
}

/**
 * Remove signals
 * @param flags operation to perform
 * @param tile coordinates where signal is being deleted from
 * @param track track-orientation
 * @param remove_flags Remove flags
 * @return the cost of this operation or an error
 */
CommandCost CmdRemoveSingleSignal(DoCommandFlag flags, TileIndex tile, Track track, RemoveSignalFlags remove_flags)
{
	Money cost = _price[PR_CLEAR_SIGNALS];

	if (IsTileType(tile, MP_TUNNELBRIDGE)) {
		if (GetTunnelBridgeTransportType(tile) != TRANSPORT_RAIL) return CommandCost(STR_ERROR_THERE_IS_NO_RAILROAD_TRACK);
		if (!ValParamTrackOrientation(track) || !IsTrackAcrossTunnelBridge(tile, track)) {
			return CommandCost(STR_ERROR_THERE_IS_NO_RAILROAD_TRACK);
		}
		if (!IsTunnelBridgeWithSignalSimulation(tile)) return CommandCost(STR_ERROR_THERE_ARE_NO_SIGNALS);
		TileIndex end = GetOtherTunnelBridgeEnd(tile);
		if (HasFlag(remove_flags, RemoveSignalFlags::NoRemoveRestricted) && (IsTunnelBridgeRestrictedSignal(tile) || IsTunnelBridgeRestrictedSignal(end))) return CommandCost(STR_ERROR_RESTRICTED_SIGNAL);
		CommandCost ret = TunnelBridgeIsFree(tile, end, nullptr, TBIFM_ACROSS_ONLY);
		if (ret.Failed()) return ret;

		cost *= GetTunnelBridgeSignalSimulationSignalCount(tile, end);
	} else {
		if (!ValParamTrackOrientation(track) || !IsPlainRailTile(tile) || !HasTrack(tile, track)) {
			return CommandCost(STR_ERROR_THERE_IS_NO_RAILROAD_TRACK);
		}
		if (!HasSignalOnTrack(tile, track)) {
			return CommandCost(STR_ERROR_THERE_ARE_NO_SIGNALS);
		}
		if (HasFlag(remove_flags, RemoveSignalFlags::NoRemoveRestricted) && GetExistingTraceRestrictProgram(tile, track) != nullptr) return CommandCost(STR_ERROR_RESTRICTED_SIGNAL);
	}

	/* Only water can remove signals from anyone */
	if (_current_company != OWNER_WATER) {
		CommandCost ret = CheckTileOwnership(tile);
		if (ret.Failed()) return ret;
	}

	if (IsTunnelBridgeWithSignalSimulation(tile)) { // handle tunnel/bridge signals.
		TileIndex end = GetOtherTunnelBridgeEnd(tile);
		std::vector<Train *> re_reserve_trains;
		for (TileIndex t : { tile, end }) {
			if (HasAcrossTunnelBridgeReservation(t)) {
				Train *v = GetTrainForReservation(t, FindFirstTrack(GetAcrossTunnelBridgeReservationTrackBits(t)));
				if (v != nullptr) {
					CommandCost ret = CheckTrainReservationPreventsTrackModification(v);
					if (ret.Failed()) return ret;
					if (flags & DC_EXEC) {
						FreeTrainTrackReservation(v);
						re_reserve_trains.push_back(v);
					}
				}
			}
		}
		if (flags & DC_EXEC) {
			Track end_track = FindFirstTrack(GetAcrossTunnelBridgeTrackBits(end));
			Company *c = Company::Get(GetTileOwner(tile));
			c->infrastructure.signal -= GetTunnelBridgeSignalSimulationSignalCount(tile, end);
			TraceRestrictNotifySignalRemoval(tile, track);
			TraceRestrictNotifySignalRemoval(end, end_track);
			ClearBridgeTunnelSignalSimulation(end, tile);
			ClearBridgeTunnelSignalSimulation(tile, end);
			SetTunnelBridgeSignalStyle(tile, 0);
			SetTunnelBridgeSignalStyle(end, 0);
			MarkBridgeOrTunnelDirty(tile, end);
			AddSideToSignalBuffer(tile, INVALID_DIAGDIR, GetTileOwner(tile));
			AddSideToSignalBuffer(end, INVALID_DIAGDIR, GetTileOwner(tile));
			YapfNotifyTrackLayoutChange(tile, track);
			YapfNotifyTrackLayoutChange(end, end_track);
			DirtyCompanyInfrastructureWindows(GetTileOwner(tile));
			for (Train *v : re_reserve_trains) {
				ReReserveTrainPath(v);
			}
		}
		return CommandCost(EXPENSES_CONSTRUCTION, cost);
	}

	Train *v = nullptr;
	if (HasReservedTracks(tile, TrackToTrackBits(track))) {
		v = GetTrainForReservation(tile, track);
	}
	if (v != nullptr) {
		CommandCost ret = CheckTrainReservationPreventsTrackModification(v);
		if (ret.Failed()) return ret;
	}

	/* Do it? */
	if (flags & DC_EXEC) {
		Company::Get(GetTileOwner(tile))->infrastructure.signal -= CountBits(GetPresentSignals(tile));
		CheckRemoveSignal(tile, track);
		SetPresentSignals(tile, GetPresentSignals(tile) & ~SignalOnTrack(track));
		Company::Get(GetTileOwner(tile))->infrastructure.signal += CountBits(GetPresentSignals(tile));
		DirtyCompanyInfrastructureWindows(GetTileOwner(tile));
		TraceRestrictNotifySignalRemoval(tile, track);

		/* removed last signal from tile? */
		if (GetPresentSignals(tile) == 0) {
			SetSignalStates(tile, 0);
			SetHasSignals(tile, false);
			SetSignalVariant(tile, INVALID_TRACK, SIG_ELECTRIC); // remove any possible semaphores
		}

		AddTrackToSignalBuffer(tile, track, GetTileOwner(tile));
		YapfNotifyTrackLayoutChange(tile, track);
		if (v != nullptr && !(v->track & TRACK_BIT_WORMHOLE && IsTunnelBridgeWithSignalSimulation(v->tile))) {
			TryPathReserve(v, false);
		}

		MarkTileDirtyByTile(tile, VMDF_NOT_MAP_MODE);
	}

	return CommandCost(EXPENSES_CONSTRUCTION, cost);
}

/**
 * Remove signals on a stretch of track.
 * Stub for the unified signal builder/remover
 * @param flags operation to perform
 * @param tile start tile of drag
 * @param end_tile end tile of drag
 * @param track track-orientation
 * @param drag_flags signal drag flags
 * @param remove_flags remove signal flags
 * @return the cost of this operation or an error
 * @see CmdSignalTrackHelper
 */
CommandCost CmdRemoveSignalTrack(DoCommandFlag flags, TileIndex tile, TileIndex end_tile, Track track, SignalDragFlags drag_flags, RemoveSignalFlags remove_flags)
{
	SignalTrackHelperRemoveParams params{ {}, remove_flags };
	return CmdSignalTrackHelper(flags, tile, end_tile, track, true, drag_flags, 1, params);
}

/** Update power of train under which is the railtype being converted */
static Vehicle *UpdateTrainPowerProc(Vehicle *v, void *data)
{
	TrainList *affected_trains = static_cast<TrainList*>(data);
	include(*affected_trains, Train::From(v)->First());

	return nullptr;
}

struct UpdateTrainPowerProcData {
	TrainList *train_list;
	TrackBits track_bits;
};

/** Update power of train under which is the railtype being converted */
static Vehicle *UpdateTrainPowerProcAcrossTunnelBridge(Vehicle *v, void *data)
{
	UpdateTrainPowerProcData *utpp_data = static_cast<UpdateTrainPowerProcData*>(data);

	TrackBits vehicle_track = Train::From(v)->track;
	if (!(vehicle_track & TRACK_BIT_WORMHOLE) && !(utpp_data->track_bits & vehicle_track)) return nullptr;

	include(*(utpp_data->train_list), Train::From(v)->First());

	return nullptr;
}

/** Update power of train under which is the railtype being converted */
static Vehicle *UpdateTrainPowerProcOnTrackBits(Vehicle *v, void *data)
{
	UpdateTrainPowerProcData *utpp_data = static_cast<UpdateTrainPowerProcData*>(data);

	if (!(utpp_data->track_bits & Train::From(v)->track)) return nullptr;

	include(*(utpp_data->train_list), Train::From(v)->First());

	return nullptr;
}

struct EnsureNoIncompatibleRailtypeTrainOnGroundData {
	int z;
	RailType type;
};

static Vehicle *EnsureNoIncompatibleRailtypeTrainProc(Vehicle *v, void *data)
{
	const EnsureNoIncompatibleRailtypeTrainOnGroundData *procdata = (EnsureNoIncompatibleRailtypeTrainOnGroundData *)data;

	if (v->z_pos > procdata->z) return nullptr;
	if (HasBit(Train::From(v)->First()->compatible_railtypes, procdata->type)) return nullptr;

	return v;
}

CommandCost EnsureNoIncompatibleRailtypeTrainOnGround(TileIndex tile, RailType type)
{
	EnsureNoIncompatibleRailtypeTrainOnGroundData data = {
		GetTileMaxPixelZ(tile),
		type
	};

	if (HasVehicleOnPos(tile, VEH_TRAIN, &data, &EnsureNoIncompatibleRailtypeTrainProc)) {
		return CommandCost(STR_ERROR_TRAIN_IN_THE_WAY);
	}
	return CommandCost();
}

struct EnsureNoIncompatibleRailtypeTrainOnTrackBitsData {
	TrackBits track_bits;
	RailType type;
};

static Vehicle *EnsureNoIncompatibleRailtypeTrainOnTrackProc(Vehicle *v, void *data)
{
	const EnsureNoIncompatibleRailtypeTrainOnTrackBitsData *procdata = (EnsureNoIncompatibleRailtypeTrainOnTrackBitsData *)data;
	TrackBits rail_bits = procdata->track_bits;

	Train *t = Train::From(v);
	if (HasBit(t->First()->compatible_railtypes, procdata->type)) return nullptr;
	if (rail_bits & TRACK_BIT_WORMHOLE) {
		if (t->track & TRACK_BIT_WORMHOLE) return v;
		rail_bits &= ~TRACK_BIT_WORMHOLE;
	} else if (t->track & TRACK_BIT_WORMHOLE) {
		return nullptr;
	}
	if ((t->track != rail_bits) && !TracksOverlap(t->track | rail_bits)) return nullptr;

	return v;
}

CommandCost EnsureNoIncompatibleRailtypeTrainOnTrackBits(TileIndex tile, TrackBits track_bits, RailType type)
{
	EnsureNoIncompatibleRailtypeTrainOnTrackBitsData data = {
		track_bits,
		type
	};

	if (HasVehicleOnPos(tile, VEH_TRAIN, &data, &EnsureNoIncompatibleRailtypeTrainOnTrackProc)) {
		return CommandCost(STR_ERROR_TRAIN_IN_THE_WAY);
	}
	return CommandCost();
}


/**
 * Convert one rail type to the other. You can convert normal rail to
 * monorail/maglev easily or vice-versa.
 * @param flags operation to perform
 * @param tile end tile of rail conversion drag
 * @param area_start start tile of drag
 * @param totype new railtype to convert to.
 * @param diagonal build diagonally or not.
 * @return the cost of this operation or an error
 */
CommandCost CmdConvertRail(DoCommandFlag flags, TileIndex tile, TileIndex area_start, RailType totype, bool diagonal)
{
	TileIndex area_end = tile;

	if (!ValParamRailType(totype)) return CMD_ERROR;
	if (area_start >= Map::Size()) return CMD_ERROR;

	TrainList affected_trains;

	CommandCost cost(EXPENSES_CONSTRUCTION);
	CommandCost error = CommandCost(STR_ERROR_NO_SUITABLE_RAILROAD_TRACK); // by default, there is no track to convert.
	bool found_convertible_track = false; // whether we actually did convert some track (see bug #7633)

	OrthogonalOrDiagonalTileIterator iter(area_start, area_end, diagonal);
	for (; (tile = *iter) != INVALID_TILE; ++iter) {
		TileType tt = GetTileType(tile);

		/* Check if there is any track on tile */
		switch (tt) {
			case MP_RAILWAY:
				break;
			case MP_STATION:
				if (!HasStationRail(tile)) continue;
				break;
			case MP_ROAD:
				if (!IsLevelCrossing(tile)) continue;
				if (RailNoLevelCrossings(totype)) {
					error.MakeError(STR_ERROR_CROSSING_DISALLOWED_RAIL);
					continue;
				}
				break;
			case MP_TUNNELBRIDGE:
				if (GetTunnelBridgeTransportType(tile) != TRANSPORT_RAIL) continue;
				break;
			default: continue;
		}

		/* Original railtype we are converting from */
		const RailType type = GetRailType(tile);
		const RailType raw_secondary_type = GetTileSecondaryRailTypeIfValid(tile);
		const RailType secondary_type = (raw_secondary_type == INVALID_RAILTYPE) ? type : raw_secondary_type;

		/* Converting to the same type or converting 'hidden' elrail -> rail */
		if ((type == totype || (_settings_game.vehicle.disable_elrails && totype == RAILTYPE_RAIL && type == RAILTYPE_ELECTRIC))
				&& (secondary_type == totype || (_settings_game.vehicle.disable_elrails && totype == RAILTYPE_RAIL && secondary_type == RAILTYPE_ELECTRIC))) continue;

		/* Trying to convert other's rail */
		CommandCost ret = CheckTileOwnership(tile);
		if (ret.Failed()) {
			error = ret;
			continue;
		}

		std::vector<Train *> vehicles_affected;

		auto find_train_reservations = [&vehicles_affected, &totype, &flags](TileIndex tile, TrackBits reserved) -> CommandCost {
			if (!(flags & DC_EXEC) && _settings_game.vehicle.train_braking_model != TBM_REALISTIC) {
				/* Nothing to do */
				return CommandCost();
			}
			Track track;
			while ((track = RemoveFirstTrack(&reserved)) != INVALID_TRACK) {
				Train *v = GetTrainForReservation(tile, track);
				bool check_train = false;
				if (v != nullptr && !HasPowerOnRail(v->railtype, totype)) {
					check_train = true;
				} else if (v != nullptr && _settings_game.vehicle.train_braking_model == TBM_REALISTIC) {
					RailType original = GetRailTypeByTrack(tile, track);
					if ((uint)(GetRailTypeInfo(original)->max_speed - 1) > (uint)(GetRailTypeInfo(totype)->max_speed - 1)) {
						check_train = true;
					}
				}
				if (check_train) {
					CommandCost ret = CheckTrainReservationPreventsTrackModification(v);
					if (ret.Failed()) return ret;

					/* No power on new rail type, reroute. */
					if (flags & DC_EXEC) {
						FreeTrainTrackReservation(v);
						vehicles_affected.push_back(v);
					}
				}
			}
			return CommandCost();
		};

		auto yapf_notify_track_change = [](TileIndex tile, TrackBits tracks) {
			while (tracks != TRACK_BIT_NONE) {
				YapfNotifyTrackLayoutChange(tile, RemoveFirstTrack(&tracks));
			}
		};

		/* Vehicle on the tile when not converting Rail <-> ElRail
		 * Tunnels and bridges have special check later */
		if (tt != MP_TUNNELBRIDGE) {
			if (!IsCompatibleRail(type, totype) || !IsCompatibleRail(secondary_type, totype)) {
				CommandCost ret = IsPlainRailTile(tile) ? EnsureNoIncompatibleRailtypeTrainOnTrackBits(tile, GetTrackBits(tile), totype) : EnsureNoIncompatibleRailtypeTrainOnGround(tile, totype);
				if (ret.Failed()) {
					error = ret;
					continue;
				}
			}
			CommandCost ret = find_train_reservations(tile, GetReservedTrackbits(tile));
			if (ret.Failed()) return ret;
			if (flags & DC_EXEC) { // we can safely convert, too
				/* Update the company infrastructure counters. */
				if (!IsRailStationTile(tile) || !IsStationTileBlocked(tile)) {
					Company *c = Company::Get(GetTileOwner(tile));
					uint num_pieces = IsLevelCrossingTile(tile) ? LEVELCROSSING_TRACKBIT_FACTOR : 1;
					if (IsPlainRailTile(tile)) {
						TrackBits bits = GetTrackBits(tile);
						if (bits == TRACK_BIT_HORZ || bits == TRACK_BIT_VERT) {
							c->infrastructure.rail[secondary_type]--;
							c->infrastructure.rail[totype]++;
						} else {
							num_pieces = CountBits(bits);
							if (TracksOverlap(bits)) num_pieces *= num_pieces;
						}
					}
					c->infrastructure.rail[type] -= num_pieces;
					c->infrastructure.rail[totype] += num_pieces;
					DirtyCompanyInfrastructureWindows(c->index);
				}

				SetRailType(tile, totype);
				if (IsPlainRailTile(tile)) SetSecondaryRailType(tile, totype);

				MarkTileDirtyByTile(tile);
				/* update power of train on this tile */
				FindVehicleOnPos(tile, VEH_TRAIN, &affected_trains, &UpdateTrainPowerProc);
			}
		}

		switch (tt) {
			case MP_RAILWAY:
				switch (GetRailTileType(tile)) {
					case RAIL_TILE_DEPOT:
						if (flags & DC_EXEC) {
							/* notify YAPF about the track layout change */
							YapfNotifyTrackLayoutChange(tile, GetRailDepotTrack(tile));

							/* Update build vehicle window related to this depot */
							InvalidateWindowData(WC_VEHICLE_DEPOT, tile.base());
							InvalidateWindowData(WC_BUILD_VEHICLE, tile.base());
						}
						found_convertible_track = true;
						cost.AddCost(RailConvertCost(type, totype));
						break;

					default: // RAIL_TILE_NORMAL, RAIL_TILE_SIGNALS
						if (flags & DC_EXEC) {
							/* notify YAPF about the track layout change */
							yapf_notify_track_change(tile, GetTrackBits(tile));
						}
						found_convertible_track = true;
						if (raw_secondary_type != INVALID_RAILTYPE) {
							cost.AddCost(RailConvertCost(type, totype));
							cost.AddCost(RailConvertCost(raw_secondary_type, totype));
						} else {
							cost.AddCost(RailConvertCost(type, totype) * CountBits(GetTrackBits(tile)));
						}
						break;
				}
				break;

			case MP_TUNNELBRIDGE: {
				TileIndex endtile = GetOtherTunnelBridgeEnd(tile);

				/* If both ends of tunnel/bridge are in the range, do not try to convert twice -
				 * it would cause assert because of different test and exec runs */
				if (endtile < tile) {
					if (diagonal) {
						if (DiagonalTileArea(area_start, area_end).Contains(endtile)) continue;
					} else {
						if (OrthogonalTileArea(area_start, area_end).Contains(endtile)) continue;
					}
				}

				/* When not converting rail <-> el. rail, any vehicle cannot be in tunnel/bridge */
				if (!IsCompatibleRail(type, totype) || !IsCompatibleRail(secondary_type, totype)) {
					CommandCost ret = EnsureNoIncompatibleRailtypeTrainOnTrackBits(tile, TRACK_BIT_MASK | TRACK_BIT_WORMHOLE, totype);
					if (ret.Succeeded()) {
						ret = EnsureNoIncompatibleRailtypeTrainOnTrackBits(endtile, TRACK_BIT_MASK | TRACK_BIT_WORMHOLE, totype);
					}
					if (ret.Failed()) {
						error = ret;
						continue;
					}
				}

				uint num_primary_pieces = GetTunnelBridgeLength(tile, endtile) + CountBits(GetPrimaryTunnelBridgeTrackBits(tile)) + CountBits(GetPrimaryTunnelBridgeTrackBits(endtile));
				found_convertible_track = true;
				cost.AddCost(num_primary_pieces * RailConvertCost(type, totype));
				RailType end_secondary_type = GetTileSecondaryRailTypeIfValid(endtile);
				if (raw_secondary_type != INVALID_RAILTYPE) cost.AddCost(RailConvertCost(raw_secondary_type, totype));
				if (end_secondary_type != INVALID_RAILTYPE) cost.AddCost(RailConvertCost(end_secondary_type, totype));

				CommandCost ret = find_train_reservations(tile, GetTunnelBridgeReservationTrackBits(tile));
				if (ret.Failed()) return ret;
				ret = find_train_reservations(endtile, GetTunnelBridgeReservationTrackBits(endtile));
				if (ret.Failed()) return ret;
				if ((uint)(GetRailTypeInfo(type)->max_speed - 1) > (uint)(GetRailTypeInfo(totype)->max_speed - 1)) {
					ret = CheckTrainInTunnelBridgePreventsTrackModification(tile, endtile);
					if (ret.Failed()) return ret;
				}

				if (flags & DC_EXEC) {
					SubtractRailTunnelBridgeInfrastructure(tile, endtile);

					SetRailType(tile, totype);
					SetRailType(endtile, totype);
					SetSecondaryRailType(tile, totype);
					SetSecondaryRailType(endtile, totype);

					FindVehicleOnPos(tile, VEH_TRAIN, &affected_trains, &UpdateTrainPowerProc);
					FindVehicleOnPos(endtile, VEH_TRAIN, &affected_trains, &UpdateTrainPowerProc);

					/* notify YAPF about the track layout change */
					yapf_notify_track_change(tile, GetTunnelBridgeTrackBits(tile));
					yapf_notify_track_change(endtile, GetTunnelBridgeTrackBits(endtile));

					MarkBridgeOrTunnelDirty(tile, endtile);

					AddRailTunnelBridgeInfrastructure(tile, endtile);
					DirtyCompanyInfrastructureWindows(Company::Get(GetTileOwner(tile))->index);
				}
				break;
			}

			default: // MP_STATION, MP_ROAD
				if (flags & DC_EXEC) {
					Track track = ((tt == MP_STATION) ? GetRailStationTrack(tile) : GetCrossingRailTrack(tile));
					YapfNotifyTrackLayoutChange(tile, track);
				}

				found_convertible_track = true;
				cost.AddCost(RailConvertCost(type, totype));
				break;
		}

		for (uint i = 0; i < vehicles_affected.size(); ++i) {
			ReReserveTrainPath(vehicles_affected[i]);
		}
	}

	if (flags & DC_EXEC) {
		/* Railtype changed, update trains as when entering different track */
		for (Train *v : affected_trains) {
			v->ConsistChanged(CCF_TRACK);
		}
	}

	return found_convertible_track ? cost : error;
}

/**
 * Convert rail on a stretch of track.
 * @param flags operation to perform
 * @param end_tile end tile of drag
 * @param tile start tile of drag
 * @param start_track track-orientation, valid values: 0-5 (Track enum)
 * @param totype railroad type normal/maglev (0 = normal, 1 = mono, 2 = maglev)
 * @param text unused
 * @return the cost of this operation or an error
 */
CommandCost CmdConvertRailTrack(DoCommandFlag flags, TileIndex end_tile, TileIndex tile, Track start_track, RailType totype)
{
	if (!ValParamRailType(totype)) return CMD_ERROR;
	if (tile >= Map::Size()) return CMD_ERROR;

	Trackdir trackdir = TrackToTrackdir(start_track);

	CommandCost ret = ValidateAutoDrag(&trackdir, tile, end_tile);
	if (ret.Failed()) return ret;

	TrainList affected_trains;

	CommandCost cost(EXPENSES_CONSTRUCTION);
	CommandCost error = CommandCost(STR_ERROR_NO_SUITABLE_RAILROAD_TRACK); // by default, there is no track to convert.
	bool found_convertible_track = false; // whether we actually did convert some track (see bug #7633)

	std::vector<TileIndex> exclude_tiles;

	auto advance_tile = [&]() -> bool {
		if (tile == end_tile) return false;

		tile += ToTileIndexDiff(_trackdelta[trackdir]);

		/* toggle railbit for the non-diagonal tracks */
		if (!IsDiagonalTrackdir(trackdir)) ToggleBit(trackdir, 0);
		return true;
	};
	do {
		if (std::find(exclude_tiles.begin(), exclude_tiles.end(), tile) != exclude_tiles.end()) continue;

		const Track track = TrackdirToTrack(trackdir);
		const TileType tt = GetTileType(tile);

		TrackBits all_track_bits = TRACK_BIT_NONE;

		/* Check if our track piece matches any track on tile */
		switch (tt) {
			case MP_RAILWAY:
				if (IsPlainRail(tile)) {
					if (!HasTrack(tile, track)) continue;
					all_track_bits = GetTrackBits(tile);
				} else if (IsRailDepot(tile)) {
					if (GetRailDepotTrack(tile) != track) continue;
					all_track_bits = TrackToTrackBits(track);
				} else {
					continue;
				}
				break;
			case MP_STATION:
				if (!HasStationRail(tile) || GetRailStationTrack(tile) != track) continue;
				all_track_bits = GetRailStationTrackBits(tile);
				break;
			case MP_ROAD:
				if (!IsLevelCrossing(tile) || GetCrossingRailTrack(tile) != track) continue;
				if (RailNoLevelCrossings(totype)) {
					error.MakeError(STR_ERROR_CROSSING_DISALLOWED_RAIL);
					continue;
				}
				all_track_bits = GetCrossingRailBits(tile);
				break;
			case MP_TUNNELBRIDGE:
				if (GetTunnelBridgeTransportType(tile) != TRANSPORT_RAIL || !HasBit(GetTunnelBridgeTrackBits(tile), track)) continue;
				all_track_bits = GetTunnelBridgeTrackBits(tile);
				break;
			default: continue;
		}

		/* Original railtype we are converting from */
		const RailType type = GetRailTypeByTrack(tile, track);

		/* Converting to the same type or converting 'hidden' elrail -> rail */
		if (type == totype || (_settings_game.vehicle.disable_elrails && totype == RAILTYPE_RAIL && type == RAILTYPE_ELECTRIC)) continue;

		/* Trying to convert other's rail */
		CommandCost ret = CheckTileOwnership(tile);
		if (ret.Failed()) {
			error = ret;
			continue;
		}

		/* Track bits on the tile to convert */
		const TrackBits track_bits = (all_track_bits == TRACK_BIT_HORZ || all_track_bits == TRACK_BIT_VERT) ? TrackToTrackBits(track) : all_track_bits;

		std::vector<Train *> vehicles_affected;

		auto find_train_reservations = [&vehicles_affected, &totype, &flags](TileIndex tile, TrackBits reserved) -> CommandCost {
			if (!(flags & DC_EXEC) && _settings_game.vehicle.train_braking_model != TBM_REALISTIC) {
				/* Nothing to do */
				return CommandCost();
			}
			Track track;
			while ((track = RemoveFirstTrack(&reserved)) != INVALID_TRACK) {
				Train *v = GetTrainForReservation(tile, track);
				bool check_train = false;
				if (v != nullptr && !HasPowerOnRail(v->railtype, totype)) {
					check_train = true;
				} else if (v != nullptr && _settings_game.vehicle.train_braking_model == TBM_REALISTIC) {
					RailType original = GetRailTypeByTrack(tile, track);
					if ((uint)(GetRailTypeInfo(original)->max_speed - 1) > (uint)(GetRailTypeInfo(totype)->max_speed - 1)) {
						check_train = true;
					}
				}
				if (check_train) {
					CommandCost ret = CheckTrainReservationPreventsTrackModification(v);
					if (ret.Failed()) return ret;

					/* No power on new rail type, reroute. */
					if (flags & DC_EXEC) {
						FreeTrainTrackReservation(v);
						vehicles_affected.push_back(v);
					}
				}
			}
			return CommandCost();
		};

		auto yapf_notify_track_change = [](TileIndex tile, TrackBits tracks) {
			while (tracks != TRACK_BIT_NONE) {
				YapfNotifyTrackLayoutChange(tile, RemoveFirstTrack(&tracks));
			}
		};

		/* Vehicle on the tile when not converting Rail <-> ElRail
		 * Tunnels and bridges have special check later */
		if (tt != MP_TUNNELBRIDGE) {
			if (!IsCompatibleRail(type, totype)) {
				CommandCost ret = IsPlainRailTile(tile) ? EnsureNoIncompatibleRailtypeTrainOnTrackBits(tile, track_bits, totype) : EnsureNoIncompatibleRailtypeTrainOnGround(tile, totype);
				if (ret.Failed()) {
					error = ret;
					continue;
				}
			}
			CommandCost ret = find_train_reservations(tile, GetReservedTrackbits(tile) & track_bits);
			if (ret.Failed()) return ret;
			if (flags & DC_EXEC) { // we can safely convert, too
				/* Update the company infrastructure counters. */
				if (!IsRailStationTile(tile) || !IsStationTileBlocked(tile)) {
					Company *c = Company::Get(GetTileOwner(tile));
					uint num_pieces = IsLevelCrossingTile(tile) ? LEVELCROSSING_TRACKBIT_FACTOR : 1;
					if (IsPlainRailTile(tile)) {
						num_pieces = CountBits(track_bits);
						if (TracksOverlap(track_bits)) num_pieces *= num_pieces;
					}
					c->infrastructure.rail[type] -= num_pieces;
					c->infrastructure.rail[totype] += num_pieces;
					DirtyCompanyInfrastructureWindows(c->index);
				}

				if (track_bits != all_track_bits) {
					/* only partially converting the tile */
					if (track_bits & TRACK_BIT_RT_1) {
						SetRailType(tile, totype);
					} else {
						SetSecondaryRailType(tile, totype);
					}
				} else {
					SetRailType(tile, totype);
					if (IsPlainRailTile(tile)) SetSecondaryRailType(tile, totype);
				}

				MarkTileDirtyByTile(tile);
				/* update power of train on this tile */
				UpdateTrainPowerProcData data;
				data.train_list = &affected_trains;
				data.track_bits = track_bits;
				FindVehicleOnPos(tile, VEH_TRAIN, &data, &UpdateTrainPowerProcOnTrackBits);
			}
		}

		switch (tt) {
			case MP_RAILWAY:
				switch (GetRailTileType(tile)) {
					case RAIL_TILE_DEPOT:
						if (flags & DC_EXEC) {
							/* notify YAPF about the track layout change */
							YapfNotifyTrackLayoutChange(tile, GetRailDepotTrack(tile));

							/* Update build vehicle window related to this depot */
							InvalidateWindowData(WC_VEHICLE_DEPOT, tile.base());
							InvalidateWindowData(WC_BUILD_VEHICLE, tile.base());
						}
						found_convertible_track = true;
						cost.AddCost(RailConvertCost(type, totype));
						break;

					default: // RAIL_TILE_NORMAL, RAIL_TILE_SIGNALS
						if (flags & DC_EXEC) {
							/* notify YAPF about the track layout change */
							yapf_notify_track_change(tile, track_bits);
						}
						found_convertible_track = true;
						cost.AddCost(RailConvertCost(type, totype) * CountBits(track_bits));
						break;
				}
				break;

			case MP_TUNNELBRIDGE: {
				TileIndex endtile = GetOtherTunnelBridgeEnd(tile);

				const bool across = (GetAcrossTunnelBridgeTrackBits(tile) & track_bits) != TRACK_BIT_NONE;
				if (across) exclude_tiles.push_back(endtile);

				/* When not converting rail <-> el. rail, any vehicle cannot be in tunnel/bridge */
				if (!IsCompatibleRail(type, totype)) {
					CommandCost ret;
					if (across) {
						ret = EnsureNoIncompatibleRailtypeTrainOnTrackBits(tile, track_bits | TRACK_BIT_WORMHOLE, totype);
						if (ret.Succeeded()) {
							ret = EnsureNoIncompatibleRailtypeTrainOnTrackBits(endtile, GetPrimaryTunnelBridgeTrackBits(endtile) | TRACK_BIT_WORMHOLE, totype);
						}
					} else {
						ret = EnsureNoIncompatibleRailtypeTrainOnTrackBits(tile, track_bits, totype);
					}
					if (ret.Failed()) {
						error = ret;
						continue;
					}
				}

				found_convertible_track = true;

				if (across) {
					uint num_primary_pieces = GetTunnelBridgeLength(tile, endtile) + CountBits(GetPrimaryTunnelBridgeTrackBits(tile)) + CountBits(GetPrimaryTunnelBridgeTrackBits(endtile));
					cost.AddCost(num_primary_pieces * RailConvertCost(type, totype));
				} else {
					cost.AddCost(RailConvertCost(type, totype));
				}

				CommandCost ret = find_train_reservations(tile, GetTunnelBridgeReservationTrackBits(tile) & track_bits);
				if (ret.Failed()) return ret;
				if (across) {
					ret = find_train_reservations(endtile, GetTunnelBridgeReservationTrackBits(endtile) & GetPrimaryTunnelBridgeTrackBits(endtile));
					if (ret.Failed()) return ret;
				}
				if (across && (uint)(GetRailTypeInfo(type)->max_speed - 1) > (uint)(GetRailTypeInfo(totype)->max_speed - 1)) {
					ret = CheckTrainInTunnelBridgePreventsTrackModification(tile, endtile);
					if (ret.Failed()) return ret;
				}

				if (flags & DC_EXEC) {
					SubtractRailTunnelBridgeInfrastructure(tile, endtile);

					if (across) {
						SetRailType(tile, totype);
						SetRailType(endtile, totype);
					} else {
						SetSecondaryRailType(tile, totype);
					}

					UpdateTrainPowerProcData data;
					data.train_list = &affected_trains;
					data.track_bits = track_bits;
					if (across) {
						FindVehicleOnPos(tile, VEH_TRAIN, &data, &UpdateTrainPowerProcAcrossTunnelBridge);
						data.track_bits = GetPrimaryTunnelBridgeTrackBits(endtile);
						FindVehicleOnPos(endtile, VEH_TRAIN, &data, &UpdateTrainPowerProcAcrossTunnelBridge);
					} else {
						FindVehicleOnPos(tile, VEH_TRAIN, &data, &UpdateTrainPowerProcOnTrackBits);
					}

					/* notify YAPF about the track layout change */
					yapf_notify_track_change(tile, track_bits);
					if (across) yapf_notify_track_change(endtile, GetPrimaryTunnelBridgeTrackBits(endtile));

					MarkBridgeOrTunnelDirty(tile, endtile);

					AddRailTunnelBridgeInfrastructure(tile, endtile);
					DirtyCompanyInfrastructureWindows(Company::Get(GetTileOwner(tile))->index);
				}
				break;
			}

			default: // MP_STATION, MP_ROAD
				if (flags & DC_EXEC) {
					YapfNotifyTrackLayoutChange(tile, track);
				}

				found_convertible_track = true;
				cost.AddCost(RailConvertCost(type, totype));
				break;
		}

		for (uint i = 0; i < vehicles_affected.size(); ++i) {
			ReReserveTrainPath(vehicles_affected[i]);
		}
	} while (advance_tile());

	if (flags & DC_EXEC) {
		/* Railtype changed, update trains as when entering different track */
		for (Train *v : affected_trains) {
			v->ConsistChanged(CCF_TRACK);
		}
	}

	return found_convertible_track ? cost : error;
}

static CommandCost RemoveTrainDepot(TileIndex tile, DoCommandFlag flags)
{
	if (_current_company != OWNER_WATER) {
		CommandCost ret = CheckTileOwnership(tile);
		if (ret.Failed()) return ret;
	}

	CommandCost ret = EnsureNoVehicleOnGround(tile);
	if (ret.Failed()) return ret;

	/* read variables before the depot is removed */
	DiagDirection dir = GetRailDepotDirection(tile);

	Train *v = nullptr;
	if (HasDepotReservation(tile)) {
		v = GetTrainForReservation(tile, DiagDirToDiagTrack(dir));
		if (v != nullptr) {
			CommandCost ret = CheckTrainReservationPreventsTrackModification(v);
			if (ret.Failed()) return ret;
		}
	}

	if (flags & DC_EXEC) {
		/* read variables before the depot is removed */
		Owner owner = GetTileOwner(tile);

		if (v != nullptr) FreeTrainTrackReservation(v);

		Company::Get(owner)->infrastructure.rail[GetRailType(tile)]--;
		DirtyCompanyInfrastructureWindows(owner);

		delete Depot::GetByTile(tile);
		DoClearSquare(tile);
		AddSideToSignalBuffer(tile, dir, owner);
		YapfNotifyTrackLayoutChange(tile, DiagDirToDiagTrack(dir));
		if (v != nullptr) ReReserveTrainPath(v);
		DeleteNewGRFInspectWindow(GSF_RAILTYPES, tile.base());
	}

	return CommandCost(EXPENSES_CONSTRUCTION, _price[PR_CLEAR_DEPOT_TRAIN]);
}

static CommandCost ClearTile_Track(TileIndex tile, DoCommandFlag flags)
{
	CommandCost cost(EXPENSES_CONSTRUCTION);

	if (flags & DC_AUTO) {
		if (!IsTileOwner(tile, _current_company)) {
			return CommandCost(STR_ERROR_AREA_IS_OWNED_BY_ANOTHER);
		}

		if (IsPlainRail(tile)) {
			return CommandCost(STR_ERROR_MUST_REMOVE_RAILROAD_TRACK);
		} else {
			return CommandCost(STR_ERROR_BUILDING_MUST_BE_DEMOLISHED);
		}
	}

	switch (GetRailTileType(tile)) {
		case RAIL_TILE_SIGNALS:
			if (flags & DC_EXEC) CheckRemoveSignalsFromTile(tile);
			// FALL THROUGH

		case RAIL_TILE_NORMAL: {
			Slope tileh = GetTileSlope(tile);
			/* Is there flat water on the lower halftile that gets cleared expensively? */
			bool water_ground = (GetRailGroundType(tile) == RAIL_GROUND_WATER && IsSlopeWithOneCornerRaised(tileh));

			TrackBits tracks = GetTrackBits(tile);
			while (tracks != TRACK_BIT_NONE) {
				Track track = RemoveFirstTrack(&tracks);
				CommandCost ret = Command<CMD_REMOVE_SINGLE_RAIL>::Do(flags, tile, track);
				if (ret.Failed()) return ret;
				cost.AddCost(ret);
			}

			/* When bankrupting, don't make water dirty, there could be a ship on lower halftile.
			 * Same holds for non-companies clearing the tile, e.g. disasters. */
			if (water_ground && !(flags & DC_BANKRUPT) && Company::IsValidID(_current_company)) {
				CommandCost ret = EnsureNoVehicleOnGround(tile);
				if (ret.Failed()) return ret;

				if (_game_mode != GM_EDITOR && !_settings_game.construction.enable_remove_water && !(flags & DC_ALLOW_REMOVE_WATER)) return CommandCost(STR_ERROR_CAN_T_BUILD_ON_WATER);

				/* The track was removed, and left a coast tile. Now also clear the water. */
				if (flags & DC_EXEC) {
					bool remove = IsDockingTile(tile);
					DoClearSquare(tile);
					if (remove) RemoveDockingTile(tile);
					InvalidateWaterRegion(tile);
				}
				cost.AddCost(_price[PR_CLEAR_WATER]);
			}

			return cost;
		}

		case RAIL_TILE_DEPOT:
			return RemoveTrainDepot(tile, flags);

		default:
			return CMD_ERROR;
	}
}

/**
 * Get surface height in point (x,y)
 * On tiles with halftile foundations move (x,y) to a safe point wrt. track
 */
static uint GetSaveSlopeZ(uint x, uint y, Track track)
{
	switch (track) {
		case TRACK_UPPER: x &= ~0xF; y &= ~0xF; break;
		case TRACK_LOWER: x |=  0xF; y |=  0xF; break;
		case TRACK_LEFT:  x |=  0xF; y &= ~0xF; break;
		case TRACK_RIGHT: x &= ~0xF; y |=  0xF; break;
		default: break;
	}
	return GetSlopePixelZ(x, y);
}

static void GetSignalXY(TileIndex tile, uint pos, bool opposite, uint &x, uint &y)
{
	bool side;
	switch (_settings_game.construction.train_signal_side) {
		case 0:  side = false;                                 break; // left
		case 2:  side = true;                                  break; // right
		default: side = _settings_game.vehicle.road_side != 0; break; // driving side
	}
	side ^= opposite;
	static const Point SignalPositions[2][12] = {
		{ // Signals on the left side
		/*  LEFT      LEFT      RIGHT     RIGHT     UPPER     UPPER */
			{ 8,  5}, {14,  1}, { 1, 14}, { 9, 11}, { 1,  0}, { 3, 10},
		/*  LOWER     LOWER     X         X         Y         Y     */
			{11,  4}, {14, 14}, {11,  3}, { 4, 13}, { 3,  4}, {11, 13}
		}, { // Signals on the right side
		/*  LEFT      LEFT      RIGHT     RIGHT     UPPER     UPPER */
			{14,  1}, {12, 10}, { 4,  6}, { 1, 14}, {10,  4}, { 0,  1},
		/*  LOWER     LOWER     X         X         Y         Y     */
			{14, 14}, { 5, 12}, {11, 13}, { 4,  3}, {13,  4}, { 3, 11}
		}
	};

	x = TileX(tile) * TILE_SIZE + SignalPositions[side][pos].x;
	y = TileY(tile) * TILE_SIZE + SignalPositions[side][pos].y;
}

void DrawRestrictedSignal(SignalType type, SpriteID sprite, int x, int y, int z, int dz, int bb_offset_z)
{
	SpriteFile *file = GetOriginFile(sprite);
	if (file != nullptr && (file->flags & SFF_OPENTTDGRF)) {
		static const SubSprite lower_part = { -50,  -8, 50,  50 };
		static const SubSprite upper_part = { -50, -50, 50, -9 };
		static const SubSprite lower_part_plain = { -50,  -5, 50,  50 };
		static const SubSprite upper_part_plain = { -50, -50, 50, -6 };

		AddSortableSpriteToDraw(sprite, SPR_TRACERESTRICT_BASE + 2, x, y, 1, 1, dz, z, false, 0, 0, bb_offset_z, (type == SIGTYPE_BLOCK) ? &lower_part_plain : &lower_part);
		AddSortableSpriteToDraw(sprite,                   PAL_NONE, x, y, 1, 1, dz, z, false, 0, 0, bb_offset_z, (type == SIGTYPE_BLOCK) ? &upper_part_plain : &upper_part);
	} else if (type == SIGTYPE_PBS || type == SIGTYPE_PBS_ONEWAY) {
		static const SubSprite lower_part = { -50, -10, 50,  50 };
		static const SubSprite upper_part = { -50, -50, 50, -11 };

		AddSortableSpriteToDraw(sprite, SPR_TRACERESTRICT_BASE, x, y, 1, 1, dz, z, false, 0, 0, bb_offset_z, &lower_part);
		AddSortableSpriteToDraw(sprite,               PAL_NONE, x, y, 1, 1, dz, z, false, 0, 0, bb_offset_z, &upper_part);
	} else {
		AddSortableSpriteToDraw(sprite, SPR_TRACERESTRICT_BASE + (type == SIGTYPE_NO_ENTRY ? 0 : 1), x, y, 1, 1, dz, z, false, 0, 0, bb_offset_z);
	}
}

void DrawSingleSignal(TileIndex tile, const RailTypeInfo *rti, Track track, SignalState condition, SignalOffsets image, uint pos, SignalType type,
		SignalVariant variant, const TraceRestrictProgram *prog, CustomSignalSpriteContext context)
{
	uint8_t style = 0;
	if (_num_new_signal_styles > 0) {
		switch (context.ctx_mode) {
			case CSSC_TRACK:
				style = GetSignalStyle(tile, track);
				break;

			case CSSC_TUNNEL_BRIDGE_ENTRANCE:
			case CSSC_TUNNEL_BRIDGE_EXIT:
				style = GetTunnelBridgeSignalStyle(tile);
				break;

			default:
				break;
		}
	}

	if (HasBit(_signal_style_masks.signal_both_sides, style) && ((context.ctx_flags & CSSCF_SECOND_SIGNAL) == 0)) {
		/* Draw second signal on opposite side */
		DrawSingleSignal(tile, rti, track, condition, image, pos, type, variant, prog, { context.ctx_mode, context.ctx_flags | CSSCF_SECOND_SIGNAL });
	}

	bool show_restricted = (prog != nullptr);

	if (type == SIGTYPE_NO_ENTRY) {
		pos ^= 1;
		condition = SIGNAL_STATE_RED;
	}

	uint x, y;
	GetSignalXY(tile, pos, HasBit(_signal_style_masks.signal_opposite_side, style) != ((context.ctx_flags & CSSCF_SECOND_SIGNAL) != 0), x, y);

	uint8_t aspect;
	if (condition == SIGNAL_STATE_GREEN) {
		aspect = 1;
		if (_extra_aspects > 0) {
			switch (context.ctx_mode) {
				case CSSC_TRACK:
					aspect = GetSignalAspect(tile, track);
					break;

				case CSSC_TUNNEL_BRIDGE_ENTRANCE:
					aspect = GetTunnelBridgeEntranceSignalAspect(tile);
					break;

				case CSSC_TUNNEL_BRIDGE_EXIT:
					aspect = GetTunnelBridgeExitSignalAspect(tile);
					break;

				default:
					break;
			}
		}
	} else {
		aspect = 0;
	}

	const uint z = GetSaveSlopeZ(x, y, track);

	const CustomSignalSpriteResult result = GetCustomSignalSprite(rti, tile, type, variant, aspect, context, style, prog, z);
	SpriteID sprite = result.sprite.sprite;
	PaletteID pal = PAL_NONE;
	bool is_custom_sprite = (sprite != 0);
	if (sprite != 0) {
		sprite += image;
		pal = result.sprite.pal;
	} else if (type == SIGTYPE_PROG) {
		if (variant == SIG_SEMAPHORE) {
			sprite = SPR_PROGSIGNAL_BASE + image * 2 + condition;
		} else {
			sprite = SPR_PROGSIGNAL_BASE + 16 + image * 2 + condition;
		}

		SpriteFile *file = GetOriginFile(sprite);
		is_custom_sprite = !(file != nullptr && file->flags & SFF_PROGSIG);
	} else if (type == SIGTYPE_NO_ENTRY) {
		if (variant == SIG_SEMAPHORE) {
			sprite = SPR_EXTRASIGNAL_BASE + image;
		} else {
			sprite = SPR_EXTRASIGNAL_BASE + 8 + image;
		}

		SpriteFile *file = GetOriginFile(sprite);
		is_custom_sprite = !(file != nullptr && file->flags & SFF_PROGSIG);
	} else {
		/* Normal electric signals are stored in a different sprite block than all other signals. */
		sprite = (type == SIGTYPE_BLOCK && variant == SIG_ELECTRIC) ? SPR_ORIGINAL_SIGNALS_BASE : SPR_SIGNALS_BASE - 16;
		sprite += type * 16 + variant * 64 + image * 2 + condition + (IsSignalSpritePBS(type) ? 64 : 0);

		SpriteFile *file = GetOriginFile(sprite);
		is_custom_sprite = (file != nullptr) && (file->flags & SFF_USERGRF);
	}

	if (style == 0 && (_settings_client.gui.show_all_signal_default == SSDM_ON ||
			(is_custom_sprite && show_restricted && _settings_client.gui.show_restricted_signal_recolour &&
			_settings_client.gui.show_all_signal_default == SSDM_RESTRICTED_RECOLOUR && !result.restricted_valid && variant == SIG_ELECTRIC))) {
		/* Use duplicate sprite block, instead of GRF-specified signals */
		if (type == SIGTYPE_PROG) {
			if (variant == SIG_SEMAPHORE) {
				sprite = SPR_DUP_PROGSIGNAL_BASE + image * 2 + condition;
			} else {
				sprite = SPR_DUP_PROGSIGNAL_BASE + 16 + image * 2 + condition;
			}
		} else if (type == SIGTYPE_NO_ENTRY) {
			if (variant == SIG_SEMAPHORE) {
				sprite = SPR_DUP_EXTRASIGNAL_BASE + image;
			} else {
				sprite = SPR_DUP_EXTRASIGNAL_BASE + 8 + image;
			}
		} else {
			sprite = (type == SIGTYPE_BLOCK && variant == SIG_ELECTRIC) ? SPR_DUP_ORIGINAL_SIGNALS_BASE : SPR_DUP_SIGNALS_BASE - 16;
			sprite += type * 16 + variant * 64 + image * 2 + condition + (IsSignalSpritePBS(type) ? 64 : 0);
		}
		pal = PAL_NONE;
		is_custom_sprite = false;
	}

	if (!is_custom_sprite && show_restricted && variant == SIG_ELECTRIC && _settings_client.gui.show_restricted_signal_recolour) {
		DrawRestrictedSignal(type, sprite, x, y, z, BB_HEIGHT_UNDER_BRIDGE, 0);
	} else {
		AddSortableSpriteToDraw(sprite, pal, x, y, 1, 1, BB_HEIGHT_UNDER_BRIDGE, z);
	}
	const Sprite *sp = GetSprite(sprite, SpriteType::Normal, 0);
	if (sp->x_offs < -SIGNAL_DIRTY_LEFT || sp->x_offs + sp->width > SIGNAL_DIRTY_RIGHT || sp->y_offs < -SIGNAL_DIRTY_TOP || sp->y_offs + sp->height > SIGNAL_DIRTY_BOTTOM) {
		_signal_sprite_oversized = true;
	}
}

static void DrawSingleSignal(TileIndex tile, const RailTypeInfo *rti, Track track, SignalState condition, SignalOffsets image, uint pos)
{
	SignalType type       = GetSignalType(tile, track);
	SignalVariant variant = GetSignalVariant(tile, track);

	const TraceRestrictProgram *prog = IsRestrictedSignal(tile) ? GetExistingTraceRestrictProgram(tile, track) : nullptr;
	DrawSingleSignal(tile, rti, track, condition, image, pos, type, variant, prog, { CSSC_TRACK });
}

static void GetSignalXYByTrackdir(TileIndex tile, Trackdir td, bool opposite, uint &x, uint &y)
{
	static const uint8_t trackdir_to_pos[TRACKDIR_END] = {
		8,  // TRACKDIR_X_NE
		10, // TRACKDIR_Y_SE
		4,  // TRACKDIR_UPPER_E
		6,  // TRACKDIR_LOWER_E
		0,  // TRACKDIR_LEFT_S
		2,  // TRACKDIR_RIGHT_S
		0,  // TRACKDIR_RVREV_NE
		0,  // TRACKDIR_RVREV_SE
		9,  // TRACKDIR_X_SW
		11, // TRACKDIR_Y_NW
		5,  // TRACKDIR_UPPER_W
		7,  // TRACKDIR_LOWER_W
		1,  // TRACKDIR_LEFT_N
		3,  // TRACKDIR_RIGHT_N
		0,  // TRACKDIR_RVREV_SW
		0,  // TRACKDIR_RVREV_NW
	};

	GetSignalXY(tile, trackdir_to_pos[td], opposite, x, y);
}

void GetSignalXYZByTrackdir(TileIndex tile, Trackdir td, bool opposite_side, uint &x, uint &y, uint &z)
{
	GetSignalXYByTrackdir(tile, td, opposite_side, x, y);
	z = GetSaveSlopeZ(x, y, TrackdirToTrack(td));
}

template <typename F>
void MarkSingleSignalDirtyIntl(TileIndex tile, Trackdir td, bool opposite, F get_z)
{
	uint x, y;
	GetSignalXYByTrackdir(tile, td, opposite, x, y);
	Point pt = RemapCoords(x, y, get_z(x, y));
	MarkAllViewportsDirty(
			pt.x - SIGNAL_DIRTY_LEFT,
			pt.y - SIGNAL_DIRTY_TOP,
			pt.x + SIGNAL_DIRTY_RIGHT,
			pt.y + SIGNAL_DIRTY_BOTTOM,
			VMDF_NOT_MAP_MODE
	);
}

void MarkSingleSignalDirty(TileIndex tile, Trackdir td)
{
	if (_signal_sprite_oversized || td >= TRACKDIR_END) {
		MarkTileDirtyByTile(tile, VMDF_NOT_MAP_MODE);
		return;
	}
	bool opposite = false;
	if (_signal_style_masks.signal_opposite_side != 0) {
		opposite = HasBit(_signal_style_masks.signal_opposite_side, GetSignalStyleGeneric(tile, TrackdirToTrack(td)));
	}
	MarkSingleSignalDirtyIntl(tile, td, opposite, [td](uint x, uint y) -> uint {
		return GetSaveSlopeZ(x, y, TrackdirToTrack(td));
	});

	if (_signal_style_masks.signal_both_sides == 0 || !HasBit(_signal_style_masks.signal_both_sides, GetSignalStyleGeneric(tile, TrackdirToTrack(td)))) return;
	MarkSingleSignalDirtyIntl(tile, td, !opposite, [td](uint x, uint y) -> uint {
		return GetSaveSlopeZ(x, y, TrackdirToTrack(td));
	});
}

void MarkSingleSignalDirtyAtZ(TileIndex tile, Trackdir td, bool opposite_side, uint z)
{
	MarkSingleSignalDirtyIntl(tile, td, opposite_side, [z](uint x, uint y) -> uint {
		return z;
	});
}

static uint32_t _drawtile_track_palette;



/** Offsets for drawing fences */
struct FenceOffset {
	Corner height_ref;  //!< Corner to use height offset from.
	int x_offs;         //!< Bounding box X offset.
	int y_offs;         //!< Bounding box Y offset.
	int x_size;         //!< Bounding box X size.
	int y_size;         //!< Bounding box Y size.
};

/** Offsets for drawing fences */
static FenceOffset _fence_offsets[] = {
	{ CORNER_INVALID,  0,  1, 16,  1 }, // RFO_FLAT_X_NW
	{ CORNER_INVALID,  1,  0,  1, 16 }, // RFO_FLAT_Y_NE
	{ CORNER_W,        8,  8,  1,  1 }, // RFO_FLAT_LEFT
	{ CORNER_N,        8,  8,  1,  1 }, // RFO_FLAT_UPPER
	{ CORNER_INVALID,  0,  1, 16,  1 }, // RFO_SLOPE_SW_NW
	{ CORNER_INVALID,  1,  0,  1, 16 }, // RFO_SLOPE_SE_NE
	{ CORNER_INVALID,  0,  1, 16,  1 }, // RFO_SLOPE_NE_NW
	{ CORNER_INVALID,  1,  0,  1, 16 }, // RFO_SLOPE_NW_NE
	{ CORNER_INVALID,  0, 15, 16,  1 }, // RFO_FLAT_X_SE
	{ CORNER_INVALID, 15,  0,  1, 16 }, // RFO_FLAT_Y_SW
	{ CORNER_E,        8,  8,  1,  1 }, // RFO_FLAT_RIGHT
	{ CORNER_S,        8,  8,  1,  1 }, // RFO_FLAT_LOWER
	{ CORNER_INVALID,  0, 15, 16,  1 }, // RFO_SLOPE_SW_SE
	{ CORNER_INVALID, 15,  0,  1, 16 }, // RFO_SLOPE_SE_SW
	{ CORNER_INVALID,  0, 15, 16,  1 }, // RFO_SLOPE_NE_SE
	{ CORNER_INVALID, 15,  0,  1, 16 }, // RFO_SLOPE_NW_SW
};

/**
 * Draw a track fence.
 * @param ti Tile drawing information.
 * @param base_image First fence sprite.
 * @param num_sprites Number of fence sprites.
 * @param rfo Fence to draw.
 */
static void DrawTrackFence(const TileInfo *ti, SpriteID base_image, uint num_sprites, RailFenceOffset rfo)
{
	int z = ti->z;
	if (_fence_offsets[rfo].height_ref != CORNER_INVALID) {
		z += GetSlopePixelZInCorner(RemoveHalftileSlope(ti->tileh), _fence_offsets[rfo].height_ref);
	}
	AddSortableSpriteToDraw(base_image + (rfo % num_sprites), _drawtile_track_palette,
		ti->x + _fence_offsets[rfo].x_offs,
		ti->y + _fence_offsets[rfo].y_offs,
		_fence_offsets[rfo].x_size,
		_fence_offsets[rfo].y_size,
		4, z);
}

/**
 * Draw fence at NW border matching the tile slope.
 */
static void DrawTrackFence_NW(const TileInfo *ti, SpriteID base_image, uint num_sprites)
{
	RailFenceOffset rfo = RFO_FLAT_X_NW;
	if (ti->tileh & SLOPE_NW) rfo = (ti->tileh & SLOPE_W) ? RFO_SLOPE_SW_NW : RFO_SLOPE_NE_NW;
	DrawTrackFence(ti, base_image, num_sprites, rfo);
}

/**
 * Draw fence at SE border matching the tile slope.
 */
static void DrawTrackFence_SE(const TileInfo *ti, SpriteID base_image, uint num_sprites)
{
	RailFenceOffset rfo = RFO_FLAT_X_SE;
	if (ti->tileh & SLOPE_SE) rfo = (ti->tileh & SLOPE_S) ? RFO_SLOPE_SW_SE : RFO_SLOPE_NE_SE;
	DrawTrackFence(ti, base_image, num_sprites, rfo);
}

/**
 * Draw fence at NE border matching the tile slope.
 */
static void DrawTrackFence_NE(const TileInfo *ti, SpriteID base_image, uint num_sprites)
{
	RailFenceOffset rfo = RFO_FLAT_Y_NE;
	if (ti->tileh & SLOPE_NE) rfo = (ti->tileh & SLOPE_E) ? RFO_SLOPE_SE_NE : RFO_SLOPE_NW_NE;
	DrawTrackFence(ti, base_image, num_sprites, rfo);
}

/**
 * Draw fence at SW border matching the tile slope.
 */
static void DrawTrackFence_SW(const TileInfo *ti, SpriteID base_image, uint num_sprites)
{
	RailFenceOffset rfo = RFO_FLAT_Y_SW;
	if (ti->tileh & SLOPE_SW) rfo = (ti->tileh & SLOPE_S) ? RFO_SLOPE_SE_SW : RFO_SLOPE_NW_SW;
	DrawTrackFence(ti, base_image, num_sprites, rfo);
}

/**
 * Draw track fences.
 * @param ti Tile drawing information.
 * @param rti Rail type information.
 */
void DrawTrackDetails(const TileInfo *ti, const RailTypeInfo *rti, const RailGroundType rgt)
{
	/* Base sprite for track fences.
	 * Note: Halftile slopes only have fences on the upper part. */
	uint num_sprites = 0;
	SpriteID base_image = GetCustomRailSprite(rti, ti->tile, RTSG_FENCES, IsHalftileSlope(ti->tileh) ? TCX_UPPER_HALFTILE : TCX_NORMAL, &num_sprites);
	if (base_image == 0) {
		base_image = SPR_TRACK_FENCE_FLAT_X;
		num_sprites = 8;
	}

	assert(num_sprites > 0);

	switch (rgt) {
		case RAIL_GROUND_FENCE_NW:     DrawTrackFence_NW(ti, base_image, num_sprites); break;
		case RAIL_GROUND_FENCE_SE:     DrawTrackFence_SE(ti, base_image, num_sprites); break;
		case RAIL_GROUND_FENCE_SENW:   DrawTrackFence_NW(ti, base_image, num_sprites);
		                               DrawTrackFence_SE(ti, base_image, num_sprites); break;
		case RAIL_GROUND_FENCE_NE:     DrawTrackFence_NE(ti, base_image, num_sprites); break;
		case RAIL_GROUND_FENCE_SW:     DrawTrackFence_SW(ti, base_image, num_sprites); break;
		case RAIL_GROUND_FENCE_NESW:   DrawTrackFence_NE(ti, base_image, num_sprites);
		                               DrawTrackFence_SW(ti, base_image, num_sprites); break;
		case RAIL_GROUND_FENCE_VERT1:  DrawTrackFence(ti, base_image, num_sprites, RFO_FLAT_LEFT);  break;
		case RAIL_GROUND_FENCE_VERT2:  DrawTrackFence(ti, base_image, num_sprites, RFO_FLAT_RIGHT); break;
		case RAIL_GROUND_FENCE_HORIZ1: DrawTrackFence(ti, base_image, num_sprites, RFO_FLAT_UPPER); break;
		case RAIL_GROUND_FENCE_HORIZ2: DrawTrackFence(ti, base_image, num_sprites, RFO_FLAT_LOWER); break;
		case RAIL_GROUND_WATER: {
			Corner track_corner;
			if (IsHalftileSlope(ti->tileh)) {
				/* Steep slope or one-corner-raised slope with halftile foundation */
				track_corner = GetHalftileSlopeCorner(ti->tileh);
			} else {
				/* Three-corner-raised slope */
				track_corner = OppositeCorner(GetHighestSlopeCorner(ComplementSlope(ti->tileh)));
			}
			switch (track_corner) {
				case CORNER_W: DrawTrackFence(ti, base_image, num_sprites, RFO_FLAT_LEFT);  break;
				case CORNER_S: DrawTrackFence(ti, base_image, num_sprites, RFO_FLAT_LOWER); break;
				case CORNER_E: DrawTrackFence(ti, base_image, num_sprites, RFO_FLAT_RIGHT); break;
				case CORNER_N: DrawTrackFence(ti, base_image, num_sprites, RFO_FLAT_UPPER); break;
				default: NOT_REACHED();
			}
			break;
		}
		default: break;
	}
}

/* SubSprite for drawing the track halftile of 'three-corners-raised'-sloped rail sprites. */
static const int INF = 1000; // big number compared to tilesprite size
static const SubSprite _halftile_sub_sprite[4] = {
	{ -INF    , -INF  , 32 - 33, INF     }, // CORNER_W, clip 33 pixels from right
	{ -INF    ,  0 + 7, INF    , INF     }, // CORNER_S, clip 7 pixels from top
	{ -31 + 33, -INF  , INF    , INF     }, // CORNER_E, clip 33 pixels from left
	{ -INF    , -INF  , INF    , 30 - 23 }  // CORNER_N, clip 23 pixels from bottom
};
static const SubSprite _dual_track_halftile_sub_sprite[4] = {
	{ -INF    , -INF  , 32 - 33, INF     }, // CORNER_W, clip 33 pixels from right
	{ -INF    , 0 + 15, INF    , INF     }, // CORNER_S, clip 15 pixels from top
	{ -31 + 33, -INF  , INF    , INF     }, // CORNER_E, clip 33 pixels from left
	{ -INF    , -INF  , INF    , 30 - 15 }  // CORNER_N, clip 15 pixels from bottom
};

static inline void DrawTrackSprite(SpriteID sprite, PaletteID pal, const TileInfo *ti, Slope s)
{
	DrawGroundSprite(sprite, pal, nullptr, 0, (ti->tileh & s) ? -8 : 0);
}

static RailGroundType GetRailOrBridgeGroundType(TileInfo *ti) {
	if (IsTileType(ti->tile, MP_TUNNELBRIDGE)) {
		return GetTunnelBridgeGroundType(ti->tile);
	} else {
		return GetRailGroundType(ti->tile);
	}
}

static void DrawTrackBitsOverlay(TileInfo *ti, TrackBits track, const RailTypeInfo *rti, RailGroundType rgt,  bool is_bridge, Corner halftile_corner, Corner draw_half_tile)
{
	if (halftile_corner != CORNER_INVALID) track &= ~CornerToTrackBits(halftile_corner);

	if (halftile_corner != CORNER_INVALID || draw_half_tile == CORNER_INVALID) {
		/* Draw ground */
		if (rgt == RAIL_GROUND_WATER) {
			if (track != TRACK_BIT_NONE || IsSteepSlope(ti->tileh)) {
				/* three-corner-raised slope or steep slope with track on upper part */
				DrawShoreTile(ti->tileh);
			} else {
				/* single-corner-raised slope with track on upper part */
				DrawGroundSprite(SPR_FLAT_WATER_TILE, PAL_NONE);
			}
		} else {
			SpriteID image;

			switch (rgt) {
				case RAIL_GROUND_BARREN:     image = SPR_FLAT_BARE_LAND;  break;
				case RAIL_GROUND_ICE_DESERT: image = SPR_FLAT_SNOW_DESERT_TILE; break;
				default:                     image = SPR_FLAT_GRASS_TILE; break;
			}

			image += SlopeToSpriteOffset(ti->tileh);

			const SubSprite *sub = nullptr;
			if (draw_half_tile != CORNER_INVALID) sub = &(_halftile_sub_sprite[draw_half_tile]);
			DrawGroundSprite(image, PAL_NONE, sub);
		}
	}

	bool no_combine = ti->tileh == SLOPE_FLAT && rti->flags.Test(RailTypeFlag::NoSpriteCombine);
	SpriteID overlay = GetCustomRailSprite(rti, ti->tile, RTSG_OVERLAY);
	SpriteID ground = GetCustomRailSprite(rti, ti->tile, no_combine ? RTSG_GROUND_COMPLETE : RTSG_GROUND);
	TrackBits pbs = TRACK_BIT_NONE;
	if (_settings_client.gui.show_track_reservation) {
		pbs = (is_bridge ? GetTunnelBridgeReservationTrackBits(ti->tile) : GetRailReservationTrackBits(ti->tile)) & track;
	}

	if (track == TRACK_BIT_NONE) {
		/* Half-tile foundation, no track here? */
	} else if (no_combine) {
		/* Use trackbits as direct index from ground sprite, subtract 1
		 * because there is no sprite for no bits. */
		DrawGroundSprite(ground + track - 1, PAL_NONE);

		/* Draw reserved track bits */
		if (pbs & TRACK_BIT_X)     DrawGroundSprite(overlay + RTO_X, PALETTE_CRASH);
		if (pbs & TRACK_BIT_Y)     DrawGroundSprite(overlay + RTO_Y, PALETTE_CRASH);
		if (pbs & TRACK_BIT_UPPER) DrawTrackSprite(overlay + RTO_N, PALETTE_CRASH, ti, SLOPE_N);
		if (pbs & TRACK_BIT_LOWER) DrawTrackSprite(overlay + RTO_S, PALETTE_CRASH, ti, SLOPE_S);
		if (pbs & TRACK_BIT_RIGHT) DrawTrackSprite(overlay + RTO_E, PALETTE_CRASH, ti, SLOPE_E);
		if (pbs & TRACK_BIT_LEFT)  DrawTrackSprite(overlay + RTO_W, PALETTE_CRASH, ti, SLOPE_W);
	} else if (ti->tileh == SLOPE_NW && track == TRACK_BIT_Y) {
		DrawGroundSprite(ground + RTO_SLOPE_NW, PAL_NONE);
		if (pbs != TRACK_BIT_NONE) DrawGroundSprite(overlay + RTO_SLOPE_NW, PALETTE_CRASH);
	} else if (ti->tileh == SLOPE_NE && track == TRACK_BIT_X) {
		DrawGroundSprite(ground + RTO_SLOPE_NE, PAL_NONE);
		if (pbs != TRACK_BIT_NONE) DrawGroundSprite(overlay + RTO_SLOPE_NE, PALETTE_CRASH);
	} else if (ti->tileh == SLOPE_SE && track == TRACK_BIT_Y) {
		DrawGroundSprite(ground + RTO_SLOPE_SE, PAL_NONE);
		if (pbs != TRACK_BIT_NONE) DrawGroundSprite(overlay + RTO_SLOPE_SE, PALETTE_CRASH);
	} else if (ti->tileh == SLOPE_SW && track == TRACK_BIT_X) {
		DrawGroundSprite(ground + RTO_SLOPE_SW, PAL_NONE);
		if (pbs != TRACK_BIT_NONE) DrawGroundSprite(overlay + RTO_SLOPE_SW, PALETTE_CRASH);
	} else {
		switch (track) {
			/* Draw single ground sprite when not overlapping. No track overlay
			 * is necessary for these sprites. */
			case TRACK_BIT_X:     DrawGroundSprite(ground + RTO_X, PAL_NONE); break;
			case TRACK_BIT_Y:     DrawGroundSprite(ground + RTO_Y, PAL_NONE); break;
			case TRACK_BIT_UPPER: DrawTrackSprite(ground + RTO_N, PAL_NONE, ti, SLOPE_N); break;
			case TRACK_BIT_LOWER: DrawTrackSprite(ground + RTO_S, PAL_NONE, ti, SLOPE_S); break;
			case TRACK_BIT_RIGHT: DrawTrackSprite(ground + RTO_E, PAL_NONE, ti, SLOPE_E); break;
			case TRACK_BIT_LEFT:  DrawTrackSprite(ground + RTO_W, PAL_NONE, ti, SLOPE_W); break;
			case TRACK_BIT_CROSS: DrawGroundSprite(ground + RTO_CROSSING_XY, PAL_NONE); break;
			case TRACK_BIT_HORZ:  DrawTrackSprite(ground + RTO_N, PAL_NONE, ti, SLOPE_N);
			                      DrawTrackSprite(ground + RTO_S, PAL_NONE, ti, SLOPE_S); break;
			case TRACK_BIT_VERT:  DrawTrackSprite(ground + RTO_E, PAL_NONE, ti, SLOPE_E);
			                      DrawTrackSprite(ground + RTO_W, PAL_NONE, ti, SLOPE_W); break;

			default:
				/* We're drawing a junction tile */
				if ((track & TRACK_BIT_3WAY_NE) == 0) {
					DrawGroundSprite(ground + RTO_JUNCTION_SW, PAL_NONE);
				} else if ((track & TRACK_BIT_3WAY_SW) == 0) {
					DrawGroundSprite(ground + RTO_JUNCTION_NE, PAL_NONE);
				} else if ((track & TRACK_BIT_3WAY_NW) == 0) {
					DrawGroundSprite(ground + RTO_JUNCTION_SE, PAL_NONE);
				} else if ((track & TRACK_BIT_3WAY_SE) == 0) {
					DrawGroundSprite(ground + RTO_JUNCTION_NW, PAL_NONE);
				} else {
					DrawGroundSprite(ground + RTO_JUNCTION_NSEW, PAL_NONE);
				}

				/* Mask out PBS bits as we shall draw them afterwards anyway. */
				track &= ~pbs;

				/* Draw regular track bits */
				if (track & TRACK_BIT_X)     DrawGroundSprite(overlay + RTO_X, PAL_NONE);
				if (track & TRACK_BIT_Y)     DrawGroundSprite(overlay + RTO_Y, PAL_NONE);
				if (track & TRACK_BIT_UPPER) DrawGroundSprite(overlay + RTO_N, PAL_NONE);
				if (track & TRACK_BIT_LOWER) DrawGroundSprite(overlay + RTO_S, PAL_NONE);
				if (track & TRACK_BIT_RIGHT) DrawGroundSprite(overlay + RTO_E, PAL_NONE);
				if (track & TRACK_BIT_LEFT)  DrawGroundSprite(overlay + RTO_W, PAL_NONE);
		}

		/* Draw reserved track bits */
		if (pbs & TRACK_BIT_X)     DrawGroundSprite(overlay + RTO_X, PALETTE_CRASH);
		if (pbs & TRACK_BIT_Y)     DrawGroundSprite(overlay + RTO_Y, PALETTE_CRASH);
		if (pbs & TRACK_BIT_UPPER) DrawTrackSprite(overlay + RTO_N, PALETTE_CRASH, ti, SLOPE_N);
		if (pbs & TRACK_BIT_LOWER) DrawTrackSprite(overlay + RTO_S, PALETTE_CRASH, ti, SLOPE_S);
		if (pbs & TRACK_BIT_RIGHT) DrawTrackSprite(overlay + RTO_E, PALETTE_CRASH, ti, SLOPE_E);
		if (pbs & TRACK_BIT_LEFT)  DrawTrackSprite(overlay + RTO_W, PALETTE_CRASH, ti, SLOPE_W);
	}

	if (IsValidCorner(halftile_corner) && (draw_half_tile == halftile_corner || draw_half_tile == CORNER_INVALID)) {
		DrawFoundation(ti, HalftileFoundation(halftile_corner));
		overlay = GetCustomRailSprite(rti, ti->tile, RTSG_OVERLAY, TCX_UPPER_HALFTILE);
		ground = GetCustomRailSprite(rti, ti->tile, RTSG_GROUND, TCX_UPPER_HALFTILE);

		/* Draw higher halftile-overlay: Use the sloped sprites with three corners raised. They probably best fit the lightning. */
		Slope fake_slope = SlopeWithThreeCornersRaised(OppositeCorner(halftile_corner));

		SpriteID image;
		switch (rgt) {
			case RAIL_GROUND_BARREN:     image = SPR_FLAT_BARE_LAND;  break;
			case RAIL_GROUND_ICE_DESERT:
			case RAIL_GROUND_HALF_SNOW:  image = SPR_FLAT_SNOW_DESERT_TILE; break;
			default:                     image = SPR_FLAT_GRASS_TILE; break;
		}

		image += SlopeToSpriteOffset(fake_slope);

		DrawGroundSprite(image, PAL_NONE, &(_halftile_sub_sprite[halftile_corner]));

		track = CornerToTrackBits(halftile_corner);

		int offset;
		switch (track) {
			default: NOT_REACHED();
			case TRACK_BIT_UPPER: offset = RTO_N; break;
			case TRACK_BIT_LOWER: offset = RTO_S; break;
			case TRACK_BIT_RIGHT: offset = RTO_E; break;
			case TRACK_BIT_LEFT:  offset = RTO_W; break;
		}

		DrawTrackSprite(ground + offset, PAL_NONE, ti, fake_slope);
		if (_settings_client.gui.show_track_reservation && HasReservedTracks(ti->tile, track)) {
			DrawTrackSprite(overlay + offset, PALETTE_CRASH, ti, fake_slope);
		}
	}
}

/**
 * Draw ground sprite and track bits
 * @param ti TileInfo
 * @param track TrackBits to draw
 * @param rt Rail type
 * @param half_tile Half tile corner
 */
void DrawTrackBits(TileInfo *ti, TrackBits track, RailType rt, RailGroundType rgt, bool is_bridge, Corner halftile_corner, Corner draw_half_tile)
{
	const RailTypeInfo *rti = GetRailTypeInfo(rt);

	if (rti->UsesOverlay()) {
		DrawTrackBitsOverlay(ti, track, rti, rgt, is_bridge, halftile_corner, draw_half_tile);
		return;
	}

	SpriteID image;
	PaletteID pal = PAL_NONE;
	const SubSprite *sub = nullptr;
	bool junction = false;

	if (halftile_corner != CORNER_INVALID) {
		track &= ~CornerToTrackBits(halftile_corner);
		if (draw_half_tile != CORNER_INVALID) {
			sub = &(_halftile_sub_sprite[draw_half_tile]);
		}
	} else {
		if (draw_half_tile != CORNER_INVALID) {
			sub = &(_dual_track_halftile_sub_sprite[draw_half_tile]);
		}
	}

	/* Select the sprite to use. */
	if (track == 0 && draw_half_tile != CORNER_INVALID) {
		image = 0;
	} else if (track == 0) {
		/* Clear ground (only track on halftile foundation) */
		if (rgt == RAIL_GROUND_WATER) {
			if (IsSteepSlope(ti->tileh)) {
				DrawShoreTile(ti->tileh);
				image = 0;
			} else {
				image = SPR_FLAT_WATER_TILE;
			}
		} else {
			switch (rgt) {
				case RAIL_GROUND_BARREN:     image = SPR_FLAT_BARE_LAND;  break;
				case RAIL_GROUND_ICE_DESERT: image = SPR_FLAT_SNOW_DESERT_TILE; break;
				default:                     image = SPR_FLAT_GRASS_TILE; break;
			}
			image += SlopeToSpriteOffset(ti->tileh);
		}
	} else {
		if (ti->tileh != SLOPE_FLAT) {
			/* track on non-flat ground */
			image = _track_sloped_sprites[ti->tileh - 1] + rti->base_sprites.track_y;
		} else {
			/* track on flat ground */
			switch (track) {
				/* single track, select combined track + ground sprite*/
				case TRACK_BIT_Y:     image = rti->base_sprites.track_y;     break;
				case TRACK_BIT_X:     image = rti->base_sprites.track_y + 1; break;
				case TRACK_BIT_UPPER: image = rti->base_sprites.track_y + 2; break;
				case TRACK_BIT_LOWER: image = rti->base_sprites.track_y + 3; break;
				case TRACK_BIT_RIGHT: image = rti->base_sprites.track_y + 4; break;
				case TRACK_BIT_LEFT:  image = rti->base_sprites.track_y + 5; break;
				case TRACK_BIT_CROSS: image = rti->base_sprites.track_y + 6; break;

				/* double diagonal track, select combined track + ground sprite*/
				case TRACK_BIT_HORZ:  image = rti->base_sprites.track_ns;     break;
				case TRACK_BIT_VERT:  image = rti->base_sprites.track_ns + 1; break;

				/* junction, select only ground sprite, handle track sprite later */
				default:
					junction = true;
					if ((track & TRACK_BIT_3WAY_NE) == 0) { image = rti->base_sprites.ground;     break; }
					if ((track & TRACK_BIT_3WAY_SW) == 0) { image = rti->base_sprites.ground + 1; break; }
					if ((track & TRACK_BIT_3WAY_NW) == 0) { image = rti->base_sprites.ground + 2; break; }
					if ((track & TRACK_BIT_3WAY_SE) == 0) { image = rti->base_sprites.ground + 3; break; }
					image = rti->base_sprites.ground + 4;
					break;
			}
		}

		switch (rgt) {
			case RAIL_GROUND_BARREN:     pal = PALETTE_TO_BARE_LAND; break;
			case RAIL_GROUND_ICE_DESERT: image += rti->snow_offset;  break;
			case RAIL_GROUND_WATER: {
				/* three-corner-raised slope */
				DrawShoreTile(ti->tileh);
				Corner track_corner = OppositeCorner(GetHighestSlopeCorner(ComplementSlope(ti->tileh)));
				sub = &(_halftile_sub_sprite[track_corner]);
				break;
			}
			default: break;
		}
	}

	if (image != 0) DrawGroundSprite(image, pal, sub);

	/* Draw track pieces individually for junction tiles */
	if (junction) {
		if (track & TRACK_BIT_X)     DrawGroundSprite(rti->base_sprites.single_x, PAL_NONE);
		if (track & TRACK_BIT_Y)     DrawGroundSprite(rti->base_sprites.single_y, PAL_NONE);
		if (track & TRACK_BIT_UPPER) DrawGroundSprite(rti->base_sprites.single_n, PAL_NONE);
		if (track & TRACK_BIT_LOWER) DrawGroundSprite(rti->base_sprites.single_s, PAL_NONE);
		if (track & TRACK_BIT_LEFT)  DrawGroundSprite(rti->base_sprites.single_w, PAL_NONE);
		if (track & TRACK_BIT_RIGHT) DrawGroundSprite(rti->base_sprites.single_e, PAL_NONE);
	}

	/* PBS debugging, draw reserved tracks darker */
	if (_game_mode != GM_MENU && _settings_client.gui.show_track_reservation) {
		/* Get reservation, but mask track on halftile slope */
		TrackBits pbs = (is_bridge ? GetTunnelBridgeReservationTrackBits(ti->tile) : GetRailReservationTrackBits(ti->tile)) & track;
		if (pbs & TRACK_BIT_X) {
			if (ti->tileh == SLOPE_FLAT || ti->tileh == SLOPE_ELEVATED) {
				DrawGroundSprite(rti->base_sprites.single_x, PALETTE_CRASH);
			} else {
				DrawGroundSprite(_track_sloped_sprites[ti->tileh - 1] + rti->base_sprites.single_sloped - 20, PALETTE_CRASH);
			}
		}
		if (pbs & TRACK_BIT_Y) {
			if (ti->tileh == SLOPE_FLAT || ti->tileh == SLOPE_ELEVATED) {
				DrawGroundSprite(rti->base_sprites.single_y, PALETTE_CRASH);
			} else {
				DrawGroundSprite(_track_sloped_sprites[ti->tileh - 1] + rti->base_sprites.single_sloped - 20, PALETTE_CRASH);
			}
		}
		if (pbs & TRACK_BIT_UPPER) DrawGroundSprite(rti->base_sprites.single_n, PALETTE_CRASH, nullptr, 0, ti->tileh & SLOPE_N ? -(int)TILE_HEIGHT : 0);
		if (pbs & TRACK_BIT_LOWER) DrawGroundSprite(rti->base_sprites.single_s, PALETTE_CRASH, nullptr, 0, ti->tileh & SLOPE_S ? -(int)TILE_HEIGHT : 0);
		if (pbs & TRACK_BIT_LEFT)  DrawGroundSprite(rti->base_sprites.single_w, PALETTE_CRASH, nullptr, 0, ti->tileh & SLOPE_W ? -(int)TILE_HEIGHT : 0);
		if (pbs & TRACK_BIT_RIGHT) DrawGroundSprite(rti->base_sprites.single_e, PALETTE_CRASH, nullptr, 0, ti->tileh & SLOPE_E ? -(int)TILE_HEIGHT : 0);
	}

	if (IsValidCorner(halftile_corner) && (draw_half_tile == halftile_corner || draw_half_tile == CORNER_INVALID)) {
		DrawFoundation(ti, HalftileFoundation(halftile_corner));

		/* Draw higher halftile-overlay: Use the sloped sprites with three corners raised. They probably best fit the lightning. */
		Slope fake_slope = SlopeWithThreeCornersRaised(OppositeCorner(halftile_corner));
		image = _track_sloped_sprites[fake_slope - 1] + rti->base_sprites.track_y;
		pal = PAL_NONE;
		switch (rgt) {
			case RAIL_GROUND_BARREN:     pal = PALETTE_TO_BARE_LAND; break;
			case RAIL_GROUND_ICE_DESERT:
			case RAIL_GROUND_HALF_SNOW:  image += rti->snow_offset;  break; // higher part has snow in this case too
			default: break;
		}
		DrawGroundSprite(image, pal, &(_halftile_sub_sprite[halftile_corner]));

		if (_game_mode != GM_MENU && _settings_client.gui.show_track_reservation && HasReservedTracks(ti->tile, CornerToTrackBits(halftile_corner))) {
			static const uint8_t _corner_to_track_sprite[] = {3, 1, 2, 0};
			DrawGroundSprite(_corner_to_track_sprite[halftile_corner] + rti->base_sprites.single_n, PALETTE_CRASH, nullptr, 0, -(int)TILE_HEIGHT);
		}
	}
}

void DrawTrackBits(TileInfo *ti, TrackBits track)
{
	const bool is_bridge = IsTileType(ti->tile, MP_TUNNELBRIDGE);
	RailGroundType rgt = GetRailOrBridgeGroundType(ti);
	Foundation f = is_bridge ? FOUNDATION_LEVELED : GetRailFoundation(ti->tileh, track);
	Corner halftile_corner = CORNER_INVALID;

	if (IsNonContinuousFoundation(f)) {
		/* Save halftile corner */
		halftile_corner = (f == FOUNDATION_STEEP_BOTH ? GetHighestSlopeCorner(ti->tileh) : GetHalftileFoundationCorner(f));
		/* Draw lower part first */
		f = (f == FOUNDATION_STEEP_BOTH ? FOUNDATION_STEEP_LOWER : FOUNDATION_NONE);
	}

	DrawFoundation(ti, f);
	/* DrawFoundation modifies ti */

	RailType rt1 = GetRailType(ti->tile);
	RailType rt2 = GetTileSecondaryRailTypeIfValid(ti->tile);
	if (rt2 == INVALID_RAILTYPE || rt1 == rt2) {
		DrawTrackBits(ti, track, rt1, rgt, is_bridge, halftile_corner, CORNER_INVALID);
	} else {
		const bool is_bridge = IsTileType(ti->tile, MP_TUNNELBRIDGE);
		TrackBits primary_track = track & (is_bridge ? GetAcrossBridgePossibleTrackBits(ti->tile) : TRACK_BIT_RT_1);
		TrackBits secondary_track = track ^ primary_track;
		assert((primary_track & (TRACK_BIT_HORZ | TRACK_BIT_VERT)) == primary_track);
		assert((primary_track & (primary_track - 1)) == 0);
		Track primary = FindFirstTrack(primary_track);

		// TRACK_UPPER 2 -> CORNER_N 3
		// TRACK_LOWER 3 -> CORNER_S 1
		// TRACK_LEFT  4 -> CORNER_W 0
		// TRACK_RIGHT 5 -> CORNER_E 2
		Corner primary_corner = (Corner) ((0x870 >> (primary * 2)) & 3);
		if (halftile_corner == primary_corner) {
			std::swap(primary_track, secondary_track);
			std::swap(rt1, rt2);
			primary_corner = OppositeCorner(primary_corner);
		}
		if (halftile_corner == CORNER_INVALID) {
			// draw ground sprite
			SpriteID image;

			switch (rgt) {
				case RAIL_GROUND_BARREN:     image = SPR_FLAT_BARE_LAND;  break;
				case RAIL_GROUND_ICE_DESERT: image = SPR_FLAT_SNOW_DESERT_TILE; break;
				default:                     image = SPR_FLAT_GRASS_TILE; break;
			}
			image += SlopeToSpriteOffset(ti->tileh);
			DrawGroundSprite(image, PAL_NONE);
		}
		DrawTrackBits(ti, primary_track, rt1, rgt, is_bridge, halftile_corner, primary_corner);
		DrawTrackBits(ti, secondary_track, rt2, rgt, is_bridge, halftile_corner, OppositeCorner(primary_corner));
	}
}

static void DrawSignals(TileIndex tile, TrackBits rails, const RailTypeInfo *rti)
{
	auto MAYBE_DRAW_SIGNAL = [&](uint8_t signalbit, SignalOffsets image, uint pos, Track track) {
		if (IsSignalPresent(tile, signalbit)) DrawSingleSignal(tile, rti, track, GetSingleSignalState(tile, signalbit), image, pos);
	};

	if (!(rails & TRACK_BIT_Y)) {
		if (!(rails & TRACK_BIT_X)) {
			if (rails & TRACK_BIT_LEFT) {
				MAYBE_DRAW_SIGNAL(2, SIGNAL_TO_NORTH, 0, TRACK_LEFT);
				MAYBE_DRAW_SIGNAL(3, SIGNAL_TO_SOUTH, 1, TRACK_LEFT);
			}
			if (rails & TRACK_BIT_RIGHT) {
				MAYBE_DRAW_SIGNAL(0, SIGNAL_TO_NORTH, 2, TRACK_RIGHT);
				MAYBE_DRAW_SIGNAL(1, SIGNAL_TO_SOUTH, 3, TRACK_RIGHT);
			}
			if (rails & TRACK_BIT_UPPER) {
				MAYBE_DRAW_SIGNAL(3, SIGNAL_TO_WEST, 4, TRACK_UPPER);
				MAYBE_DRAW_SIGNAL(2, SIGNAL_TO_EAST, 5, TRACK_UPPER);
			}
			if (rails & TRACK_BIT_LOWER) {
				MAYBE_DRAW_SIGNAL(1, SIGNAL_TO_WEST, 6, TRACK_LOWER);
				MAYBE_DRAW_SIGNAL(0, SIGNAL_TO_EAST, 7, TRACK_LOWER);
			}
		} else {
			MAYBE_DRAW_SIGNAL(3, SIGNAL_TO_SOUTHWEST, 8, TRACK_X);
			MAYBE_DRAW_SIGNAL(2, SIGNAL_TO_NORTHEAST, 9, TRACK_X);
		}
	} else {
		MAYBE_DRAW_SIGNAL(3, SIGNAL_TO_SOUTHEAST, 10, TRACK_Y);
		MAYBE_DRAW_SIGNAL(2, SIGNAL_TO_NORTHWEST, 11, TRACK_Y);
	}
}

static void DrawTile_Track(TileInfo *ti, DrawTileProcParams params)
{
	const RailTypeInfo *rti = GetRailTypeInfo(GetRailType(ti->tile));

	_drawtile_track_palette = COMPANY_SPRITE_COLOUR(GetTileOwner(ti->tile));

	if (IsPlainRail(ti->tile)) {
		if (!IsBridgeAbove(ti->tile) && params.min_visible_height > std::max<int>(SIGNAL_DIRTY_TOP, (TILE_HEIGHT + BB_HEIGHT_UNDER_BRIDGE) * ZOOM_BASE) && !_signal_sprite_oversized) return;

		TrackBits rails = GetTrackBits(ti->tile);

		DrawTrackBits(ti, rails);

		if (HasBit(_display_opt, DO_FULL_DETAIL)) DrawTrackDetails(ti, rti, GetRailGroundType(ti->tile));

		const RailType secondary_railtype = GetTileSecondaryRailTypeIfValid(ti->tile);

		if (HasRailCatenaryDrawn(GetRailType(ti->tile), secondary_railtype)) DrawRailCatenary(ti);

		if (HasSignals(ti->tile)) {
			if (rails == TRACK_BIT_VERT) {
				const RailTypeInfo *rti2 = GetRailTypeInfo(secondary_railtype);
				if (IsSignalPresent(ti->tile, 2)) DrawSingleSignal(ti->tile, rti,   TRACK_LEFT, GetSingleSignalState(ti->tile, 2), SIGNAL_TO_NORTH, 0);
				if (IsSignalPresent(ti->tile, 3)) DrawSingleSignal(ti->tile, rti,   TRACK_LEFT, GetSingleSignalState(ti->tile, 3), SIGNAL_TO_SOUTH, 1);
				if (IsSignalPresent(ti->tile, 0)) DrawSingleSignal(ti->tile, rti2, TRACK_RIGHT, GetSingleSignalState(ti->tile, 0), SIGNAL_TO_NORTH, 2);
				if (IsSignalPresent(ti->tile, 1)) DrawSingleSignal(ti->tile, rti2, TRACK_RIGHT, GetSingleSignalState(ti->tile, 1), SIGNAL_TO_SOUTH, 3);
			} else if (rails == TRACK_BIT_HORZ) {
				const RailTypeInfo *rti2 = GetRailTypeInfo(secondary_railtype);
				if (IsSignalPresent(ti->tile, 3)) DrawSingleSignal(ti->tile, rti,  TRACK_UPPER, GetSingleSignalState(ti->tile, 3), SIGNAL_TO_WEST, 4);
				if (IsSignalPresent(ti->tile, 2)) DrawSingleSignal(ti->tile, rti,  TRACK_UPPER, GetSingleSignalState(ti->tile, 2), SIGNAL_TO_EAST, 5);
				if (IsSignalPresent(ti->tile, 1)) DrawSingleSignal(ti->tile, rti2, TRACK_LOWER, GetSingleSignalState(ti->tile, 1), SIGNAL_TO_WEST, 6);
				if (IsSignalPresent(ti->tile, 0)) DrawSingleSignal(ti->tile, rti2, TRACK_LOWER, GetSingleSignalState(ti->tile, 0), SIGNAL_TO_EAST, 7);
			} else {
				DrawSignals(ti->tile, rails, rti);
			}
		}
	} else {
		/* draw depot */
		const DrawTileSprites *dts;
		PaletteID pal = PAL_NONE;
		SpriteID relocation;

		if (ti->tileh != SLOPE_FLAT) DrawFoundation(ti, FOUNDATION_LEVELED);

		if (IsInvisibilitySet(TO_BUILDINGS)) {
			/* Draw rail instead of depot */
			dts = &_depot_invisible_gfx_table[GetRailDepotDirection(ti->tile)];
		} else {
			dts = &_depot_gfx_table[GetRailDepotDirection(ti->tile)];
		}

		SpriteID image;
		if (rti->UsesOverlay()) {
			image = SPR_FLAT_GRASS_TILE;
		} else {
			image = dts->ground.sprite;
			if (image != SPR_FLAT_GRASS_TILE) image += rti->GetRailtypeSpriteOffset();
		}

		/* Adjust ground tile for desert and snow. */
		if (IsSnowRailGround(ti->tile)) {
			if (image != SPR_FLAT_GRASS_TILE) {
				image += rti->snow_offset; // tile with tracks
			} else {
				image = SPR_FLAT_SNOW_DESERT_TILE; // flat ground
			}
		}

		DrawGroundSprite(image, GroundSpritePaletteTransform(image, pal, _drawtile_track_palette));

		if (rti->UsesOverlay()) {
			SpriteID ground = GetCustomRailSprite(rti, ti->tile, RTSG_GROUND);

			switch (GetRailDepotDirection(ti->tile)) {
				case DIAGDIR_NE:
					if (!IsInvisibilitySet(TO_BUILDINGS)) break;
					[[fallthrough]];
				case DIAGDIR_SW:
					DrawGroundSprite(ground + RTO_X, PAL_NONE);
					break;
				case DIAGDIR_NW:
					if (!IsInvisibilitySet(TO_BUILDINGS)) break;
					[[fallthrough]];
				case DIAGDIR_SE:
					DrawGroundSprite(ground + RTO_Y, PAL_NONE);
					break;
				default:
					break;
			}

			if (_settings_client.gui.show_track_reservation && HasDepotReservation(ti->tile)) {
				SpriteID overlay = GetCustomRailSprite(rti, ti->tile, RTSG_OVERLAY);

				switch (GetRailDepotDirection(ti->tile)) {
					case DIAGDIR_NE:
						if (!IsInvisibilitySet(TO_BUILDINGS)) break;
						[[fallthrough]];
					case DIAGDIR_SW:
						DrawGroundSprite(overlay + RTO_X, PALETTE_CRASH);
						break;
					case DIAGDIR_NW:
						if (!IsInvisibilitySet(TO_BUILDINGS)) break;
						[[fallthrough]];
					case DIAGDIR_SE:
						DrawGroundSprite(overlay + RTO_Y, PALETTE_CRASH);
						break;
					default:
						break;
				}
			}
		} else {
			/* PBS debugging, draw reserved tracks darker */
			if (_game_mode != GM_MENU && _settings_client.gui.show_track_reservation && HasDepotReservation(ti->tile)) {
				switch (GetRailDepotDirection(ti->tile)) {
					case DIAGDIR_NE:
						if (!IsInvisibilitySet(TO_BUILDINGS)) break;
						[[fallthrough]];
					case DIAGDIR_SW:
						DrawGroundSprite(rti->base_sprites.single_x, PALETTE_CRASH);
						break;
					case DIAGDIR_NW:
						if (!IsInvisibilitySet(TO_BUILDINGS)) break;
						[[fallthrough]];
					case DIAGDIR_SE:
						DrawGroundSprite(rti->base_sprites.single_y, PALETTE_CRASH);
						break;
					default:
						break;
				}
			}
		}
		int depot_sprite = GetCustomRailSprite(rti, ti->tile, RTSG_DEPOT);
		relocation = depot_sprite != 0 ? depot_sprite - SPR_RAIL_DEPOT_SE_1 : rti->GetRailtypeSpriteOffset();

		if (HasRailCatenaryDrawn(GetRailType(ti->tile))) DrawRailCatenary(ti);

		DrawRailTileSeq(ti, dts, TO_BUILDINGS, relocation, 0, _drawtile_track_palette);
	}
	DrawBridgeMiddle(ti);
}

void DrawTrainDepotSprite(int x, int y, int dir, RailType railtype)
{
	const DrawTileSprites *dts = &_depot_gfx_table[dir];
	const RailTypeInfo *rti = GetRailTypeInfo(railtype);
	SpriteID image = rti->UsesOverlay() ? SPR_FLAT_GRASS_TILE : dts->ground.sprite;
	uint32_t offset = rti->GetRailtypeSpriteOffset();

	if (image != SPR_FLAT_GRASS_TILE) image += offset;
	PaletteID palette = COMPANY_SPRITE_COLOUR(_local_company);

	DrawSprite(image, PAL_NONE, x, y);

	if (rti->UsesOverlay()) {
		SpriteID ground = GetCustomRailSprite(rti, INVALID_TILE, RTSG_GROUND);

		switch (dir) {
			case DIAGDIR_SW: DrawSprite(ground + RTO_X, PAL_NONE, x, y); break;
			case DIAGDIR_SE: DrawSprite(ground + RTO_Y, PAL_NONE, x, y); break;
			default: break;
		}
	}
	int depot_sprite = GetCustomRailSprite(rti, INVALID_TILE, RTSG_DEPOT);
	if (depot_sprite != 0) offset = depot_sprite - SPR_RAIL_DEPOT_SE_1;

	DrawRailTileSeqInGUI(x, y, dts, offset, 0, palette);
}

static int GetSlopePixelZ_Track(TileIndex tile, uint x, uint y, bool)
{
	if (IsPlainRail(tile)) {
		auto [tileh, z] = GetTilePixelSlope(tile);
		if (tileh == SLOPE_FLAT) return z;

		z += ApplyPixelFoundationToSlope(GetRailFoundation(tileh, GetTrackBits(tile)), tileh);
		return z + GetPartialPixelZ(x & 0xF, y & 0xF, tileh);
	} else {
		return GetTileMaxPixelZ(tile);
	}
}

static Foundation GetFoundation_Track(TileIndex tile, Slope tileh)
{
	return IsPlainRail(tile) ? GetRailFoundation(tileh, GetTrackBits(tile)) : FlatteningFoundation(tileh);
}

RailGroundType RailTrackToFence(TileIndex tile, TrackBits rail)
{
	Owner owner = GetTileOwner(tile);
	uint8_t fences = 0;

	for (DiagDirection d = DIAGDIR_BEGIN; d < DIAGDIR_END; d++) {
		static const TrackBits dir_to_trackbits[DIAGDIR_END] = {TRACK_BIT_3WAY_NE, TRACK_BIT_3WAY_SE, TRACK_BIT_3WAY_SW, TRACK_BIT_3WAY_NW};

		/* Track bit on this edge => no fence. */
		if ((rail & dir_to_trackbits[d]) != TRACK_BIT_NONE) continue;

		TileIndex tile2 = tile + TileOffsByDiagDir(d);

		/* Show fences if it's a house, industry, object, road, tunnelbridge or not owned by us. */
		if (!IsValidTile(tile2) || IsTileType(tile2, MP_HOUSE) || IsTileType(tile2, MP_INDUSTRY) ||
				IsTileType(tile2, MP_ROAD) || (IsTileType(tile2, MP_OBJECT) && !IsObjectType(tile2, OBJECT_OWNED_LAND)) || IsTileType(tile2, MP_TUNNELBRIDGE) || !IsTileOwner(tile2, owner)) {
			fences |= 1 << d;
		}
	}

	RailGroundType new_ground;
	switch (fences) {
		case 0: new_ground = RAIL_GROUND_GRASS; break;
		case (1 << DIAGDIR_NE): new_ground = RAIL_GROUND_FENCE_NE; break;
		case (1 << DIAGDIR_SE): new_ground = RAIL_GROUND_FENCE_SE; break;
		case (1 << DIAGDIR_SW): new_ground = RAIL_GROUND_FENCE_SW; break;
		case (1 << DIAGDIR_NW): new_ground = RAIL_GROUND_FENCE_NW; break;
		case (1 << DIAGDIR_NE) | (1 << DIAGDIR_SW): new_ground = RAIL_GROUND_FENCE_NESW; break;
		case (1 << DIAGDIR_SE) | (1 << DIAGDIR_NW): new_ground = RAIL_GROUND_FENCE_SENW; break;
		case (1 << DIAGDIR_NE) | (1 << DIAGDIR_SE): new_ground = RAIL_GROUND_FENCE_VERT1; break;
		case (1 << DIAGDIR_NE) | (1 << DIAGDIR_NW): new_ground = RAIL_GROUND_FENCE_HORIZ2; break;
		case (1 << DIAGDIR_SE) | (1 << DIAGDIR_SW): new_ground = RAIL_GROUND_FENCE_HORIZ1; break;
		case (1 << DIAGDIR_SW) | (1 << DIAGDIR_NW): new_ground = RAIL_GROUND_FENCE_VERT2; break;
		default: NOT_REACHED();
	}
	return new_ground;
}

static void TileLoop_Track(TileIndex tile)
{
	RailGroundType old_ground = GetRailGroundType(tile);
	RailGroundType new_ground;

	if (old_ground == RAIL_GROUND_WATER) {
		TileLoop_Water(tile);
		return;
	}

	switch (_settings_game.game_creation.landscape) {
		case LandscapeType::Arctic: {
			auto [slope, z] = GetTileSlopeZ(tile);
			bool half = false;

			/* for non-flat track, use lower part of track
			 * in other cases, use the highest part with track */
			if (IsPlainRail(tile)) {
				TrackBits track = GetTrackBits(tile);
				Foundation f = GetRailFoundation(slope, track);

				switch (f) {
					case FOUNDATION_NONE:
						/* no foundation - is the track on the upper side of three corners raised tile? */
						if (IsSlopeWithThreeCornersRaised(slope)) z++;
						break;

					case FOUNDATION_INCLINED_X:
					case FOUNDATION_INCLINED_Y:
						/* sloped track - is it on a steep slope? */
						if (IsSteepSlope(slope)) z++;
						break;

					case FOUNDATION_STEEP_LOWER:
						/* only lower part of steep slope */
						z++;
						break;

					default:
						/* if it is a steep slope, then there is a track on higher part */
						if (IsSteepSlope(slope)) z++;
						z++;
						break;
				}

				half = IsInsideMM(f, FOUNDATION_STEEP_BOTH, FOUNDATION_HALFTILE_N + 1);
			} else {
				/* is the depot on a non-flat tile? */
				if (slope != SLOPE_FLAT) z++;
			}

			/* 'z' is now the lowest part of the highest track bit -
			 * for sloped track, it is 'z' of lower part
			 * for two track bits, it is 'z' of higher track bit
			 * For non-continuous foundations (and STEEP_BOTH), 'half' is set */
			if (z > GetSnowLine()) {
				if (half && z - GetSnowLine() == 1) {
					/* track on non-continuous foundation, lower part is not under snow */
					new_ground = RAIL_GROUND_HALF_SNOW;
				} else {
					new_ground = RAIL_GROUND_ICE_DESERT;
				}
				goto set_ground;
			}
			break;
			}

		case LandscapeType::Tropic:
			if (GetTropicZone(tile) == TROPICZONE_DESERT) {
				new_ground = RAIL_GROUND_ICE_DESERT;
				goto set_ground;
			}
			break;

		default:
			break;
	}

	new_ground = RAIL_GROUND_GRASS;

	if (IsPlainRail(tile) && old_ground != RAIL_GROUND_BARREN) { // wait until bottom is green
		/* determine direction of fence */
		TrackBits rail = GetTrackBits(tile);
		new_ground = RailTrackToFence(tile, rail);
	}

set_ground:
	if (old_ground != new_ground) {
		SetRailGroundType(tile, new_ground);
		MarkTileDirtyByTile(tile, VMDF_NOT_MAP_MODE);
	}
}


static TrackStatus GetTileTrackStatus_Track(TileIndex tile, TransportType mode, uint sub_mode, DiagDirection side)
{
	/* Case of half tile slope with water. */
	if (mode == TRANSPORT_WATER && IsPlainRail(tile) && GetRailGroundType(tile) == RAIL_GROUND_WATER && IsSlopeWithOneCornerRaised(GetTileSlope(tile))) {
		TrackBits tb = GetTrackBits(tile);
		switch (tb) {
			default: NOT_REACHED();
			case TRACK_BIT_UPPER: tb = TRACK_BIT_LOWER; break;
			case TRACK_BIT_LOWER: tb = TRACK_BIT_UPPER; break;
			case TRACK_BIT_LEFT:  tb = TRACK_BIT_RIGHT; break;
			case TRACK_BIT_RIGHT: tb = TRACK_BIT_LEFT;  break;
		}
		return CombineTrackStatus(TrackBitsToTrackdirBits(tb), TRACKDIR_BIT_NONE);
	}

	if (mode != TRANSPORT_RAIL) return 0;

	TrackBits trackbits = TRACK_BIT_NONE;
	TrackdirBits red_signals = TRACKDIR_BIT_NONE;

	switch (GetRailTileType(tile)) {
		default: NOT_REACHED();
		case RAIL_TILE_NORMAL:
			trackbits = GetTrackBits(tile);
			break;

		case RAIL_TILE_SIGNALS: {
			trackbits = GetTrackBits(tile);
			if (sub_mode & TTSSM_NO_RED_SIGNALS) break;
			uint8_t a = GetPresentSignals(tile);
			uint b = GetSignalStates(tile);

			b &= a;

			/* When signals are not present (in neither direction),
			 * we pretend them to be green. Otherwise, it depends on
			 * the signal type. For signals that are only active from
			 * one side, we set the missing signals explicitly to
			 * `green'. Otherwise, they implicitly become `red'. */
			if (!IsOnewaySignal(tile, TRACK_UPPER) || (a & SignalOnTrack(TRACK_UPPER)) == 0) b |= ~a & SignalOnTrack(TRACK_UPPER);
			if (!IsOnewaySignal(tile, TRACK_LOWER) || (a & SignalOnTrack(TRACK_LOWER)) == 0) b |= ~a & SignalOnTrack(TRACK_LOWER);

			if ((b & 0x8) == 0) red_signals |= (TRACKDIR_BIT_LEFT_N | TRACKDIR_BIT_X_NE | TRACKDIR_BIT_Y_SE | TRACKDIR_BIT_UPPER_E);
			if ((b & 0x4) == 0) red_signals |= (TRACKDIR_BIT_LEFT_S | TRACKDIR_BIT_X_SW | TRACKDIR_BIT_Y_NW | TRACKDIR_BIT_UPPER_W);
			if ((b & 0x2) == 0) red_signals |= (TRACKDIR_BIT_RIGHT_N | TRACKDIR_BIT_LOWER_E);
			if ((b & 0x1) == 0) red_signals |= (TRACKDIR_BIT_RIGHT_S | TRACKDIR_BIT_LOWER_W);

			break;
		}

		case RAIL_TILE_DEPOT: {
			DiagDirection dir = GetRailDepotDirection(tile);

			if (side != INVALID_DIAGDIR && side != dir) break;

			trackbits = DiagDirToDiagTrackBits(dir);
			break;
		}
	}

	return CombineTrackStatus(TrackBitsToTrackdirBits(trackbits), red_signals);
}

static bool ClickTile_Track(TileIndex tile)
{
	if (_ctrl_pressed && IsPlainRailTile(tile)) {
		TrackBits trackbits = TrackdirBitsToTrackBits(GetTileTrackdirBits(tile, TRANSPORT_RAIL, 0));

		if (trackbits & TRACK_BIT_VERT) { // N-S direction
			trackbits = (_tile_fract_coords.x <= _tile_fract_coords.y) ? TRACK_BIT_RIGHT : TRACK_BIT_LEFT;
		}

		if (trackbits & TRACK_BIT_HORZ) { // E-W direction
			trackbits = (_tile_fract_coords.x + _tile_fract_coords.y <= 15) ? TRACK_BIT_UPPER : TRACK_BIT_LOWER;
		}

		Track track = FindFirstTrack(trackbits);
		if (HasTrack(tile, track) && HasSignalOnTrack(tile, track)) {
			const bool programmable = IsPresignalProgrammable(tile, track);
			if (GetExistingTraceRestrictProgram(tile, track) != nullptr || !programmable) {
				ShowTraceRestrictProgramWindow(tile, track);
			}
			if (programmable) {
				ShowSignalProgramWindow(SignalReference(tile, track));
			}
			return true;
		}
	}

	if (!IsRailDepot(tile)) return false;

	ShowDepotWindow(tile, VEH_TRAIN);
	return true;
}

static void GetTileDesc_Track(TileIndex tile, TileDesc *td)
{
	RailType rt = GetRailType(tile);
	const RailTypeInfo *rti = GetRailTypeInfo(rt);
	td->rail_speed = rti->max_speed;
	td->railtype = rti->strings.name;
	RailType secondary_rt = GetTileSecondaryRailTypeIfValid(tile);
	if (secondary_rt != rt && secondary_rt != INVALID_RAILTYPE) {
		const RailTypeInfo *secondary_rti = GetRailTypeInfo(secondary_rt);
		td->rail_speed2 = secondary_rti->max_speed;
		td->railtype2 = secondary_rti->strings.name;
	}
	td->owner[0] = GetTileOwner(tile);
	switch (GetRailTileType(tile)) {
		case RAIL_TILE_NORMAL:
			td->str = STR_LAI_RAIL_DESCRIPTION_TRACK;
			break;

		case RAIL_TILE_SIGNALS: {
			static const StringID signal_type[8][8] = {
				{
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_SIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_PRESIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_EXITSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_COMBOSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_PBSSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_NOENTRYSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_PROGSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_NOENTRY2SIGNALS
				},
				{
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_PRESIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRESIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRE_EXITSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRE_COMBOSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRE_PBSSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRE_NOENTRYSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRE_PROGSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRE_NOENTRY2SIGNALS
				},
				{
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_EXITSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRE_EXITSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_EXITSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_EXIT_COMBOSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_EXIT_PBSSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_EXIT_NOENTRYSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_EXIT_PROGSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_EXIT_NOENTRY2SIGNALS
				},
				{
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_COMBOSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRE_COMBOSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_EXIT_COMBOSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_COMBOSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_COMBO_PBSSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_COMBO_NOENTRYSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_COMBO_PROGSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_COMBO_NOENTRY2SIGNALS
				},
				{
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_PBSSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRE_PBSSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_EXIT_PBSSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_COMBO_PBSSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PBSSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PBS_NOENTRYSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PBS_PROGSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PBS_NOENTRY2SIGNALS
				},
				{
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_NOENTRYSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRE_NOENTRYSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_EXIT_NOENTRYSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_COMBO_NOENTRYSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PBS_NOENTRYSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NOENTRYSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NOENTRY_PROGSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NOENTRY_NOENTRY2SIGNALS
				},
				{
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_PROGSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRE_PROGSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_EXIT_PROGSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_COMBO_PROGSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PBS_PROGSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NOENTRY_PROGSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PROGSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PROG_NOENTRY2SIGNALS
				},
				{
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_NOENTRY2SIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRE_NOENTRY2SIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_EXIT_NOENTRY2SIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_COMBO_NOENTRY2SIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PBS_NOENTRY2SIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NOENTRY_NOENTRY2SIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PROG_NOENTRY2SIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NOENTRY2SIGNALS
				}
			};

			SignalType primary_signal;
			SignalType secondary_signal;
			int primary_style = -1;
			int secondary_style = -1;
			if (HasSignalOnTrack(tile, TRACK_UPPER)) {
				primary_signal = GetSignalType(tile, TRACK_UPPER);
				primary_style = GetSignalStyle(tile, TRACK_UPPER);
				if (HasSignalOnTrack(tile, TRACK_LOWER)) {
					secondary_signal = GetSignalType(tile, TRACK_LOWER);
					secondary_style = GetSignalStyle(tile, TRACK_LOWER);
				} else {
					secondary_signal = primary_signal;
				}
			} else {
				secondary_signal = primary_signal = GetSignalType(tile, TRACK_LOWER);
				primary_style = GetSignalStyle(tile, TRACK_LOWER);
			}

			td->str = signal_type[secondary_signal][primary_signal];

			if (primary_style > 0 || secondary_style > 0) {
				/* Add suffix about signal style */
				td->dparam[0] = td->str;
				td->dparam[1] = primary_style == 0 ? STR_BUILD_SIGNAL_DEFAULT_STYLE : _new_signal_styles[primary_style - 1].name;
				if (secondary_style >= 0) {
					td->dparam[2] = secondary_style == 0 ? STR_BUILD_SIGNAL_DEFAULT_STYLE : _new_signal_styles[secondary_style - 1].name;
					td->str = STR_LAI_RAIL_DESCRIPTION_TRACK_SIGNAL_STYLE2;
				} else {
					td->str = STR_LAI_RAIL_DESCRIPTION_TRACK_SIGNAL_STYLE;
				}
			}

			if (IsRestrictedSignal(tile)) {
				td->dparam[3] = td->dparam[2];
				td->dparam[2] = td->dparam[1];
				td->dparam[1] = td->dparam[0];
				td->dparam[0] = td->str;
				td->str = STR_LAI_RAIL_DESCRIPTION_RESTRICTED_SIGNAL;
			}
			break;
		}

		case RAIL_TILE_DEPOT:
			td->str = STR_LAI_RAIL_DESCRIPTION_TRAIN_DEPOT;
			if (_settings_game.vehicle.train_acceleration_model != AM_ORIGINAL) {
				if (td->rail_speed > 0) {
					td->rail_speed = std::min<uint16_t>(td->rail_speed, _settings_game.vehicle.rail_depot_speed_limit);
				} else {
					td->rail_speed = _settings_game.vehicle.rail_depot_speed_limit;
				}
			}
			td->build_date = Depot::GetByTile(tile)->build_date;
			break;

		default:
			NOT_REACHED();
	}
}

static void ChangeTileOwner_Track(TileIndex tile, Owner old_owner, Owner new_owner)
{
	if (!IsTileOwner(tile, old_owner)) return;

	if (new_owner != INVALID_OWNER) {
		/* Update company infrastructure counts. No need to dirty windows here, we'll redraw the whole screen anyway. */
		uint num_pieces = 1;
		if (IsPlainRail(tile)) {
			TrackBits bits = GetTrackBits(tile);
			if (bits == TRACK_BIT_HORZ || bits == TRACK_BIT_VERT) {
				RailType secondary_rt = GetSecondaryRailType(tile);
				Company::Get(old_owner)->infrastructure.rail[secondary_rt]--;
				Company::Get(new_owner)->infrastructure.rail[secondary_rt]++;
			} else {
				num_pieces = CountBits(bits);
				if (TracksOverlap(bits)) num_pieces *= num_pieces;
			}
		}
		RailType rt = GetRailType(tile);
		Company::Get(old_owner)->infrastructure.rail[rt] -= num_pieces;
		Company::Get(new_owner)->infrastructure.rail[rt] += num_pieces;

		if (HasSignals(tile)) {
			uint num_sigs = CountBits(GetPresentSignals(tile));
			Company::Get(old_owner)->infrastructure.signal -= num_sigs;
			Company::Get(new_owner)->infrastructure.signal += num_sigs;
		}

		SetTileOwner(tile, new_owner);
	} else {
		Command<CMD_LANDSCAPE_CLEAR>::Do(DC_EXEC | DC_BANKRUPT, tile);
	}
}

static const uint8_t _fractcoords_behind[4] = { 0x8F, 0x8, 0x80, 0xF8 };
static const uint8_t _fractcoords_enter[4] = { 0x8A, 0x48, 0x84, 0xA8 };
static const int8_t _deltacoord_leaveoffset[8] = {
	-1,  0,  1,  0, /* x */
	 0,  1,  0, -1  /* y */
};


/**
 * Compute number of ticks when next wagon will leave a depot.
 * Negative means next wagon should have left depot n ticks before.
 * @param v vehicle outside (leaving) the depot
 * @return number of ticks when the next wagon will leave
 */
int TicksToLeaveDepot(const Train *v)
{
	DiagDirection dir = GetRailDepotDirection(v->tile);
	int length = v->CalcNextVehicleOffset();

	switch (dir) {
		case DIAGDIR_NE: return  ((int)(v->x_pos & 0x0F) - ((_fractcoords_enter[dir] & 0x0F) - (length + 1)));
		case DIAGDIR_SE: return -((int)(v->y_pos & 0x0F) - ((_fractcoords_enter[dir] >> 4)   + (length + 1)));
		case DIAGDIR_SW: return -((int)(v->x_pos & 0x0F) - ((_fractcoords_enter[dir] & 0x0F) + (length + 1)));
		case DIAGDIR_NW: return  ((int)(v->y_pos & 0x0F) - ((_fractcoords_enter[dir] >> 4)   - (length + 1)));
		default: NOT_REACHED();
	}
}

/**
 * Tile callback routine when vehicle enters tile
 * @see vehicle_enter_tile_proc
 */
static VehicleEnterTileStatus VehicleEnter_Track(Vehicle *u, TileIndex tile, int x, int y)
{
	/* This routine applies only to trains in depot tiles. */
	if (u->type != VEH_TRAIN || !IsRailDepotTile(tile)) return VETSB_CONTINUE;

	Train *v = Train::From(u);

	auto abort_load_through = [&](bool leave_station) {
		if (_local_company == v->owner) {
			SetDParam(0, v->index);
			SetDParam(1, v->current_order.GetDestination().ToStationID());
			AddNewsItem(STR_VEHICLE_LOAD_THROUGH_ABORTED_DEPOT, NewsType::Advice, NewsStyle::Small, {NewsFlag::InColour, NewsFlag::VehicleParam0},
					NewsReferenceType::Vehicle, v->index,
					NewsReferenceType::Station, v->current_order.GetDestination().ToStationID());
		}
		if (leave_station) {
			v->LeaveStation();
			/* Only advance to next order if we are loading at the current one */
			const Order *order = v->GetOrder(v->cur_implicit_order_index);
			if (order != nullptr && order->IsType(OT_GOTO_STATION) && order->GetDestination() == v->last_station_visited) {
				v->IncrementImplicitOrderIndex();
			}
		} else {
			for (Train *u = v; u != nullptr; u = u->Next()) {
				ClrBit(u->flags, VRF_BEYOND_PLATFORM_END);
			}
		}
	};

	if (v->IsFrontEngine() && v->current_order.IsType(OT_LOADING_ADVANCE)) abort_load_through(true);

	/* Depot direction. */
	DiagDirection dir = GetRailDepotDirection(tile);

	/* Calculate the point where the following wagon should be activated. */
	int length = v->CalcNextVehicleOffset();

	uint8_t fract_coord_leave =
		((_fractcoords_enter[dir] & 0x0F) + // x
			(length + 1) * _deltacoord_leaveoffset[dir]) +
		(((_fractcoords_enter[dir] >> 4) +  // y
			((length + 1) * _deltacoord_leaveoffset[dir + 4])) << 4);

	uint8_t fract_coord = (x & 0xF) + ((y & 0xF) << 4);

	if (_fractcoords_behind[dir] == fract_coord) {
		/* make sure a train is not entering the tile from behind */
		return VETSB_CANNOT_ENTER;
	} else if (_fractcoords_enter[dir] == fract_coord) {
		if (DiagDirToDir(ReverseDiagDir(dir)) == v->direction) {
			/* enter the depot */

			if (v->IsFrontEngine()) {
				if (v->current_order.IsType(OT_LOADING_ADVANCE)) {
					abort_load_through(true);
				} else if (HasBit(v->flags, VRF_BEYOND_PLATFORM_END)) {
					abort_load_through(false);
				}
				SetBit(v->flags, VRF_CONSIST_SPEED_REDUCTION);
			}

			v->track = TRACK_BIT_DEPOT,
			v->vehstatus |= VS_HIDDEN; // hide it
			v->UpdateIsDrawn();
			v->direction = ReverseDir(v->direction);
			if (v->Next() == nullptr) VehicleEnterDepot(v->First());
			v->tile = tile;

			InvalidateWindowData(WC_VEHICLE_DEPOT, v->tile.base());
			return VETSB_ENTERED_WORMHOLE;
		}
	} else if (fract_coord_leave == fract_coord) {
		if (DiagDirToDir(dir) == v->direction) {
			/* leave the depot? */
			if ((v = v->Next()) != nullptr) {
				v->vehstatus &= ~VS_HIDDEN;
				v->track = (DiagDirToAxis(dir) == AXIS_X ? TRACK_BIT_X : TRACK_BIT_Y);
				v->UpdateIsDrawn();
			}
		}
	}

	return VETSB_CONTINUE;
}

/**
 * Tests if autoslope is allowed.
 *
 * @param tile The tile.
 * @param flags Terraform command flags.
 * @param z_old Old TileZ.
 * @param tileh_old Old TileSlope.
 * @param z_new New TileZ.
 * @param tileh_new New TileSlope.
 * @param rail_bits Trackbits.
 */
static CommandCost TestAutoslopeOnRailTile(TileIndex tile, uint flags, int z_old, Slope tileh_old, int z_new, Slope tileh_new, TrackBits rail_bits)
{
	if (!_settings_game.construction.build_on_slopes || !AutoslopeEnabled()) return CommandCost(STR_ERROR_MUST_REMOVE_RAILROAD_TRACK);

	/* Is the slope-rail_bits combination valid in general? I.e. is it safe to call GetRailFoundation() ? */
	if (CheckRailSlope(tileh_new, rail_bits, TRACK_BIT_NONE, tile).Failed()) return CommandCost(STR_ERROR_MUST_REMOVE_RAILROAD_TRACK);

	/* Get the slopes on top of the foundations */
	z_old += ApplyFoundationToSlope(GetRailFoundation(tileh_old, rail_bits), tileh_old);
	z_new += ApplyFoundationToSlope(GetRailFoundation(tileh_new, rail_bits), tileh_new);

	Corner track_corner;
	switch (rail_bits) {
		case TRACK_BIT_LEFT:  track_corner = CORNER_W; break;
		case TRACK_BIT_LOWER: track_corner = CORNER_S; break;
		case TRACK_BIT_RIGHT: track_corner = CORNER_E; break;
		case TRACK_BIT_UPPER: track_corner = CORNER_N; break;

		/* Surface slope must not be changed */
		default:
			if (z_old != z_new || tileh_old != tileh_new) return CommandCost(STR_ERROR_MUST_REMOVE_RAILROAD_TRACK);
			return CommandCost(EXPENSES_CONSTRUCTION, _price[PR_BUILD_FOUNDATION]);
	}

	/* The height of the track_corner must not be changed. The rest ensures GetRailFoundation() already. */
	z_old += GetSlopeZInCorner(RemoveHalftileSlope(tileh_old), track_corner);
	z_new += GetSlopeZInCorner(RemoveHalftileSlope(tileh_new), track_corner);
	if (z_old != z_new) return CommandCost(STR_ERROR_MUST_REMOVE_RAILROAD_TRACK);

	CommandCost cost = CommandCost(EXPENSES_CONSTRUCTION, _price[PR_BUILD_FOUNDATION]);
	/* Make the ground dirty, if surface slope has changed */
	if (tileh_old != tileh_new) {
		/* If there is flat water on the lower halftile add the cost for clearing it */
		if (GetRailGroundType(tile) == RAIL_GROUND_WATER && IsSlopeWithOneCornerRaised(tileh_old)) {
			if (_game_mode != GM_EDITOR && !_settings_game.construction.enable_remove_water && !(flags & DC_ALLOW_REMOVE_WATER)) return CommandCost(STR_ERROR_CAN_T_BUILD_ON_WATER);
			cost.AddCost(_price[PR_CLEAR_WATER]);
		}
		if ((flags & DC_EXEC) != 0) SetRailGroundType(tile, RAIL_GROUND_BARREN);
	}
	return  cost;
}

/**
 * Test-procedure for HasVehicleOnPos to check for a ship.
 */
static Vehicle *EnsureNoShipProc(Vehicle *v, void *)
{
	return v;
}

static CommandCost TerraformTile_Track(TileIndex tile, DoCommandFlag flags, int z_new, Slope tileh_new)
{
	auto [tileh_old, z_old] = GetTileSlopeZ(tile);
	if (IsPlainRail(tile)) {
		TrackBits rail_bits = GetTrackBits(tile);
		/* Is there flat water on the lower halftile that must be cleared expensively? */
		bool was_water = (GetRailGroundType(tile) == RAIL_GROUND_WATER && IsSlopeWithOneCornerRaised(tileh_old));

		/* Allow clearing the water only if there is no ship */
		if (was_water && HasVehicleOnPos(tile, VEH_SHIP, nullptr, &EnsureNoShipProc)) return CommandCost(STR_ERROR_SHIP_IN_THE_WAY);

		if (was_water && _game_mode != GM_EDITOR && !_settings_game.construction.enable_remove_water && !(flags & DC_ALLOW_REMOVE_WATER)) return CommandCost(STR_ERROR_CAN_T_BUILD_ON_WATER);

		/* First test autoslope. However if it succeeds we still have to test the rest, because non-autoslope terraforming is cheaper. */
		CommandCost autoslope_result = TestAutoslopeOnRailTile(tile, flags, z_old, tileh_old, z_new, tileh_new, rail_bits);

		/* When there is only a single horizontal/vertical track, one corner can be terraformed. */
		Corner allowed_corner;
		switch (rail_bits) {
			case TRACK_BIT_RIGHT: allowed_corner = CORNER_W; break;
			case TRACK_BIT_UPPER: allowed_corner = CORNER_S; break;
			case TRACK_BIT_LEFT:  allowed_corner = CORNER_E; break;
			case TRACK_BIT_LOWER: allowed_corner = CORNER_N; break;
			default: return autoslope_result;
		}

		Foundation f_old = GetRailFoundation(tileh_old, rail_bits);

		/* Do not allow terraforming if allowed_corner is part of anti-zig-zag foundations */
		if (tileh_old != SLOPE_NS && tileh_old != SLOPE_EW && IsSpecialRailFoundation(f_old)) return autoslope_result;

		/* Everything is valid, which only changes allowed_corner */
		for (Corner corner = (Corner)0; corner < CORNER_END; corner = (Corner)(corner + 1)) {
			if (allowed_corner == corner) continue;
			if (z_old + GetSlopeZInCorner(tileh_old, corner) != z_new + GetSlopeZInCorner(tileh_new, corner)) return autoslope_result;
		}

		/* Make the ground dirty */
		if ((flags & DC_EXEC) != 0) SetRailGroundType(tile, RAIL_GROUND_BARREN);

		/* allow terraforming */
		return CommandCost(EXPENSES_CONSTRUCTION, was_water ? _price[PR_CLEAR_WATER] : (Money)0);
	} else if (_settings_game.construction.build_on_slopes && AutoslopeEnabled() &&
			AutoslopeCheckForEntranceEdge(tile, z_new, tileh_new, GetRailDepotDirection(tile))) {
		return CommandCost(EXPENSES_CONSTRUCTION, _price[PR_BUILD_FOUNDATION]);
	}
	return Command<CMD_LANDSCAPE_CLEAR>::Do(flags, tile);
}


extern const TileTypeProcs _tile_type_rail_procs = {
	DrawTile_Track,           // draw_tile_proc
	GetSlopePixelZ_Track,     // get_slope_z_proc
	ClearTile_Track,          // clear_tile_proc
	nullptr,                     // add_accepted_cargo_proc
	GetTileDesc_Track,        // get_tile_desc_proc
	GetTileTrackStatus_Track, // get_tile_track_status_proc
	ClickTile_Track,          // click_tile_proc
	nullptr,                     // animate_tile_proc
	TileLoop_Track,           // tile_loop_proc
	ChangeTileOwner_Track,    // change_tile_owner_proc
	nullptr,                     // add_produced_cargo_proc
	VehicleEnter_Track,       // vehicle_enter_tile_proc
	GetFoundation_Track,      // get_foundation_proc
	TerraformTile_Track,      // terraform_tile_proc
};
