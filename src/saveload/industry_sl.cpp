/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file industry_sl.cpp Code handling saving and loading of industries */

#include "../stdafx.h"

#include "saveload.h"
#include "compat/industry_sl_compat.h"

#include "../industry.h"
#include "newgrf_sl.h"

#include "../safeguards.h"

extern OldIndustryAccepted _old_industry_accepted;
extern OldIndustryProduced _old_industry_produced;
extern void LoadMoveOldAcceptsProduced(Industry *i);

namespace upstream_sl {

class SlIndustryAccepted : public DefaultSaveLoadHandler<SlIndustryAccepted, Industry> {
public:
	inline static const SaveLoad description[] = {
		 SLE_VAR(Industry::AcceptedCargo, cargo, SLE_UINT8),
		 SLE_VAR(Industry::AcceptedCargo, waiting, SLE_UINT16),
		 SLE_VAR(Industry::AcceptedCargo, last_accepted, SLE_INT32),
	};
	inline const static SaveLoadCompatTable compat_description = {};

	void Save(Industry *i) const override
	{
		NOT_REACHED();
	}

	void Load(Industry *i) const override
	{
		size_t len = SlGetStructListLength(INDUSTRY_NUM_INPUTS);

		i->accepted_cargo_count = static_cast<uint8_t>(len);
		i->accepted = std::make_unique<Industry::AcceptedCargo[]>(i->accepted_cargo_count);

		for (size_t j = 0; j < len; j++) {
			Industry::AcceptedCargo &a = i->accepted[j];
			SlObject(&a, this->GetDescription());
		}
	}
};

class SlIndustryProducedHistory : public DefaultSaveLoadHandler<SlIndustryProducedHistory, Industry::ProducedCargo> {
public:
	inline static const SaveLoad description[] = {
		 SLE_VAR(Industry::ProducedHistory, production, SLE_FILE_U16 | SLE_VAR_U32),
		 SLE_VAR(Industry::ProducedHistory, transported, SLE_FILE_U16 | SLE_VAR_U32),
	};
	inline const static SaveLoadCompatTable compat_description = {};

	void Save(Industry::ProducedCargo *p) const override
	{
		NOT_REACHED();
	}

	void Load(Industry::ProducedCargo *p) const override
	{
		size_t len = SlGetStructListLength(p->history.size());

		for (auto &h : p->history) {
			if (--len > p->history.size()) break; // unsigned so wraps after hitting zero.
			SlObject(&h, this->GetDescription());
		}
	}
};

class SlIndustryProduced : public DefaultSaveLoadHandler<SlIndustryProduced, Industry> {
public:
	inline static const SaveLoad description[] = {
		 SLE_VAR(Industry::ProducedCargo, cargo, SLE_UINT8),
		 SLE_VAR(Industry::ProducedCargo, waiting, SLE_UINT16),
		 SLE_VAR(Industry::ProducedCargo, rate, SLE_UINT8),
		SLEG_STRUCTLIST("history", SlIndustryProducedHistory),
	};
	inline const static SaveLoadCompatTable compat_description = {};

	void Save(Industry *i) const override
	{
		NOT_REACHED();
	}

