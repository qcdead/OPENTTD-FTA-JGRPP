/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file main_gui.cpp Handling of the main viewport. */

#include "stdafx.h"
#include "currency.h"
#include "spritecache.h"
#include "window_gui.h"
#include "window_func.h"
#include "textbuf_gui.h"
#include "viewport_func.h"
#include "command_func.h"
#include "console_gui.h"
#include "progress.h"
#include "transparency_gui.h"
#include "map_func.h"
#include "sound_func.h"
#include "transparency.h"
#include "strings_func.h"
#include "zoom_func.h"
#include "company_base.h"
#include "company_cmd.h"
#include "company_func.h"
#include "toolbar_gui.h"
#include "statusbar_gui.h"
#include "linkgraph/linkgraph_gui.h"
#include "tilehighlight_func.h"
#include "hotkeys.h"
#include "guitimer_func.h"
#include "error.h"
#include "news_gui.h"
#include "misc_cmd.h"

#include "sl/saveload.h"

#include "widgets/main_widget.h"

#include "network/network.h"
#include "network/network_func.h"
#include "network/network_gui.h"
#include "network/network_base.h"

#include "table/sprites.h"
#include "table/strings.h"

#include "safeguards.h"

void CcGiveMoney(const CommandCost &result, Money money, CompanyID dest_company)
{
	if (result.Failed() || !_settings_game.economy.give_money || !_networking) return;

	/* Inform the company of the action of one of its clients (controllers). */
	SetDParam(0, dest_company);
	std::string msg = GetString(STR_COMPANY_NAME);

	/*
	 * bits 31-16: source company
	 * bits 15-0: target company
	 */
	uint64_t auxdata = (uint64_t)dest_company | (((uint64_t) _local_company) << 16);

	if (!_network_server) {
		NetworkClientSendChat(NETWORK_ACTION_GIVE_MONEY, DESTTYPE_BROADCAST_SS, dest_company, msg, NetworkTextMessageData(result.GetCost(), auxdata));
	} else {
		NetworkServerSendChat(NETWORK_ACTION_GIVE_MONEY, DESTTYPE_BROADCAST_SS, dest_company, msg, CLIENT_ID_SERVER, NetworkTextMessageData(result.GetCost(), auxdata));
	}
}

/**
 * This code is shared for the majority of the pushbuttons.
 * Handles e.g. the pressing of a button (to build things), playing of click sound and sets certain parameters
 *
 * @param w Window which called the function
 * @param widget ID of the widget (=button) that called this function
 * @param cursor How should the cursor image change? E.g. cursor with depot image in it
 * @param mode Tile highlighting mode, e.g. drawing a rectangle or a dot on the ground
 * @return true if the button is clicked, false if it's unclicked
 */
bool HandlePlacePushButton(Window *w, WidgetID widget, CursorID cursor, HighLightStyle mode)
{
	if (w->IsWidgetDisabled(widget)) return false;

	if (_settings_client.sound.click_beep) SndPlayFx(SND_15_BEEP);
	w->SetDirty();

	if (w->IsWidgetLowered(widget)) {
		ResetObjectToPlace();
		return false;
	}

	SetObjectToPlace(cursor, PAL_NONE, mode, w->window_class, w->window_number);
	w->LowerWidget(widget);
	return true;
}


void CcPlaySound_EXPLOSION(const CommandCost &result, TileIndex tile)
{
	if (result.Succeeded() && _settings_client.sound.confirm) SndPlayTileFx(SND_12_EXPLOSION, tile);
}

/**
 * Zooms a viewport in a window in or out.
 * @param how Zooming direction.
 * @param w   Window owning the viewport.
 * @return Returns \c true if zooming step could be done, \c false if further zooming is not possible.
 * @note No button handling or what so ever is done.
 */
