/*****************************************************************************
Copyright (C) 2016-2017 by Colin Edwards.
Additional Code Copyright (C) 2016-2017 by c3r1c3 <c3r1c3@nevermindonline.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#ifndef OBS_STUDIO_EDITORDIALOG_H
#define OBS_STUDIO_EDITORDIALOG_H

#include "aeffectx.h"

class VSTPlugin;

class VstRect {

public:
	short top;
	short left;
	short bottom;
	short right;
};

#if defined(_WIN32)

#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

// Qt-free editor host: the Braidcast CEF build ships no Qt6, so the VST's
// native editor is hosted directly in a raw Win32 top-level window that owns
// its own message loop on a dedicated thread. effEditOpen/effEditIdle/
// effEditClose all run on that thread; the plugin's editor GUI is created as a
// child of this window's HWND.
class EditorWidget {

	VSTPlugin *plugin;
	AEffect *effect = nullptr;

	std::atomic<HWND> windowHandle{nullptr};
	std::wstring title;
	DWORD windowStyle = 0;
	DWORD windowExStyle = 0;

	std::thread uiThread;
	std::mutex readyMutex;
	std::condition_variable readyCv;
	bool windowReady = false;
	std::atomic_bool finished{false};
	// Guards a single effEditClose; owned by the UI thread's WndProc.
	std::atomic_bool editOpened{false};

	void runWindow();
	void applyEditRect();
	static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

public:
	EditorWidget(void *parent, VSTPlugin *plugin);
	~EditorWidget();

	void buildEffectContainer(AEffect *effect);
	void setWindowTitle(const std::string &title);
	void show();
	void close();
	void handleResizeRequest(int width, int height);

	// True once the window has been closed (e.g. by the user) and its thread
	// has finished; lets VSTPlugin reclaim the object and allow reopening.
	bool isFinished() const { return finished.load(); }
};

#else // Qt editor host (mac / linux) — unchanged

#include <QWidget>

#if defined(__linux__)
#include <QWindow>
#include <xcb/xcb.h>
#endif

#include "VSTPlugin.h"

class EditorWidget : public QWidget {

	VSTPlugin *plugin;

#if defined(__APPLE__)
	QWidget *cocoaViewContainer = NULL;
#endif

public:
	EditorWidget(QWidget *parent, VSTPlugin *plugin);
	void buildEffectContainer(AEffect *effect);
	void closeEvent(QCloseEvent *event) override;
	void handleResizeRequest(int width, int height);
};

#endif

#endif // OBS_STUDIO_EDITORDIALOG_H
