/*
	Copyright 2016 _that/TeamWin
	This file is part of TWRP/TeamWin Recovery Project.

	Copyright (C) 2018-2025 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	TWRP is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	TWRP is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/

// terminal.cpp - GUITerminal object
//
// The terminal back-end (TerminalEngine) is built on libvterm: it owns a real
// xterm-class terminal state machine and cell grid. We feed it PTY bytes and it
// maintains the screen, scrollback, cursor, colours and attributes. The GUI side
// (GUITerminal, a GUIScrollList) only renders the resulting cells and forwards
// key/character input. This replaces the old hand-rolled VT100 subset parser and
// gives us SGR colours, the alternate screen (vi/htop/less), scroll regions, etc.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termio.h>

#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <cctype>
#include <linux/input.h>
#include <poll.h>
#include <sys/wait.h>

#include <vterm.h>

extern "C" {
#include "../twcommon.h"
}
#include "minuitwrp/minui.h"
#include "minuitwrp/truetype.hpp"

#include "gui.hpp"
#include "recovery_ui2/backend.hpp"

#include "rapidxml.hpp"
#include "objects.hpp"

#if 0
#define debug_printf printf
#else
#define debug_printf(...)
#endif

extern int g_pty_fd; // in gui.cpp where the select is

// Maximum number of scrollback lines kept above the visible screen. Kept modest
// on purpose: each line stores a full row of VTermScreenCell, and recovery RAM is
// limited. ~2000 lines is plenty for an on-device terminal.
static const size_t kMaxScrollback = 2000;

// Append a UTF-8 codepoint to string s
static size_t utf8add(std::string& s, uint32_t cp)
{
	if (cp < 0x80) {
		s += (char)cp;
		return 1;
	}
	else if (cp < 0x800) {
		s += (char)(0xc0 | (cp >> 6));
		s += (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	else if (cp < 0x10000) {
		s += (char)(0xe0 | (cp >> 12));
		s += (char)(0x80 | ((cp >> 6) & 0x3f));
		s += (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	else if (cp < 0x110000) {
		s += (char)(0xf0 | (cp >> 18));
		s += (char)(0x80 | ((cp >> 12) & 0x3f));
		s += (char)(0x80 | ((cp >> 6) & 0x3f));
		s += (char)(0x80 | (cp & 0x3f));
		return 4;
	}
	return 0;
}

/*
Pseudoterminal handler.
*/
class Pseudoterminal
{
public:
	Pseudoterminal() : fdMaster(0), pid(0)
	{
	}

	bool started() const { return pid > 0; }
	int descriptor() const { return fdMaster; }

	bool start()
	{
		fdMaster = getpt();
		if (fdMaster < 0) {
			LOGERR("Error %d on getpt()\n", errno);
			return false;
		}

		if (unlockpt(fdMaster) != 0) {
			LOGERR("Error %d on unlockpt()\n", errno);
			return false;
		}

		pid = fork();
		if (pid < 0) {
			LOGERR("fork failed for pty, error %d\n", errno);
			close(fdMaster);
			pid = 0;
			return false;
		}
		else if (pid) {
			// child started, now someone needs to periodically read from fdMaster
			// and write it to the terminal
			// this currently works through gui.cpp calling terminal_pty_read below
			g_pty_fd = fdMaster;
			set_select_fd();
			return true;
		}
		else {
			int fdSlave = open(ptsname(fdMaster), O_RDWR);
			close(fdMaster);
			runSlave(fdSlave);
		}
		// we can't get here
		LOGERR("impossible error in pty\n");
		return false;
	}