bool DoZoomInOutWindow(ZoomStateChange how, Window *w)
{
	Viewport *vp;

	assert(w != nullptr);
	vp = w->viewport;

	switch (how) {
		case ZOOM_NONE:
			/* On initialisation of the viewport we don't do anything. */
			break;

		case ZOOM_IN:
			if (vp->zoom <= _settings_client.gui.zoom_min) return false;
			vp->zoom = (ZoomLevel)((int)vp->zoom - 1);
			vp->virtual_width >>= 1;
			vp->virtual_height >>= 1;

			w->viewport->scrollpos_x += vp->virtual_width >> 1;
			w->viewport->scrollpos_y += vp->virtual_height >> 1;
			w->viewport->dest_scrollpos_x = w->viewport->scrollpos_x;
			w->viewport->dest_scrollpos_y = w->viewport->scrollpos_y;
			break;
		case ZOOM_OUT:
			if (vp->zoom >= _settings_client.gui.zoom_max) return false;
			if (w->window_class != WC_MAIN_WINDOW && w->window_class != WC_EXTRA_VIEWPORT && vp->zoom >= ZOOM_LVL_DRAW_SPR) return false;
			vp->zoom = (ZoomLevel)((int)vp->zoom + 1);

			w->viewport->scrollpos_x -= vp->virtual_width >> 1;
			w->viewport->scrollpos_y -= vp->virtual_height >> 1;
			w->viewport->dest_scrollpos_x = w->viewport->scrollpos_x;
			w->viewport->dest_scrollpos_y = w->viewport->scrollpos_y;

			vp->virtual_width <<= 1;
			vp->virtual_height <<= 1;
			break;
	}
	if (vp != nullptr) { // the vp can be null when how == ZOOM_NONE
		vp->virtual_left = w->viewport->scrollpos_x;
		vp->virtual_top = w->viewport->scrollpos_y;
		UpdateViewportSizeZoom(vp);
	}
	/* Update the windows that have zoom-buttons to perhaps disable their buttons */
	w->InvalidateData();
	if (how != ZOOM_NONE) {
		RebuildViewportOverlay(w, false);
	}
	return true;
}

void ZoomInOrOutToCursorWindow(bool in, Window *w)
{
	assert(w != nullptr);

	if (_game_mode != GM_MENU) {
		Viewport *vp = w->viewport;
		if ((in && vp->zoom <= _settings_client.gui.zoom_min) || (!in && vp->zoom >= _settings_client.gui.zoom_max)) return;

		Point pt = GetTileZoomCenterWindow(in, w);
		if (pt.x != -1) {
			ScrollWindowTo(pt.x, pt.y, -1, w, true);

			DoZoomInOutWindow(in ? ZOOM_IN : ZOOM_OUT, w);
		}
	}
}

void FixTitleGameZoom(int zoom_adjust)
{
	if (_game_mode != GM_MENU) return;

	Viewport *vp = GetMainWindow()->viewport;

	/* Adjust the zoom in/out.
	 * Can't simply add, since operator+ is not defined on the ZoomLevel type. */
	vp->zoom = _gui_zoom;
	while (zoom_adjust < 0 && vp->zoom != _settings_client.gui.zoom_min) {
		vp->zoom--;
		zoom_adjust++;
	}
	while (zoom_adjust > 0 && vp->zoom != _settings_client.gui.zoom_max) {
		vp->zoom++;
		zoom_adjust--;
	}

	vp->virtual_width = ScaleByZoom(vp->width, vp->zoom);
	vp->virtual_height = ScaleByZoom(vp->height, vp->zoom);
	UpdateViewportSizeZoom(vp);
}

static constexpr NWidgetPart _nested_main_window_widgets[] = {
	NWidget(NWID_VIEWPORT, INVALID_COLOUR, WID_M_VIEWPORT), SetResize(1, 1),
};