	void Load(Industry *i) const override
	{
		size_t len = SlGetStructListLength(INDUSTRY_NUM_OUTPUTS);

		i->produced_cargo_count = static_cast<uint8_t>(len);
		i->produced = std::make_unique<Industry::ProducedCargo[]>(i->produced_cargo_count);

		for (size_t j = 0; j < len; j++) {
			Industry::ProducedCargo &p = i->produced[j];
			SlObject(&p, this->GetDescription());
		}
	}
};

static OldPersistentStorage _old_ind_persistent_storage;

static const SaveLoad _industry_desc[] = {
	SLE_CONDVAR(Industry, location.tile,              SLE_FILE_U16 | SLE_VAR_U32,  SL_MIN_VERSION, SLV_6),
	SLE_CONDVAR(Industry, location.tile,              SLE_UINT32,                  SLV_6, SL_MAX_VERSION),
	    SLE_VAR(Industry, location.w,                 SLE_FILE_U8 | SLE_VAR_U16),
	    SLE_VAR(Industry, location.h,                 SLE_FILE_U8 | SLE_VAR_U16),
	    SLE_REF(Industry, town,                       REF_TOWN),
	SLE_CONDREF(Industry, neutral_station,            REF_STATION,                SLV_SERVE_NEUTRAL_INDUSTRIES, SL_MAX_VERSION),
	SLEG_CONDARR("produced_cargo",             _old_industry_produced.old_cargo,                  SLE_UINT8,  INDUSTRY_ORIGINAL_NUM_OUTPUTS, SLV_78, SLV_EXTEND_INDUSTRY_CARGO_SLOTS),
	SLEG_CONDARR("produced_cargo",             _old_industry_produced.old_cargo,                  SLE_UINT8,  INDUSTRY_NUM_OUTPUTS, SLV_EXTEND_INDUSTRY_CARGO_SLOTS, SLV_INDUSTRY_CARGO_REORGANISE),
	SLEG_CONDARR("incoming_cargo_waiting",     _old_industry_accepted.old_waiting,                SLE_UINT16, INDUSTRY_ORIGINAL_NUM_INPUTS, SLV_70, SLV_EXTEND_INDUSTRY_CARGO_SLOTS),
	SLEG_CONDARR("incoming_cargo_waiting",     _old_industry_accepted.old_waiting,                SLE_UINT16, INDUSTRY_NUM_INPUTS, SLV_EXTEND_INDUSTRY_CARGO_SLOTS, SLV_INDUSTRY_CARGO_REORGANISE),
	SLEG_CONDARR("produced_cargo_waiting",     _old_industry_produced.old_waiting,                SLE_UINT16, INDUSTRY_ORIGINAL_NUM_OUTPUTS, SL_MIN_VERSION, SLV_EXTEND_INDUSTRY_CARGO_SLOTS),
	SLEG_CONDARR("produced_cargo_waiting",     _old_industry_produced.old_waiting,                SLE_UINT16, INDUSTRY_NUM_OUTPUTS, SLV_EXTEND_INDUSTRY_CARGO_SLOTS, SLV_INDUSTRY_CARGO_REORGANISE),
	SLEG_CONDARR("production_rate",            _old_industry_produced.old_rate,                   SLE_UINT8,  INDUSTRY_ORIGINAL_NUM_OUTPUTS, SL_MIN_VERSION, SLV_EXTEND_INDUSTRY_CARGO_SLOTS),
	SLEG_CONDARR("production_rate",            _old_industry_produced.old_rate,                   SLE_UINT8,  INDUSTRY_NUM_OUTPUTS, SLV_EXTEND_INDUSTRY_CARGO_SLOTS, SLV_INDUSTRY_CARGO_REORGANISE),
	SLEG_CONDARR("accepts_cargo",              _old_industry_accepted.old_cargo,                  SLE_UINT8,  INDUSTRY_ORIGINAL_NUM_INPUTS, SLV_78, SLV_EXTEND_INDUSTRY_CARGO_SLOTS),
	SLEG_CONDARR("accepts_cargo",              _old_industry_accepted.old_cargo,                  SLE_UINT8,  INDUSTRY_NUM_INPUTS, SLV_EXTEND_INDUSTRY_CARGO_SLOTS, SLV_INDUSTRY_CARGO_REORGANISE),
	    SLE_VAR(Industry, prod_level,                 SLE_UINT8),
	SLEG_CONDARR("this_month_production",      _old_industry_produced.old_this_month_production,  SLE_FILE_U16 | SLE_VAR_U32, INDUSTRY_ORIGINAL_NUM_OUTPUTS, SL_MIN_VERSION, SLV_EXTEND_INDUSTRY_CARGO_SLOTS),
	SLEG_CONDARR("this_month_production",      _old_industry_produced.old_this_month_production,  SLE_FILE_U16 | SLE_VAR_U32, INDUSTRY_NUM_OUTPUTS, SLV_EXTEND_INDUSTRY_CARGO_SLOTS, SLV_INDUSTRY_CARGO_REORGANISE),
	SLEG_CONDARR("this_month_transported",     _old_industry_produced.old_this_month_transported, SLE_FILE_U16 | SLE_VAR_U32, INDUSTRY_ORIGINAL_NUM_OUTPUTS, SL_MIN_VERSION, SLV_EXTEND_INDUSTRY_CARGO_SLOTS),
	SLEG_CONDARR("this_month_transported",     _old_industry_produced.old_this_month_transported, SLE_FILE_U16 | SLE_VAR_U32, INDUSTRY_NUM_OUTPUTS, SLV_EXTEND_INDUSTRY_CARGO_SLOTS, SLV_INDUSTRY_CARGO_REORGANISE),
	SLEG_CONDARR("last_month_production",      _old_industry_produced.old_last_month_production,  SLE_FILE_U16 | SLE_VAR_U32, INDUSTRY_ORIGINAL_NUM_OUTPUTS, SL_MIN_VERSION, SLV_EXTEND_INDUSTRY_CARGO_SLOTS),
	SLEG_CONDARR("last_month_production",      _old_industry_produced.old_last_month_production,  SLE_FILE_U16 | SLE_VAR_U32, INDUSTRY_NUM_OUTPUTS, SLV_EXTEND_INDUSTRY_CARGO_SLOTS, SLV_INDUSTRY_CARGO_REORGANISE),
	SLEG_CONDARR("last_month_transported",     _old_industry_produced.old_last_month_transported, SLE_FILE_U16 | SLE_VAR_U32, INDUSTRY_ORIGINAL_NUM_OUTPUTS, SL_MIN_VERSION, SLV_EXTEND_INDUSTRY_CARGO_SLOTS),
	SLEG_CONDARR("last_month_transported",     _old_industry_produced.old_last_month_transported, SLE_FILE_U16 | SLE_VAR_U32, INDUSTRY_NUM_OUTPUTS, SLV_EXTEND_INDUSTRY_CARGO_SLOTS, SLV_INDUSTRY_CARGO_REORGANISE),

	    SLE_VAR(Industry, counter,                    SLE_UINT16),

	    SLE_VAR(Industry, type,                       SLE_UINT8),
	SLE_CONDVAR(Industry, owner,                      SLE_FILE_U8 | SLE_VAR_U16,  SL_MIN_VERSION, SLV_MAX_OG),
	SLE_CONDVAR(Industry, owner,                      SLE_UINT16,                 SLV_FIVE_HUNDRED_COMPANIES, SL_MAX_VERSION),
	    SLE_VAR(Industry, random_colour,              SLE_UINT8),
	SLE_CONDVAR(Industry, last_prod_year,             SLE_FILE_U8 | SLE_VAR_I32,  SL_MIN_VERSION, SLV_31),
	SLE_CONDVAR(Industry, last_prod_year,             SLE_INT32,                 SLV_31, SL_MAX_VERSION),
	    SLE_VAR(Industry, was_cargo_delivered,        SLE_UINT8),
	SLE_CONDVAR(Industry, ctlflags,                   SLE_UINT8,                 SLV_GS_INDUSTRY_CONTROL, SL_MAX_VERSION),

	SLE_CONDVAR(Industry, founder,                    SLE_FILE_U8 | SLE_VAR_U16,  SLV_70, SLV_MAX_OG),
	SLE_CONDVAR(Industry, founder,                    SLE_UINT16,                SLV_FIVE_HUNDRED_COMPANIES, SL_MAX_VERSION),
	SLE_CONDVAR(Industry, construction_date,          SLE_INT32,                 SLV_70, SL_MAX_VERSION),
	SLE_CONDVAR(Industry, construction_type,          SLE_UINT8,                 SLV_70, SL_MAX_VERSION),
	SLEG_CONDVAR("last_cargo_accepted_at[0]",  _old_industry_accepted.old_last_accepted[0], SLE_INT32,     SLV_70, SLV_EXTEND_INDUSTRY_CARGO_SLOTS),
	SLEG_CONDARR("last_cargo_accepted_at",     _old_industry_accepted.old_last_accepted,    SLE_INT32, 16, SLV_EXTEND_INDUSTRY_CARGO_SLOTS, SLV_INDUSTRY_CARGO_REORGANISE),
	SLE_CONDVAR(Industry, selected_layout,            SLE_UINT8,                 SLV_73, SL_MAX_VERSION),
	SLE_CONDVAR(Industry, exclusive_supplier,         SLE_FILE_U8 | SLE_VAR_U16,  SLV_GS_INDUSTRY_CONTROL, SLV_MAX_OG),
	SLE_CONDVAR(Industry, exclusive_supplier,         SLE_UINT16,                SLV_FIVE_HUNDRED_COMPANIES, SL_MAX_VERSION),
	SLE_CONDVAR(Industry, exclusive_consumer,         SLE_FILE_U8 | SLE_VAR_U16,  SLV_GS_INDUSTRY_CONTROL, SLV_MAX_OG),
	SLE_CONDVAR(Industry, exclusive_consumer,         SLE_UINT16,                SLV_FIVE_HUNDRED_COMPANIES, SL_MAX_VERSION),

	SLEG_CONDARR("storage", _old_ind_persistent_storage.storage, SLE_UINT32, 16, SLV_76, SLV_161),
	SLE_CONDREF(Industry, psa,                        REF_STORAGE,              SLV_161, SL_MAX_VERSION),

	SLE_CONDVAR(Industry, random,                     SLE_UINT16,                SLV_82, SL_MAX_VERSION),
	SLE_CONDSSTR(Industry, text,     SLE_STR | SLF_ALLOW_CONTROL,     SLV_INDUSTRY_TEXT, SL_MAX_VERSION),

	SLEG_CONDSTRUCTLIST("accepted", SlIndustryAccepted,                          SLV_INDUSTRY_CARGO_REORGANISE, SL_MAX_VERSION),
	SLEG_CONDSTRUCTLIST("produced", SlIndustryProduced,                          SLV_INDUSTRY_CARGO_REORGANISE, SL_MAX_VERSION),
};

struct INDYChunkHandler : ChunkHandler {
	INDYChunkHandler() : ChunkHandler('INDY', CH_TABLE) {}