	void runSlave(int fdSlave)
	{
		dup2(fdSlave, 0); // PTY becomes standard input (0)
		dup2(fdSlave, 1); // PTY becomes standard output (1)
		dup2(fdSlave, 2); // PTY becomes standard error (2)

		// Now the original file descriptor is useless
		close(fdSlave);

		// Make the current process a new session leader
		if (setsid() == (pid_t)-1)
			LOGERR("setsid failed: %d\n", errno);

		// As the child is a session leader, set the controlling terminal to be the slave side of the PTY
		// (Mandatory for programs like the shell to make them manage correctly their outputs)
		ioctl(0, TIOCSCTTY, 1);

		// Advertise full xterm-class emulation. libvterm understands xterm-256color
		// sequences, and the matching terminfo entry is shipped under
		// /system/etc/terminfo (see Android.mk). TERMINFO points there in case the
		// system database is not on the default search path in recovery.
		setenv("TERM", "xterm-256color", 1);
		setenv("TERMINFO", "/system/etc/terminfo", 1);

		execl("/system/bin/sh", "sh", NULL);
		_exit(127);
	}

	int read(char* buffer, size_t size)
	{
		if (!started()) {
			LOGERR("someone tried to read from pty, but it was not started\n");
			return -1;
		}
		int rc = ::read(fdMaster, buffer, size);
		debug_printf("pty read: %d bytes\n", rc);
		if (rc < 0) {
			// assume child has died (usual errno when shell exits seems to be EIO == 5)
			if (errno != EIO)
				LOGERR("pty read failed: %d\n", errno);
			stop();
		}
		return rc;
	}

	int write(const char* buffer, size_t size)
	{
		if (!started()) {
			LOGERR("someone tried to write to pty, but it was not started\n");
			return -1;
		}
		int rc = ::write(fdMaster, buffer, size);
		debug_printf("pty write: %zu bytes -> %d\n", size, rc);
		if (rc < 0) {
			LOGERR("pty write failed: %d\n", errno);
			// assume child has died
			stop();
		}
		return rc;
	}

	template<size_t n>
	inline int write(const char (&literal)[n])
	{
		return write(literal, n-1);
	}

	void resize(int xChars, int yChars, int w, int h)
	{
		struct winsize ws;
		ws.ws_row = yChars;
		ws.ws_col = xChars;
		ws.ws_xpixel = w;
		ws.ws_ypixel = h;
		if (ioctl(fdMaster, TIOCSWINSZ, &ws) < 0)
			LOGERR("failed to set window size, error %d\n", errno);
	}

	void stop()
	{
		if (!started()) {
			LOGERR("someone tried to stop pty, but it was not started\n");
			return;
		}
		close(fdMaster);
		g_pty_fd = fdMaster = -1;
		set_select_fd();
		int status;
		waitpid(pid, &status, WNOHANG); // avoid zombies but don't hang if the child is still alive and we got here due to some error
		pid = 0;
	}

private:
	int fdMaster;
	pid_t pid;
};

// A single rendered cell handed to the GUI side. Colours are already resolved to
// RGB, except that the default-fg/default-bg flags are preserved so the renderer
// can substitute the theme's text/background colours.
struct RenderCell
{
	std::string text;       // UTF-8 for the cell glyph (empty means blank)
	uint8_t width;          // 1 for normal, 2 for wide (CJK), 0 for the right half of a wide cell
	bool bold;
	bool underline;
	bool reverse;
	bool fgDefault, bgDefault;
	uint8_t fr, fg, fb;     // foreground RGB (valid when !fgDefault)
	uint8_t br, bg, bb;     // background RGB (valid when !bgDefault)

	RenderCell() : width(1), bold(false), underline(false), reverse(false),
		fgDefault(true), bgDefault(true),
		fr(0), fg(0), fb(0), br(0), bg(0), bb(0) {}
};

/*
TerminalEngine is the terminal back-end, wrapping a libvterm instance. It owns the
PTY, the terminal state machine and the scrollback buffer. It does not care about
visual things like fonts or windows.
The idea is that 0 to n GUITerminal instances (e.g. on different pages) can connect
to one TerminalEngine to interact with the terminal, and that the TerminalEngine
survives things like page changes or even theme reloads.
*/
class TerminalEngine
{
public:
	TerminalEngine()
		: vt(NULL), screen(NULL), rows(10), cols(40),
		  cursorRow(0), cursorCol(0), cursorVisible(true), updateCounter(0)
	{
		createVterm();
	}

	~TerminalEngine()
	{
		if (vt)
			vterm_free(vt);
	}

