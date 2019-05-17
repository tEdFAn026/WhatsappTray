﻿/*
*
* WhatsappTray
* Copyright (C) 1998-2017  Sebastian Amann, Nikolay Redko, J.D. Purcell
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
*/

#include "stdafx.h"
#include "WhatsappTray.h"
#include "SharedDefines.h"
#include "resource.h"

#include "AppData.h"
#include "Registry.h"
#include "WhatsAppApi.h"
#include "TrayManager.h"
#include "AboutDialog.h"
#include "Helper.h"
#include "Logger.h"

#include <Strsafe.h>
#include <psapi.h>
#include <filesystem>

namespace fs = std::experimental::filesystem;

#ifdef _DEBUG
constexpr auto CompileConfiguration = "Debug";
#else
constexpr auto CompileConfiguration = "Release";
#endif

#undef MODULE_NAME
#define MODULE_NAME "WhatsappTray"

const UINT WM_TASKBARCREATED = ::RegisterWindowMessage(TEXT("TaskbarCreated"));

static HINSTANCE _hInstance;
static HMODULE _hLib;
static HWND _hwndWhatsappTray;
static HWND _hwndForMenu;
static HWND _hwndWhatsapp;

static int messagesSinceMinimize = 0;

static std::unique_ptr<TrayManager> trayManager;

