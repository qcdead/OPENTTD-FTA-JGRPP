/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file script_instance.cpp Implementation of ScriptInstance. */

#include "../stdafx.h"
#include "../debug.h"
#include "../sl/saveload.h"

#include "../script/squirrel_class.hpp"
#include "../script/squirrel_std.hpp"

#include "script_fatalerror.hpp"
#include "script_storage.hpp"
#include "script_info.hpp"
#include "script_instance.hpp"

#include "api/script_controller.hpp"
#include "api/script_error.hpp"
#include "api/script_event.hpp"
#include "api/script_log.hpp"

#include "../company_base.h"
#include "../company_func.h"
#include "../fileio_func.h"
#include "../league_type.h"
#include "../goal_type.h"
#include "../group_type.h"
#include "../signs_type.h"
#include "../story_type.h"
#include "../vehicle_type.h"

#include "../core/format.hpp"

#include "../safeguards.h"

ScriptStorage::~ScriptStorage()
{
	/* Free our pointers */
	if (event_data != nullptr) ScriptEventController::FreeEventPointer();
}

/**
 * Callback called by squirrel when a script uses "print" and for error messages.
 * @param error_msg Is this an error message?
 * @param message The actual message text.
 */
static void PrintFunc(bool error_msg, const std::string &message)
{
	/* Convert to OpenTTD internal capable string */
	ScriptController::Print(error_msg, message);
}

ScriptInstance::ScriptInstance(const char *APIName, ScriptType script_type) :
	engine(nullptr),
	controller(nullptr),
	storage(nullptr),
	instance(nullptr),
	is_started(false),
	is_dead(false),
	is_save_data_on_stack(false),
	suspend(0),
	is_paused(false),
	in_shutdown(false),
	callback(nullptr),
	APIName(APIName),
	script_type(script_type),
	allow_text_param_mismatch(false)
{
	this->storage = new ScriptStorage();
	this->engine  = new Squirrel(APIName);
	this->engine->SetPrintFunction(&PrintFunc);
}

void ScriptInstance::Initialize(const std::string &main_script, const std::string &instance_name, CompanyID company)
{
	ScriptObject::ActiveInstance active(this);

	this->controller = new ScriptController(company);

	/* Register the API functions and classes */
	this->engine->SetGlobalPointer(this->engine);
	this->RegisterAPI();
	if (this->IsDead()) {
		/* Failed to register API; a message has already been logged. */
		return;
	}

	try {
		ScriptObject::SetAllowDoCommand(false);
		/* Load and execute the script for this script */
		if (main_script == "%_dummy") {
			this->LoadDummyScript();
		} else if (!this->engine->LoadScript(main_script) || this->engine->IsSuspended()) {
			if (this->engine->IsSuspended()) ScriptLog::Error("This script took too long to load script. AI is not started.");
			this->Died();
			return;
		}

		if (this->script_type == ScriptType::GS) {
			if (instance_name == "BeeRewardClass") {
				this->LoadCompatibilityScripts("brgs", GAME_DIR);
			}
		}

		/* Create the main-class */
		this->instance = new SQObject();
		if (!this->engine->CreateClassInstance(instance_name, this->controller, this->instance)) {
			/* If CreateClassInstance has returned false instance has not been
			 * registered with squirrel, so avoid trying to Release it by clearing it now */
			delete this->instance;
			this->instance = nullptr;
			this->Died();
			return;
		}
		ScriptObject::SetAllowDoCommand(true);
	} catch (Script_FatalError &e) {
		this->is_dead = true;
		this->engine->ThrowError(e.GetErrorMessage());
		this->engine->ResumeError();
		this->Died();
	}
}

void ScriptInstance::RegisterAPI()
{
	squirrel_register_std(this->engine);
}