enum GlobalHotKeys : int32_t {
	GHK_QUIT,
	GHK_ABANDON,
	GHK_CONSOLE,
	GHK_BOUNDING_BOXES,
	GHK_DIRTY_BLOCKS,
	GHK_WIDGET_OUTLINES,
	GHK_CENTER,
	GHK_CENTER_ZOOM,
	GHK_RESET_OBJECT_TO_PLACE,
	GHK_DELETE_WINDOWS,
	GHK_DELETE_NONVITAL_WINDOWS,
	GHK_DELETE_ALL_MESSAGES,
	GHK_REFRESH_SCREEN,
	GHK_CRASH,
	GHK_MONEY,
	GHK_UPDATE_COORDS,
	GHK_TOGGLE_TRANSPARENCY,
	GHK_TOGGLE_INVISIBILITY = GHK_TOGGLE_TRANSPARENCY + 10,
	GHK_TRANSPARENCY_TOOLBAR = GHK_TOGGLE_INVISIBILITY + 8,
	GHK_TRANSPARANCY,
	GHK_CHAT,
	GHK_CHAT_ALL,
	GHK_CHAT_COMPANY,
	GHK_CHAT_SERVER,
	GHK_CLOSE_NEWS,
	GHK_CLOSE_ERROR,
	GHK_CHANGE_MAP_MODE_PREV,
	GHK_CHANGE_MAP_MODE_NEXT,
	GHK_SWITCH_VIEWPORT_ROUTE_OVERLAY_MODE,
	GHK_SWITCH_VIEWPORT_MAP_SLOPE_MODE,
	GHK_SWITCH_VIEWPORT_MAP_HEIGHT_MODE,
};

struct MainWindow : Window
{
	GUITimer refresh;

	/* Refresh times in milliseconds */
	static const uint LINKGRAPH_REFRESH_PERIOD = 7650;
	static const uint LINKGRAPH_DELAY = 450;

	MainWindow(WindowDesc &desc) : Window(desc)
	{
		this->InitNested(0);
		this->flags.Reset(WindowFlag::WhiteBorder);
		ResizeWindow(this, _screen.width, _screen.height);

		NWidgetViewport *nvp = this->GetWidget<NWidgetViewport>(WID_M_VIEWPORT);
		nvp->InitializeViewport(this, TileXY(32, 32).base(), ScaleZoomGUI(ZOOM_LVL_VIEWPORT));

		this->viewport->map_type = (ViewportMapType) _settings_client.gui.default_viewport_map_mode;
		CompanyMask empty;
		this->viewport->overlay = new LinkGraphOverlay(this, WID_M_VIEWPORT, 0, empty, 2);
		this->refresh.SetInterval(LINKGRAPH_DELAY);
	}

	void OnRealtimeTick(uint delta_ms) override
	{
		if (!this->refresh.Elapsed(delta_ms)) return;

		this->refresh.SetInterval(LINKGRAPH_REFRESH_PERIOD);

		if (this->viewport->overlay->GetCargoMask() == 0 ||
				this->viewport->overlay->GetCompanyMask().None()) {
			return;
		}

		if (this->viewport->overlay->RebuildCacheCheckChanged()) {
			this->GetWidget<NWidgetBase>(WID_M_VIEWPORT)->SetDirty(this);
		}
	}

	void OnPaint() override
	{
		this->DrawWidgets();
		if (_game_mode == GM_MENU) {
			ViewportDoDrawProcessAllPending();

			static const std::initializer_list<SpriteID> title_sprites = {SPR_OTTD_O, SPR_OTTD_P, SPR_OTTD_E, SPR_OTTD_N, SPR_OTTD_T, SPR_OTTD_T, SPR_OTTD_D};
			uint letter_spacing = ScaleGUITrad(10);
			int name_width = static_cast<int>(std::size(title_sprites) - 1) * letter_spacing;

			for (const SpriteID &sprite : title_sprites) {
				name_width += GetSpriteSize(sprite).width;
			}
			int off_x = (this->width - name_width) / 2;

			for (const SpriteID &sprite : title_sprites) {
				DrawSprite(sprite, PAL_NONE, off_x, ScaleGUITrad(50));
				off_x += GetSpriteSize(sprite).width + letter_spacing;
			}
		}
	}