	void Save() const override
	{
		SlTableHeader(_industry_desc);

		/* Write the industries */
		for (Industry *ind : Industry::Iterate()) {
			SlSetArrayIndex(ind->index);
			SlObject(ind, _industry_desc);
		}
	}

	void Load() const override
	{
		const std::vector<SaveLoad> slt = SlCompatTableHeader(_industry_desc, _industry_sl_compat);

		int index;

		_old_industry_accepted.Reset();
		_old_industry_produced.Reset();

		while ((index = SlIterateArray()) != -1) {
			Industry *i = new (index) Industry();
			SlObject(i, slt);

			/* Before savegame version 161, persistent storages were not stored in a pool. */
			if (IsSavegameVersionBefore(SLV_161) && !IsSavegameVersionBefore(SLV_76)) {
				/* Store the old persistent storage. The GRFID will be added later. */
				assert(PersistentStorage::CanAllocateItem());
				i->psa = new PersistentStorage(0, 0, {});
				std::copy(std::begin(_old_ind_persistent_storage.storage), std::end(_old_ind_persistent_storage.storage), std::begin(i->psa->storage));
			}
			if (IsSavegameVersionBefore(SLV_INDUSTRY_CARGO_REORGANISE)) {
				LoadMoveOldAcceptsProduced(i);
			}
		}
	}