bool ScriptInstance::LoadCompatibilityScripts(const std::string &api_version, Subdirectory dir)
{
	const char *api_vers[] = { "1.2", "1.3", "1.4", "1.5", "1.6", "1.7", "1.8", "1.9", "1.10", "1.11", "12", "13", "14", "15" };
	uint api_idx = 0;
	for (; api_idx < lengthof(api_vers) ; api_idx++) {
		if (api_version == api_vers[api_idx]) break;
	}
	if (api_idx < 12) {
		/* 13 and below */
		this->allow_text_param_mismatch = true;
	}

	std::string script_name = fmt::format("compat_{}.nut", api_version);
	for (Searchpath sp : _valid_searchpaths) {
		std::string buf = FioGetDirectory(sp, dir);
		buf += script_name;
		if (!FileExists(buf)) continue;

		if (this->engine->LoadScript(buf)) return true;

		ScriptLog::Error("Failed to load API compatibility script");
		Debug(script, 0, "Error compiling / running API compatibility script: {}", buf);
		return false;
	}

	const char *message_suffix;
	switch (dir) {
		case AI_DIR:
			message_suffix = ", please check that the 'ai/' directory is properly installed";
			break;

		case GAME_DIR:
			message_suffix = ", please check that the 'game/' directory is properly installed";
			break;

		default:
			message_suffix = "";
			break;
	}

	ScriptLog::Warning(fmt::format("API compatibility script not found: {}{}", script_name, message_suffix));
	return true;
}

ScriptInstance::~ScriptInstance()
{
	ScriptObject::ActiveInstance active(this);
	this->in_shutdown = true;

	if (instance != nullptr) this->engine->ReleaseObject(this->instance);
	if (engine != nullptr) delete this->engine;
	delete this->storage;
	delete this->controller;
	delete this->instance;
}

void ScriptInstance::Continue()
{
	assert(this->suspend < 0);
	this->suspend = -this->suspend - 1;
}

void ScriptInstance::Died()
{
	Debug(script, 0, "The script died unexpectedly.");
	this->is_dead = true;
	this->in_shutdown = true;

	this->last_allocated_memory = this->GetAllocatedMemory(); // Update cache

	if (this->instance != nullptr) this->engine->ReleaseObject(this->instance);
	delete this->instance;
	delete this->engine;
	this->instance = nullptr;
	this->engine = nullptr;
}

void ScriptInstance::GameLoop()
{
	ScriptObject::ActiveInstance active(this);

	if (this->IsDead()) return;
	if (this->engine->HasScriptCrashed()) {
		/* The script crashed during saving, kill it here. */
		this->Died();
		return;
	}
	if (this->is_paused) return;
	this->controller->ticks++;

	if (this->suspend   < -1) this->suspend++; // Multiplayer suspend, increase up to -1.
	if (this->suspend   < 0)  return;          // Multiplayer suspend, wait for Continue().
	if (--this->suspend > 0)  return;          // Singleplayer suspend, decrease to 0.

	_current_company = ScriptObject::GetCompany();

	/* If there is a callback to call, call that first */
	if (this->callback != nullptr) {
		if (this->is_save_data_on_stack) {
			sq_poptop(this->engine->GetVM());
			this->is_save_data_on_stack = false;
		}
		try {
			this->callback(this);
		} catch (Script_Suspend &e) {
			this->suspend  = e.GetSuspendTime();
			this->callback = e.GetSuspendCallback();

			return;
		}
	}

	this->suspend  = 0;
	this->callback = nullptr;

	if (!this->is_started) {
		try {
			ScriptObject::SetAllowDoCommand(false);
			/* Run the constructor if it exists. Don't allow any DoCommands in it. */
			if (this->engine->MethodExists(*this->instance, "constructor")) {
				if (!this->engine->CallMethod(*this->instance, "constructor", MAX_CONSTRUCTOR_OPS) || this->engine->IsSuspended()) {
					if (this->engine->IsSuspended()) ScriptLog::Error("This script took too long to initialize. Script is not started.");
					this->Died();
					return;
				}
			}
			if (!this->CallLoad() || this->engine->IsSuspended()) {
				if (this->engine->IsSuspended()) ScriptLog::Error("This script took too long in the Load function. Script is not started.");
				this->Died();
				return;
			}
			ScriptObject::SetAllowDoCommand(true);
			/* Start the script by calling Start() */
			if (!this->engine->CallMethod(*this->instance, "Start", this->GetMaxOpsTillSuspend()) || !this->engine->IsSuspended()) this->Died();
		} catch (Script_Suspend &e) {
			this->suspend  = e.GetSuspendTime();
			this->callback = e.GetSuspendCallback();
		} catch (Script_FatalError &e) {
			this->is_dead = true;
			this->engine->ThrowError(e.GetErrorMessage());
			this->engine->ResumeError();
			this->Died();
		}

		this->is_started = true;
		return;
	}
	if (this->is_save_data_on_stack) {
		sq_poptop(this->engine->GetVM());
		this->is_save_data_on_stack = false;
	}

	/* Continue the VM */
	try {
		if (!this->engine->Resume(this->GetMaxOpsTillSuspend())) this->Died();
	} catch (Script_Suspend &e) {
		this->suspend  = e.GetSuspendTime();
		this->callback = e.GetSuspendCallback();
	} catch (Script_FatalError &e) {
		this->is_dead = true;
		this->engine->ThrowError(e.GetErrorMessage());
		this->engine->ResumeError();
		this->Died();
	}
}

