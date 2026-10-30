#include "gui.hpp"
#include <nana/gui/filebox.hpp>
#include "icons.hpp"

#include <regex>
#include <codecvt>
#include <algorithm>
#include <cwctype>
#include <nana/system/platform.hpp>
#include <sys/types.h>
#include <sys/stat.h>

settings_t GUI::conf;
std::unordered_map<std::string, settings_t> GUI::conf_presets;
nana::internationalization GUI::lang;


GUI::GUI() : themed_form {std::bind(&GUI::apply_theme, this, std::placeholders::_1)}
{
	using namespace nana;
	main_thread_id = std::this_thread::get_id();

	make_message_handlers();
	pwr_can_shutdown = util::pwr_enable_shutdown_privilege();

	SYSTEM_INFO sysinfo;
	GetSystemInfo(&sysinfo);
	number_of_processors = sysinfo.dwNumberOfProcessors;

	std::wstring modpath(4096, '\0');
	modpath.resize(GetModuleFileNameW(0, &modpath.front(), modpath.size()));
	self_path = modpath;
	modpath.erase(modpath.rfind('\\'));
	appdir = modpath;

	if(conf.ffmpeg_path.empty())
	{
		if(fs::exists(appdir / "ffmpeg.exe"))
			conf.ffmpeg_path = ".\\";
		else if(!conf.ytdlp_path.empty())
		{
			auto tmp {fs::path {conf.ytdlp_path}.replace_filename("ffmpeg.exe")};
			if(fs::exists(tmp))
				conf.ffmpeg_path = tmp.parent_path();
		}
	}

	tmsg.interval(std::chrono::milliseconds {300});
	tmsg.elapse([this]
	{
		if(!tmsg_text.empty())
		{
			if(api::is_window(tmsg_parent))
			{
				::widgets::msgbox mbox {tmsg_parent, tmsg_title};
				mbox.icon(MB_ICONERROR) << tmsg_text;
				tmsg_text.clear();
				tmsg_title.clear();
				mbox();
			}
		}
	});
	tmsg.start();

	tqueue.interval(std::chrono::milliseconds {350});
	tqueue.elapse([this]
	{
		if(conf.cb_clear_done)
		{
			std::vector<std::wstring> completed;

			for(size_t cat {0}; cat < lbq.size_categ(); cat++)
				for(auto item : lbq.at(cat))
				{
					const auto text {item.text(3)};
					if(text == "done" || text == "already downloaded")
						completed.push_back(item.value<lbqval_t>().url);
				}

			if(!completed.empty())
			{
				lbq.auto_draw(false);
				autostart_next_item = false;
				for(auto &url : completed)
				{
					if(!bottoms.at(url).dl_thread.joinable())
					{
						auto item {lbq.item_from_value(url)};
						if(item != lbq.empty_item)
							queue_remove_item(url, false);
					}
				}
				autostart_next_item = true;
				auto sel {lbq.selected()};
				if(sel.size() > 1)
					for(auto n {1}; n < sel.size(); n++)
						lbq.at(sel[n]).select(false);
				lbq.auto_draw(true);
				queue_save();
				if(!queue_panel.visible())
				{
					show_output();
					api::refresh_window(*this);
				}
			}
		}
		if(!lbq_url_to_select.empty())
		{
			lbq.item_from_value(lbq_url_to_select).select(true);
			lbq_url_to_select.clear();
		}
		if(start_suspend_fm)
		{
			start_suspend_fm = false;
			fm_suspend();
		}
		if(save_queue)
		{
			save_queue = false;
			queue_save();
			//queue_save_data(50);
		}
	});
	tqueue.start();

	t_url_flash.interval(std::chrono::milliseconds {10});
	t_url_flash.elapse([this]
	{
		static double steps {1000};
		static int iterations {0};
		static bool increasing {false};
		l_url.fgcolor(::widgets::theme::path_link_fg.blend(::widgets::theme::fmbg, 1.0 - steps / 1000));
		if(increasing)
			steps += 100;
		else steps -= 150;
		if(steps == 1000)
		{
			iterations++;
			increasing = false;
		}
		if(steps <= 0)
		{
			steps = 0;
			increasing = true;
		}
		if(iterations == 2)
		{
			steps = 0;
			iterations = 0;
			increasing = true;
			l_url.fgcolor(::widgets::theme::path_link_fg);
			t_url_flash.stop();
		}
	});

	t_load_qitem_data.interval(std::chrono::milliseconds {0});
	t_load_qitem_data.elapse([this]
	{
		t_load_qitem_data.stop();
		if(fs::exists(infopath))
			fm_loading();
		else init_qitems();

		if(!conf.url_passed_as_arg.empty())
			add_url(conf.url_passed_as_arg, false, false);
	});
	t_load_qitem_data.start();

	if(conf.cbtheme == 2)
	{
		if(system_supports_darkmode())
			system_theme(true);
		else conf.cbtheme = 1;
	}
	else if(conf.cbtheme == 0) dark_theme(true);
	if(conf.cbtheme == 1) dark_theme(false);

	if(conf.get_releases_at_startup)
		get_releases(*this);
	caption(title);
	snap(conf.cbsnap);
	snap_fit(true);
	make_form();
	g_log.make_form(*this);
	RegisterDragDrop(hwnd, this);

	if(system_theme())
		apply_theme(is_system_theme_dark());
	else apply_theme(dark_theme());

	get_versions();

	use_ffmpeg_location = !ffmpeg_location_in_conf_file();

	center(1180, 720);
	show_queue(false);
	if(conf.gpopt_hidden)
	{
		expcol.events().click.emit({}, expcol);
		if(conf.winrect.empty())
			center();
	}
	auto sz {api::window_outline_size(*this)};
	minw = sz.width;
	minh = sz.height;
	if(!conf.winrect.empty())
	{
		if(!conf.cbminw && conf.winrect.width < 640)
			conf.winrect.width = dpi_scale(640, conf.dpi);
		else conf.winrect.width = dpi_scale(conf.winrect.width, conf.dpi);
		conf.winrect.height = dpi_scale(conf.winrect.height, conf.dpi);
		const auto maxh {screen {}.from_window(*this).area().dimension().height};
		if(conf.winrect.height < 480)
			conf.winrect.height = 480;
		move(conf.winrect);
		size(conf.winrect.dimension());
		sz = api::window_outline_size(*this);
		if(sz.height > maxh)
		{
			sz.height = maxh;
			api::window_outline_size(*this, sz);
		}
	}
	else center(1180, 720);
	bring_top(true);
	if(conf.zoomed) zoom(true);
	no_draw_freeze = false;
	update_compact_mode();

	auto m {GetSystemMenu(hwnd, FALSE)};
	if(m)
	{
		MENUITEMINFOA i {0};
		i.cbSize = sizeof MENUITEMINFO;
		i.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_ID;
		i.fType = MFT_STRING;
		i.wID = 1337;
		i.dwTypeData = (CHAR*)"Reset size and position\tCtrl+Num0";
		InsertMenuItemA(m, SC_CLOSE, FALSE, &i);
		i.wID = 1338;
		i.dwTypeData = (CHAR*)"Full screen\tF11";
		InsertMenuItemA(m, SC_CLOSE, FALSE, &i);
		i.wID = 1344;
		i.dwTypeData = (CHAR*)"Open logs folder";
		InsertMenuItemA(m, SC_CLOSE, FALSE, &i);
		i.wID = 1337;
		i.dwTypeData = (CHAR*)"Close\tEsc, Alt+F4";
		i.fMask = MIIM_STRING;
		SetMenuItemInfoA(m, SC_CLOSE, FALSE, &i);
	}
}