	void FixPointers() const override
	{
		for (Industry *i : Industry::Iterate()) {
			SlObject(i, _industry_desc);
		}
	}
};

struct IIDSChunkHandler : NewGRFMappingChunkHandler {
	IIDSChunkHandler() : NewGRFMappingChunkHandler('IIDS', _industry_mngr) {}
};

struct TIDSChunkHandler : NewGRFMappingChunkHandler {
	TIDSChunkHandler() : NewGRFMappingChunkHandler('TIDS', _industile_mngr) {}
};

/** Description of the data to save and load in #IndustryBuildData. */
static const SaveLoad _industry_builder_desc[] = {
	SLEG_VAR("wanted_inds", _industry_builder.wanted_inds, SLE_UINT32),
};

/** Industry builder. */
struct IBLDChunkHandler : ChunkHandler {
	IBLDChunkHandler() : ChunkHandler('IBLD', CH_TABLE) {}

	void Save() const override
	{
		SlTableHeader(_industry_builder_desc);

		SlSetArrayIndex(0);
		SlGlobList(_industry_builder_desc);
	}

	void Load() const override
	{
		const std::vector<SaveLoad> slt = SlCompatTableHeader(_industry_builder_desc, _industry_builder_sl_compat);

		if (!IsSavegameVersionBefore(SLV_RIFF_TO_ARRAY) && SlIterateArray() == -1) return;
		SlGlobList(slt);
		if (!IsSavegameVersionBefore(SLV_RIFF_TO_ARRAY) && SlIterateArray() != -1) SlErrorCorrupt("Too many IBLD entries");
	}
};

/** Description of the data to save and load in #IndustryTypeBuildData. */
static const SaveLoad _industrytype_builder_desc[] = {
	SLE_VAR(IndustryTypeBuildData, probability,  SLE_UINT32),
	SLE_VAR(IndustryTypeBuildData, min_number,   SLE_UINT8),
	SLE_VAR(IndustryTypeBuildData, target_count, SLE_UINT16),
	SLE_VAR(IndustryTypeBuildData, max_wait,     SLE_UINT16),
	SLE_VAR(IndustryTypeBuildData, wait_count,   SLE_UINT16),
};

/** Industry-type build data. */
struct ITBLChunkHandler : ChunkHandler {
	ITBLChunkHandler() : ChunkHandler('ITBL', CH_TABLE) {}

