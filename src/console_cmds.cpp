/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file console_cmds.cpp Implementation of the console hooks. */

#include "stdafx.h"
#include "console_internal.h"
#include "crashlog.h"
#include "debug.h"
#include "engine_func.h"
#include "landscape.h"
#include "sl/saveload.h"
#include "network/core/network_game_info.h"
#include "network/network.h"
#include "network/network_func.h"
#include "network/network_base.h"
#include "network/network_admin.h"
#include "network/network_client.h"
#include "network/network_server.h"
#include "command_func.h"
#include "command_log.h"
#include "settings_func.h"
#include "fios.h"
#include "fileio_func.h"
#include "fontcache.h"
#include "screenshot.h"
#include "genworld.h"
#include "strings_func.h"
#include "viewport_func.h"
#include "window_func.h"
#include "date_func.h"
#include "timer/timer.h"
#include "timer/timer_game_calendar.h"
#include "company_func.h"
#include "gamelog.h"
#include "ai/ai.hpp"
#include "ai/ai_config.hpp"
#include "newgrf.h"
#include "newgrf_profiling.h"
#include "console_func.h"
#include "engine_base.h"
#include "engine_override.h"
#include "road.h"
#include "rail.h"
#include "game/game.hpp"
#include "table/strings.h"
#include "aircraft.h"
#include "airport.h"
#include "station_base.h"
#include "waypoint_base.h"
#include "waypoint_func.h"
#include "economy_func.h"
#include "town.h"
#include "industry.h"
#include "string_func_extra.h"
#include "linkgraph/linkgraphjob.h"
#include "base_media_base.h"
#include "debug_settings.h"
#include "walltime_func.h"
#include "debug_desync.h"
#include "scope_info.h"
#include "event_logs.h"
#include "tile_cmd.h"
#include "object_base.h"
#include "newgrf_newsignals.h"
#include "roadstop_base.h"
#include "core/backup_type.hpp"
#include "3rdparty/fmt/chrono.h"
#include "company_cmd.h"
#include "misc_cmd.h"
#include "order_backup.h"
#include "cheat_func.h"
#include <time.h>

#include "3rdparty/cpp-btree/btree_set.h"

#include <sstream>

#include "safeguards.h"

/* scriptfile handling */
static uint _script_current_depth; ///< Depth of scripts running (used to abort execution when #ConReturn is encountered).

/* Scheduled execution handling. */
static std::string _scheduled_monthly_script; ///< Script scheduled to execute by the 'schedule' console command (empty if no script is scheduled).

/** Timer that runs every month of game time for the 'schedule' console command. */
static IntervalTimer<TimerGameCalendar> _scheduled_monthly_timer = {{TimerGameCalendar::MONTH, TimerGameCalendar::Priority::NONE}, [](auto) {
	if (_scheduled_monthly_script.empty()) {
		return;
	}

	/* Clear the schedule before rather than after the script to allow the script to itself call
	 * schedule without it getting immediately cleared. */
	const std::string filename = _scheduled_monthly_script;
	_scheduled_monthly_script.clear();

	IConsolePrint(CC_DEFAULT, "Executing scheduled script file '{}'...", filename);
	IConsoleCmdExec(std::string("exec") + " " + filename);
}};

/** File list storage for the console, for caching the last 'ls' command. */
class ConsoleFileList : public FileList {
public:
	ConsoleFileList(AbstractFileType abstract_filetype, bool show_dirs) : FileList(), abstract_filetype(abstract_filetype), show_dirs(show_dirs)
	{
	}

	/** Declare the file storage cache as being invalid, also clears all stored files. */
	void InvalidateFileList()
	{
		this->clear();
		this->file_list_valid = false;
	}

	/**
	 * (Re-)validate the file storage cache. Only makes a change if the storage was invalid, or if \a force_reload.
	 * @param force_reload Always reload the file storage cache.
	 */
	void ValidateFileList(bool force_reload = false)
	{
		if (force_reload || !this->file_list_valid) {
			this->BuildFileList(this->abstract_filetype, SLO_LOAD, this->show_dirs);
			this->file_list_valid = true;
		}
	}

	AbstractFileType abstract_filetype; ///< The abstract file type to list.
	bool show_dirs; ///< Whether to show directories in the file list.
	bool file_list_valid = false; ///< If set, the file list is valid.
};

static ConsoleFileList _console_file_list_savegame{FT_SAVEGAME, true}; ///< File storage cache for savegames.
static ConsoleFileList _console_file_list_scenario{FT_SCENARIO, false}; ///< File storage cache for scenarios.
static ConsoleFileList _console_file_list_heightmap{FT_HEIGHTMAP, false}; ///< File storage cache for heightmaps.

/* console command defines */
#define DEF_CONSOLE_CMD(function) static bool function([[maybe_unused]] uint8_t argc, [[maybe_unused]] char *argv[])
#define DEF_CONSOLE_HOOK(function) static ConsoleHookResult function(bool echo)


/****************
 * command hooks
 ****************/

/**
 * Check network availability and inform in console about failure of detection.
 * @return Network availability.
 */
static inline bool NetworkAvailable(bool echo)
{
	if (!_network_available) {
		if (echo) IConsolePrint(CC_ERROR, "You cannot use this command because there is no network available.");
		return false;
	}
	return true;
}

/**
 * Check whether we are a server.
 * @return Are we a server? True when yes, false otherwise.
 */
DEF_CONSOLE_HOOK(ConHookServerOnly)
{
	if (!NetworkAvailable(echo)) return CHR_DISALLOW;

	if (!_network_server) {
		if (echo) IConsolePrint(CC_ERROR, "This command is only available to a network server.");
		return CHR_DISALLOW;
	}
	return CHR_ALLOW;
}

/**
 * Check whether we are a client in a network game.
 * @return Are we a client in a network game? True when yes, false otherwise.
 */
DEF_CONSOLE_HOOK(ConHookClientOnly)
{
	if (!NetworkAvailable(echo)) return CHR_DISALLOW;

	if (_network_server) {
		if (echo) IConsolePrint(CC_ERROR, "This command is not available to a network server.");
		return CHR_DISALLOW;
	}
	return CHR_ALLOW;
}

/**
 * Check whether we are in a multiplayer game.
 * @return True when we are client or server in a network game.
 */
DEF_CONSOLE_HOOK(ConHookNeedNetwork)
{
	if (!NetworkAvailable(echo)) return CHR_DISALLOW;

	if (!_networking || (!_network_server && !MyClient::IsConnected())) {
		if (echo) IConsolePrint(CC_ERROR, "Not connected. This command is only available in multiplayer.");
		return CHR_DISALLOW;
	}
	return CHR_ALLOW;
}

/**
 * Check whether we are in a multiplayer game and are playing, i.e. we are not the dedicated server, or not in a network game.
 * @return Are we a client or non-dedicated server in a network game, or not in a network game? True when yes, false otherwise.
 */
DEF_CONSOLE_HOOK(ConHookNeedNonDedicatedOrNoNetwork)
{
	if (!_networking) return CHR_ALLOW;

	if (!NetworkAvailable(echo)) return CHR_DISALLOW;

	if (_network_dedicated) {
		if (echo) IConsolePrint(CC_ERROR, "This command is not available to a dedicated network server.");
		return CHR_DISALLOW;
	}
	return CHR_ALLOW;
}

/**
 * Check whether we are in singleplayer mode.
 * @return True when no network is active.
 */
DEF_CONSOLE_HOOK(ConHookNoNetwork)
{
	if (_networking) {
		if (echo) IConsolePrint(CC_ERROR, "This command is forbidden in multiplayer.");
		return CHR_DISALLOW;
	}
	return CHR_ALLOW;
}

/**
 * Check if are either in singleplayer or a server.
 * @return True iff we are either in singleplayer or a server.
 */
DEF_CONSOLE_HOOK(ConHookServerOrNoNetwork)
{
	if (_networking && !_network_server) {
		if (echo) IConsolePrint(CC_ERROR, "This command is only available to a network server, or in single-player.");
		return CHR_DISALLOW;
	}
	return CHR_ALLOW;
}

DEF_CONSOLE_HOOK(ConHookNewGRFDeveloperTool)
{
	if (_settings_client.gui.newgrf_developer_tools) {
		if (_game_mode == GM_MENU) {
			if (echo) IConsolePrint(CC_ERROR, "This command is only available in-game and in the editor.");
			return CHR_DISALLOW;
		}
		return ConHookNoNetwork(echo);
	}
	return CHR_HIDE;
}

DEF_CONSOLE_HOOK(ConHookSpecialCmd)
{
	if (HasBit(_misc_debug_flags, MDF_SPECIAL_CMDS)) {
		return ConHookNoNetwork(echo);
	}
	return CHR_HIDE;
}

/**
 * Reset status of all engines.
 * @return Will always succeed.
 */
DEF_CONSOLE_CMD(ConResetEngines)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Reset status data of all engines. This might solve some issues with 'lost' engines. Usage: 'resetengines'.");
		return true;
	}

	StartupEngines();
	return true;
}

/**
 * Reset status of the engine pool.
 * @return Will always return true.
 * @note Resetting the pool only succeeds when there are no vehicles ingame.
 */
DEF_CONSOLE_CMD(ConResetEnginePool)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Reset NewGRF allocations of engine slots. This will remove invalid engine definitions, and might make default engines available again.");
		return true;
	}

	if (_game_mode == GM_MENU) {
		IConsolePrint(CC_ERROR, "This command is only available in-game and in the editor.");
		return true;
	}

	if (!EngineOverrideManager::ResetToCurrentNewGRFConfig()) {
		IConsolePrint(CC_ERROR, "This can only be done when there are no vehicles in the game.");
		return true;
	}

	return true;
}

#ifdef _DEBUG
/**
 * Reset a tile to bare land in debug mode.
 * param tile number.
 * @return True when the tile is reset or the help on usage was printed (0 or two parameters).
 */
DEF_CONSOLE_CMD(ConResetTile)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Reset a tile to bare land. Usage: 'resettile <tile>'.");
		IConsolePrint(CC_HELP, "Tile can be either decimal (34161) or hexadecimal (0x4a5B).");
		return true;
	}

	if (argc == 2) {
		uint32_t result;
		if (GetArgumentInteger(&result, argv[1])) {
			DoClearSquare((TileIndex)result);
			return true;
		}
	}

	return false;
}
#endif /* _DEBUG */

/**
 * Zoom map to given level.
 * param level As defined by ZoomLevel and as limited by zoom_min/zoom_max from GUISettings.
 * @return True when either console help was shown or a proper amount of parameters given.
 */
DEF_CONSOLE_CMD(ConZoomToLevel)
{
	switch (argc) {
		case 0:
			IConsolePrint(CC_HELP, "Set the current zoom level of the main viewport.");
			IConsolePrint(CC_HELP, "Usage: 'zoomto <level>'.");

			if (ZOOM_LVL_MIN < _settings_client.gui.zoom_min) {
				IConsolePrint(CC_HELP, "The lowest zoom-in level allowed by current client settings is {}.", std::max(ZOOM_LVL_MIN, _settings_client.gui.zoom_min));
			} else {
				IConsolePrint(CC_HELP, "The lowest supported zoom-in level is {}.", std::max(ZOOM_LVL_MIN, _settings_client.gui.zoom_min));
			}

			if (_settings_client.gui.zoom_max < ZOOM_LVL_MAX) {
				IConsolePrint(CC_HELP, "The highest zoom-out level allowed by current client settings is {}.", std::min(_settings_client.gui.zoom_max, ZOOM_LVL_MAX));
			} else {
				IConsolePrint(CC_HELP, "The highest supported zoom-out level is {}.", std::min(_settings_client.gui.zoom_max, ZOOM_LVL_MAX));
			}
			return true;

		case 2: {
			uint32_t level;
			if (GetArgumentInteger(&level, argv[1])) {
				/* In case ZOOM_LVL_MIN is more than 0, the next if statement needs to be amended.
				 * A simple check for less than ZOOM_LVL_MIN does not work here because we are
				 * reading an unsigned integer from the console, so just check for a '-' char. */
				static_assert(ZOOM_LVL_MIN == 0);
				if (argv[1][0] == '-') {
					IConsolePrint(CC_ERROR, "Zoom-in levels below {} are not supported.", ZOOM_LVL_MIN);
				} else if (level < _settings_client.gui.zoom_min) {
					IConsolePrint(CC_ERROR, "Current client settings do not allow zooming in below level {}.", _settings_client.gui.zoom_min);
				} else if (level > ZOOM_LVL_MAX) {
					IConsolePrint(CC_ERROR, "Zoom-in levels above {} are not supported.", ZOOM_LVL_MAX);
				} else if (level > _settings_client.gui.zoom_max) {
					IConsolePrint(CC_ERROR, "Current client settings do not allow zooming out beyond level {}.", _settings_client.gui.zoom_max);
				} else {
					Window *w = GetMainWindow();
					Viewport *vp = w->viewport;
					while (vp->zoom > level) DoZoomInOutWindow(ZOOM_IN, w);
					while (vp->zoom < level) DoZoomInOutWindow(ZOOM_OUT, w);
				}
				return true;
			}
			break;
		}
	}

	return false;
}

/**
 * Scroll to a tile on the map.
 * param x tile number or tile x coordinate.
 * param y optional y coordinate.
 * @note When only one argument is given it is interpreted as the tile number.
 *       When two arguments are given, they are interpreted as the tile's x
 *       and y coordinates.
 * @return True when either console help was shown or a proper amount of parameters given.
 */
DEF_CONSOLE_CMD(ConScrollToTile)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Center the screen on a given tile.");
		IConsolePrint(CC_HELP, "Usage: 'scrollto [instant] <tile>' or 'scrollto [instant] <x> <y>'.");
		IConsolePrint(CC_HELP, "Numbers can be either decimal (34161) or hexadecimal (0x4a5B).");
		IConsolePrint(CC_HELP, "'instant' will immediately move and redraw viewport without smooth scrolling.");
		return true;
	}
	if (argc < 2) return false;

	uint32_t arg_index = 1;
	bool instant = false;
	if (strcmp(argv[arg_index], "instant") == 0) {
		++arg_index;
		instant = true;
	}

	switch (argc - arg_index) {
		case 1: {
			uint32_t result;
			if (GetArgumentInteger(&result, argv[arg_index])) {
				if (result >= Map::Size()) {
					IConsolePrint(CC_ERROR, "Tile does not exist.");
					return true;
				}
				ScrollMainWindowToTile((TileIndex)result, instant);
				return true;
			}
			break;
		}

		case 2: {
			uint32_t x, y;
			if (GetArgumentInteger(&x, argv[arg_index]) && GetArgumentInteger(&y, argv[arg_index + 1])) {
				if (x >= Map::SizeX() || y >= Map::SizeY()) {
					IConsolePrint(CC_ERROR, "Tile does not exist.");
					return true;
				}
				ScrollMainWindowToTile(TileXY(x, y), instant);
				return true;
			}
			break;
		}
	}

	return false;
}

/**
 * Highlight a tile on the map.
 * param x tile number or tile x coordinate.
 * param y optional y coordinate.
 * @note When only one argument is given it is interpreted as the tile number.
 *       When two arguments are given, they are interpreted as the tile's x
 *       and y coordinates.
 * @return True when either console help was shown or a proper amount of parameters given.
 */
DEF_CONSOLE_CMD(ConHighlightTile)
{
	switch (argc) {
		case 0:
			IConsolePrint(CC_HELP, "Highlight a given tile.");
			IConsolePrint(CC_HELP, "Usage: 'highlight_tile <tile>' or 'highlight_tile <x> <y>'");
			IConsolePrint(CC_HELP, "Numbers can be either decimal (34161) or hexadecimal (0x4a5B).");
			return true;

		case 2: {
			uint32_t result;
			if (GetArgumentInteger(&result, argv[1])) {
				if (result >= Map::Size()) {
					IConsolePrint(CC_ERROR, "Tile does not exist.");
					return true;
				}
				SetRedErrorSquare((TileIndex)result);
				return true;
			}
			break;
		}

		case 3: {
			uint32_t x, y;
			if (GetArgumentInteger(&x, argv[1]) && GetArgumentInteger(&y, argv[2])) {
				if (x >= Map::SizeX() || y >= Map::SizeY()) {
					IConsolePrint(CC_ERROR, "Tile does not exist.");
					return true;
				}
				SetRedErrorSquare(TileXY(x, y));
				return true;
			}
			break;
		}
	}

	return false;
}

/**
 * Save the map to a file.
 * param filename the filename to save the map to.
 * @return True when help was displayed or the file attempted to be saved.
 */
DEF_CONSOLE_CMD(ConSave)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Save the current game. Usage: 'save <filename>'.");
		return true;
	}

	if (argc == 2) {
		std::string filename = argv[1];
		filename += ".sav";
		IConsolePrint(CC_DEFAULT, "Saving map...");

		if (SaveOrLoad(filename, SLO_SAVE, DFT_GAME_FILE, SAVE_DIR) != SL_OK) {
			IConsolePrint(CC_ERROR, "Saving map failed.");
		} else {
			IConsolePrint(CC_INFO, "Map successfully saved to '{}'.", filename);
		}
		return true;
	}

	return false;
}

/**
 * Explicitly save the configuration.
 * @return True.
 */
DEF_CONSOLE_CMD(ConSaveConfig)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Saves the configuration for new games to the configuration file, typically 'openttd.cfg'.");
		IConsolePrint(CC_HELP, "It does not save the configuration of the current game to the configuration file.");
		return true;
	}

	SaveToConfig(STCF_ALL);
	IConsolePrint(CC_DEFAULT, "Saved config.");
	return true;
}

DEF_CONSOLE_CMD(ConLoad)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Load a game by name or index. Usage: 'load <file | number>'.");
		return true;
	}

	if (argc != 2) return false;

	const char *file = argv[1];
	_console_file_list_savegame.ValidateFileList();
	const FiosItem *item = _console_file_list_savegame.FindItem(file);
	if (item != nullptr) {
		if (GetAbstractFileType(item->type) == FT_SAVEGAME) {
			_switch_mode = SM_LOAD_GAME;
			_file_to_saveload.Set(*item);
		} else {
			IConsolePrint(CC_ERROR, "'{}' is not a savegame.", file);
		}
	} else {
		IConsolePrint(CC_ERROR, "'{}' cannot be found.", file);
	}

	return true;
}

DEF_CONSOLE_CMD(ConLoadScenario)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Load a scenario by name or index. Usage: 'load_scenario <file | number>'.");
		return true;
	}

	if (argc != 2) return false;

	const char *file = argv[1];
	_console_file_list_scenario.ValidateFileList();
	const FiosItem *item = _console_file_list_scenario.FindItem(file);
	if (item != nullptr) {
		if (GetAbstractFileType(item->type) == FT_SCENARIO) {
			_switch_mode = SM_LOAD_GAME;
			_file_to_saveload.Set(*item);
		} else {
			IConsolePrint(CC_ERROR, "'{}' is not a scenario.", file);
		}
	} else {
		IConsolePrint(CC_ERROR, "'{}' cannot be found.", file);
	}

	return true;
}

DEF_CONSOLE_CMD(ConLoadHeightmap)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Load a heightmap by name or index. Usage: 'load_heightmap <file | number>'.");
		return true;
	}

	if (argc != 2) return false;

	const char *file = argv[1];
	_console_file_list_heightmap.ValidateFileList();
	const FiosItem *item = _console_file_list_heightmap.FindItem(file);
	if (item != nullptr) {
		if (GetAbstractFileType(item->type) == FT_HEIGHTMAP) {
			_switch_mode = SM_START_HEIGHTMAP;
			_file_to_saveload.Set(*item);
		} else {
			IConsolePrint(CC_ERROR, "'{}' is not a heightmap.", file);
		}
	} else {
		IConsolePrint(CC_ERROR, "'{}' cannot be found.", file);
	}

	return true;
}

DEF_CONSOLE_CMD(ConRemove)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Remove a savegame by name or index. Usage: 'rm <file | number>'.");
		return true;
	}

	if (argc != 2) return false;

	const char *file = argv[1];
	_console_file_list_savegame.ValidateFileList();
	const FiosItem *item = _console_file_list_savegame.FindItem(file);
	if (item != nullptr) {
		if (GetAbstractFileType(item->type) == FT_SAVEGAME) {
			if (!FioRemove(item->name)) {
				IConsolePrint(CC_ERROR, "Failed to delete '{}'.", item->name);
			}
		} else {
			IConsolePrint(CC_ERROR, "'{}' is not a savegame.", file);
		}
	} else {
		IConsolePrint(CC_ERROR, "'{}' could not be found.", file);
	}

	_console_file_list_savegame.InvalidateFileList();
	return true;
}


