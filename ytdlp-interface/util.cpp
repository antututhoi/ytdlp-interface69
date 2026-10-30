#include "util.hpp"
#include "log.hpp"
#include "bitextractor.hpp"
#include "bitexception.hpp"

#include <Netlistmgr.h>
#include <WinInet.h>
#include <TlHelp32.h>
#include <powrprof.h>

#include <iostream>
#include <codecvt>

#pragma warning (disable: 4244)
#pragma comment(lib, "winhttp.lib")

logger g_log;
bool g_exiting;

std::string util::format_int(std::uint64_t i)
{
	std::stringstream ss;
	ss.imbue(std::locale {ss.getloc(), new Sep<char>{}});
	ss << i;
	return ss.str();
}


std::string util::format_double(double f, unsigned precision)
{
	std::stringstream ss;
	ss << std::fixed << std::setprecision(precision) << f;
	return ss.str();
}


std::string util::int_to_filesize(std::uint64_t i, bool with_bytes)
{
	double f {static_cast<double>(i)};
	std::string s, bytes {" (" + format_int(i) + ")"};
	if(i < 1024)
		s = format_int(i);
	else if(i < 1024*1024)
		s = std::to_string(i/1024) + " KB" + (with_bytes ? bytes : "");
	else if(i < 1024*1024*1024)
		s = format_double(f/(1024*1024)) + " MB" + (with_bytes ? bytes : "");
	else s = format_double(f/(1024*1024*1024)) + " GB" + (with_bytes ? bytes : "");
	return s;
}


std::string util::get_last_error_str(bool inet)
{
	std::string str(4096, '\0');
	if(inet) str.resize(FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_FROM_HMODULE, 
									   GetModuleHandleA("Wininet.dll"), GetLastError(), 0, &str.front(), 4096, 0));
	else str.resize(FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, 0, GetLastError(), 0, &str.front(), 4096, 0));
	size_t pos {str.find_first_of("\r\n")};
	if(pos != -1)
		str.erase(pos);
	return str;
}


HWND util::hwnd_from_pid(DWORD pid)
{
	struct lparam_t { HWND hwnd {nullptr}; DWORD pid {0}; } lparam;

	WNDENUMPROC enumfn = [](HWND hwnd, LPARAM lparam) -> BOOL
	{
		auto &target {*reinterpret_cast<lparam_t*>(lparam)};
		DWORD pid {0};
		GetWindowThreadProcessId(hwnd, &pid);
		if(pid == target.pid)
		{
			target.hwnd = hwnd;
			return FALSE;
		}
		return TRUE;
	};

	lparam.pid = pid;
	EnumWindows(enumfn, reinterpret_cast<LPARAM>(&lparam));
	return lparam.hwnd;
}


std::vector<HWND> util::hwnds_from_pid(DWORD pid)
{
	struct lparam_t { std::vector<HWND> hwnds; DWORD pid {0}; } lparam;

	WNDENUMPROC enumfn = [](HWND hwnd, LPARAM lparam) -> BOOL
	{
		auto &target {*reinterpret_cast<lparam_t*>(lparam)};
		DWORD pid {0};
		GetWindowThreadProcessId(hwnd, &pid);
		if(pid == target.pid && IsWindowVisible(hwnd))
			target.hwnds.push_back(hwnd);
		return TRUE;
	};

	lparam.pid = pid;
	EnumWindows(enumfn, reinterpret_cast<LPARAM>(&lparam));
	return lparam.hwnds;
}


