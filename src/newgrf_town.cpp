/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file newgrf_town.cpp Implementation of the town part of NewGRF houses. */

#include "stdafx.h"
#include "debug.h"
#include "town.h"
#include "newgrf_town.h"
#include "newgrf_extension.h"

#include "safeguards.h"

/* virtual */ uint32_t TownScopeResolver::GetVariable(uint16_t variable, uint32_t parameter, GetVariableExtra &extra) const
{
	if (this->t == nullptr) {
		extra.available = false;
		return UINT_MAX;
	}

	CargoType cargo_type;
	switch (variable) {
		/* Larger towns */
		case 0x40:
			if (_settings_game.economy.larger_towns == 0) return 2;
			if (this->t->larger_town) return 1;
			return 0;

		/* Town index */
		case 0x41: return this->t->index;

		/* Get a variable from the persistent storage */
		case 0x7C: {
			/* Check the persistent storage for the GrfID stored in register 100h. */
			uint32_t grfid = GetRegister(0x100);
			if (grfid == 0xFFFFFFFF) {
				if (this->ro.grffile == nullptr) return 0;
				grfid = this->ro.grffile->grfid;
			}

			for (auto &it : this->t->psa_list) {
				if (it->grfid == grfid) return it->GetValue(parameter);
			}

			return 0;
		}

		/* Town properties */
		case 0x80: return this->t->xy.base();
		case 0x81: return GB(this->t->xy.base(), 8, 8);
		case 0x82: return ClampTo<uint16_t>(this->t->cache.population);
		case 0x83: return GB(ClampTo<uint16_t>(this->t->cache.population), 8, 8);
		case 0x8A: return this->t->grow_counter / TOWN_GROWTH_TICKS;
		case 0x92: return this->t->flags;  // In original game, 0x92 and 0x93 are really one word. Since flags is a byte, this is to adjust
		case 0x93: return 0;
		case 0x94: return ClampTo<uint16_t>(this->t->cache.squared_town_zone_radius[HZB_TOWN_EDGE]);
		case 0x95: return GB(ClampTo<uint16_t>(this->t->cache.squared_town_zone_radius[HZB_TOWN_EDGE]), 8, 8);
		case 0x96: return ClampTo<uint16_t>(this->t->cache.squared_town_zone_radius[HZB_TOWN_OUTSKIRT]);
		case 0x97: return GB(ClampTo<uint16_t>(this->t->cache.squared_town_zone_radius[HZB_TOWN_OUTSKIRT]), 8, 8);
		case 0x98: return ClampTo<uint16_t>(this->t->cache.squared_town_zone_radius[HZB_TOWN_OUTER_SUBURB]);
		case 0x99: return GB(ClampTo<uint16_t>(this->t->cache.squared_town_zone_radius[HZB_TOWN_OUTER_SUBURB]), 8, 8);
		case 0x9A: return ClampTo<uint16_t>(this->t->cache.squared_town_zone_radius[HZB_TOWN_INNER_SUBURB]);
		case 0x9B: return GB(ClampTo<uint16_t>(this->t->cache.squared_town_zone_radius[HZB_TOWN_INNER_SUBURB]), 8, 8);
		case 0x9C: return ClampTo<uint16_t>(this->t->cache.squared_town_zone_radius[HZB_TOWN_CENTRE]);
		case 0x9D: return GB(ClampTo<uint16_t>(this->t->cache.squared_town_zone_radius[HZB_TOWN_CENTRE]), 8, 8);
		case 0x9E: return this->t->ratings[0];
		case 0x9F: return GB(this->t->ratings[0], 8, 8);
		case 0xA0: return this->t->ratings[1];
		case 0xA1: return GB(this->t->ratings[1], 8, 8);
		case 0xA2: return this->t->ratings[2];
		case 0xA3: return GB(this->t->ratings[2], 8, 8);
		case 0xA4: return this->t->ratings[3];
		case 0xA5: return GB(this->t->ratings[3], 8, 8);
		case 0xA6: return this->t->ratings[4];
		case 0xA7: return GB(this->t->ratings[4], 8, 8);
		case 0xA8: return this->t->ratings[5];
		case 0xA9: return GB(this->t->ratings[5], 8, 8);
		case 0xAA: return this->t->ratings[6];
		case 0xAB: return GB(this->t->ratings[6], 8, 8);
		case 0xAC: return this->t->ratings[7];
		case 0xAD: return GB(this->t->ratings[7], 8, 8);
		case 0xAE: return (uint32_t)this->t->have_ratings.base();
		case 0xB2: return (uint32_t)this->t->statues.base();
		case 0xB6: return ClampTo<uint16_t>(this->t->cache.num_houses);
		case 0xB9: return this->t->growth_rate / TOWN_GROWTH_TICKS;
		case 0xBA: cargo_type = GetCargoTypeByLabel(CT_PASSENGERS); return IsValidCargoType(cargo_type) ? ClampTo<uint16_t>(this->t->supplied[cargo_type].new_max) : 0;
		case 0xBB: cargo_type = GetCargoTypeByLabel(CT_PASSENGERS); return IsValidCargoType(cargo_type) ? GB(ClampTo<uint16_t>(this->t->supplied[cargo_type].new_max), 8, 8) : 0;
		case 0xBC: cargo_type = GetCargoTypeByLabel(CT_MAIL); return IsValidCargoType(cargo_type) ? ClampTo<uint16_t>(this->t->supplied[cargo_type].new_max) : 0;
		case 0xBD: cargo_type = GetCargoTypeByLabel(CT_MAIL); return IsValidCargoType(cargo_type) ? GB(ClampTo<uint16_t>(this->t->supplied[cargo_type].new_max), 8, 8) : 0;
		case 0xBE: cargo_type = GetCargoTypeByLabel(CT_PASSENGERS); return IsValidCargoType(cargo_type) ? ClampTo<uint16_t>(this->t->supplied[cargo_type].new_act) : 0;
		case 0xBF: cargo_type = GetCargoTypeByLabel(CT_PASSENGERS); return IsValidCargoType(cargo_type) ? GB(ClampTo<uint16_t>(this->t->supplied[cargo_type].new_act), 8, 8) : 0;
		case 0xC0: cargo_type = GetCargoTypeByLabel(CT_MAIL); return IsValidCargoType(cargo_type) ? ClampTo<uint16_t>(this->t->supplied[cargo_type].new_act) : 0;
		case 0xC1: cargo_type = GetCargoTypeByLabel(CT_MAIL); return IsValidCargoType(cargo_type) ? GB(ClampTo<uint16_t>(this->t->supplied[cargo_type].new_act), 8, 8) : 0;
		case 0xC2: cargo_type = GetCargoTypeByLabel(CT_PASSENGERS); return IsValidCargoType(cargo_type) ? ClampTo<uint16_t>(this->t->supplied[cargo_type].old_max) : 0;
		case 0xC3: cargo_type = GetCargoTypeByLabel(CT_PASSENGERS); return IsValidCargoType(cargo_type) ? GB(ClampTo<uint16_t>(this->t->supplied[cargo_type].old_max), 8, 8) : 0;
		case 0xC4: cargo_type = GetCargoTypeByLabel(CT_MAIL); return IsValidCargoType(cargo_type) ? ClampTo<uint16_t>(this->t->supplied[cargo_type].old_max) : 0;
		case 0xC5: cargo_type = GetCargoTypeByLabel(CT_MAIL); return IsValidCargoType(cargo_type) ? GB(ClampTo<uint16_t>(this->t->supplied[cargo_type].old_max), 8, 8) : 0;
		case 0xC6: cargo_type = GetCargoTypeByLabel(CT_PASSENGERS); return IsValidCargoType(cargo_type) ? ClampTo<uint16_t>(this->t->supplied[cargo_type].old_act) : 0;
		case 0xC7: cargo_type = GetCargoTypeByLabel(CT_PASSENGERS); return IsValidCargoType(cargo_type) ? GB(ClampTo<uint16_t>(this->t->supplied[cargo_type].old_act), 8, 8) : 0;
		case 0xC8: cargo_type = GetCargoTypeByLabel(CT_MAIL); return IsValidCargoType(cargo_type) ? ClampTo<uint16_t>(this->t->supplied[cargo_type].old_act) : 0;
		case 0xC9: cargo_type = GetCargoTypeByLabel(CT_MAIL); return IsValidCargoType(cargo_type) ? GB(ClampTo<uint16_t>(this->t->supplied[cargo_type].old_act), 8, 8) : 0;
		case 0xCA: return this->t->GetPercentTransported(GetCargoTypeByLabel(CT_PASSENGERS));
		case 0xCB: return this->t->GetPercentTransported(GetCargoTypeByLabel(CT_MAIL));
		case 0xCC: return this->t->received[TAE_FOOD].new_act;
		case 0xCD: return GB(this->t->received[TAE_FOOD].new_act, 8, 8);
		case 0xCE: return this->t->received[TAE_WATER].new_act;
		case 0xCF: return GB(this->t->received[TAE_WATER].new_act, 8, 8);
		case 0xD0: return this->t->received[TAE_FOOD].old_act;
		case 0xD1: return GB(this->t->received[TAE_FOOD].old_act, 8, 8);
		case 0xD2: return this->t->received[TAE_WATER].old_act;
		case 0xD3: return GB(this->t->received[TAE_WATER].old_act, 8, 8);
		case 0xD4: return this->t->road_build_months;
		case 0xD5: return this->t->fund_buildings_months;
		case A2VRI_TOWNS_HOUSE_COUNT: return this->t->cache.num_houses;
		case A2VRI_TOWNS_POPULATION: return this->t->cache.population;

		case A2VRI_TOWNS_ZONE_0:
		case A2VRI_TOWNS_ZONE_1:
		case A2VRI_TOWNS_ZONE_2:
		case A2VRI_TOWNS_ZONE_3:
		case A2VRI_TOWNS_ZONE_4:
			return this->t->cache.squared_town_zone_radius[variable - A2VRI_TOWNS_ZONE_0];

		case A2VRI_TOWNS_XY:
			return TileY(this->t->xy) << 16 | (TileX(this->t->xy) & 0xFFFF);
	}

	Debug(grf, 1, "Unhandled town variable 0x{:X}", variable);

	extra.available = false;
	return UINT_MAX;
}