LRESULT CALLBACK WhatsAppTrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
HWND findWhatsapp();
HWND startWhatsapp();
bool createWindow();
bool setHook();
void setLaunchOnWindowsStartupSetting(bool value);

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nShowCmd)
{
	_hInstance = hInstance;

	Logger::Setup();

	Logger::Info(MODULE_NAME "::WinMain() - Starting WhatsappTray %s in %s CompileConfiguration.", Helper::GetProductAndVersion().c_str(), CompileConfiguration);

	WhatsAppApi::Init();

	Gdiplus::GdiplusStartupInput gdiplusStartupInput;
	ULONG_PTR gdiplusToken;

	// Initialize GDI+.
	Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

	// Setup the settings for launch on windows startup.
	setLaunchOnWindowsStartupSetting(AppData::LaunchOnWindowsStartup.Get());

	// Check if closeToTray was set per commandline. (this overrides the persistent storage-value.)
	if (strstr(lpCmdLine, "--closeToTray")) {
		AppData::CloseToTray.Set(true);
	}

	if (!(_hLib = LoadLibrary("Hook.dll"))) {
		Logger::Error(MODULE_NAME "::WinMain() - Error loading Hook.dll.");
		MessageBox(NULL, "Error loading Hook.dll.", "WhatsappTray", MB_OK | MB_ICONERROR);
		return 0;
	}

	if (startWhatsapp() == NULL) {
		return 0;
	}

	if (AppData::StartMinimized.Get()) {
		Logger::Info(MODULE_NAME "::WinMain() - Prepare for starting minimized.");

		WhatsAppApi::NotifyOnFullInit([]() {
			Logger::Info(MODULE_NAME "::WinMain() - NotifyOnFullInit");
			Sleep(2000);
			PostMessageA(_hwndWhatsappTray, WM_ADDTRAY, 0, 0);
			// Remove event after the first execution
			WhatsAppApi::NotifyOnFullInit(NULL);
		});
	}

	// Test if WhatsappTray is already running.
	// NOTE: This also matches the class-name of the window so we can be sure its our window and not for example an explorer-window with this name.
	_hwndWhatsappTray = FindWindow(NAME, NAME);
	if (_hwndWhatsappTray) {
		Logger::Error(MODULE_NAME "::WinMain() - Found an already open instance of WhatsappTray. Trying to close the other instance.");
		Logger::Error(MODULE_NAME "::WinMain() - If this error persists, try to close the other instance by hand using for example the taskmanager.");
		//if (strstr(lpCmdLine, "--exit")) {
		SendMessage(_hwndWhatsappTray, WM_CLOSE, 0, 0);
		//} else {
		//	//MessageBox(NULL, "WhatsappTray is already running. Reapplying hook", "WhatsappTray", MB_OK | MB_ICONINFORMATION);
		//	SendMessage(_hwndWhatsappTray, WM_REAPPLY_HOOK, 0, 0);
		//}
		//return 0;

#pragma WARNING("It would be best to wait her a bit and check if it is still active. And if it is still active shoot it down.")
	}

	if (setHook() == false) {
		return 0;
	}

	if (createWindow() == false) {
		return 0;
	}

	if (!_hwndWhatsappTray) {
		MessageBox(NULL, "Error creating window", "WhatsappTray", MB_OK | MB_ICONERROR);
		return 0;
	}
	trayManager = std::make_unique<TrayManager>(_hwndWhatsappTray, _hwndWhatsapp);

	// Send a WM_WHATSAPP_API_NEW_MESSAGE-message when a new WhatsApp-message has arrived.
	WhatsAppApi::NotifyOnNewMessage([]() { PostMessageA(_hwndWhatsappTray, WM_WHATSAPP_API_NEW_MESSAGE, 0, 0); });

	MSG msg;
	while (IsWindow(_hwndWhatsappTray) && GetMessage(&msg, _hwndWhatsappTray, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	Gdiplus::GdiplusShutdown(gdiplusToken);

	return 0;
}

/**
 * Start WhatsApp.
 */
HWND startWhatsapp()
{
	_hwndWhatsapp = findWhatsapp();

	fs::path waStartPath = std::string(AppData::WhatsappStartpath.Get());
	std::string waStartPathString;
	if (waStartPath.is_relative()) {
		fs::path appPath = Helper::GetApplicationFilePath();
		auto combinedPath = appPath / waStartPath;

		Logger::Info(MODULE_NAME "startWhatsapp() - Starting WhatsApp from combinedPath:%s", combinedPath.string().c_str());

		// Shorten the path by converting to absoltue path.
		auto combinedPathCanonical = fs::canonical(combinedPath);
		waStartPathString = combinedPathCanonical.string();
	} else {
		waStartPathString = waStartPath.string();
	}

	Logger::Info(MODULE_NAME "::startWhatsapp() - Starting WhatsApp from canonical-path:'" + waStartPathString + "'");
	HINSTANCE hInst = ShellExecuteA(0, NULL, waStartPathString.c_str(), NULL, NULL, SW_NORMAL);
	if (hInst <= (HINSTANCE)32) {
		MessageBoxA(NULL, (std::string("Error launching WhatsApp from path='") + waStartPathString + "'").c_str(), "WhatsappTray", MB_OK);
		return NULL;
	}

	// Wait for WhatsApp to be started.
	Sleep(100);
	for (int attemptN = 0; (_hwndWhatsapp = findWhatsapp()) == NULL; ++attemptN) {
		if (attemptN > 60) {
			MessageBoxA(NULL, "WhatsApp-Window not found.", "WhatsappTray", MB_OK | MB_ICONERROR);
			return NULL;
		}
		Sleep(500);
	}

	return _hwndWhatsapp;
}

/**
 * Search for the WhatsApp window.
 * Checks if it is the correct window:
 * 1. Get the path to the binary(exe) for the window with "WhatsApp" in the title
 * 2. Match it with the appData-setting.
 */
HWND findWhatsapp()
{
	HWND currentWindow = NULL;
	while (true) {
		currentWindow = FindWindowExA(NULL, currentWindow, NULL, WHATSAPP_CLIENT_NAME);
		if (currentWindow == NULL) {
			return NULL;
		}

		DWORD processId;
		DWORD threadId = GetWindowThreadProcessId(currentWindow, &processId);

		HANDLE processHandle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
		if (processHandle == NULL) {
			Logger::Error(MODULE_NAME "::startWhatsapp() - Failed to open process.");
			continue;
		}

		char filepath[MAX_PATH];
		if (GetModuleFileNameExA(processHandle, NULL, filepath, MAX_PATH) == 0) {
			CloseHandle(processHandle);
			Logger::Error(MODULE_NAME "::startWhatsapp() - Failed to get module filepath.");
			continue;
		}
		CloseHandle(processHandle);

		Logger::Info(MODULE_NAME "::startWhatsapp() - Filepath is: '%s'", filepath);

		std::string filenameFromWindow = Helper::GetFilenameFromPath(filepath);
		std::string filenameFromSettings = Helper::GetFilenameFromPath(AppData::WhatsappStartpath.Get());

		// NOTE: I do not compare the extension because when i start from an link, the name is WhatsApp.lnk whicht does not match the WhatsApp.exe
		// This could be improved by reading the real value from the .lnk but i think this should be fine for now.
		if (filenameFromWindow.compare(filenameFromSettings) != 0) {
			Logger::Error(MODULE_NAME "::startWhatsapp() - Filenames from window and from settings do not match.");
			continue;
		}

		Logger::Info(MODULE_NAME "::startWhatsapp() - Found match.");
		break;
	}

	return currentWindow;
}

/**
 * Create a window.
 * This window will be mainly used to receive messages.
 */
bool createWindow()
{
	WNDCLASS wc;
	wc.style = 0;
	wc.lpfnWndProc = WhatsAppTrayWndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = _hInstance;
	wc.hIcon = NULL;
	wc.hCursor = NULL;
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszMenuName = NULL;
	wc.lpszClassName = NAME;

	if (!RegisterClass(&wc)) {
		MessageBox(NULL, "Error creating window class", "WhatsappTray", MB_OK | MB_ICONERROR);
		return false;
	}

	_hwndWhatsappTray = CreateWindow(NAME, NAME, WS_OVERLAPPED, 0, 0, 0, 0, (HWND)NULL, (HMENU)NULL, _hInstance, (LPVOID)NULL);
	return true;
}

/*
 * Create the rightclick-menue.
 */
void ExecuteMenu()
{
	HMENU hMenu = CreatePopupMenu();
	if (!hMenu) {
		Logger::Error(MODULE_NAME "::ExecuteMenu() - Error creating menu.");
		MessageBox(NULL, "Error creating menu.", "WhatsappTray", MB_OK | MB_ICONERROR);
		return;
	}

	AppendMenu(hMenu, MF_STRING, IDM_ABOUT, "About WhatsappTray");
	// - Display options.

	// -- Close to Tray
	if (AppData::CloseToTray.Get()) {
		AppendMenu(hMenu, MF_CHECKED, IDM_SETTING_CLOSE_TO_TRAY, "Close to tray");
	} else {
		AppendMenu(hMenu, MF_UNCHECKED, IDM_SETTING_CLOSE_TO_TRAY, "Close to tray");
	}

	// -- Launch on Windows startup.
	if (AppData::LaunchOnWindowsStartup.Get()) {
		AppendMenu(hMenu, MF_CHECKED, IDM_SETTING_LAUNCH_ON_WINDOWS_STARTUP, "Launch on Windows startup");
	} else {
		AppendMenu(hMenu, MF_UNCHECKED, IDM_SETTING_LAUNCH_ON_WINDOWS_STARTUP, "Launch on Windows startup");
	}

	// -- Start minimized.
	if (AppData::StartMinimized.Get()) {
		AppendMenu(hMenu, MF_CHECKED, IDM_SETTING_START_MINIMIZED, "Start minimized");
	} else {
		AppendMenu(hMenu, MF_UNCHECKED, IDM_SETTING_START_MINIMIZED, "Start minimized");
	}

	AppendMenu(hMenu, MF_SEPARATOR, 0, NULL); //--------------

	AppendMenu(hMenu, MF_STRING, IDM_RESTORE, "Restore Window");
	AppendMenu(hMenu, MF_STRING, IDM_CLOSE, "Close Whatsapp");

	POINT point;
	GetCursorPos(&point);
	SetForegroundWindow(_hwndWhatsappTray);

	TrackPopupMenu(hMenu, TPM_LEFTBUTTON | TPM_RIGHTBUTTON | TPM_RIGHTALIGN | TPM_BOTTOMALIGN, point.x, point.y, 0, _hwndWhatsappTray, NULL);

	PostMessage(_hwndWhatsappTray, WM_USER, 0, 0);
	DestroyMenu(hMenu);
}

LRESULT CALLBACK WhatsAppTrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	Logger::Info(MODULE_NAME "::WhatsAppTrayWndProc() - Message Received msg='0x%X'", msg);

	switch (msg) {
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDM_ABOUT:
			AboutDialog::Create(_hInstance, _hwndWhatsappTray);
			break;
		case IDM_SETTING_CLOSE_TO_TRAY:
		{
			// Toggle the 'close to tray'-feature.
			// We have to first change the value and then unregister and register to set the ne value in the hook.
			AppData::CloseToTray.Set(!AppData::CloseToTray.Get());

			SendMessage(_hwndWhatsappTray, WM_REAPPLY_HOOK, 0, 0);
			break;
		}
		case IDM_SETTING_LAUNCH_ON_WINDOWS_STARTUP:
			// Toggle the 'launch on windows startup'-feature.
			setLaunchOnWindowsStartupSetting(!AppData::LaunchOnWindowsStartup.Get());
			break;
		case IDM_SETTING_START_MINIMIZED:
			// Toggle the 'start minimized'-feature.
			AppData::StartMinimized.Set(!AppData::StartMinimized.Get());
			break;
		case IDM_RESTORE:
			Logger::Info(MODULE_NAME "::WhatsAppTrayWndProc() - IDM_RESTORE");
			trayManager->RestoreWindowFromTray(_hwndForMenu);
			break;
		case IDM_CLOSE:
			trayManager->CloseWindowFromTray(_hwndForMenu);

			// Running WhatsappTray without Whatsapp makes no sence because if a new instance of Whatsapp is started, WhatsappTray would not hook it. Atleast not in the current implementation...
			DestroyWindow(_hwndWhatsappTray);
			break;
		}
		break;
	case WM_REAPPLY_HOOK:
		UnRegisterHook();
		_hwndWhatsapp = findWhatsapp();
		setHook();
		break;
	case WM_ADDTRAY:
		Logger::Info(MODULE_NAME "::WhatsAppTrayWndProc() - WM_ADDTRAY");
		messagesSinceMinimize = 0;
		trayManager->MinimizeWindowToTray(_hwndWhatsapp);
		break;
	case WM_TRAYCMD:
#pragma WARNING(Move into TrayManager. Problem is executeMenue...)
		switch (static_cast<UINT>(lParam)) {
		case NIN_SELECT:
			trayManager->RestoreFromTray(wParam);
			break;
		case WM_CONTEXTMENU:
			_hwndForMenu = trayManager->GetHwndFromIndex(wParam);
			ExecuteMenu();
			break;
		case WM_MOUSEMOVE:
			//HWND handleWindow = trayManager->GetHwndFromIndex(wParam);
			//trayManager->RefreshWindowInTray(handleWindow);
			break;
		}
		break;

	case WM_WHAHTSAPP_CLOSING:
		// If Whatsapp is closing we want to close WhatsappTray as well.
		Logger::Info(MODULE_NAME "::WhatsAppTrayWndProc() - WM_WHAHTSAPP_CLOSING");
		DestroyWindow(_hwndWhatsappTray);
		break;
	case WM_DESTROY:
		Logger::Info(MODULE_NAME "::WhatsAppTrayWndProc() - WM_DESTROY");

		trayManager->RestoreAllWindowsFromTray();

		UnRegisterHook();
		FreeLibrary(_hLib);
		PostQuitMessage(0);
		Logger::Info(MODULE_NAME "::WhatsAppTrayWndProc() - QuitMessage posted.");
		break;
	case WM_WHATSAPP_API_NEW_MESSAGE:
		{
			Logger::Info(MODULE_NAME "::WhatsAppTrayWndProc() - WM_WHATSAPP_API_NEW_MESSAGE");
			messagesSinceMinimize++;
			char messagesSinceMinimizeBuffer[20] = { 0 };
			snprintf(messagesSinceMinimizeBuffer, sizeof(messagesSinceMinimizeBuffer), "%d", messagesSinceMinimize);
			trayManager->SetIcon(_hwndWhatsapp, messagesSinceMinimizeBuffer);
		}
		break;
	default:
		if (msg == WM_TASKBARCREATED)
			trayManager->AddWindowToTray(_hwndWhatsapp);
		break;
	}

	return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool setHook()
{
	// Damit nicht alle Prozesse gehookt werde, verwende ich jetzt die ThreadID des WhatsApp-Clients.
	DWORD processId;
	DWORD threadId = GetWindowThreadProcessId(_hwndWhatsapp, &processId);
	if (threadId == NULL) {
		MessageBox(NULL, "ThreadID of WhatsApp-Window not found.", "WhatsappTray", MB_OK | MB_ICONERROR);
		return false;
	}

	if (RegisterHook(_hLib, threadId, AppData::CloseToTray.Get()) == false) {
		MessageBox(NULL, "Error setting hook procedure.", "WhatsappTray", MB_OK | MB_ICONERROR);
		return false;
	}
	return true;
}

/*
 * @brief Sets the 'launch on windows startup'-setting.
 */
void setLaunchOnWindowsStartupSetting(bool value)
{
	AppData::LaunchOnWindowsStartup.Set(value);

	if (value) {
		Registry::RegisterProgram();
	} else {
		Registry::UnregisterProgram();
	}
}
