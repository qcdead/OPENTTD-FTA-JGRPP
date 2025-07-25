/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file misc_sl.cpp Saving and loading of things that didn't fit anywhere else */

#include "../stdafx.h"

#include "saveload.h"
#include "compat/misc_sl_compat.h"

#include "../date_func.h"
#include "../zoom_func.h"
#include "../window_gui.h"
#include "../window_func.h"
#include "../viewport_func.h"
#include "../gfx_func.h"
#include "../core/random_func.hpp"
#include "../fios.h"
#include "../load_check.h"
#include "../timer/timer.h"
#include "../timer/timer_game_tick.h"

#include "../safeguards.h"

extern TileIndex _cur_tileloop_tile;
extern TileIndex _aux_tileloop_tile;
extern uint16_t _disaster_delay;
extern uint8_t _trees_tick_ctr;

/* Keep track of current game position */
extern int _saved_scrollpos_x;
extern int _saved_scrollpos_y;
extern ZoomLevel _saved_scrollpos_zoom;

extern uint8_t _age_cargo_skip_counter; ///< Skip aging of cargo? Used before savegame version 162.
extern TimeoutTimer<TimerGameTick> _new_competitor_timeout;

namespace upstream_sl {

static const SaveLoad _date_desc[] = {
	SLEG_CONDVAR("date",                   CalTime::Detail::now.cal_date,          SLE_FILE_U16 | SLE_VAR_I32,  SL_MIN_VERSION,  SLV_31),
	SLEG_CONDVAR("date",                   CalTime::Detail::now.cal_date,          SLE_INT32,                   SLV_31, SL_MAX_VERSION),
	    SLEG_VAR("date_fract",             CalTime::Detail::now.cal_date_fract,    SLE_UINT16),
	SLEG_CONDVAR("tick_counter",           _tick_counter,                          SLE_FILE_U16 | SLE_VAR_U64,  SL_MIN_VERSION, SLV_U64_TICK_COUNTER),
	SLEG_CONDVAR("tick_counter",           _tick_counter,                          SLE_UINT64,                  SLV_U64_TICK_COUNTER, SL_MAX_VERSION),
	SLEG_CONDVAR("economy_date",           EconTime::Detail::now.econ_date,        SLE_INT32,                   SLV_ECONOMY_DATE, SL_MAX_VERSION),
	SLEG_CONDVAR("economy_date_fract",     EconTime::Detail::now.econ_date_fract,  SLE_UINT16,                  SLV_ECONOMY_DATE, SL_MAX_VERSION),
	SLEG_CONDVAR("calendar_sub_date_fract", CalTime::Detail::now.sub_date_fract,   SLE_UINT16,                SLV_CALENDAR_SUB_DATE_FRACT, SL_MAX_VERSION),
	SLEG_CONDVAR("age_cargo_skip_counter", _age_cargo_skip_counter, SLE_UINT8,                   SL_MIN_VERSION, SLV_162),
	SLEG_CONDVAR("cur_tileloop_tile",      _cur_tileloop_tile,      SLE_FILE_U16 | SLE_VAR_U32,  SL_MIN_VERSION, SLV_6),
	SLEG_CONDVAR("cur_tileloop_tile",      _cur_tileloop_tile,      SLE_UINT32,                  SLV_6, SL_MAX_VERSION),
	    SLEG_VAR("next_disaster_start",         _disaster_delay,         SLE_UINT16),
	    SLEG_VAR("random_state[0]",        _random.state[0],        SLE_UINT32),
	    SLEG_VAR("random_state[1]",        _random.state[1],        SLE_UINT32),
	SLEG_CONDVAR("company_tick_counter", _cur_company_tick_index,   SLE_FILE_U8  | SLE_VAR_U32, SL_MIN_VERSION, SLV_MAX_OG),
	SLEG_CONDVAR("company_tick_counter", _cur_company_tick_index,   SLE_FILE_U16 | SLE_VAR_U32, SLV_FIVE_HUNDRED_COMPANIES, SL_MAX_VERSION),	    SLEG_VAR("trees_tick_counter",     _trees_tick_ctr,         SLE_UINT8),
	SLEG_CONDVAR("pause_mode",             _pause_mode,             SLE_UINT8,                   SLV_4, SL_MAX_VERSION),
	SLEG_CONDSSTR("id",                    _game_session_stats.savegame_id, SLE_STR,                     SLV_SAVEGAME_ID, SL_MAX_VERSION),
	/* For older savegames, we load the current value as the "period"; afterload will set the "fired" and "elapsed". */
	SLEG_CONDVAR("next_competitor_start",        _new_competitor_timeout.period.value,    SLE_FILE_U16 | SLE_VAR_U32,  SL_MIN_VERSION, SLV_109),
	SLEG_CONDVAR("next_competitor_start",        _new_competitor_timeout.period.value,    SLE_UINT32,                  SLV_109, SLV_AI_START_DATE),
	SLEG_CONDVAR("competitors_interval",         _new_competitor_timeout.period.value,    SLE_UINT32,                  SLV_AI_START_DATE, SL_MAX_VERSION),
	SLEG_CONDVAR("competitors_interval_elapsed", _new_competitor_timeout.storage.elapsed, SLE_UINT32,                  SLV_AI_START_DATE, SL_MAX_VERSION),
	SLEG_CONDVAR("competitors_interval_fired",   _new_competitor_timeout.fired,           SLE_BOOL,                    SLV_AI_START_DATE, SL_MAX_VERSION),
};

static const SaveLoad _date_check_desc[] = {
	SLEG_CONDVAR("date", _load_check_data.current_date,  SLE_FILE_U16 | SLE_VAR_I32,  SL_MIN_VERSION,  SLV_31),
	SLEG_CONDVAR("date", _load_check_data.current_date,  SLE_INT32,                  SLV_31, SL_MAX_VERSION),
};

/* Save load date related variables as well as persistent tick counters
 * XXX: currently some unrelated stuff is just put here */
struct DATEChunkHandler : ChunkHandler {
	DATEChunkHandler() : ChunkHandler('DATE', CH_TABLE) {}