void ScriptInstance::CollectGarbage()
{
	if (this->is_started && !this->IsDead()) {
		ScriptObject::ActiveInstance active(this);
		this->engine->CollectGarbage();
	}
}

/* static */ void ScriptInstance::DoCommandReturn(ScriptInstance *instance)
{
	instance->engine->InsertResult(ScriptObject::GetLastCommandRes());
}

/* static */ void ScriptInstance::DoCommandReturnVehicleID(ScriptInstance *instance)
{
	instance->engine->InsertResult(ScriptObject::GetLastCommandResultData<VehicleID>(::INVALID_VEHICLE));
}

/* static */ void ScriptInstance::DoCommandReturnSignID(ScriptInstance *instance)
{
	instance->engine->InsertResult(ScriptObject::GetLastCommandResultData<SignID>(::INVALID_SIGN));
}

/* static */ void ScriptInstance::DoCommandReturnGroupID(ScriptInstance *instance)
{
	instance->engine->InsertResult(ScriptObject::GetLastCommandResultData<GroupID>(::INVALID_GROUP));
}

/* static */ void ScriptInstance::DoCommandReturnGoalID(ScriptInstance *instance)
{
	instance->engine->InsertResult(ScriptObject::GetLastCommandResultData<GoalID>(::INVALID_GOAL));
}

/* static */ void ScriptInstance::DoCommandReturnStoryPageID(ScriptInstance *instance)
{
	instance->engine->InsertResult(ScriptObject::GetLastCommandResultData<StoryPageID>(::INVALID_STORY_PAGE));
}

/* static */ void ScriptInstance::DoCommandReturnStoryPageElementID(ScriptInstance *instance)
{
	instance->engine->InsertResult(ScriptObject::GetLastCommandResultData<StoryPageElementID>(::INVALID_STORY_PAGE_ELEMENT));
}

/* static */ void ScriptInstance::DoCommandReturnLeagueTableElementID(ScriptInstance *instance)
{
	instance->engine->InsertResult(ScriptObject::GetLastCommandResultData<LeagueTableElementID>(::INVALID_LEAGUE_TABLE_ELEMENT));
}

/* static */ void ScriptInstance::DoCommandReturnLeagueTableID(ScriptInstance *instance)
{
	instance->engine->InsertResult(ScriptObject::GetLastCommandResultData<LeagueTableID>(::INVALID_LEAGUE_TABLE));
}


ScriptStorage *ScriptInstance::GetStorage()
{
	return this->storage;
}

ScriptLogTypes::LogData &ScriptInstance::GetLogData()
{
	ScriptObject::ActiveInstance active(this);

	return ScriptObject::GetLogData();
}

/*
 * All data is stored in the following format:
 * First 1 byte indicating if there is a data blob at all.
 * 1 byte indicating the type of data.
 * The data itself, this differs per type:
 *  - integer: a binary representation of the integer (int32_t).
 *  - string:  First one byte with the string length, then a 0-terminated char
 *             array. The string can't be longer than 255 bytes (including
 *             terminating '\0').
 *  - array:   All data-elements of the array are saved recursive in this
 *             format, and ended with an element of the type
 *             SQSL_ARRAY_TABLE_END.
 *  - table:   All key/value pairs are saved in this format (first key 1, then
 *             value 1, then key 2, etc.). All keys and values can have an
 *             arbitrary type (as long as it is supported by the save function
 *             of course). The table is ended with an element of the type
 *             SQSL_ARRAY_TABLE_END.
 *  - bool:    A single byte with value 1 representing true and 0 false.
 *  - null:    No data.
 */