	void setSize(int xChars, int yChars, int w, int h)
	{
		xChars = std::max(xChars, 1);
		yChars = std::max(yChars, 1);
		if (xChars == cols && yChars == rows) {
			// still update the pty pixel size, harmless
			if (pty.started())
				pty.resize(cols, rows, w, h);
			return;
		}
		cols = xChars;
		rows = yChars;
		if (vt)
			vterm_set_size(vt, rows, cols); // note: libvterm takes (rows, cols)
		if (pty.started())
			pty.resize(cols, rows, w, h);
		++updateCounter;
		debug_printf("setSize: %d*%d chars, %d*%d pixels\n", xChars, yChars, w, h);
	}

	void initPty()
	{
		if (!pty.started()) {
			pty.start();
			pty.resize(cols, rows, 0, 0);
		}
	}

	void readPty()
	{
		char buffer[4096];
		int rc = pty.read(buffer, sizeof(buffer));
		debug_printf("readPty: %d bytes\n", rc);
		if (rc <= 0) {
			// EOF means the shell closed the slave end. Mark the pty stopped so
			// AERA's non-blocking poll timer does not keep observing POLLHUP and
			// repeatedly redraw the terminal forever.
			if (rc == 0)
				pty.stop();
			const char msg[] = "\r\nChild process exited.\r\n";
			vterm_input_write(vt, msg, sizeof(msg) - 1);
		}
		else {
			vterm_input_write(vt, buffer, rc);
		}
		vterm_screen_flush_damage(screen);
		++updateCounter;
	}

	bool pollPty()
	{
		if (!pty.started())
			return false;
		bool changed = false;
		// Drain a bounded number of ready chunks. This is called from AERA's
		// LVGL timer and must never block its frame loop.
		for (int i = 0; i < 8; ++i) {
			struct pollfd descriptor = { pty.descriptor(), POLLIN, 0 };
			if (poll(&descriptor, 1, 0) <= 0 ||
			    !(descriptor.revents & (POLLIN | POLLHUP | POLLERR)))
				break;
			readPty();
			changed = true;
			if (!pty.started())
				break;
		}
		return changed;
	}

	bool status() {
		return pty.started();
	}

	void stop() {
		pty.stop();
	}

	void clear()
	{
		scrollback.clear();
		if (screen)
			vterm_screen_reset(screen, 1);
		cursorRow = cursorCol = 0;
		++updateCounter;
	}

	// --- input ---

	bool inputChar(int ch)
	{
		debug_printf("inputChar: %d\n", ch);
		initPty(); // reinit just in case it died before
		if (ch < 0x20 || ch == 0x7f) {
			// control characters (incl. Enter, Tab, Ctrl-*) go to the pty verbatim,
			// so app-mode and signal-generating keys keep working as the shell expects
			char c = (char)ch;
			pty.write(&c, 1);
		}
		else {
			vterm_keyboard_unichar(vt, (uint32_t)ch, VTERM_MOD_NONE);
		}
		return true;
	}

