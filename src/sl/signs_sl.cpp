/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file signs_sl.cpp Code handling saving and loading of economy data */

#include "../stdafx.h"
#include "../signs_base.h"
#include "../fios.h"

#include "saveload.h"

#include "../safeguards.h"

/** Description of a sign within the savegame. */
static const SaveLoad _sign_desc[] = {
	SLE_CONDVAR(Sign, name,  SLE_NAME,                   SL_MIN_VERSION, SLV_84),
	SLE_CONDSSTR(Sign, name, SLE_STR | SLF_ALLOW_CONTROL, SLV_84, SL_MAX_VERSION),
	SLE_CONDVAR(Sign, x,     SLE_FILE_I16 | SLE_VAR_I32, SL_MIN_VERSION, SLV_5),
	SLE_CONDVAR(Sign, y,     SLE_FILE_I16 | SLE_VAR_I32, SL_MIN_VERSION, SLV_5),
	SLE_CONDVAR(Sign, x,     SLE_INT32,                  SLV_5, SL_MAX_VERSION),
	SLE_CONDVAR(Sign, y,     SLE_INT32,                  SLV_5, SL_MAX_VERSION),
	SLE_CONDVAR(Sign, owner, SLE_UINT16,                  SLV_6, SL_MAX_VERSION),
	SLE_CONDVAR_X(Sign, z,   SLE_FILE_U8  | SLE_VAR_I32, SL_MIN_VERSION, SLV_164, SlXvFeatureTest(XSLFTO_AND, XSLFI_ZPOS_32_BIT, 0, 0)),
	SLE_CONDVAR_X(Sign, z,   SLE_INT32,                  SLV_164, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_OR, XSLFI_ZPOS_32_BIT)),
};

/** Load all signs */
static void Load_SIGN()
{
	int index;
	while ((index = SlIterateArray()) != -1) {
		Sign *si = new (index) Sign();
		SlObject(si, _sign_desc);
		/* Before version 6.1, signs didn't have owner.
		 * Before version 83, invalid signs were determined by si->str == 0.
		 * Before version 103, owner could be a bankrupted company.
		 *  - we can't use IsValidCompany() now, so this is fixed in AfterLoadGame()
		 * All signs that were saved are valid (including those with just 'Sign' and INVALID_OWNER).
		 *  - so set owner to OWNER_NONE if needed (signs from pre-version 6.1 would be lost) */
		if (IsSavegameVersionBefore(SLV_6, 1) || (IsSavegameVersionBefore(SLV_83) && si->owner == INVALID_OWNER)) {
			si->owner = OWNER_NONE;
		}

		/* Signs placed in scenario editor shall now be OWNER_DEITY */
		if (IsSavegameVersionBefore(SLV_171) && si->owner == OWNER_NONE && _file_to_saveload.abstract_ftype == FT_SCENARIO) {
			si->owner = OWNER_DEITY;
		}
	}
}

/** Chunk handlers related to signs. */
static const ChunkHandler sign_chunk_handlers[] = {
	MakeSaveUpstreamFeatureConditionalLoadUpstreamChunkHandler<'SIGN', XSLFI_TABLE_MISC_SL, 2>(Load_SIGN, nullptr, nullptr),
};

extern const ChunkHandlerTable _sign_chunk_handlers(sign_chunk_handlers);
