/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file window_gui.h Functions, definitions and such used only by the GUI. */

#ifndef WINDOW_GUI_H
#define WINDOW_GUI_H

#include "vehicle_type.h"
#include "viewport_type.h"
#include "company_type.h"
#include "tile_type.h"
#include "widget_type.h"
#include "string_type.h"
#include "3rdparty/cpp-btree/btree_map.h"

#include <algorithm>
#include <functional>
#include <vector>

/**
 * Flags to describe the look of the frame
 */
enum class FrameFlag : uint8_t {
	Transparent, ///< Makes the background transparent if set
	BorderOnly, ///< Draw border only, no background
	Lowered, ///< If set the frame is lowered and the background colour brighter (ie. buttons when pressed)
	Darkened, ///< If set the background is darker, allows for lowered frames with normal background colour when used with FrameFlag::Lowered (ie. dropdown boxes)
};
using FrameFlags = EnumBitSet<FrameFlag, uint8_t>;

class WidgetDimensions {
public:
	RectPadding imgbtn;        ///< Padding around image button image.
	RectPadding inset;         ///< Padding inside inset container.
	RectPadding vscrollbar;    ///< Padding inside vertical scrollbar buttons.
	RectPadding hscrollbar;    ///< Padding inside horizontal scrollbar buttons.
	RectPadding bevel;         ///< Bevel thickness, affected by "scaled bevels" game option.
	RectPadding fullbevel;     ///< Always-scaled bevel thickness.
	RectPadding framerect;     ///< Standard padding inside many panels.
	RectPadding frametext;     ///< Padding inside frame with text.
	RectPadding matrix;        ///< Padding of WWT_MATRIX items.
	RectPadding shadebox;      ///< Padding around image in shadebox widget.
	RectPadding stickybox;     ///< Padding around image in stickybox widget.
	RectPadding debugbox;      ///< Padding around image in debugbox widget.
	RectPadding defsizebox;    ///< Padding around image in defsizebox widget.
	RectPadding resizebox;     ///< Padding around image in resizebox widget.
	RectPadding closebox;      ///< Padding around image in closebox widget.
	RectPadding captiontext;   ///< Padding for text within caption widget.
	RectPadding dropdowntext;  ///< Padding of drop down list item.
	RectPadding dropdownlist;  ///< Padding of complete drop down list.
	RectPadding modalpopup;    ///< Spacing for popup warning/information windows.
	RectPadding picker;        ///< Padding for a picker (dock, station, etc) window.
	RectPadding sparse;        ///< Padding used for 'sparse' widget window, usually containing multiple frames.
	RectPadding sparse_resize; ///< Padding used for a resizeable 'sparse' widget window, usually containing multiple frames.

	int vsep_picker;          ///< Vertical spacing of picker-window widgets.
	int vsep_normal;          ///< Normal vertical spacing.
	int vsep_sparse;          ///< Normal vertical spacing for 'sparse' widget window.
	int vsep_wide;            ///< Wide vertical spacing.
	int hsep_normal;          ///< Normal horizontal spacing.
	int hsep_wide;            ///< Wide horizontal spacing.
	int hsep_indent;          ///< Width of indentation for tree layouts.

	static const WidgetDimensions unscaled; ///< Unscaled widget dimensions.
	static WidgetDimensions scaled;         ///< Widget dimensions scaled for current zoom level.

	static constexpr float ASPECT_LOCATION = 12.f / 14.f;
	static constexpr float ASPECT_RENAME = 12.f / 14.f;
	static constexpr float ASPECT_SETTINGS_BUTTON = 21.f / 12.f;
	static constexpr float ASPECT_TOGGLE_SIZE = 12.f / 14.f;
	static constexpr float ASPECT_LEFT_RIGHT_BUTTON = 8.f / 12.f;
	static constexpr float ASPECT_UP_DOWN_BUTTON = 11.f / 12.f;
	static constexpr float ASPECT_VEHICLE_ICON = 15.f / 12.f;
	static constexpr float ASPECT_VEHICLE_FLAG = 11.f / 12.f;

private:
	/**
	 * Distances used in drawing widgets.
	 * These constants should not be used elsewhere, use scaled/unscaled WidgetDimensions instead.
	 */
	static constexpr uint WD_SHADEBOX_WIDTH = 12; ///< Minimum width of a standard shade box widget.
	static constexpr uint WD_STICKYBOX_WIDTH = 12; ///< Minimum width of a standard sticky box widget.
	static constexpr uint WD_DEBUGBOX_WIDTH = 12; ///< Minimum width of a standard debug box widget.
	static constexpr uint WD_DEFSIZEBOX_WIDTH = 12; ///< Minimum width of a standard defsize box widget.
	static constexpr uint WD_RESIZEBOX_WIDTH = 12; ///< Minimum width of a resize box widget.
	static constexpr uint WD_CLOSEBOX_WIDTH = 11; ///< Minimum width of a close box widget.
	static constexpr uint WD_CAPTION_HEIGHT = 14; ///< Minimum height of a title bar.
	static constexpr uint WD_DROPDOWN_HEIGHT = 12; ///< Minimum height of a drop down widget.

	friend NWidgetLeaf;
};