bool GUI::process_queue_item(std::wstring url)
{
	using widgets::theme;
	auto &bottom {bottoms.at(url)};
	auto &tbpipe {outbox};
	auto &tbpipe_overlay {overlay};
	auto &working {bottom.working};
	auto &graceful_exit {bottom.graceful_exit};

	if(!bottom.started)
	{
		if(qurl == url)
			btndl.caption("Stop download");
		bottom.started = true;
		bottom.idx_error = 0;
		taskbar_overall_progress();
		auto item {lbq.item_from_value(url)};
		if(item.checked())
		{
			item.check(false);
			item.fgcolor(lbq.fgcolor());
		}
		if(tbpipe.current() == url)
		{
			tbpipe.clear();
			if(tbpipe_overlay.visible() && btnq.caption().find("queue") != -1)
				tbpipe.show(url);
		}
		else tbpipe.clear(url);
		if(bottom.cbargs)
		{
			bool check {true};
			const auto size {com_args.the_number_of_options()};
			auto caption {com_args.caption()};
			const auto label_from_caption  {label_from_argset(caption)},
			           argset_from_caption {argset_without_label(caption)};
			auto &conf_argsets {conf.argsets};
			if(!label_from_caption.empty())
			{
				for(size_t n {0}; n < size; n++)
				{
					const auto item_text {com_args.text(n)}, item_label {label_from_argset(item_text)};
					if(argset_without_label(item_text) == argset_from_caption && label_from_caption != item_label)
					{
						caption = '[' + label_from_caption + "] " + argset_from_caption;
						com_args.caption(caption);
						com_args[argset_from_caption].text(caption);
						auto it {std::find(conf.argsets.begin(), conf.argsets.end(), item_text)};
						if(it != conf.argsets.end())
							*it = caption;
						if(conf.common_dl_options)
						{
							for(auto &pbot : bottoms)
							{
								auto &bot {*pbot.second};
								bot.argset = caption;
							}
						}
						else bottoms.current().argset = caption;
						btnerase.enable(true);
						check = false;
						break;
					}
				}
			}

			if(check)
			{
				const auto args {bottom.argset};
				auto idx {com_args.caption_index()};
				if(idx == -1)
				{
					com_args[argset_without_label(args)].text(args);
					com_args.option(size);
					if(size >= 10)
					{
						com_args.erase(0);
						conf.argsets.erase(conf.argsets.begin());
					}
					conf.argsets.push_back(args);
				}
				com_args.option(idx);
				if(conf.common_dl_options)
					bottoms.propagate_args_options(bottom);
			}
		}

		std::wstring cmd {L'\"' + conf.ytdlp_path.wstring() + L'\"'};

		if(conf.cb_aria)
		{
			cmd += L" --downloader aria2c";
			if(!conf.aria_options.empty())
			{
				cmd += L" --downloader-args aria2c:";
				if(conf.aria_options.front() == '\"' && conf.aria_options.back() == '\"')
					cmd += nana::to_wstring(conf.aria_options);
				else cmd += L'\"' + nana::to_wstring(conf.aria_options) + L'\"';
			}
		}

		if(bottom.is_ytlink || bottom.is_ytplaylist)
		{
			std::error_code ec;
			if(fs::exists(conf.ytdlp_path.parent_path() / "qjs.exe", ec))
				cmd += L" --js-runtimes quickjs";
		}

		if(conf.com_cookies)
			cmd += L" --cookies-from-browser " + com_cookies_options[conf.com_cookies] + nana::to_wstring(conf.cookie_options);
		else if(conf.cb_cookies && !conf.cookies_path.empty())
			cmd += L" --cookies \"" + conf.cookies_path.wstring() + L"\"";

		if(!bottom.using_custom_fmt())
		{
			if(bottom.use_strfmt)
			{
				if(bottom.fmt1.empty() && conf.pref_audio == 6)
				{
					auto it {std::find_if(bottom.vidinfo["formats"].begin(), bottom.vidinfo["formats"].end(), [&](const auto &el)
					{
						return el["format"].get<std::string>().rfind(nana::to_utf8(bottom.fmt2)) != -1;
					})};
					if(it != bottom.vidinfo["formats"].end())
					{
						std::string acodec, aext;
						auto &fmt {*it};
						if(fmt.contains("acodec") && fmt["acodec"] != nullptr)
							acodec = fmt["acodec"];
						if(fmt.contains("audio_ext") && fmt["audio_ext"] != nullptr)
							aext = fmt["audio_ext"];
						if(acodec == "opus" && aext != "opus")
							cmd += L" -x";
					}
				}
				else if(bottom.fmt2.find('+') != -1 || bottom.fmt2 == L"mergeall")
				{
					cmd += L" --audio-multistreams";
					if(!bottom.cbargs || bottom.argset.find("--embed-metadata") == -1)
						cmd += L" --embed-metadata";
				}
				if(!bottom.fmt1.empty() && conf.pref_video == 3)
					cmd += L" --remux-video mkv";

				cmd += L" -f " + bottom.strfmt;
			}
			else
			{
				// per-item quality (side panel): 1 = best, 2..7 = max height, 8 = audio only
				const int quality {bottom.quality};
				if(quality == 8)
					cmd += L" -f \"ba/b\"";
				if(quality == 0 && bottom.is_ytlink && conf.cb_premium && conf.pref_res <= 4 && (conf.pref_vcodec != 0 && conf.pref_vcodec != 3 || conf.pref_video == 2))
				{
					if(bottom.is_ytchan || bottom.is_yttab || bottom.is_ytplaylist)
					{
						if(conf.pref_res == 4)
							cmd += L" -f \"bv*[format_note*=Premium]+ba / bv*+ba\"";
					}
					else
					{
						std::string fmtid_premium;
						const auto &formats {bottom.vidinfo["formats"]};
						for(auto &el : formats)
						{
							if(el.contains("format_note") && el["format_note"] != nullptr && el["format_note"] == "Premium")
							{
								fmtid_premium = el["format_id"];
								break;
							}
						}
						if(!fmtid_premium.empty())
						{
							int maxres {1080};
							if(conf.pref_res < 4)
							{
								for(auto &el : formats)
								{
									if(el.contains("height") && el["height"] != nullptr)
									{
										int height {el["height"]};
										if(height > maxres)
											maxres = height;
									}
								}
							}
							if(maxres == 1080 && !fmtid_premium.empty())
								cmd += L" -f " + nana::to_wstring(fmtid_premium) + L"+ba";
						}
					}
				}

				if(conf.pref_video == 3 && bottom.vidinfo_contains("vcodec") && bottom.vidinfo["vcodec"] != "none")
					cmd += L" --remux-video mkv";

				std::wstring strpref {L" -S \""};
				static const wchar_t *quality_res[] {L"", L"", L"2160", L"1440", L"1080", L"720", L"480", L"360", L""};
				if(quality >= 2 && quality <= 7)
					strpref += L"res:" + std::wstring {quality_res[quality]};
				else if(quality == 0 && conf.pref_res)
					strpref += L"res:" + com_res_options[conf.pref_res];
				if(conf.pref_video && conf.pref_video != 3)
				{
					if(strpref.size() > 5)
						strpref += ',';
					strpref += L"vext:" + com_video_options[conf.pref_video];
				}
				if(!conf.pref_audio && (conf.pref_video == 1 || conf.pref_video == 2))
				{
					// pick audio that fits the preferred container, otherwise yt-dlp can't mux it and falls back to mkv/webm
					if(strpref.size() > 5)
						strpref += ',';
					strpref += conf.pref_video == 1 ? L"aext:m4a" : L"aext:webm";
				}
				if(conf.pref_audio)
				{
					if(strpref.size() > 5)
						strpref += ',';
					strpref += L"aext:" + com_audio_options[conf.pref_audio];
				}
				if(conf.pref_fps)
				{
					if(strpref.size() > 5)
						strpref += ',';
					strpref += L"fps";
				}
				if(conf.pref_vcodec)
				{
					if(strpref.size() > 5)
						strpref += ',';
					strpref += L"vcodec:" + com_vcodec_options[conf.pref_vcodec];
				}
				if(conf.pref_acodec)
				{
					if(strpref.size() > 5)
						strpref += ',';
					strpref += L"acodec:" + com_acodec_options[conf.pref_acodec];
				}
				if(strpref.size() > 5)
					cmd += strpref + L"\" ";
			}
		}

		// make the preferred video container effective: "-S vext:mp4" only *prefers* mp4 streams, the merged
		// file still ends up as mkv/webm unless the container is requested explicitly
		if(!bottom.using_custom_fmt() && (conf.pref_video == 1 || conf.pref_video == 2) &&
			(!bottom.cbargs || bottom.argset.find("--merge-output-format") == -1))
			cmd += conf.pref_video == 1 ? L" --merge-output-format \"mp4/mkv\"" : L" --merge-output-format \"webm/mkv\"";

		if(cmd.back() != ' ')
			cmd.push_back(' ');

		const auto &argset {bottom.argset};
		if(bottom.com_chap == 1 && argset.find("--embed-chapters") == -1)
			cmd += L"--embed-chapters ";
		if(bottom.cbsubs && argset.find("--embed-subs") == -1)
			cmd += L"--embed-subs ";
		if(bottom.cbsubs || argset.find("--embed-subs") != -1 || argset.find("--write-subs") != -1)
		{
			if(!bottom.sub_format.empty())
				cmd += L"--sub-format " + nana::to_wstring(bottom.sub_format) + L" ";
			else if(!conf.sub_format.empty())
				cmd += L"--sub-format " + nana::to_wstring(conf.sub_format) + L" ";
			if(!bottom.sub_langs.empty())
				cmd += L"--sub-langs " + nana::to_wstring(bottom.sub_langs) + L" ";
			else if(!conf.sub_format.empty())
				cmd += L"--sub-langs " + nana::to_wstring(conf.sub_langs) + L" ";
		}
		if(bottom.cbthumb && argset.find("--embed-thumbnail") == -1)
		{
			cmd += L"--embed-thumbnail ";
			// shrink the cover art to fit inside the configured size (keeps the aspect ratio)
			if(conf.embed_thumb_w > 0 && conf.embed_thumb_h > 0 && argset.find("ThumbnailsConvertor") == -1)
			{
				const auto w {std::to_wstring(conf.embed_thumb_w)}, h {std::to_wstring(conf.embed_thumb_h)};
				if(argset.find("--convert-thumbnails") == -1)
					cmd += L"--convert-thumbnails jpg ";
				cmd += L"--postprocessor-args \"ThumbnailsConvertor+ffmpeg_o:-vf scale=" + w + L":" + h + L":force_original_aspect_ratio=decrease\" ";
			}
		}
		if(bottom.cbtime && argset.find("--no-mtime") == -1)
			cmd += L"--no-mtime ";
		if(bottom.cbkeyframes && argset.find("--force-keyframes-at-cuts") == -1)
			cmd += L"--force-keyframes-at-cuts ";
		if(!conf.ffmpeg_path.empty() && conf.ffmpeg_path != conf.ytdlp_path.parent_path())
		{
			if(argset.find("--ffmpeg-location") == -1 && use_ffmpeg_location)
			{
				if(conf.ffmpeg_path != L".\\")
					cmd += L"--ffmpeg-location \"" + conf.ffmpeg_path.wstring() + L"\" ";
			}
		}
		if(tbrate.to_double() && argset.find("-r ") == -1)
			cmd += L"-r " + tbrate.caption_wstring() + (com_rate.option() ? L"M " : L"K ");
		if(bottom.cbmp3 && argset.find("--audio-format") == -1)
		{
			cmd += L"-x --audio-format mp3 ";
			if(argset.find("--audio-quality") == -1) 
				L"--audio-quality 0 ";
		}
		if(bottom.com_chap == 2 && argset.find("--split-chapters") == -1)
			cmd += L"--split-chapters -o chapter:\"" + bottom.outpath.wstring() + L"\\%(title)s - %(section_number)s -%(section_title)s.%(ext)s\" ";
		if(bottom.cbargs && !argset.empty())
			cmd += nana::to_wstring(argset_without_label(argset)) + L" ";
		for(auto &section : bottom.sections)
		{
			const auto first {util::normalize_timestamp(section.first)}, second {util::normalize_timestamp(section.second)};
			if(first.empty() && second.empty())
				continue;
			auto arg
			{
				L"--download-sections *" + (first.empty() ? L"0" : first) + L'-' +
				(second.empty() || second == L"0" || second == L"00:00:00" ? L"inf" : second) + L' '
			};
			cmd += arg;
		}

		if(bottom.is_ytlink || bottom.is_ytchan)
		{
			std::wstring mark, remove;
			if(conf.cb_sblock_mark && !conf.sblock_mark.empty())
			{
				if(conf.sblock_mark.front() == 0)
				{
					mark = L"all";
				}
				else if((sblock_cats_mark.size() - 1) / 2 < conf.sblock_mark.size())
				{
					mark = L"all";
					for(int n{1}; n < sblock_cats_mark.size(); n++)
					{
						if(std::find(conf.sblock_mark.begin(), conf.sblock_mark.end(), n) == conf.sblock_mark.end())
							mark += L",-" + sblock_cats_mark[n];
					}
				}
				else
				{
					for(auto idx : conf.sblock_mark)
						mark += sblock_cats_mark[idx] + L',';
					mark.pop_back();
				}
			}

			if(conf.cb_sblock_remove && !conf.sblock_remove.empty())
			{
				if(conf.sblock_remove.front() == 0)
				{
					remove = L"all";
				}
				else if((sblock_cats_remove.size() - 1) / 2 < conf.sblock_remove.size())
				{
					remove = L"all";
					for(int n {1}; n < sblock_cats_remove.size(); n++)
					{
						if(std::find(conf.sblock_remove.begin(), conf.sblock_remove.end(), n) == conf.sblock_remove.end())
							remove += L",-" + sblock_cats_remove[n];
					}
				}
				else
				{
					for(auto idx : conf.sblock_remove)
						remove += sblock_cats_remove[idx] + L',';
					remove.pop_back();
				}
			}

			if(!mark.empty() && bottom.argset.find("--sponsorblock-mark ") == -1)
				cmd += L"--sponsorblock-mark " + mark + L' ';
			if(!remove.empty() && bottom.argset.find("--sponsorblock-remove ") == -1)
				cmd += L"--sponsorblock-remove " + remove + L' ';
		}

		if(conf.cb_proxy && !conf.proxy.empty() && bottom.argset.find("--proxy ") == -1)
			cmd += L"--proxy " + conf.proxy + L' ';

		auto display_cmd {cmd};

		cmd += L"--encoding UTF-8 --progress-delta .8 ";

		std::wstringstream ss;
		ss << hwnd;
		auto strhwnd {ss.str()};
		strhwnd.erase(0, strhwnd.find_first_not_of('0'));
		if(!bottom.is_ytplaylist && !bottom.is_scplaylist && !bottom.is_gen_playlist && !bottom.is_bcplaylist && !bottom.is_bcchan && !bottom.is_ytchan)
		{
			cmd += L"--exec \"after_video:\\\"" + self_path.wstring() + L"\\\" ytdlp_status " + std::to_wstring(YTDLP_POSTPROCESS)
				+ L" " + strhwnd + L" \\\"" + url + L"\\\"\" ";
			cmd += L"--exec \"before_dl:\\\"" + self_path.wstring() + L"\\\" ytdlp_status " + std::to_wstring(YTDLP_DOWNLOAD)
				+ L" " + strhwnd + L" \\\"" + url + L"\\\"\" ";
		}
		fs::path tempfile;
		if(!bottom.is_ytplaylist && !bottom.is_ytchan && !bottom.is_bcplaylist)
		{
			tempfile = fs::temp_directory_path() / std::tmpnam(nullptr);
			cmd += L"--print-to-file after_move:filepath " + tempfile.wstring();
		}
		std::wstring cmd2;

		auto wstr {bottom.outpath.wstring()};
		if(wstr.find(' ') == -1)
			cmd2 += L" -P " + wstr;
		else cmd2 += L" -P \"" + wstr + L"\"";

		if((!bottom.cbargs || argset.find("-o ") == -1) && !conf.output_template.empty())
		{
			std::wstring folder;
			if(bottom.is_ytplaylist || bottom.is_gen_playlist)
			{
				if(conf.cb_playlist_folder)
					folder = L"%(playlist_title)s\\";
				if(!conf.playlist_indexing.empty())
				{
					if(conf.cb_zeropadding && bottom.playlist_selection.size() > 9)
					{
						auto pos {conf.playlist_indexing.find(L"%(playlist_index)d")};
						if(pos != -1)
						{
							auto str1 {conf.playlist_indexing.substr(0, pos)},
								str2 {conf.playlist_indexing.substr(pos + 18)},
								str_padval {std::to_wstring(std::to_string(bottom.playlist_selection.size()).size())},
								newstr {str1 + L"%(playlist_index)0" + str_padval + L'd' + str2};
							cmd2 += L" -o \"" + folder + newstr + conf.output_template + L'\"';
						}
					}
					else cmd2 += L" -o \"" + folder + conf.playlist_indexing + conf.output_template + L'\"';
				}
				else cmd2 += L" -o \"" + folder + conf.output_template + L'\"';
			}
			else if(bottom.is_bcplaylist || bottom.is_bcchan)
			{
				if(conf.cb_playlist_folder)
					folder = L"%(artist)s\\%(album)s\\";
				cmd2 += L" -o \"" + folder + conf.output_template_bandcamp + L'\"';
			}
			else if(bottom.is_ytchan)
			{
				if(conf.cb_playlist_folder)
					folder = L"%(playlist_title)s\\";
				cmd2 += L" -o \"" + folder + conf.output_template + L'\"';
			}
			else if(bottom.sections.size() > 1)
			{
				if(bottom.outfile.empty())
				{
					cmd2 += L" -o \"" + conf.output_template;
					if(cmd2.rfind(L".%(ext)s") == cmd2.size() - 8)
						cmd2.erase(cmd2.size() - 8);
				}
				else cmd2 += L" -o \"" + bottom.outfile.filename().wstring();
				cmd2 += L" - section %(section_start)d to %(section_end)d.%(ext)s\"";
			}
			else if(bottom.sections.size() == 1)
			{
				// single segment: append the time range to the file name, e.g. "Title [000130-000300].mp4"
				const auto tag {L' ' + util::segment_tag(bottom.sections.front().first, bottom.sections.front().second)};
				std::wstring name;
				if(bottom.outfile.empty())
				{
					name = conf.output_template;
					const std::wstring ext_token {L".%(ext)s"};
					if(name.size() >= ext_token.size() && name.compare(name.size() - ext_token.size(), ext_token.size(), ext_token) == 0)
						name.insert(name.size() - ext_token.size(), tag);
					else name += tag;
				}
				else name = bottom.outfile.filename().wstring() + tag + L".%(ext)s";
				cmd2 += L" -o \"" + name + L'\"';
			}
			else 
			{
				if(bottom.outfile.empty())
					cmd2 += L" -o \"" + conf.output_template + L'\"';
				else cmd2 += L" -o \"" + bottom.outfile.filename().wstring() + L".%(ext)s\"";
			}
		}
		if((bottom.is_ytplaylist || bottom.is_bcplaylist || bottom.is_scplaylist || bottom.is_gen_playlist) && !bottom.playsel_string.empty())
			cmd2 += L" -I " + bottom.playsel_string + L" --compat-options no-youtube-unavailable-videos";
		cmd2 += L" \"" + url + L'\"';
		display_cmd += cmd2;
		cmd += cmd2;
		//display_cmd = cmd;

		if(tbpipe.current() == url)
			tbpipe.clear();

		bottom.dl_thread = std::thread([&, tempfile, display_cmd, cmd, url]
		{
			working = true;
			auto ca {tbpipe.colored_area_access()};
			bool ca_change {false};
			if(outbox.current() == url)
			{
				SendMessage(hwnd, WM_COLORED_AREA_CLEAR, reinterpret_cast<WPARAM>(ca), 0);
				ca_change = true;
			}
			if(fs::exists(conf.ytdlp_path))
				tbpipe.append(url, L"[GUI] executing command line: " + display_cmd + L"\n\n");
			else tbpipe.append(url, L"ytdlp.exe not found: " + conf.ytdlp_path.wstring());
			auto p {ca_change ? ca->get(0) : nullptr};
			if(ca_change)
				p->count = 1;
			if(!fs::exists(conf.ytdlp_path))
			{
				if(ca_change)
					p->fgcolor = theme::is_dark() ? nana::color {"#f99"} : nana::color {"#832"};
				SendMessage(hwnd, WM_REFRESH, reinterpret_cast<WPARAM>(tbpipe.handle()), 0);
				if(qurl == url)
					btndl.caption("Start download");
				bottom.started = false;
				working = false;
				if(bottom.dl_thread.joinable())
					bottom.dl_thread.detach();
				return;
			}
			if(ca_change)
				p->fgcolor = theme::is_dark() ? theme::path_link_fg : nana::color {"#569"};
			SendMessage(hwnd, WM_REFRESH, reinterpret_cast<WPARAM>(tbpipe.handle()), 0);
			ULONGLONG prev_val {0};
			bool playlist_progress {false};

			auto cb_progress = [&, url](ULONGLONG completed, ULONGLONG total, std::string text, int playlist_completed, int playlist_total)
			{
				if(text.find("Unknown B/s") != -1)
					return;

				const bool current {url == qurl};

				if(playlist_total && !playlist_progress)
				{
					playlist_progress = true;
					bottom.prog_amount = playlist_total;
					if(current)
						prog.amount(playlist_total);
				}
				while(text.find_last_of("\r\n") != -1)
					text.pop_back();
				if(total != -1)
				{
					auto strpct {(std::stringstream {} << static_cast<double>(completed) / 10).str() + '%'};
					if(playlist_progress && working)
					{
						auto strprog {"[" + std::to_string(playlist_completed + 1) + "/" + std::to_string(playlist_total) + "] "};
						lbq.set_line_text(url, {"", "", strprog + strpct, "", "", "", ""});
						if(i_taskbar && bottoms.size() == 2)
							i_taskbar->SetProgressValue(hwnd, playlist_completed, playlist_total > 1 ? playlist_total - 1 : playlist_total);
					}
					else if(working)
					{
						lbq.set_line_text(url, {"", "", strpct, "", "", "", ""});
						if(i_taskbar && bottoms.size() == 2)
							i_taskbar->SetProgressValue(hwnd, completed, total);
					}
				}
				if(completed < 1000 && total != -1)
				{
					if(playlist_progress)
						text = "[" + std::to_string(playlist_completed + 1) + " of " + std::to_string(playlist_total) + "]\t" + text;
					if(current)
					{
						thread_local std::string caption;
						caption = text;
						PostMessage(hwnd, WM_PROGEX_CAPTION, reinterpret_cast<WPARAM>(&prog), reinterpret_cast<LPARAM>(&caption));
					}
					bottom.progtext = text;
				}
				else
				{
					if(prev_val)
					{
						if(text == "[ExtractAudio]" || text.find("[Merger]") == 0 || text.find("[Fixup") == 0)
						{
							COPYDATASTRUCT cds;
							cds.dwData = YTDLP_POSTPROCESS;
							cds.cbData = url.size() * 2;
							cds.lpData = const_cast<std::wstring&>(url).data();
							SendMessageW(hwnd, WM_COPYDATA, NULL, reinterpret_cast<LPARAM>(&cds));

							if(text.find("[Merger]") == 0)
							{
								auto pos {text.find('\"')};
								if(pos++ != -1)
								{
									auto pos2 {text.find('\"', pos)};
									if(pos2 != -1)
									{
										try { bottom.merger_path = fs::u8path(text.substr(pos, pos2 - pos)); }
										catch(...) { bottom.merger_path.clear(); }
									}
								}
							}
						}
						else if(total != -1)
						{
							auto pos {text.find_last_not_of(" \t")};
							if(text.size() > pos)
								text.erase(pos + 1);
							if(playlist_progress)
							{
								bottom.progval_shadow = completed;
								bottom.progval = playlist_completed + 1;
								auto strprog {"[" + std::to_string(playlist_completed + 1) + " of " + std::to_string(playlist_total) + "]\t"};
								bottom.progtext = strprog + text;
								if(current)
								{
									prog.shadow_progress(1000, completed);
									prog.value(playlist_completed + 1);
									SendMessage(hwnd, WM_PROGEX_CAPTION, reinterpret_cast<WPARAM>(&prog), reinterpret_cast<LPARAM>(&bottom.progtext));
								}
							}
							else 
							{
								if(current)
									SendMessage(hwnd, WM_PROGEX_CAPTION, reinterpret_cast<WPARAM>(&prog), reinterpret_cast<LPARAM>(&text));
								bottom.progtext = text;
							}
						}
					}
					if(total == -1)
					{
						if(text.find("[download]") == 0)
						{
							auto pos {text.find("has already been downloaded")};
							if(pos != -1)
							{
								try { bottom.download_path = fs::u8path(text.substr(11, pos - 2)); }
								catch(...) { bottom.download_path.clear(); }
							}
							else
							{
								pos = text.find("Destination: ");
								if(pos != -1)
								{
									try { bottom.download_path = fs::u8path(text.substr(24)); }
									catch(...) { bottom.download_path.clear(); }
								}
								else bottom.download_path.clear();
							}
							return;
						}
						else if(playlist_completed == -1 && playlist_total == -1) // aria2c fragmented download
						{
							SendMessage(hwnd, WM_PROGEX_CAPTION, reinterpret_cast<WPARAM>(&prog), reinterpret_cast<LPARAM>(&text));
							return;
						}
					}
				}
				if(playlist_progress)
				{
					if(current)
						prog.shadow_progress(1000, completed);
					bottom.progval_shadow = completed;
				}
				else if(completed <= 1000 && (completed > prog.value() || completed == 0 || prog.value() - completed > 50))
				{
					if(current)
						prog.value(completed);
					bottom.progval = completed;
				}
				prev_val = completed;
			};

			if(url == qurl)
			{
				prog.value(0);
				prog.caption("");
			}
			bottom.progval = 0;
			bottom.progtext = "";
			if(i_taskbar && lbq.item_count() == 1)
				i_taskbar->SetProgressState(hwnd, TBPF_NORMAL);
			lbq.set_line_text(url, {"", "", "started", "", "", "", ""});

			std::string run_output; // everything this run printed (for the error reason and the cleanup)
			auto cb_append = [url, this, &run_output](std::string text, bool keyword)
			{
				if(keyword)
					outbox.set_keyword(text);
				else
				{
					if(run_output.size() < 4'000'000)
						run_output += text;
					outbox.append(url, text);
				}
			};
			bottom.printed_path.clear();
			bottom.merger_path.clear();
			bottom.download_path.clear();
			auto outpath {bottom.outpath};

			{
				std::lock_guard flock {finfo_mtx};
				bottom.finfo = {}; // details of a previous run no longer apply
			}

			// don't start when the folder can't be reached or the video is already there
			std::string skip_reason;
			fs::path existing;
			{
				std::error_code ec;
				const auto absout {fs::absolute(outpath, ec)};
				const auto root {absout.root_path()};
				if(!outpath.empty() && !ec && !root.empty() && !util::path_exists(root))
					skip_reason = "folder";
				else if(media_exists(expected_media_stem(bottom), existing))
					skip_reason = "exists";
			}
			const auto run_start {fs::file_time_type::clock::now()};
			std::string res;
			if(skip_reason.empty())
				res = util::run_piped_process(cmd, &working, cb_append, cb_progress, &graceful_exit, tempfile.filename().string());
			else if(skip_reason == "exists")
			{
				applog::write("download", L"skipped, file already exists: " + existing.wstring() + L" <- " + url);
				tbpipe.append(url, L"[GUI] skipped: the file already exists:\n" + existing.wstring() + L"\n");
			}
			else
			{
				applog::write("download", L"not started, folder not reachable: " + outpath.wstring() + L" <- " + url);
				tbpipe.append(url, L"[GUI] not started: the download folder can't be reached:\n" + outpath.wstring() + L"\n");
			}
			bottom.received_procmsg = false;
			if(!tempfile.empty() && fs::exists(tempfile))
			{
				if(std::ifstream is {tempfile, std::ios::binary | std::ios::ate})
				{
					auto size {is.tellg()};
					std::string str (size, '\0');
					is.seekg(0);
					if(is.read(&str.front(), size))
					{
						while(str.find_last_of("\r\n") != -1)
							str.pop_back();
						bottom.printed_path = fs::u8path(str);
					}
				}
				std::error_code ec;
				fs::remove(tempfile, ec);
			}
			else if((bottom.is_ytplaylist || bottom.is_gen_playlist) && bottom.playlist_info.contains("title") && bottom.playlist_info["title"] != nullptr)
			{
				bottom.printed_path = bottom.outpath / bottom.playlist_info["title"].get<std::string>();
			}
			else if(bottom.is_bcplaylist)
			{
				// todo
			}

			if(working && bottoms.index(url) > 0)
			{
				if(bottom.timer_proc.started())
					bottom.timer_proc.stop();
				if(skip_reason == "exists")
					lbq.set_line_text(url, {"", "", "already downloaded", "", "", "", ""});
				else if(skip_reason == "folder")
				{
					bottom.last_error = "the download folder can't be reached: " + nana::to_utf8(outpath.wstring());
					lbq.set_line_text(url, {"", "", "error: folder not reachable", "", "", "", ""});
				}
				else if(res == "failed")
				{
					// short reason from the last "ERROR:" line, full line kept for the mini log / tooltip
					std::string errline;
					{
						size_t p {run_output.rfind("ERROR:")};
						while(p != std::string::npos && p > 0 && run_output[p - 1] != '\n')
							p = p > 0 ? run_output.rfind("ERROR:", p - 1) : std::string::npos;
						if(p != std::string::npos)
						{
							const auto e {run_output.find('\n', p)};
							errline = run_output.substr(p, e == std::string::npos ? std::string::npos : e - p);
						}
					}
					bottom.last_error = errline;
					const std::string reason {errline.empty() ? std::string {} : util::short_error(errline)};
					applog::write("download", "failed: " + (errline.empty() ? std::string {"(no ERROR line)"} : errline) + " <- " + nana::to_utf8(url));
					lbq.set_line_text(url, {"", "", reason.empty() ? "error" : "error: " + reason, "", "", "", ""});
					try { cleanup_failed_download(bottom, run_output, run_start); }
					catch(...) { applog::write("cleanup", "cleanup failed with an exception"); }
					auto progtext {prog.nana::progress::caption()};
					auto pos1 {progtext.find('[')};
					if(pos1 != -1)
					{
						auto pos2 {progtext.find(" of ")};
						if(pos2 != -1)
						{
							auto pos3 {progtext.find(']')};
							if(pos3 != -1 && pos1 < pos2 && pos2 < pos3)
							{
								auto strval {progtext.substr(++pos1, pos2 - pos1)};
								try { bottom.idx_error = std::stoi(strval); }
								catch(...) { }
							}
						}
					}
				}
				else lbq.set_line_text(url, {"", "", "done", "", "", "", ""});

				// read what the finished (or already existing) file really contains
				if(skip_reason == "exists")
					probe_file_info(bottom, existing);
				else if(skip_reason.empty() && res != "failed")
				{
					fs::path done_file;
					try { done_file = bottom.file_path(); }
					catch(...) {}
					if(!done_file.empty())
						probe_file_info(bottom, done_file);
				}

				taskbar_overall_progress();
				if(i_taskbar && lbq.item_count() == 1)
					i_taskbar->SetProgressState(hwnd, TBPF_NOPROGRESS);
				btndl.enabled(true);
				tbpipe.append(url, "\n[GUI] " + conf.ytdlp_path.filename().string() + " process has exited\n");
				if(tbpipe.current() == url)
				{
					ca = tbpipe.colored_area_access();
					p = ca->get(tbpipe.text_line_count() - 2);
					p->count = 1;
					p->fgcolor = theme::is_dark() ? theme::path_link_fg : nana::color {"#569"};
					SendMessage(hwnd, WM_REFRESH, reinterpret_cast<WPARAM>(tbpipe.handle()), 0);
				}
				if(qurl == url)
					btndl.caption("Start download");
				bottom.started = false;
				if(!start_next_urls(url)) // if no more urls are startable
				{
					if(pwr_shutdown || pwr_hibernate || pwr_sleep)
					{
						auto items_currently_downloading {0};
						for(const auto &bottom : bottoms)
							if(bottom.second->started)
								items_currently_downloading++;
						if(!items_currently_downloading)
							start_suspend_fm = true;
					}
				}
				if(working && bottom.dl_thread.joinable())
					bottom.dl_thread.detach();
				if(close_when_finished)
					close();
			}
		});
		return true; // started
	}
	else
	{
		btndl.enabled(false);
		if(bottom.timer_proc.started())
			bottom.timer_proc.stop();
		if(bottom.dl_thread.joinable())
		{
			working = false;
			bottom.dl_thread.join();
		}
		bottom.received_procmsg = false;
		const auto fname {conf.ytdlp_path.filename().string()};
		if(graceful_exit)
			tbpipe.append(url, "\n[GUI] " + fname + " process was ended gracefully via Ctrl+C signal\n");
		else tbpipe.append(url, "\n[GUI] " + fname + " process was ended forcefully via WM_CLOSE message\n");
		auto item {lbq.item_from_value(url)};
		auto text {item.text(3)};
		auto pos {text.find(']')};
		if(pos != -1)
			lbq.set_line_text(url, {"", "", "stopped (" + text.substr(1, pos - 1) + ")", "", "", "", ""});
		else if(text.find('%') != -1)
			lbq.set_line_text(url, {"", "", "stopped (" + text + ")", "", "", "", ""});
		else lbq.set_line_text(url, {"", "", "stopped", "", "", "", ""});
		if(tbpipe.current() == url)
		{
			auto ca {tbpipe.colored_area_access()};
			auto p {ca->get(tbpipe.text_line_count() - 2)};
			p->count = 1;
			if(graceful_exit)
				p->fgcolor = theme::is_dark() ? theme::path_link_fg : nana::color {"#569"};
			else p->fgcolor = theme::is_dark() ? nana::color {"#f99"} : nana::color {"#832"};
		}
		nana::API::refresh_window(tbpipe);
		btndl.enabled(true);
		if(qurl == url)
			btndl.caption("Start download");
		bottom.started = false;
		if(conf.cb_autostart)
			start_next_urls(url);
		else if(i_taskbar) i_taskbar->SetProgressState(hwnd, TBPF_NOPROGRESS);
		return false; // stopped
	}
}


