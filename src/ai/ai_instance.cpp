/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file ai_instance.cpp Implementation of AIInstance. */

#include "../stdafx.h"
#include "../debug.h"
#include "../error.h"

#include "../script/squirrel_class.hpp"

#include "ai_config.hpp"
#include "ai.hpp"

#include "../script/script_storage.hpp"
#include "../script/script_gui.h"
#include "ai_info.hpp"
#include "ai_instance.hpp"

/* Manually include the Text glue. */
#include "../script/api/template/template_text.hpp.sq"

/* Convert all AI related classes to Squirrel data. */
#include "../script/api/ai/ai_includes.hpp"

#include "../company_base.h"
#include "../company_func.h"

#include "../safeguards.h"

AIInstance::AIInstance() :
	ScriptInstance("AI", ScriptType::AI)
{}

void AIInstance::Initialize(AIInfo *info)
{
	this->versionAPI = info->GetAPIVersion();

	/* Register the AIController (including the "import" command) */
	SQAIController_Register(this->engine);

	ScriptInstance::Initialize(info->GetMainScript(), info->GetInstanceName(), _current_company);
}

void AIInstance::RegisterAPI()
{
	ScriptInstance::RegisterAPI();

	/* Register all classes */
	SQAI_RegisterAll(this->engine);

	if (!this->LoadCompatibilityScripts(this->versionAPI, AI_DIR)) this->Died();
}

void AIInstance::Died()
{
	ScriptInstance::Died();

	/* Intro is not supposed to use AI, but it may have 'dummy' AI which instant dies. */
	if (_game_mode == GM_MENU) return;

	/* Don't show errors while loading savegame. They will be shown at end of loading anyway. */
	if (_switch_mode != SM_NONE) return;

	ShowScriptDebugWindow(_current_company);

	const AIInfo *info = AIConfig::GetConfig(_current_company)->GetInfo();
	if (info != nullptr) {
		ShowErrorMessage(STR_ERROR_AI_PLEASE_REPORT_CRASH, INVALID_STRING_ID, WL_WARNING);

		if (!info->GetURL().empty()) {
			ScriptLog::Info("Please report the error to the following URL:");
			ScriptLog::Info(info->GetURL());
		}
	}
}

void AIInstance::LoadDummyScript()
{
	ScriptAllocatorScope alloc_scope(this->engine);
	Script_CreateDummy(this->engine->GetVM(), STR_ERROR_AI_NO_AI_FOUND, "AI");
}

int AIInstance::GetSetting(const std::string &name)
{
	return AIConfig::GetConfig(_current_company)->GetSetting(name);
}

ScriptInfo *AIInstance::FindLibrary(const std::string &library, int version)
{
	return (ScriptInfo *)AI::FindLibrary(library, version);
}

/**
 * DoCommand callback function for all commands executed by AIs.
 */
void CcAI(const CommandCost &result, Commands cmd, TileIndex tile, const CommandPayloadBase &payload, CallbackParameter param)
{
	/*
	 * The company might not exist anymore. Check for this.
	 * The command checks are not useful since this callback
	 * is also called when the command fails, which is does
	 * when the company does not exist anymore.
	 */
	const Company *c = Company::GetIfValid(_current_company);
	if (c == nullptr || c->ai_instance == nullptr) return;

	if (c->ai_instance->DoCommandCallback(result, cmd, payload, param)) {
		c->ai_instance->Continue();
	}
}

CommandCallback AIInstance::GetDoCommandCallback()
{
	return CommandCallback::AI;
}