inline constexpr WidgetDimensions WidgetDimensions::unscaled = {
	.imgbtn        = { .left =  1, .top =  1, .right =  1, .bottom =  1},
	.inset         = { .left =  2, .top =  1, .right =  2, .bottom =  1},
	.vscrollbar    = { .left =  2, .top =  3, .right =  2, .bottom =  3},
	.hscrollbar    = { .left =  3, .top =  2, .right =  3, .bottom =  2},
	.bevel         = { .left =  1, .top =  1, .right =  1, .bottom =  1},
	.fullbevel     = { .left =  1, .top =  1, .right =  1, .bottom =  1},
	.framerect     = { .left =  2, .top =  1, .right =  2, .bottom =  1},
	.frametext     = { .left =  6, .top =  6, .right =  6, .bottom =  6},
	.matrix        = { .left =  2, .top =  3, .right =  2, .bottom =  1},
	.shadebox      = { .left =  2, .top =  3, .right =  2, .bottom =  3},
	.stickybox     = { .left =  2, .top =  3, .right =  2, .bottom =  3},
	.debugbox      = { .left =  2, .top =  3, .right =  2, .bottom =  3},
	.defsizebox    = { .left =  2, .top =  3, .right =  2, .bottom =  3},
	.resizebox     = { .left =  2, .top =  2, .right =  2, .bottom =  2},
	.closebox      = { .left =  2, .top =  2, .right =  1, .bottom =  2},
	.captiontext   = { .left =  2, .top =  2, .right =  2, .bottom =  2},
	.dropdowntext  = { .left =  2, .top =  1, .right =  2, .bottom =  1},
	.dropdownlist  = { .left =  1, .top =  2, .right =  1, .bottom =  2},
	.modalpopup    = { .left = 20, .top = 10, .right = 20, .bottom = 10},
	.picker        = { .left =  3, .top =  3, .right =  3, .bottom =  3},
	.sparse        = { .left = 10, .top =  8, .right = 10, .bottom =  8},
	.sparse_resize = { .left = 10, .top =  8, .right = 10, .bottom =  0},
	.vsep_picker   = 1,
	.vsep_normal   = 2,
	.vsep_sparse   = 4,
	.vsep_wide     = 8,
	.hsep_normal   = 2,
	.hsep_wide     = 6,
	.hsep_indent   = 10,
};

/* widget.cpp */
void DrawFrameRect(int left, int top, int right, int bottom, Colours colour, FrameFlags flags);

inline void DrawFrameRect(const Rect &r, Colours colour, FrameFlags flags)
{
	DrawFrameRect(r.left, r.top, r.right, r.bottom, colour, flags);
}

void DrawCaption(const Rect &r, Colours colour, Owner owner, TextColour text_colour, StringID str, StringAlignment align, FontSize fs);

/* window.cpp */
extern Window *_z_front_window;
extern Window *_z_back_window;
extern Window *_first_window;
extern Window *_focused_window;

inline uint64_t GetWindowUpdateNumber()
{
	extern uint64_t _window_update_number;
	return _window_update_number;
}

inline void IncrementWindowUpdateNumber()
{
	extern uint64_t _window_update_number;
	_window_update_number++;
}


/** How do we the window to be placed? */
enum WindowPosition : uint8_t {
	WDP_MANUAL,        ///< Manually align the window (so no automatic location finding)
	WDP_AUTO,          ///< Find a place automatically
	WDP_CENTER,        ///< Center the window
	WDP_ALIGN_TOOLBAR, ///< Align toward the toolbar
};

/**
 * Window default widget/window handling flags
 */
enum class WindowDefaultFlag : uint8_t {
	Construction, ///< This window is used for construction; close it whenever changing company.
	Modal, ///< The window is a modal child of some other window, meaning the parent is 'inactive'
	NoFocus, ///< This window won't get focus/make any other window lose focus when click
	NoClose, ///< This window can't be interactively closed
	Network, ///< This window is used for network client functionality
};
using WindowDefaultFlags = EnumBitSet<WindowDefaultFlag, uint8_t>;

Point GetToolbarAlignedWindowPosition(int window_width);

struct HotkeyList;

struct WindowDescPreferences {
	bool pref_sticky;              ///< Preferred stickyness.
	int16_t pref_width;            ///< User-preferred width of the window. Zero if unset.
	int16_t pref_height;           ///< User-preferred height of the window. Zero if unset.
};

/**
 * High level window description
 */
struct WindowDesc {

	WindowDesc(const char * const file, const int line, WindowPosition default_pos, const char *ini_key, int16_t def_width_trad, int16_t def_height_trad,
			WindowClass window_class, WindowClass parent_class, WindowDefaultFlags flags,
			const std::span<const NWidgetPart> nwid_parts, HotkeyList *hotkeys = nullptr, WindowDesc *ini_parent = nullptr);

	~WindowDesc();

	const char * const file; ///< Source file of this definition
	const int line; ///< Source line of this definition
	WindowPosition default_pos;    ///< Preferred position of the window. @see WindowPosition()
	WindowClass cls;               ///< Class of the window, @see WindowClass.
	WindowClass parent_cls;        ///< Class of the parent window. @see WindowClass
	const char *ini_key;           ///< Key to store window defaults in openttd.cfg. \c nullptr if nothing shall be stored.
	const WindowDefaultFlags flags; ///< Flags. @see WindowDefaultFlag
	const std::span<const NWidgetPart> nwid_parts; ///< Span of nested widget parts describing the window.
	HotkeyList *hotkeys;           ///< Hotkeys for the window.
	WindowDesc *ini_parent;        ///< Other window desc to use for WindowDescPreferences.

	WindowDescPreferences prefs;   ///< Preferences for this window

	const WindowDescPreferences &GetPreferences() const;
	WindowDescPreferences &GetPreferences() { return const_cast<WindowDescPreferences &>(const_cast<const WindowDesc*>(this)->GetPreferences()); }

	int16_t GetDefaultWidth() const;
	int16_t GetDefaultHeight() const;

	static void LoadFromConfig();
	static void SaveToConfig();

private:
	int16_t default_width_trad;      ///< Preferred initial width of the window (pixels at 1x zoom).
	int16_t default_height_trad;     ///< Preferred initial height of the window (pixels at 1x zoom).

	/**
	 * Dummy private copy constructor to prevent compilers from
	 * copying the structure, which fails due to _window_descs.
	 */
	WindowDesc(const WindowDesc &other);
};

/**
 * Data structure for resizing a window
 */
struct ResizeInfo {
	uint step_width;  ///< Step-size of width resize changes
	uint step_height; ///< Step-size of height resize changes
};

/** State of a sort direction button. */
enum SortButtonState : uint8_t {
	SBS_OFF,  ///< Do not sort (with this button).
	SBS_DOWN, ///< Sort ascending.
	SBS_UP,   ///< Sort descending.
};

/**
 * Window flags.
 */