void GUI::add_url(std::wstring url, bool refresh, bool saveq, const size_t cat)
{
	using namespace nana;

	auto item {lbq.item_from_value(url)};

	if(item == lbq.empty_item || refresh)
	{
		if(refresh)
			lbq.set_line_text(url, {"...", "...", "", "...", "...", "...", "..."});
		else SendMessage(hwnd, WM_LBQ_NEWITEM, cat, reinterpret_cast<LPARAM>(&url));

		auto &bottom {bottoms.add(url)};
		if(!refresh)
		{
			if(lbq.selected().empty())
			{
				SendMessage(hwnd, WM_LBQ_SELECT, reinterpret_cast<LPARAM>(&url), true);
				btndl.enable(true);
			}
		}
		else
		{
			bottom.vidinfo.clear();
			bottom.playlist_info.clear();
		}

		bottom.working_info = true;

		bottom.info_thread = std::thread([&, url, refresh, saveq]
		{
			total_info_threads++;
			bottom.info_thread_id = GetCurrentThreadId();
			const bool has_data {!unfinished_qitems_data.empty() && unfinished_qitems_data.contains(to_utf8(url))};

			while(active_info_threads >= (has_data ? number_of_processors : conf.max_data_threads))
			{
				Sleep(100);
				if(!bottom.working_info)
				{
					total_info_threads--;
					return;
				}
			}
			active_info_threads++;
			bottom.info_thread_active = true;

			auto favicon_url {lbq.favicon_url_from_value(url)};
			if(!favicon_url.empty())
			{
				const auto &working {bottom.working_info};
				std::function<void(paint::image&)> cbfn = [favicon_url, url, &working, this](paint::image &img)
				{
					if(working && !g_exiting && !lbq.force_no_thread_safe_ops())
					{
						auto item {lbq.item_from_value(url)};
						if(item != lbq.empty_item)
						{
							item.value<lbqval_t>().pimg = &img;
							PostMessage(hwnd, WM_REFRESH, reinterpret_cast<WPARAM>(lbq.handle()), 0);
						}
					}
				};
				favicons[favicon_url].add(favicon_url, cbfn);
			}

			if(has_data)
			{
				const auto url8 {to_utf8(url)};
				const auto &j {unfinished_qitems_data[url8]};
				bottom.from_json(j);
				auto item {lbq.item_from_value(url)};
				if(item != lbq.empty_item && item.selected())
					bottoms.show(url);
				if(j.contains("columns"))
				{
					auto item {lbq.item_from_value(url)};
					auto website {j["columns"]["website"].get<std::string>()};
					std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> u16conv;
					auto media_title {bottom.media_title};
					if(conf.cb_display_custom_filenames && !bottom.outfile.empty())
						media_title = bottom.outfile.filename().string();
					else if(!is_utf8(media_title))
					{
						std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> u16conv;
						auto u16str {u16conv.from_bytes(media_title)};
						std::wstring wstr(u16str.size(), L'\0');
						memcpy(&wstr.front(), &u16str.front(), wstr.size() * 2);
						std::wstring_convert<std::codecvt_utf8<wchar_t>> u8conv;
						media_title = u8conv.to_bytes(wstr);
					}
					if(media_title.empty())
					{
						if(j["columns"].contains("title"))
							media_title = j["columns"]["title"].get<std::string>();
						else media_title = "!!! failed to restore the media title !!!";
					}
					auto status {j["columns"]["status"].get<std::string>()};
					auto format {j["columns"]["format"].get<std::string>()},
						format_note {j["columns"]["format_note"].get<std::string>()},
						ext = j["columns"]["ext"].get<std::string>(),
						fsize = j["columns"]["fsize"].get<std::string>();
					lbq.set_line_text(url, {website, media_title, status, format, format_note, ext, fsize}, false);
				}
				unfinished_qitems_data.erase(url8);
			}
			else if(bottoms.contains(url))
			{
				if(total_info_threads > active_info_threads)
					lbq.set_item_bg(url, ::widgets::theme::list_check_highlight_bg);
				std::wstring fmt_sort {' '}, strpref {L" -S \""}, cookies;
				if(conf.pref_res)
					strpref += L"res:" + com_res_options[conf.pref_res];
				if(conf.pref_video)
				{
					if(strpref.size() > 5)
						strpref += ',';
					strpref += L"vext:" + com_video_options[conf.pref_video];
				}
				if(conf.pref_audio)
				{
					if(strpref.size() > 5)
						strpref += ',';
					strpref += L"aext:" + com_audio_options[conf.pref_audio];
				}
				if(conf.pref_fps)
				{
					if(strpref.size() > 5)
						strpref += ',';
					strpref += L"fps";
				}
				if(conf.pref_vcodec)
				{
					if(strpref.size() > 5)
						strpref += ',';
					strpref += L"vcodec:" + com_vcodec_options[conf.pref_vcodec];
				}
				if(conf.pref_acodec)
				{
					if(strpref.size() > 5)
						strpref += ',';
					strpref += L"acodec:" + com_acodec_options[conf.pref_acodec];
				}
				if(strpref.size() > 5)
					fmt_sort = strpref + L"\" ";

				if(conf.com_cookies)
					cookies = L" --cookies-from-browser " + com_cookies_options[conf.com_cookies] + nana::to_wstring(conf.cookie_options) + L" ";
				else if(conf.cb_cookies && !conf.cookies_path.empty())
					cookies = L" --cookies \"" + conf.cookies_path.wstring() + L"\" ";

				std::string media_info, media_website {"---"}, media_title, format_id {"---"}, format_note {"---"}, ext {"---"}, filesize {"---"};
				auto json_error = [&](const nlohmann::detail::exception &e)
				{
					media_title = "Can't parse the JSON data produced by yt-dlp! See output for details.";
					lbq.set_line_text(url, {"", "", "error", "", "", "", ""});
					outbox.caption(e.what() + std::string {"\n\n"} + media_info, url);
					if(outbox.current() == url)
					{
						auto ca {outbox.colored_area_access()};
						//ca->clear();
						SendMessage(hwnd, WM_COLORED_AREA_CLEAR, reinterpret_cast<WPARAM>(ca), 0);
						auto p {ca->get(0)};
						p->fgcolor = ::widgets::theme::is_dark() ? ::widgets::theme::path_link_fg : color {"#569"};
					}
					if(!queue_panel.visible() && overlay.visible())
					{
						outbox.widget::show();
						overlay.hide();
					}
				};

				if(bottom.is_ytlink && !bottom.is_ytchan || bottom.is_bcplaylist || bottom.is_scplaylist || bottom.is_gen_playlist)
				{
					std::error_code ec;
					std::wstring qjs;
					if(bottom.is_ytlink || bottom.is_ytplaylist)
						qjs = fs::exists(conf.ytdlp_path.parent_path() / "qjs.exe", ec) ? L"--js-runtimes quickjs " : L"";
					if(bottom.is_ytplaylist || bottom.is_bcplaylist || bottom.is_scplaylist || bottom.is_gen_playlist)
					{
						std::wstring flat {bottom.is_scplaylist ? L"" : L"--flat-playlist "};
						std::wstring compat_options {bottom.is_ytplaylist ? L" --compat-options no-youtube-unavailable-videos" : L""},
							cmd {L" --no-warnings " + qjs + cookies + flat + L"-J " + compat_options + L" \"" + url + L'\"'};
						if(conf.cb_proxy && !conf.proxy.empty())
							cmd = L" --proxy " + conf.proxy + cmd;
						bottom.cmdinfo = conf.ytdlp_path.filename().wstring() + cmd;
						media_info = util::run_piped_process(L'\"' + conf.ytdlp_path.wstring() + L'\"' + cmd, &bottom.working_info);
						if(bottoms.contains(url))
						{
							if(!bottom.working_info)
							{
								bottom.vidinfo.clear();
								bottom.playlist_info.clear();
								lbq.set_item_bg(url, ::widgets::theme::lbbg);
								total_info_threads--;
								active_info_threads--;
								bottom.info_thread_active = false;
								return;
							}
							bool incomplete_data_received {false};
							if(media_info.starts_with("ERROR: Incomplete data received"))
							{
								auto pos {media_info.find('{')};
								if(pos != -1)
								{
									media_info.erase(0, pos);
									incomplete_data_received = true;
								}
							}
							else if(media_info.starts_with("ERROR: "))
							{
								auto pos {media_info.find('{')};
								if(pos != -1)
									media_info.erase(pos);
							}
							if(!media_info.empty() && media_info.front() == '{')
							{
								try { bottom.playlist_info = nlohmann::json::parse(media_info); }
								catch(nlohmann::detail::exception e)
								{
									bottom.playlist_info.clear();
									if(bottoms.contains(url))
										json_error(e);
								}
								if(!bottom.playlist_info.empty())
								{
									if(incomplete_data_received)
									{
										auto it {bottom.playlist_info["entries"].end()};
										bottom.playlist_info["entries"].erase(--it);
									}
									const auto &jpl {bottom.playlist_info["entries"]};
									if(jpl.size() == 0)
									{
										lbq.set_line_text(url, {"---", "error: playlist with zero entries!", "---", "---", "---", "---"});
										bottom.vidinfo.clear();
										bottom.playlist_info.clear();
										lbq.set_item_bg(url, ::widgets::theme::lbbg);
										total_info_threads--;
										active_info_threads--;
										return;
									}
									std::string URL {jpl[0]["url"]};
									cmd = L" --no-warnings -j " + qjs + cookies + fmt_sort + to_wstring(URL);
									media_info = {util::run_piped_process(L'\"' + conf.ytdlp_path.wstring() + L'\"' + cmd, &bottom.working_info)};
									if(!bottom.working_info)
									{
										bottom.vidinfo.clear();
										bottom.playlist_info.clear();
										lbq.set_item_bg(url, ::widgets::theme::lbbg);
										total_info_threads--;
										active_info_threads--;
										return;
									}
									if(!media_info.empty() && media_info.front() == '{')
									{
										try { bottom.vidinfo = nlohmann::json::parse(media_info); }
										catch(nlohmann::detail::exception e)
										{
											bottom.vidinfo.clear();
											if(bottoms.contains(url))
												json_error(e);
										}
										if(!bottom.vidinfo.empty() && qurl == url)
											show_btnfmt(true);
									}
									else bottom.playlist_vid_cmdinfo = conf.ytdlp_path.filename().wstring() + cmd;
								}
							}
						}
					}
					else // YouTube video
					{
						std::wstring cmd {L" --no-warnings -j " + qjs + cookies + (conf.output_template.empty() ? L"" : L"-o \"" +
							conf.output_template + L'\"') + fmt_sort + L'\"' + url + L'\"'};
						if(conf.cb_proxy && !conf.proxy.empty())
							cmd = L" --proxy " + conf.proxy + cmd;
						bottom.cmdinfo = conf.ytdlp_path.filename().wstring() + cmd;
						media_info = util::run_piped_process(L'\"' + conf.ytdlp_path.wstring() + L'\"' + cmd, &bottom.working_info);
						if(bottoms.contains(url))
						{
							if(!bottom.working_info)
							{
								bottom.vidinfo.clear();
								bottom.playlist_info.clear();
								lbq.set_item_bg(url, ::widgets::theme::lbbg);
								total_info_threads--;
								active_info_threads--;
								bottom.info_thread_active = false;
								return;
							}
							if(media_info.find("ERROR:") == 0)
							{
								auto pos {media_info.find("This live event will begin in ")};
								if(pos != -1)
								{
									auto strtime {media_info.substr(pos + 30, media_info.rfind('.') - pos - 30)};
									lbq.set_line_text(url, {"youtube.com", "[live event scheduled to begin in " + strtime + ']', "---", "---", "---", "---"});
									bottom.vidinfo.clear();
									if(--active_info_threads == 0)
										queue_save();
									if(bottom.working_info && bottom.info_thread.joinable())
										bottom.info_thread.detach();
									lbq.set_item_bg(url, ::widgets::theme::lbbg);
									total_info_threads--;
									active_info_threads--;
									bottom.info_thread_active = false;
									if(bottom.info_thread.joinable())
										bottom.info_thread.detach();
									return;
								}
							}
							auto pos {media_info.rfind('}')};
							if(pos != -1)
								media_info.erase(pos + 1);
							if(bottom.working_info)
							{
								auto pos {media_info.find("{\"id\":")};
								if(pos != -1)
								{
									media_info.erase(0, pos);
									try { bottom.vidinfo = nlohmann::json::parse(media_info); }
									catch(nlohmann::detail::exception e)
									{
										bottom.vidinfo.clear();
										if(bottoms.contains(url))
											json_error(e);
									}
								}
							}
						}
					}
				}
				else if(bottom.is_ytchan || bottom.is_bcchan)
				{
					std::wstring cmd {L" --no-warnings --flat-playlist -I :0 -J " + cookies + L"\"" + url + L'\"'};
					if(bottom.is_ytplaylist)
						cmd = L" --no-warnings --flat-playlist -J " + cookies + L"\"" + url + L'\"';
					if(conf.cb_proxy && !conf.proxy.empty())
						cmd += L" --proxy " + conf.proxy + cmd;
					bottom.cmdinfo = conf.ytdlp_path.filename().wstring() + cmd;
					media_info = util::run_piped_process(L'\"' + conf.ytdlp_path.wstring() + L'\"' + cmd, &bottom.working_info);
					if(bottoms.contains(url))
					{
						if(!bottom.working_info)
						{
							bottom.vidinfo.clear();
							bottom.playlist_info.clear();
							lbq.set_item_bg(url, ::widgets::theme::lbbg);
							total_info_threads--;
							active_info_threads--;
							bottom.info_thread_active = false;
							return;
						}
						try { bottom.playlist_info = nlohmann::json::parse(media_info); }
						catch(nlohmann::detail::exception e)
						{
							bottom.playlist_info.clear();
							if(bottoms.contains(url))
								json_error(e);
						}
						if(!bottom.playlist_info.empty())
						{
							std::string tab {bottom.is_bcchan ? "[user page] " : "[whole channel] "};
							std::vector<std::string> tabs {"videos", "featured", "playlists", "shorts", "streams", "community", "podcasts"};
							if(bottom.is_ytchan)
								for(const auto &t : tabs)
									if(url.rfind(L"/" + to_wstring(t)) == url.size() - t.size() - 1)
									{
										tab = "[channel tab] ";
										if(t == "videos" || t == "shorts" || t == "streams" || t == "podcasts")
											bottom.is_yttab = true;
										break;
									}
							auto media_title {bottom.playlist_info["title"].get<std::string>()};
							bottom.media_title = media_title;

							if(!is_utf8(media_title))
							{
								std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> u16conv;
								auto u16str {u16conv.from_bytes(media_title)};
								std::wstring wstr(u16str.size(), L'\0');
								memcpy(&wstr.front(), &u16str.front(), wstr.size() * 2);
								std::wstring_convert<std::codecvt_utf8<wchar_t>> u8conv;
								media_title = u8conv.to_bytes(wstr);
							}

							if(bottom.is_ytplaylist || bottom.is_gen_playlist)
							{
								std::string URL {bottom.playlist_info["entries"][0]["url"]};
								cmd = L" --no-warnings -j " + cookies + fmt_sort + to_wstring(URL);
								media_info = {util::run_piped_process(L'\"' + conf.ytdlp_path.wstring() + L'\"' + cmd, &bottom.working_info)};
								if(!bottom.working_info)
								{
									bottom.vidinfo.clear();
									bottom.playlist_info.clear();
									lbq.set_item_bg(url, ::widgets::theme::lbbg);
									total_info_threads--;
									active_info_threads--;
									bottom.info_thread_active = false;
									return;
								}
								if(!media_info.empty() && media_info.front() == '{')
								{
									try { bottom.vidinfo = nlohmann::json::parse(media_info); }
									catch(nlohmann::detail::exception e)
									{
										bottom.vidinfo.clear();
										if(bottoms.contains(url))
											json_error(e);
									}
									if(!bottom.vidinfo.empty() && qurl == url)
										show_btnfmt(true);
								}
								else bottom.playlist_vid_cmdinfo = conf.ytdlp_path.filename().wstring() + cmd;
							}
							else
							{
								lbq.set_line_text(url, {bottom.is_bcchan ? "bandcamp.com" : "youtube.com", tab + media_title, "", "---", "---", "---", "---"});
								bottom.vidinfo.clear();
								if(vidsel_item.m && lbq.item_from_value(url).selected())
								{
									auto &m {*vidsel_item.m};
									for(int n {0}; n < m.size(); n++)
									{
										m.enabled(n, true);
										if(m.text(n) == "Select formats" && !bottom.btnfmt_visible())
											m.erase(n--);
									}
									SendMessage(hwnd, WM_REFRESH, reinterpret_cast<WPARAM>(m.handle()), 0);
									vidsel_item.m = nullptr;
								}

								if(--active_info_threads == 0)
									queue_save();
								if(bottom.working_info && bottom.info_thread.joinable())
									bottom.info_thread.detach();
								lbq.set_item_bg(url, ::widgets::theme::lbbg);
								total_info_threads--;
								bottom.info_thread_active = false;
								if(bottom.info_thread.joinable())
									bottom.info_thread.detach();
								return;
							}
						}
					}
				}
				else // not YouTube
				{
					std::wstring fsize_approx;
					if(ver_ytdlp > version_t {2023, 11, 13})
						fsize_approx = L" --compat-options manifest-filesize-approx";
					std::wstring cmd {L" --no-warnings" + cookies + fsize_approx + L" -J --flat-playlist " + 
						(conf.output_template.empty() ? L"" : L"-o \"" + conf.output_template + L'\"') + fmt_sort + L'\"' + url + L'\"'};
					if(conf.cb_proxy && !conf.proxy.empty())
						cmd = L" --proxy " + conf.proxy + cmd;
					bottom.cmdinfo = conf.ytdlp_path.filename().wstring() + cmd;
					media_info = util::run_piped_process(L'\"' + conf.ytdlp_path.wstring() + L'\"' + cmd, &bottom.working_info);
					if(!bottom.working_info)
					{
						bottom.vidinfo.clear();
						bottom.playlist_info.clear();
						if(!g_exiting)
							lbq.set_item_bg(url, ::widgets::theme::lbbg);
						total_info_threads--;
						active_info_threads--;
						bottom.info_thread_active = false;
						return;
					}
					auto pos {media_info.rfind('}')};
					if(pos != -1)
						media_info.erase(pos + 1);
					if(bottom.working_info)
					{
						auto pos {media_info.find('{')};
						if(pos != -1)
						{
							media_info.erase(0, pos);
							try { bottom.vidinfo = nlohmann::json::parse(media_info); }
							catch(nlohmann::detail::exception e)
							{
								bottom.vidinfo.clear();
								if(bottoms.contains(url))
									json_error(e);
							}
							if(!bottom.vidinfo.empty() && bottom.vidinfo_contains("_type"))
							{
								if(bottom.vidinfo["_type"] == "playlist")
								{
									bottom.is_gen_playlist = true;
									bottom.playlist_info = std::move(bottom.vidinfo);
									const auto &jpl {bottom.playlist_info["entries"]};
									if(jpl.size() == 0)
									{
										lbq.set_line_text(url, {"---", "error: playlist with zero entries!", "---", "---", "---", "---"});
										bottom.vidinfo.clear();
										bottom.playlist_info.clear();
										lbq.set_item_bg(url, ::widgets::theme::lbbg);
										total_info_threads--;
										active_info_threads--;
										return;
									}
									std::string URL {jpl[0]["url"]};
									cmd = L" --no-warnings -j "+ cookies + fmt_sort + to_wstring(URL);
									media_info = {util::run_piped_process(L'\"' + conf.ytdlp_path.wstring() + L'\"' + cmd, &bottom.working_info)};
									if(!bottom.working_info)
									{
										bottom.vidinfo.clear();
										bottom.playlist_info.clear();
										lbq.set_item_bg(url, ::widgets::theme::lbbg);
										total_info_threads--;
										active_info_threads--;
										return;
									}
									if(!media_info.empty() && media_info.front() == '{')
									{
										try { bottom.vidinfo = nlohmann::json::parse(media_info); }
										catch(nlohmann::detail::exception e)
										{
											bottom.vidinfo.clear();
											if(bottoms.contains(url))
												json_error(e);
										}
										if(!bottom.vidinfo.empty() && qurl == url)
											show_btnfmt(true);
									}
									else bottom.playlist_vid_cmdinfo = conf.ytdlp_path.filename().wstring() + cmd;
								}
							}
						}
					}
				}
				if(!g_exiting && bottom.working_info && bottoms.contains(url))
				{
					if(!media_info.empty())
					{
						if(media_info[0] == '{')
						{
							if(bottom.is_ytplaylist || bottom.is_bcplaylist || bottom.is_scplaylist || bottom.is_gen_playlist)
							{
								if(!bottom.playlist_info.empty())
								{
									auto playlist_size {bottom.playlist_info["entries"].size()};
									if(bottom.playsel_string.size())
										bottom.apply_playsel_string();
									else bottom.playlist_selection.assign(playlist_size, true);
									if(bottom.is_bcplaylist)
										media_website = "bandcamp.com";
									else media_website = bottom.playlist_info["webpage_url_domain"];
									if(bottom.is_yttab)
										media_title = "[channel tab] " + bottom.playlist_info["title"].get<std::string>();
									else 
									{
										if(bottom.playlist_info.contains("title"))
											media_title = "[playlist] " + bottom.playlist_info["title"].get<std::string>();
										else if(bottom.playlist_info.contains("id"))
											media_title = "[playlist] " + bottom.playlist_info["id"].get<std::string>();
										else media_title = to_utf8(url);
									}
									if(vidsel_item.m && lbq.item_from_value(url).selected())
									{
										auto &m {*vidsel_item.m};
										auto pos {vidsel_item.pos};
										auto str {std::to_string(playlist_size)};
										if(bottom.playsel_string.empty())
											m.text(pos, (bottom.is_ytplaylist ? "Select videos (" : "Select songs (") + str + '/' + str + ")");
										else m.text(pos, (bottom.is_ytplaylist ? "Select videos (" : "Select songs (") + std::to_string(bottom.playlist_selected()) + '/' + str + ")");
										m.enabled(pos, true);
										m.enabled(pos + 1, true);
										m.enabled(pos + 2, true);
										m.enabled(pos + 3, true);
										SendMessage(hwnd, WM_REFRESH, reinterpret_cast<WPARAM>(m.handle()), 0);
										vidsel_item.m = nullptr;
									}
									for(const auto &entry : bottom.playlist_info["entries"])
									{
										if(entry.contains("id") && entry["id"] != nullptr)
											outbox.set_keyword(entry["id"].get<std::string>() + ':', "id");
									}
								}
							}
							else if(!bottom.vidinfo.empty())
							{
								if(bottom.is_bclink)
									media_website = "bandcamp.com";
								else if(bottom.vidinfo_contains("webpage_url_domain"))
									media_website = bottom.vidinfo["webpage_url_domain"];
								if(bottom.vidinfo_contains("title"))
									media_title = bottom.vidinfo["title"];
								if(bottom.vidinfo_contains("is_live") && bottom.vidinfo["is_live"] ||
									bottom.vidinfo_contains("live_status") && bottom.vidinfo["live_status"] == "is_live")
									media_title = "[live] " + media_title;
								if(bottom.vidinfo_contains("format_id"))
									format_id = bottom.vidinfo["format_id"];
								if(bottom.vidinfo_contains("resolution"))
									format_note = bottom.vidinfo["resolution"];
								else if(bottom.vidinfo_contains("format_note"))
									format_note = bottom.vidinfo["format_note"];
								if(bottom.vidinfo_contains("ext"))
									ext = bottom.vidinfo["ext"];
								if(bottom.vidinfo_contains("filesize"))
								{
									auto fsize {bottom.vidinfo["filesize"].get<std::uint64_t>()};
									filesize = util::int_to_filesize(fsize, false);
								}
								else if(bottom.vidinfo_contains("filesize_approx"))
								{
									auto fsize {bottom.vidinfo["filesize_approx"].get<std::uint64_t>()};
									if(bottom.vidinfo_contains("requested_formats"))
									{
										auto &reqfmt {bottom.vidinfo["requested_formats"]};
										if(reqfmt.size() == 2)
										{
											if(reqfmt[0].contains("filesize") && reqfmt[1].contains("filesize"))
												filesize = '~' + util::int_to_filesize(fsize, false);
											else filesize = "---";
										}
									}
									else filesize = '~' + util::int_to_filesize(fsize, false);
								}

								if(!bottom.vidinfo.empty() && qurl == url)
									show_btnfmt(true);
							}
							bottom.media_title = media_title;
							if(!is_utf8(media_title))
							{
								std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> u16conv;
								auto u16str {u16conv.from_bytes(media_title)};
								std::wstring wstr(u16str.size(), L'\0');
								memcpy(&wstr.front(), &u16str.front(), wstr.size() * 2);
								std::wstring_convert<std::codecvt_utf8<wchar_t>> u8conv;
								media_title = u8conv.to_bytes(wstr);
							}

							if(!g_exiting)
							{
								lbq.set_line_text(url, {media_website, media_title, "", format_id, format_note, ext, filesize});

								if(bottom.vidinfo_contains("id"))
								{
									std::string idstr {bottom.vidinfo["id"]};
									outbox.set_keyword(idstr + ':', "id");
								}
							}
						}
						else
						{
							lbq.set_line_text(url, {" ", "yt-dlp failed to get info (see output)", " ", " ", " ", " ", " "});

							auto cmdline {bottom.playlist_vid_cmdinfo.empty() ? to_utf8(bottom.cmdinfo) : to_utf8(bottom.playlist_vid_cmdinfo)};
							outbox.caption("[GUI] got error executing command line: " + cmdline + "\n\n" + media_info + "\n", url);
							if(outbox.current() == url)
							{
								auto ca {outbox.colored_area_access()};
								//ca->clear();
								SendMessage(hwnd, WM_COLORED_AREA_CLEAR, reinterpret_cast<WPARAM>(ca), 0);
								auto p {ca->get(0)};
								p->fgcolor = ::widgets::theme::is_dark() ? ::widgets::theme::path_link_fg : color {"#569"};
							}
							if(!queue_panel.visible() && overlay.visible())
							{
								outbox.widget::show();
								overlay.hide();
							}
							if(vidsel_item.m && lbq.item_from_value(url).selected())
							{
								auto &m {*vidsel_item.m};
								for(int n {0}; n < m.size(); n++)
								{
									if(!m.enabled(n))
										m.erase(n--);
								}
								for(int n {0}; n < m.size(); n++)
								{
									if(m.text(n).empty() && n < m.size() - 1 && m.text(n + 1).empty())
									{
										m.erase(n);
										break;
									}
								}
								SendMessage(hwnd, WM_REFRESH, reinterpret_cast<WPARAM>(m.handle()), 0);
								vidsel_item.m = nullptr;
							}
						}
					}
					else if(bottom.working_info && bottom.info_thread.joinable())
					{
						if(conf.ytdlp_path.empty() || !fs::exists(conf.ytdlp_path))
							lbq.set_line_text(url, {"", "can't get data for URL, yt-dlp.exe missing!", "", "", "", "", ""});
						else lbq.set_line_text(url, {"", "yt-dlp did not provide any data for this URL!", "", "", "", "", ""});
					}
				}
				if(!g_exiting)
				{
					auto item {lbq.item_from_value(url)};
					if(vidsel_item.m && item != lbq.empty_item && item.selected())
					{
						const auto &vidinfo {bottom.vidinfo};
						auto &m {*vidsel_item.m};
						for(int n {0}; n < m.size(); n++)
						{
							m.enabled(n, true);
							if(m.text(n) == "Select formats" && !bottom.btnfmt_visible())
								m.erase(n--);
							if(m.text(n) == "Select subtitles")
							{
								if(!vidinfo.empty() && vidinfo.contains("subtitles") && vidinfo.at("subtitles").is_object())
								{
									try
									{
										const auto &jsubs {vidinfo.at("subtitles")};
										int subcount {0};
										for(auto it {jsubs.begin()}; it != jsubs.end(); it++)
											if(it.key() != "live_chat" && it->is_array() && it->size() && it->front().contains("name"))
												subcount++;
										m.text(n, "Select subtitles (0/" + std::to_string(subcount) + ')');
									}
									catch(...) {};
								}
								else (m.text(n, "Select subtitles (0/0)"));
							}
						}
						SendMessage(hwnd, WM_REFRESH, reinterpret_cast<WPARAM>(m.handle()), 0);
						vidsel_item.m = nullptr;
					}
				}
			}
			if(--active_info_threads == 0)
			{
				if(saveq) save_queue = true;
				if(!items_initialized)
				{
					if(!g_exiting)
					{
						SendMessage(hwnd, WM_LBQ_SKIP, 0, 0);
						lbq.auto_draw(true);
					}
					items_initialized = true;
				}
			}
			if(!g_exiting)
			{
				auto item {lbq.item_from_value(url)};
				if(item != lbq.empty_item)
				{
					lbq.set_item_bg(url, ::widgets::theme::lbbg);
					if(bottom.working_info && bottom.info_thread.joinable())
						bottom.info_thread.detach();
				}
			}
			total_info_threads--;
			bottom.info_thread_active = false;
			if(!g_exiting && bottom.working_info && bottom.info_thread.joinable())
			{
				bottom.info_thread.detach();
			}
		});
	}
}