	bool inputKey(int key)
	{
		debug_printf("inputKey: %d\n", key);
		initPty();
		VTermKey vk = VTERM_KEY_NONE;
		switch (key)
		{
			case KEY_UP: vk = VTERM_KEY_UP; break;
			case KEY_DOWN: vk = VTERM_KEY_DOWN; break;
			case KEY_RIGHT: vk = VTERM_KEY_RIGHT; break;
			case KEY_LEFT: vk = VTERM_KEY_LEFT; break;
			case KEY_HOME: vk = VTERM_KEY_HOME; break;
			case KEY_END: vk = VTERM_KEY_END; break;
			case KEY_INSERT: vk = VTERM_KEY_INS; break;
			case KEY_DELETE: vk = VTERM_KEY_DEL; break;
			case KEY_PAGEUP: vk = VTERM_KEY_PAGEUP; break;
			case KEY_PAGEDOWN: vk = VTERM_KEY_PAGEDOWN; break;
			case KEY_ENTER: vk = VTERM_KEY_ENTER; break;
			case KEY_TAB: vk = VTERM_KEY_TAB; break;
			case KEY_BACKSPACE: vk = VTERM_KEY_BACKSPACE; break;
			case KEY_ESC: vk = VTERM_KEY_ESCAPE; break;
			// F-keys are not contiguous in linux/input.h (F11/F12 jump), so map them explicitly
			case KEY_F1: vk = (VTermKey)VTERM_KEY_FUNCTION(1); break;
			case KEY_F2: vk = (VTermKey)VTERM_KEY_FUNCTION(2); break;
			case KEY_F3: vk = (VTermKey)VTERM_KEY_FUNCTION(3); break;
			case KEY_F4: vk = (VTermKey)VTERM_KEY_FUNCTION(4); break;
			case KEY_F5: vk = (VTermKey)VTERM_KEY_FUNCTION(5); break;
			case KEY_F6: vk = (VTermKey)VTERM_KEY_FUNCTION(6); break;
			case KEY_F7: vk = (VTermKey)VTERM_KEY_FUNCTION(7); break;
			case KEY_F8: vk = (VTermKey)VTERM_KEY_FUNCTION(8); break;
			case KEY_F9: vk = (VTermKey)VTERM_KEY_FUNCTION(9); break;
			case KEY_F10: vk = (VTermKey)VTERM_KEY_FUNCTION(10); break;
			case KEY_F11: vk = (VTermKey)VTERM_KEY_FUNCTION(11); break;
			case KEY_F12: vk = (VTermKey)VTERM_KEY_FUNCTION(12); break;
			default:
				return false;
		}
		vterm_keyboard_key(vt, vk, VTERM_MOD_NONE);
		return true;
	}

	// --- queries used by the renderer ---

	int getCols() const { return cols; }
	int getRows() const { return rows; }
	size_t getLinesCount() const { return scrollback.size() + rows; }
	int getCursorAbsRow() const { return (int)scrollback.size() + cursorRow; }
	int getCursorCol() const { return cursorCol; }
	bool isCursorVisible() const { return cursorVisible; }
	int getUpdateCounter() const { return updateCounter; }

	// Fill 'out' with the cells of the given absolute row (scrollback rows come
	// first, then the live screen rows).
	void getRow(size_t absRow, std::vector<RenderCell>& out)
	{
		out.clear();
		if (absRow < scrollback.size()) {
			const std::vector<VTermScreenCell>& line = scrollback[absRow];
			out.resize(line.size());
			for (size_t c = 0; c < line.size(); ++c)
				fillRenderCell(line[c], out[c]);
			return;
		}
		int r = (int)(absRow - scrollback.size());
		if (r < 0 || r >= rows)
			return;
		out.resize(cols);
		for (int c = 0; c < cols; ++c) {
			VTermScreenCell cell;
			VTermPos pos; pos.row = r; pos.col = c;
			if (vterm_screen_get_cell(screen, pos, &cell))
				fillRenderCell(cell, out[c]);
		}
	}

private:
	void createVterm()
	{
		vt = vterm_new(rows, cols);
		vterm_set_utf8(vt, 1);
		screen = vterm_obtain_screen(vt);
		vterm_screen_set_callbacks(screen, &kCallbacks, this);
		vterm_screen_enable_altscreen(screen, 1);
		vterm_screen_reset(screen, 1);
		// libvterm emits replies (cursor reports, DA, etc.) through this callback;
		// route them straight back to the shell's pty.
		vterm_output_set_callback(vt, &outputCb, this);
	}

	void fillRenderCell(const VTermScreenCell& c, RenderCell& rc)
	{
		rc.text.clear();
		if (c.chars[0] == 0xffffffff) {
			// right half of a wide character: nothing to draw here
			rc.width = 0;
			return;
		}
		for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && c.chars[i]; ++i)
			utf8add(rc.text, c.chars[i]);
		rc.width = c.width ? c.width : 1;
		rc.bold = c.attrs.bold;
		rc.underline = c.attrs.underline != 0;
		rc.reverse = c.attrs.reverse;