enum class WindowFlag : uint8_t {
	Timeout,          ///< Window timeout counter.

	Dragging,         ///< Window is being dragged.
	SizingRight,      ///< Window is being resized towards the right.
	SizingLeft,       ///< Window is being resized towards the left.

	Sticky,           ///< Window is made sticky by user
	DisableVpScroll,  ///< Window does not do autoscroll, @see HandleAutoscroll().
	WhiteBorder,      ///< Window white border counter bit mask.
	Highlighted,      ///< Window has a widget that has a highlight.
	Centred,          ///< Window is centered and shall stay centered after ReInit.

	Dirty,            ///< Whole window is dirty, and requires repainting.
	WidgetsDirty,     ///< One or more widgets are dirty, and require repainting.
	DragDirtied,      ///< The window has already been marked dirty as blocks as part of the current drag operation

	NoTabFastForward, ///< Suppress tab to fast-forward if this window is focused
};
using WindowFlags = EnumBitSet<WindowFlag, uint16_t>;

static const int TIMEOUT_DURATION = 7; ///< The initial timeout value for WindowFlag::Timeout.
static const int WHITE_BORDER_DURATION = 3; ///< The initial timeout value for WindowFlag::WhiteBorder.

/**
 * Data structure for a window viewport.
 * A viewport is either following a vehicle (its id in then in #follow_vehicle), or it aims to display a specific
 * location #dest_scrollpos_x, #dest_scrollpos_y (#follow_vehicle is then #INVALID_VEHICLE).
 * The actual location being shown is #scrollpos_x, #scrollpos_y.
 * @see InitializeViewport(), UpdateNextViewportPosition(), ApplyNextViewportPosition(), UpdateViewportCoordinates().
 */
struct ViewportData : Viewport {
	VehicleID follow_vehicle;          ///< VehicleID to follow if following a vehicle, #INVALID_VEHICLE otherwise.
	int32_t scrollpos_x;               ///< Currently shown x coordinate (virtual screen coordinate of topleft corner of the viewport).
	int32_t scrollpos_y;               ///< Currently shown y coordinate (virtual screen coordinate of topleft corner of the viewport).
	int32_t dest_scrollpos_x;          ///< Current destination x coordinate to display (virtual screen coordinate of topleft corner of the viewport).
	int32_t dest_scrollpos_y;          ///< Current destination y coordinate to display (virtual screen coordinate of topleft corner of the viewport).
	int32_t next_scrollpos_x;          ///< Next x coordinate to display (virtual screen coordinate of topleft corner of the viewport).
	int32_t next_scrollpos_y;          ///< Next y coordinate to display (virtual screen coordinate of topleft corner of the viewport).
	bool force_update_overlay_pending; ///< Forced overlay update is pending (see SetViewportPosition)

	void CancelFollow(const Window &viewport_window);
};

struct QueryString;

/* misc_gui.cpp */
enum TooltipCloseCondition : uint8_t {
	TCC_RIGHT_CLICK,
	TCC_HOVER,
	TCC_NONE,
	TCC_HOVER_VIEWPORT,
	TCC_NEXT_LOOP,
	TCC_EXIT_VIEWPORT,
};

typedef std::vector<const Vehicle *> VehicleList;

/**
 * Data structure for an opened window
 */
struct Window : ZeroedMemoryAllocator {
	Window *z_front;             ///< The window in front of us in z-order.
	Window *z_back;              ///< The window behind us in z-order.
	Window *next_window;         ///< The next window in arbitrary iteration order.
	WindowClass window_class;        ///< Window class

private:
	WindowToken window_token;

	/**
	 * Helper allocation function to disallow something.
	 * Don't allow arrays; arrays of Windows are pointless as you need
	 * to destruct them all at the same time too, which is kinda hard.
	 * @param size the amount of space not to allocate
	 */
	inline void *operator new[](size_t size) { NOT_REACHED(); }
	inline void operator delete[](void *ptr) { NOT_REACHED(); }

protected:
	void InitializeData(WindowNumber window_number);
	void InitializePositionSize(int x, int y, int min_width, int min_height);
	virtual void FindWindowPlacementAndResize(int def_width, int def_height);

	std::vector<int> scheduled_invalidation_data;  ///< Data of scheduled OnInvalidateData() calls.
	bool scheduled_resize; ///< Set if window has been resized.

	virtual ~Window();

public:
	Window(WindowDesc &desc);

	virtual void Close(int data = 0);
	static void DeleteClosedWindows();

	WindowDesc &window_desc;    ///< Window description
	WindowFlags flags;          ///< Window flags
	WindowNumber window_number; ///< Window number within the window class

	int scale; ///< Scale of this window -- used to determine how to resize.

	uint8_t timeout_timer;      ///< Timer value of the WindowFlag::Timeout for flags.
	uint8_t white_border_timer; ///< Timer value of the WindowFlag::WhiteBorder for flags.

	int left;   ///< x position of left edge of the window
	int top;    ///< y position of top edge of the window
	int width;  ///< width of the window (number of pixels to the right in x direction)
	int height; ///< Height of the window (number of pixels down in y direction)

	ResizeInfo resize;  ///< Resize information

	Owner owner;        ///< The owner of the content shown in this window. Company colour is acquired from this variable.

	ViewportData *viewport;          ///< Pointer to viewport data, if present.
	NWidgetViewport *viewport_widget; ///< Pointer to viewport widget, if present.
	NWidgetCore *nested_focus;       ///< Currently focused nested widget, or \c nullptr if no nested widget has focus.
	btree::btree_map<WidgetID, QueryString*> querystrings; ///< QueryString associated to WWT_EDITBOX widgets.
	std::unique_ptr<NWidgetBase> nested_root; ///< Root of the nested tree.
	WidgetLookup widget_lookup; ///< Indexed access to the nested widget tree. Do not access directly, use #Window::GetWidget() instead.
	NWidgetStacked *shade_select;    ///< Selection widget (#NWID_SELECTION) to use for shading the window. If \c nullptr, window cannot shade.
	Dimension unshaded_size;         ///< Last known unshaded size (only valid while shaded).