bool GUI::apply_theme(bool dark)
{
	using widgets::theme;

	if(dark) 
	{
		if(conf.cb_custom_dark_theme)
		{
			conf.theme_dark.contrast(conf.contrast, true);
			theme::make_dark(conf.theme_dark);
		}
		else
		{
			theme_t def {true};
			def.contrast(conf.contrast, true);
			theme::make_dark(def);
		}
	}
	else 
	{
		if(conf.cb_custom_light_theme)
		{
			conf.theme_light.contrast(conf.contrast, false);
			theme::make_light(conf.theme_light);
		}
		else
		{
			theme_t def;
			def.contrast(conf.contrast, false);
			theme::make_light(def);
		}
	}

	const auto &text {outbox.current_buffer()};
	if(!text.empty())
	{
		auto ca {outbox.colored_area_access()};
		ca->clear();
		auto p {ca->get(0)};
		if(text.starts_with("[GUI]"))
		{
			p->count = 1;
			p->fgcolor = theme::is_dark() ? theme::path_link_fg : nana::color {"#569"};
		}
		if(text.rfind("[GUI] ") != 0)
		{
			p = ca->get(outbox.text_line_count() - 2);
			p->count = 1;
			if(text.rfind("WM_CLOSE") == -1)
				p->fgcolor = widgets::theme::is_dark() ? widgets::theme::path_link_fg : nana::color {"#569"};
			else p->fgcolor = widgets::theme::is_dark() ? nana::color {"#f99"} : nana::color {"#832"};
		}
		nana::API::refresh_window(outbox);
	}

	bgcolor(theme::fmbg);
	refresh_widgets();
	lbq.refresh_theme();
	tgrid.refresh();

	return false; // don't refresh widgets a second time, already done once in this function
}