	EventState OnHotkey(int hotkey) override
	{
		if (hotkey == GHK_QUIT) {
			HandleExitGameRequest();
			return ES_HANDLED;
		}

		/* Disable all key shortcuts, except quit shortcuts when
		 * generating the world, otherwise they create threading
		 * problem during the generating, resulting in random
		 * assertions that are hard to trigger and debug */
		if (HasModalProgress()) return ES_NOT_HANDLED;

		switch (hotkey) {
			case GHK_ABANDON:
				/* No point returning from the main menu to itself */
				if (_game_mode == GM_MENU) return ES_HANDLED;
				if (_settings_client.gui.autosave_on_exit) {
					DoExitSave();
					_switch_mode = SM_MENU;
				} else {
					AskExitToGameMenu();
				}
				return ES_HANDLED;

			case GHK_CONSOLE:
				IConsoleSwitch();
				return ES_HANDLED;

			case GHK_BOUNDING_BOXES:
				ToggleBoundingBoxes();
				return ES_HANDLED;

			case GHK_DIRTY_BLOCKS:
				ToggleDirtyBlocks();
				return ES_HANDLED;

			case GHK_WIDGET_OUTLINES:
				ToggleWidgetOutlines();
				return ES_HANDLED;
		}

		if (_game_mode == GM_MENU) return ES_NOT_HANDLED;

		switch (hotkey) {
			case GHK_CENTER:
			case GHK_CENTER_ZOOM: {
				Point pt = GetTileBelowCursor();
				if (pt.x != -1) {
					bool instant = (hotkey == GHK_CENTER_ZOOM && this->viewport->zoom != _settings_client.gui.zoom_min);
					if (hotkey == GHK_CENTER_ZOOM) MaxZoomInOut(ZOOM_IN, this);
					ScrollMainWindowTo(pt.x, pt.y, -1, instant);
				}
				break;
			}

			case GHK_RESET_OBJECT_TO_PLACE: ResetObjectToPlace(); break;
			case GHK_DELETE_WINDOWS: CloseNonVitalWindows(); break;
			case GHK_DELETE_NONVITAL_WINDOWS: CloseAllNonVitalWindows(); break;
			case GHK_DELETE_ALL_MESSAGES: DeleteAllMessages(); break;
			case GHK_REFRESH_SCREEN: MarkWholeScreenDirty(); break;

			case GHK_CRASH: // Crash the game
				*(volatile uint8_t *)nullptr = 0;
				break;

			case GHK_MONEY: // Gimme money
				/* You can only cheat for money in single player or when otherwise suitably authorised. */
				if (!_networking || _settings_game.difficulty.money_cheat_in_multiplayer) {
					Command<CMD_MONEY_CHEAT>::Post(10000000);
				} else if (IsNetworkSettingsAdmin()) {
					Command<CMD_MONEY_CHEAT_ADMIN>::Post(10000000);
				}
				break;

			case GHK_UPDATE_COORDS: // Update the coordinates of all station signs
				UpdateAllVirtCoords();
				break;

			case GHK_TOGGLE_TRANSPARENCY:
			case GHK_TOGGLE_TRANSPARENCY + 1:
			case GHK_TOGGLE_TRANSPARENCY + 2:
			case GHK_TOGGLE_TRANSPARENCY + 3:
			case GHK_TOGGLE_TRANSPARENCY + 4:
			case GHK_TOGGLE_TRANSPARENCY + 5:
			case GHK_TOGGLE_TRANSPARENCY + 6:
			case GHK_TOGGLE_TRANSPARENCY + 7:
			case GHK_TOGGLE_TRANSPARENCY + 8:
			case GHK_TOGGLE_TRANSPARENCY + 9:
				/* Transparency toggle hot keys */
				ToggleTransparency((TransparencyOption)(hotkey - GHK_TOGGLE_TRANSPARENCY));
				MarkWholeScreenDirty();
				break;

			case GHK_TOGGLE_INVISIBILITY:
			case GHK_TOGGLE_INVISIBILITY + 1:
			case GHK_TOGGLE_INVISIBILITY + 2:
			case GHK_TOGGLE_INVISIBILITY + 3:
			case GHK_TOGGLE_INVISIBILITY + 4:
			case GHK_TOGGLE_INVISIBILITY + 5:
			case GHK_TOGGLE_INVISIBILITY + 6:
			case GHK_TOGGLE_INVISIBILITY + 7:
				/* Invisibility toggle hot keys */
				ToggleInvisibilityWithTransparency((TransparencyOption)(hotkey - GHK_TOGGLE_INVISIBILITY));
				MarkWholeScreenDirty();
				break;

			case GHK_TRANSPARENCY_TOOLBAR:
				ShowTransparencyToolbar();
				break;

			case GHK_TRANSPARANCY:
				ResetRestoreAllTransparency();
				break;

			case GHK_CHAT: // smart chat; send to team if any, otherwise to all
				if (_networking) {
					const NetworkClientInfo *cio = NetworkClientInfo::GetByClientID(_network_own_client_id);
					if (cio == nullptr) break;

					ShowNetworkChatQueryWindow(NetworkClientPreferTeamChat(cio) ? DESTTYPE_TEAM : DESTTYPE_BROADCAST, cio->client_playas);
				}
				break;

			case GHK_CHAT_ALL: // send text message to all clients
				if (_networking) ShowNetworkChatQueryWindow(DESTTYPE_BROADCAST, 0);
				break;

			case GHK_CHAT_COMPANY: // send text to all team mates
				if (_networking) {
					const NetworkClientInfo *cio = NetworkClientInfo::GetByClientID(_network_own_client_id);
					if (cio == nullptr) break;

					ShowNetworkChatQueryWindow(DESTTYPE_TEAM, cio->client_playas);
				}
				break;

			case GHK_CHAT_SERVER: // send text to the server
				if (_networking && !_network_server) {
					ShowNetworkChatQueryWindow(DESTTYPE_CLIENT, CLIENT_ID_SERVER);
				}
				break;

			case GHK_CLOSE_NEWS: // close active news window
				if (!HideActiveNewsMessage()) return ES_NOT_HANDLED;
				break;

			case GHK_CLOSE_ERROR: // close active error window
				if (!HideActiveErrorMessage()) return ES_NOT_HANDLED;
				break;

			case GHK_CHANGE_MAP_MODE_PREV:
				if (_focused_window && _focused_window->viewport && _focused_window->viewport->zoom >= ZOOM_LVL_DRAW_MAP) {
					ChangeRenderMode(_focused_window->viewport, true);
					_focused_window->SetDirty();
				} else if (this->viewport->zoom >= ZOOM_LVL_DRAW_MAP) {
					ChangeRenderMode(this->viewport, true);
					this->SetDirty();
				}
				break;
			case GHK_CHANGE_MAP_MODE_NEXT:
				if (_focused_window && _focused_window->viewport && _focused_window->viewport->zoom >= ZOOM_LVL_DRAW_MAP) {
					ChangeRenderMode(_focused_window->viewport, false);
					_focused_window->SetDirty();
				} else if (this->viewport->zoom >= ZOOM_LVL_DRAW_MAP) {
					ChangeRenderMode(this->viewport, false);
					this->SetDirty();
				}
				break;
			case GHK_SWITCH_VIEWPORT_ROUTE_OVERLAY_MODE:
				if (_settings_client.gui.show_vehicle_route_mode != 0) {
					_settings_client.gui.show_vehicle_route_mode ^= 3;
					SetWindowDirty(WC_GAME_OPTIONS, WN_GAME_OPTIONS_GAME_SETTINGS);
				}
				break;
			case GHK_SWITCH_VIEWPORT_MAP_SLOPE_MODE: {
				_settings_client.gui.show_slopes_on_viewport_map = !_settings_client.gui.show_slopes_on_viewport_map;
				extern void MarkAllViewportMapLandscapesDirty();
				MarkAllViewportMapLandscapesDirty();
				break;
			}
			case GHK_SWITCH_VIEWPORT_MAP_HEIGHT_MODE: {
				_settings_client.gui.show_height_on_viewport_map = !_settings_client.gui.show_height_on_viewport_map;
				extern void MarkAllViewportMapLandscapesDirty();
				MarkAllViewportMapLandscapesDirty();
				break;
			}

			default: return ES_NOT_HANDLED;
		}
		return ES_HANDLED;
	}