	void Save() const override
	{
		SlTableHeader(_date_desc);

		SlSetArrayIndex(0);
		SlGlobList(_date_desc);
	}

	void LoadCommon(const SaveLoadTable &slt, const SaveLoadCompatTable &slct) const
	{
		const std::vector<SaveLoad> oslt = SlCompatTableHeader(slt, slct);

		if (!IsSavegameVersionBefore(SLV_RIFF_TO_ARRAY) && SlIterateArray() == -1) return;
		SlGlobList(oslt);
		if (!IsSavegameVersionBefore(SLV_RIFF_TO_ARRAY) && SlIterateArray() != -1) SlErrorCorrupt("Too many DATE entries");
	}

	void Load() const override
	{
		this->LoadCommon(_date_desc, _date_sl_compat);
	}


	void LoadCheck(size_t) const override
	{
		this->LoadCommon(_date_check_desc, _date_check_sl_compat);

		if (IsSavegameVersionBefore(SLV_31)) {
			_load_check_data.current_date += CalTime::DAYS_TILL_ORIGINAL_BASE_YEAR.AsDelta();
		}
	}
};

static const SaveLoad _view_desc[] = {
	SLEG_CONDVAR("x",    _saved_scrollpos_x,    SLE_FILE_I16 | SLE_VAR_I32, SL_MIN_VERSION, SLV_6),
	SLEG_CONDVAR("x",    _saved_scrollpos_x,    SLE_INT32,                  SLV_6, SL_MAX_VERSION),
	SLEG_CONDVAR("y",    _saved_scrollpos_y,    SLE_FILE_I16 | SLE_VAR_I32, SL_MIN_VERSION, SLV_6),
	SLEG_CONDVAR("y",    _saved_scrollpos_y,    SLE_INT32,                  SLV_6, SL_MAX_VERSION),
	    SLEG_VAR("zoom", _saved_scrollpos_zoom, SLE_UINT8),
};

struct VIEWChunkHandler : ChunkHandler {
	VIEWChunkHandler() : ChunkHandler('VIEW', CH_TABLE) {}

	void Save() const override
	{
		SlTableHeader(_view_desc);

		SlSetArrayIndex(0);
		SlGlobList(_view_desc);
	}

	void Load() const override
	{
		const std::vector<SaveLoad> slt = SlCompatTableHeader(_view_desc, _view_sl_compat);

		if (!IsSavegameVersionBefore(SLV_RIFF_TO_ARRAY) && SlIterateArray() == -1) return;
		SlGlobList(slt);
		if (!IsSavegameVersionBefore(SLV_RIFF_TO_ARRAY) && SlIterateArray() != -1) SlErrorCorrupt("Too many DATE entries");
	}
};

static const DATEChunkHandler DATE;
static const VIEWChunkHandler VIEW;
static const ChunkHandlerRef misc_chunk_handlers[] = {
	DATE,
	VIEW,
};

extern const ChunkHandlerTable _misc_chunk_handlers(misc_chunk_handlers);

}