/* static */ bool ScriptInstance::SaveObject(HSQUIRRELVM vm, SQInteger index, int max_depth)
{
	if (max_depth == 0) {
		ScriptLog::Error("Savedata can only be nested to 25 deep. No data saved."); // SQUIRREL_MAX_DEPTH = 25
		return false;
	}

	switch (sq_gettype(vm, index)) {
		case OT_INTEGER: {
			SlWriteByte(SQSL_INT);
			SQInteger res;
			sq_getinteger(vm, index, &res);
			SlWriteUint64((int64_t)res);
			return true;
		}

		case OT_STRING: {
			SlWriteByte(SQSL_STRING);
			const SQChar *buf;
			sq_getstring(vm, index, &buf);
			size_t len = strlen(buf) + 1;
			if (len >= 255) {
				ScriptLog::Error("Maximum string length is 254 chars. No data saved.");
				return false;
			}
			SlWriteByte((uint8_t)len);
			SlCopyBytesWrite(buf, len);
			return true;
		}

		case OT_ARRAY: {
			SlWriteByte(SQSL_ARRAY);
			sq_pushnull(vm);
			while (SQ_SUCCEEDED(sq_next(vm, index - 1))) {
				/* Store the value */
				bool res = SaveObject(vm, -1, max_depth - 1);
				sq_pop(vm, 2);
				if (!res) {
					sq_pop(vm, 1);
					return false;
				}
			}
			sq_pop(vm, 1);
			SlWriteByte(SQSL_ARRAY_TABLE_END);
			return true;
		}

		case OT_TABLE: {
			SlWriteByte(SQSL_TABLE);
			sq_pushnull(vm);
			while (SQ_SUCCEEDED(sq_next(vm, index - 1))) {
				/* Store the key + value */
				bool res = SaveObject(vm, -2, max_depth - 1) && SaveObject(vm, -1, max_depth - 1);
				sq_pop(vm, 2);
				if (!res) {
					sq_pop(vm, 1);
					return false;
				}
			}
			sq_pop(vm, 1);
			SlWriteByte(SQSL_ARRAY_TABLE_END);
			return true;
		}

		case OT_BOOL: {
			SlWriteByte(SQSL_BOOL);
			SQBool res;
			sq_getbool(vm, index, &res);
			SlWriteByte(res ? 1 : 0);
			return true;
		}

		case OT_NULL: {
			SlWriteByte(SQSL_NULL);
			return true;
		}

		case OT_INSTANCE:{
			SQInteger top = sq_gettop(vm);
			try {
				ScriptObject *obj = static_cast<ScriptObject *>(Squirrel::GetRealInstance(vm, -1, "Object"));
				if (!obj->SaveObject(vm)) throw std::exception();
				if (sq_gettop(vm) != top + 2) throw std::exception();
				if (sq_gettype(vm, -2) != OT_STRING || !SaveObject(vm, -2, max_depth - 1)) throw std::exception();
				if (!SaveObject(vm, -1, max_depth - 1)) throw std::exception();
				sq_settop(vm, top);
				return true;
			} catch (...) {
				ScriptLog::Error("You tried to save an unsupported type. No data saved.");
				sq_settop(vm, top);
				return false;
			}
		}

		default:
			ScriptLog::Error("You tried to save an unsupported type. No data saved.");
			return false;
	}
}

/* static */ void ScriptInstance::SaveEmpty()
{
	SlWriteByte(0);
}