/* List all the files in the current dir via console */
DEF_CONSOLE_CMD(ConListFiles)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "List all loadable savegames and directories in the current dir via console. Usage: 'ls | dir'.");
		return true;
	}

	_console_file_list_savegame.ValidateFileList(true);
	for (uint i = 0; i < _console_file_list_savegame.size(); i++) {
		IConsolePrint(CC_DEFAULT, "{}) {}", i, _console_file_list_savegame[i].title);
	}

	return true;
}

/* List all the scenarios */
DEF_CONSOLE_CMD(ConListScenarios)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "List all loadable scenarios. Usage: 'list_scenarios'.");
		return true;
	}

	_console_file_list_scenario.ValidateFileList(true);
	for (uint i = 0; i < _console_file_list_scenario.size(); i++) {
		IConsolePrint(CC_DEFAULT, "{}) {}", i, _console_file_list_scenario[i].title);
	}

	return true;
}

/* List all the heightmaps */
DEF_CONSOLE_CMD(ConListHeightmaps)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "List all loadable heightmaps. Usage: 'list_heightmaps'.");
		return true;
	}

	_console_file_list_heightmap.ValidateFileList(true);
	for (uint i = 0; i < _console_file_list_heightmap.size(); i++) {
		IConsolePrint(CC_DEFAULT, "{}) {}", i, _console_file_list_heightmap[i].title);
	}

	return true;
}

/* Change the dir via console */
DEF_CONSOLE_CMD(ConChangeDirectory)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Change the dir via console. Usage: 'cd <directory | number>'.");
		return true;
	}

	if (argc != 2) return false;

	const char *file = argv[1];
	_console_file_list_savegame.ValidateFileList(true);
	const FiosItem *item = _console_file_list_savegame.FindItem(file);
	if (item != nullptr) {
		switch (item->type) {
			case FIOS_TYPE_DIR: case FIOS_TYPE_DRIVE: case FIOS_TYPE_PARENT:
				FiosBrowseTo(item);
				break;
			default: IConsolePrint(CC_ERROR, "{}: Not a directory.", file);
		}
	} else {
		IConsolePrint(CC_ERROR, "{}: No such file or directory.", file);
	}

	_console_file_list_savegame.InvalidateFileList();
	return true;
}

DEF_CONSOLE_CMD(ConPrintWorkingDirectory)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Print out the current working directory. Usage: 'pwd'.");
		return true;
	}

	/* XXX - Workaround for broken file handling */
	_console_file_list_savegame.ValidateFileList(true);
	_console_file_list_savegame.InvalidateFileList();

	IConsolePrint(CC_DEFAULT, FiosGetCurrentPath());
	return true;
}

DEF_CONSOLE_CMD(ConClearBuffer)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Clear the console buffer. Usage: 'clear'.");
		return true;
	}

	IConsoleClearBuffer();
	SetWindowDirty(WC_CONSOLE, 0);
	return true;
}


/**********************************
 * Network Core Console Commands
 **********************************/

static bool ConKickOrBan(const char *argv, bool ban, const std::string &reason)
{
	uint n;

	if (strchr(argv, '.') == nullptr && strchr(argv, ':') == nullptr) { // banning with ID
		ClientID client_id = (ClientID)atoi(argv);

		/* Don't kill the server, or the client doing the rcon. The latter can't be kicked because
		 * kicking frees closes and subsequently free the connection related instances, which we
		 * would be reading from and writing to after returning. So we would read or write data
		 * from freed memory up till the segfault triggers. */
		if (client_id == CLIENT_ID_SERVER || client_id == _redirect_console_to_client) {
			IConsolePrint(CC_ERROR, "You can not {} yourself!", ban ? "ban" : "kick");
			return true;
		}

		NetworkClientInfo *ci = NetworkClientInfo::GetByClientID(client_id);
		if (ci == nullptr) {
			IConsolePrint(CC_ERROR, "Invalid client ID.");
			return true;
		}

		if (!ban) {
			/* Kick only this client, not all clients with that IP */
			NetworkServerKickClient(client_id, reason);
			return true;
		}

		/* When banning, kick+ban all clients with that IP */
		n = NetworkServerKickOrBanIP(client_id, ban, reason);
	} else {
		n = NetworkServerKickOrBanIP(argv, ban, reason);
	}

	if (n == 0) {
		IConsolePrint(CC_DEFAULT, ban ? "Client not online, address added to banlist." : "Client not found.");
	} else {
		IConsolePrint(CC_DEFAULT, "{}ed {} client(s).", ban ? "Bann" : "Kick", n);
	}

	return true;
}

DEF_CONSOLE_CMD(ConKick)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Kick a client from a network game. Usage: 'kick <ip | client-id> [<kick-reason>]'.");
		IConsolePrint(CC_HELP, "For client-id's, see the command 'clients'.");
		return true;
	}

	if (argc != 2 && argc != 3) return false;

	/* No reason supplied for kicking */
	if (argc == 2) return ConKickOrBan(argv[1], false, {});

	/* Reason for kicking supplied */
	size_t kick_message_length = strlen(argv[2]);
	if (kick_message_length >= 255) {
		IConsolePrint(CC_ERROR, "Maximum kick message length is 254 characters. You entered {} characters.", kick_message_length);
		return false;
	} else {
		return ConKickOrBan(argv[1], false, argv[2]);
	}
}

DEF_CONSOLE_CMD(ConBan)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Ban a client from a network game. Usage: 'ban <ip | client-id> [<ban-reason>]'.");
		IConsolePrint(CC_HELP, "For client-id's, see the command 'clients'.");
		IConsolePrint(CC_HELP, "If the client is no longer online, you can still ban their IP.");
		return true;
	}

	if (argc != 2 && argc != 3) return false;

	/* No reason supplied for kicking */
	if (argc == 2) return ConKickOrBan(argv[1], true, {});

	/* Reason for kicking supplied */
	size_t kick_message_length = strlen(argv[2]);
	if (kick_message_length >= 255) {
		IConsolePrint(CC_ERROR, "Maximum kick message length is 254 characters. You entered {} characters.", kick_message_length);
		return false;
	} else {
		return ConKickOrBan(argv[1], true, argv[2]);
	}
}

DEF_CONSOLE_CMD(ConUnBan)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Unban a client from a network game. Usage: 'unban <ip | banlist-index>'.");
		IConsolePrint(CC_HELP, "For a list of banned IP's, see the command 'banlist'.");
		return true;
	}

	if (argc != 2) return false;

	/* Try by IP. */
	uint index;
	for (index = 0; index < _network_ban_list.size(); index++) {
		if (_network_ban_list[index] == argv[1]) break;
	}

	/* Try by index. */
	if (index >= _network_ban_list.size()) {
		index = atoi(argv[1]) - 1U; // let it wrap
	}

	if (index < _network_ban_list.size()) {
		IConsolePrint(CC_DEFAULT, "Unbanned {}.", _network_ban_list[index]);
		_network_ban_list.erase(_network_ban_list.begin() + index);
	} else {
		IConsolePrint(CC_DEFAULT, "Invalid list index or IP not in ban-list.");
		IConsolePrint(CC_DEFAULT, "For a list of banned IP's, see the command 'banlist'.");
	}

	return true;
}

DEF_CONSOLE_CMD(ConBanList)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "List the IP's of banned clients: Usage 'banlist'.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "Banlist:");

	uint i = 1;
	for (const auto &entry : _network_ban_list) {
		IConsolePrint(CC_DEFAULT, "  {}) {}", i, entry);
		i++;
	}

	return true;
}

DEF_CONSOLE_CMD(ConPauseGame)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Pause a network game. Usage: 'pause'.");
		return true;
	}

	if (_game_mode == GM_MENU) {
		IConsolePrint(CC_ERROR, "This command is only available in-game and in the editor.");
		return true;
	}

	if ((_pause_mode & PM_PAUSED_NORMAL) == PM_UNPAUSED) {
		Command<CMD_PAUSE>::Post(PM_PAUSED_NORMAL, true);
		if (!_networking) IConsolePrint(CC_DEFAULT, "Game paused.");
	} else {
		IConsolePrint(CC_DEFAULT, "Game is already paused.");
	}

	return true;
}

DEF_CONSOLE_CMD(ConUnpauseGame)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Unpause a network game. Usage: 'unpause'.");
		return true;
	}

	if (_game_mode == GM_MENU) {
		IConsolePrint(CC_ERROR, "This command is only available in-game and in the editor.");
		return true;
	}

	if ((_pause_mode & PM_PAUSED_NORMAL) != PM_UNPAUSED) {
		Command<CMD_PAUSE>::Post(PM_PAUSED_NORMAL, false);
		if (!_networking) IConsolePrint(CC_DEFAULT, "Game unpaused.");
	} else if ((_pause_mode & PM_PAUSED_ERROR) != PM_UNPAUSED) {
		IConsolePrint(CC_DEFAULT, "Game is in error state and cannot be unpaused via console.");
	} else if (_pause_mode != PM_UNPAUSED) {
		IConsolePrint(CC_DEFAULT, "Game cannot be unpaused manually; disable pause_on_join/min_active_clients.");
	} else {
		IConsolePrint(CC_DEFAULT, "Game is already unpaused.");
	}

	return true;
}

DEF_CONSOLE_CMD(ConStepGame)
{
	if (argc == 0 || argc > 2) {
		IConsolePrint(CC_HELP, "Advances the game for a certain amount of ticks (default 1). Usage: 'step [n]'");
		return true;
	}
	auto n = (argc > 1 ? atoi(argv[1]) : 1);

	extern void UnpauseStepGame(uint32_t steps);
	UnpauseStepGame(n);

	return true;
}

DEF_CONSOLE_CMD(ConRcon)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Remote control the server from another client. Usage: 'rcon <password> <command>'.");
		IConsolePrint(CC_HELP, "Remember to enclose the command in quotes, otherwise only the first parameter is sent.");
		IConsolePrint(CC_HELP, "When your client's public key is in the 'authorized keys' for 'rcon', '*' may be used instead of the password.");
		return true;
	}

	if (argc < 3) return false;

	if (_network_server) {
		IConsoleCmdExec(argv[2]);
	} else {
		NetworkClientSendRcon(argv[1], argv[2]);
	}
	return true;
}

DEF_CONSOLE_CMD(ConSettingsAccess)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Enable changing game settings from this client. Usage: 'settings_access <password>'");
		IConsolePrint(CC_HELP, "Send an empty password \"\" to drop access");
		IConsolePrint(CC_HELP, "When your client's public key is in the 'authorized keys' for 'settings', the password is not checked and may be '*'.");
		return true;
	}

	if (argc < 2) return false;

	if (!_network_server) {
		NetworkClientSendSettingsPassword(argv[1]);
	}
	return true;
}

DEF_CONSOLE_CMD(ConStatus)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "List the status of all clients connected to the server. Usage 'status'.");
		return true;
	}

	NetworkServerShowStatusToConsole();
	return true;
}

DEF_CONSOLE_CMD(ConServerInfo)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "List current and maximum client/company limits. Usage 'server_info'.");
		IConsolePrint(CC_HELP, "You can change these values by modifying settings 'network.max_clients' and 'network.max_companies'.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "Invite code:                {}", _network_server_invite_code);
	IConsolePrint(CC_DEFAULT, "Current/maximum clients:    {:3d}/{:3d}", _network_game_info.clients_on, _settings_client.network.max_clients);
	IConsolePrint(CC_DEFAULT, "Current/maximum companies:  {:3d}/{:3d}", Company::GetNumItems(), _settings_client.network.max_companies);
	IConsolePrint(CC_DEFAULT, "Current spectators:         {:3d}", NetworkSpectatorCount());

	return true;
}

DEF_CONSOLE_CMD(ConClientNickChange)
{
	if (argc != 3) {
		IConsolePrint(CC_HELP, "Change the nickname of a connected client. Usage: 'client_name <client-id> <new-name>'.");
		IConsolePrint(CC_HELP, "For client-id's, see the command 'clients'.");
		return true;
	}

	ClientID client_id = (ClientID)atoi(argv[1]);

	if (client_id == CLIENT_ID_SERVER) {
		IConsolePrint(CC_ERROR, "Please use the command 'name' to change your own name!");
		return true;
	}

	if (NetworkClientInfo::GetByClientID(client_id) == nullptr) {
		IConsolePrint(CC_ERROR, "Invalid client ID.");
		return true;
	}

	std::string client_name(argv[2]);
	StrTrimInPlace(client_name);
	if (!NetworkIsValidClientName(client_name)) {
		IConsolePrint(CC_ERROR, "Cannot give a client an empty name.");
		return true;
	}

	if (!NetworkServerChangeClientName(client_id, client_name)) {
		IConsolePrint(CC_ERROR, "Cannot give a client a duplicate name.");
	}

	return true;
}

DEF_CONSOLE_CMD(ConJoinCompany)
{
	if (argc < 2) {
		IConsolePrint(CC_HELP, "Request joining another company. Usage: 'join <company-id> [<password>]'.");
		IConsolePrint(CC_HELP, "For valid company-id see company list, use 255 for spectator.");
		return true;
	}

	CompanyID company_id = (CompanyID)(atoi(argv[1]) <= MAX_COMPANIES ? atoi(argv[1]) - 1 : atoi(argv[1]));

	if (!_networking) {
		/* Check we have a valid company id! */
		if (!Company::IsValidID(company_id)) {
			IConsolePrint(CC_ERROR, "Company does not exist. Company-id must be between 1 and {}.", MAX_COMPANIES);
			return true;
		}

		OrderBackup::Reset();
		SetLocalCompany(company_id);
		_cheats.switch_company.been_used = true;
		return true;
	}

	const NetworkClientInfo *info = NetworkClientInfo::GetByClientID(_network_own_client_id);
	if (info == nullptr) {
		IConsolePrint(CC_ERROR, "You have not joined the game yet!");
		return true;
	}

	/* Check we have a valid company id! */
	if (!Company::IsValidID(company_id) && company_id != COMPANY_SPECTATOR) {
		IConsolePrint(CC_ERROR, "Company does not exist. Company-id must be between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	if (info->client_playas == company_id) {
		IConsolePrint(CC_ERROR, "You are already there!");
		return true;
	}

	if (company_id != COMPANY_SPECTATOR && !Company::IsHumanID(company_id)) {
		IConsolePrint(CC_ERROR, "Cannot join AI company.");
		return true;
	}

	/* Check if the company requires a password */
	if (NetworkCompanyIsPassworded(company_id) && argc < 3) {
		IConsolePrint(CC_ERROR, "Company {} requires a password to join.", company_id + 1);
		return true;
	}

	/* non-dedicated server may just do the move! */
	if (_network_server) {
		NetworkServerDoMove(CLIENT_ID_SERVER, company_id);
	} else {
		NetworkClientRequestMove(company_id, NetworkCompanyIsPassworded(company_id) ? argv[2] : "");
	}

	return true;
}

DEF_CONSOLE_CMD(ConMoveClient)
{
	if (argc < 3) {
		IConsolePrint(CC_HELP, "Move a client to another company. Usage: 'move <client-id> <company-id>'.");
		IConsolePrint(CC_HELP, "For valid client-id see 'clients', for valid company-id see 'companies', use 255 for moving to spectators.");
		return true;
	}

	const NetworkClientInfo *ci = NetworkClientInfo::GetByClientID((ClientID)atoi(argv[1]));
	CompanyID company_id = (CompanyID)(atoi(argv[2]) <= MAX_COMPANIES ? atoi(argv[2]) - 1 : atoi(argv[2]));

	/* check the client exists */
	if (ci == nullptr) {
		IConsolePrint(CC_ERROR, "Invalid client-id, check the command 'clients' for valid client-id's.");
		return true;
	}

	if (!Company::IsValidID(company_id) && company_id != COMPANY_SPECTATOR) {
		IConsolePrint(CC_ERROR, "Company does not exist. Company-id must be between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	if (company_id != COMPANY_SPECTATOR && !Company::IsHumanID(company_id)) {
		IConsolePrint(CC_ERROR, "You cannot move clients to AI companies.");
		return true;
	}

	if (ci->client_id == CLIENT_ID_SERVER && _network_dedicated) {
		IConsolePrint(CC_ERROR, "You cannot move the server!");
		return true;
	}

	if (ci->client_playas == company_id) {
		IConsolePrint(CC_ERROR, "You cannot move someone to where they already are!");
		return true;
	}

	/* we are the server, so force the update */
	NetworkServerDoMove(ci->client_id, company_id);

	return true;
}

DEF_CONSOLE_CMD(ConResetCompany)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Remove an idle company from the game. Usage: 'reset_company <company-id>'.");
		IConsolePrint(CC_HELP, "For company-id's, see the list of companies from the dropdown menu. Company 1 is 1, etc.");
		return true;
	}

	if (argc != 2) return false;

	CompanyID index = (CompanyID)(atoi(argv[1]) - 1);

	/* Check valid range */
	if (!Company::IsValidID(index)) {
		IConsolePrint(CC_ERROR, "Company does not exist. Company-id must be between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	if (!Company::IsHumanID(index)) {
		IConsolePrint(CC_ERROR, "Company is owned by an AI.");
		return true;
	}

	if (NetworkCompanyHasClients(index)) {
		IConsolePrint(CC_ERROR, "Cannot remove company: a client is connected to that company.");
		return false;
	}
	const NetworkClientInfo *ci = NetworkClientInfo::GetByClientID(CLIENT_ID_SERVER);
	assert(ci != nullptr);
	if (ci->client_playas == index) {
		IConsolePrint(CC_ERROR, "Cannot remove company: the server is connected to that company.");
		return true;
	}

	/* It is safe to remove this company */
	Command<CMD_COMPANY_CTRL>::Post(CCA_DELETE, index, CRR_MANUAL, INVALID_CLIENT_ID, {});
	IConsolePrint(CC_DEFAULT, "Company deleted.");

	return true;
}

DEF_CONSOLE_CMD(ConOfferCompanySale)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Offer a company for sale. Usage: 'offer_company_sale <company-id>'");
		IConsolePrint(CC_HELP, "For company-id's, see the list of companies from the dropdown menu. Company 1 is 1, etc.");
		return true;
	}

	if (argc != 2) return false;

	CompanyID index = (CompanyID)(atoi(argv[1]) - 1);

	/* Check valid range */
	if (!Company::IsValidID(index)) {
		IConsolePrint(CC_ERROR, "Company does not exist. Company-id must be between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	Command<CMD_COMPANY_CTRL>::Post(CCA_SALE, index, CRR_NONE, INVALID_CLIENT_ID, {});
	IConsolePrint(CC_DEFAULT, "Company offered for sale.");

	return true;
}

DEF_CONSOLE_CMD(ConMergeCompanies)
{
	if (argc != 3) {
		IConsolePrint(CC_HELP, "Merge two companies together. Usage: 'merge_companies <main-company-id> <to-merge-company-id>'");
		IConsolePrint(CC_HELP, "The first company ID <main-company-id> will be left with the combined assets of both companies.");
		IConsolePrint(CC_HELP, "The second company ID <to-merge-company-id> will be removed, with all assets transferred to the first company ID.");
		IConsolePrint(CC_HELP, "For company-id's, see the list of companies from the dropdown menu. Company 1 is 1, etc.");
		return true;
	}

	CompanyID main_company = (CompanyID)(atoi(argv[1]) - 1);
	CompanyID to_merge_company = (CompanyID)(atoi(argv[2]) - 1);

	/* Check valid range */
	if (!Company::IsValidID(main_company)) {
		IConsolePrint(CC_ERROR, "Main company does not exist. Company-id must be between 1 and {}.", MAX_COMPANIES);
		return true;
	}
	if (!Company::IsValidID(to_merge_company)) {
		IConsolePrint(CC_ERROR, "Company to merge does not exist. Company-id must be between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	Command<CMD_COMPANY_CTRL>::Post(CCA_MERGE, main_company, CRR_NONE, INVALID_CLIENT_ID, to_merge_company);
	IConsolePrint(CC_DEFAULT, "Companies merged.");

	return true;
}

DEF_CONSOLE_CMD(ConNetworkClients)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Get a list of connected clients including their ID, name, company-id, and IP. Usage: 'clients'.");
		return true;
	}

	NetworkPrintClients();

	return true;
}

DEF_CONSOLE_CMD(ConNetworkReconnect)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Reconnect to server to which you were connected last time. Usage: 'reconnect [<company>]'.");
		IConsolePrint(CC_HELP, "Company 255 is spectator (default, if not specified), 0 means creating new company.");
		IConsolePrint(CC_HELP, "All others are a certain company with Company 1 being #1.");
		return true;
	}

	CompanyID playas = (argc >= 2) ? (CompanyID)atoi(argv[1]) : COMPANY_SPECTATOR;
	switch (playas) {
		case 0: playas = COMPANY_NEW_COMPANY; break;
		case COMPANY_SPECTATOR: /* nothing to do */ break;
		default:
			/* From a user pov 0 is a new company, internally it's different and all
			 * companies are offset by one to ease up on users (eg companies 1-8 not 0-7) */
			if (playas < COMPANY_FIRST + 1 || playas > MAX_COMPANIES + 1) return false;
			break;
	}

	if (_settings_client.network.last_joined.empty()) {
		IConsolePrint(CC_DEFAULT, "No server for reconnecting.");
		return true;
	}

	/* Don't resolve the address first, just print it directly as it comes from the config file. */
	IConsolePrint(CC_DEFAULT, "Reconnecting to {} ...", _settings_client.network.last_joined);

	return NetworkClientConnectGame(_settings_client.network.last_joined, playas);
}