		VTermColor fg = c.fg;
		VTermColor bg = c.bg;
		rc.fgDefault = VTERM_COLOR_IS_DEFAULT_FG(&fg);
		rc.bgDefault = VTERM_COLOR_IS_DEFAULT_BG(&bg);
		if (!rc.fgDefault) {
			vterm_screen_convert_color_to_rgb(screen, &fg);
			rc.fr = fg.rgb.red; rc.fg = fg.rgb.green; rc.fb = fg.rgb.blue;
		}
		if (!rc.bgDefault) {
			vterm_screen_convert_color_to_rgb(screen, &bg);
			rc.br = bg.rgb.red; rc.bg = bg.rgb.green; rc.bb = bg.rgb.blue;
		}
	}

	// --- libvterm callbacks ---

	void pushScrollback(int ncols, const VTermScreenCell* cells)
	{
		std::vector<VTermScreenCell> line(cells, cells + ncols);
		scrollback.push_back(line);
		while (scrollback.size() > kMaxScrollback)
			scrollback.pop_front();
		++updateCounter;
	}

	int popScrollback(int ncols, VTermScreenCell* cells)
	{
		if (scrollback.empty())
			return 0;
		const std::vector<VTermScreenCell>& line = scrollback.back();
		int n = std::min((int)line.size(), ncols);
		for (int i = 0; i < n; ++i)
			cells[i] = line[i];
		// pad the remainder with blank cells
		for (int i = n; i < ncols; ++i) {
			VTermScreenCell blank;
			memset(&blank, 0, sizeof(blank));
			blank.width = 1;
			cells[i] = blank;
		}
		scrollback.pop_back();
		++updateCounter;
		return 1;
	}

	static int damageCb(VTermRect, void* user)
	{
		((TerminalEngine*)user)->updateCounter++;
		return 1;
	}
	static int moverectCb(VTermRect, VTermRect, void* user)
	{
		((TerminalEngine*)user)->updateCounter++;
		return 1;
	}
	static int movecursorCb(VTermPos pos, VTermPos, int visible, void* user)
	{
		TerminalEngine* e = (TerminalEngine*)user;
		e->cursorRow = pos.row;
		e->cursorCol = pos.col;
		e->cursorVisible = visible != 0;
		e->updateCounter++;
		return 1;
	}
	static int settermpropCb(VTermProp prop, VTermValue* val, void* user)
	{
		TerminalEngine* e = (TerminalEngine*)user;
		if (prop == VTERM_PROP_CURSORVISIBLE)
			e->cursorVisible = val->boolean != 0;
		e->updateCounter++;
		return 1;
	}
	static int bellCb(void* /*user*/)
	{
#ifndef TW_NO_HAPTICS
		DataManager::Vibrate("tw_button_vibrate");
#endif
		return 1;
	}
	static int sbPushlineCb(int ncols, const VTermScreenCell* cells, void* user)
	{
		((TerminalEngine*)user)->pushScrollback(ncols, cells);
		return 1;
	}
	static int sbPoplineCb(int ncols, VTermScreenCell* cells, void* user)
	{
		return ((TerminalEngine*)user)->popScrollback(ncols, cells);
	}
	static int sbClearCb(void* user)
	{
		TerminalEngine* e = (TerminalEngine*)user;
		e->scrollback.clear();
		e->updateCounter++;
		return 1;
	}
	static void outputCb(const char* s, size_t len, void* user)
	{
		((TerminalEngine*)user)->pty.write(s, len);
	}

	static const VTermScreenCallbacks kCallbacks;

	VTerm* vt;
	VTermScreen* screen;
	Pseudoterminal pty;

	std::deque<std::vector<VTermScreenCell> > scrollback;

	int rows, cols;
	int cursorRow, cursorCol; // 0-based, relative to the live screen
	bool cursorVisible;
	int updateCounter; // changes whenever the terminal could require a redraw
};

const VTermScreenCallbacks TerminalEngine::kCallbacks = {
	&TerminalEngine::damageCb,
	&TerminalEngine::moverectCb,
	&TerminalEngine::movecursorCb,
	&TerminalEngine::settermpropCb,
	&TerminalEngine::bellCb,
	NULL, // resize: we drive sizing ourselves
	&TerminalEngine::sbPushlineCb,
	&TerminalEngine::sbPoplineCb,
	&TerminalEngine::sbClearCb,
	NULL, // sb_pushline4
};