	WidgetID mouse_capture_widget;   ///< ID of current mouse capture widget (e.g. dragged scrollbar). -1 if no widget has mouse capture.

	Window *parent;                  ///< Parent window.

	template <class NWID>
	inline const NWID *GetWidget(WidgetID widnum) const;
	template <class NWID>
	inline NWID *GetWidget(WidgetID widnum);

	const Scrollbar *GetScrollbar(WidgetID widnum) const;
	Scrollbar *GetScrollbar(WidgetID widnum);

	const QueryString *GetQueryString(WidgetID widnum) const;
	QueryString *GetQueryString(WidgetID widnum);
	void UpdateQueryStringSize();

	virtual const struct Textbuf *GetFocusedTextbuf() const;
	virtual Point GetCaretPosition() const;
	virtual Rect GetTextBoundingRect(const char *from, const char *to) const;
	virtual ptrdiff_t GetTextCharacterAtPosition(const Point &pt) const;

	void InitNested(WindowNumber number = 0);
	void CreateNestedTree();
	void FinishInitNested(WindowNumber window_number = 0);

	void ChangeWindowClass(WindowClass cls);

	WindowToken GetWindowToken() const { return this->window_token; }

	/**
	 * Set the timeout flag of the window and initiate the timer.
	 */
	inline void SetTimeout()
	{
		this->flags.Set(WindowFlag::Timeout);
		this->timeout_timer = TIMEOUT_DURATION;
	}

	/**
	 * Set the timeout flag of the window and initiate the timer.
	 */
	inline void SetWhiteBorder()
	{
		this->flags.Set(WindowFlag::WhiteBorder);
		this->white_border_timer = WHITE_BORDER_DURATION;
	}

	void DisableAllWidgetHighlight();
	void SetWidgetHighlight(WidgetID widget_index, TextColour highlighted_colour);
	bool IsWidgetHighlighted(WidgetID widget_index) const;

	/**
	 * Sets the enabled/disabled status of a widget.
	 * By default, widgets are enabled.
	 * On certain conditions, they have to be disabled.
	 * @param widget_index index of this widget in the window
	 * @param disab_stat status to use ie: disabled = true, enabled = false
	 */
	inline void SetWidgetDisabledState(WidgetID widget_index, bool disab_stat)
	{
		NWidgetCore *nwid = this->GetWidget<NWidgetCore>(widget_index);
		if (nwid != nullptr) nwid->SetDisabled(disab_stat);
	}

	/**
	 * Sets a widget to disabled.
	 * @param widget_index index of this widget in the window
	 */
	inline void DisableWidget(WidgetID widget_index)
	{
		SetWidgetDisabledState(widget_index, true);
	}

	/**
	 * Sets a widget to Enabled.
	 * @param widget_index index of this widget in the window
	 */
	inline void EnableWidget(WidgetID widget_index)
	{
		SetWidgetDisabledState(widget_index, false);
	}

	/**
	 * Gets the enabled/disabled status of a widget.
	 * @param widget_index index of this widget in the window
	 * @return status of the widget ie: disabled = true, enabled = false
	 */
	inline bool IsWidgetDisabled(WidgetID widget_index) const
	{
		return this->GetWidget<NWidgetCore>(widget_index)->IsDisabled();
	}

	/**
	 * Check if given widget is focused within this window
	 * @param widget_index : index of the widget in the window to check
	 * @return true if given widget is the focused window in this window
	 */
	inline bool IsWidgetFocused(WidgetID widget_index) const
	{
		return this->nested_focus != nullptr && this->nested_focus->GetIndex() == widget_index;
	}

	/**
	 * Check if given widget has user input focus. This means that both the window
	 * has focus and that the given widget has focus within the window.
	 * @param widget_index : index of the widget in the window to check
	 * @return true if given widget is the focused window in this window and this window has focus
	 */
	inline bool IsWidgetGloballyFocused(WidgetID widget_index) const
	{
		return _focused_window == this && IsWidgetFocused(widget_index);
	}

	/**
	 * Check if given widget is active in the current window layout.
	 * This means that the widget exists, is not disabled and is not in a non-selected NWidgetStacked sub-tree.
	 * @param widget_index : index of the widget in the window to check
	 * @return true if given widget is active in the current window layout
	 */
	inline bool IsWidgetActiveInLayout(WidgetID widget_index) const
	{
		const NWidgetCore *nwid = this->GetWidget<NWidgetCore>(widget_index);
		if (nwid == nullptr) return false;

		return nwid->IsActiveInLayout();
	}

	/**
	 * Sets the lowered/raised status of a widget.
	 * @param widget_index index of this widget in the window
	 * @param lowered_stat status to use ie: lowered = true, raised = false
	 */
	inline void SetWidgetLoweredState(WidgetID widget_index, bool lowered_stat)
	{
		this->GetWidget<NWidgetCore>(widget_index)->SetLowered(lowered_stat);
	}

	/**
	 * Invert the lowered/raised  status of a widget.
	 * @param widget_index index of this widget in the window
	 */
	inline void ToggleWidgetLoweredState(WidgetID widget_index)
	{
		NWidgetCore *nwid = this->GetWidget<NWidgetCore>(widget_index);
		bool lowered_state = nwid->IsLowered();
		nwid->SetLowered(!lowered_state);
	}

	/**
	 * Marks a widget as lowered.
	 * @param widget_index index of this widget in the window
	 */
	inline void LowerWidget(WidgetID widget_index)
	{
		this->SetWidgetLoweredState(widget_index, true);
	}

	/**
	 * Marks a widget as raised.
	 * @param widget_index index of this widget in the window
	 */
	inline void RaiseWidget(WidgetID widget_index)
	{
		this->SetWidgetLoweredState(widget_index, false);
	}

	/**
	 * Marks a widget as raised and dirty (redraw), when it is marked as lowered.
	 * @param widget_index index of this widget in the window
	 */
	inline void RaiseWidgetWhenLowered(WidgetID widget_index)
	{
		NWidgetCore *nwid = this->GetWidget<NWidgetCore>(widget_index);
		if (nwid->IsLowered()) {
			nwid->SetLowered(false);
			nwid->SetDirty(this);
		}
	}