DEF_CONSOLE_CMD(ConNetworkConnect)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Connect to a remote OTTD server and join the game. Usage: 'connect <ip>'.");
		IConsolePrint(CC_HELP, "IP can contain port and company: 'IP[:Port][#Company]', eg: 'server.ottd.org:443#2'.");
		IConsolePrint(CC_HELP, "Company #255 is spectator all others are a certain company with Company 1 being #1.");
		return true;
	}

	if (argc < 2) return false;

	return NetworkClientConnectGame(argv[1], COMPANY_NEW_COMPANY);
}

/*********************************
 *  script file console commands
 *********************************/

DEF_CONSOLE_CMD(ConExec)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Execute a local script file. Usage: 'exec <script> <?>'.");
		return true;
	}

	if (argc < 2) return false;

	auto script_file = FioFOpenFile(argv[1], "r", BASE_DIR);

	if (!script_file.has_value()) {
		if (argc == 2 || atoi(argv[2]) != 0) IConsolePrint(CC_ERROR, "Script file '{}' not found.", argv[1]);
		return true;
	}

	if (_script_current_depth == 11) {
		IConsolePrint(CC_ERROR, "Maximum 'exec' depth reached; script A is calling script B is calling script C ... more than 10 times.");
		return true;
	}

	_script_current_depth++;
	uint script_depth = _script_current_depth;

	char cmdline[ICON_CMDLN_SIZE];
	while (fgets(cmdline, sizeof(cmdline), *script_file) != nullptr) {
		/* Remove newline characters from the executing script */
		for (char *cmdptr = cmdline; *cmdptr != '\0'; cmdptr++) {
			if (*cmdptr == '\n' || *cmdptr == '\r') {
				*cmdptr = '\0';
				break;
			}
		}
		IConsoleCmdExec(cmdline);
		/* Ensure that we are still on the same depth or that we returned via 'return'. */
		assert(_script_current_depth == script_depth || _script_current_depth == script_depth - 1);

		/* The 'return' command was executed. */
		if (_script_current_depth == script_depth - 1) break;
	}

	if (ferror(*script_file) != 0) {
		IConsolePrint(CC_ERROR, "Encountered error while trying to read from script file '{}'.", argv[1]);
	}

	if (_script_current_depth == script_depth) _script_current_depth--;
	return true;
}

DEF_CONSOLE_CMD(ConSchedule)
{
	if (argc < 3 || std::string_view(argv[1]) != "on-next-calendar-month") {
		IConsolePrint(CC_HELP, "Schedule a local script to execute later. Usage: 'schedule on-next-calendar-month <script>'.");
		return true;
	}

	/* Check if the file exists. It might still go away later, but helpful to show an error now. */
	if (!FioCheckFileExists(argv[2], BASE_DIR)) {
		IConsolePrint(CC_ERROR, "Script file '{}' not found.", argv[2]);
		return true;
	}

	/* We only support a single script scheduled, so we tell the user what's happening if there was already one. */
	const std::string_view filename = std::string_view(argv[2]);
	if (!_scheduled_monthly_script.empty() && filename == _scheduled_monthly_script) {
		IConsolePrint(CC_INFO, "Script file '{}' was already scheduled to execute at the start of next calendar month.", filename);
	} else if (!_scheduled_monthly_script.empty() && filename != _scheduled_monthly_script) {
		IConsolePrint(CC_INFO, "Script file '{}' scheduled to execute at the start of next calendar month, replacing the previously scheduled script file '{}'.", filename, _scheduled_monthly_script);
	} else {
		IConsolePrint(CC_INFO, "Script file '{}' scheduled to execute at the start of next calendar month.", filename);
	}

	/* Store the filename to be used by _schedule_timer on the start of next calendar month. */
	_scheduled_monthly_script = filename;

	return true;
}

DEF_CONSOLE_CMD(ConReturn)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Stop executing a running script. Usage: 'return'.");
		return true;
	}

	_script_current_depth--;
	return true;
}

/*****************************
 *  default console commands
 ******************************/
extern bool CloseConsoleLogIfActive();
extern const std::vector<GRFFile *> &GetAllGRFFiles();
extern void ConPrintFramerate(); // framerate_gui.cpp
extern void ShowFramerateWindow();

DEF_CONSOLE_CMD(ConScript)
{
	extern std::optional<FileHandle> _iconsole_output_file;

	if (argc == 0) {
		IConsolePrint(CC_HELP, "Start or stop logging console output to a file. Usage: 'script <filename>'.");
		IConsolePrint(CC_HELP, "If filename is omitted, a running log is stopped if it is active.");
		return true;
	}

	if (!CloseConsoleLogIfActive()) {
		if (argc < 2) return false;

		_iconsole_output_file = FileHandle::Open(argv[1], "ab");
		if (!_iconsole_output_file.has_value()) {
			IConsolePrint(CC_ERROR, "Could not open console log file '{}'.", argv[1]);
		} else {
			IConsolePrint(CC_INFO, "Console log output started to '{}'.", argv[1]);
		}
	}

	return true;
}


DEF_CONSOLE_CMD(ConEcho)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Print back the first argument to the console. Usage: 'echo <arg>'.");
		return true;
	}

	if (argc < 2) return false;
	IConsolePrint(CC_DEFAULT, argv[1]);
	return true;
}

DEF_CONSOLE_CMD(ConEchoC)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Print back the first argument to the console in a given colour. Usage: 'echoc <colour> <arg2>'.");
		return true;
	}

	if (argc < 3) return false;
	IConsolePrint((TextColour)Clamp(atoi(argv[1]), TC_BEGIN, TC_END - 1), argv[2]);
	return true;
}

DEF_CONSOLE_CMD(ConNewGame)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Start a new game. Usage: 'newgame [seed]'.");
		IConsolePrint(CC_HELP, "The server can force a new game using 'newgame'; any client joined will rejoin after the server is done generating the new game.");
		return true;
	}

	StartNewGameWithoutGUI((argc == 2) ? std::strtoul(argv[1], nullptr, 10) : GENERATE_NEW_SEED);
	return true;
}

DEF_CONSOLE_CMD(ConRestart)
{
	if (argc == 0 || argc > 2) {
		IConsolePrint(CC_HELP, "Restart game. Usage: 'restart [current|newgame]'.");
		IConsolePrint(CC_HELP, "Restarts a game, using either the current or newgame (default) settings.");
		IConsolePrint(CC_HELP, " * if you started from a new game, and your current/newgame settings haven't changed, the game will be identical to when you started it.");
		IConsolePrint(CC_HELP, " * if you started from a savegame / scenario / heightmap, the game might be different, because the current/newgame settings might differ.");
		return true;
	}

	if (argc == 1 || std::string_view(argv[1]) == "newgame") {
		StartNewGameWithoutGUI(_settings_game.game_creation.generation_seed);
	} else {
		_settings_game.game_creation.map_x = Map::LogX();
		_settings_game.game_creation.map_y = Map::LogY();
		_switch_mode = SM_RESTARTGAME;
	}

	return true;
}

DEF_CONSOLE_CMD(ConReload)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Reload game. Usage: 'reload'.");
		IConsolePrint(CC_HELP, "Reloads a game if loaded via savegame / scenario / heightmap.");
		return true;
	}

	if (_file_to_saveload.abstract_ftype == FT_NONE || _file_to_saveload.abstract_ftype == FT_INVALID) {
		IConsolePrint(CC_ERROR, "No game loaded to reload.");
		return true;
	}

	/* Use a switch-mode to prevent copying over newgame settings to active settings. */
	_settings_game.game_creation.map_x = Map::LogX();
	_settings_game.game_creation.map_y = Map::LogY();
	_switch_mode = SM_RELOADGAME;
	return true;
}

/**
 * Print a text buffer line by line to the console. Lines are separated by '\n'.
 * @param full_string The multi-line string to print.
 */
static void PrintLineByLine(std::string_view full_string)
{
	ProcessLineByLine(full_string, [&](std::string_view line) {
		IConsolePrint(CC_DEFAULT, std::string{line});
	});
}

DEF_CONSOLE_CMD(ConListAILibs)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "List installed AI libraries. Usage: 'list_ai_libs'.");
		return true;
	}

	PrintLineByLine(AI::GetConsoleLibraryList());
	return true;
}

DEF_CONSOLE_CMD(ConListAI)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "List installed AIs. Usage: 'list_ai'.");
		return true;
	}

	PrintLineByLine(AI::GetConsoleList());
	return true;
}

DEF_CONSOLE_CMD(ConListGameLibs)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "List installed Game Script libraries. Usage: 'list_game_libs'.");
		return true;
	}

	PrintLineByLine(Game::GetConsoleLibraryList());
	return true;
}

DEF_CONSOLE_CMD(ConListGame)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "List installed Game Scripts. Usage: 'list_game'.");
		return true;
	}

	PrintLineByLine(Game::GetConsoleList());
	return true;
}

DEF_CONSOLE_CMD(ConStartAI)
{
	if (argc == 0 || argc > 5) {
		IConsolePrint(CC_HELP, "Start a new AI. Usage: 'start_ai [<AI>] [num <number>] [<settings>]'.");
		IConsolePrint(CC_HELP, "Start a new AI. If <AI> is given, it starts that specific AI (if found).");
		IConsolePrint(CC_HELP, "If <number> is given, start <number> copies of the AI.");
		IConsolePrint(CC_HELP, "If <settings> is given, it is parsed and the AI settings are set to that.");
		return true;
	}

	if (_game_mode != GM_NORMAL) {
		IConsolePrint(CC_ERROR, "AIs can only be managed in a game.");
		return true;
	}

	if (Company::GetNumItems() == CompanyPool::MAX_SIZE) {
		IConsolePrint(CC_ERROR, "Can't start a new AI (no more free slots).");
		return true;
	}
	if (_networking && !_network_server) {
		IConsolePrint(CC_ERROR, "Only the server can start a new AI.");
		return true;
	}
	if (_networking && !_settings_game.ai.ai_in_multiplayer) {
		IConsolePrint(CC_ERROR, "AIs are not allowed in multiplayer by configuration.");
		IConsolePrint(CC_ERROR, "Switch AI -> AI in multiplayer to True.");
		return true;
	}
	if (!AI::CanStartNew()) {
		IConsolePrint(CC_ERROR, "Can't start a new AI.");
		return true;
	}

	uint32_t arg_index = 2;
	uint32_t number = 1;

	if (argc > arg_index + 1 && strcmp(argv[arg_index], "num") == 0) {
		GetArgumentInteger(&number, argv[arg_index + 1]);
		arg_index += 2;
	}

	int n = 0;
	for (uint32_t i = 0; i < number; i++) {
		/* Find the next free slot */
		bool found = false;
		do {
			found = false;
			for (const Company *c : Company::Iterate()) {
				if (c->index == n) {
					found = true;
					break;
				}
			}
			if (found) {
				n++;
			}
		} while (found);

		AIConfig *config = AIConfig::GetConfig((CompanyID)n);
		if (argc >= arg_index) {
			config->Change(argv[1], -1, false);

			/* If the name is not found, and there is a dot in the name,
			* try again with the assumption everything right of the dot is
			* the version the user wants to load. */
			if (!config->HasScript()) {
				char *name = stredup(argv[1]);
				char *e = strrchr(name, '.');
				if (e != nullptr) {
					*e = '\0';
					e++;

					int version = atoi(e);
					config->Change(name, version, true);
				}
				free(name);
			}

			if (!config->HasScript()) {
				IConsolePrint(CC_ERROR, "Failed to load the specified AI.");
				return true;
			}
			if (argc == arg_index + 1) {
				config->StringToSettings(argv[arg_index]);
			}
		}

		n++;
		/* Start a new AI company */
		Command<CMD_COMPANY_CTRL>::Post(CCA_NEW_AI, INVALID_COMPANY, CRR_NONE, INVALID_CLIENT_ID, {});

	}

	/* Start a new AI company */
	return true;
}

DEF_CONSOLE_CMD(ConReloadAI)
{
	if (argc != 2) {
		IConsolePrint(CC_HELP, "Reload an AI. Usage: 'reload_ai <company-id>'.");
		IConsolePrint(CC_HELP, "Reload the AI with the given company id. For company-id's, see the list of companies from the dropdown menu. Company 1 is 1, etc.");
		return true;
	}

	if (_game_mode != GM_NORMAL) {
		IConsolePrint(CC_ERROR, "AIs can only be managed in a game.");
		return true;
	}

	if (_networking && !_network_server) {
		IConsolePrint(CC_ERROR, "Only the server can reload an AI.");
		return true;
	}

	CompanyID company_id = (CompanyID)(atoi(argv[1]) - 1);
	if (!Company::IsValidID(company_id)) {
		IConsolePrint(CC_ERROR, "Unknown company. Company range is between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	/* In singleplayer mode the player can be in an AI company, after cheating or loading network save with an AI in first slot. */
	if (Company::IsHumanID(company_id) || company_id == _local_company) {
		IConsolePrint(CC_ERROR, "Company is not controlled by an AI.");
		return true;
	}

	/* First kill the company of the AI, then start a new one. This should start the current AI again */
	Command<CMD_COMPANY_CTRL>::Post(CCA_DELETE, company_id, CRR_MANUAL, INVALID_CLIENT_ID, {});
	Command<CMD_COMPANY_CTRL>::Post(CCA_NEW_AI, company_id, CRR_NONE, INVALID_CLIENT_ID, {});
	IConsolePrint(CC_DEFAULT, "AI reloaded.");

	return true;
}

DEF_CONSOLE_CMD(ConStopAI)
{
	if (argc != 2) {
		IConsolePrint(CC_HELP, "Stop an AI. Usage: 'stop_ai <company-id>'.");
		IConsolePrint(CC_HELP, "Stop the AI with the given company id. For company-id's, see the list of companies from the dropdown menu. Company 1 is 1, etc.");
		return true;
	}

	if (_game_mode != GM_NORMAL) {
		IConsolePrint(CC_ERROR, "AIs can only be managed in a game.");
		return true;
	}

	if (_networking && !_network_server) {
		IConsolePrint(CC_ERROR, "Only the server can stop an AI.");
		return true;
	}

	CompanyID company_id = (CompanyID)(atoi(argv[1]) - 1);
	if (!Company::IsValidID(company_id)) {
		IConsolePrint(CC_ERROR, "Unknown company. Company range is between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	/* In singleplayer mode the player can be in an AI company, after cheating or loading network save with an AI in first slot. */
	if (Company::IsHumanID(company_id) || company_id == _local_company) {
		IConsolePrint(CC_ERROR, "Company is not controlled by an AI.");
		return true;
	}

	/* Now kill the company of the AI. */
	Command<CMD_COMPANY_CTRL>::Post(CCA_DELETE, company_id, CRR_MANUAL, INVALID_CLIENT_ID, {});
	IConsolePrint(CC_DEFAULT, "AI stopped, company deleted.");

	return true;
}

DEF_CONSOLE_CMD(ConRescanAI)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Rescan the AI dir for scripts. Usage: 'rescan_ai'.");
		return true;
	}

	if (_networking && !_network_server) {
		IConsolePrint(CC_ERROR, "Only the server can rescan the AI dir for scripts.");
		return true;
	}

	AI::Rescan();

	return true;
}

DEF_CONSOLE_CMD(ConRescanGame)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Rescan the Game Script dir for scripts. Usage: 'rescan_game'.");
		return true;
	}

	if (_networking && !_network_server) {
		IConsolePrint(CC_ERROR, "Only the server can rescan the Game Script dir for scripts.");
		return true;
	}

	Game::Rescan();

	return true;
}

DEF_CONSOLE_CMD(ConRescanNewGRF)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Rescan the data dir for NewGRFs. Usage: 'rescan_newgrf'.");
		return true;
	}

	if (!RequestNewGRFScan()) {
		IConsolePrint(CC_ERROR, "NewGRF scanning is already running. Please wait until completed to run again.");
	}

	return true;
}

DEF_CONSOLE_CMD(ConGetSeed)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Returns the seed used to create this game. Usage: 'getseed'.");
		IConsolePrint(CC_HELP, "The seed can be used to reproduce the exact same map as the game started with.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "Generation Seed: {}", _settings_game.game_creation.generation_seed);
	return true;
}

DEF_CONSOLE_CMD(ConGetDate)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Returns the current date (year-month-day) of the game. Usage: 'getdate'.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "Date: {:04}-{:02}-{:02}", CalTime::CurYear(), CalTime::CurMonth() + 1, CalTime::CurDay());
	return true;
}

DEF_CONSOLE_CMD(ConGetSysDate)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Returns the current date (year-month-day) of your system. Usage: 'getsysdate'.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "System Date: {:%Y-%m-%d %H:%M:%S}", fmt::localtime(time(nullptr)));
	return true;
}


DEF_CONSOLE_CMD(ConAlias)
{
	IConsoleAlias *alias;

	if (argc == 0) {
		IConsolePrint(CC_HELP, "Add a new alias, or redefine the behaviour of an existing alias . Usage: 'alias <name> <command>'.");
		return true;
	}

	if (argc < 3) return false;

	alias = IConsole::AliasGet(argv[1]);
	if (alias == nullptr) {
		IConsole::AliasRegister(argv[1], argv[2]);
	} else {
		alias->cmdline = argv[2];
	}
	return true;
}

DEF_CONSOLE_CMD(ConScreenShot)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Create a screenshot of the game. Usage: 'screenshot [viewport | normal | big | giant | world | heightmap | minimap] [no_con] [size <width> <height>] [<filename>]'.");
		IConsolePrint(CC_HELP, "  'viewport' (default) makes a screenshot of the current viewport (including menus, windows).");
		IConsolePrint(CC_HELP, "  'normal' makes a screenshot of the visible area.");
		IConsolePrint(CC_HELP, "  'big' makes a zoomed-in screenshot of the visible area.");
		IConsolePrint(CC_HELP, "  'giant' makes a screenshot of the whole map using the default zoom level.");
		IConsolePrint(CC_HELP, "  'world' makes a screenshot of the whole map using the current zoom level.");
		IConsolePrint(CC_HELP, "  'heightmap' makes a heightmap screenshot of the map that can be loaded in as heightmap.");
		IConsolePrint(CC_HELP, "  'minimap' makes a top-viewed minimap screenshot of the whole world which represents one tile by one pixel.");
		IConsolePrint(CC_HELP, "  'topography' makes a top-viewed topography screenshot of the whole world which represents one tile by one pixel.");
		IConsolePrint(CC_HELP, "  'industry' makes a top-viewed industries screenshot of the whole world which represents one tile by one pixel.");
		IConsolePrint(CC_HELP, "  'no_con' hides the console to create the screenshot (only useful in combination with 'viewport').");
		IConsolePrint(CC_HELP, "  'size' sets the width and height of the viewport to make a screenshot of (only useful in combination with 'normal' or 'big').");
		IConsolePrint(CC_HELP, "  A filename ending in # will prevent overwriting existing files and will number files counting upwards.");
		return true;
	}

	if (argc > 7) return false;

	ScreenshotType type = SC_VIEWPORT;
	uint32_t width = 0;
	uint32_t height = 0;
	std::string name{};
	uint32_t arg_index = 1;

	if (argc > arg_index) {
		if (strcmp(argv[arg_index], "viewport") == 0) {
			type = SC_VIEWPORT;
			arg_index += 1;
		} else if (strcmp(argv[arg_index], "normal") == 0) {
			type = SC_DEFAULTZOOM;
			arg_index += 1;
		} else if (strcmp(argv[arg_index], "big") == 0) {
			type = SC_ZOOMEDIN;
			arg_index += 1;
		} else if (strcmp(argv[arg_index], "giant") == 0) {
			type = SC_WORLD;
			arg_index += 1;
		} else if (strcmp(argv[arg_index], "world") == 0) {
			type = SC_WORLD_ZOOM;
			arg_index += 1;
		} else if (strcmp(argv[arg_index], "heightmap") == 0) {
			type = SC_HEIGHTMAP;
			arg_index += 1;
		} else if (strcmp(argv[arg_index], "minimap") == 0) {
			type = SC_MINIMAP;
			arg_index += 1;
		} else if (strcmp(argv[arg_index], "topography") == 0) {
			type = SC_TOPOGRAPHY;
			arg_index += 1;
		} else if (strcmp(argv[arg_index], "industry") == 0) {
			type = SC_INDUSTRY;
			arg_index += 1;
		}
	}

	if (argc > arg_index && strcmp(argv[arg_index], "no_con") == 0) {
		if (type != SC_VIEWPORT) {
			IConsolePrint(CC_ERROR, "'no_con' can only be used in combination with 'viewport'.");
			return true;
		}
		IConsoleClose();
		arg_index += 1;
	}

	if (argc > arg_index + 2 && strcmp(argv[arg_index], "size") == 0) {
		/* size <width> <height> */
		if (type != SC_DEFAULTZOOM && type != SC_ZOOMEDIN) {
			IConsolePrint(CC_ERROR, "'size' can only be used in combination with 'normal' or 'big'.");
			return true;
		}
		GetArgumentInteger(&width, argv[arg_index + 1]);
		GetArgumentInteger(&height, argv[arg_index + 2]);
		arg_index += 3;
	}

	if (argc > arg_index) {
		/* Last parameter that was not one of the keywords must be the filename. */
		name = argv[arg_index];
		arg_index += 1;
	}

	if (argc > arg_index) {
		/* We have parameters we did not process; means we misunderstood any of the above. */
		return false;
	}

	MakeScreenshot(type, name, width, height);
	return true;
}

