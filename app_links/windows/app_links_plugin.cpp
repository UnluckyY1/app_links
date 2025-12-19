// app_links_plugin_fixed_full.cpp
#include "app_links_plugin.h"
#include <iostream>
#include <windows.h>
#include <regex>
#include <algorithm>
#include <vector>
#include <string>
#include "include/app_links/app_links_plugin_c_api.h"

using namespace flutter;

namespace applinks
{

	// -------------------- Helpers --------------------

	// Convert wide string to UTF-8 std::string 
	static std::string WideToUtf8(const std::wstring &wstr)
	{
		if (wstr.empty())
		{
			return std::string();
		}
		int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
		if (size_needed <= 0)
			return std::string();
		std::string result(size_needed - 1, '\0'); // exclude terminating null
		WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], size_needed, NULL, NULL);
		return result;
	}

	// Trim surrounding quotes (single or double)
	static std::wstring TrimQuotes(const std::wstring &s)
	{
		if (s.size() >= 2)
		{
			if ((s.front() == L'"' && s.back() == L'"') || (s.front() == L'\'' && s.back() == L'\''))
			{
				return s.substr(1, s.size() - 2);
			}
		}
		return s;
	}

	// Detect whether input looks like a URI with scheme://
	static bool IsUri(const std::wstring &input)
	{
		static const std::wregex schemeRegex(LR"(^[A-Za-z][A-Za-z0-9+\-.]*://)");
		return std::regex_search(input, schemeRegex);
	}

	// Try to get a wide string from raw COPYDATASTRUCT safely.
	// Detect whether data is UTF-16 (wchar_t) or UTF-8/ANSI and convert accordingly.
	static std::wstring CopyDataToWString(const COPYDATASTRUCT *cds)
	{
		if (cds == nullptr || cds->lpData == nullptr || cds->cbData == 0)
			return std::wstring();

		const unsigned char *bytes = reinterpret_cast<const unsigned char *>(cds->lpData);

		// Heuristic: if there is a BOM or many even-position zeros treat as UTF-16
		bool looksLikeUtf16 = false;
		if (cds->cbData >= 2 && ((bytes[0] == 0xFF && bytes[1] == 0xFE) || (bytes[0] == 0xFE && bytes[1] == 0xFF)))
		{
			looksLikeUtf16 = true;
		}
		else if (cds->cbData % 2 == 0)
		{
			int zeros = 0;
			int checks = (std::min)(10, int(cds->cbData / 2));
			for (int i = 0; i < checks; ++i)
			{
				if (bytes[i * 2 + 1] == 0)
					++zeros;
			}
			if (zeros >= checks / 2)
				looksLikeUtf16 = true;
		}

		if (looksLikeUtf16)
		{
			const wchar_t *wdata = reinterpret_cast<const wchar_t *>(cds->lpData);
			size_t wlen = cds->cbData / sizeof(wchar_t);
			if (wlen > 0 && wdata[wlen - 1] == L'\0')
				--wlen;
			return std::wstring(wdata, wlen);
		}

		// Otherwise treat as UTF-8 (or ANSI interpreted as UTF-8). Convert to wide.
		{
			const char *data = reinterpret_cast<const char *>(cds->lpData);
			int cb = static_cast<int>(cds->cbData);
			std::string tmp(data, data + cb);
			if (!tmp.empty() && tmp.back() == '\0')
				tmp.pop_back();
			int needed = MultiByteToWideChar(CP_UTF8, 0, tmp.c_str(), -1, NULL, 0);
			if (needed <= 0)
				return std::wstring();
			std::vector<wchar_t> wbuf(needed);
			MultiByteToWideChar(CP_UTF8, 0, tmp.c_str(), -1, wbuf.data(), needed);
			return std::wstring(wbuf.data(), wbuf.data() + needed - 1);
		}
	}

	// Normalize inputs: if URI -> return UTF-8 as-is, otherwise normalize file paths to Windows absolute path.
	static std::string NormalizeInput(const std::wstring &rawInput)
	{
		if (rawInput.empty())
			return std::string();

		std::wstring trimmed = TrimQuotes(rawInput);

		// If it's a URI with scheme://, return as-is (UTF-8)
		if (IsUri(trimmed))
		{
			return WideToUtf8(trimmed);
		}

		// At this point, we handle file-path-like inputs
		std::wstring path = trimmed;

		// Handle file:///C:/... URIs specifically (these are valid file URIs)
		const std::wstring filePrefixSlash = L"file:///";
		const std::wstring filePrefix = L"file://";
		if (path.rfind(filePrefixSlash, 0) == 0)
		{
			path = path.substr(filePrefixSlash.size());
		}
		else if (path.rfind(filePrefix, 0) == 0)
		{
			// file://server/share -> UNC \\server\share\...
			std::wstring rest = path.substr(filePrefix.size());
			while (!rest.empty() && (rest.front() == L'/' || rest.front() == L'\\'))
				rest.erase(rest.begin());
			std::replace(rest.begin(), rest.end(), L'/', L'\\');
			path = L"\\";
			path += L"\\";
			path += rest; // prepend UNC slashes
		}

		// POSIX-style leading '/C:/...' -> 'C:\...'
		if (path.size() >= 3 && path.front() == L'/' && iswalpha(path[1]) && path[2] == L':')

		{
			path = path.substr(1);
		}

		// Replace forward slashes with backslashes
		std::replace(path.begin(), path.end(), L'/', L'\\');

		// If path is relative (no drive letter and not UNC), make absolute relative to current working directory
		bool hasDrive = (path.size() >= 2 && path[1] == L':');
		bool isUNC = (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\');

		if (!hasDrive && !isUNC)
		{
			std::vector<wchar_t> cwdBuf(MAX_PATH);
			DWORD len = GetCurrentDirectoryW(static_cast<DWORD>(cwdBuf.size()), cwdBuf.data());
			if (len > 0 && len < cwdBuf.size())
			{
				std::wstring cwd(cwdBuf.data());
				if (!cwd.empty() && cwd.back() != L'\\')
					cwd.push_back(L'\\');
				path = cwd + path;
			}
			else
			{
				// fallback to C:\ to avoid returning truly relative paths
				path = L"C:\\" + path;
			}
		}

		// Final safety check
		if (!(path.size() >= 2 && path[1] == L':') && !(path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\'))
		{
			OutputDebugStringW(L"[AppLinks] WARNING: normalized path has no drive or UNC root.\n");
		}

		return WideToUtf8(path);
	}

	// -------------------- Plugin implementation --------------------

	void AppLinksPlugin::RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar)
	{
		auto plugin = std::make_unique<AppLinksPlugin>(registrar);

		auto methodChannel = std::make_unique<FlMethodChannel>(
			registrar->messenger(), "com.llfbandit.app_links/messages",
			&flutter::StandardMethodCodec::GetInstance());

		methodChannel->SetMethodCallHandler(
			[plugin_pointer = plugin.get()](const auto &call, auto result)
			{
				plugin_pointer->HandleMethodCall(call, std::move(result));
			});

		auto eventChannel = std::make_unique<FlEventChannel>(
			registrar->messenger(), "com.llfbandit.app_links/events",
			&flutter::StandardMethodCodec::GetInstance());

		auto eventHandler = std::make_unique<StreamHandlerFunctions<EncodableValue>>(
			[plugin_pointer = plugin.get()](const EncodableValue *arguments, std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> &&events) -> std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>>
			{
				return plugin_pointer->OnListen(arguments, std::move(events));
			},
			[plugin_pointer = plugin.get()](const EncodableValue *arguments) -> std::unique_ptr<FlStreamHandlerError>
			{
				return plugin_pointer->OnCancel(arguments);
			});

		eventChannel->SetStreamHandler(std::move(eventHandler));

		registrar->AddPlugin(std::move(plugin));
	}

	// static, Parse command line
	std::optional<std::string> AppLinksPlugin::GetLink()
	{
		int argc = 0;
		wchar_t **argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
		if (argv == nullptr)
		{
			OutputDebugStringW(L"[AppLinks] Failed to parse command-line arguments.\n");
			return std::nullopt;
		}

		std::wstring logMessage = L"[AppLinks] argc = " + std::to_wstring(argc) + L"\n";
		OutputDebugStringW(logMessage.c_str());

		if (argc < 2)
		{
			OutputDebugStringW(L"[AppLinks] No additional arguments received.\n");
			::LocalFree(argv);
			return std::nullopt;
		}

		std::wstring arg(argv[1]);
		::LocalFree(argv);

		// Normalize the input (URI -> left alone; file paths -> normalized)
		std::string link = NormalizeInput(arg);

		// Debug: show original and normalized in the debug output
		std::wstring debugMsg = L"[AppLinks] GetLink original: " + arg + L"\n";
		OutputDebugStringW(debugMsg.c_str());

		return link;
	}

	AppLinksPlugin::AppLinksPlugin(PluginRegistrarWindows *registrar)
		: registrar_(registrar)
	{

		window_proc_id_ = registrar->RegisterTopLevelWindowProcDelegate(
			[this](HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
			{
				return HandleWindowProc(hwnd, message, wparam, lparam);
			});
	}

	AppLinksPlugin::~AppLinksPlugin()
	{
		registrar_->UnregisterTopLevelWindowProcDelegate(window_proc_id_);
	}

	void AppLinksPlugin::HandleMethodCall(
		const flutter::MethodCall<flutter::EncodableValue> &method_call,
		std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result)
	{
		if (method_call.method_name().compare("getInitialLink") == 0)
		{
			auto link = GetLink();
			result->Success(flutter::EncodableValue(link.value_or("")));
		}
		else if (method_call.method_name().compare("getLatestLink") == 0)
		{
			result->Success(flutter::EncodableValue(latestLink_.value_or("")));
		}
		else
		{
			result->NotImplemented();
		}
	}

	std::optional<LRESULT> AppLinksPlugin::HandleWindowProc(
		HWND hwnd,
		UINT message,
		WPARAM wparam,
		LPARAM lparam)
	{

		if (message == WM_COPYDATA)
		{
			COPYDATASTRUCT *cds = (COPYDATASTRUCT *)lparam;

			if (cds && cds->dwData == APPLINK_MSG_ID)
			{
				std::wstring wide = CopyDataToWString(cds);
				if (!wide.empty())
				{
					std::string normalized = NormalizeInput(wide);

					latestLink_ = normalized;

					if (!initialLink_)
					{
						initialLink_ = normalized;
					}

					if (eventSink_)
					{
						initialLinkSent_ = true;
						eventSink_->Success(latestLink_.value());
					}
				}
				else
				{
					OutputDebugStringW(L"[AppLinks] WM_COPYDATA payload could not be interpreted.\n");
				}
			}
		}

		if (message == WM_COMMAND)
		{
			int wmId = LOWORD(wparam);
			if (wmId == IDM_GETARGSWAS)
			{
				SendAppLink(hwnd);
			}
		}

		return std::nullopt;
	}

	std::unique_ptr<FlStreamHandlerError> AppLinksPlugin::OnListen(
		const flutter::EncodableValue *arguments,
		std::unique_ptr<FlEventSink> &&events)
	{

		eventSink_ = std::move(events);

		auto link = GetLink();
		if (!initialLinkSent_ && link)
		{
			initialLinkSent_ = true;
			initialLink_ = link;
			eventSink_->Success(initialLink_.value());
		}

		return nullptr;
	}

	std::unique_ptr<FlStreamHandlerError> AppLinksPlugin::OnCancel(
		const flutter::EncodableValue *arguments)
	{
		eventSink_ = nullptr;
		return nullptr;
	}

} // namespace