std::string util::run_piped_process
(
	std::wstring cmd, std::atomic_bool *working, append_callback cbappend, progress_callback cbprog, 
	std::atomic_bool *graceful_exit, std::string suppress
)
{
	const std::string strtid {"[tid " + (std::stringstream {} << std::hex << std::this_thread::get_id()).str() + "]"};
	std::wstring modpath(4096, '\0');
	modpath.resize(GetModuleFileNameW(0, &modpath.front(), modpath.size()));

	std::string ret;
	HANDLE hPipeRead, hPipeWrite;

	SECURITY_ATTRIBUTES sa {sizeof(SECURITY_ATTRIBUTES)};
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;

	if(!CreatePipe(&hPipeRead, &hPipeWrite, &sa, 0))
		return ret;

	STARTUPINFOW si {sizeof(STARTUPINFOW)};
	si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
	si.hStdOutput = hPipeWrite;
	si.hStdError = hPipeWrite;
	si.wShowWindow = SW_HIDE;

	PROCESS_INFORMATION pi {0};

	const std::wstring cmd_for_log {cmd};
	BOOL res {CreateProcessW(NULL, &cmd.front(), NULL, NULL, TRUE, CREATE_NEW_CONSOLE|CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi)};
	if(!res)
	{
		const auto err {GetLastError()};
		applog::write_cmd("process", L"FAILED to start (error " + std::to_wstring(err) + L"): ", cmd_for_log);
		CloseHandle(hPipeWrite);
		CloseHandle(hPipeRead);
		return ret;
	}
	const auto start_tick {GetTickCount64()};
	applog::write_cmd("process", L"start pid " + std::to_wstring(pi.dwProcessId) + L": ", cmd_for_log);
	int logged_errors {0};

	auto killproc = [&] (bool quick)
	{
		if(!g_exiting)
		{
			static std::mutex mtx;
			std::lock_guard<std::mutex> lock {mtx};
			if(AttachConsole(pi.dwProcessId))
			{
				auto res {GenerateConsoleCtrlEvent(CTRL_C_EVENT, pi.dwProcessId)};
				FreeConsole();
				if(res && WaitForSingleObject(pi.hProcess, 6000) == WAIT_OBJECT_0)
				{
					if(graceful_exit)
						*graceful_exit = true;
					return;
				}
			}
		}
		auto hwnd {hwnd_from_pid(pi.dwProcessId)};
		if(hwnd)
		{
			if(IsWindow(hwnd))
				SendMessage(hwnd, WM_CLOSE, 0, 0);
			else TerminateProcess(pi.hProcess, 0);
		}
		else TerminateProcess(pi.hProcess, 0);
		if(graceful_exit)
			*graceful_exit = false;
	};

	const bool quick {cmd.find(L" -j ") != -1 || cmd.find(L" -J ") != -1};
	int playlist_complete {1}, playlist_total {0};
	bool procexit {false}, subs {false};
	while(!procexit)
	{
		procexit = WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0;

		for(;;)
		{
			char buf[1024];
			DWORD dwRead = 0;
			DWORD dwAvail = 0;

			if(!::PeekNamedPipe(hPipeRead, NULL, 0, NULL, &dwAvail, NULL))
				break;

			if(!dwAvail)
				break;

			if(!::ReadFile(hPipeRead, buf, std::min(static_cast<DWORD>(sizeof(buf) - 1), dwAvail), &dwRead, NULL) || !dwRead)
				break;

			if(working && !(*working))
			{
				killproc(quick);
				break;
			}

			buf[dwRead] = 0;
			std::string s;
			if(cbprog)
			{
				if(std::string(buf).find("Writing video subtitles") != -1)
					subs = true;
				std::string line, strbuf {buf};
				if(!nana::is_utf8(strbuf))
				{
					std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> u16conv;
					auto u16str {u16conv.from_bytes(strbuf)};
					std::wstring wstr(u16str.size(), L'\0');
					memcpy(&wstr.front(), &u16str.front(), wstr.size() * 2);
					std::wstring_convert<std::codecvt_utf8<wchar_t>> u8conv;
					strbuf = u8conv.to_bytes(wstr);
				}
				size_t lnstart {0}, lnend {0};
				do
				{
					line.clear();
					if(lnend != -1)
					{
						lnend = strbuf.find_first_of("\r\n", lnstart);
						if(lnend != 1)
							line = strbuf.substr(lnstart, lnend - lnstart);
						else line = strbuf.substr(lnstart);
						if(line.size()) 
							line += '\n';
						if(lnend + 1 == strbuf.size())
							lnend = -1;
						else lnstart = lnend + 1;
					}
					if(logged_errors < 20 && line.starts_with("ERROR:"))
					{
						applog::write("yt-dlp", "pid " + std::to_string(pi.dwProcessId) + " " + line);
						logged_errors++;
					}
					if(!line.empty() && (suppress.empty() || line.find(suppress) == -1))
					{
						if(cbappend && *working && line[0] == '[')
						{
							auto pos {line.find(']')};
							if(pos != -1)
							{
								auto text {line.substr(0, pos + 1)};
								if(text != "[download]" && text[1] != '#')
								{
									if(text.size() > 3 && text[1] == 'D' && text[2] == 'L' && text[3] == ':')
									{
										cbprog(-1, -1, line, -1, -1);
										continue;
									}
									if(*working)
										cbappend(text, true);
								}
								if(text == "[Exec]" && text.find("ytdlp_status") != 1)
									continue;
								if(text == "[ExtractAudio]" || text.starts_with("[Fixup") || text == "[Merger]")
									cbprog(-1, -1, text, 0, 0);
								else if(!suppress.empty())
									if(text == "[download]" && (line.find("Destination:") == 11 || line.find("has already been downloaded") != -1))
										cbprog(-1, -1, line, 0, 0);
							}
						}
						const bool line_starts_with_download {line.starts_with("[download]")},
						           playlist_progress {line_starts_with_download && line.find(" Downloading item ") == 10};
						if(playlist_progress)
						{
							auto pos {line.find(" of ", 28)};
							if(pos != -1)
							{
								playlist_complete = std::stod(line.substr(28, pos - 28));
								playlist_total = std::stod(line.substr(pos + 4, line.size() - pos - 1));
							}
						}
						auto pos {line.find('%')};
						if(pos != -1 && line_starts_with_download)
						{
							if(subs)
							{
								s += line;
								if(line.find("100%") != -1)
									subs = false;
							}
							else
							{
								auto pos2 {line.rfind(' ', pos) + 1};
								if(pos2 != -1)
								{
									try {
										auto percent {std::stod(line.substr(pos2, pos - pos2))};
										line.pop_back();
										if(*working)
										{											
											auto pos {line.find("[download]", pos2)};
											std::string text {pos == -1 ? line.substr(pos2) : line.substr(pos2, pos-pos2)};
											cbprog(static_cast<ULONGLONG>(percent * 10), 1000, text, playlist_complete - 1, playlist_total);
										}
									} catch(...) {}
								}
							}
						}
						else if(pos != -1 && line.starts_with("[#") && line[line.size()-2] == ']')
						{
							auto paren {line.rfind('(', pos) + 1};
							auto strpct {line.substr(paren, pos - paren)};
							auto pos3 {line.find("ETA:")};
							std::string eta;
							if(pos3 != -1)
								eta = line.substr(pos3, line.size() - 2 - pos3);
							auto slash {line.find('/')}, first {line.find(' ')};
							std::string text {line.substr(first, slash-first) + " of "};
							text += line.substr(slash + 1, paren - slash - 2) + " ";
							text += line.substr(paren-1, line.rfind(']') - paren);
							if(*working)
								cbprog(static_cast<ULONGLONG>(std::stod(strpct) * 10), 1000, text, playlist_complete - 1, playlist_total);
						}
						else s += line;
					}
				} while(lnend != -1);
			}
			else ret += buf;
			if(cbappend && *working)
			{
				if(s.size() > 2)
				{
					auto pos {s.find_first_not_of(" \r\n")};
					if(pos == -1)
						s.clear();
					else s = s.substr(pos);
				}
				if(s.find("frag ") == -1 || s.starts_with("[download] Destination:") || s.starts_with("[fixup"))
					cbappend(s, false);
			}
		}
		if(working && !*working)
		{
			DWORD exit_code {0};
			GetExitCodeProcess(pi.hProcess, &exit_code);
			if(exit_code == STILL_ACTIVE)
			{
				killproc(quick);
			}
			break;
		}
	}

	if(!quick)
	{
		DWORD exit_code {0};
		GetExitCodeProcess(pi.hProcess, &exit_code);
		if(exit_code == 1 && cbprog)
			ret = "failed";
		if(exit_code == STILL_ACTIVE)
		{
			killproc(quick);
		}
	}

	{
		DWORD exit_code {0};
		GetExitCodeProcess(pi.hProcess, &exit_code);
		if(!cbprog)
		{
			// output was collected in ret: log its error lines too
			size_t pos {0};
			while(logged_errors < 20 && (pos = ret.find("ERROR:", pos)) != std::string::npos)
			{
				if(pos == 0 || ret[pos - 1] == '\n')
				{
					const auto end {ret.find('\n', pos)};
					applog::write("yt-dlp", "pid " + std::to_string(pi.dwProcessId) + " " + ret.substr(pos, std::min<size_t>(end == std::string::npos ? ret.size() - pos : end - pos, 600)));
					logged_errors++;
				}
				pos += 6;
			}
		}
		char buf[160];
		sprintf_s(buf, "end pid %lu: exit code %lu%s, %.1f s", pi.dwProcessId, exit_code,
			exit_code == STILL_ACTIVE ? " (still running/killed)" : (working && !*working ? " (stopped by user)" : ""),
			(GetTickCount64() - start_tick) / 1000.0);
		applog::write("process", buf);
	}

	CloseHandle(hPipeWrite);
	CloseHandle(hPipeRead);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return ret;
}