DEF_CONSOLE_CMD(ConMinimap)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Create a flat image of the game minimap. Usage: 'minimap [owner] [file name]'");
		IConsolePrint(CC_HELP, "'owner' uses the tile owner to colour the minimap image, this is the only mode at present");
		return true;
	}

	const char *name = nullptr;
	if (argc > 1) {
		if (strcmp(argv[1], "owner") != 0) {
			/* invalid mode */
			return false;
		}
	}
	if (argc > 2) {
		name = argv[2];
	}

	MakeMinimapWorldScreenshot(name);
	return true;
}

DEF_CONSOLE_CMD(ConInfoCmd)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Print out debugging information about a command. Usage: 'info_cmd <cmd>'.");
		return true;
	}

	if (argc < 2) return false;

	const IConsoleCmd *cmd = IConsole::CmdGet(argv[1]);
	if (cmd == nullptr) {
		IConsolePrint(CC_ERROR, "The given command was not found.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "Command name: '{}'", cmd->name);

	if (cmd->hook != nullptr) IConsolePrint(CC_DEFAULT, "Command is hooked.");

	return true;
}

DEF_CONSOLE_CMD(ConDebugLevel)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Get/set the default debugging level for the game. Usage: 'debug_level [<level>]'.");
		IConsolePrint(CC_HELP, "Level can be any combination of names, levels. Eg 'net=5 ms=4'. Remember to enclose it in \"'\"s.");
		return true;
	}

	if (argc > 2) return false;

	if (argc == 1) {
		IConsolePrint(CC_DEFAULT, "Current debug-level: '{}'", GetDebugString());
	} else {
		SetDebugString(argv[1], [](std::string err) { IConsolePrint(CC_ERROR, err); });
	}

	return true;
}

DEF_CONSOLE_CMD(ConExit)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Exit the game. Usage: 'exit'.");
		return true;
	}

	if (_game_mode == GM_NORMAL && _settings_client.gui.autosave_on_exit) DoExitSave();

	_exit_game = true;
	return true;
}

DEF_CONSOLE_CMD(ConPart)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Leave the currently joined/running game (only ingame). Usage: 'part'.");
		return true;
	}

	if (_game_mode != GM_NORMAL) return false;

	if (_network_dedicated) {
		IConsolePrint(CC_ERROR, "A dedicated server can not leave the game.");
		return false;
	}

	_switch_mode = SM_MENU;
	return true;
}

DEF_CONSOLE_CMD(ConHelp)
{
	if (argc == 2) {
		const IConsoleCmd *cmd;
		const IConsoleAlias *alias;

		cmd = IConsole::CmdGet(argv[1]);
		if (cmd != nullptr) {
			cmd->proc(0, nullptr);
			return true;
		}

		alias = IConsole::AliasGet(argv[1]);
		if (alias != nullptr) {
			cmd = IConsole::CmdGet(alias->cmdline);
			if (cmd != nullptr) {
				cmd->proc(0, nullptr);
				return true;
			}
			IConsolePrint(CC_ERROR, "Alias is of special type, please see its execution-line: '{}'.", alias->cmdline);
			return true;
		}

		IConsolePrint(CC_ERROR, "Command not found.");
		return true;
	}

	IConsolePrint(TC_LIGHT_BLUE, " ---- OpenTTD Console Help ---- ");
	IConsolePrint(CC_DEFAULT, " - commands: the command to list all commands is 'list_cmds'.");
	IConsolePrint(CC_DEFAULT, " call commands with '<command> <arg2> <arg3>...'");
	IConsolePrint(CC_DEFAULT, " - to assign strings, or use them as arguments, enclose it within quotes.");
	IConsolePrint(CC_DEFAULT, " like this: '<command> \"string argument with spaces\"'.");
	IConsolePrint(CC_DEFAULT, " - use 'help <command>' to get specific information.");
	IConsolePrint(CC_DEFAULT, " - scroll console output with shift + (up | down | pageup | pagedown).");
	IConsolePrint(CC_DEFAULT, " - scroll console input history with the up or down arrows.");
	IConsolePrint(CC_DEFAULT, "");
	return true;
}

DEF_CONSOLE_CMD(ConListCommands)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "List all registered commands. Usage: 'list_cmds [<pre-filter>]'.");
		return true;
	}

	for (auto &it : IConsole::Commands()) {
		const IConsoleCmd *cmd = &it.second;
		if (argv[1] == nullptr || cmd->name.find(argv[1]) != std::string::npos) {
			if ((_settings_client.gui.console_show_unlisted || !cmd->unlisted) && (cmd->hook == nullptr || cmd->hook(false) != CHR_HIDE)) IConsolePrint(CC_DEFAULT, cmd->name);
		}
	}

	return true;
}

DEF_CONSOLE_CMD(ConListAliases)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "List all registered aliases. Usage: 'list_aliases [<pre-filter>]'.");
		return true;
	}

	for (auto &it : IConsole::Aliases()) {
		const IConsoleAlias *alias = &it.second;
		if (argv[1] == nullptr || alias->name.find(argv[1]) != std::string::npos) {
			IConsolePrint(CC_DEFAULT, "{} => {}", alias->name, alias->cmdline);
		}
	}

	return true;
}

DEF_CONSOLE_CMD(ConCompanies)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "List the details of all companies in the game. Usage 'companies'.");
		return true;
	}

	for (const Company *c : Company::Iterate()) {
		/* Grab the company name */
		SetDParam(0, c->index);
		std::string company_name = GetString(STR_COMPANY_NAME);

		const char *password_state = "";
		if (c->is_ai) {
			password_state = "AI";
		} else if (_network_server) {
			password_state = _network_company_states[c->index].password.empty() ? "unprotected" : "protected";
		}

		std::string colour = GetString(STR_COLOUR_DARK_BLUE + _company_colours[c->index]);
		IConsolePrint(CC_INFO, "#:{}({}) Company Name: '{}'  Year Founded: {}  Age: {}  Money: {}  Loan: {}  Value: {}  (T:{}, R:{}, P:{}, S:{}) {}",
			c->index + 1, colour, company_name,
			c->InauguratedDisplayYear(), c->age_years, (int64_t)c->money, (int64_t)c->current_loan, (int64_t)CalculateCompanyValue(c),
			c->group_all[VEH_TRAIN].num_vehicle,
			c->group_all[VEH_ROAD].num_vehicle,
			c->group_all[VEH_AIRCRAFT].num_vehicle,
			c->group_all[VEH_SHIP].num_vehicle,
			password_state);
	}

	return true;
}

DEF_CONSOLE_CMD(ConSay)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Chat to your fellow players in a multiplayer game. Usage: 'say \"<msg>\"'.");
		return true;
	}

	if (argc != 2) return false;

	if (!_network_server) {
		NetworkClientSendChat(NETWORK_ACTION_CHAT, DESTTYPE_BROADCAST, 0 /* param does not matter */, argv[1]);
	} else {
		bool from_admin = (_redirect_console_to_admin < INVALID_ADMIN_ID);
		NetworkServerSendChat(NETWORK_ACTION_CHAT, DESTTYPE_BROADCAST, 0, argv[1], CLIENT_ID_SERVER, from_admin);
	}

	return true;
}

DEF_CONSOLE_CMD(ConSayCompany)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Chat to a certain company in a multiplayer game. Usage: 'say_company <company-no> \"<msg>\"'.");
		IConsolePrint(CC_HELP, "CompanyNo is the company that plays as company <companyno>, 1 through max_companies.");
		return true;
	}

	if (argc != 3) return false;

	CompanyID company_id = (CompanyID)(atoi(argv[1]) - 1);
	if (!Company::IsValidID(company_id)) {
		IConsolePrint(CC_DEFAULT, "Unknown company. Company range is between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	if (!_network_server) {
		NetworkClientSendChat(NETWORK_ACTION_CHAT_COMPANY, DESTTYPE_TEAM, company_id, argv[2]);
	} else {
		bool from_admin = (_redirect_console_to_admin < INVALID_ADMIN_ID);
		NetworkServerSendChat(NETWORK_ACTION_CHAT_COMPANY, DESTTYPE_TEAM, company_id, argv[2], CLIENT_ID_SERVER, from_admin);
	}

	return true;
}

DEF_CONSOLE_CMD(ConSayClient)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Chat to a certain client in a multiplayer game. Usage: 'say_client <client-no> \"<msg>\"'.");
		IConsolePrint(CC_HELP, "For client-id's, see the command 'clients'.");
		return true;
	}

	if (argc != 3) return false;

	if (!_network_server) {
		NetworkClientSendChat(NETWORK_ACTION_CHAT_CLIENT, DESTTYPE_CLIENT, atoi(argv[1]), argv[2]);
	} else {
		bool from_admin = (_redirect_console_to_admin < INVALID_ADMIN_ID);
		NetworkServerSendChat(NETWORK_ACTION_CHAT_CLIENT, DESTTYPE_CLIENT, atoi(argv[1]), argv[2], CLIENT_ID_SERVER, from_admin);
	}

	return true;
}

DEF_CONSOLE_CMD(ConCompanyPassword)
{
	if (argc == 0) {
		if (_network_dedicated) {
			IConsolePrint(CC_HELP, "Change the password of a company. Usage: 'company_pw <company-no> \"<password>\".");
		} else if (_network_server) {
			IConsolePrint(CC_HELP, "Change the password of your or any other company. Usage: 'company_pw [<company-no>] \"<password>\"'.");
		} else {
			IConsolePrint(CC_HELP, "Change the password of your company. Usage: 'company_pw \"<password>\"'.");
		}

		IConsolePrint(CC_HELP, "Use \"*\" to disable the password.");
		return true;
	}

	CompanyID company_id;
	std::string password;
	const char *errormsg;

	if (argc == 2) {
		company_id = _local_company;
		password = argv[1];
		errormsg = "You have to own a company to make use of this command.";
	} else if (argc == 3 && _network_server) {
		company_id = (CompanyID)(atoi(argv[1]) - 1);
		password = argv[2];
		errormsg = "You have to specify the ID of a valid human controlled company.";
	} else {
		return false;
	}

	if (!Company::IsValidHumanID(company_id)) {
		IConsolePrint(CC_ERROR, errormsg);
		return false;
	}

	password = NetworkChangeCompanyPassword(company_id, password);

	if (password.empty()) {
		IConsolePrint(CC_INFO, "Company password cleared.");
	} else {
		IConsolePrint(CC_INFO, "Company password changed to '{}'.", password);
	}

	return true;
}

/** All the known authorized keys with their name. */
static std::vector<std::pair<std::string_view, NetworkAuthorizedKeys *>> _console_cmd_authorized_keys{
	{ "admin", &_settings_client.network.admin_authorized_keys },
	{ "rcon", &_settings_client.network.rcon_authorized_keys },
	{ "server", &_settings_client.network.server_authorized_keys },
	{ "settings", &_settings_client.network.settings_authorized_keys },
};

enum ConNetworkAuthorizedKeyAction : uint8_t {
	CNAKA_LIST,
	CNAKA_ADD,
	CNAKA_REMOVE,
};

static void PerformNetworkAuthorizedKeyAction(std::string_view name, NetworkAuthorizedKeys *authorized_keys, ConNetworkAuthorizedKeyAction action, const std::string &authorized_key, CompanyID company = INVALID_COMPANY)
{
	switch (action) {
		case CNAKA_LIST:
			IConsolePrint(CC_WHITE, "The authorized keys for {} are:", name);
			for (auto &ak : *authorized_keys) IConsolePrint(CC_INFO, "  {}", ak);
			return;

		case CNAKA_ADD:
			if (authorized_keys->Contains(authorized_key)) {
				IConsolePrint(CC_WARNING, "Not added {} to {} as it already exists.", authorized_key, name);
				return;
			}

			if (company == INVALID_COMPANY) {
				authorized_keys->Add(authorized_key);
			} else {
				AutoRestoreBackup backup(_current_company, company);
				Command<CMD_COMPANY_ALLOW_LIST_CTRL>::Post(CALCA_ADD, authorized_key);
			}
			IConsolePrint(CC_INFO, "Added {} to {}.", authorized_key, name);
			return;

		case CNAKA_REMOVE:
			if (!authorized_keys->Contains(authorized_key)) {
				IConsolePrint(CC_WARNING, "Not removed {} from {} as it does not exist.", authorized_key, name);
				return;
			}

			if (company == INVALID_COMPANY) {
				authorized_keys->Remove(authorized_key);
			} else {
				AutoRestoreBackup backup(_current_company, company);
				Command<CMD_COMPANY_ALLOW_LIST_CTRL>::Post(CALCA_REMOVE, authorized_key);
			}
			IConsolePrint(CC_INFO, "Removed {} from {}.", authorized_key, name);
			return;
	}
}

DEF_CONSOLE_CMD(ConNetworkAuthorizedKey)
{
	if (argc <= 2) {
		IConsolePrint(CC_HELP, "List and update authorized keys. Usage: 'authorized_key list [type]|add [type] [key]|remove [type] [key]'.");
		IConsolePrint(CC_HELP, "  list: list all the authorized keys of the given type.");
		IConsolePrint(CC_HELP, "  add: add the given key to the authorized keys of the given type.");
		IConsolePrint(CC_HELP, "  remove: remove the given key from the authorized keys of the given type; use 'all' to remove all authorized keys.");
		IConsolePrint(CC_HELP, "Instead of a key, use 'client:<id>' to add/remove the key of that given client.");

		std::string buffer;
		for (auto [name, _] : _console_cmd_authorized_keys) fmt::format_to(std::back_inserter(buffer), ", {}", name);
		IConsolePrint(CC_HELP, "The supported types are: all{} and company:<id>.", buffer);
		return true;
	}

	ConNetworkAuthorizedKeyAction action;
	std::string_view action_string = argv[1];
	if (StrEqualsIgnoreCase(action_string, "list")) {
		action = CNAKA_LIST;
	} else if (StrEqualsIgnoreCase(action_string, "add")) {
		action = CNAKA_ADD;
	} else if (StrEqualsIgnoreCase(action_string, "remove") || StrEqualsIgnoreCase(action_string, "delete")) {
		action = CNAKA_REMOVE;
	} else {
		IConsolePrint(CC_WARNING, "No valid action was given.");
		return false;
	}

	std::string authorized_key;
	if (action != CNAKA_LIST) {
		if (argc <= 3) {
			IConsolePrint(CC_ERROR, "You must enter the key.");
			return false;
		}

		authorized_key = argv[3];
		if (StrStartsWithIgnoreCase(authorized_key, "client:")) {
			std::string id_string(authorized_key.substr(7));
			authorized_key = NetworkGetPublicKeyOfClient(static_cast<ClientID>(std::stoi(id_string)));
			if (authorized_key.empty()) {
				IConsolePrint(CC_ERROR, "You must enter a valid client id; see 'clients'.");
				return false;
			}
		}

		if (authorized_key.size() != NETWORK_PUBLIC_KEY_LENGTH - 1) {
			IConsolePrint(CC_ERROR, "You must enter a valid authorized key.");
			return false;
		}
	}

	std::string_view type = argv[2];
	if (StrEqualsIgnoreCase(type, "all")) {
		for (auto [name, authorized_keys] : _console_cmd_authorized_keys) PerformNetworkAuthorizedKeyAction(name, authorized_keys, action, authorized_key);
		for (Company *c : Company::Iterate()) PerformNetworkAuthorizedKeyAction(fmt::format("company:{}", c->index + 1), &c->allow_list, action, authorized_key, c->index);
		return true;
	}

	if (StrStartsWithIgnoreCase(type, "company:")) {
		std::string id_string(type.substr(8));
		Company *c = Company::GetIfValid(std::stoi(id_string) - 1);
		if (c == nullptr) {
			IConsolePrint(CC_ERROR, "You must enter a valid company id; see 'companies'.");
			return false;
		}

		PerformNetworkAuthorizedKeyAction(type, &c->allow_list, action, authorized_key, c->index);
		return true;
	}

	for (auto [name, authorized_keys] : _console_cmd_authorized_keys) {
		if (StrEqualsIgnoreCase(type, name)) continue;

		PerformNetworkAuthorizedKeyAction(name, authorized_keys, action, authorized_key);
		return true;
	}

	IConsolePrint(CC_WARNING, "No valid type was given.");
	return false;
}

DEF_CONSOLE_CMD(ConCompanyPasswordHash)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Change the password hash of a company. Usage: 'company_pw_hash <company-no> \"<password_hash>\"");
		IConsolePrint(CC_HELP, "Use \"*\" to disable the password.");
		return true;
	}

	if (argc != 3) return false;

	CompanyID company_id = (CompanyID)(atoi(argv[1]) - 1);
	const char *password = argv[2];

	if (!Company::IsValidHumanID(company_id)) {
		IConsolePrint(CC_ERROR, "You have to specify the ID of a valid human controlled company.");
		return false;
	}

	if (strcmp(password, "*") == 0) password = "";

	NetworkServerSetCompanyPassword(company_id, password, true);

	if (StrEmpty(password)) {
		IConsolePrint(CC_WARNING, "Company password hash cleared");
	} else {
		IConsolePrint(CC_WARNING, "Company password hash changed to: {}", password);
	}

	return true;
}

DEF_CONSOLE_CMD(ConCompanyPasswordHashes)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "List the password hashes of all companies in the game. Usage 'company_pw_hashes'");
		return true;
	}

	for (const Company *c : Company::Iterate()) {
		/* Grab the company name */
		SetDParam(0, c->index);
		std::string company_name = GetString(STR_COMPANY_NAME);

		IConsolePrint(CC_INFO, "#:{}({}) Company Name: '{}'  Hash: '{}'",
			c->index + 1, GetStringPtr(STR_COLOUR_DARK_BLUE + _company_colours[c->index]), company_name.c_str(), _network_company_states[c->index].password.c_str());
	}

	return true;
}

/* Content downloading only is available with ZLIB */
#if defined(WITH_ZLIB)
#include "network/network_content.h"

/** Resolve a string to a content type. */
static ContentType StringToContentType(const char *str)
{
	static const std::initializer_list<std::pair<std::string_view, ContentType>> content_types = {
		{"base",      CONTENT_TYPE_BASE_GRAPHICS},
		{"newgrf",    CONTENT_TYPE_NEWGRF},
		{"ai",        CONTENT_TYPE_AI},
		{"ailib",     CONTENT_TYPE_AI_LIBRARY},
		{"scenario",  CONTENT_TYPE_SCENARIO},
		{"heightmap", CONTENT_TYPE_HEIGHTMAP},
	};
	for (const auto &ct : content_types) {
		if (StrEqualsIgnoreCase(str, ct.first)) return ct.second;
	}
	return CONTENT_TYPE_END;
}

/** Asynchronous callback */
struct ConsoleContentCallback : public ContentCallback {
	void OnConnect(bool success) override
	{
		IConsolePrint(CC_DEFAULT, "Content server connection {}.", success ? "established" : "failed");
	}

	void OnDisconnect() override
	{
		IConsolePrint(CC_DEFAULT, "Content server connection closed.");
	}

	void OnDownloadComplete(ContentID cid) override
	{
		IConsolePrint(CC_DEFAULT, "Completed download of {}.", cid);
	}
};