void ScriptInstance::Save()
{
	ScriptObject::ActiveInstance active(this);

	/* Don't save data if the script didn't start yet or if it crashed. */
	if (this->engine == nullptr || this->engine->HasScriptCrashed()) {
		SaveEmpty();
		return;
	}

	HSQUIRRELVM vm = this->engine->GetVM();
	if (this->is_save_data_on_stack) {
		SlWriteByte(1);
		/* Save the data that was just loaded. */
		SaveObject(vm, -1, SQUIRREL_MAX_DEPTH);
	} else if (!this->is_started) {
		SaveEmpty();
		return;
	} else if (this->engine->MethodExists(*this->instance, "Save")) {
		HSQOBJECT savedata;
		/* We don't want to be interrupted during the save function. */
		bool backup_allow = ScriptObject::GetAllowDoCommand();
		ScriptObject::SetAllowDoCommand(false);
		try {
			if (!this->engine->CallMethod(*this->instance, "Save", &savedata, MAX_SL_OPS)) {
				/* The script crashed in the Save function. We can't kill
				 * it here, but do so in the next script tick. */
				SaveEmpty();
				this->engine->CrashOccurred();
				return;
			}
		} catch (Script_FatalError &e) {
			/* If we don't mark the script as dead here cleaning up the squirrel
			 * stack could throw Script_FatalError again. */
			this->is_dead = true;
			this->engine->ThrowError(e.GetErrorMessage());
			this->engine->ResumeError();
			SaveEmpty();
			/* We can't kill the script here, so mark it as crashed (not dead) and
			 * kill it in the next script tick. */
			this->is_dead = false;
			this->engine->CrashOccurred();
			return;
		}
		ScriptObject::SetAllowDoCommand(backup_allow);

		if (!sq_istable(savedata)) {
			ScriptLog::Error(this->engine->IsSuspended() ? "This script took too long to Save." : "Save function should return a table.");
			SaveEmpty();
			this->engine->CrashOccurred();
			return;
		}
		sq_pushobject(vm, savedata);
		bool saved = SlConditionallySave([&]() {
			SlWriteByte(1);
			return SaveObject(vm, -1, SQUIRREL_MAX_DEPTH);
		});
		if (saved) {
			this->is_save_data_on_stack = true;
		} else {
			SaveEmpty();
			this->engine->CrashOccurred();
		}
	} else {
		ScriptLog::Warning("Save function is not implemented");
		SlWriteByte(0);
	}
}

void ScriptInstance::Pause()
{
	/* Suspend script. */
	HSQUIRRELVM vm = this->engine->GetVM();
	Squirrel::DecreaseOps(vm, this->GetOpsTillSuspend());

	this->is_paused = true;
}

void ScriptInstance::Unpause()
{
	this->is_paused = false;
}

bool ScriptInstance::IsPaused()
{
	return this->is_paused;
}

/* static */ bool ScriptInstance::LoadObjects(ScriptData *data)
{
	uint8_t type = SlReadByte();
	switch (type) {
		case SQSL_INT: {
			int64_t value;
			if (IsSavegameVersionBefore(SLV_SCRIPT_INT64) && SlXvIsFeatureMissing(XSLFI_SCRIPT_INT64)) {
				value = (int32_t)SlReadUint32();
			} else {
				value = (int64_t)SlReadUint64();
			}
			if (data != nullptr) data->push_back(static_cast<SQInteger>(value));
			return true;
		}

		case SQSL_STRING: {
			uint8_t len = SlReadByte();
			char buf[std::numeric_limits<decltype(len)>::max()];
			SlCopyBytesRead(buf, len);
			if (data != nullptr) data->push_back(StrMakeValid(std::string_view(buf, len)));
			return true;
		}

		case SQSL_ARRAY:
		case SQSL_TABLE: {
			if (data != nullptr) data->push_back(static_cast<SQSaveLoadType>(type));
			while (LoadObjects(data));
			return true;
		}

		case SQSL_BOOL: {
			uint8_t sl_byte = SlReadByte();
			if (data != nullptr) data->push_back(static_cast<SQBool>(sl_byte != 0));
			return true;
		}

		case SQSL_NULL: {
			if (data != nullptr) data->push_back(static_cast<SQSaveLoadType>(type));
			return true;
		}

		case SQSL_INSTANCE: {
			if (data != nullptr) data->push_back(static_cast<SQSaveLoadType>(type));
			return true;
		}

		case SQSL_ARRAY_TABLE_END: {
			if (data != nullptr) data->push_back(static_cast<SQSaveLoadType>(type));
			return false;
		}

		default: SlErrorCorrupt("Invalid script data type");
	}
}

