#pragma once

// Activity log + crash reporting.
//
//  logs\activity.log      one line per important event, written to disk immediately (survives a crash)
//  logs\activity.old.log  previous activity log (rotated at 5 MB)
//  logs\crash_<time>.txt  readable crash report: error, module+offset, call stack, last activity lines
//  logs\crash_<time>.dmp  minidump of the crashed process (open with the matching ytdlp-interface.pdb)

#include <Windows.h>
#include <DbgHelp.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <deque>
#include <string>
#include <vector>
#include <atomic>
#include <exception>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstdint>

#pragma comment(lib, "Dbghelp.lib")

namespace applog
{
	namespace detail
	{
		inline std::recursive_mutex mtx;
		inline std::ofstream file;
		inline std::filesystem::path dir, path, oldpath;
		inline std::deque<std::string> recent;
		inline bool ready {false};
		inline std::atomic_bool crashing {false};
		inline constexpr std::uintmax_t max_size {5ull * 1024 * 1024};
		inline constexpr size_t keep_recent {60};

		inline std::string utf8(const std::wstring &w)
		{
			if(w.empty())
				return {};
			const int n {WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr)};
			std::string s(n > 0 ? n : 0, '\0');
			if(n > 0)
				WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
			return s;
		}

		inline std::string timestamp(bool for_filename = false)
		{
			SYSTEMTIME st;
			GetLocalTime(&st);
			char buf[64];
			if(for_filename)
				sprintf_s(buf, "%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
			else sprintf_s(buf, "%04u-%02u-%02u %02u:%02u:%02u.%03u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
			return buf;
		}

		inline void open_file()
		{
			std::error_code ec;
			if(std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) >= max_size && !ec)
			{
				std::filesystem::remove(oldpath, ec);
				std::filesystem::rename(path, oldpath, ec);
			}
			file.open(path, std::ios::app | std::ios::binary);
		}

		// strips the values of options that may contain credentials
		inline std::string redact(std::string s)
		{
			for(const char *opt : {"--proxy ", "--password ", "--video-password ", "--username ", "--ap-password ", "--ap-username ", "-p ", "-u "})
			{
				size_t pos {0};
				const std::string o {opt};
				while((pos = s.find(o, pos)) != std::string::npos)
				{
					if(pos > 0 && s[pos - 1] != ' ')
					{
						pos += o.size();
						continue;
					}
					size_t start {pos + o.size()}, end;
					if(start < s.size() && s[start] == '"')
						end = s.find('"', start + 1), end = end == std::string::npos ? s.size() : end + 1;
					else end = s.find(' ', start), end = end == std::string::npos ? s.size() : end;
					s.replace(start, end - start, "***");
					pos = start + 3;
				}
			}
			return s;
		}
	}

	inline std::filesystem::path folder() { return detail::dir; }

	inline void write(const std::string &category, std::string msg)
	{
		for(auto &c : msg)
			if(c == '\r' || c == '\n')
				c = ' ';
		while(!msg.empty() && msg.back() == ' ')
			msg.pop_back();
		const std::string line {detail::timestamp() + " [" + category + "] " + msg};
		std::unique_lock<std::recursive_mutex> lock {detail::mtx, std::defer_lock};
		if(detail::crashing)
		{
			if(!lock.try_lock())
				return;
		}
		else lock.lock();
		detail::recent.push_back(line);
		if(detail::recent.size() > detail::keep_recent)
			detail::recent.pop_front();
		if(!detail::ready || !detail::file.is_open())
			return;
		detail::file << line << "\r\n";
		detail::file.flush();
		if(static_cast<std::uintmax_t>(detail::file.tellp()) >= detail::max_size)
		{
			detail::file.close();
			detail::open_file();
		}
	}

	inline void write(const std::string &category, const std::wstring &msg) { write(category, detail::utf8(msg)); }
	inline void write(const std::string &category, const char *msg) { write(category, std::string {msg ? msg : ""}); }
	inline void write(const std::string &category, const wchar_t *msg) { write(category, std::wstring {msg ? msg : L""}); }

	// for command lines: hides passwords/proxy credentials
	inline void write_cmd(const std::string &category, const std::wstring &prefix, const std::wstring &cmd)
	{
		write(category, detail::utf8(prefix) + detail::redact(detail::utf8(cmd)));
	}

	inline void init(const std::filesystem::path &logdir)
	{
		std::lock_guard<std::recursive_mutex> lock {detail::mtx};
		std::error_code ec;
		detail::dir = logdir;
		std::filesystem::create_directories(logdir, ec);
		detail::path = logdir / "activity.log";
		detail::oldpath = logdir / "activity.old.log";
		detail::open_file();
		detail::ready = detail::file.is_open();
		if(detail::ready)
		{
			detail::file << "\r\n";
			detail::file.flush();
		}
	}