/**
 * Outputs content state information to console
 * @param ci the content info
 */
static void OutputContentState(const ContentInfo *const ci)
{
	static const char * const types[] = { "Base graphics", "NewGRF", "AI", "AI library", "Scenario", "Heightmap", "Base sound", "Base music", "Game script", "GS library" };
	static_assert(lengthof(types) == CONTENT_TYPE_END - CONTENT_TYPE_BEGIN);
	static const char * const states[] = { "Not selected", "Selected", "Dep Selected", "Installed", "Unknown" };
	static const TextColour state_to_colour[] = { CC_COMMAND, CC_INFO, CC_INFO, CC_WHITE, CC_ERROR };

	IConsolePrint(state_to_colour[ci->state], "{}, {}, {}, {}, {:08X}, {}", ci->id, types[ci->type - 1], states[ci->state], ci->name, ci->unique_id, FormatArrayAsHex(ci->md5sum));
}

DEF_CONSOLE_CMD(ConContent)
{
	static ContentCallback *cb = nullptr;
	if (cb == nullptr) {
		cb = new ConsoleContentCallback();
		_network_content_client.AddCallback(cb);
	}

	if (argc <= 1) {
		IConsolePrint(CC_HELP, "Query, select and download content. Usage: 'content update|upgrade|select [id]|unselect [all|id]|state [filter]|download'.");
		IConsolePrint(CC_HELP, "  update: get a new list of downloadable content; must be run first.");
		IConsolePrint(CC_HELP, "  upgrade: select all items that are upgrades.");
		IConsolePrint(CC_HELP, "  select: select a specific item given by its id. If no parameter is given, all selected content will be listed.");
		IConsolePrint(CC_HELP, "  unselect: unselect a specific item given by its id or 'all' to unselect all.");
		IConsolePrint(CC_HELP, "  state: show the download/select state of all downloadable content. Optionally give a filter string.");
		IConsolePrint(CC_HELP, "  download: download all content you've selected.");
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "update")) {
		_network_content_client.RequestContentList((argc > 2) ? StringToContentType(argv[2]) : CONTENT_TYPE_END);
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "upgrade")) {
		_network_content_client.SelectUpgrade();
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "select")) {
		if (argc <= 2) {
			/* List selected content */
			IConsolePrint(CC_WHITE, "id, type, state, name");
			for (ConstContentIterator iter = _network_content_client.Begin(); iter != _network_content_client.End(); iter++) {
				if ((*iter)->state != ContentInfo::SELECTED && (*iter)->state != ContentInfo::AUTOSELECTED) continue;
				OutputContentState(*iter);
			}
		} else if (StrEqualsIgnoreCase(argv[2], "all")) {
			/* The intention of this function was that you could download
			 * everything after a filter was applied; but this never really
			 * took off. Instead, a select few people used this functionality
			 * to download every available package on BaNaNaS. This is not in
			 * the spirit of this service. Additionally, these few people were
			 * good for 70% of the consumed bandwidth of BaNaNaS. */
			IConsolePrint(CC_ERROR, "'select all' is no longer supported since 1.11.");
		} else {
			_network_content_client.Select((ContentID)atoi(argv[2]));
		}
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "unselect")) {
		if (argc <= 2) {
			IConsolePrint(CC_ERROR, "You must enter the id.");
			return false;
		}
		if (StrEqualsIgnoreCase(argv[2], "all")) {
			_network_content_client.UnselectAll();
		} else {
			_network_content_client.Unselect((ContentID)atoi(argv[2]));
		}
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "state")) {
		IConsolePrint(CC_WHITE, "id, type, state, name");
		for (ConstContentIterator iter = _network_content_client.Begin(); iter != _network_content_client.End(); iter++) {
			if (argc > 2 && strcasestr((*iter)->name.c_str(), argv[2]) == nullptr) continue;
			OutputContentState(*iter);
		}
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "download")) {
		uint files;
		uint bytes;
		_network_content_client.DownloadSelectedContent(files, bytes);
		IConsolePrint(CC_DEFAULT, "Downloading {} file(s) ({} bytes).", files, bytes);
		return true;
	}

	return false;
}
#endif /* defined(WITH_ZLIB) */

DEF_CONSOLE_CMD(ConFont)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Manage the fonts configuration.");
		IConsolePrint(CC_HELP, "Usage 'font'.");
		IConsolePrint(CC_HELP, "  Print out the fonts configuration.");
		IConsolePrint(CC_HELP, "  The \"Currently active\" configuration is the one actually in effect (after interface scaling and replacing unavailable fonts).");
		IConsolePrint(CC_HELP, "  The \"Requested\" configuration is the one requested via console command or config file.");
		IConsolePrint(CC_HELP, "Usage 'font [medium|small|large|mono] [<font name>] [<size>]'.");
		IConsolePrint(CC_HELP, "  Change the configuration for a font.");
		IConsolePrint(CC_HELP, "  Omitting an argument will keep the current value.");
		IConsolePrint(CC_HELP, "  Set <font name> to \"\" for the default font. Note that <size> has no effect if the default font is in use, and fixed defaults are used instead.");
		IConsolePrint(CC_HELP, "  If the sprite font is enabled in Game Options, it is used instead of the default font.");
		IConsolePrint(CC_HELP, "  The <size> is automatically multiplied by the current interface scaling.");
		return true;
	}

	FontSize argfs;
	for (argfs = FS_BEGIN; argfs < FS_END; argfs++) {
		if (argc > 1 && StrEqualsIgnoreCase(argv[1], FontSizeToName(argfs))) break;
	}

	/* First argument must be a FontSize. */
	if (argc > 1 && argfs == FS_END) return false;

	if (argc > 2) {
		FontCacheSubSetting *setting = GetFontCacheSubSetting(argfs);
		std::string font = setting->font;
		uint size = setting->size;
		uint v;
		uint8_t arg_index = 2;
		/* For <name> we want a string. */

		if (!GetArgumentInteger(&v, argv[arg_index])) {
			font = argv[arg_index++];
		}

		if (argc > arg_index) {
			/* For <size> we want a number. */
			if (GetArgumentInteger(&v, argv[arg_index])) {
				size = v;
				arg_index++;
			}
		}

		SetFont(argfs, font, size);
	}

	for (FontSize fs = FS_BEGIN; fs < FS_END; fs++) {
		FontCache *fc = FontCache::Get(fs);
		FontCacheSubSetting *setting = GetFontCacheSubSetting(fs);
		/* Make sure all non sprite fonts are loaded. */
		if (!setting->font.empty() && !fc->HasParent()) {
			InitFontCache(fs == FS_MONO);
			fc = FontCache::Get(fs);
		}
		IConsolePrint(CC_DEFAULT, "{} font:", FontSizeToName(fs));
		IConsolePrint(CC_DEFAULT, "Currently active: \"{}\", size {}", fc->GetFontName(), fc->GetFontSize());
		IConsolePrint(CC_DEFAULT, "Requested: \"{}\", size {}", setting->font, setting->size);
	}

	FontChanged();

	return true;
}

DEF_CONSOLE_CMD(ConSetting)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Change setting for all clients. Usage: 'setting <name> [<value>]'.");
		IConsolePrint(CC_HELP, "Omitting <value> will print out the current value of the setting.");
		return true;
	}

	if (argc == 1 || argc > 3) return false;

	if (argc == 2) {
		IConsoleGetSetting(argv[1]);
	} else {
		IConsoleSetSetting(argv[1], argv[2]);
	}

	return true;
}

DEF_CONSOLE_CMD(ConSettingNewgame)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Change setting for the next game. Usage: 'setting_newgame <name> [<value>]'.");
		IConsolePrint(CC_HELP, "Omitting <value> will print out the current value of the setting.");
		return true;
	}

	if (argc == 1 || argc > 3) return false;

	if (argc == 2) {
		IConsoleGetSetting(argv[1], true);
	} else {
		IConsoleSetSetting(argv[1], argv[2], true);
	}

	return true;
}

DEF_CONSOLE_CMD(ConListSettings)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "List settings. Usage: 'list_settings [<pre-filter>]'.");
		return true;
	}

	if (argc > 2) return false;

	IConsoleListSettings((argc == 2) ? argv[1] : nullptr, false);
	return true;
}

DEF_CONSOLE_CMD(ConListSettingsDefaults)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "List settings and also show default value. Usage: 'list_settings_def [<pre-filter>]'");
		return true;
	}

	if (argc > 2) return false;

	IConsoleListSettings((argc == 2) ? argv[1] : nullptr, true);
	return true;
}

DEF_CONSOLE_CMD(ConGamelogPrint)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Print logged fundamental changes to the game since the start. Usage: 'gamelog'.");
		return true;
	}

	GamelogPrintConsole();
	return true;
}

DEF_CONSOLE_CMD(ConNewGRFReload)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Reloads all active NewGRFs from disk. Equivalent to reapplying NewGRFs via the settings, but without asking for confirmation. This might crash OpenTTD!");
		return true;
	}

	ReloadNewGRFData();

	extern void PostCheckNewGRFLoadWarnings();
	PostCheckNewGRFLoadWarnings();
	return true;
}

DEF_CONSOLE_CMD(ConListDirs)
{
	struct SubdirNameMap {
		Subdirectory subdir; ///< Index of subdirectory type
		const char *name;    ///< UI name for the directory
		bool default_only;   ///< Whether only the default (first existing) directory for this is interesting
	};
	static const SubdirNameMap subdir_name_map[] = {
		/* Game data directories */
		{ BASESET_DIR,      "baseset",    false },
		{ NEWGRF_DIR,       "newgrf",     false },
		{ AI_DIR,           "ai",         false },
		{ AI_LIBRARY_DIR,   "ailib",      false },
		{ GAME_DIR,         "gs",         false },
		{ GAME_LIBRARY_DIR, "gslib",      false },
		{ SCENARIO_DIR,     "scenario",   false },
		{ HEIGHTMAP_DIR,    "heightmap",  false },
		/* Default save locations for user data */
		{ SAVE_DIR,         "save",       true  },
		{ AUTOSAVE_DIR,     "autosave",   true  },
		{ SCREENSHOT_DIR,   "screenshot", true  },
		{ SOCIAL_INTEGRATION_DIR, "social_integration", true },
	};

	if (argc != 2) {
		IConsolePrint(CC_HELP, "List all search paths or default directories for various categories.");
		IConsolePrint(CC_HELP, "Usage: list_dirs <category>");
		std::string cats = subdir_name_map[0].name;
		bool first = true;
		for (const SubdirNameMap &sdn : subdir_name_map) {
			if (!first) cats = cats + ", " + sdn.name;
			first = false;
		}
		IConsolePrint(CC_HELP, "Valid categories: {}", cats);
		return true;
	}

	btree::btree_set<std::string> seen_dirs;
	for (const SubdirNameMap &sdn : subdir_name_map) {
		if (!StrEqualsIgnoreCase(argv[1], sdn.name))  continue;
		bool found = false;
		for (Searchpath sp : _valid_searchpaths) {
			/* Get the directory */
			std::string path = FioGetDirectory(sp, sdn.subdir);
			/* Check it hasn't already been listed */
			if (seen_dirs.find(path) != seen_dirs.end()) continue;
			seen_dirs.insert(path);
			/* Check if exists and mark found */
			bool exists = FileExists(path);
			found |= exists;
			/* Print */
			if (!sdn.default_only || exists) {
				IConsolePrint(exists ? CC_DEFAULT : CC_INFO, "{} {}", path, exists ? "[ok]" : "[not found]");
				if (sdn.default_only) break;
			}
		}
		if (!found) {
			IConsolePrint(CC_ERROR, "No directories exist for category {}", argv[1]);
		}
		return true;
	}

	IConsolePrint(CC_ERROR, "Invalid category name: {}", argv[1]);
	return false;
}

DEF_CONSOLE_CMD(ConResetBlockedHeliports)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Resets heliports blocked by the improved breakdowns bug, for single-player use only.");
		return true;
	}

	unsigned int count = 0;
	for (Station *st : Station::Iterate()) {
		if (st->airport.tile == INVALID_TILE) continue;
		if (st->airport.HasHangar()) continue;
		if (!st->airport.flags) continue;

		bool occupied = false;
		for (const Aircraft *a : Aircraft::Iterate()) {
			if (a->targetairport == st->index && a->state != FLYING) {
				occupied = true;
				break;
			}
		}
		if (!occupied) {
			st->airport.flags = 0;
			count++;
			SetDParam(0, st->index);
			IConsolePrint(CC_DEFAULT, "Unblocked: {}", GetString(STR_STATION_NAME));
		}
	}

	IConsolePrint(CC_DEFAULT, "Unblocked {} heliports", count);
	return true;
}

DEF_CONSOLE_CMD(ConMergeLinkgraphJobsAsap)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Merge linkgraph jobs asap, for single-player use only.");
		return true;
	}

	for (LinkGraphJob *lgj : LinkGraphJob::Iterate()) {
		lgj->SetJoinTick(_scaled_tick_counter);
	}
	return true;
}

DEF_CONSOLE_CMD(ConUnblockBayRoadStops)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Unblock bay road stops blocked by a bug, for single-player use only.");
		return true;
	}

	for (Station *st : Station::Iterate()) {
		for (RoadStopType rs_type : { RoadStopType::Bus, RoadStopType::Truck }) {
			for (RoadStop *rs = st->GetPrimaryRoadStop(rs_type); rs != nullptr; rs = rs->next) {
				if (IsBayRoadStopTile(rs->xy)) {
					rs->DebugClearOccupancy();
				}
			}
		}
	}
	for (const RoadVehicle *rv : RoadVehicle::Iterate()) {
		if (IsInsideMM(rv->state, RVSB_IN_ROAD_STOP, RVSB_IN_ROAD_STOP_END)) {
			RoadStop::GetByTile(rv->tile, GetRoadStopType(rv->tile))->DebugReEnter(rv);
		}
	}
	return true;
}

DEF_CONSOLE_CMD(ConDbgSpecial)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Debug special.");
		return true;
	}

	if (argc == 2) {
		if (strcmp(argv[1], "error") == 0) {
			FatalErrorI("User triggered");
			return true;
		}
	}

	return false;
}

#ifdef _DEBUG
DEF_CONSOLE_CMD(ConDeleteVehicleID)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Delete vehicle ID, for emergency single-player use only.");
		return true;
	}

	if (argc == 2) {
		uint32_t result;
		if (GetArgumentInteger(&result, argv[1])) {
			extern void ConsoleRemoveVehicle(VehicleID id);
			ConsoleRemoveVehicle(result);
			return true;
		}
	}

	return false;
}

DEF_CONSOLE_CMD(ConRunTileLoopTile)
{
	if (argc == 0 || argc > 3) {
		IConsolePrint(CC_HELP, "Run tile loop proc on tile.");
		return true;
	}

	if (argc >= 2) {
		uint32_t tile;
		if (!GetArgumentInteger(&tile, argv[1])) return false;

		if (tile >= Map::Size()) {
			IConsolePrint(CC_ERROR, "Tile does not exist.");
			return true;
		}
		uint32_t count = 1;
		if (argc >= 3) {
			if (!GetArgumentInteger(&count, argv[2])) return false;
		}
		for (uint32_t i = 0; i < count; i++) {
			_tile_type_procs[GetTileType(TileIndex{tile})]->tile_loop_proc(TileIndex{tile});
		}
		return true;
	}

	return false;
}
#endif

DEF_CONSOLE_CMD(ConGetFullDate)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Returns the current full date/tick information of the game. Usage: 'getfulldate'");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "Calendar Date: {:04}-{:02}-{:02} ({}), fract: {}, sub_fract: {}", CalTime::CurYear(), CalTime::CurMonth() + 1, CalTime::CurDay(), CalTime::CurDate(), CalTime::CurDateFract(), CalTime::Detail::now.sub_date_fract);
	IConsolePrint(CC_DEFAULT, "Economy Date: {:04}-{:02}-{:02} ({}), fract: {}, tick skip: {}", EconTime::CurYear(), EconTime::CurMonth() + 1, EconTime::CurDay(), EconTime::CurDate(), EconTime::CurDateFract(), TickSkipCounter());
	IConsolePrint(CC_DEFAULT, "Period display offset: {}", EconTime::Detail::period_display_offset);
	IConsolePrint(CC_DEFAULT, "Elapsed years: {}", EconTime::Detail::years_elapsed);
	IConsolePrint(CC_DEFAULT, "Tick counter: {}", _tick_counter);
	IConsolePrint(CC_DEFAULT, "Tick counter (scaled): {}", _scaled_tick_counter);
	IConsolePrint(CC_DEFAULT, "State ticks: {} (offset: {})", _state_ticks, DateDetail::_state_ticks_offset);
	IConsolePrint(CC_DEFAULT, "Effective economy speed reduction factor: {}", DayLengthFactor());
	if (!CalTime::IsCalendarFrozen()) IConsolePrint(CC_DEFAULT, "Ticks per calendar day: {}", TicksPerCalendarDay());
	if (_settings_time.time_in_minutes) {
		Ticks remainder = _settings_time.GetTickMinutesRemainder(_state_ticks);
		ClockFaceMinutes hhmm = _settings_time.ToTickMinutes(_state_ticks).ToClockFaceMinutes();
		IConsolePrint(CC_DEFAULT, "Timetable time: {:02}:{:02} + {} ticks", hhmm.ClockHour(), hhmm.ClockMinute(), remainder);
	}
	return true;
}

DEF_CONSOLE_CMD(ConDumpCommandLog)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump log of recently executed commands.");
		return true;
	}

	format_buffer buffer;
	DumpCommandLog(buffer);
	PrintLineByLine(buffer);
	return true;
}

DEF_CONSOLE_CMD(ConDumpSpecialEventsLog)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump log of special events.");
		return true;
	}

	format_buffer buffer;
	DumpSpecialEventsLog(buffer);
	PrintLineByLine(buffer);
	return true;
}

DEF_CONSOLE_CMD(ConDumpDesyncMsgLog)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump log of desync messages.");
		return true;
	}

	format_buffer buffer;
	DumpDesyncMsgLog(buffer);
	PrintLineByLine(buffer);
	return true;
}

DEF_CONSOLE_CMD(ConDumpInflation)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump inflation data.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "interest_rate: {}", _economy.interest_rate);
	IConsolePrint(CC_DEFAULT, "infl_amount: {}", _economy.infl_amount);
	IConsolePrint(CC_DEFAULT, "infl_amount_pr: {}", _economy.infl_amount_pr);
	IConsolePrint(CC_DEFAULT, "inflation_prices: {}", _economy.inflation_prices / 65536.0);
	IConsolePrint(CC_DEFAULT, "inflation_payment: {}", _economy.inflation_payment / 65536.0);
	IConsolePrint(CC_DEFAULT, "inflation ratio: {}", (double) _economy.inflation_prices / (double) _economy.inflation_payment);
	return true;
}

DEF_CONSOLE_CMD(ConDumpCpdpStats)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump cargo packet deferred payment stats.");
		return true;
	}

	extern std::string DumpCargoPacketDeferredPaymentStats();
	PrintLineByLine(DumpCargoPacketDeferredPaymentStats());
	return true;
}

DEF_CONSOLE_CMD(ConVehicleStats)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump vehicle stats.");
		return true;
	}

	extern void DumpVehicleStats(format_target &buffer);
	format_buffer buffer;
	DumpVehicleStats(buffer);
	PrintLineByLine(buffer);
	return true;
}

DEF_CONSOLE_CMD(ConMapStats)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump map stats.");
		return true;
	}

	extern void DumpMapStats(format_target &buffer);
	format_buffer buffer;
	DumpMapStats(buffer);
	PrintLineByLine(buffer);

	IConsolePrint(CC_DEFAULT, "");
	IConsolePrint(CC_DEFAULT, "towns: {}", Town::GetNumItems());
	IConsolePrint(CC_DEFAULT, "industries: {}", Industry::GetNumItems());
	IConsolePrint(CC_DEFAULT, "objects: {}", Object::GetNumItems());
	return true;
}

DEF_CONSOLE_CMD(ConStFlowStats)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump station flow stats.");
		return true;
	}

	extern void DumpStationFlowStats(format_target &buffer);
	format_buffer buffer;
	DumpStationFlowStats(buffer);
	PrintLineByLine(buffer);
	return true;
}

DEF_CONSOLE_CMD(ConSlotsStats)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump routing restrictions slots and counter stats.");
		return true;
	}

	extern void DumpTraceRestrictSlotsStats(format_target &buffer);
	format_buffer buffer;
	DumpTraceRestrictSlotsStats(buffer);
	PrintLineByLine(buffer);
	return true;
}