	void OnScroll(Point delta) override
	{
		this->viewport->scrollpos_x += ScaleByZoom(delta.x, this->viewport->zoom);
		this->viewport->scrollpos_y += ScaleByZoom(delta.y, this->viewport->zoom);
		this->viewport->dest_scrollpos_x = this->viewport->scrollpos_x;
		this->viewport->dest_scrollpos_y = this->viewport->scrollpos_y;
		this->refresh.SetInterval(LINKGRAPH_DELAY);
	}

	void OnMouseWheel(int wheel) override
	{
		if (_ctrl_pressed) {
			/* Cycle through the drawing modes */
			ChangeRenderMode(this->viewport, wheel < 0);
			this->SetDirty();
		} else if (_settings_client.gui.scrollwheel_scrolling != SWS_OFF) {
			bool in = wheel < 0;

			/* When following, only change zoom - otherwise zoom to the cursor. */
			if (this->viewport->follow_vehicle != INVALID_VEHICLE) {
				DoZoomInOutWindow(in ? ZOOM_IN : ZOOM_OUT, this);
			} else {
				ZoomInOrOutToCursorWindow(in, this);
			}
		}
	}

	void OnResize() override
	{
		if (this->viewport != nullptr) {
			NWidgetViewport *nvp = this->GetWidget<NWidgetViewport>(WID_M_VIEWPORT);
			nvp->UpdateViewportCoordinates(this);
			this->refresh.SetInterval(LINKGRAPH_DELAY);
		}
	}