	/**
	 * Gets the lowered state of a widget.
	 * @param widget_index index of this widget in the window
	 * @return status of the widget ie: lowered = true, raised= false
	 */
	inline bool IsWidgetLowered(WidgetID widget_index) const
	{
		return this->GetWidget<NWidgetCore>(widget_index)->IsLowered();
	}

	void UnfocusFocusedWidget();
	bool SetFocusedWidget(WidgetID widget_index);

	EventState HandleEditBoxKey(WidgetID wid, char32_t key, uint16_t keycode);
	bool ClearEditBox(WidgetID wid);
	virtual void InsertTextString(WidgetID wid, const char *str, bool marked, const char *caret, const char *insert_location, const char *replacement_end);

	void HandleButtonClick(WidgetID widget);
	int GetRowFromWidget(int clickpos, WidgetID widget, int padding, int line_height = -1) const;

	void RaiseButtons(bool autoraise = false);

	/**
	 * Sets the enabled/disabled status of a list of widgets.
	 * By default, widgets are enabled.
	 * On certain conditions, they have to be disabled.
	 * @param disab_stat status to use ie: disabled = true, enabled = false
	 * @param widgets list of widgets
	 */
	template <typename... Args>
	void SetWidgetsDisabledState(bool disab_stat, Args... widgets)
	{
		(SetWidgetDisabledState(widgets, disab_stat), ...);
	}

	/**
	 * Sets the lowered/raised status of a list of widgets.
	 * @param lowered_stat status to use ie: lowered = true, raised = false
	 * @param widgets list of widgets
	 */
	template <typename... Args>
	void SetWidgetsLoweredState(bool lowered_stat, Args... widgets)
	{
		(SetWidgetLoweredState(widgets, lowered_stat), ...);
	}

	/**
	 * Raises the widgets and sets widgets dirty that are lowered.
	 * @param widgets list of widgets
	 */
	template <typename... Args>
	void RaiseWidgetsWhenLowered(Args... widgets)
	{
		(this->RaiseWidgetWhenLowered(widgets), ...);
	}

	void SetWidgetDirty(WidgetID widget_index);

	void DrawWidgets() const;
	void DrawViewport(NWidgetDisplayFlags display_flags) const;
	void DrawSortButtonState(WidgetID widget, SortButtonState state) const;
	static int SortButtonWidth();

	Window *FindChildWindow(WindowClass wc = WC_INVALID) const;
	void CloseChildWindows(WindowClass wc = WC_INVALID) const;

	void SetDirty();
	void SetDirtyAsBlocks();
	void ReInit(int rx = 0, int ry = 0, bool reposition = false);

	/** Is window shaded currently? */
	inline bool IsShaded() const
	{
		return this->shade_select != nullptr && this->shade_select->shown_plane == SZSP_HORIZONTAL;
	}

	void SetShaded(bool make_shaded);

	void ScheduleResize();
	void ProcessScheduledResize();
	void InvalidateData(int data = 0, bool gui_scope = true);
	void ProcessScheduledInvalidations();
	void ProcessHighlightedInvalidations();

	/*** Event handling ***/

	/**
	 * Notification that the nested widget tree gets initialized. The event can be used to perform general computations.
	 * @note #nested_root and/or #widget_lookup (normally accessed via #GetWidget()) may not exist during this call.
	 */
	virtual void OnInit() { }

	virtual void ApplyDefaults();

	/**
	 * Compute the initial position of the window.
	 * @param sm_width      Smallest width of the window.
	 * @param sm_height     Smallest height of the window.
	 * @param window_number The window number of the new window.
	 * @return Initial position of the top-left corner of the window.
	 */
	virtual Point OnInitialPosition(int16_t sm_width, int16_t sm_height, int window_number);

	/**
	 * The window must be repainted.
	 * @note This method should not change any state, it should only use drawing functions.
	 */
	virtual void OnPaint()
	{
		this->DrawWidgets();
	}

	/**
	 * Draw the contents of a nested widget.
	 * @param r      Rectangle occupied by the widget.
	 * @param widget Number of the widget to draw.
	 * @note This method may not change any state, it may only use drawing functions.
	 */
	virtual void DrawWidget([[maybe_unused]] const Rect &r, [[maybe_unused]] WidgetID widget) const {}

	/**
	 * Update size and resize step of a widget in the window.
	 * After retrieval of the minimal size and the resize-steps of a widget, this function is called to allow further refinement,
	 * typically by computing the real maximal size of the content. Afterwards, \a size is taken to be the minimal size of the widget
	 * and \a resize is taken to contain the resize steps. For the convenience of the callee, \a padding contains the amount of
	 * padding between the content and the edge of the widget. This should be added to the returned size.
	 * @param widget  Widget number.
	 * @param[in,out] size Size of the widget.
	 * @param padding Recommended amount of space between the widget content and the widget edge.
	 * @param[in,out] fill Fill step of the widget.
	 * @param[in,out] resize Resize step of the widget.
	 */
	virtual void UpdateWidgetSize([[maybe_unused]] WidgetID widget, [[maybe_unused]] Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) {}

	/**
	 * Initialize string parameters for a widget.
	 * Calls to this function are made during initialization to measure the size (that is as part of #InitNested()), during drawing,
	 * and while re-initializing the window. Only for widgets that render text initializing is requested.
	 * @param widget  Widget number.
	 */
	virtual void SetStringParameters([[maybe_unused]] WidgetID widget) const {}

	virtual void OnFocus(Window *previously_focused_window);

	virtual void OnFocusLost(bool closing, Window *newly_focused_window);

	/**
	 * A key has been pressed.
	 * @param key     the Unicode value of the key.
	 * @param keycode the untranslated key code including shift state.
	 * @return #ES_HANDLED if the key press has been handled and no other
	 *         window should receive the event.
	 */
	virtual EventState OnKeyPress(char32_t key, uint16_t keycode) { return ES_NOT_HANDLED; }