DEF_CONSOLE_CMD(ConDumpGameEvents)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump game events.");
		return true;
	}

	format_buffer buffer;
	DumpGameEventFlags(_game_events_since_load, buffer);
	IConsolePrint(CC_DEFAULT, "Since load: {}", buffer);
	buffer.clear();
	DumpGameEventFlags(_game_events_overall, buffer);
	IConsolePrint(CC_DEFAULT, "Overall: {}", buffer);
	return true;
}

DEF_CONSOLE_CMD(ConDumpLoadDebugLog)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump load debug log.");
		return true;
	}

	std::string dbgl = _loadgame_DBGL_data;
	PrintLineByLine(dbgl.data());
	return true;
}

DEF_CONSOLE_CMD(ConDumpLoadDebugConfig)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump load debug config.");
		return true;
	}

	std::string dbgc = _loadgame_DBGC_data;
	PrintLineByLine(dbgc.data());
	return true;
}


DEF_CONSOLE_CMD(ConDumpLinkgraphJobs)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump link-graph jobs.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "{} link graph jobs", LinkGraphJob::GetNumItems());
	for (const LinkGraphJob *lgj : LinkGraphJob::Iterate()) {
		IConsolePrint(CC_DEFAULT, "  Job: {:5}, nodes: {}, cost: {}, started: {}, ends in: {}, duration: {}",
				lgj->index, lgj->Graph().Size(), lgj->Graph().CalculateCostEstimate(),
				(int64_t)(lgj->StartTick() - _scaled_tick_counter), (int64_t)(lgj->JoinTick() - _scaled_tick_counter), (lgj->JoinTick() - lgj->StartTick()));
	 }
	return true;
}

DEF_CONSOLE_CMD(ConDumpRoadTypes)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump road/tram types.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "  Flags:");
	IConsolePrint(CC_DEFAULT, "    c = catenary");
	IConsolePrint(CC_DEFAULT, "    l = no level crossings");
	IConsolePrint(CC_DEFAULT, "    X = no houses");
	IConsolePrint(CC_DEFAULT, "    h = hidden");
	IConsolePrint(CC_DEFAULT, "    T = buildable by towns");
	IConsolePrint(CC_DEFAULT, "  Extra flags:");
	IConsolePrint(CC_DEFAULT, "    s = not available to scripts (AI/GS)");
	IConsolePrint(CC_DEFAULT, "    t = not modifiable by towns");
	IConsolePrint(CC_DEFAULT, "    T = disallow tunnels");
	IConsolePrint(CC_DEFAULT, "    c = disallow collisions with trains for vehicles of this type");

	btree::btree_map<uint32_t, const GRFFile *> grfs;
	for (RoadType rt = ROADTYPE_BEGIN; rt < ROADTYPE_END; rt++) {
		const RoadTypeInfo *rti = GetRoadTypeInfo(rt);
		if (rti->label == 0) continue;
		uint32_t grfid = 0;
		const GRFFile *grf = rti->grffile[ROTSG_GROUND];
		if (grf == nullptr) {
			uint32_t str_grfid = GetStringGRFID(rti->strings.name);
			if (str_grfid != 0) {
				extern GRFFile *GetFileByGRFID(uint32_t grfid);
				grf = GetFileByGRFID(grfid);
			}
		}
		if (grf != nullptr) {
			grfid = grf->grfid;
			grfs.insert(std::pair<uint32_t, const GRFFile *>(grfid, grf));
		}
		IConsolePrint(CC_DEFAULT, "  {:2} {} {}, Flags: {}{}{}{}{}, Extra Flags: {}{}{}{}, GRF: {:08X},{}",
				(uint) rt,
				RoadTypeIsTram(rt) ? "Tram" : "Road",
				NewGRFLabelDumper().Label(rti->label),
				rti->flags.Test(RoadTypeFlag::Catenary)        ? 'c' : '-',
				rti->flags.Test(RoadTypeFlag::NoLevelCrossing) ? 'l' : '-',
				rti->flags.Test(RoadTypeFlag::NoHouses)        ? 'X' : '-',
				rti->flags.Test(RoadTypeFlag::Hidden)          ? 'h' : '-',
				rti->flags.Test(RoadTypeFlag::TownBuild)       ? 'T' : '-',
				rti->extra_flags.Test(RoadTypeExtraFlag::NotAvailableAiGs)   ? 's' : '-',
				rti->extra_flags.Test(RoadTypeExtraFlag::NoTownModification) ? 't' : '-',
				rti->extra_flags.Test(RoadTypeExtraFlag::NoTunnels)          ? 'T' : '-',
				rti->extra_flags.Test(RoadTypeExtraFlag::NoTrainCollision)   ? 'c' : '-',
				std::byteswap(grfid),
				GetStringPtr(rti->strings.name)
		);
	}
	for (const auto &grf : grfs) {
		IConsolePrint(CC_DEFAULT, "  GRF: {:08X} = {}", std::byteswap(grf.first), grf.second->filename);
	}
	return true;
}

DEF_CONSOLE_CMD(ConDumpRailTypes)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump rail types.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "  Flags:");
	IConsolePrint(CC_DEFAULT, "    c = catenary");
	IConsolePrint(CC_DEFAULT, "    l = no level crossings");
	IConsolePrint(CC_DEFAULT, "    h = hidden");
	IConsolePrint(CC_DEFAULT, "    s = no sprite combine");
	IConsolePrint(CC_DEFAULT, "    a = allow 90° turns");
	IConsolePrint(CC_DEFAULT, "    d = disallow 90° turns");
	IConsolePrint(CC_DEFAULT, "  Ctrl flags:");
	IConsolePrint(CC_DEFAULT, "    p = signal graphics callback enabled for programmable pre-signals");
	IConsolePrint(CC_DEFAULT, "    r = signal graphics callback restricted signal flag enabled");

	btree::btree_map<uint32_t, const GRFFile *> grfs;
	for (RailType rt = RAILTYPE_BEGIN; rt < RAILTYPE_END; rt++) {
		const RailTypeInfo *rti = GetRailTypeInfo(rt);
		if (rti->label == 0) continue;
		uint32_t grfid = 0;
		const GRFFile *grf = rti->grffile[RTSG_GROUND];
		if (grf == nullptr) {
			uint32_t str_grfid = GetStringGRFID(rti->strings.name);
			if (str_grfid != 0) {
				extern GRFFile *GetFileByGRFID(uint32_t grfid);
				grf = GetFileByGRFID(grfid);
			}
		}
		if (grf != nullptr) {
			grfid = grf->grfid;
			grfs.insert(std::pair<uint32_t, const GRFFile *>(grfid, grf));
		}
		IConsolePrint(CC_DEFAULT, "  {:2} {}, Flags: {}{}{}{}{}{}, Ctrl Flags: {}{}{}{}{}, GRF: {:08X}, {}",
				(uint) rt,
				NewGRFLabelDumper().Label(rti->label),
				rti->flags.Test(RailTypeFlag::Catenary)        ? 'c' : '-',
				rti->flags.Test(RailTypeFlag::NoLevelCrossing) ? 'l' : '-',
				rti->flags.Test(RailTypeFlag::Hidden)          ? 'h' : '-',
				rti->flags.Test(RailTypeFlag::NoSpriteCombine) ? 's' : '-',
				rti->flags.Test(RailTypeFlag::Allow90Deg)      ? 'a' : '-',
				rti->flags.Test(RailTypeFlag::Disallow90Deg)   ? 'd' : '-',
				rti->ctrl_flags.Test(RailTypeCtrlFlag::SigSpriteProgSig)         ? 'p' : '-',
				rti->ctrl_flags.Test(RailTypeCtrlFlag::SigSpriteRestrictedSig)   ? 'r' : '-',
				rti->ctrl_flags.Test(RailTypeCtrlFlag::NoRealisticBraking)       ? 'b' : '-',
				rti->ctrl_flags.Test(RailTypeCtrlFlag::SigSpriteRecolourEnabled) ? 'c' : '-',
				rti->ctrl_flags.Test(RailTypeCtrlFlag::SigSpriteNoEntry)         ? 'n' : '-',
				std::byteswap(grfid),
				GetStringPtr(rti->strings.name)
		);
	}
	for (const auto &grf : grfs) {
		IConsolePrint(CC_DEFAULT, "  GRF: {:08X} = {}", std::byteswap(grf.first), grf.second->filename);
	}
	return true;
}

DEF_CONSOLE_CMD(ConDumpBridgeTypes)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump bridge types.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "  Ctrl flags:");
	IConsolePrint(CC_DEFAULT, "    c = custom pillar flags");
	IConsolePrint(CC_DEFAULT, "    i = invalid pillar flags");
	IConsolePrint(CC_DEFAULT, "    t = not available to towns");
	IConsolePrint(CC_DEFAULT, "    s = not available to scripts (AI/GS)");

	btree::btree_set<uint32_t> grfids;
	for (BridgeType bt = 0; bt < MAX_BRIDGES; bt++) {
		const BridgeSpec *spec = GetBridgeSpec(bt);
		uint32_t grfid = GetStringGRFID(spec->material);
		if (grfid != 0) grfids.insert(grfid);
		IConsolePrint(CC_DEFAULT, "  {:2} Year: {:7}, Min: {:3}, Max: {:5}, Flags: {:02X}, Ctrl Flags: {}{}{}{}, Pillars: {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}, GRF: {:08X}, {}",
				(uint) bt,
				spec->avail_year,
				spec->min_length,
				spec->max_length,
				spec->flags,
				HasBit(spec->ctrl_flags, BSCF_CUSTOM_PILLAR_FLAGS) ? 'c' : '-',
				HasBit(spec->ctrl_flags, BSCF_INVALID_PILLAR_FLAGS) ? 'i' : '-',
				HasBit(spec->ctrl_flags, BSCF_NOT_AVAILABLE_TOWN) ? 't' : '-',
				HasBit(spec->ctrl_flags, BSCF_NOT_AVAILABLE_AI_GS) ? 's' : '-',
				spec->pillar_flags[0],
				spec->pillar_flags[1],
				spec->pillar_flags[2],
				spec->pillar_flags[3],
				spec->pillar_flags[4],
				spec->pillar_flags[5],
				spec->pillar_flags[6],
				spec->pillar_flags[7],
				spec->pillar_flags[8],
				spec->pillar_flags[9],
				spec->pillar_flags[10],
				spec->pillar_flags[11],
				std::byteswap(grfid),
				GetStringPtr(spec->material)
		);
	}
	for (uint32_t grfid : grfids) {
		extern GRFFile *GetFileByGRFID(uint32_t grfid);
		const GRFFile *grffile = GetFileByGRFID(grfid);
		IConsolePrint(CC_DEFAULT, "  GRF: {:08X} = {}", std::byteswap(grfid), grffile ? (std::string_view)grffile->filename : "????");
	}
	return true;
}

DEF_CONSOLE_CMD(ConDumpCargoTypes)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump cargo types.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "  Cargo classes:");
	IConsolePrint(CC_DEFAULT, "    p = passenger");
	IConsolePrint(CC_DEFAULT, "    m = mail");
	IConsolePrint(CC_DEFAULT, "    x = express");
	IConsolePrint(CC_DEFAULT, "    a = armoured");
	IConsolePrint(CC_DEFAULT, "    b = bulk");
	IConsolePrint(CC_DEFAULT, "    g = piece goods");
	IConsolePrint(CC_DEFAULT, "    l = liquid");
	IConsolePrint(CC_DEFAULT, "    r = refrigerated");
	IConsolePrint(CC_DEFAULT, "    h = hazardous");
	IConsolePrint(CC_DEFAULT, "    c = covered/sheltered");
	IConsolePrint(CC_DEFAULT, "    o = oversized");
	IConsolePrint(CC_DEFAULT, "    d = powderized");
	IConsolePrint(CC_DEFAULT, "    n = not pourable");
	IConsolePrint(CC_DEFAULT, "    e = potable");
	IConsolePrint(CC_DEFAULT, "    i = non-potable");
	IConsolePrint(CC_DEFAULT, "    S = special");
	IConsolePrint(CC_DEFAULT, "  Town acceptance effect:");
	IConsolePrint(CC_DEFAULT, "    P = passenger");
	IConsolePrint(CC_DEFAULT, "    M = mail");
	IConsolePrint(CC_DEFAULT, "    G = goods");
	IConsolePrint(CC_DEFAULT, "    W = water");
	IConsolePrint(CC_DEFAULT, "    F = food");

	static const char tae_char[NUM_TAE] = { '-', 'P', 'M', 'G', 'W', 'F' };

	btree::btree_map<uint32_t, const GRFFile *> grfs;
	for (CargoType i = 0; i < NUM_CARGO; i++) {
		const CargoSpec *spec = CargoSpec::Get(i);
		if (!spec->IsValid()) continue;
		uint32_t grfid = 0;
		const GRFFile *grf = spec->grffile;
		if (grf == nullptr) {
			uint32_t str_grfid = GetStringGRFID(spec->name);
			if (str_grfid != 0) {
				extern GRFFile *GetFileByGRFID(uint32_t grfid);
				grf = GetFileByGRFID(grfid);
			}
		}
		if (grf != nullptr) {
			grfid = grf->grfid;
			grfs.insert(std::pair<uint32_t, const GRFFile *>(grfid, grf));
		}
		IConsolePrint(CC_DEFAULT, "  {:2} Bit: {:2}, Label: {}, Callback mask: 0x{:02X}, Cargo class: {}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}, Town: {}, GRF: {:08X}, {}",
				(uint) i,
				spec->bitnum,
				NewGRFLabelDumper().Label(spec->label.base()),
				spec->callback_mask,
				(spec->classes & CC_PASSENGERS)   != 0 ? 'p' : '-',
				(spec->classes & CC_MAIL)         != 0 ? 'm' : '-',
				(spec->classes & CC_EXPRESS)      != 0 ? 'x' : '-',
				(spec->classes & CC_ARMOURED)     != 0 ? 'a' : '-',
				(spec->classes & CC_BULK)         != 0 ? 'b' : '-',
				(spec->classes & CC_PIECE_GOODS)  != 0 ? 'g' : '-',
				(spec->classes & CC_LIQUID)       != 0 ? 'l' : '-',
				(spec->classes & CC_REFRIGERATED) != 0 ? 'r' : '-',
				(spec->classes & CC_HAZARDOUS)    != 0 ? 'h' : '-',
				(spec->classes & CC_COVERED)      != 0 ? 'c' : '-',
				(spec->classes & CC_OVERSIZED)    != 0 ? 'o' : '-',
				(spec->classes & CC_POWDERIZED)   != 0 ? 'd' : '-',
				(spec->classes & CC_NOT_POURABLE) != 0 ? 'n' : '-',
				(spec->classes & CC_POTABLE)      != 0 ? 'e' : '-',
				(spec->classes & CC_NON_POTABLE)  != 0 ? 'i' : '-',
				(spec->classes & CC_SPECIAL)      != 0 ? 'S' : '-',
				tae_char[spec->town_acceptance_effect - TAE_BEGIN],
				std::byteswap(grfid),
				GetStringPtr(spec->name)
		);
	}
	for (const auto &grf : grfs) {
		IConsolePrint(CC_DEFAULT, "  GRF: {:08X} = {}", std::byteswap(grf.first), grf.second->filename);
	}
	return true;
}

DEF_CONSOLE_CMD(ConDumpVehicle)
{
	if (argc != 2) {
		IConsolePrint(CC_HELP, "Debug: Show vehicle information.  Usage: 'dump_vehicle <vehicle-id>'");
		return true;
	}

	const Vehicle *v = Vehicle::GetIfValid(atoi(argv[1]));
	if (v != nullptr) {
		IConsolePrint(CC_DEFAULT, "{}", VehicleInfoDumper(v));
	} else {
		IConsolePrint(CC_DEFAULT, "No such vehicle");
	}

	return true;
}

/**
 * Dump the state of a tile on the map.
 * param x tile number or tile x coordinate.
 * param y optional y coordinate.
 * @note When only one argument is given it is interpreted as the tile number.
 *       When two arguments are given, they are interpreted as the tile's x
 *       and y coordinates.
 * @return True when either console help was shown or a proper amount of parameters given.
 */
DEF_CONSOLE_CMD(ConDumpTile)
{
	switch (argc) {
		case 0:
			IConsolePrint(CC_HELP, "Dump the map state of a given tile.");
			IConsolePrint(CC_HELP, "Usage: 'dump_tile <tile>' or 'dump_tile <x> <y>'");
			IConsolePrint(CC_HELP, "Numbers can be either decimal (34161) or hexadecimal (0x4a5B).");
			return true;

		case 2: {
			uint32_t result;
			if (GetArgumentInteger(&result, argv[1])) {
				if (result >= Map::Size()) {
					IConsolePrint(CC_ERROR, "Tile does not exist.");
					return true;
				}
				format_buffer buffer;
				buffer.append("  ");
				DumpTileInfo(buffer, (TileIndex)result);
				IConsolePrint(CC_DEFAULT, buffer.to_string());
				return true;
			}
			break;
		}

		case 3: {
			uint32_t x, y;
			if (GetArgumentInteger(&x, argv[1]) && GetArgumentInteger(&y, argv[2])) {
				if (x >= Map::SizeX() || y >= Map::SizeY()) {
					IConsolePrint(CC_ERROR, "Tile does not exist.");
					return true;
				}
				format_buffer buffer;
				buffer.append("  ");
				DumpTileInfo(buffer, TileXY(x, y));
				IConsolePrint(CC_DEFAULT, buffer.to_string());
				return true;
			}
			break;
		}
	}

	return false;
}

DEF_CONSOLE_CMD(ConDumpGrfCargoTables)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump GRF cargo translation tables.");
		return true;
	}

	const std::vector<GRFFile *> &files = GetAllGRFFiles();

	format_buffer buffer;

	for (const GRFFile *grf : files) {
		if (grf->cargo_list.empty()) continue;

		IConsolePrint(CC_DEFAULT, "[{:08X}] {}: {} cargoes", std::byteswap(grf->grfid), grf->filename, grf->cargo_list.size());

		uint i = 0;
		for (const CargoLabel &cl : grf->cargo_list) {
			buffer.clear();
			for (const CargoSpec *cs : CargoSpec::Iterate()) {
				if (grf->cargo_map[cs->Index()] == i) {
					buffer.format("{}{:02}[{}]", buffer.size() == 0 ? ": " : ", ", cs->Index(), NewGRFLabelDumper().Label(cs->label.base()));
				}
			}
			IConsolePrint(CC_DEFAULT, "  {}{}", NewGRFLabelDumper().Label(cl.base()), buffer);
			i++;
		}
	}

	return true;
}

DEF_CONSOLE_CMD(ConDumpSignalStyles)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump custom signal styles.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "  Flags:");
	IConsolePrint(CC_DEFAULT, "    n = no aspect increment");
	IConsolePrint(CC_DEFAULT, "    a = always reserve through");
	IConsolePrint(CC_DEFAULT, "    l = lookahead aspects set");
	IConsolePrint(CC_DEFAULT, "    o = opposite side");
	IConsolePrint(CC_DEFAULT, "    s = lookahead single signal");
	IConsolePrint(CC_DEFAULT, "    c = combined normal and shunt");
	IConsolePrint(CC_DEFAULT, "    r = realistic braking only");
	IConsolePrint(CC_DEFAULT, "    b = both sides");
	IConsolePrint(CC_DEFAULT, "  Extra aspects: {}", _extra_aspects);
	IConsolePrint(CC_DEFAULT, "  Default style extra aspects: {}", _default_signal_style_lookahead_extra_aspects);

	btree::btree_map<uint32_t, const GRFFile *> grfs;
	for (uint8_t i = 0; i < _num_new_signal_styles; i++) {
		const NewSignalStyle &style = _new_signal_styles[i];

		uint32_t grfid = 0;
		if (style.grffile != nullptr) {
			grfid = style.grffile->grfid;
			grfs.insert(std::pair<uint32_t, const GRFFile *>(grfid, style.grffile));
		}
		IConsolePrint(CC_DEFAULT, "  {:2}: GRF: {:08X}, Local: {:2}, Extra aspects: {:3}, Flags: {}{}{}{}{}{}{}{}, {}",
				(uint) (i + 1),
				std::byteswap(grfid),
				style.grf_local_id,
				style.lookahead_extra_aspects,
				HasBit(style.style_flags, NSSF_NO_ASPECT_INC)           ? 'n' : '-',
				HasBit(style.style_flags, NSSF_ALWAYS_RESERVE_THROUGH)  ? 'a' : '-',
				HasBit(style.style_flags, NSSF_LOOKAHEAD_ASPECTS_SET)   ? 'l' : '-',
				HasBit(style.style_flags, NSSF_OPPOSITE_SIDE)           ? 'o' : '-',
				HasBit(style.style_flags, NSSF_LOOKAHEAD_SINGLE_SIGNAL) ? 's' : '-',
				HasBit(style.style_flags, NSSF_COMBINED_NORMAL_SHUNT)   ? 'c' : '-',
				HasBit(style.style_flags, NSSF_REALISTIC_BRAKING_ONLY)  ? 'r' : '-',
				HasBit(style.style_flags, NSSF_BOTH_SIDES)              ? 'b' : '-',
				GetStringPtr(style.name)
		);
	}
	for (const auto &grf : grfs) {
		IConsolePrint(CC_DEFAULT, "  GRF: {:08X} = {}", std::byteswap(grf.first), grf.second->filename);
	}

	return true;
}