	bool OnTooltip([[maybe_unused]] Point pt, WidgetID widget, TooltipCloseCondition close_cond) override
	{
		if (widget != WID_M_VIEWPORT) return false;
		return this->viewport->overlay->ShowTooltip(pt, close_cond);
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data Information about the changed data.
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;
		/* Forward the message to the appropriate toolbar (ingame or scenario editor) */
		InvalidateWindowData(WC_MAIN_TOOLBAR, 0, data, true);
	}

	virtual void OnMouseOver(Point pt, WidgetID widget) override
	{
		if (pt.x != -1 && _game_mode != GM_MENU && IsViewportMouseHoverActive()) {
			/* Show tooltip with last month production or town name */
			const Point p = GetTileBelowCursor();
			const TileIndex tile = TileVirtXY(p.x, p.y);
			if (tile < Map::Size()) ShowTooltipForTile(this, tile);
		}
	}

	static HotkeyList hotkeys;
};

const uint16_t _ghk_quit_keys[] = {'Q' | WKC_CTRL, 'Q' | WKC_META, 0};
const uint16_t _ghk_abandon_keys[] = {'W' | WKC_CTRL, 'W' | WKC_META, 0};
const uint16_t _ghk_chat_keys[] = {WKC_RETURN, 'T', 0};
const uint16_t _ghk_chat_all_keys[] = {WKC_SHIFT | WKC_RETURN, WKC_SHIFT | 'T', 0};
const uint16_t _ghk_chat_company_keys[] = {WKC_CTRL | WKC_RETURN, WKC_CTRL | 'T', 0};
const uint16_t _ghk_chat_server_keys[] = {WKC_CTRL | WKC_SHIFT | WKC_RETURN, WKC_CTRL | WKC_SHIFT | 'T', 0};

