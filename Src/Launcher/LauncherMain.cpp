#include "Projects/ProjectService.h"

#include <windows.h>
#include <commdlg.h>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	constexpr int kWindowWidth = 760;
	constexpr int kWindowHeight = 440;
	constexpr int kMaxRecentProjects = 16;

	enum ControlId : int
	{
		ProjectNameEdit = 1001,
		LocationEdit = 1002,
		BrowseLocationButton = 1003,
		CreateProjectButton = 1004,
		OpenProjectButton = 1005,
		RecentProjectsList = 1006,
		LaunchProjectButton = 1007,
		StatusText = 1008
	};

	struct LauncherState
	{
		HWND ProjectNameEdit = nullptr;
		HWND LocationEdit = nullptr;
		HWND RecentProjectsList = nullptr;
		HWND LaunchProjectButton = nullptr;
		HWND StatusText = nullptr;
		std::vector<std::filesystem::path> RecentProjects;
	};

	[[nodiscard]] std::wstring Utf8ToWide(std::string_view text)
	{
		if (text.empty())
		{
			return {};
		}

		const int requiredSize = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
		std::wstring result(static_cast<size_t>(requiredSize), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), requiredSize);
		return result;
	}

	[[nodiscard]] std::string WideToUtf8(std::wstring_view text)
	{
		if (text.empty())
		{
			return {};
		}

		const int requiredSize = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		std::string result(static_cast<size_t>(requiredSize), '\0');
		WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), requiredSize, nullptr, nullptr);
		return result;
	}

	[[nodiscard]] std::wstring GetWindowTextString(HWND control)
	{
		const int length = GetWindowTextLengthW(control);
		std::wstring text(static_cast<size_t>(length) + 1, L'\0');
		GetWindowTextW(control, text.data(), length + 1);
		text.resize(static_cast<size_t>(length));
		return text;
	}

	void SetStatus(LauncherState& state, std::wstring_view status)
	{
		SetWindowTextW(state.StatusText, std::wstring(status).c_str());
	}

	[[nodiscard]] std::filesystem::path GetExecutablePath()
	{
		std::wstring buffer(MAX_PATH, L'\0');
		DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		while (length == buffer.size())
		{
			buffer.resize(buffer.size() * 2);
			length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		}
		buffer.resize(length);
		return buffer;
	}

	[[nodiscard]] std::filesystem::path FindEngineWorkingDirectory(const std::filesystem::path& engineExecutable)
	{
		std::filesystem::path candidate = engineExecutable.parent_path();
		for (int depth = 0; depth < 5 && !candidate.empty(); ++depth)
		{
			if (std::filesystem::exists(candidate / "DX12Eninge.vcxproj") ||
				std::filesystem::exists(candidate / "Src" / "Core" / "Engine" / "Engine.cpp"))
			{
				return candidate;
			}
			candidate = candidate.parent_path();
		}
		return engineExecutable.parent_path();
	}

	[[nodiscard]] std::filesystem::path AskForEngineExecutable(HWND owner)
	{
		std::wstring fileName(MAX_PATH, L'\0');
		OPENFILENAMEW openFileName = {};
		openFileName.lStructSize = sizeof(openFileName);
		openFileName.hwndOwner = owner;
		openFileName.lpstrFilter = L"EnginePlatformer.exe\0EnginePlatformer.exe\0Executable\0*.exe\0";
		openFileName.lpstrFile = fileName.data();
		openFileName.nMaxFile = static_cast<DWORD>(fileName.size());
		openFileName.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
		openFileName.lpstrTitle = L"Select EnginePlatformer.exe";
		if (!GetOpenFileNameW(&openFileName))
		{
			return {};
		}
		return std::filesystem::path(fileName.c_str());
	}

	[[nodiscard]] std::filesystem::path ResolveEngineExecutable(HWND owner)
	{
		const std::filesystem::path launcherPath = GetExecutablePath();
		const std::filesystem::path sameFolderEngine = launcherPath.parent_path() / "EnginePlatformer.exe";
		if (std::filesystem::exists(sameFolderEngine))
		{
			return sameFolderEngine;
		}
		return AskForEngineExecutable(owner);
	}

	[[nodiscard]] std::filesystem::path GetRecentProjectsFilePath()
	{
		size_t requiredSize = 0;
		_wgetenv_s(&requiredSize, nullptr, 0, L"LOCALAPPDATA");
		std::wstring localAppData;
		if (requiredSize > 0)
		{
			localAppData.resize(requiredSize - 1);
			_wgetenv_s(&requiredSize, localAppData.data(), requiredSize, L"LOCALAPPDATA");
		}

		std::filesystem::path root = localAppData.empty()
			? std::filesystem::current_path()
			: std::filesystem::path(localAppData);
		return root / "EnginePlatformer" / "Launcher" / "recent-projects.json";
	}

	void SaveRecentProjects(const LauncherState& state)
	{
		const std::filesystem::path recentPath = GetRecentProjectsFilePath();
		std::filesystem::create_directories(recentPath.parent_path());

		rapidjson::StringBuffer buffer;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
		writer.StartObject();
		writer.Key("projects");
		writer.StartArray();
		for (const std::filesystem::path& projectPath : state.RecentProjects)
		{
			const std::string pathText = WideToUtf8(projectPath.wstring());
			writer.String(pathText.c_str());
		}
		writer.EndArray();
		writer.EndObject();

		std::ofstream stream(recentPath, std::ios::binary | std::ios::trunc);
		stream.write(buffer.GetString(), static_cast<std::streamsize>(buffer.GetSize()));
	}

	void LoadRecentProjects(LauncherState& state)
	{
		const std::filesystem::path recentPath = GetRecentProjectsFilePath();
		std::ifstream stream(recentPath, std::ios::binary);
		if (!stream)
		{
			return;
		}

		const std::string text{ std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
		rapidjson::Document document;
		document.Parse(text.c_str(), text.size());
		if (document.HasParseError() || !document.IsObject() || !document.HasMember("projects") || !document["projects"].IsArray())
		{
			return;
		}

		for (const auto& value : document["projects"].GetArray())
		{
			if (value.IsString())
			{
				state.RecentProjects.emplace_back(Utf8ToWide(value.GetString()));
			}
		}
	}

	void RefreshRecentList(LauncherState& state)
	{
		SendMessageW(state.RecentProjectsList, LB_RESETCONTENT, 0, 0);
		for (const std::filesystem::path& projectPath : state.RecentProjects)
		{
			const std::wstring label = projectPath.wstring();
			SendMessageW(state.RecentProjectsList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
		}
		EnableWindow(state.LaunchProjectButton, !state.RecentProjects.empty());
	}

	void AddRecentProject(LauncherState& state, const std::filesystem::path& projectPath)
	{
		const std::filesystem::path normalized = std::filesystem::absolute(projectPath).lexically_normal();
		std::erase_if(state.RecentProjects, [&normalized](const std::filesystem::path& existing)
			{
				return existing.lexically_normal() == normalized;
			});
		state.RecentProjects.insert(state.RecentProjects.begin(), normalized);
		if (state.RecentProjects.size() > kMaxRecentProjects)
		{
			state.RecentProjects.resize(kMaxRecentProjects);
		}
		SaveRecentProjects(state);
		RefreshRecentList(state);
	}

	[[nodiscard]] bool LaunchProject(HWND owner, LauncherState& state, const std::filesystem::path& projectPath)
	{
		Projects::ProjectResult validation = Projects::ProjectService::LoadProject(projectPath);
		if (!validation.Success)
		{
			MessageBoxW(owner, Utf8ToWide(validation.ErrorMessage).c_str(), L"Project Error", MB_OK | MB_ICONERROR);
			return false;
		}

		const std::filesystem::path engineExecutable = ResolveEngineExecutable(owner);
		if (engineExecutable.empty())
		{
			SetStatus(state, L"Engine executable was not selected.");
			return false;
		}

		std::wstring commandLine = L"\"";
		commandLine.append(engineExecutable.wstring());
		commandLine.append(L"\" --project \"");
		commandLine.append(validation.Descriptor.ProjectFilePath.wstring());
		commandLine.append(L"\"");

		STARTUPINFOW startupInfo = {};
		startupInfo.cb = sizeof(startupInfo);
		PROCESS_INFORMATION processInformation = {};
		const std::filesystem::path workingDirectory = FindEngineWorkingDirectory(engineExecutable);
		std::wstring mutableCommandLine = commandLine;
		const BOOL created = CreateProcessW(
			nullptr,
			mutableCommandLine.data(),
			nullptr,
			nullptr,
			FALSE,
			0,
			nullptr,
			workingDirectory.c_str(),
			&startupInfo,
			&processInformation);
		if (!created)
		{
			MessageBoxW(owner, L"EnginePlatformer.exe could not be launched.", L"Launch Error", MB_OK | MB_ICONERROR);
			return false;
		}

		CloseHandle(processInformation.hThread);
		CloseHandle(processInformation.hProcess);
		AddRecentProject(state, validation.Descriptor.ProjectFilePath);
		SetStatus(state, L"Project launched.");
		return true;
	}

	[[nodiscard]] std::filesystem::path SelectFolder(HWND owner)
	{
		BROWSEINFOW browseInfo = {};
		browseInfo.hwndOwner = owner;
		browseInfo.lpszTitle = L"Select parent folder for the new project";
		browseInfo.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
		PIDLIST_ABSOLUTE itemIdList = SHBrowseForFolderW(&browseInfo);
		if (!itemIdList)
		{
			return {};
		}

		std::wstring path(MAX_PATH, L'\0');
		SHGetPathFromIDListW(itemIdList, path.data());
		CoTaskMemFree(itemIdList);
		return std::filesystem::path(path.c_str());
	}

	[[nodiscard]] std::filesystem::path SelectProjectFile(HWND owner)
	{
		std::wstring fileName(MAX_PATH, L'\0');
		OPENFILENAMEW openFileName = {};
		openFileName.lStructSize = sizeof(openFileName);
		openFileName.hwndOwner = owner;
		openFileName.lpstrFilter = L"Engine Project\0*.engineproject\0";
		openFileName.lpstrFile = fileName.data();
		openFileName.nMaxFile = static_cast<DWORD>(fileName.size());
		openFileName.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
		openFileName.lpstrTitle = L"Open Engine Project";
		if (!GetOpenFileNameW(&openFileName))
		{
			return {};
		}
		return std::filesystem::path(fileName.c_str());
	}

	void CreateControls(HWND window, LauncherState& state)
	{
		CreateWindowW(L"STATIC", L"Project Name", WS_CHILD | WS_VISIBLE, 24, 24, 120, 22, window, nullptr, nullptr, nullptr);
		state.ProjectNameEdit = CreateWindowW(L"EDIT", L"MyGame", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 150, 22, 360, 24, window, reinterpret_cast<HMENU>(ProjectNameEdit), nullptr, nullptr);

		CreateWindowW(L"STATIC", L"Location", WS_CHILD | WS_VISIBLE, 24, 62, 120, 22, window, nullptr, nullptr, nullptr);
		state.LocationEdit = CreateWindowW(L"EDIT", std::filesystem::current_path().wstring().c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 150, 60, 460, 24, window, reinterpret_cast<HMENU>(LocationEdit), nullptr, nullptr);
		CreateWindowW(L"BUTTON", L"Browse", WS_CHILD | WS_VISIBLE, 620, 59, 100, 26, window, reinterpret_cast<HMENU>(BrowseLocationButton), nullptr, nullptr);

		CreateWindowW(L"BUTTON", L"Create Project", WS_CHILD | WS_VISIBLE, 150, 100, 140, 30, window, reinterpret_cast<HMENU>(CreateProjectButton), nullptr, nullptr);
		CreateWindowW(L"BUTTON", L"Open Project", WS_CHILD | WS_VISIBLE, 300, 100, 140, 30, window, reinterpret_cast<HMENU>(OpenProjectButton), nullptr, nullptr);

		CreateWindowW(L"STATIC", L"Recent Projects", WS_CHILD | WS_VISIBLE, 24, 152, 160, 22, window, nullptr, nullptr, nullptr);
		state.RecentProjectsList = CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL, 24, 178, 696, 160, window, reinterpret_cast<HMENU>(RecentProjectsList), nullptr, nullptr);
		state.LaunchProjectButton = CreateWindowW(L"BUTTON", L"Launch Selected", WS_CHILD | WS_VISIBLE, 24, 350, 150, 30, window, reinterpret_cast<HMENU>(LaunchProjectButton), nullptr, nullptr);
		state.StatusText = CreateWindowW(L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE, 190, 356, 520, 24, window, reinterpret_cast<HMENU>(StatusText), nullptr, nullptr);
	}

	[[nodiscard]] std::filesystem::path GetSelectedRecentProject(const LauncherState& state)
	{
		const LRESULT selectedIndex = SendMessageW(state.RecentProjectsList, LB_GETCURSEL, 0, 0);
		if (selectedIndex == LB_ERR || static_cast<size_t>(selectedIndex) >= state.RecentProjects.size())
		{
			return {};
		}
		return state.RecentProjects[static_cast<size_t>(selectedIndex)];
	}

	void CreateProject(HWND window, LauncherState& state)
	{
		const std::wstring projectName = GetWindowTextString(state.ProjectNameEdit);
		const std::wstring location = GetWindowTextString(state.LocationEdit);

		Projects::ProjectCreateRequest request;
		request.Name = WideToUtf8(projectName);
		request.ParentDirectory = location;
		Projects::ProjectResult result = Projects::ProjectService::CreateProject(request);
		if (!result.Success)
		{
			MessageBoxW(window, Utf8ToWide(result.ErrorMessage).c_str(), L"Create Project Error", MB_OK | MB_ICONERROR);
			return;
		}

		AddRecentProject(state, result.Descriptor.ProjectFilePath);
		static_cast<void>(LaunchProject(window, state, result.Descriptor.ProjectFilePath));
	}

	LRESULT CALLBACK LauncherWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
	{
		auto* state = reinterpret_cast<LauncherState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
		switch (message)
		{
		case WM_CREATE:
		{
			auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
			state = reinterpret_cast<LauncherState*>(createStruct->lpCreateParams);
			SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
			CreateControls(window, *state);
			LoadRecentProjects(*state);
			RefreshRecentList(*state);
			return 0;
		}
		case WM_COMMAND:
		{
			const int controlId = LOWORD(wParam);
			const int notification = HIWORD(wParam);
			if (!state)
			{
				break;
			}

			if (controlId == BrowseLocationButton)
			{
				const std::filesystem::path folder = SelectFolder(window);
				if (!folder.empty())
				{
					SetWindowTextW(state->LocationEdit, folder.wstring().c_str());
				}
				return 0;
			}
			if (controlId == CreateProjectButton)
			{
				CreateProject(window, *state);
				return 0;
			}
			if (controlId == OpenProjectButton)
			{
				const std::filesystem::path projectFile = SelectProjectFile(window);
				if (!projectFile.empty())
				{
					static_cast<void>(LaunchProject(window, *state, projectFile));
				}
				return 0;
			}
			if (controlId == LaunchProjectButton || (controlId == RecentProjectsList && notification == LBN_DBLCLK))
			{
				const std::filesystem::path projectFile = GetSelectedRecentProject(*state);
				if (!projectFile.empty())
				{
					static_cast<void>(LaunchProject(window, *state, projectFile));
				}
				return 0;
			}
			break;
		}
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		default:
			break;
		}

		return DefWindowProcW(window, message, wParam, lParam);
	}
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand)
{
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	const WNDCLASSEXW windowClass = {
		.cbSize = sizeof(WNDCLASSEXW),
		.style = CS_HREDRAW | CS_VREDRAW,
		.lpfnWndProc = LauncherWindowProc,
		.hInstance = instance,
		.hIcon = LoadIconW(nullptr, IDI_APPLICATION),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1),
		.lpszClassName = L"EnginePlatformerLauncherWindow",
		.hIconSm = LoadIconW(nullptr, IDI_APPLICATION)
	};

	if (!RegisterClassExW(&windowClass))
	{
		MessageBoxW(nullptr, L"Launcher window class registration failed.", L"Launcher Error", MB_OK | MB_ICONERROR);
		return 0;
	}

	LauncherState state;
	HWND window = CreateWindowW(
		windowClass.lpszClassName,
		L"EnginePlatformer Launcher",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		kWindowWidth,
		kWindowHeight,
		nullptr,
		nullptr,
		instance,
		&state);
	if (!window)
	{
		MessageBoxW(nullptr, L"Launcher window creation failed.", L"Launcher Error", MB_OK | MB_ICONERROR);
		return 0;
	}

	ShowWindow(window, showCommand);
	UpdateWindow(window);

	MSG message = {};
	while (GetMessageW(&message, nullptr, 0, 0) > 0)
	{
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}

	CoUninitialize();
	return static_cast<int>(message.wParam);
}