// The one and only terminal engine for now
TerminalEngine gEngine;

namespace recovery_ui2 {

void RecoveryTerminalStart(int columns, int rows, int pixel_width,
		int pixel_height)
{
	gEngine.setSize(columns, rows, pixel_width, pixel_height);
	gEngine.initPty();
}

bool RecoveryTerminalPoll()
{
	return gEngine.pollPty();
}

int RecoveryTerminalUpdateCounter()
{
	return gEngine.getUpdateCounter();
}

std::vector<std::string> RecoveryTerminalLines(size_t maximum_lines)
{
	std::vector<std::string> result;
	const size_t total = gEngine.getLinesCount();
	const size_t first = total > maximum_lines ? total - maximum_lines : 0;
	result.reserve(total - first);
	for (size_t row_index = first; row_index < total; ++row_index) {
		std::vector<RenderCell> row;
		gEngine.getRow(row_index, row);
		std::string line;
		for (const RenderCell& cell : row) {
			if (cell.width == 0)
				continue;
			line += cell.text.empty() ? " " : cell.text;
		}
		while (!line.empty() && line.back() == ' ')
			line.pop_back();
		result.push_back(line);
	}
	return result;
}

void RecoveryTerminalWrite(const std::string& text)
{
	for (unsigned char character : text)
		gEngine.inputChar(character);
}

void RecoveryTerminalSendKey(TerminalKey key)
{
	switch (key) {
		case TerminalKey::kUp: gEngine.inputKey(KEY_UP); break;
		case TerminalKey::kDown: gEngine.inputKey(KEY_DOWN); break;
		case TerminalKey::kLeft: gEngine.inputKey(KEY_LEFT); break;
		case TerminalKey::kRight: gEngine.inputKey(KEY_RIGHT); break;
		case TerminalKey::kTab: gEngine.inputKey(KEY_TAB); break;
		case TerminalKey::kEscape: gEngine.inputKey(KEY_ESC); break;
		case TerminalKey::kInterrupt: gEngine.inputChar(3); break;
	}
}

void RecoveryTerminalClear()
{
	gEngine.clear();
}

bool RecoveryTerminalRunning()
{
	return gEngine.status();
}

} // namespace recovery_ui2

void terminal_pty_read()
{
	gEngine.readPty();
}


GUITerminal::GUITerminal(xml_node<>* node) : GUIScrollList(node)
{
	allowSelection = false; // terminal doesn't support list item selections
	lastCondition = false;

	if (!node) {
		mRenderX = 0;
		mRenderY = 0;
		mRenderW = gr_fb_width();
		mRenderH = gr_fb_height();
	}

	engine = &gEngine;
	updateCounter = 0;
}

int GUITerminal::Update(void)
{
	if (!isConditionTrue()) {
		lastCondition = false;
		return 0;
	}

	if (lastCondition == false) {
		lastCondition = true;
		// we're becoming visible, so we might need to resize the terminal content
		InitAndResize();
	}

	if (updateCounter != engine->getUpdateCounter()) {
		// try to keep the cursor in view
		SetVisibleListLocation(engine->getCursorAbsRow());
		updateCounter = engine->getUpdateCounter();
		mUpdate = 1;
	}

	GUIScrollList::Update();

	if (mUpdate) {
		mUpdate = 0;
		if (Render() == 0)
			return 2;
	}
	return 0;
}

// NotifyTouch - Notify of a touch event
//  Return 0 on success, >0 to ignore remainder of touch, and <0 on error
int GUITerminal::NotifyTouch(TOUCH_STATE state, int x, int y)
{
	if (!isConditionTrue())
		return -1;

	// [f/d] hide/show keyboard
	switch (state) {
		case TOUCH_START:
			blockKeyboard = false; break;
		case TOUCH_DRAG:
			if (abs(y - lastY) < touchDebounce)
				break;
			blockKeyboard = true; break;
		case TOUCH_RELEASE:
			if (!blockKeyboard) {
				DataManager::SetValue("tw_hide_kb",
					DataManager::GetIntValue("tw_hide_kb") == 0 ? 1 : 0);
				#ifndef TW_NO_HAPTICS
					DataManager::Vibrate("tw_button_vibrate");
				#endif
			}
		break;
		default:
  		break;
	}

	// TODO: grab focus correctly
	// TODO: fix focus handling in PageManager and GUIInput
	SetInputFocus(1);
	debug_printf("Terminal: SetInputFocus\n");
	return GUIScrollList::NotifyTouch(state, x, y);
	// TODO later: allow cursor positioning by touch (simulate mouse click?)
}