void GUI::show_queue(bool freeze_redraw)
{
	if(freeze_redraw)
		SendMessage(hwnd, WM_SETREDRAW, FALSE, 0);
	change_field_attr("Bottom", "weight", 48);
	auto &plc {get_place()};
	btnq.caption("Show output");
	plc.field_display("prog", false);
	plc.field_display("separator", true);
	plc.field_display("minilog", true);
	plc.field_display("minilog_spacer", true);
	plc.collocate();
	update_minilog(true);
	outbox.highlight(false);
	outbox.hide();
	overlay.hide();
	queue_panel.show();
	if(freeze_redraw)
		SendMessage(hwnd, WM_SETREDRAW, TRUE, 0);
	SendMessage(hwnd, WM_REFRESH, reinterpret_cast<WPARAM>(handle()), 0);
}


void GUI::show_output()
{
	SendMessage(hwnd, WM_SETREDRAW, FALSE, 0);
	change_field_attr("Bottom", "weight", 78);
	btnq.caption("Show queue");
	auto &curbot {bottoms.current()};
	auto &plc {get_place()};
	plc.field_display("prog", true);
	plc.field_display("separator", false);
	plc.field_display("minilog", false); // the full log is showing
	plc.field_display("minilog_spacer", false);
	plc.collocate();
	queue_panel.hide();
	outbox.show(curbot.url);
	outbox.highlight(conf.kwhilite);
	outbox.enable_caret();
	SendMessage(hwnd, WM_SETREDRAW, TRUE, 0);
}