	namespace detail
	{
		inline const char *exception_name(DWORD code)
		{
			switch(code)
			{
			case EXCEPTION_ACCESS_VIOLATION: return "access violation";
			case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
			case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
			case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
			case EXCEPTION_IN_PAGE_ERROR: return "in-page error (file/network read failed)";
			case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
			case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
			case EXCEPTION_BREAKPOINT: return "breakpoint";
			case 0xE06D7363: return "unhandled C++ exception";
			case 0xC0000409: return "stack buffer overrun / fast fail";
			case 0xC0000374: return "heap corruption";
			default: return "unknown";
			}
		}

		inline std::string module_and_offset(DWORD64 addr)
		{
			HMODULE mod {nullptr};
			char buf[600];
			if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(addr), &mod) && mod)
			{
				wchar_t name[MAX_PATH] {};
				GetModuleFileNameW(mod, name, MAX_PATH);
				const std::wstring wname {name};
				const auto slash {wname.find_last_of(L"\\/")};
				sprintf_s(buf, "%s+0x%llX", utf8(slash == std::wstring::npos ? wname : wname.substr(slash + 1)).c_str(),
					static_cast<unsigned long long>(addr - reinterpret_cast<DWORD64>(mod)));
			}
			else sprintf_s(buf, "<unknown module> 0x%llX", static_cast<unsigned long long>(addr));
			return buf;
		}

		inline std::string stack_trace(CONTEXT ctx)
		{
			std::string out;
			const HANDLE proc {GetCurrentProcess()}, thread {GetCurrentThread()};
			SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
			const bool syms {SymInitialize(proc, nullptr, TRUE) == TRUE};

			STACKFRAME64 sf {};
#if defined(_M_X64) || defined(_M_AMD64)
			const DWORD machine {IMAGE_FILE_MACHINE_AMD64};
			sf.AddrPC.Offset = ctx.Rip;
			sf.AddrFrame.Offset = ctx.Rbp;
			sf.AddrStack.Offset = ctx.Rsp;
#else
			const DWORD machine {IMAGE_FILE_MACHINE_I386};
			sf.AddrPC.Offset = ctx.Eip;
			sf.AddrFrame.Offset = ctx.Ebp;
			sf.AddrStack.Offset = ctx.Esp;
#endif
			sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;

			alignas(SYMBOL_INFO) char symbuf[sizeof(SYMBOL_INFO) + 512] {};
			auto *sym {reinterpret_cast<SYMBOL_INFO*>(symbuf)};
			for(int i {0}; i < 64; i++)
			{
				if(!StackWalk64(machine, proc, thread, &sf, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
					break;
				const DWORD64 addr {sf.AddrPC.Offset};
				if(!addr)
					break;
				char line[1200];
				std::string fn, src;
				if(syms)
				{
					sym->SizeOfStruct = sizeof(SYMBOL_INFO);
					sym->MaxNameLen = 511;
					DWORD64 disp {0};
					if(SymFromAddr(proc, addr, &disp, sym))
						fn = std::string {sym->Name} + "+0x" + [&] { char b[32]; sprintf_s(b, "%llX", static_cast<unsigned long long>(disp)); return std::string {b}; }();
					IMAGEHLP_LINE64 ln {};
					ln.SizeOfStruct = sizeof ln;
					DWORD ldisp {0};
					if(SymGetLineFromAddr64(proc, addr, &ldisp, &ln) && ln.FileName)
						src = std::string {ln.FileName} + ":" + std::to_string(ln.LineNumber);
				}
				sprintf_s(line, "  #%02d  %-40s %s %s\r\n", i, module_and_offset(addr).c_str(), fn.c_str(), src.c_str());
				out += line;
			}
			if(syms)
				SymCleanup(proc);
			return out;
		}

		inline void write_report(EXCEPTION_POINTERS *ep, const std::string &what)
		{
			if(crashing.exchange(true))
				return;
			std::error_code ec;
			if(dir.empty())
				return;
			std::filesystem::create_directories(dir, ec);
			const std::string stamp {timestamp(true)};
			const auto txtpath {dir / ("crash_" + stamp + ".txt")}, dmppath {dir / ("crash_" + stamp + ".dmp")};

			std::string rep;
			rep += "ytdlp-interface crash report\r\n";
			rep += "time:    " + timestamp() + "\r\n";
			rep += "build:   " __DATE__ " " __TIME__ "\r\n";
			rep += "reason:  " + what + "\r\n";
			char buf[512];
			CONTEXT ctx {};
			if(ep && ep->ExceptionRecord)
			{
				const auto &er {*ep->ExceptionRecord};
				sprintf_s(buf, "code:    0x%08lX (%s)\r\n", er.ExceptionCode, exception_name(er.ExceptionCode));
				rep += buf;
				rep += "where:   " + module_and_offset(reinterpret_cast<DWORD64>(er.ExceptionAddress)) + "\r\n";
				if((er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION || er.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && er.NumberParameters >= 2)
				{
					const auto op {er.ExceptionInformation[0]};
					sprintf_s(buf, "access:  %s address 0x%llX\r\n", op == 0 ? "reading" : op == 1 ? "writing" : "executing",
						static_cast<unsigned long long>(er.ExceptionInformation[1]));
					rep += buf;
				}
				if(ep->ContextRecord)
					ctx = *ep->ContextRecord;
			}
			else RtlCaptureContext(&ctx);
			sprintf_s(buf, "thread:  %lu%s\r\n", GetCurrentThreadId(), GetCurrentThreadId() == GetWindowThreadProcessId(GetForegroundWindow(), nullptr) ? " (UI thread)" : "");
			rep += buf;
			rep += "\r\ncall stack:\r\n" + stack_trace(ctx);

			rep += "\r\nlast activity:\r\n";
			{
				std::unique_lock<std::recursive_mutex> lock {mtx, std::try_to_lock};
				for(const auto &l : recent)
					rep += "  " + l + "\r\n";
			}

			std::ofstream {txtpath, std::ios::binary} << rep;

			const HANDLE hf {CreateFileW(dmppath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
			if(hf != INVALID_HANDLE_VALUE)
			{
				MINIDUMP_EXCEPTION_INFORMATION mei {};
				mei.ThreadId = GetCurrentThreadId();
				mei.ExceptionPointers = ep;
				mei.ClientPointers = FALSE;
				MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hf,
					static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory | MiniDumpWithThreadInfo),
					ep ? &mei : nullptr, nullptr, nullptr);
				CloseHandle(hf);
			}

			if(file.is_open())
			{
				file << timestamp() << " [CRASH] " << what << " -> " << txtpath.filename().string() << "\r\n";
				file.flush();
			}
		}

		inline LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *ep)
		{
			write_report(ep, "unhandled exception");
			return EXCEPTION_CONTINUE_SEARCH; // let Windows show/record the crash as usual
		}

		inline void on_terminate()
		{
			std::string what {"std::terminate"};
			if(auto e {std::current_exception()})
			{
				try { std::rethrow_exception(e); }
				catch(const std::exception &x) { what += std::string {" - uncaught exception: "} + x.what(); }
				catch(...) { what += " - uncaught non-standard exception"; }
			}
			write_report(nullptr, what);
			TerminateProcess(GetCurrentProcess(), 3);
		}

		inline void on_abort(int)
		{
			write_report(nullptr, "abort() (usually an uncaught C++ exception in a worker thread)");
			TerminateProcess(GetCurrentProcess(), 3);
		}

		inline void on_purecall()
		{
			write_report(nullptr, "pure virtual function call");
			TerminateProcess(GetCurrentProcess(), 3);
		}

		inline void on_invalid_parameter(const wchar_t *expr, const wchar_t *func, const wchar_t *file, unsigned line, uintptr_t)
		{
			std::string what {"invalid CRT parameter"};
			if(func)
				what += " in " + utf8(func);
			if(file)
				what += " (" + utf8(file) + ":" + std::to_string(line) + ")";
			if(expr)
				what += ": " + utf8(expr);
			write_report(nullptr, what);
			TerminateProcess(GetCurrentProcess(), 3);
		}
	}

	// call once at startup, on the main thread
	inline void install_crash_handlers()
	{
		ULONG guarantee {64 * 1024}; // room for the handler after a stack overflow
		SetThreadStackGuarantee(&guarantee);
		SetUnhandledExceptionFilter(detail::unhandled_filter);
		std::set_terminate(detail::on_terminate);
		std::signal(SIGABRT, detail::on_abort);
		_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
		_set_purecall_handler(detail::on_purecall);
		_set_invalid_parameter_handler(detail::on_invalid_parameter);
	}

	// worker threads: std::terminate handlers are per thread in MSVC
	inline void install_thread_handlers()
	{
		std::set_terminate(detail::on_terminate);
	}

	inline std::string windows_version()
	{
		using fnRtlGetVersion = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);
		RTL_OSVERSIONINFOW vi {sizeof vi};
		if(auto f {reinterpret_cast<fnRtlGetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))})
			f(&vi);
		return "Windows " + std::to_string(vi.dwMajorVersion) + "." + std::to_string(vi.dwMinorVersion) + " build " + std::to_string(vi.dwBuildNumber);
	}
}