/* static */ bool ScriptInstance::LoadObjects(HSQUIRRELVM vm, ScriptData *data)
{
	ScriptDataVariant value = std::move(data->front());
	data->pop_front();

	struct visitor {
		HSQUIRRELVM vm;
		ScriptData *data;

		bool operator()(const SQInteger &value) { sq_pushinteger(this->vm, value); return true; }
		bool operator()(const std::string &value) { sq_pushstring(this->vm, value, -1); return true; }
		bool operator()(const SQBool &value) { sq_pushbool(this->vm, value); return true; }
		bool operator()(const SQSaveLoadType &type)
		{
			switch (type) {
				case SQSL_ARRAY:
					sq_newarray(this->vm, 0);
					while (LoadObjects(this->vm, this->data)) {
						sq_arrayappend(this->vm, -2);
						/* The value is popped from the stack by squirrel. */
					}
					return true;

				case SQSL_TABLE:
					sq_newtable(this->vm);
					while (LoadObjects(this->vm, this->data)) {
						LoadObjects(this->vm, this->data);
						sq_rawset(this->vm, -3);
						/* The key (-2) and value (-1) are popped from the stack by squirrel. */
					}
					return true;

				case SQSL_NULL:
					sq_pushnull(this->vm);
					return true;

				case SQSL_INSTANCE: {
					SQInteger top = sq_gettop(this->vm);
					LoadObjects(this->vm, this->data);
					const SQChar *buf;
					sq_getstring(this->vm, -1, &buf);
					Squirrel *engine = static_cast<Squirrel *>(sq_getforeignptr(this->vm));
					std::string class_name = fmt::format("{}{}", engine->GetAPIName(), buf);
					sq_pushroottable(this->vm);
					sq_pushstring(this->vm, class_name);
					if (SQ_FAILED(sq_get(this->vm, -2))) throw Script_FatalError(fmt::format("'{}' doesn't exist", class_name));
					sq_pushroottable(vm);
					if (SQ_FAILED(sq_call(this->vm, 1, SQTrue, SQFalse))) throw Script_FatalError(fmt::format("Failed to instantiate '{}'", class_name));
					HSQOBJECT res;
					sq_getstackobj(vm, -1, &res);
					sq_addref(vm, &res);
					sq_settop(this->vm, top);
					sq_pushobject(vm, res);
					sq_release(vm, &res);
					ScriptObject *obj = static_cast<ScriptObject *>(Squirrel::GetRealInstance(vm, -1, "Object"));
					LoadObjects(this->vm, this->data);
					if (!obj->LoadObject(vm)) throw Script_FatalError(fmt::format("Failed to load '{}'", class_name));
					sq_pop(this->vm, 1);
					return true;
				}

				case SQSL_ARRAY_TABLE_END:
					return false;

				default: NOT_REACHED();
			}
		}
	};

	return std::visit(visitor{vm, data}, value);
}

/* static */ void ScriptInstance::LoadEmpty()
{
	uint8_t sl_byte = SlReadByte();
	/* Check if there was anything saved at all. */
	if (sl_byte == 0) return;

	LoadObjects(nullptr);
}

/* static */ ScriptInstance::ScriptData *ScriptInstance::Load(int version)
{
	if (version == -1) {
		LoadEmpty();
		return nullptr;
	}

	uint8_t sl_byte = SlReadByte();
	/* Check if there was anything saved at all. */
	if (sl_byte == 0) return nullptr;

	ScriptData *data = new ScriptData();
	data->push_back((SQInteger)version);
	LoadObjects(data);
	return data;
}

void ScriptInstance::LoadOnStack(ScriptData *data)
{
	ScriptObject::ActiveInstance active(this);

	if (this->IsDead() || data == nullptr) return;

	HSQUIRRELVM vm = this->engine->GetVM();

	ScriptDataVariant version = data->front();
	data->pop_front();
	SQInteger top = sq_gettop(vm);
	try {
		sq_pushinteger(vm, std::get<SQInteger>(version));
		LoadObjects(vm, data);
		this->is_save_data_on_stack = true;
	} catch (Script_FatalError &e) {
		ScriptLog::Warning(fmt::format("Loading failed: {}", e.GetErrorMessage()));
		/* Discard partially loaded savegame data and version. */
		sq_settop(vm, top);
	}
}