	void Save() const override
	{
		SlTableHeader(_industrytype_builder_desc);

		for (int i = 0; i < NUM_INDUSTRYTYPES; i++) {
			SlSetArrayIndex(i);
			SlObject(_industry_builder.builddata + i, _industrytype_builder_desc);
		}
	}

	void Load() const override
	{
		const std::vector<SaveLoad> slt = SlCompatTableHeader(_industrytype_builder_desc, _industrytype_builder_sl_compat);

		for (IndustryType it = 0; it < NUM_INDUSTRYTYPES; it++) {
			_industry_builder.builddata[it].Reset();
		}
		int index;
		while ((index = SlIterateArray()) != -1) {
			if ((uint)index >= NUM_INDUSTRYTYPES) SlErrorCorrupt("Too many industry builder datas");
			SlObject(_industry_builder.builddata + index, slt);
		}
	}
};

static const INDYChunkHandler INDY;
static const IIDSChunkHandler IIDS;
static const TIDSChunkHandler TIDS;
static const IBLDChunkHandler IBLD;
static const ITBLChunkHandler ITBL;
static const ChunkHandlerRef industry_chunk_handlers[] = {
	INDY,
	IIDS,
	TIDS,
	IBLD,
	ITBL,
};

extern const ChunkHandlerTable _industry_chunk_handlers(industry_chunk_handlers);

}