/* virtual */ void TownScopeResolver::StorePSA(uint pos, int32_t value)
{
	if (this->readonly) return;

	assert(this->t != nullptr);
	/* We can't store anything if the caller has no #GRFFile. */
	if (this->ro.grffile == nullptr) return;

	/* Check the persistent storage for the GrfID stored in register 100h. */
	uint32_t grfid = GetRegister(0x100);

	/* A NewGRF can only write in the persistent storage associated to its own GRFID. */
	if (grfid == 0xFFFFFFFF) grfid = this->ro.grffile->grfid;
	if (grfid != this->ro.grffile->grfid) return;

	/* Check if the storage exists. */
	for (auto &it : t->psa_list) {
		if (it->grfid == grfid) {
			it->StoreValue(pos, value);
			return;
		}
	}

	/* Create a new storage. */
	assert(PersistentStorage::CanAllocateItem());
	PersistentStorage *psa = new PersistentStorage(grfid, GSF_FAKE_TOWNS, this->t->xy);
	psa->StoreValue(pos, value);
	t->psa_list.push_back(psa);
}

/* virtual */ uint32_t FakeTownScopeResolver::GetVariable(uint16_t variable, uint32_t parameter, GetVariableExtra &extra) const
{
	switch (variable) {
		/* Town index */
		case 0x41: return 0xFFFF;

		case 0x40: case 0x7C: case 0x80: case 0x81: case 0x82: case 0x83: case 0x8A: case 0x92:
		case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: case 0x98: case 0x99: case 0x9A:
		case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F: case 0xA0: case 0xA1: case 0xA2:
		case 0xA3: case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xA8: case 0xA9: case 0xAA:
		case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xB2: case 0xB6: case 0xB9: case 0xBA:
		case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF: case 0xC0: case 0xC1: case 0xC2:
		case 0xC3: case 0xC4: case 0xC5: case 0xC6: case 0xC7: case 0xC8: case 0xC9: case 0xCA:
		case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF: case 0xD0: case 0xD1: case 0xD2:
		case 0xD3: case 0xD4: case 0xD5:
		case A2VRI_TOWNS_HOUSE_COUNT:
		case A2VRI_TOWNS_POPULATION:
		case A2VRI_TOWNS_ZONE_0:
		case A2VRI_TOWNS_ZONE_1:
		case A2VRI_TOWNS_ZONE_2:
		case A2VRI_TOWNS_ZONE_3:
		case A2VRI_TOWNS_ZONE_4:
		case A2VRI_TOWNS_XY:
			return 0;
	}

	Debug(grf, 1, "Unhandled town variable 0x{:X}", variable);

	extra.available = false;
	return UINT_MAX;
}

/**
 * Resolver for a town.
 * @param grffile NewGRF file associated with the town.
 * @param t %Town of the scope.
 * @param readonly Scope may change persistent storage of the town.
 */
TownResolverObject::TownResolverObject(const struct GRFFile *grffile, Town *t, bool readonly)
		: ResolverObject(grffile), town_scope(*this, t, readonly)
{
}