bool ScriptInstance::CallLoad()
{
	HSQUIRRELVM vm = this->engine->GetVM();
	/* Is there save data that we should load? */
	if (!this->is_save_data_on_stack) return true;
	/* Whatever happens, after CallLoad the savegame data is removed from the stack. */
	this->is_save_data_on_stack = false;

	if (!this->engine->MethodExists(*this->instance, "Load")) {
		ScriptLog::Warning("Loading failed: there was data for the script to load, but the script does not have a Load() function.");

		/* Pop the savegame data and version. */
		sq_pop(vm, 2);
		return true;
	}

	/* Go to the instance-root */
	sq_pushobject(vm, *this->instance);
	/* Find the function-name inside the script */
	sq_pushstring(vm, "Load", -1);
	/* Change the "Load" string in a function pointer */
	sq_get(vm, -2);
	/* Push the main instance as "this" object */
	sq_pushobject(vm, *this->instance);
	/* Push the version data and savegame data as arguments */
	sq_push(vm, -5);
	sq_push(vm, -5);

	/* Call the script load function. sq_call removes the arguments (but not the
	 * function pointer) from the stack. */
	if (SQ_FAILED(sq_call(vm, 3, SQFalse, SQFalse, MAX_SL_OPS))) return false;

	/* Pop 1) The version, 2) the savegame data, 3) the object instance, 4) the function pointer. */
	sq_pop(vm, 4);
	return true;
}

SQInteger ScriptInstance::GetOpsTillSuspend()
{
	return this->engine->GetOpsTillSuspend();
}

void ScriptInstance::LimitOpsTillSuspend(SQInteger suspend)
{
	SQInteger current = this->GetOpsTillSuspend();
	if (suspend < current) {
		/* Reduce script ops. */
		HSQUIRRELVM vm = this->engine->GetVM();
		Squirrel::DecreaseOps(vm, current - suspend);
	}
}

uint32_t ScriptInstance::GetMaxOpsTillSuspend() const
{
	if (this->script_type == ScriptType::GS && (_pause_mode & PM_PAUSED_GAME_SCRIPT) != PM_UNPAUSED) {
		/* Boost opcodes till suspend when paused due to game script */
		return std::min<uint32_t>(250000, _settings_game.script.script_max_opcode_till_suspend * 10);
	}
	return _settings_game.script.script_max_opcode_till_suspend;
}

bool ScriptInstance::DoCommandCallback(const CommandCost &result, Commands cmd, const CommandPayloadBase &payload, CallbackParameter param)
{
	ScriptObject::ActiveInstance active(this);

	if (!ScriptObject::CheckLastCommand(cmd, param)) {
		Debug(script, 1, "DoCommandCallback terminating a script, last command does not match expected command");
		return false;
	}

	ScriptObject::SetLastCommandRes(result.Succeeded());

	if (result.Failed()) {
		ScriptObject::SetLastError(ScriptError::StringToError(result.GetErrorMessage()));
	} else {
		ScriptObject::IncreaseDoCommandCosts(result.GetCost());
		ScriptObject::SetLastCost(result.GetCost());
		ScriptObject::SetLastCommandResultData(result.GetResultData());
	}

	ScriptObject::SetLastCommand(CMD_END, 0);

	return true;
}

void ScriptInstance::InsertEvent(class ScriptEvent *event)
{
	ScriptObject::ActiveInstance active(this);

	ScriptEventController::InsertEvent(event);
}

size_t ScriptInstance::GetAllocatedMemory() const
{
	if (this->engine == nullptr) return this->last_allocated_memory;
	return this->engine->GetAllocatedMemory();
}

void ScriptInstance::SetMemoryAllocationLimit(size_t limit) const
{
	if (this->engine != nullptr) this->engine->SetMemoryAllocationLimit(limit);
}

void ScriptInstance::ReleaseSQObject(HSQOBJECT *obj)
{
	if (!this->in_shutdown) this->engine->ReleaseObject(obj);
}