DWORD util::other_instance(std::wstring path)
{
	std::wstring modpath(4096, '\0');
	if(path.empty())
		modpath.resize(GetModuleFileNameW(0, &modpath.front(), modpath.size()));
	else modpath = path;
	auto img_name {fs::path {modpath}.filename().wstring()};

	HANDLE hSnapshot {CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
	if(hSnapshot)
	{
		PROCESSENTRY32 pe32;
		pe32.dwSize = sizeof(PROCESSENTRY32);
		if(Process32First(hSnapshot, &pe32))
		{
			DWORD curpid {GetCurrentProcessId()};
			do
			{
				if(img_name == pe32.szExeFile)
				{
					auto hproc {OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pe32.th32ProcessID)};
					if(hproc)
					{
						DWORD bufsize {8192};
						std::wstring buf(bufsize, '\0');
						if(QueryFullProcessImageName(hproc, 0, &buf.front(), &bufsize))
						{
							buf.resize(bufsize);
							if(modpath == buf && pe32.th32ProcessID != curpid)
							{
								CloseHandle(hproc);
								CloseHandle(hSnapshot);
								return pe32.th32ProcessID;
							}
						}
						CloseHandle(hproc);
					}
				}
			} while(Process32Next(hSnapshot, &pe32));
		}
		CloseHandle(hSnapshot);
	}
	return 0;
}