static Hotkey global_hotkeys[] = {
	Hotkey(_ghk_quit_keys, "quit", GHK_QUIT),
	Hotkey(_ghk_abandon_keys, "abandon", GHK_ABANDON),
	Hotkey(WKC_BACKQUOTE, "console", GHK_CONSOLE),
	Hotkey('B' | WKC_CTRL, "bounding_boxes", GHK_BOUNDING_BOXES),
	Hotkey('I' | WKC_CTRL, "dirty_blocks", GHK_DIRTY_BLOCKS),
	Hotkey((uint16_t)0,    "widget_outlines", GHK_WIDGET_OUTLINES),
	Hotkey('C', "center", GHK_CENTER),
	Hotkey('Z', "center_zoom", GHK_CENTER_ZOOM),
	Hotkey(WKC_ESC, "reset_object_to_place", GHK_RESET_OBJECT_TO_PLACE),
	Hotkey(WKC_DELETE, "delete_windows", GHK_DELETE_WINDOWS),
	Hotkey(WKC_DELETE | WKC_SHIFT, "delete_all_windows", GHK_DELETE_NONVITAL_WINDOWS),
	Hotkey(WKC_DELETE | WKC_CTRL, "delete_all_messages", GHK_DELETE_ALL_MESSAGES),
	Hotkey('R' | WKC_CTRL, "refresh_screen", GHK_REFRESH_SCREEN),
#if defined(_DEBUG)
	Hotkey('0' | WKC_ALT, "crash_game", GHK_CRASH),
	Hotkey('1' | WKC_ALT, "money", GHK_MONEY),
	Hotkey('2' | WKC_ALT, "update_coordinates", GHK_UPDATE_COORDS),
#endif
	Hotkey('1' | WKC_CTRL, "transparency_signs", GHK_TOGGLE_TRANSPARENCY),
	Hotkey('2' | WKC_CTRL, "transparency_trees", GHK_TOGGLE_TRANSPARENCY + 1),
	Hotkey('3' | WKC_CTRL, "transparency_houses", GHK_TOGGLE_TRANSPARENCY + 2),
	Hotkey('4' | WKC_CTRL, "transparency_industries", GHK_TOGGLE_TRANSPARENCY + 3),
	Hotkey('5' | WKC_CTRL, "transparency_buildings", GHK_TOGGLE_TRANSPARENCY + 4),
	Hotkey('6' | WKC_CTRL, "transparency_bridges", GHK_TOGGLE_TRANSPARENCY + 5),
	Hotkey('7' | WKC_CTRL, "transparency_structures", GHK_TOGGLE_TRANSPARENCY + 6),
	Hotkey('8' | WKC_CTRL, "transparency_catenary", GHK_TOGGLE_TRANSPARENCY + 7),
	Hotkey('9' | WKC_CTRL, "transparency_loading", GHK_TOGGLE_TRANSPARENCY + 8),
	Hotkey('0' | WKC_CTRL, "transparency_tunnels", GHK_TOGGLE_TRANSPARENCY + 9),
	Hotkey('1' | WKC_CTRL | WKC_SHIFT, "invisibility_signs", GHK_TOGGLE_INVISIBILITY),
	Hotkey('2' | WKC_CTRL | WKC_SHIFT, "invisibility_trees", GHK_TOGGLE_INVISIBILITY + 1),
	Hotkey('3' | WKC_CTRL | WKC_SHIFT, "invisibility_houses", GHK_TOGGLE_INVISIBILITY + 2),
	Hotkey('4' | WKC_CTRL | WKC_SHIFT, "invisibility_industries", GHK_TOGGLE_INVISIBILITY + 3),
	Hotkey('5' | WKC_CTRL | WKC_SHIFT, "invisibility_buildings", GHK_TOGGLE_INVISIBILITY + 4),
	Hotkey('6' | WKC_CTRL | WKC_SHIFT, "invisibility_bridges", GHK_TOGGLE_INVISIBILITY + 5),
	Hotkey('7' | WKC_CTRL | WKC_SHIFT, "invisibility_structures", GHK_TOGGLE_INVISIBILITY + 6),
	Hotkey('8' | WKC_CTRL | WKC_SHIFT, "invisibility_catenary", GHK_TOGGLE_INVISIBILITY + 7),
	Hotkey('X' | WKC_CTRL, "transparency_toolbar", GHK_TRANSPARENCY_TOOLBAR),
	Hotkey('X', "toggle_transparency", GHK_TRANSPARANCY),
	Hotkey(_ghk_chat_keys, "chat", GHK_CHAT),
	Hotkey(_ghk_chat_all_keys, "chat_all", GHK_CHAT_ALL),
	Hotkey(_ghk_chat_company_keys, "chat_company", GHK_CHAT_COMPANY),
	Hotkey(_ghk_chat_server_keys, "chat_server", GHK_CHAT_SERVER),
	Hotkey(WKC_SPACE, "close_news", GHK_CLOSE_NEWS),
	Hotkey(WKC_SPACE, "close_error", GHK_CLOSE_ERROR),
	Hotkey(WKC_PAGEUP,   "previous_map_mode", GHK_CHANGE_MAP_MODE_PREV),
	Hotkey(WKC_PAGEDOWN, "next_map_mode",     GHK_CHANGE_MAP_MODE_NEXT),
	Hotkey(WKC_SLASH | WKC_CTRL,  "switch_viewport_route_overlay_mode", GHK_SWITCH_VIEWPORT_ROUTE_OVERLAY_MODE),
	Hotkey((uint16_t)0,  "switch_viewport_map_slope_mode", GHK_SWITCH_VIEWPORT_MAP_SLOPE_MODE),
	Hotkey((uint16_t)0,  "switch_viewport_map_height_mode", GHK_SWITCH_VIEWPORT_MAP_HEIGHT_MODE),
};
HotkeyList MainWindow::hotkeys("global", global_hotkeys);