void GUI::get_releases(nana::window parent_for_msgbox)
{
	thr_releases = std::thread {[parent_for_msgbox, this]
	{
		using json = nlohmann::json;
		auto jtext {util::get_inet_res("https://api.github.com/repos/ErrorFlynn/ytdlp-interface/releases", &inet_error)};
		if(!jtext.empty())
		{
			try { releases = json::parse(jtext); }
			catch(nlohmann::detail::exception e)
			{
				releases.clear();
				tmsg_parent = parent_for_msgbox;
				tmsg_title = "69-ytdlp-interface JSON error";
				std::string str;
				if(jtext.size() > 600)
					str = " (showing the first 600 characters)";
				tmsg_text = "Got an unexpected response when checking GitHub for a new version" + str + ":\n\n" + jtext.substr(0, 600) +
					"\n\nError from the JSON parser:\n\n" + e.what();
				if(thr_releases.joinable())
					thr_releases.detach();
				return;
			}
			if(releases.is_array())
			{
				if(releases[0].contains("tag_name"))
				{					
					std::string tag_name {releases[0]["tag_name"]};
					if(tag_name.size() < 4)
					{
						releases.clear();
						tmsg_parent = parent_for_msgbox;
						tmsg_title = "ytdlp-interface error";
						tmsg_text = std::string {"Got an unexpected response when checking GitHub for a new version. "} +
							"The response is valid JSON and is properly formatted, but the value of the key \"tag_name\" " +
							"for the latest release is less that 4 characters in length: \"" + tag_name + "\"";						
					}
					else if(is_tag_a_new_version(tag_name) && conf.get_releases_at_startup)
						caption(title + "   (" + tag_name + " is available)");
				}
				else
				{
					tmsg_parent = parent_for_msgbox;
					tmsg_title = "ytdlp-interface error";
					std::string str;
					if(jtext.size() > 600)
					{
						jtext.erase(600);
						str = " (showing the first 600 characters)";
					}
					tmsg_text = "Got an unexpected response when checking GitHub for a new version" + str + ":\n\n" + jtext +
						"\n\nThe response is valid JSON and is formatted as an array (as expected), but the first element " +
						"does not contain the key \"tag_name\".";
					releases.clear();
				}
			}
			else
			{
				tmsg_parent = parent_for_msgbox;
				tmsg_title = "ytdlp-interface error";
				std::string str;
				if(jtext.size() > 600)
				{
					jtext.erase(600);
					str = " (showing the first 600 characters)";
				}
				tmsg_text = "Got an unexpected response when checking GitHub for a new version" + str + ":\n\n" + jtext +
					"\n\nThe response is valid JSON, but is not formatted as an array.";
				releases.clear();
			}
		}
		if(thr_releases.joinable())
			thr_releases.detach();
	}};
}


void GUI::get_latest_ffmpeg(nana::window parent_for_msgbox)
{
	using json = nlohmann::json;

	thr_releases_ffmpeg = std::thread {[=]
	{
		std::string jtext;
		if(win7)
			jtext = util::get_inet_res("https://api.github.com/repos/kusaanko/FFmpeg-Auto-Build/releases/latest", &inet_error);
		else jtext = util::get_inet_res("https://api.github.com/repos/yt-dlp/FFmpeg-Builds/releases/latest", &inet_error);
		if(!jtext.empty())
		{
			json json_ffmpeg;
			try { json_ffmpeg = json::parse(jtext); }
			catch(nlohmann::detail::exception e)
			{
				tmsg_parent = parent_for_msgbox;
				tmsg_title = "ytdlp-interface error";
				std::string str;
				if(jtext.size() > 600)
				{
					jtext.erase(600);
					str = " (showing the first 600 characters)";
				}
				tmsg_text = "Got an unexpected response when checking GitHub for a new FFmpeg version" + str + ":\n\n" + jtext +
					"\n\nError from the JSON parser:\n\n" + e.what();
				if(thr_releases_ffmpeg.joinable())
					thr_releases_ffmpeg.detach();
				return;
			}
			if(!json_ffmpeg.empty() && json_ffmpeg.contains("assets"))
			{
				url_latest_ffmpeg.clear();
				for(auto &el : json_ffmpeg["assets"])
				{
					std::string url {el["browser_download_url"]};
					if(win7 && url.contains(X64 ? "/win64_win64_" : "/win32_win32_") || 
						url.contains(X64 ? "ffmpeg-master-latest-win64-gpl.zip" : "ffmpeg-master-latest-win32-gpl.zip") ||
						url.contains(X64 ? "win64-gpl.zip" : "win32-gpl.zip"))
					{
						url_latest_ffmpeg = url;
						size_latest_ffmpeg = el["size"];
						std::string date {json_ffmpeg["published_at"]};
						ver_ffmpeg_latest.year = stoi(date.substr(0, 4));
						ver_ffmpeg_latest.month = stoi(date.substr(5, 2));
						ver_ffmpeg_latest.day = stoi(date.substr(8, 2));
						break;
					}
				}
			}
		}
		if(thr_releases_ffmpeg.joinable())
			thr_releases_ffmpeg.detach();
	}};
}


void GUI::get_latest_ytdlp(nana::window parent_for_msgbox)
{
	using json = nlohmann::json;

	thr_releases_ytdlp = std::thread {[=]
	{
		std::string jtext;
		auto fname {conf.ytdlp_path.filename().string()};
		if(fname.empty())
			fname = ytdlp_fname;
		if(fname == "ytdl-patched-red.exe")
			jtext = util::get_inet_res("https://api.github.com/repos/ytdl-patched/ytdl-patched/releases/latest", &inet_error);
		else 
		{
			if(win7)
				jtext = util::get_inet_res("https://api.github.com/repos/nicolaasjan/yt-dlp/releases/latest", &inet_error);
			else
			{
				if(conf.ytdlp_nightly)
					jtext = util::get_inet_res("https://api.github.com/repos/yt-dlp/yt-dlp-nightly-builds/releases/latest", &inet_error);
				else jtext = util::get_inet_res("https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest", &inet_error);
			}
		}
		if(!jtext.empty() && thr_releases_ytdlp.joinable())
		{
			json json_ytdlp;
			try { json_ytdlp = json::parse(jtext); }
			catch(nlohmann::detail::exception e)
			{
				tmsg_parent = parent_for_msgbox;
				tmsg_title = "ytdlp-interface error";
				std::string str;
				if(jtext.size() > 600)
				{
					jtext.erase(600);
					str = " (showing the first 600 characters)";
				}
				tmsg_text = "Got an unexpected response when checking GitHub for a new yt-dlp version" + str + ":\n\n" + jtext +
					"\n\nError from the JSON parser:\n\n" + e.what();
				if(thr_releases_ytdlp.joinable())
					thr_releases_ytdlp.detach();
				return;
			}
			if(!json_ytdlp.empty() && thr_releases_ytdlp.joinable())
			{
				for(auto &el : json_ytdlp["assets"])
				{
					std::string url {el["browser_download_url"]};
					if(url.find(fname) != -1)
					{
						url_latest_ytdlp = url;
						size_latest_ytdlp = el["size"];
						url_latest_ytdlp_relnotes = json_ytdlp["html_url"];
						std::string date {json_ytdlp["published_at"]};
						ver_ytdlp_latest.year = stoi(date.substr(0, 4));
						ver_ytdlp_latest.month = stoi(date.substr(5, 2));
						ver_ytdlp_latest.day = stoi(date.substr(8, 2));
						break;
					}
				}
			}
		}
		if(thr_releases_ytdlp.joinable())
			thr_releases_ytdlp.detach();
	}};
}


void GUI::get_latest_deno(nana::window parent_for_msgbox)
{
	thr_releases_deno = std::thread {[parent_for_msgbox, this]
	{
		using json = nlohmann::json;
		auto jtext {util::get_inet_res("https://api.github.com/repos/denoland/deno/releases/latest", &inet_error)};
		if(!jtext.empty())
		{
			json jrel;
			try { jrel = json::parse(jtext); }
			catch(nlohmann::detail::exception e)
			{
				tmsg_parent = parent_for_msgbox;
				tmsg_title = "69-ytdlp-interface JSON error";
				std::string str;
				if(jtext.size() > 600)
					str = " (showing the first 600 characters)";
				tmsg_text = "Got an unexpected response when checking GitHub for a new Deno version" + str + ":\n\n" + jtext.substr(0, 600) +
					"\n\nError from the JSON parser:\n\n" + e.what();
				if(thr_releases_deno.joinable())
					thr_releases_deno.detach();
				return;
			}
			if(!jrel.empty() && jrel.contains("assets"))
			{
				url_latest_deno.clear();				
				for(auto &el : jrel["assets"])
				{
					std::string url {el["browser_download_url"]};
					if(url.contains("deno-x86_64-pc-windows-msvc.zip"))
					{
						url_latest_deno = url;
						size_latest_deno = el["size"];
						url_latest_deno_relnotes = jrel["html_url"];
						if(jrel.contains("tag_name") && jrel["tag_name"].is_string())
							ver_deno_latest = semver_t {jrel["tag_name"].get<std::string>()};
						break;
					}
				}
			}
		}
		if(thr_releases_deno.joinable())
			thr_releases_deno.detach();
	}};
}


void GUI::get_versions()
{
	thr_versions = std::thread {[this]
	{
		get_version_ffmpeg(false);
		get_version_ytdlp();
		if(X64 && !win7)
			get_version_deno();
		if(thr_ver_ffmpeg.joinable())
			thr_ver_ffmpeg.join();
		if(thr_ver_deno.joinable())
			thr_ver_deno.join();
		if(thr_versions.joinable())
			thr_versions.detach();
	}};
}


void GUI::get_version_ytdlp()
{
	if(!conf.ytdlp_path.empty())
	{
		if(fs::exists(conf.ytdlp_path))
		{
			auto ver {util::run_piped_process(L'\"' + conf.ytdlp_path.wstring() + L"\" --version")};
			if(ver.size() >= 10)
			{
				ver_ytdlp = {-1, -1, -1};
				auto rawver {ver.substr(0, 10)};
				try { ver_ytdlp.year = stoi(rawver.substr(0, 4)); }
				catch(...) { return; }
				ver_ytdlp.month = stoi(rawver.substr(5, 2));
				ver_ytdlp.day = stoi(rawver.substr(8, 2));
			}
		}
		else conf.ytdlp_path.clear();
	}
}


void GUI::get_version_deno()
{
	fs::path deno_path;
	if(!conf.ytdlp_path.empty())
		deno_path = conf.ytdlp_path.parent_path() / "deno.exe";
	if(deno_path.empty() || !fs::exists(deno_path))
		deno_path = appdir / "deno.exe";
	if(fs::exists(deno_path) && !thr_ver_deno.joinable())
	{
		thr_ver_deno = std::thread {[=, this]
		{
			auto ver {util::run_piped_process(L'\"' + deno_path.wstring() + L"\" --version")};
			if(ver.starts_with("deno "))
			{
				auto rawver {ver.substr(5, ver.find(' ', 5) - 5)};
				ver_deno = semver_t {rawver};
			}
		}};
	}
}


void GUI::get_version_ffmpeg(bool auto_detach)
{
	if(win7)
	{
		if(!conf.ffmpeg_path.empty())
		{
			struct stat result;
			if(stat((conf.ffmpeg_path / "ffmpeg.exe").string().data(), &result) == 0)
			{
				tm utc_tm {*gmtime(&result.st_mtime)};
				ver_ffmpeg.year = utc_tm.tm_year + 1900;
				ver_ffmpeg.month = utc_tm.tm_mon + 1;
				ver_ffmpeg.day = utc_tm.tm_mday;
			}
		}
	}
	else if(!thr_ver_ffmpeg.joinable())
	{
		thr_ver_ffmpeg = std::thread {[=, this]
		{
			if(!conf.ffmpeg_path.empty())
			{
				std::wstring cmd {L'\"' + (conf.ffmpeg_path / "ffmpeg.exe").wstring() + L"\" -version"};
				auto ver {util::run_piped_process(cmd)};
				auto pos {ver.find("--extra-version=")};
				if(pos != -1)
				{
					auto rawver {ver.substr(pos + 16, 8)};
					try
					{
						ver_ffmpeg.year = stoi(rawver.substr(0, 4));
						ver_ffmpeg.month = stoi(rawver.substr(4, 2));
						ver_ffmpeg.day = stoi(rawver.substr(6, 2));
					}
					catch(...)
					{
						outbox.append(L"", "[GUI] failed to get ffmpeg version! Falling back to file mtime.");
						struct stat result;
						if(stat((conf.ffmpeg_path / "ffmpeg.exe").string().data(), &result) == 0)
						{
							tm utc_tm {*gmtime(&result.st_mtime)};
							ver_ffmpeg.year = utc_tm.tm_year + 1900;
							ver_ffmpeg.month = utc_tm.tm_mon + 1;
							ver_ffmpeg.day = utc_tm.tm_mday;
						}
					}
				}
			}
			if(auto_detach && thr_ver_ffmpeg.joinable())
				thr_ver_ffmpeg.detach();
		}};
	}
}


bool GUI::is_ytlink(std::wstring url)
{
	auto pos {url.find(L"www.")};
	if(pos != -1)
		url.erase(pos, 4);
	pos = url.find(L"m.youtube.");
	if(pos != -1)
		url.erase(pos, 2);
	if(url.starts_with(L"https://music.youtube.com"))
		return true;
	if(url.starts_with(L"https://youtube.com/clip/"))
		return true;
	if(url.starts_with(L"https://youtube.com/") && url.size() > 20 || url.starts_with(L"youtube.com/") && url.size() > 12)
		return true;
	if(url.starts_with(L"https://youtu.be/") && url.size() > 17 || url.starts_with(L"youtu.be/") && url.size() > 9)
		return true;
	return false;
}


bool GUI::is_ytchan(std::wstring url)
{
	if(url.find(L"youtube.com/c/") != -1 || url.find(L"youtube.com/@") != -1 ||
		url.find(L"youtube.com/channel/") != -1 || url.find(L"youtube.com/user/") != -1)
		return true;
	auto pos {url.find(L"youtube.com/")};
	if(pos != -1 && url.size() > pos + 12 && url.find(L"watch?v=") == -1 && url.find(L"list=") == -1 && url.find(L"/live/") == -1
		&& url.find(L"music.youtube.com") == -1 && url.find(L"youtube.com/clip/") == -1 && url.find(L"youtube.com/shorts/") == -1)
		return true;
	return false;
}