	virtual EventState OnHotkey(int hotkey);

	/**
	 * The state of the control key has changed
	 * @return #ES_HANDLED if the change has been handled and no other
	 *         window should receive the event.
	 */
	virtual EventState OnCTRLStateChange() { return ES_NOT_HANDLED; }

	/**
	 * The state of the control key has changed, this is sent even if an OnCTRLStateChange handler has return ES_HANDLED
	 */
	virtual void OnCTRLStateChangeAlways() {}

	/**
	 * The state of the shift key has changed
	 */
	virtual void OnShiftStateChange() {}

	/**
	 * A click with the left mouse button has been made on the window.
	 * @param pt     the point inside the window that has been clicked.
	 * @param widget the clicked widget.
	 * @param click_count Number of fast consecutive clicks at same position
	 */
	virtual void OnClick([[maybe_unused]] Point pt, [[maybe_unused]] WidgetID widget, [[maybe_unused]] int click_count) {}

	/**
	 * A click with the right mouse button has been made on the window.
	 * @param pt     the point inside the window that has been clicked.
	 * @param widget the clicked widget.
	 * @return true if the click was actually handled, i.e. do not show a
	 *         tooltip if tooltip-on-right-click is enabled.
	 */
	virtual bool OnRightClick([[maybe_unused]] Point pt, [[maybe_unused]] WidgetID widget) { return false; }

	/**
	 * The mouse is hovering over a widget in the window, perform an action for it.
	 * @param pt     The point where the mouse is hovering.
	 * @param widget The widget where the mouse is hovering.
	 */
	virtual void OnHover([[maybe_unused]] Point pt, [[maybe_unused]] WidgetID widget) {}

	/**
	 * Event to display a custom tooltip.
	 * @param pt     The point where the mouse is located.
	 * @param widget The widget where the mouse is located.
	 * @return True if the event is handled, false if it is ignored.
	 */
	virtual bool OnTooltip([[maybe_unused]] Point pt, [[maybe_unused]] WidgetID widget, [[maybe_unused]] TooltipCloseCondition close_cond) { return false; }

	/**
	 * An 'object' is being dragged at the provided position, highlight the target if possible.
	 * @param pt     The point inside the window that the mouse hovers over.
	 * @param widget The widget the mouse hovers over.
	 */
	virtual void OnMouseDrag([[maybe_unused]] Point pt, [[maybe_unused]] WidgetID widget) {}

	/**
	 * A dragged 'object' has been released.
	 * @param pt     the point inside the window where the release took place.
	 * @param widget the widget where the release took place.
	 */
	virtual void OnDragDrop([[maybe_unused]] Point pt, [[maybe_unused]] WidgetID widget) {}

	/**
	 * Handle the request for (viewport) scrolling.
	 * @param delta the amount the viewport must be scrolled.
	 */
	virtual void OnScroll([[maybe_unused]] Point delta) {}

	/**
	 * The mouse is currently moving over the window or has just moved outside
	 * of the window. In the latter case pt is (-1, -1).
	 * @param pt     the point inside the window that the mouse hovers over.
	 * @param widget the widget the mouse hovers over.
	 */
	virtual void OnMouseOver([[maybe_unused]] Point pt, [[maybe_unused]] WidgetID widget) {}

	/**
	 * The mouse wheel has been turned.
	 * @param wheel the amount of movement of the mouse wheel.
	 */
	virtual void OnMouseWheel([[maybe_unused]] int wheel) {}


	/**
	 * Called for every mouse loop run, which is at least once per (game) tick.
	 */
	virtual void OnMouseLoop() {}

	/**
	 * Called once per (game) tick.
	 */
	virtual void OnGameTick() {}

	/**
	 * Called once every 100 (game) ticks, or once every 3s, whichever comes last.
	 * In normal game speed the frequency is 1 call every 100 ticks (can be more than 3s).
	 * In fast-forward the frequency is 1 call every ~3s (can be more than 100 ticks).
	 */
	virtual void OnHundredthTick() {}

	/**
	 * Called periodically.
	 */
	virtual void OnRealtimeTick([[maybe_unused]] uint delta_ms) {}

	/**
	 * Called when this window's timeout has been reached.
	 */
	virtual void OnTimeout() {}


	/**
	 * Called after the window got resized.
	 * For nested windows with a viewport, call NWidgetViewport::UpdateViewportCoordinates.
	 */
	virtual void OnResize() {}

	/**
	 * A dropdown option associated to this window has been selected.
	 * @param widget the widget (button) that the dropdown is associated with.
	 * @param index  the element in the dropdown that is selected.
	 */
	virtual void OnDropdownSelect([[maybe_unused]] WidgetID widget, [[maybe_unused]] int index) {}

	virtual void OnDropdownClose(Point pt, WidgetID widget, int index, bool instant_close);

	/**
	 * The text in an editbox has been edited.
	 * @param widget The widget of the editbox.
	 */
	virtual void OnEditboxChanged([[maybe_unused]] WidgetID widget) {}

	/**
	 * The query window opened from this window has closed.
	 * @param str the new value of the string, \c std::nullopt if the window
	 *            was cancelled or an empty string when the default
	 *            button was pressed, i.e. \c str->empty().
	 */
	virtual void OnQueryTextFinished([[maybe_unused]] std::optional<std::string> str) {}

	/** Same, for two-string query windows. */
	virtual void OnQueryTextFinished([[maybe_unused]] std::optional<std::string> str1, [[maybe_unused]] std::optional<std::string> str2) {}

	/**
	 * Some data on this window has become invalid.
	 * @param data information about the changed data.
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	virtual void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) {}

	/**
	 * The user clicked some place on the map when a tile highlight mode
	 * has been set.
	 * @param pt   the exact point on the map that has been clicked.
	 * @param tile the tile on the map that has been clicked.
	 */
	virtual void OnPlaceObject([[maybe_unused]] Point pt, [[maybe_unused]] TileIndex tile) {}