static WindowDesc _main_window_desc(__FILE__, __LINE__,
	WDP_MANUAL, nullptr, 0, 0,
	WC_MAIN_WINDOW, WC_NONE,
	WindowDefaultFlag::NoClose,
	_nested_main_window_widgets,
	&MainWindow::hotkeys
);

/**
 * Does the given keycode match one of the keycodes bound to 'quit game'?
 * @param keycode The keycode that was pressed by the user.
 * @return True iff the keycode matches one of the hotkeys for 'quit'.
 */
bool IsQuitKey(uint16_t keycode)
{
	int num = MainWindow::hotkeys.CheckMatch(keycode);
	return num == GHK_QUIT;
}


void ShowSelectGameWindow();

/**
 * Initialise the default colours (remaps and the likes), and load the main windows.
 */
void SetupColoursAndInitialWindow()
{
	for (Colours i = COLOUR_BEGIN; i != COLOUR_END; i++) {
		const uint8_t *b = GetNonSprite(GENERAL_SPRITE_COLOUR(i), SpriteType::Recolour);
		assert(b != nullptr);
		for (ColourShade j = SHADE_BEGIN; j < SHADE_END; j++) {
			SetColourGradient(i, j, b[0xC6 + j]);
		}
	}

	new MainWindow(_main_window_desc);

	/* XXX: these are not done */
	switch (_game_mode) {
		default: NOT_REACHED();
		case GM_MENU:
			ShowSelectGameWindow();
			break;

		case GM_NORMAL:
		case GM_EDITOR:
			ShowVitalWindows();
			break;
	}
}

/**
 * Show the vital in-game windows.
 */
void ShowVitalWindows()
{
	AllocateToolbar();

	/* Status bad only for normal games */
	if (_game_mode == GM_EDITOR) return;

	ShowStatusBar();
}

/**
 * Size of the application screen changed.
 * Adapt the game screen-size, re-allocate the open windows, and repaint everything
 */
void GameSizeChanged()
{
	_cur_resolution.width  = _screen.width;
	_cur_resolution.height = _screen.height;
	ScreenSizeChanged();
	RelocateAllWindows(_screen.width, _screen.height);
}