DEF_CONSOLE_CMD(ConSpriteCacheStats)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump sprite cache stats.");
		return true;
	}

	extern void DumpSpriteCacheStats(format_target &buffer);
	format_buffer buffer;
	DumpSpriteCacheStats(buffer);
	PrintLineByLine(buffer);
	return true;
}

DEF_CONSOLE_CMD(ConDumpVersion)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Dump version info");
		return true;
	}

	format_buffer buffer;
	CrashLog::VersionInfoLog(buffer);
	PrintLineByLine(buffer);
	return true;
}

DEF_CONSOLE_CMD(ConCheckCaches)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Debug: Check caches. Usage: 'check_caches [<broadcast>]'");
		return true;
	}

	if (argc > 2) return false;

	bool broadcast = (argc == 2 && atoi(argv[1]) > 0 && (!_networking || _network_server));
	if (broadcast) {
		Command<CMD_DESYNC_CHECK>::Post();
	} else {
		auto logger = [&](std::string_view str) {
			IConsolePrint(CC_WARNING, std::string{str});
		};
		CheckCaches(true, logger, CHECK_CACHE_ALL | CHECK_CACHE_EMIT_LOG);
	}

	return true;
}

DEF_CONSOLE_CMD(ConShowTownWindow)
{
	if (argc != 2) {
		IConsolePrint(CC_HELP, "Debug: Show town window.  Usage: 'show_town_window <town-id>'");
		return true;
	}

	if (_game_mode != GM_NORMAL && _game_mode != GM_EDITOR) {
		return true;
	}

	TownID town_id = (TownID)(atoi(argv[1]));
	if (!Town::IsValidID(town_id)) {
		return true;
	}

	ShowTownViewWindow(town_id);

	return true;
}

DEF_CONSOLE_CMD(ConShowStationWindow)
{
	if (argc != 2) {
		IConsolePrint(CC_HELP, "Debug: Show station window.  Usage: 'show_station_window <station-id>'");
		return true;
	}

	if (_game_mode != GM_NORMAL && _game_mode != GM_EDITOR) {
		return true;
	}

	const BaseStation *bst = BaseStation::GetIfValid(atoi(argv[1]));
	if (bst == nullptr) return true;
	if (bst->facilities & FACIL_WAYPOINT) {
		ShowWaypointWindow(Waypoint::From(bst));
	} else {
		ShowStationViewWindow(bst->index);
	}

	return true;
}

DEF_CONSOLE_CMD(ConShowIndustryWindow)
{
	if (argc != 2) {
		IConsolePrint(CC_HELP, "Debug: Show industry window.  Usage: 'show_industry_window <industry-id>'");
		return true;
	}

	if (_game_mode != GM_NORMAL && _game_mode != GM_EDITOR) {
		return true;
	}

	IndustryID ind_id = (IndustryID)(atoi(argv[1]));
	if (!Industry::IsValidID(ind_id)) {
		return true;
	}

	extern void ShowIndustryViewWindow(IndustryID industry);
	ShowIndustryViewWindow(ind_id);

	return true;
}

DEF_CONSOLE_CMD(ConViewportDebug)
{
	if (argc < 1 || argc > 2) {
		IConsolePrint(CC_HELP, "Debug: viewports flags.  Usage: 'viewport_debug [<flags>]'");
		IConsolePrint(CC_HELP, "   1: VDF_DIRTY_BLOCK_PER_DRAW");
		IConsolePrint(CC_HELP, "   2: VDF_DIRTY_WHOLE_VIEWPORT");
		IConsolePrint(CC_HELP, "   4: VDF_DIRTY_BLOCK_PER_SPLIT");
		IConsolePrint(CC_HELP, "   8: VDF_DISABLE_DRAW_SPLIT");
		IConsolePrint(CC_HELP, "  10: VDF_SHOW_NO_LANDSCAPE_MAP_DRAW");
		IConsolePrint(CC_HELP, "  20: VDF_DISABLE_LANDSCAPE_CACHE");
		IConsolePrint(CC_HELP, "  40: VDF_DISABLE_THREAD");
		return true;
	}

	extern uint32_t _viewport_debug_flags;
	if (argc == 1) {
		IConsolePrint(CC_DEFAULT, "Viewport debug flags: {:X}", _viewport_debug_flags);
	} else {
		_viewport_debug_flags = std::strtoul(argv[1], nullptr, 16);
	}

	return true;
}

DEF_CONSOLE_CMD(ConViewportMarkDirty)
{
	if (argc < 3 || argc > 5) {
		IConsolePrint(CC_HELP, "Debug: Mark main viewport dirty.  Usage: 'viewport_mark_dirty <x> <y> [<w> <h>]'");
		return true;
	}

	Viewport *vp = FindWindowByClass(WC_MAIN_WINDOW)->viewport;
	uint l = std::strtoul(argv[1], nullptr, 0);
	uint t = std::strtoul(argv[2], nullptr, 0);
	uint r = std::min<uint>(l + ((argc > 3) ? strtoul(argv[3], nullptr, 0) : 1), vp->dirty_blocks_per_row);
	uint b = std::min<uint>(t + ((argc > 4) ? strtoul(argv[4], nullptr, 0) : 1), vp->dirty_blocks_per_column);
	for (uint x = l; x < r; x++) {
		for (uint y = t; y < b; y++) {
			SetBit(vp->dirty_blocks[(x * vp->dirty_blocks_column_pitch) + (y / VP_BLOCK_BITS)], y % VP_BLOCK_BITS);
		}
	}
	vp->is_dirty = true;

	return true;
}


DEF_CONSOLE_CMD(ConViewportMarkStationOverlayDirty)
{
	if (argc != 2) {
		IConsolePrint(CC_HELP, "Debug: Mark main viewport link graph overlay station links.  Usage: 'viewport_mark_dirty_st_overlay <station-id>'");
		return true;
	}

	if (_game_mode != GM_NORMAL && _game_mode != GM_EDITOR) {
		return true;
	}

	const Station *st = Station::GetIfValid(atoi(argv[1]));
	if (st == nullptr) return true;
	MarkAllViewportOverlayStationLinksDirty(st);

	return true;
}

DEF_CONSOLE_CMD(ConGfxDebug)
{
	if (argc < 1 || argc > 2) {
		IConsolePrint(CC_HELP, "Debug: gfx flags.  Usage: 'gfx_debug [<flags>]'");
		IConsolePrint(CC_HELP, "  1: GDF_SHOW_WINDOW_DIRTY");
		IConsolePrint(CC_HELP, "  2: GDF_SHOW_WIDGET_DIRTY");
		IConsolePrint(CC_HELP, "  4: GDF_SHOW_RECT_DIRTY");
		return true;
	}

	extern uint32_t _gfx_debug_flags;
	if (argc == 1) {
		IConsolePrint(CC_DEFAULT, "Gfx debug flags: {:X}", _gfx_debug_flags);
	} else {
		_gfx_debug_flags = std::strtoul(argv[1], nullptr, 16);
	}

	return true;
}

DEF_CONSOLE_CMD(ConCSleep)
{
	if (argc != 2) {
		IConsolePrint(CC_HELP, "Debug: Sleep.  Usage: 'csleep <milliseconds>'");
		return true;
	}

	CSleep(atoi(argv[1]));

	return true;
}

DEF_CONSOLE_CMD(ConRecalculateRoadCachedOneWayStates)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Debug: Recalculate road cached one way states");
		return true;
	}

	extern void RecalculateRoadCachedOneWayStates();
	RecalculateRoadCachedOneWayStates();

	return true;
}

DEF_CONSOLE_CMD(ConMiscDebug)
{
	if (argc < 1 || argc > 2) {
		IConsolePrint(CC_HELP, "Debug: misc flags.  Usage: 'misc_debug [<flags>]'");
		IConsolePrint(CC_HELP, "  1: MDF_OVERHEAT_BREAKDOWN_OPEN_WIN");
		IConsolePrint(CC_HELP, "  2: MDF_ZONING_DEBUG_MODES");
		IConsolePrint(CC_HELP, " 10: MDF_NEWGRF_SG_SAVE_RAW");
		IConsolePrint(CC_HELP, " 20: MDF_SPECIAL_CMDS");
		return true;
	}

	if (argc == 1) {
		IConsolePrint(CC_DEFAULT, "Misc debug flags: {:X}", _misc_debug_flags);
	} else {
		_misc_debug_flags = std::strtoul(argv[1], nullptr, 16);
	}

	return true;
}

DEF_CONSOLE_CMD(ConSetNewGRFOptimiserFlags)
{
	if (argc < 1 || argc > 2) {
		IConsolePrint(CC_HELP, "Debug: misc set_newgrf_optimiser_flags.  Usage: 'set_newgrf_optimiser_flags [<flags>]'");
		return true;
	}

	if (argc == 1) {
		IConsolePrint(CC_DEFAULT, "NewGRF optimiser flags: {:X}", _settings_game.debug.newgrf_optimiser_flags);
	} else {
		if (_game_mode == GM_MENU || (_networking && !_network_server)) {
			IConsolePrint(CC_ERROR, "This command is only available in-game and in the editor, and not as a network client.");
			return true;
		}
		extern uint NetworkClientCount();
		if (_networking && NetworkClientCount() > 1) {
			IConsolePrint(CC_ERROR, "This command is not available when network clients are connected.");
			return true;
		}

		uint value = std::strtoul(argv[1], nullptr, 16);
		if (_settings_game.debug.newgrf_optimiser_flags == value) return true;
		_settings_game.debug.newgrf_optimiser_flags = value;

		ReloadNewGRFData();

		extern void PostCheckNewGRFLoadWarnings();
		PostCheckNewGRFLoadWarnings();
	}

	return true;
}

DEF_CONSOLE_CMD(ConDoDisaster)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Debug: Do disaster");
		return true;
	}

	extern void DoDisaster();
	DoDisaster();

	return true;
}

DEF_CONSOLE_CMD(ConBankruptCompany)
{
	if (argc != 2) {
		IConsolePrint(CC_HELP, "Debug: Mark company as bankrupt.  Usage: 'bankrupt_company <company-id>'");
		return true;
	}

	if (_game_mode != GM_NORMAL) {
		IConsolePrint(CC_ERROR, "Companies can only be managed in a game.");
		return true;
	}

	CompanyID company_id = (CompanyID)(atoi(argv[1]) - 1);
	if (!Company::IsValidID(company_id)) {
		IConsolePrint(CC_DEFAULT, "Unknown company. Company range is between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	Company *c = Company::Get(company_id);
	c->bankrupt_value = 42;
	c->bankrupt_asked = CompanyMask{}.Set(c->index); // Don't ask the owner
	c->bankrupt_timeout = 0;
	c->money = INT64_MIN / 2;
	IConsolePrint(CC_DEFAULT, "Company marked as bankrupt.");

	return true;
}

DEF_CONSOLE_CMD(ConDeleteCompany)
{
	if (argc != 2) {
		IConsolePrint(CC_HELP, "Debug: Delete company.  Usage: 'delete_company <company-id>'");
		return true;
	}

	if (_game_mode != GM_NORMAL) {
		IConsolePrint(CC_ERROR, "Companies can only be managed in a game.");
		return true;
	}

	CompanyID company_id = (CompanyID)(atoi(argv[1]) - 1);
	if (!Company::IsValidID(company_id)) {
		IConsolePrint(CC_DEFAULT, "Unknown company. Company range is between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	if (company_id == _local_company) {
		IConsolePrint(CC_ERROR, "Cannot delete current company.");
		return true;
	}

	Command<CMD_COMPANY_CTRL>::Post(CCA_DELETE, company_id, CRR_MANUAL, INVALID_CLIENT_ID, {});
	IConsolePrint(CC_DEFAULT, "Company deleted.");

	return true;
}

DEF_CONSOLE_CMD(ConNewGRFProfile)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Collect performance data about NewGRF sprite requests and callbacks. Sub-commands can be abbreviated.");
		IConsolePrint(CC_HELP, "Usage: 'newgrf_profile [list]':");
		IConsolePrint(CC_HELP, "  List all NewGRFs that can be profiled, and their status.");
		IConsolePrint(CC_HELP, "Usage: 'newgrf_profile select <grf-num>...':");
		IConsolePrint(CC_HELP, "  Select one or more GRFs for profiling.");
		IConsolePrint(CC_HELP, "Usage: 'newgrf_profile unselect <grf-num>...':");
		IConsolePrint(CC_HELP, "  Unselect one or more GRFs from profiling. Use the keyword \"all\" instead of a GRF number to unselect all. Removing an active profiler aborts data collection.");
		IConsolePrint(CC_HELP, "Usage: 'newgrf_profile start [<num-ticks>]':");
		IConsolePrint(CC_HELP, "  Begin profiling all selected GRFs. If a number of ticks is provided, profiling stops after that many game ticks. There are 74 ticks in a calendar day.");
		IConsolePrint(CC_HELP, "Usage: 'newgrf_profile stop':");
		IConsolePrint(CC_HELP, "  End profiling and write the collected data to CSV files.");
		IConsolePrint(CC_HELP, "Usage: 'newgrf_profile abort':");
		IConsolePrint(CC_HELP, "  End profiling and discard all collected data.");
		return true;
	}

	const std::vector<GRFFile *> &files = GetAllGRFFiles();

	/* "list" sub-command */
	if (argc == 1 || StrStartsWithIgnoreCase(argv[1], "lis")) {
		IConsolePrint(CC_INFO, "Loaded GRF files:");
		int i = 1;
		for (GRFFile *grf : files) {
			auto profiler = std::ranges::find(_newgrf_profilers, grf, &NewGRFProfiler::grffile);
			bool selected = profiler != _newgrf_profilers.end();
			bool active = selected && profiler->active;
			TextColour tc = active ? TC_LIGHT_BLUE : selected ? TC_GREEN : CC_INFO;
			const char *statustext = active ? " (active)" : selected ? " (selected)" : "";
			IConsolePrint(tc, "{}: [{:08X}] {}{}", i, std::byteswap(grf->grfid), grf->filename, statustext);
			i++;
		}
		return true;
	}

	/* "select" sub-command */
	if (StrStartsWithIgnoreCase(argv[1], "sel") && argc >= 3) {
		for (size_t argnum = 2; argnum < argc; ++argnum) {
			int grfnum = atoi(argv[argnum]);
			if (grfnum < 1 || grfnum > (int)files.size()) { // safe cast, files.size() should not be larger than a few hundred in the most extreme cases
				IConsolePrint(CC_WARNING, "GRF number {} out of range, not added.", grfnum);
				continue;
			}
			GRFFile *grf = files[grfnum - 1];
			if (std::any_of(_newgrf_profilers.begin(), _newgrf_profilers.end(), [&](NewGRFProfiler &pr) { return pr.grffile == grf; })) {
				IConsolePrint(CC_WARNING, "GRF number {} [{:08X}] is already selected for profiling.", grfnum, std::byteswap(grf->grfid));
				continue;
			}
			_newgrf_profilers.emplace_back(grf);
		}
		return true;
	}

	/* "unselect" sub-command */
	if (StrStartsWithIgnoreCase(argv[1], "uns") && argc >= 3) {
		for (size_t argnum = 2; argnum < argc; ++argnum) {
			if (StrEqualsIgnoreCase(argv[argnum], "all")) {
				_newgrf_profilers.clear();
				break;
			}
			int grfnum = atoi(argv[argnum]);
			if (grfnum < 1 || grfnum > (int)files.size()) {
				IConsolePrint(CC_WARNING, "GRF number {} out of range, not removing.", grfnum);
				continue;
			}
			GRFFile *grf = files[grfnum - 1];
			_newgrf_profilers.erase(std::ranges::find(_newgrf_profilers, grf, &NewGRFProfiler::grffile));
		}
		return true;
	}

	/* "start" sub-command */
	if (StrStartsWithIgnoreCase(argv[1], "sta")) {
		std::string grfids;
		size_t started = 0;
		for (NewGRFProfiler &pr : _newgrf_profilers) {
			if (!pr.active) {
				pr.Start();
				started++;

				if (!grfids.empty()) grfids += ", ";
				fmt::format_to(std::back_inserter(grfids), "[{:08X}]", std::byteswap(pr.grffile->grfid));
			}
		}
		if (started > 0) {
			IConsolePrint(CC_DEBUG, "Started profiling for GRFID{} {}.", (started > 1) ? "s" : "", grfids);

			if (argc >= 3) {
				uint64_t ticks = std::max(atoi(argv[2]), 1);
				NewGRFProfiler::StartTimer(ticks);
				IConsolePrint(CC_DEBUG, "Profiling will automatically stop after {} ticks.", ticks);
			}
		} else if (_newgrf_profilers.empty()) {
			IConsolePrint(CC_ERROR, "No GRFs selected for profiling, did not start.");
		} else {
			IConsolePrint(CC_ERROR, "Did not start profiling for any GRFs, all selected GRFs are already profiling.");
		}
		return true;
	}

	/* "stop" sub-command */
	if (StrStartsWithIgnoreCase(argv[1], "sto")) {
		NewGRFProfiler::FinishAll();
		return true;
	}

	/* "abort" sub-command */
	if (StrStartsWithIgnoreCase(argv[1], "abo")) {
		for (NewGRFProfiler &pr : _newgrf_profilers) {
			pr.Abort();
		}
		NewGRFProfiler::AbortTimer();
		return true;
	}

	return false;
}

DEF_CONSOLE_CMD(ConRoadTypeFlagCtl)
{
	if (argc != 3) {
		IConsolePrint(CC_HELP, "Debug: Road/tram type flag control.");
		return true;
	}

	RoadType rt = (RoadType)atoi(argv[1]);
	uint flag = atoi(argv[2]);

	if (rt >= ROADTYPE_END) return true;
	extern RoadTypeInfo _roadtypes[ROADTYPE_END];

	if (flag >= 100) {
		ToggleBit(_roadtypes[rt].extra_flags.edit_base(), flag - 100);
	} else {
		ToggleBit(_roadtypes[rt].flags.edit_base(), flag);
	}

	return true;
}

DEF_CONSOLE_CMD(ConRailTypeMapColourCtl)
{
	if (argc != 3) {
		IConsolePrint(CC_HELP, "Debug: Rail type map colour control.");
		return true;
	}

	RailType rt = (RailType)atoi(argv[1]);
	uint8_t map_colour = atoi(argv[2]);

	if (rt >= RAILTYPE_END) return true;
	extern RailTypeInfo _railtypes[RAILTYPE_END];

	_railtypes[rt].map_colour = map_colour;
	MarkAllViewportMapLandscapesDirty();

	return true;
}

DEF_CONSOLE_CMD(ConSwitchBaseset)
{
	if (argc != 2) {
		IConsolePrint(CC_HELP, "Debug: Try to switch baseset and reload NewGRFs. Usage: 'switch_baseset <baseset-name>'");
		return true;
	}

	for (int i = 0; i < BaseGraphics::GetNumSets(); i++) {
		const GraphicsSet *basegfx = BaseGraphics::GetSet(i);
		if (argv[1] == basegfx->name) {
			extern std::string _switch_baseset;
			_switch_baseset = basegfx->name;
			_check_special_modes = true;
			return true;
		}
	}

	IConsolePrint(CC_WARNING, "No such baseset: {}.", argv[1]);
	return 1;
}

static bool ConConditionalCommon(uint8_t argc, char *argv[], int value, const char *value_name, const char *name)
{
	if (argc < 4) {
		IConsolePrint(CC_WARNING, "- Execute command if {} is within the specified range. Usage: '{} <minimum> <maximum> <command...>'", value_name, name);
		return true;
	}

	int min_value = atoi(argv[1]);
	int max_value = atoi(argv[2]);

	if (value >= min_value && value <= max_value) IConsoleCmdExecTokens(argc - 3, argv + 3);

	return true;
}

DEF_CONSOLE_CMD(ConIfYear)
{
	return ConConditionalCommon(argc, argv, CalTime::CurYear().base(), "the current year (in game)", "if_year");
}

DEF_CONSOLE_CMD(ConIfMonth)
{
	return ConConditionalCommon(argc, argv, CalTime::CurMonth() + 1, "the current month (in game)", "if_month");
}

DEF_CONSOLE_CMD(ConIfDay)
{
	return ConConditionalCommon(argc, argv, CalTime::CurDay(), "the current day of the month (in game)", "if_day");
}

DEF_CONSOLE_CMD(ConIfHour)
{
	TickMinutes minutes = _settings_time.NowInTickMinutes();
	return ConConditionalCommon(argc, argv, minutes.ClockHour(), "the current hour (in game, assuming time is in minutes)", "if_hour");
}

DEF_CONSOLE_CMD(ConIfMinute)
{
	TickMinutes minutes = _settings_time.NowInTickMinutes();
	return ConConditionalCommon(argc, argv, minutes.ClockMinute(), "the current minute (in game, assuming time is in minutes)", "if_minute");
}

DEF_CONSOLE_CMD(ConIfHourMinute)
{
	TickMinutes minutes = _settings_time.NowInTickMinutes();
	return ConConditionalCommon(argc, argv, minutes.ClockHHMM(), "the current hour and minute 0000 - 2359 (in game, assuming time is in minutes)", "if_hour_minute");
}

#ifdef _DEBUG
/******************
 *  debug commands
 ******************/

static void IConsoleDebugLibRegister()
{
	IConsole::CmdRegister("resettile",        ConResetTile);
	IConsole::AliasRegister("dbg_echo",       "echo %A; echo %B");
	IConsole::AliasRegister("dbg_echo2",      "echo %!");
}
#endif

DEF_CONSOLE_CMD(ConFramerate)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Show frame rate and game speed information.");
		return true;
	}

	ConPrintFramerate();
	return true;
}