	/**
	 * The user clicked on a vehicle while HT_VEHICLE has been set.
	 * @param v clicked vehicle
	 * @return true if the click is handled, false if it is ignored
	 * @pre v->IsPrimaryVehicle() == true
	 */
	virtual bool OnVehicleSelect([[maybe_unused]] const struct Vehicle *v) { return false; }

	/**
	 * The user clicked on a vehicle while HT_VEHICLE has been set.
	 * @param v clicked vehicle
	 * @return True if the click is handled, false if it is ignored
	 * @pre v->IsPrimaryVehicle() == true
	 */
	virtual bool OnVehicleSelect([[maybe_unused]] VehicleList::const_iterator begin, [[maybe_unused]] VehicleList::const_iterator end) { return false; }

	/**
	 * The user clicked on a template vehicle while HT_VEHICLE has been set.
	 * @param v clicked vehicle. It is guaranteed to be v->IsPrimaryVehicle() == true
	 * @return True if the click is handled, false if it is ignored.
	 */
	virtual bool OnTemplateVehicleSelect(const struct TemplateVehicle *v) { return false; }

	/**
	 * The user cancelled a tile highlight mode that has been set.
	 */
	virtual void OnPlaceObjectAbort() {}


	/**
	 * The user is dragging over the map when the tile highlight mode
	 * has been set.
	 * @param select_method the method of selection (allowed directions)
	 * @param select_proc   what will be created when the drag is over.
	 * @param pt            the exact point on the map where the mouse is.
	 */
	virtual void OnPlaceDrag([[maybe_unused]] ViewportPlaceMethod select_method, [[maybe_unused]] ViewportDragDropSelectionProcess select_proc, [[maybe_unused]] Point pt) {}

	/**
	 * The user has dragged over the map when the tile highlight mode
	 * has been set.
	 * @param select_method the method of selection (allowed directions)
	 * @param select_proc   what should be created.
	 * @param pt            the exact point on the map where the mouse was released.
	 * @param start_tile    the begin tile of the drag.
	 * @param end_tile      the end tile of the drag.
	 */
	virtual void OnPlaceMouseUp([[maybe_unused]] ViewportPlaceMethod select_method, [[maybe_unused]] ViewportDragDropSelectionProcess select_proc, [[maybe_unused]] Point pt, [[maybe_unused]] TileIndex start_tile, [[maybe_unused]] TileIndex end_tile) {}

	/**
	 * The user moves over the map when a tile highlight mode has been set
	 * when the special mouse mode has been set to 'PRESIZE' mode. An
	 * example of this is the tile highlight for dock building.
	 * @param pt   the exact point on the map where the mouse is.
	 * @param tile the tile on the map where the mouse is.
	 */
	virtual void OnPlacePresize([[maybe_unused]] Point pt, [[maybe_unused]] TileIndex tile) {}

	/*** End of the event handling ***/

	/**
	 * Is the data related to this window NewGRF inspectable?
	 * @return true iff it is inspectable.
	 */
	virtual bool IsNewGRFInspectable() const { return false; }

	/**
	 * Show the NewGRF inspection window. When this function is called it is
	 * up to the window to call and pass the right parameters to the
	 * ShowInspectWindow function.
	 * @pre this->IsNewGRFInspectable()
	 */
	virtual void ShowNewGRFInspectWindow() const { NOT_REACHED(); }

	template <class T>
	using window_type = std::conditional_t<std::is_const<T>{}, Window const, Window>;

	enum IterationMode {
		IM_FROM_FRONT,
		IM_FROM_BACK,
		IM_ARBITRARY,
	};

	/**
	 * Iterator to iterate all valid Windows
	 * @tparam T Type of the class/struct that is going to be iterated
	 * @tparam Tmode Iteration mode
	 */
	template <class T, IterationMode Tmode>
	struct WindowIterator {
		typedef T value_type;
		typedef T *pointer;
		typedef T &reference;
		typedef size_t difference_type;
		typedef std::forward_iterator_tag iterator_category;

		explicit WindowIterator(window_type<T> *start) : w(start)
		{
			this->Validate();
		}

		bool operator==(const WindowIterator &other) const { return this->w == other.w; }
		bool operator!=(const WindowIterator &other) const { return !(*this == other); }
		T * operator*() const { return static_cast<T *>(this->w); }
		WindowIterator & operator++() { this->Next(); this->Validate(); return *this; }

	private:
		window_type<T> *w;
		void Validate() { while (this->w != nullptr && this->w->window_class == WC_INVALID) this->Next(); }

		void Next()
		{
			if (this->w != nullptr) {
				switch (Tmode) {
					case IM_FROM_FRONT:
						this->w = this->w->z_back;
						break;
					case IM_FROM_BACK:
						this->w = this->w->z_front;
						break;
					case IM_ARBITRARY:
						this->w = this->w->next_window;
						break;
				}
			}
		}
	};

	/**
	 * Iterable ensemble of all valid Windows
	 * @tparam T Type of the class/struct that is going to be iterated
	 * @tparam Tmode Iteration mode
	 */
	template <class T, IterationMode Tmode>
	struct IterateCommon {
		IterateCommon(window_type<T> *from) : from(from) {}
		WindowIterator<T, Tmode> begin() { return WindowIterator<T, Tmode>(this->from); }
		WindowIterator<T, Tmode> end() { return WindowIterator<T, Tmode>(nullptr); }
		bool empty() { return this->begin() == this->end(); }
	private:
		window_type<T> *from;
	};

	/**
	 * Returns an iterable ensemble of all valid Window from back to front
	 * @tparam T Type of the class/struct that is going to be iterated
	 * @param from index of the first Window to consider
	 * @return an iterable ensemble of all valid Window
	 */
	template <class T = Window>
	static IterateCommon<T, IM_FROM_BACK> IterateFromBack(window_type<T> *from = _z_back_window) { return IterateCommon<T, IM_FROM_BACK>(from); }