bool GUI::is_scplaylist(std::wstring url)
{
	if(url.find(L"soundcloud.com/") != -1)
	{
		auto pos_in {url.find(L"?in=")}, pos_sets {url.find(L"/sets/")};
		if(pos_sets != -1 && pos_in == -1)
			return true;
	}
	return false;
}


void GUI::taskbar_overall_progress()
{
	ULONGLONG completed {0}, skip {0}, total {lbq.item_count()}, startable {0};
	if(i_taskbar && total > 1)
	{
		for(size_t cat {0}; cat < lbq.size_categ(); cat++)
		{
			for(auto &item : lbq.at(cat))
			{
				if(item.text(3) == "done" || item.text(3) == "already downloaded")
					completed++;
				else if(item.text(3) == "skip")
					skip++;
				else startable++;
			}
		}
		total -= skip;
		if(completed || startable)
		{
			if(completed == total || !startable)
				i_taskbar->SetProgressState(hwnd, TBPF_NOPROGRESS);
			else 
			{
				if(completed == 0)
					i_taskbar->SetProgressValue(hwnd, 1, ULLONG_MAX);
				else i_taskbar->SetProgressValue(hwnd, completed, total);
			}
		}
		else i_taskbar->SetProgressState(hwnd, TBPF_NOPROGRESS);
	}
}


void GUI::on_btn_dl(std::wstring url)
{
	use_ffmpeg_location = !ffmpeg_location_in_conf_file();
	if(process_queue_item(url)) // if the current url was started (as opposed to stopped)
		start_next_urls(url);    // also start the next ones if any are startable	
}


unsigned GUI::start_next_urls(std::wstring current_url)
{
	using namespace nana;
	using index_pair = drawerbase::listbox::index_pair;
	unsigned urls_started {0};

	if(autostart_next_item)
	{
		auto items_currently_downloading {0};

		for(const auto &bottom : bottoms)
			if(bottom.second->started)
				++items_currently_downloading;

		if(items_currently_downloading < conf.max_concurrent_downloads && lbq.item_count())
		{
			std::vector<drawerbase::listbox::index_pair> preceding_startables;
			auto current_item {lbq.item_from_value(current_url)};
			auto current_pos {current_item != lbq.empty_item ? current_item.to_display() : index_pair{}};
			for(size_t cat {0}; cat < lbq.size_categ() && items_currently_downloading < conf.max_concurrent_downloads; cat++)
			{
				for(auto &item : lbq.at(cat))
				{
					if(item == current_item)
						continue;
					auto status_text {item.text(3)};
					if(status_text == "queued" || status_text.find("stopped") == 0 || !bottoms.at(item.value<lbqval_t>().url).started && status_text.find("%") != -1)
					{
						auto pos {item.pos()};
						if(pos.cat <= current_pos.cat && pos.item < current_pos.item)
						{
							if(preceding_startables.size() < 10)
								preceding_startables.push_back(pos);
						}
						else if(pos.cat == current_pos.cat && pos.item > current_pos.item || pos.cat > current_pos.cat)
						{
							process_queue_item(item.value<lbqval_t>());
							urls_started++;
							if(++items_currently_downloading >= conf.max_concurrent_downloads)
								break;
						}
					}
				}
			}
			if(items_currently_downloading < conf.max_concurrent_downloads)
				for(auto &pos : preceding_startables)
				{
					process_queue_item(lbq.at(pos).value<lbqval_t>());
					urls_started++;
					if(++items_currently_downloading >= conf.max_concurrent_downloads)
						break;
				}
		}
	}
	return urls_started;
}


bool GUI::lbq_has_scrollbar()
{
	nana::paint::graphics g {{100, 100}};
	g.typeface(lbq.typeface());
	const auto text_height {g.text_extent_size("mm").height};
	const auto item_height {text_height + lbq.scheme().item_height_ex};
	unsigned nitems {0};
	for(int cat {0}; cat < lbq.size_categ(); cat++)
		nitems += lbq.at(cat).size();
	nitems += lbq.size_categ() - 1;
	if(nitems * item_height > lbq.size().height - 25)
		return true;
	return false;
}


void GUI::adjust_lbq_headers()
{
	using namespace util;

	if(std::this_thread::get_id() == main_thread_id)
	{
		std::vector<size_t> cat_sizes {lbq.size_categ(), 0};
		for(int catidx {0}; catidx < lbq.size_categ(); catidx++)
			cat_sizes.push_back(lbq.at(catidx).size());
		auto it {std::max_element(cat_sizes.begin(), cat_sizes.end())};
		int largest_cat_size {0};
		if(it != cat_sizes.end())
			largest_cat_size = *it;

		nana::label l {*this};
		l.typeface(lbq.typeface());
		l.caption(std::to_string(largest_cat_size));
		const auto minw {l.measure(0).width};
		const auto padding {scale(14)};
		int col_zero_size {std::max(static_cast<int>(minw) + padding, scale(30))};

		auto zero {col_zero_size},
			one {scale(20 + !conf.col_site_text * 10) * conf.col_site_icon + scale(110) * conf.col_site_text},
			three {scale(116)},
			four {scale(130) * lbq.column_at(4).visible() + scale(16) * lbq_has_scrollbar()},
			five {scale(150) * lbq.column_at(5).visible()},
			six {scale(60) * lbq.column_at(6).visible()},
			seven {scale(100) * lbq.column_at(7).visible()};

		lbq.column_at(0).width(zero);

		if(conf.col_site_icon || conf.col_site_text)
		{
			lbq.column_at(1).width(one);
			lbq.column_at(1).text(conf.col_site_text ? "Website" : "");
			if(!lbq.column_at(1).visible())
				lbq.column_at(1).visible(true);
		}
		else lbq.column_at(1).visible(false);

		const auto two_computed {lbq.size().width - (zero + one + three + four + five + six + seven) - scale(9)};
		lbq.column_at(2).width(two_computed);
	}
	else PostMessage(hwnd, WM_LBQ_HEADERS, 0, 0);
}


bool GUI::ffmpeg_location_in_conf_file()
{
	std::ifstream infile {conf.ytdlp_path.parent_path() / "yt-dlp.conf"};
	if(infile)
	{
		std::stringstream ss;
		ss << infile.rdbuf();
		if(ss.str().find("--ffmpeg-location") != -1)
			return true;
	}
	return false;
}