DEF_CONSOLE_CMD(ConFramerateWindow)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Open the frame rate window.");
		return true;
	}

	if (_network_dedicated) {
		IConsolePrint(CC_ERROR, "Can not open frame rate window on a dedicated server.");
		return false;
	}

	ShowFramerateWindow();
	return true;
}

DEF_CONSOLE_CMD(ConFindNonRealisticBrakingSignal)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Find the next signal tile which prevents enabling of realistic braking");
		return true;
	}

	for (TileIndex t(0); t < Map::Size(); t++) {
		if (IsTileType(t, MP_RAILWAY) && GetRailTileType(t) == RAIL_TILE_SIGNALS) {
			uint signals = GetPresentSignals(t);
			if ((signals & 0x3) & ((signals & 0x3) - 1) || (signals & 0xC) & ((signals & 0xC) - 1)) {
				/* Signals in both directions */
				ScrollMainWindowToTile(t);
				SetRedErrorSquare(t);
				return true;
			}
			if (((signals & 0x3) && IsSignalTypeUnsuitableForRealisticBraking(GetSignalType(t, TRACK_LOWER))) ||
					((signals & 0xC) && IsSignalTypeUnsuitableForRealisticBraking(GetSignalType(t, TRACK_UPPER)))) {
				/* Banned signal types present */
				ScrollMainWindowToTile(t);
				SetRedErrorSquare(t);
				return true;
			}
		}
	}

	return true;
}

DEF_CONSOLE_CMD(ConFindMissingObject)
{
	if (argc == 0) {
		IConsolePrint(CC_HELP, "Find the next object tile where the spec is missing");
		return true;
	}

	extern TileIndex FindMissingObjectTile();
	TileIndex t = FindMissingObjectTile();
	if (t != INVALID_TILE) {
		ScrollMainWindowToTile(t);
		SetRedErrorSquare(t);
	}

	return true;
}


DEF_CONSOLE_CMD(ConDumpInfo)
{
	if (argc != 2) {
		IConsolePrint(CC_HELP, "Dump debugging information.");
		IConsolePrint(CC_HELP, "Usage: 'dump_info roadtypes|railtypes|cargotypes'.");
		IConsolePrint(CC_HELP, "  Show information about road/tram types, rail types or cargo types.");
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "roadtypes")) {
		ConDumpRoadTypes(argc, argv);
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "railtypes")) {
		ConDumpRailTypes(argc, argv);
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "cargotypes")) {
		ConDumpCargoTypes(argc, argv);
		return true;
	}

	return false;
}

/*******************************
 * console command registration
 *******************************/

void IConsoleStdLibRegister()
{
	IConsole::CmdRegister("debug_level",             ConDebugLevel);
	IConsole::CmdRegister("echo",                    ConEcho);
	IConsole::CmdRegister("echoc",                   ConEchoC);
	IConsole::CmdRegister("exec",                    ConExec);
	IConsole::CmdRegister("schedule",                ConSchedule);
	IConsole::CmdRegister("exit",                    ConExit);
	IConsole::CmdRegister("part",                    ConPart);
	IConsole::CmdRegister("help",                    ConHelp);
	IConsole::CmdRegister("info_cmd",                ConInfoCmd);
	IConsole::CmdRegister("list_cmds",               ConListCommands);
	IConsole::CmdRegister("list_aliases",            ConListAliases);
	IConsole::CmdRegister("newgame",                 ConNewGame);
	IConsole::CmdRegister("restart",                 ConRestart);
	IConsole::CmdRegister("reload",                  ConReload);
	IConsole::CmdRegister("getseed",                 ConGetSeed);
	IConsole::CmdRegister("getdate",                 ConGetDate);
	IConsole::CmdRegister("getsysdate",              ConGetSysDate);
	IConsole::CmdRegister("quit",                    ConExit);
	IConsole::CmdRegister("resetengines",            ConResetEngines,     ConHookNoNetwork);
	IConsole::CmdRegister("reset_enginepool",        ConResetEnginePool,  ConHookNoNetwork);
	IConsole::CmdRegister("return",                  ConReturn);
	IConsole::CmdRegister("screenshot",              ConScreenShot);
	IConsole::CmdRegister("minimap",                 ConMinimap);
	IConsole::CmdRegister("script",                  ConScript);
	IConsole::CmdRegister("zoomto",                  ConZoomToLevel);
	IConsole::CmdRegister("scrollto",                ConScrollToTile);
	IConsole::CmdRegister("highlight_tile",          ConHighlightTile);
	IConsole::AliasRegister("scrollto_highlight",    "scrollto %+; highlight_tile %+");
	IConsole::CmdRegister("alias",                   ConAlias);
	IConsole::CmdRegister("load",                    ConLoad);
	IConsole::CmdRegister("load_save",               ConLoad);
	IConsole::CmdRegister("load_scenario",           ConLoadScenario);
	IConsole::CmdRegister("load_heightmap",          ConLoadHeightmap);
	IConsole::CmdRegister("rm",                      ConRemove);
	IConsole::CmdRegister("save",                    ConSave);
	IConsole::CmdRegister("saveconfig",              ConSaveConfig);
	IConsole::CmdRegister("ls",                      ConListFiles);
	IConsole::CmdRegister("list_saves",              ConListFiles);
	IConsole::CmdRegister("list_scenarios",          ConListScenarios);
	IConsole::CmdRegister("list_heightmaps",         ConListHeightmaps);
	IConsole::CmdRegister("cd",                      ConChangeDirectory);
	IConsole::CmdRegister("pwd",                     ConPrintWorkingDirectory);
	IConsole::CmdRegister("clear",                   ConClearBuffer);
	IConsole::CmdRegister("font",                    ConFont);
	IConsole::CmdRegister("setting",                 ConSetting);
	IConsole::CmdRegister("setting_newgame",         ConSettingNewgame);
	IConsole::CmdRegister("list_settings",           ConListSettings);
	IConsole::CmdRegister("list_settings_def",       ConListSettingsDefaults);
	IConsole::CmdRegister("gamelog",                 ConGamelogPrint);
	IConsole::CmdRegister("rescan_newgrf",           ConRescanNewGRF);
	IConsole::CmdRegister("list_dirs",               ConListDirs);

	IConsole::AliasRegister("dir",                   "ls");
	IConsole::AliasRegister("del",                   "rm %+");
	IConsole::AliasRegister("newmap",                "newgame");
	IConsole::AliasRegister("patch",                 "setting %+");
	IConsole::AliasRegister("set",                   "setting %+");
	IConsole::AliasRegister("set_newgame",           "setting_newgame %+");
	IConsole::AliasRegister("list_patches",          "list_settings %+");
	IConsole::AliasRegister("developer",             "setting developer %+");

	IConsole::CmdRegister("list_ai_libs",            ConListAILibs);
	IConsole::CmdRegister("list_ai",                 ConListAI);
	IConsole::CmdRegister("reload_ai",               ConReloadAI);
	IConsole::CmdRegister("rescan_ai",               ConRescanAI);
	IConsole::CmdRegister("start_ai",                ConStartAI);
	IConsole::CmdRegister("stop_ai",                 ConStopAI);

	IConsole::CmdRegister("list_game",               ConListGame);
	IConsole::CmdRegister("list_game_libs",          ConListGameLibs);
	IConsole::CmdRegister("rescan_game",             ConRescanGame);

	IConsole::CmdRegister("companies",               ConCompanies);
	IConsole::AliasRegister("players",               "companies");

	/* networking functions */

/* Content downloading is only available with ZLIB */
#if defined(WITH_ZLIB)
	IConsole::CmdRegister("content",                 ConContent);
#endif /* defined(WITH_ZLIB) */

	/*** Networking commands ***/
	IConsole::CmdRegister("say",                     ConSay,              ConHookNeedNetwork);
	IConsole::CmdRegister("say_company",             ConSayCompany,       ConHookNeedNetwork);
	IConsole::AliasRegister("say_player",            "say_company %+");
	IConsole::CmdRegister("say_client",              ConSayClient,        ConHookNeedNetwork);

	IConsole::CmdRegister("connect",                 ConNetworkConnect,   ConHookClientOnly);
	IConsole::CmdRegister("clients",                 ConNetworkClients,   ConHookNeedNetwork);
	IConsole::CmdRegister("status",                  ConStatus,           ConHookServerOnly);
	IConsole::CmdRegister("server_info",             ConServerInfo,       ConHookServerOnly);
	IConsole::AliasRegister("info",                  "server_info");
	IConsole::CmdRegister("reconnect",               ConNetworkReconnect, ConHookClientOnly);
	IConsole::CmdRegister("rcon",                    ConRcon,             ConHookNeedNetwork);
	IConsole::CmdRegister("settings_access",         ConSettingsAccess,   ConHookNeedNetwork);

	IConsole::CmdRegister("join",                    ConJoinCompany,      ConHookNeedNonDedicatedOrNoNetwork);
	IConsole::AliasRegister("spectate",              "join 767");
	IConsole::CmdRegister("move",                    ConMoveClient,       ConHookServerOnly);
	IConsole::CmdRegister("reset_company",           ConResetCompany,     ConHookServerOnly);
	IConsole::AliasRegister("clean_company",         "reset_company %A");
	IConsole::CmdRegister("offer_company_sale",      ConOfferCompanySale, ConHookServerOrNoNetwork);
	IConsole::CmdRegister("merge_companies",         ConMergeCompanies,   ConHookServerOrNoNetwork);
	IConsole::CmdRegister("client_name",             ConClientNickChange, ConHookServerOnly);
	IConsole::CmdRegister("kick",                    ConKick,             ConHookServerOnly);
	IConsole::CmdRegister("ban",                     ConBan,              ConHookServerOnly);
	IConsole::CmdRegister("unban",                   ConUnBan,            ConHookServerOnly);
	IConsole::CmdRegister("banlist",                 ConBanList,          ConHookServerOnly);

	IConsole::CmdRegister("pause",                   ConPauseGame,        ConHookServerOrNoNetwork);
	IConsole::CmdRegister("unpause",                 ConUnpauseGame,      ConHookServerOrNoNetwork);
	IConsole::CmdRegister("step",                    ConStepGame,         ConHookNoNetwork);

	IConsole::CmdRegister("authorized_key",          ConNetworkAuthorizedKey, ConHookServerOnly);
	IConsole::AliasRegister("ak", "authorized_key %+");

	IConsole::CmdRegister("company_pw",              ConCompanyPassword,  ConHookNeedNetwork);
	IConsole::AliasRegister("company_password",      "company_pw %+");
	IConsole::CmdRegister("company_pw_hash",         ConCompanyPasswordHash, ConHookServerOnly);
	IConsole::AliasRegister("company_password_hash", "company_pw %+");
	IConsole::CmdRegister("company_pw_hashes",       ConCompanyPasswordHashes, ConHookServerOnly);
	IConsole::AliasRegister("company_password_hashes", "company_pw_hashes");

	IConsole::AliasRegister("net_frame_freq",        "setting frame_freq %+");
	IConsole::AliasRegister("net_sync_freq",         "setting sync_freq %+");
	IConsole::AliasRegister("server_pw",             "setting server_password %+");
	IConsole::AliasRegister("server_password",       "setting server_password %+");
	IConsole::AliasRegister("rcon_pw",               "setting rcon_password %+");
	IConsole::AliasRegister("rcon_password",         "setting rcon_password %+");
	IConsole::AliasRegister("settings_pw",           "setting settings_password %+");
	IConsole::AliasRegister("settings_password",     "setting settings_password %+");
	IConsole::AliasRegister("name",                  "setting client_name %+");
	IConsole::AliasRegister("server_name",           "setting server_name %+");
	IConsole::AliasRegister("server_port",           "setting server_port %+");
	IConsole::AliasRegister("max_clients",           "setting max_clients %+");
	IConsole::AliasRegister("max_companies",         "setting max_companies %+");
	IConsole::AliasRegister("max_join_time",         "setting max_join_time %+");
	IConsole::AliasRegister("pause_on_join",         "setting pause_on_join %+");
	IConsole::AliasRegister("autoclean_companies",   "setting autoclean_companies %+");
	IConsole::AliasRegister("autoclean_protected",   "setting autoclean_protected %+");
	IConsole::AliasRegister("autoclean_unprotected", "setting autoclean_unprotected %+");
	IConsole::AliasRegister("restart_game_year",     "setting restart_game_year %+");
	IConsole::AliasRegister("min_players",           "setting min_active_clients %+");
	IConsole::AliasRegister("reload_cfg",            "setting reload_cfg %+");

	/* conditionals */
	IConsole::CmdRegister("if_year",                 ConIfYear);
	IConsole::CmdRegister("if_month",                ConIfMonth);
	IConsole::CmdRegister("if_day",                  ConIfDay);
	IConsole::CmdRegister("if_hour",                 ConIfHour);
	IConsole::CmdRegister("if_minute",               ConIfMinute);
	IConsole::CmdRegister("if_hour_minute",          ConIfHourMinute);

	/* debugging stuff */
#ifdef _DEBUG
	IConsoleDebugLibRegister();
#endif
	IConsole::CmdRegister("fps",                     ConFramerate);
	IConsole::CmdRegister("fps_wnd",                 ConFramerateWindow);

	IConsole::CmdRegister("find_non_realistic_braking_signal", ConFindNonRealisticBrakingSignal);
	IConsole::CmdRegister("find_missing_object",     ConFindMissingObject);

	IConsole::CmdRegister("getfulldate",             ConGetFullDate,      nullptr, true);
	IConsole::CmdRegister("dump_command_log",        ConDumpCommandLog,   nullptr, true);
	IConsole::CmdRegister("dump_special_events_log", ConDumpSpecialEventsLog, nullptr, true);
	IConsole::CmdRegister("dump_desync_msgs",        ConDumpDesyncMsgLog, nullptr, true);
	IConsole::CmdRegister("dump_inflation",          ConDumpInflation,    nullptr, true);
	IConsole::CmdRegister("dump_cpdp_stats",         ConDumpCpdpStats,    nullptr, true);
	IConsole::CmdRegister("dump_veh_stats",          ConVehicleStats,     nullptr, true);
	IConsole::CmdRegister("dump_map_stats",          ConMapStats,         nullptr, true);
	IConsole::CmdRegister("dump_st_flow_stats",      ConStFlowStats,      nullptr, true);
	IConsole::CmdRegister("dump_slot_stats",         ConSlotsStats,       nullptr, true);
	IConsole::CmdRegister("dump_game_events",        ConDumpGameEvents,   nullptr, true);
	IConsole::CmdRegister("dump_load_debug_log",     ConDumpLoadDebugLog, nullptr, true);
	IConsole::CmdRegister("dump_load_debug_config",  ConDumpLoadDebugConfig, nullptr, true);
	IConsole::CmdRegister("dump_linkgraph_jobs",     ConDumpLinkgraphJobs, nullptr, true);
	IConsole::CmdRegister("dump_road_types",         ConDumpRoadTypes,    nullptr, true);
	IConsole::CmdRegister("dump_rail_types",         ConDumpRailTypes,    nullptr, true);
	IConsole::CmdRegister("dump_bridge_types",       ConDumpBridgeTypes,  nullptr, true);
	IConsole::CmdRegister("dump_cargo_types",        ConDumpCargoTypes,   nullptr, true);
	IConsole::CmdRegister("dump_vehicle",            ConDumpVehicle,      nullptr, true);
	IConsole::CmdRegister("dump_tile",               ConDumpTile,         nullptr, true);
	IConsole::CmdRegister("dump_grf_cargo_tables",   ConDumpGrfCargoTables, nullptr, true);
	IConsole::CmdRegister("dump_signal_styles",      ConDumpSignalStyles, nullptr, true);
	IConsole::CmdRegister("dump_sprite_cache_stats", ConSpriteCacheStats, nullptr, true);
	IConsole::CmdRegister("dump_version",            ConDumpVersion,      nullptr, true);
	IConsole::CmdRegister("check_caches",            ConCheckCaches,      nullptr, true);
	IConsole::CmdRegister("show_town_window",        ConShowTownWindow,   nullptr, true);
	IConsole::CmdRegister("show_station_window",     ConShowStationWindow, nullptr, true);
	IConsole::CmdRegister("show_industry_window",    ConShowIndustryWindow, nullptr, true);
	IConsole::CmdRegister("viewport_debug",          ConViewportDebug,    nullptr, true);
	IConsole::CmdRegister("viewport_mark_dirty",     ConViewportMarkDirty, nullptr, true);
	IConsole::CmdRegister("viewport_mark_dirty_st_overlay", ConViewportMarkStationOverlayDirty, nullptr, true);
	IConsole::CmdRegister("gfx_debug",               ConGfxDebug,         nullptr, true);
	IConsole::CmdRegister("csleep",                  ConCSleep,           nullptr, true);
	IConsole::CmdRegister("recalculate_road_cached_one_way_states", ConRecalculateRoadCachedOneWayStates, ConHookNoNetwork, true);
	IConsole::CmdRegister("misc_debug",              ConMiscDebug,        nullptr, true);
	IConsole::CmdRegister("set_newgrf_optimiser_flags", ConSetNewGRFOptimiserFlags, nullptr, true);

	/* NewGRF development stuff */
	IConsole::CmdRegister("reload_newgrfs",          ConNewGRFReload,     ConHookNewGRFDeveloperTool);
	IConsole::CmdRegister("newgrf_profile",          ConNewGRFProfile,    ConHookNewGRFDeveloperTool);
	IConsole::CmdRegister("dump_info",               ConDumpInfo);
	IConsole::CmdRegister("do_disaster",             ConDoDisaster,       ConHookNewGRFDeveloperTool, true);
	IConsole::CmdRegister("bankrupt_company",        ConBankruptCompany,  ConHookNewGRFDeveloperTool, true);
	IConsole::CmdRegister("delete_company",          ConDeleteCompany,    ConHookNewGRFDeveloperTool, true);
	IConsole::CmdRegister("road_type_flag_ctl",      ConRoadTypeFlagCtl,  ConHookNewGRFDeveloperTool, true);
	IConsole::CmdRegister("rail_type_map_colour_ctl", ConRailTypeMapColourCtl, ConHookNewGRFDeveloperTool, true);
	IConsole::CmdRegister("switch_baseset",          ConSwitchBaseset,    ConHookNewGRFDeveloperTool, true);

	/* Bug workarounds */
	IConsole::CmdRegister("jgrpp_bug_workaround_unblock_heliports", ConResetBlockedHeliports, ConHookNoNetwork, true);
	IConsole::CmdRegister("merge_linkgraph_jobs_asap", ConMergeLinkgraphJobsAsap, ConHookNoNetwork, true);
	IConsole::CmdRegister("unblock_bay_road_stops",  ConUnblockBayRoadStops,  ConHookNoNetwork, true);

	IConsole::CmdRegister("dbgspecial",              ConDbgSpecial,       ConHookSpecialCmd, true);

#ifdef _DEBUG
	IConsole::CmdRegister("delete_vehicle_id",       ConDeleteVehicleID,  ConHookNoNetwork, true);
	IConsole::CmdRegister("run_tile_loop_tile",      ConRunTileLoopTile,  ConHookNoNetwork, true);
#endif
}