	/**
	 * Returns an iterable ensemble of all valid Window from front to back
	 * @tparam T Type of the class/struct that is going to be iterated
	 * @param from index of the first Window to consider
	 * @return an iterable ensemble of all valid Window
	 */
	template <class T = Window>
	static IterateCommon<T, IM_FROM_FRONT> IterateFromFront(window_type<T> *from = _z_front_window) { return IterateCommon<T, IM_FROM_FRONT>(from); }

	/**
	 * Returns an iterable ensemble of all valid Window in an arbitrary order which is safe to use when deleting
	 * @tparam T Type of the class/struct that is going to be iterated
	 * @param from index of the first Window to consider
	 * @return an iterable ensemble of all valid Window
	 */
	template <class T = Window>
	static IterateCommon<T, IM_ARBITRARY> Iterate(window_type<T> *from = _first_window) { return IterateCommon<T, IM_ARBITRARY>(from); }
};

/**
 * Generic helper function that checks if all elements of the range are equal with respect to the given predicate.
 * @param begin The start of the range.
 * @param end The end of the range.
 * @param pred The predicate to use.
 * @return True if all elements are equal, false otherwise.
 */
template <class It, class Pred>
inline bool AllEqual(It begin, It end, Pred pred)
{
	return std::adjacent_find(begin, end, std::not_fn(pred)) == end;
}

/**
 * Get the nested widget with number \a widnum from the nested widget tree.
 * @tparam NWID Type of the nested widget.
 * @param widnum Widget number of the widget to retrieve.
 * @return The requested widget if it is instantiated, \c nullptr otherwise.
 */
template <class NWID>
inline NWID *Window::GetWidget(WidgetID widnum)
{
	auto it = this->widget_lookup.find(widnum);
	if (it == std::end(this->widget_lookup)) return nullptr;
	NWID *nwid = dynamic_cast<NWID *>(it->second);
	assert(nwid != nullptr);
	return nwid;
}

/** Specialized case of #Window::GetWidget for the nested widget base class. */
template <>
inline const NWidgetBase *Window::GetWidget<NWidgetBase>(WidgetID widnum) const
{
	auto it = this->widget_lookup.find(widnum);
	if (it == std::end(this->widget_lookup)) return nullptr;
	return it->second;
}

/**
 * Get the nested widget with number \a widnum from the nested widget tree.
 * @tparam NWID Type of the nested widget.
 * @param widnum Widget number of the widget to retrieve.
 * @return The requested widget if it is instantiated, \c nullptr otherwise.
 */
template <class NWID>
inline const NWID *Window::GetWidget(WidgetID widnum) const
{
	return const_cast<Window *>(this)->GetWidget<NWID>(widnum);
}


/**
 * Base class for windows opened from a toolbar.
 */
class PickerWindowBase : public Window {

public:
	PickerWindowBase(WindowDesc &desc, Window *parent) : Window(desc)
	{
		this->parent = parent;
	}

	void Close([[maybe_unused]] int data = 0) override;
};

void BringWindowToFront(Window *w);
Window *BringWindowToFrontById(WindowClass cls, WindowNumber number);
Window *FindWindowFromPt(int x, int y);

/**
 * Open a new window.
 * @tparam Twindow %Window class to use if the window does not exist.
 * @tparam Treturn_existing If set, also return the window if it already existed.
 * @param desc The pointer to the WindowDesc to be created
 * @param window_number the window number of the new window
 * @param extra_arguments optional extra arguments to pass to the window's constructor.
 * @return %Window pointer of the newly created window, or the existing one if \a Treturn_existing is set, or \c nullptr.
 */
template <typename Twindow, bool Treturn_existing = false, typename... Targs>
Twindow *AllocateWindowDescFront(WindowDesc &desc, WindowNumber window_number, Targs... extra_arguments)
{
	Twindow *w = static_cast<Twindow *>(BringWindowToFrontById(desc.cls, window_number));
	if (w != nullptr) return Treturn_existing ? w : nullptr;
	return new Twindow(desc, window_number, std::forward<Targs>(extra_arguments)...);
}

void RelocateAllWindows(int neww, int newh);

void GuiShowTooltips(Window *parent, StringID str, TooltipCloseCondition close_tooltip, uint paramcount = 0);

/* widget.cpp */
WidgetID GetWidgetFromPos(const Window *w, int x, int y);

extern Point _cursorpos_drag_start;

extern int _scrollbar_start_pos;
extern int _scrollbar_size;
extern uint8_t _scroller_click_timeout;

extern Window *_scrolling_viewport;
extern Rect _scrolling_viewport_bound;
extern bool _mouse_hovering;

/** Mouse modes. */
enum SpecialMouseMode : uint8_t {
	WSM_NONE,     ///< No special mouse mode.
	WSM_DRAGDROP, ///< Drag&drop an object.
	WSM_SIZING,   ///< Sizing mode.
	WSM_PRESIZE,  ///< Presizing mode (docks, tunnels).
	WSM_DRAGGING, ///< Dragging mode (trees).
};
extern SpecialMouseMode _special_mouse_mode;

void SetFocusedWindow(Window *w);

void ScrollbarClickHandler(Window *w, NWidgetCore *nw, int x, int y);
Rect ScrollRect(Rect r, const Scrollbar &sb, int resize_step = 1);

/**
 * Returns whether a window may be shown or not.
 * @param w The window to consider.
 * @return True iff it may be shown, otherwise false.
 */
inline bool MayBeShown(const Window *w)
{
	/* If we're not modal, everything is okay. */
	extern bool _in_modal_progress;
	if (likely(!_in_modal_progress)) return true;

	switch (w->window_class) {
		case WC_MAIN_WINDOW:    ///< The background, i.e. the game.
		case WC_MODAL_PROGRESS: ///< The actual progress window.
		case WC_CONFIRM_POPUP_QUERY: ///< The abort window.
			return true;

		default:
			return false;
	}
}

struct GeneralVehicleWindow : public Window {
	const Vehicle *vehicle;

	GeneralVehicleWindow(WindowDesc &desc, const Vehicle *v) : Window(desc), vehicle(v) {}
};

#endif /* WINDOW_GUI_H */