HRESULT STDMETHODCALLTYPE GUI::DragEnter(IDataObject *pDataObj, DWORD, POINTL, DWORD*)
{
	FORMATETC fmtetc {CF_TEXT, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
	STGMEDIUM stgmedium;
	HRESULT hr {pDataObj->GetData(&fmtetc, &stgmedium)};
	if(FAILED(hr)) return hr;

	auto ptext {reinterpret_cast<const char *>(GlobalLock(stgmedium.hGlobal))};
	if(ptext)
	{
		std::string text {ptext};
		if(text.starts_with("http://") || text.starts_with("https://"))
		{
			drop_cliptext_temp = util::get_clipboard_text();
			util::set_clipboard_text(hwnd, nana::to_wstring(text));
			l_url.events().mouse_enter.emit({}, l_url);
			ReleaseStgMedium(&stgmedium);
			return S_OK;
		}
	}
	ReleaseStgMedium(&stgmedium);
	return S_FALSE;
}


HRESULT STDMETHODCALLTYPE GUI::DragLeave()
{
	l_url.events().mouse_leave.emit({}, l_url);
	util::set_clipboard_text(hwnd, drop_cliptext_temp);
	return S_OK;
}


HRESULT STDMETHODCALLTYPE GUI::Drop(IDataObject *pDataObj, DWORD, POINTL, DWORD*)
{
	auto sel {lbq.selected()};
	if(sel.empty())
		qurl.clear();
	else qurl = lbq.at(sel.front()).value<lbqval_t>();
	l_url.update_caption();
	l_url.events().mouse_up.emit({}, l_url);
	util::set_clipboard_text(hwnd, drop_cliptext_temp);
	return S_OK;
}

void GUI::update_compact_mode()
{
	// When the window is too narrow or short for the side panel (typically after snapping it to a
	// screen half or corner), hide the panel so the queue stays usable; bring it back when there is room.
	if(no_draw_freeze || fullscreen)
		return;
	const int cw {static_cast<int>(size().width)}, ch {static_cast<int>(size().height)};
	const int need_w {dpi_scale(860)}, need_h {dpi_scale(640)};
	bool want {auto_compact};
	if(!auto_compact && (cw < need_w || ch < need_h))
		want = true;
	else if(auto_compact && cw >= need_w + dpi_scale(24) && ch >= need_h + dpi_scale(24)) // hysteresis
		want = false;
	if(want == auto_compact)
		return;

	auto_compact = want;
	auto &plc {get_place()};
	const bool show {!auto_compact && !expcol.collapsed()};
	plc.field_display("side", show);
	plc.field_display("gpopt_spacer", show);
	if(auto_compact)
		expcol.hide();
	else expcol.show();
	plc.collocate();
	nana::api::refresh_window(*this);
}


void GUI::toggle_fullscreen()
{
	if(!fullscreen)
	{
		wp_fs.length = sizeof wp_fs;
		GetWindowPlacement(hwnd, &wp_fs);
		style_fs = GetWindowLongPtr(hwnd, GWL_STYLE);
		MONITORINFO mi {sizeof mi};
		if(!GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
			return;
		fullscreen = true;
		SetWindowLongPtr(hwnd, GWL_STYLE, style_fs & ~(WS_CAPTION | WS_THICKFRAME));
		SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
			mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
	}
	else
	{
		fullscreen = false;
		SetWindowLongPtr(hwnd, GWL_STYLE, style_fs);
		SetWindowPlacement(hwnd, &wp_fs);
		SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
	}
	update_compact_mode();
}


// ---- download folders ----------------------------------------------------------------------------------

bool GUI::pick_folder(const fs::path &start, const std::string &title, fs::path &result)
{
	// the shell dialog misbehaves with relative or unreachable start folders (seen with rclone mounts),
	// so always hand it an absolute folder that exists
	std::error_code ec;
	fs::path origin {conf.open_dialog_origin ? start : appdir};
	if(origin.empty())
		origin = appdir;
	if(origin.is_relative())
	{
		auto abs {fs::absolute(origin, ec)};
		origin = ec ? appdir : abs;
	}
	if(!util::path_exists(origin))
		origin = appdir;
	applog::write("folder", L"dialog open (" + nana::to_wstring(title) + L"), start in: " + origin.wstring());
	try
	{
		nana::folderbox fb {*this, origin};
		fb.allow_multi_select(false);
		fb.title(title);
		auto res {fb()};
		if(res.empty())
		{
			applog::write("folder", "dialog cancelled");
			return false;
		}
		applog::write("folder", L"chosen: " + res.front().wstring());
		result = util::to_relative_path(res.front());
		return true;
	}
	catch(const std::exception &e)
	{
		applog::write("folder", std::string {"dialog error: "} + e.what());
	}
	catch(...)
	{
		applog::write("folder", "dialog error (unknown exception)");
	}
	return false;
}


void GUI::apply_outpath(gui_bottom &bot)
{
	bot.outpath = (!conf.common_dl_options && !bot.custom_outpath.empty()) ? bot.custom_outpath : conf.outpath;
}


void GUI::apply_outpaths()
{
	for(auto &pbot : bottoms)
		apply_outpath(*pbot.second);
	l_outpath.update_caption();
	tgrid.refresh();
}


void GUI::set_default_folder(const fs::path &path)
{
	if(!conf.outpath.empty() && path != conf.outpath)
	{
		if(conf.outpaths.size() >= 11 && conf.outpaths.find(conf.outpath) == conf.outpaths.end())
			conf.outpaths.erase(conf.outpaths.begin());
		conf.outpaths.insert(conf.outpath);
	}
	conf.outpath = path;
	conf.outpaths.insert(path);
	applog::write("folder", L"default download folder: " + path.wstring());
	apply_outpaths();
}


std::vector<std::wstring> GUI::selected_queue_urls()
{
	std::vector<std::wstring> urls;
	for(auto &ip : lbq.selected())
	{
		auto url {lbq.at(ip).value<lbqval_t>().url};
		if(bottoms.contains(url))
			urls.push_back(url);
	}
	return urls;
}


void GUI::choose_items_folder(std::vector<std::wstring> urls)
{
	if(conf.common_dl_options || urls.empty())
		return;
	fs::path start {bottoms.at(urls.front()).outpath}, chosen;
	const std::string title {urls.size() == 1 ? "Download folder for this queue item" :
		"Download folder for " + std::to_string(urls.size()) + " queue items"};
	if(!pick_folder(start, title, chosen))
		return;
	for(const auto &url : urls)
	{
		if(!bottoms.contains(url))
			continue;
		auto &bot {bottoms.at(url)};
		bot.custom_outpath = chosen;
		apply_outpath(bot);
		applog::write("folder", L"item folder: " + chosen.wstring() + L" <- " + url);
	}
	if(conf.outpaths.size() >= 11 && conf.outpaths.find(chosen) == conf.outpaths.end())
		conf.outpaths.erase(conf.outpaths.begin());
	conf.outpaths.insert(chosen);
	tgrid.refresh();
}


void GUI::reset_items_folder(const std::vector<std::wstring> &urls)
{
	for(const auto &url : urls)
	{
		if(!bottoms.contains(url))
			continue;
		auto &bot {bottoms.at(url)};
		bot.custom_outpath.clear();
		apply_outpath(bot);
		applog::write("folder", L"item uses the default folder again: " + url);
	}
	tgrid.refresh();
}


void GUI::open_logs_folder()
{
	const auto dir {applog::folder()};
	applog::write("app", "logs folder opened");
	if(!dir.empty() && util::path_exists(dir))
		ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}


// ---- multi-selection, mini log, existing files, cleanup -------------------------------------------------

void GUI::apply_to_selection(const std::function<void(gui_bottom &)> &fn)
{
	// with several queue items selected, an option changed in the side panel applies to all of them
	if(conf.common_dl_options || bot_showing)
		return;
	const auto urls {selected_queue_urls()};
	if(urls.size() < 2)
		return;
	for(const auto &url : urls)
	{
		if(!bottoms.contains(url))
			continue;
		auto &bot {bottoms.at(url)};
		if(bot.started)
			continue;
		fn(bot);
	}
	tgrid.refresh();
}


void GUI::update_options_caption()
{
	if(conf.common_dl_options)
		gpopt.caption("Download options");
	else
	{
		const auto sel {lbq.selected()};
		if(sel.size() > 1)
			gpopt.caption("Download options for " + std::to_string(sel.size()) + " selected items");
		else if(!sel.empty())
			gpopt.caption("Download options for queue item #" + lbq.at(sel.front()).text(0));
		else gpopt.caption("Download options");
	}
	nana::api::refresh_window(gpopt);
}


void GUI::update_minilog(bool force)
{
	if(!minilog.visible())
		return;
	std::string text;
	if(!qurl.empty())
	{
		const auto &buf {outbox.buffer(qurl)};
		std::vector<std::string> lines;
		size_t end {buf.size()};
		while(end > 0 && lines.size() < 5)
		{
			const auto start {buf.rfind('\n', end - 1)};
			const size_t from {start == std::string::npos ? 0 : start + 1};
			auto line {buf.substr(from, end - from)};
			while(!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
				line.pop_back();
			if(!line.empty())
			{
				if(line.size() > 300)
					line = line.substr(0, 297) + "...";
				lines.insert(lines.begin(), line);
			}
			if(start == std::string::npos)
				break;
			end = start;
		}
		for(const auto &l : lines)
			text += (text.empty() ? "" : "\n") + l;
		if(text.empty())
			text = "(no output yet)";
	}
	if(!force && text == minilog_shown && qurl == minilog_url)
		return;
	minilog_shown = text;
	minilog_url = qurl;
	minilog.caption(text);
}


fs::path GUI::expected_media_stem(gui_bottom &bot)
{
	// the downloaded file's path without extension, when it can be known before downloading
	if(bot.is_ytplaylist || bot.is_ytchan || bot.is_yttab || bot.is_bcplaylist || bot.is_bcchan || bot.is_scplaylist || bot.is_gen_playlist)
		return {};
	if(bot.sections.size() > 1 || bot.com_chap == 2)
		return {};
	if(bot.cbargs && (bot.argset.find("-o ") != std::string::npos || bot.argset.find("--output") != std::string::npos ||
		bot.argset.find("-P ") != std::string::npos || bot.argset.find("--paths") != std::string::npos))
		return {};
	std::wstring name;
	try
	{
		if(!bot.outfile.empty())
			name = bot.outfile.filename().wstring();
		else if(bot.vidinfo_contains("filename") && bot.vidinfo["filename"].is_string())
		{
			fs::path f {fs::u8path(bot.vidinfo["filename"].get<std::string>())};
			name = f.replace_extension().wstring();
		}
	}
	catch(...) { return {}; }
	if(name.empty())
		return {};
	if(bot.sections.size() == 1)
		name += L' ' + util::segment_tag(bot.sections.front().first, bot.sections.front().second);
	return bot.outpath / name;
}


bool GUI::media_exists(const fs::path &stem, fs::path &found)
{
	if(stem.empty())
		return false;
	for(const wchar_t *ext : {L"mp4", L"mkv", L"webm", L"mov", L"avi", L"flv", L"m4a", L"mp3", L"opus", L"ogg", L"flac", L"wav", L"aac"})
	{
		fs::path p {stem};
		p += L'.';
		p += ext;
		std::error_code ec;
		if(util::path_exists(p) && !fs::is_directory(p, ec))
		{
			found = p;
			return true;
		}
	}
	return false;
}


void GUI::cleanup_failed_download(gui_bottom &bot, const std::string &output, fs::file_time_type started)
{
	// Deletes the leftovers a failed run created for this item: partial downloads (.part, .ytdl, fragments),
	// separate format pieces (name.f137.mp4), temporary merge/fixup files (name.temp.mkv) and the thumbnail
	// prepared for embedding. Finished media files and anything older than this run are never touched.
	std::error_code ec;
	const auto root {fs::absolute(bot.outpath, ec).lexically_normal()};
	if(ec || bot.outpath.empty())
		return;

	std::set<fs::path> cands;
	auto add = [&](std::string p)
	{
		while(!p.empty() && (p.back() == '\r' || p.back() == '\n' || p.back() == ' ' || p.back() == '"'))
			p.pop_back();
		while(!p.empty() && (p.front() == ' ' || p.front() == '"'))
			p.erase(0, 1);
		if(p.empty())
			return;
		try { cands.insert(fs::u8path(p)); }
		catch(...) {}
	};

	size_t pos {0};
	while(pos < output.size())
	{
		auto end {output.find('\n', pos)};
		if(end == std::string::npos)
			end = output.size();
		const std::string line {output.substr(pos, end - pos)};
		pos = end + 1;
		size_t p;
		if((p = line.find("Destination: ")) != std::string::npos)
			add(line.substr(p + 13));
		else if(line.find("Writing video thumbnail") != std::string::npos && (p = line.find(" to: ")) != std::string::npos)
			add(line.substr(p + 5));
		else if((p = line.find("Converting thumbnail \"")) != std::string::npos)
		{
			const auto q {line.find('"', p + 22)};
			if(q != std::string::npos)
			{
				const auto src {line.substr(p + 22, q - p - 22)};
				add(src);
				const auto to {line.find(" to ", q)};
				if(to != std::string::npos)
				{
					std::string ext {line.substr(to + 4)};
					while(!ext.empty() && !isalnum(static_cast<unsigned char>(ext.back())))
						ext.pop_back();
					try
					{
						fs::path conv {fs::u8path(src)};
						conv.replace_extension(ext);
						cands.insert(conv);
					}
					catch(...) {}
				}
			}
		}
		else if((p = line.find("Merging formats into \"")) != std::string::npos)
		{
			const auto q {line.find('"', p + 22)};
			if(q != std::string::npos)
			{
				try
				{
					fs::path merged {fs::u8path(line.substr(p + 22, q - p - 22))};
					auto temp {merged};
					temp.replace_extension(L".temp" + merged.extension().wstring());
					cands.insert(temp);
				}
				catch(...) {}
			}
		}
	}

	auto is_leftover = [](const fs::path &f)
	{
		const auto name {f.filename().wstring()};
		auto lname {name};
		for(auto &c : lname)
			c = towlower(c);
		auto ends = [&](const wchar_t *s) { const std::wstring e {s}; return lname.size() >= e.size() && lname.compare(lname.size() - e.size(), e.size(), e) == 0; };
		if(ends(L".part") || ends(L".ytdl") || lname.find(L".part-frag") != std::wstring::npos || lname.find(L".temp.") != std::wstring::npos)
			return true;
		if(ends(L".jpg") || ends(L".jpeg") || ends(L".png") || ends(L".webp"))
			return true; // thumbnail written for embedding
		// separate format piece: "name.f137.mp4" / "name.f251-1.webm"
		const auto dot2 {lname.rfind(L'.')};
		if(dot2 != std::wstring::npos && dot2 > 0)
		{
			const auto dot1 {lname.rfind(L'.', dot2 - 1)};
			if(dot1 != std::wstring::npos && dot1 + 2 < dot2 && lname[dot1 + 1] == L'f')
			{
				bool fmt {true};
				for(auto i {dot1 + 2}; i < dot2; i++)
					if(!iswdigit(lname[i]) && lname[i] != L'-')
						fmt = false;
				if(fmt)
					return true;
			}
		}
		return false;
	};

	// partial-download companions of every candidate
	std::set<fs::path> all {cands};
	for(const auto &c : cands)
	{
		for(const wchar_t *suffix : {L".part", L".ytdl"})
		{
			fs::path p {c};
			p += suffix;
			all.insert(p);
		}
	}
	// fragment files ("name.mp4.part-Frag12") next to the candidates
	std::set<fs::path> dirs;
	for(const auto &c : cands)
		dirs.insert(fs::absolute(c, ec).parent_path());
	for(const auto &d : dirs)
	{
		std::error_code dec;
		for(fs::directory_iterator it {d, dec}, endit; !dec && it != endit; it.increment(dec))
		{
			const auto fname {it->path().filename().wstring()};
			for(const auto &c : cands)
			{
				const auto cname {c.filename().wstring() + L".part-Frag"};
				if(fname.starts_with(cname))
					all.insert(it->path());
			}
		}
	}

	const auto threshold {started - std::chrono::seconds {3}};
	int removed {0};
	for(const auto &f : all)
	{
		std::error_code fec;
		const auto abs {fs::absolute(f, fec).lexically_normal()};
		if(fec)
			continue;
		const auto rel {abs.lexically_relative(root)};
		if(rel.empty() || rel.native().starts_with(L".."))
			continue; // only inside the item's download folder
		if(!is_leftover(abs) || !fs::is_regular_file(abs, fec) || fec)
			continue;
		const auto mtime {fs::last_write_time(abs, fec)};
		if(fec || mtime < threshold)
			continue; // older than this run: not ours
		if(fs::remove(abs, fec) && !fec)
		{
			removed++;
			applog::write("cleanup", L"removed leftover: " + abs.wstring());
		}
		else applog::write("cleanup", L"could not remove: " + abs.wstring() + L" (" + nana::to_wstring(fec.message()) + L")");
	}
	if(removed)
		outbox.append(bot.url, "[GUI] removed " + std::to_string(removed) + " leftover file(s) of the failed download\n");
}


// ---- file info ------------------------------------------------------------------------------------------

void GUI::probe_file_info(gui_bottom &bot, const fs::path &file)
{
	gui_bottom::file_info_t fi;
	std::error_code ec;
	if(file.empty() || !util::path_exists(file))
		return;
	fi.path = nana::to_utf8(file.wstring());
	fi.ext = nana::to_utf8(file.extension().wstring());
	if(!fi.ext.empty() && fi.ext.front() == '.')
		fi.ext.erase(0, 1);

	auto human_size = [](double bytes)
	{
		const char *units[] {"B", "KB", "MB", "GB", "TB"};
		int u {0};
		while(bytes >= 1024 && u < 4)
		{
			bytes /= 1024;
			u++;
		}
		char buf[32];
		sprintf_s(buf, u ? "%.1f %s" : "%.0f %s", bytes, units[u]);
		return std::string {buf};
	};
	auto hms = [](double secs)
	{
		const long long t {static_cast<long long>(secs + .5)};
		char buf[32];
		if(t >= 3600)
			sprintf_s(buf, "%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
		else sprintf_s(buf, "%lld:%02lld", t / 60, t % 60);
		return std::string {buf};
	};

	const auto fsize {fs::file_size(file, ec)};
	if(!ec)
		fi.size = human_size(static_cast<double>(fsize));

	// ffprobe: next to ffmpeg, in the program folder, next to yt-dlp, or on the PATH
	fs::path ffprobe;
	for(const auto &dir : {conf.ffmpeg_path, appdir, conf.ytdlp_path.parent_path()})
	{
		if(!dir.empty() && util::path_exists(dir / "ffprobe.exe"))
		{
			ffprobe = dir / "ffprobe.exe";
			break;
		}
	}
	const std::wstring probe_exe {ffprobe.empty() ? L"ffprobe.exe" : ffprobe.wstring()};
	std::string out;
	try
	{
		out = util::run_piped_process(L'"' + probe_exe + L"\" -v error -print_format json -show_format -show_streams \"" + file.wstring() + L'"');
	}
	catch(...) {}
	try
	{
		const auto jstart {out.find('{')};
		if(jstart != std::string::npos)
		{
			const auto j {nlohmann::json::parse(out.substr(jstart))};
			if(j.contains("format") && j["format"].contains("duration") && j["format"]["duration"].is_string())
				fi.duration = hms(std::stod(j["format"]["duration"].get<std::string>()));
			std::vector<std::string> sublangs;
			int subcount {0};
			if(j.contains("streams") && j["streams"].is_array())
			{
				for(const auto &st : j["streams"])
				{
					const std::string type {st.contains("codec_type") && st["codec_type"].is_string() ? st["codec_type"].get<std::string>() : ""};
					const std::string codec {st.contains("codec_name") && st["codec_name"].is_string() ? st["codec_name"].get<std::string>() : ""};
					const bool attached {st.contains("disposition") && st["disposition"].contains("attached_pic") && st["disposition"]["attached_pic"] == 1};
					if(type == "video" && attached)
						fi.thumb = true; // cover art in mp4/m4a/mp3
					else if(type == "attachment")
					{
						std::string mime;
						if(st.contains("tags") && st["tags"].contains("mimetype") && st["tags"]["mimetype"].is_string())
							mime = st["tags"]["mimetype"].get<std::string>();
						if(mime.starts_with("image/"))
							fi.thumb = true; // cover art attachment in mkv
					}
					else if(type == "video" && fi.resolution.empty())
					{
						if(st.contains("width") && st.contains("height") && st["width"].is_number() && st["height"].is_number())
							fi.resolution = std::to_string(st["width"].get<int>()) + "x" + std::to_string(st["height"].get<int>());
						fi.vcodec = codec;
					}
					else if(type == "audio" && fi.acodec.empty())
						fi.acodec = codec;
					else if(type == "subtitle")
					{
						subcount++;
						if(st.contains("tags") && st["tags"].contains("language") && st["tags"]["language"].is_string())
							sublangs.push_back(st["tags"]["language"].get<std::string>());
					}
				}
			}
			for(const auto &l : sublangs)
				fi.subs += (fi.subs.empty() ? "" : ", ") + l;
			if(fi.subs.empty() && subcount)
				fi.subs = std::to_string(subcount);
		}
		else applog::write("fileinfo", "ffprobe gave no JSON (missing ffprobe.exe?) for " + fi.path);
	}
	catch(const std::exception &e)
	{
		applog::write("fileinfo", std::string {"ffprobe output could not be read: "} + e.what());
	}

	if(bot.sections.size() == 1)
	{
		const auto a {util::normalize_timestamp(bot.sections.front().first)}, b {util::normalize_timestamp(bot.sections.front().second)};
		fi.cut = nana::to_utf8((a.empty() ? std::wstring {L"start"} : a) + L" - " + (b.empty() ? std::wstring {L"end"} : b));
	}
	else if(bot.sections.size() > 1)
		fi.cut = std::to_string(bot.sections.size()) + " segments";
	fi.valid = true;

	std::string summary {fi.resolution.empty() ? "audio" : fi.resolution};
	summary += " | " + fi.ext + " | " + (fi.size.empty() ? "?" : fi.size);
	if(!fi.duration.empty())
		summary += " | " + fi.duration;
	if(!fi.vcodec.empty() || !fi.acodec.empty())
		summary += " | " + (fi.vcodec.empty() ? "-" : fi.vcodec) + "/" + (fi.acodec.empty() ? "-" : fi.acodec);
	summary += std::string {" | thumbnail: "} + (fi.thumb ? "yes" : "no");
	summary += " | subtitles: " + (fi.subs.empty() ? std::string {"none"} : fi.subs);
	if(!fi.cut.empty())
		summary += " | cut: " + fi.cut;

	{
		std::lock_guard lock {finfo_mtx};
		bot.finfo = fi;
	}
	lbq.set_line_text(bot.url, {"", "", "", "", fi.resolution.empty() ? "audio only" : fi.resolution, fi.ext, fi.size.empty() ? "?" : fi.size});
	outbox.append(bot.url, "[GUI] file: " + fi.path + "\n[GUI] info: " + summary + "\n");
	applog::write("fileinfo", summary + " <- " + fi.path);
}