int GUITerminal::NotifyKey(int key, bool down)
{
	if (!HasInputFocus)
		return 1;
	if (down)
		if (engine->inputKey(key))
			mUpdate = 1;
	return 0;
}

// character input
int GUITerminal::NotifyCharInput(int ch)
{
	if (engine->inputChar(ch))
		mUpdate = 1;
	return 0;
}

size_t GUITerminal::GetItemCount() const
{
	return engine->getLinesCount();
}

void GUITerminal::RenderItem(size_t itemindex, int yPos, bool selected __unused)
{
	if (!mFont || !mFont->GetResource())
		return;
	void* font = mFont->GetResource();

	int charWidth = twrpTruetype::gr_ttf_measureEx("N", font);
	if (charWidth <= 0)
		charWidth = 1;

	std::vector<RenderCell> row;
	engine->getRow(itemindex, row);

	int cursorCol = -1;
	if ((int)itemindex == engine->getCursorAbsRow() && engine->isCursorVisible())
		cursorCol = engine->getCursorCol();

	for (size_t col = 0; col < row.size(); ++col) {
		RenderCell& c = row[col];
		if (c.width == 0)
			continue; // right half of a wide cell, already drawn

		bool reverse = c.reverse;
		if ((int)col == cursorCol)
			reverse = !reverse; // draw the cursor as an inverted cell

		// resolve effective colours, substituting theme defaults
		COLOR fg = c.fgDefault ? mFontColor : COLOR(c.fr, c.fg, c.fb, 255);
		COLOR bg = c.bgDefault ? mBackgroundColor : COLOR(c.br, c.bg, c.bb, 255);
		bool bgIsDefault = c.bgDefault;
		if (reverse) {
			std::swap(fg, bg);
			bgIsDefault = false; // an inverted cell always needs its background painted
		}

		int cw = charWidth * (c.width ? c.width : 1);
		int x = mRenderX + (int)col * charWidth;

		// paint background unless it is just the page background showing through
		if (!bgIsDefault) {
			gr_color(bg.red, bg.green, bg.blue, bg.alpha);
			gr_fill(x, yPos, cw, actualItemHeight);
		}

		// paint glyph
		if (!c.text.empty() && c.text != " ") {
			gr_color(fg.red, fg.green, fg.blue, fg.alpha);
			gr_textEx_scaleW(x, yPos, c.text.c_str(), font, mRenderW, TOP_LEFT, 0);
		}
	}
}

void GUITerminal::NotifySelect(size_t item_selected __unused)
{
	// do nothing - terminal ignores selections
}

bool GUITerminal::status()
{
	return engine->status();
}

void GUITerminal::stop()
{
	engine->stop();
	engine->clear();
}

void GUITerminal::InitAndResize()
{
	// make sure the shell is started
	engine->initPty();
	// send window resize
	if (mFont && mFont->GetResource()) {
		int charWidth = twrpTruetype::gr_ttf_measureEx("N", mFont->GetResource());
		if (charWidth <= 0)
			charWidth = 1;
		engine->setSize(mRenderW / charWidth, GetDisplayItemCount(), mRenderW, mRenderH);
	}
}

void GUITerminal::SetPageFocus(int inFocus)
{
	if (inFocus && isConditionTrue()) {
		// TODO: grab focus correctly, this hack grabs focus and insists that the terminal be the focus regardless of other elements
		// It's highly unlikely that there will be any other visible input elements on the page anyway...
		SetInputFocus(1);
		InitAndResize();
	}
}