unsigned util::close_children(bool report_only)
{
	unsigned children {0};
	std::vector<std::thread> threads;
	HANDLE hSnapshot {CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
	if(hSnapshot)
	{
		PROCESSENTRY32 pe32;
		pe32.dwSize = sizeof(PROCESSENTRY32);
		if(Process32First(hSnapshot, &pe32))
		{
			DWORD curpid {GetCurrentProcessId()};
			do
			{
				if(pe32.th32ParentProcessID == curpid)
				{
					auto hwnd {hwnd_from_pid(pe32.th32ProcessID)};
					if(hwnd)
					{
						children++;
						if(!report_only)
							threads.emplace_back([hwnd] { SendMessageA(hwnd, WM_CLOSE, 0, 0); });
					}
				}
			} while(Process32Next(hSnapshot, &pe32));
		}
		CloseHandle(hSnapshot);
	}
	for(auto &thread : threads)
		thread.join();
	return children;
}


std::wstring util::get_sys_folder(REFKNOWNFOLDERID rfid)
{
	wchar_t *res {nullptr};
	std::wstring sres;
	if(SHGetKnownFolderPath(rfid, 0, 0, &res) == S_OK)
		sres = res;
	CoTaskMemFree(res);
	return sres;
}


std::string util::get_inet_res(std::string res, std::string *error, bool truncate)
{
	std::string ret;
	if(error) error->clear();

	/*auto get_inet_error = []
	{
		std::string str(4096, '\0');
		DWORD dwerr {0}, dwsize {4096};
		if(InternetGetLastResponseInfoA(&dwerr, &str.front(), &dwsize))
			str.resize(dwsize);
		else str.clear();
		return str;
	};*/

	const char *agent {res.find("github.com") == -1 ? "Mozilla/5.0 (Windows NT 10.0; Win64; x64)" : "ytdlp-interface"};
	auto hinet {InternetOpenA(agent, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0)};
	if(hinet)
	{
		auto hfile {InternetOpenUrlA(hinet, res.data(), NULL, 0, 0, 0)};
		if(hfile)
		{
			DWORD read {1};
			while(read)
			{
				std::string buf(4096, '\0');
				if(InternetReadFile(hfile, &buf.front(), buf.size(), &read))
				{
					if(read < buf.size())
						buf.resize(read);
					ret += buf;
					if(truncate) break;
				}
				else
				{
					if(error) *error = get_last_error_str(true);
					break;
				}
			}
			InternetCloseHandle(hfile);
		}
		else if(error) *error = get_last_error_str(true);
		InternetCloseHandle(hinet);
	}
	else if(error) *error = get_last_error_str(true);
	return ret;
}


std::string util::dl_inet_res(std::string res, fs::path fname, bool *working, std::function<void(unsigned)> cb)
{
	std::string ret;
	std::ofstream f {fname, std::ios::binary};
	if(!f.good())
		return "Failed to open file for writing: " + fname.string();

	auto hinet {InternetOpenA("ytdlp-interface", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0)};
	if(hinet)
	{
		auto hfile {InternetOpenUrlA(hinet, res.data(), NULL, 0, 0, 0)};
		if(hfile)
		{
			DWORD read {1};

			while(read)
			{
				if(working && !*working)
				{
					f.close();
					fs::remove(fname);
					break;
				}
				std::string buf(65535, '\0');
				if(InternetReadFile(hfile, &buf.front(), buf.size(), &read))
				{
					if(read < buf.size())
						buf.resize(read);
					f.write(buf.data(), buf.size());
					if(!f.good())
					{
						ret = "Failed writing to file: " + fname.string();
						break;
					}
					if(cb)
					{
						if(working)
						{
							if(*working)
								cb(buf.size());
						}
						else cb(buf.size());
					}
				}
				else
				{
					ret = get_last_error_str(true);
					break;
				}
			}
			InternetCloseHandle(hfile);
		}
		else ret = get_last_error_str(true);
		InternetCloseHandle(hinet);
	}
	else ret = get_last_error_str(true);

	return ret;
}


std::string util::extract_7z(fs::path arc_path, fs::path out_path, unsigned ffmpeg, bool ytdlp_interface)
{
	using namespace bit7z;

	std::wstring modpath(4096, '\0');
	modpath.resize(GetModuleFileNameW(0, &modpath.front(), modpath.size()));
	fs::path lib_path {modpath};
	lib_path.replace_filename("7z.dll");

	try
	{
		Bit7zLibrary lib {lib_path};
		BitExtractor extractor {lib, ffmpeg ? BitFormat::Zip : BitFormat::SevenZip};
		if(ffmpeg)
		{
			if(is_win7())
			{
				extractor.extractMatching(arc_path, L"ffmpeg_git\\ffmpeg.exe", out_path);
				extractor.extractMatching(arc_path, L"ffmpeg_git\\ffprobe.exe", out_path);
				if(ffmpeg > 1)
					extractor.extractMatching(arc_path, L"ffmpeg_git\\ffplay.exe", out_path);
			}
			else
			{
				extractor.extractMatching(arc_path, L"*\\bin\\ffmpeg.exe", out_path);
				extractor.extractMatching(arc_path, L"*\\bin\\ffprobe.exe", out_path);
				if(ffmpeg > 1)
					extractor.extractMatching(arc_path, L"*\\bin\\ffplay.exe", out_path);
			}
		}
		else if(ytdlp_interface)
		{
			extractor.extractMatching(arc_path, L"ytdlp-interface.exe", out_path);
		}
		else if(arc_path.string().contains("deno-"))
		{
			BitExtractor ex {lib, BitFormat::Zip};
			ex.extract(arc_path, out_path);
		}
		else extractor.extract(arc_path, out_path);
		return "";
	}
	catch(const BitException& ex)
	{
		return ex.what();
	}
}


std::wstring util::get_clipboard_text()
{
	std::wstring text;
	if(OpenClipboard(NULL))
	{
		if(IsClipboardFormatAvailable(CF_UNICODETEXT))
		{
			auto h {GetClipboardData(CF_UNICODETEXT)};
			if(h)
			{
				auto ptext {static_cast<wchar_t*>(GlobalLock(h))};
				if(ptext)
				{
					text = ptext;
					GlobalUnlock(h);
				}
			}
		}
		CloseClipboard();
	}
	return text;
}


void util::set_clipboard_text(HWND hwnd, std::wstring text)
{
	if(!text.empty() && OpenClipboard(hwnd))
	{
		if(EmptyClipboard())
		{
			HGLOBAL hmem {GlobalAlloc(GMEM_MOVEABLE, text.size()*2+2)};
			if(hmem)
			{
				auto ptext {static_cast<wchar_t*>(GlobalLock(hmem))};
				if(ptext)
				{
					wmemcpy(ptext, text.data(), text.size());
					GlobalUnlock(hmem);
					if(!SetClipboardData(CF_UNICODETEXT, hmem))
						GlobalFree(hmem);
				}
				else GlobalFree(hmem);
			}
		}
		CloseClipboard();
	}
}

bool util::is_dir_writable(fs::path dir)
{
	try
	{
		if(std::ofstream {dir / "write_test.out"}.good())
		{
			std::error_code ec;
			fs::remove(dir / "write_test.out", ec);
			return true;
		}
	}
	catch(...) {}
	return false;
}


bool util::is_win7()
{
	OSVERSIONINFOEX osvi;
	ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
	osvi.dwMajorVersion = 6;
	osvi.dwMinorVersion = 1;

	DWORDLONG conditionMask {0};
	VER_SET_CONDITION(conditionMask, VER_MAJORVERSION, VER_EQUAL);
	VER_SET_CONDITION(conditionMask, VER_MINORVERSION, VER_EQUAL);

	return VerifyVersionInfo(&osvi, VER_MAJORVERSION | VER_MINORVERSION, conditionMask);
}


int util::scale(int val)
{
	const static double dpi {static_cast<double>(nana::API::screen_dpi(true))};
	if(dpi != 96)
		val = round(val * dpi / 96);
	return val;
};


unsigned util::scale_uint(unsigned val)
{
	const static double dpi {static_cast<double>(nana::API::screen_dpi(true))};
	if(dpi != 96)
		val = round(val * dpi / 96);
	return val;
}


util::INTERNET_STATUS util::check_inet_connection()
{
	INTERNET_STATUS connectedStatus = INTERNET_STATUS::CONNECTION_ERROR;
	HRESULT hr = S_FALSE;

	try
	{
		hr = CoInitialize(NULL);
		if(SUCCEEDED(hr))
		{
			INetworkListManager *pNetworkListManager;
			hr = CoCreateInstance(CLSID_NetworkListManager, NULL, CLSCTX_ALL, __uuidof(INetworkListManager), (LPVOID *)&pNetworkListManager);
			if(SUCCEEDED(hr))
			{
				NLM_CONNECTIVITY nlmConnectivity = NLM_CONNECTIVITY::NLM_CONNECTIVITY_DISCONNECTED;
				VARIANT_BOOL isConnected = VARIANT_FALSE;
				hr = pNetworkListManager->get_IsConnectedToInternet(&isConnected);
				if(SUCCEEDED(hr))
				{
					if(isConnected == VARIANT_TRUE)
						connectedStatus = INTERNET_STATUS::CONNECTED;
					else
						connectedStatus = INTERNET_STATUS::DISCONNECTED;
				}

				if(isConnected == VARIANT_FALSE && SUCCEEDED(pNetworkListManager->GetConnectivity(&nlmConnectivity)))
				{
					if(nlmConnectivity & (NLM_CONNECTIVITY_IPV4_LOCALNETWORK | NLM_CONNECTIVITY_IPV4_SUBNET | NLM_CONNECTIVITY_IPV6_LOCALNETWORK | NLM_CONNECTIVITY_IPV6_SUBNET))
					{
						connectedStatus = INTERNET_STATUS::CONNECTED_TO_LOCAL;
					}
				}

				pNetworkListManager->Release();
			}
		}

		CoUninitialize();
	}
	catch(...)
	{
		connectedStatus = INTERNET_STATUS::CONNECTION_ERROR;
	}

	return connectedStatus;
}


BOOL util::pwr_shutdown()
{	
	return ExitWindowsEx(EWX_POWEROFF | EWX_FORCE, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
}


BOOL util::pwr_enable_shutdown_privilege()
{
	HANDLE hToken;
	TOKEN_PRIVILEGES tkp;
	if(OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
	{
		LookupPrivilegeValue(NULL, SE_SHUTDOWN_NAME, &tkp.Privileges[0].Luid);
		tkp.PrivilegeCount = 1;
		tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
		auto res {AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, NULL, 0)};
		if(res) res = GetLastError() == ERROR_SUCCESS;
		CloseHandle(hToken);
		return res;
	}
	return FALSE;
}


BOOL util::pwr_sleep()
{
	return SetSuspendState(0, 0, 0);
}


BOOL util::pwr_hibernate()
{
	return SetSuspendState(1, 0, 0);
}


bool util::pwr_can_hibernate()
{
	regkey regpwr {HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Power"};
	auto hib_enabled {regpwr.get_dword("HibernateEnabled")};
	if(hib_enabled != -1)
		return hib_enabled == 1;
	return false;
}


fs::path util::to_relative_path(const fs::path &abs)
{
	// Never throws: fs::relative() raises filesystem_error on some network/virtual
	// file systems (e.g. rclone/WinFsp mounts), so use the error_code overloads
	// and fall back to the absolute path.
	std::error_code ec;
	if(abs.empty())
		return abs;
	const auto cwd {fs::current_path(ec)};
	if(ec || cwd.empty() || !abs.is_absolute()
		|| abs.root_name() != cwd.root_name()) // different drive/mount: relative form is meaningless
		return abs;
	auto path {abs.lexically_relative(cwd)};
	const auto path_string {path.string()};
	if(path_string.empty() || path_string.starts_with("..\\") || path_string.starts_with("../"))
		return abs;
	if(path_string == ".")
		return ".\\";
	return ".\\" / path;
}


std::wstring util::sanitize_timestamp(const std::wstring &s)
{
	std::wstring out;
	for(auto c : s)
		if((c >= L'0' && c <= L'9') || c == L':' || c == L'.')
			out += c;
	return out;
}


std::wstring util::normalize_timestamp(const std::wstring &in)
{
	auto s {sanitize_timestamp(in)};
	if(s.empty() || s.find(L':') != std::wstring::npos)
		return s; // already in h:m:s form (or blank)
	std::wstring digits {s}, frac;
	if(const auto dot {s.find(L'.')}; dot != std::wstring::npos)
	{
		digits = s.substr(0, dot);
		frac = s.substr(dot);
		if(frac.find(L'.', 1) != std::wstring::npos)
			return s;
	}
	if(digits.empty() || digits.size() > 9)
		return s;
	long long ss {0}, mm {0}, hh {0};
	try
	{
		const auto n {digits.size()};
		ss = std::stoll(digits.substr(n > 2 ? n - 2 : 0));
		if(n > 2) mm = std::stoll(digits.substr(n > 4 ? n - 4 : 0, n > 4 ? 2 : n - 2));
		if(n > 4) hh = std::stoll(digits.substr(0, n - 4));
	}
	catch(...) { return s; }
	const long long total {hh * 3600 + mm * 60 + ss};
	wchar_t buf[48];
	swprintf_s(buf, L"%02lld:%02lld:%02lld", total / 3600, total / 60 % 60, total % 60);
	return buf + (frac.size() > 1 ? frac : std::wstring {});
}


static bool parse_timestamp(const std::wstring &str, double &seconds)
{
	// accepts "90", "1:30", "01:02:03" and fractions on the last part ("1:30.5")
	seconds = 0;
	if(str.empty())
		return false;
	size_t start {0};
	double total {0};
	for(;;)
	{
		const auto pos {str.find(L':', start)};
		const auto part {str.substr(start, pos == std::wstring::npos ? std::wstring::npos : pos - start)};
		if(part.empty())
			return false;
		double val {0};
		try { val = std::stod(part); }
		catch(...) { return false; }
		total = total * 60 + val;
		if(pos == std::wstring::npos)
			break;
		start = pos + 1;
	}
	seconds = total;
	return true;
}


std::wstring util::segment_tag(const std::wstring &start, const std::wstring &end)
{
	auto fmt = [](double secs)
	{
		const long long t {static_cast<long long>(secs)};
		wchar_t buf[32];
		swprintf_s(buf, L"%02lld%02lld%02lld", t / 3600, (t / 60) % 60, t % 60);
		return std::wstring {buf};
	};
	double s {0}, e {0};
	const bool has_start {parse_timestamp(normalize_timestamp(start), s)}, has_end {parse_timestamp(normalize_timestamp(end), e) && e > 0};
	return L"[" + (has_start ? fmt(s) : std::wstring {L"000000"}) + L'-' + (has_end ? fmt(e) : std::wstring {L"end"}) + L']';
}


bool util::path_exists(const fs::path &p)
{
	std::error_code ec;
	const bool res {fs::exists(p, ec)};
	return !ec && res;
}


fs::path util::appdir()
{
	std::wstring modpath(4096, '\0');
	modpath.resize(GetModuleFileNameW(0, &modpath.front(), modpath.size()));
	modpath.erase(modpath.rfind('\\'));
	return modpath;
}


fs::path util::app_path()
{
	std::wstring modpath(4096, '\0');
	modpath.resize(GetModuleFileNameW(0, &modpath.front(), modpath.size()));
	return modpath;
}


bool util::is_file_in_sys_path(const std::wstring &fname)
{
	std::wstringstream ss {util::get_sys_var(L"PATH")};
	std::wstring path;
	while(std::getline(ss, path, L';'))
	{
		std::error_code ec;
		if(fs::exists(fs::path {path} / fname, ec))
			return true;
	}
	return false;
}


std::wstring util::get_sys_var(const std::wstring &name)
{
	std::wstring var (32767, ' ');
	var.resize(GetEnvironmentVariable(name.data(), &var.front(), var.size()));
	var.shrink_to_fit();
	return var;
}


std::string util::short_error(const std::string &error_line)
{
	std::string line {error_line};
	while(!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
		line.pop_back();
	std::string low {line};
	for(auto &c : low)
		c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	auto has = [&](const char *s) { return low.find(s) != std::string::npos; };

	if(has("requested format is not available") || has("requested format not available"))
		return "format not available";
	if(has("not a bot"))
		return "YouTube bot check (try cookies)";
	if(has("confirm your age") || has("age-restricted") || has("age restricted"))
		return "age check / sign-in required";
	if(has("private video"))
		return "private video";
	if(has("members-only") || has("join this channel") || has("members only"))
		return "members only";
	if(has("sign in") || has("login required") || has("--cookies"))
		return "sign-in required (use cookies)";
	if(has("premieres in") || has("this live event will begin") || has("live event will begin"))
		return "not started yet (premiere/live)";
	if(has("video unavailable") || has("this video is unavailable") || has("has been removed"))
		return "video unavailable";
	if(has("not available in your country") || has("geo restrict") || has("geo-restrict"))
		return "blocked in your country";
	if(has("unsupported url"))
		return "unsupported URL";
	if(has("signature") || has("nsig") || has("n challenge") || has("unable to extract") || has("unable to download json"))
		return "site changed - update yt-dlp";
	if(has("no space left") || has("errno 28"))
		return "disk full";
	if(has("permission denied") || has("errno 13") || has("access is denied"))
		return "permission denied (folder)";
	if(has("being used by another process") || has("winerror 32"))
		return "file in use by another program";
	if(has("unable to rename"))
		return "can't rename file (in use?)";
	if(has("ffmpeg") && (has("not found") || has("not installed")))
		return "ffmpeg not found";
	if(has("conversion failed") || has("postprocessing") || has("post-processing"))
		return "post-processing (ffmpeg) failed";
	if(has("errno 2") || has("no such file or directory") || has("winerror 3") || has("cannot find the path"))
		return "file/folder not found";
	if(has("timed out") || has("timeout"))
		return "network timeout";
	if(has("getaddrinfo") || has("failed to resolve") || has("name or service not known") || has("no address associated"))
		return "no internet / DNS error";
	if(has("http error"))
	{
		const auto pos {low.find("http error")};
		std::string code;
		for(size_t i {pos + 10}; i < low.size() && code.size() < 3; i++)
			if(isdigit(static_cast<unsigned char>(low[i])))
				code += low[i];
			else if(!code.empty())
				break;
		if(code == "403") return "HTTP 403 (blocked/forbidden)";
		if(code == "404") return "HTTP 404 (not found)";
		if(code == "429") return "HTTP 429 (too many requests)";
		if(code == "410") return "HTTP 410 (gone)";
		if(!code.empty()) return "HTTP " + code;
		return "HTTP error";
	}

	// fallback: the message itself without the "ERROR: [site] id:" prefix, shortened
	std::string msg {line};
	if(msg.starts_with("ERROR:"))
		msg.erase(0, 6);
	while(!msg.empty() && msg.front() == ' ')
		msg.erase(0, 1);
	if(!msg.empty() && msg.front() == '[')
	{
		const auto pos {msg.find("] ")};
		if(pos != std::string::npos)
			msg.erase(0, pos + 2);
		const auto colon {msg.find(": ")};
		if(colon != std::string::npos && colon < 40)
			msg.erase(0, colon + 2);
	}
	if(msg.size() > 60)
	{
		msg.resize(57);
		msg += "...";
	}
	return msg.empty() ? "unknown error" : msg;
}
