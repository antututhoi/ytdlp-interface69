#pragma once
#include <functional>
#include <string>
#include <algorithm>
#include <memory>
#include <map>
#include <vector>
#include <nana/gui.hpp>
#include <nana/gui/drawing.hpp>
#include <nana/gui/widgets/panel.hpp>
#include "widgets.hpp"
#include "util.hpp"

// windows.h (pulled in by the includes above) defines min/max macros that break std::min/std::max;
// the rest of the project removes them in gui.hpp, but that happens after this header is included.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif


// Card-style "thumbnail view" of the download queue. It owns no data: the GUI supplies the
// number of cards and the contents of each card through callbacks, and handles user actions
// (selection, double click, context menu) through the callbacks too, so the queue listbox
// remains the single source of truth.
class thumb_grid : public nana::panel<true>
{
public:
	struct card_t
	{
		std::string num, title, status, site, format, note, ext, filesize, duration;
		const nana::paint::image *thumb {nullptr}, *icon {nullptr};
		bool selected {false}, checked {false};
		// per-item download folder button: 0 = hidden (folder shared by all items), 1 = default folder, 2 = own folder
		int folder_state {0};
		std::wstring folder;
		// real file details once the item is downloaded / found on disk (replace the estimated details line)
		std::string info1, info2;
		// per-item quality button text, e.g. "Default", "1080p", "1080p > 1440p" (shown when folder_state != 0)
		std::wstring quality;
		bool quality_custom {false};
	};

	std::function<size_t()> count;
	std::function<card_t(size_t)> card;
	std::function<void(size_t, bool ctrl, bool shift)> on_click;
	std::function<void(size_t)> on_dblclick;
	std::function<void(size_t, int x, int y)> on_context;
	std::function<void(size_t)> on_need_thumb; // called (from paint) for visible cards that have no thumbnail yet
	// the card's buttons were clicked (x, y: where to pop up a menu, widget coordinates)
	std::function<void(size_t, int x, int y)> on_folder_click, on_quality_click;
	// rubber-band selection / Ctrl+A: the cards to select (additive = keep the current selection)
	std::function<void(const std::vector<size_t> &, bool additive)> on_box_select;

	thumb_grid(nana::window parent)
	{
		create(parent, true);
		dw = std::make_unique<nana::drawing>(*this); // must be created after the widget itself
		dw->draw([this](nana::paint::graphics &g) { paint(g); });

		events().mouse_wheel([this](const nana::arg_wheel &arg)
		{
			scroll_by(arg.upwards ? -util::scale(70) : util::scale(70));
		});

		events().mouse_move([this](const nana::arg_mouse &arg)
		{
			if(box_active)
			{
				if(!arg.is_left_button())
				{
					end_box();
					return;
				}
				// auto-scroll when dragging past the top/bottom edge
				if(arg.pos.y < 0)
					scroll_by(-util::scale(24));
				else if(arg.pos.y > static_cast<int>(size().height))
					scroll_by(util::scale(24));
				box_cur = {arg.pos.x, arg.pos.y + offset};
				update_box_selection();
				nana::api::refresh_window(*this);
				return;
			}
			const auto idx {hit(arg.pos.x, arg.pos.y)};
			const auto bidx {hit_btn(arg.pos.x, arg.pos.y)};
			if(idx != hover || bidx != hover_btn)
			{
				hover = idx;
				hover_btn = bidx;
				nana::api::refresh_window(*this);
			}
		});

		events().mouse_leave([this]
		{
			if(hover != npos || hover_btn != npos)
			{
				hover = hover_btn = npos;
				nana::api::refresh_window(*this);
			}
		});

		events().mouse_down([this](const nana::arg_mouse &arg)
		{
			if(!arg.is_left_button() && arg.button != nana::mouse::right_button)
				return;
			const auto idx {hit(arg.pos.x, arg.pos.y)};
			if(idx != npos && on_click)
			{
				// right click on an already-selected card keeps the multi-selection intact
				if(arg.is_left_button() || !card(idx).selected)
					on_click(idx, arg.ctrl, arg.shift);
			}
			focus();
			if(arg.is_left_button() && idx != npos)
			{
				const auto key {hit_btn(arg.pos.x, arg.pos.y)};
				if(key != npos && key / 2 == idx)
					pending_btn = key; // acted on in mouse_up, so the menu doesn't open mid-click
			}
			if(arg.is_left_button() && idx == npos && on_box_select)
			{
				// click on empty space: start a selection box (like Explorer)
				box_active = true;
				box_moved = false;
				box_additive = arg.ctrl;
				box_origin = box_cur = {arg.pos.x, arg.pos.y + offset};
				box_last.clear();
				nana::api::set_capture(*this, true);
			}
		});

		events().key_press([this](const nana::arg_keyboard &arg)
		{
			if(arg.ctrl && (arg.key == 'A' || arg.key == 'a') && on_box_select && count)
			{
				std::vector<size_t> all(count());
				for(size_t i {0}; i < all.size(); i++)
					all[i] = i;
				on_box_select(all, false);
			}
		});

		events().mouse_up([this](const nana::arg_mouse &arg)
		{
			if(box_active)
			{
				end_box();
				return;
			}
			if(arg.button == nana::mouse::right_button && on_context)
				on_context(hit(arg.pos.x, arg.pos.y), arg.pos.x, arg.pos.y);
			else if(arg.is_left_button() && pending_btn != npos)
			{
				const auto key {pending_btn};
				pending_btn = npos;
				if(hit_btn(arg.pos.x, arg.pos.y) == key)
				{
					const auto &r {buttons.at(key)};
					const int mx {r.x}, my {r.y + static_cast<int>(r.height)};
					if(key % 2 == 0 && on_folder_click)
						on_folder_click(key / 2, mx, my);
					else if(key % 2 == 1 && on_quality_click)
						on_quality_click(key / 2, mx, my);
				}
			}
		});

		events().dbl_click([this](const nana::arg_mouse &arg)
		{
			if(!arg.is_left_button())
				return;
			const auto idx {hit(arg.pos.x, arg.pos.y)};
			if(idx != npos && on_dblclick)
				on_dblclick(idx);
		});

		events().resized([this](const nana::arg_resized &)
		{
			clamp_scroll();
			nana::api::refresh_window(*this);
		});
	}

	void refresh() { nana::api::refresh_window(*this); }

	// make sure the card of the given index is fully visible
	void reveal(size_t idx)
	{
		const auto g {geometry()};
		if(g.cols == 0)
			return;
		const int row {static_cast<int>(idx / g.cols)}, top {g.margin + row * (g.card_h + g.gap)}, bottom {top + g.card_h};
		const int view_h {static_cast<int>(size().height)};
		if(top < offset)
			offset = std::max(0, top - g.margin);
		else if(bottom > offset + view_h)
			offset = bottom - view_h + g.margin;
		clamp_scroll();
		refresh();
	}

private:
	static constexpr size_t npos {static_cast<size_t>(-1)};

	std::unique_ptr<nana::drawing> dw;
	int offset {0};
	size_t hover {npos}, hover_btn {npos}, pending_btn {npos};
	// buttons of the painted cards (widget coordinates); key = card index * 2 + (0 = folder, 1 = quality)
	std::map<size_t, nana::rectangle> buttons;

	// rubber-band selection state (content coordinates, i.e. including the scroll offset)
	bool box_active {false}, box_moved {false}, box_additive {false};
	nana::point box_origin, box_cur;
	std::vector<size_t> box_last;

	nana::rectangle box_rect_content() const
	{
		const int x1 {std::min(box_origin.x, box_cur.x)}, y1 {std::min(box_origin.y, box_cur.y)},
			x2 {std::max(box_origin.x, box_cur.x)}, y2 {std::max(box_origin.y, box_cur.y)};
		return {x1, y1, static_cast<unsigned>(x2 - x1), static_cast<unsigned>(y2 - y1)};
	}

	void update_box_selection()
	{
		const auto br {box_rect_content()};
		if(br.width < 4 && br.height < 4)
			return; // still a click, not a drag
		box_moved = true;
		const auto g {geometry()};
		const size_t n {count ? count() : 0};
		std::vector<size_t> hits;
		for(size_t i {0}; i < n; i++)
		{
			const int row {static_cast<int>(i / g.cols)}, col {static_cast<int>(i % g.cols)};
			const nana::rectangle cr {g.margin + col * (g.card_w + g.gap), g.margin + row * (g.card_h + g.gap),
				static_cast<unsigned>(g.card_w), static_cast<unsigned>(g.card_h)};
			const bool overlap {cr.x < br.x + static_cast<int>(br.width) && br.x < cr.x + static_cast<int>(cr.width) &&
				cr.y < br.y + static_cast<int>(br.height) && br.y < cr.y + static_cast<int>(cr.height)};
			if(overlap)
				hits.push_back(i);
		}
		if(hits != box_last && !hits.empty())
		{
			box_last = hits;
			if(on_box_select)
				on_box_select(hits, box_additive);
		}
	}

	void end_box()
	{
		box_active = false;
		nana::api::release_capture(*this);
		nana::api::refresh_window(*this);
	}

	size_t hit_btn(int x, int y) const
	{
		for(const auto &[key, r] : buttons)
			if(r.is_hit(x, y))
				return key;
		return npos;
	}

	struct geom_t { int cols {1}, card_w {0}, card_h {0}, thumb_w {0}, thumb_h {0}, gap {0}, margin {0}; };

	geom_t geometry() const
	{
		geom_t g;
		g.gap = util::scale(12);
		g.margin = util::scale(16);
		g.card_h = util::scale(160);
		g.thumb_h = g.card_h - 2;
		g.thumb_w = g.thumb_h * 16 / 9;
		const int avail {std::max(1, static_cast<int>(size().width) - 2 * g.margin)};
		const int min_card_w {g.thumb_w + util::scale(300)};
		g.cols = std::max(1, (avail + g.gap) / (min_card_w + g.gap));
		g.card_w = (avail - (g.cols - 1) * g.gap) / g.cols;
		return g;
	}

	int content_height() const
	{
		const auto g {geometry()};
		const size_t n {count ? count() : 0};
		const int rows {static_cast<int>((n + g.cols - 1) / g.cols)};
		return rows ? g.margin * 2 + rows * g.card_h + (rows - 1) * g.gap : 0;
	}

	void clamp_scroll()
	{
		const int maxoff {std::max(0, content_height() - static_cast<int>(size().height))};
		offset = std::clamp(offset, 0, maxoff);
	}

	void scroll_by(int delta)
	{
		const auto prev {offset};
		offset += delta;
		clamp_scroll();
		if(offset != prev)
			nana::api::refresh_window(*this);
	}

	size_t hit(int x, int y) const
	{
		const auto g {geometry()};
		const size_t n {count ? count() : 0};
		x -= g.margin;
		y += offset - g.margin;
		if(x < 0 || y < 0)
			return npos;
		const int col {x / (g.card_w + g.gap)}, row {y / (g.card_h + g.gap)};
		if(col >= g.cols || x % (g.card_w + g.gap) >= g.card_w || y % (g.card_h + g.gap) >= g.card_h)
			return npos;
		const size_t idx {static_cast<size_t>(row) * g.cols + col};
		return idx < n ? idx : npos;
	}

	// longest prefix of str (with an ellipsis if cut) that fits in max_w pixels
	static std::wstring fit(nana::paint::graphics &g, const std::wstring &str, int max_w)
	{
		if(str.empty() || static_cast<int>(g.text_extent_size(str).width) <= max_w)
			return str;
		size_t lo {0}, hi {str.size()};
		while(lo < hi)
		{
			const size_t mid {(lo + hi + 1) / 2};
			if(static_cast<int>(g.text_extent_size(str.substr(0, mid) + L"\u2026").width) <= max_w)
				lo = mid;
			else hi = mid - 1;
		}
		return str.substr(0, lo) + L"\u2026";
	}

	// splits str into at most two lines of max_w pixels
	static std::pair<std::wstring, std::wstring> two_lines(nana::paint::graphics &g, const std::wstring &str, int max_w)
	{
		if(static_cast<int>(g.text_extent_size(str).width) <= max_w)
			return {str, L""};
		size_t lo {1}, hi {str.size()};
		while(lo < hi)
		{
			const size_t mid {(lo + hi + 1) / 2};
			if(static_cast<int>(g.text_extent_size(str.substr(0, mid)).width) <= max_w)
				lo = mid;
			else hi = mid - 1;
		}
		size_t cut {lo};
		const auto space {str.rfind(L' ', cut)};
		if(space != std::wstring::npos && space > cut / 2)
			cut = space;
		auto rest {str.substr(cut)};
		while(!rest.empty() && rest.front() == L' ')
			rest.erase(0, 1);
		return {str.substr(0, cut), fit(g, rest, max_w)};
	}

	void paint(nana::paint::graphics &g)
	{
		using namespace nana;
		using ::widgets::theme;

		const bool dark {theme::is_dark()};
		const color bg {theme::lbbg};
		g.rectangle(true, bg);
		if(!count)
			return;

		const auto n {count()};
		buttons.clear();
		clamp_scroll();
		const auto geo {geometry()};
		const int view_h {static_cast<int>(size().height)};

		const color card_bg {dark ? bg.blend(colors::white, .06) : bg.blend(colors::black, .035)},
			card_hover {dark ? bg.blend(colors::white, .11) : bg.blend(colors::black, .07)},
			text_fg {theme::lbfg},
			dim_fg {theme::lbfg.blend(bg, .5)},
			accent {theme::lbselfg},
			ok_fg {dark ? color {"#7fd48a"} : color {"#2e8b3d"}},
			err_fg {theme::Text_fg_error};

		const paint::font_info f_title {"Segoe UI Semibold", 10}, f_small {"Segoe UI", 9}, f_badge {"Segoe UI Semibold", 8};
		const int pad {util::scale(12)};

		for(size_t i {0}; i < n; i++)
		{
			const int row {static_cast<int>(i / geo.cols)}, col {static_cast<int>(i % geo.cols)};
			const int x {geo.margin + col * (geo.card_w + geo.gap)}, y {geo.margin + row * (geo.card_h + geo.gap) - offset};
			if(y + geo.card_h < 0)
				continue;
			if(y > view_h)
				break;

			auto c {card(i)};
			const rectangle r {x, y, static_cast<unsigned>(geo.card_w), static_cast<unsigned>(geo.card_h)};

			// card colour follows the status (downloading / done / already downloaded / error / stopped / skipped)
			const int skind {::widgets::status_kind(c.status)};
			const color sclr {::widgets::status_color(skind)};
			const color fill {::widgets::status_tint(c.status, (c.selected || i == hover) ? card_hover : card_bg, dark)};
			const color edge {c.selected ? accent : (skind ? card_bg.blend(sclr, dark ? .55 : .45) : color {bg})};
			g.round_rectangle(r, 8, 8, edge, true, fill);
			if(c.selected || skind)
				g.round_rectangle(r, 8, 8, edge, false, edge);

			// thumbnail (left)
			const rectangle tr {x + 1, y + 1, static_cast<unsigned>(geo.thumb_w), static_cast<unsigned>(geo.thumb_h)};
			if(c.thumb && !c.thumb->empty())
			{
				// center-crop to 16:9
				const auto isz {c.thumb->size()};
				rectangle src {0, 0, isz.width, isz.height};
				if(isz.width * 9 > isz.height * 16)
				{
					src.width = isz.height * 16 / 9;
					src.x = static_cast<int>((isz.width - src.width) / 2);
				}
				else
				{
					src.height = isz.width * 9 / 16;
					src.y = static_cast<int>((isz.height - src.height) / 2);
				}
				c.thumb->stretch(src, g, tr);
			}
			else
			{
				g.rectangle(tr, true, dark ? bg.blend(colors::white, .02) : bg.blend(colors::black, .08));
				g.typeface(f_badge);
				const std::wstring label {nana::to_wstring(c.site.empty() ? std::string {"video"} : c.site)};
				const auto ext {g.text_extent_size(label)};
				g.string({tr.x + (static_cast<int>(tr.width) - static_cast<int>(ext.width)) / 2,
					tr.y + (static_cast<int>(tr.height) - static_cast<int>(ext.height)) / 2}, label, dim_fg);
			}

			// small dark badges on the thumbnail: number (top-left), duration (bottom-right)
			g.typeface(f_badge);
			auto badge = [&](const std::wstring &text, int bx, int by, bool right_align)
			{
				const auto bext {g.text_extent_size(text)};
				const int bw {static_cast<int>(bext.width) + util::scale(10)}, bh {static_cast<int>(bext.height) + util::scale(3)};
				const rectangle br {right_align ? bx - bw : bx, by - (right_align ? bh : 0), static_cast<unsigned>(bw), static_cast<unsigned>(bh)};
				g.round_rectangle(br, 5, 5, color {"#000"}, true, color {"#000"});
				g.string({br.x + util::scale(5), br.y + util::scale(1)}, text, colors::white);
			};
			badge(L"#" + nana::to_wstring(c.num), tr.x + util::scale(6), tr.y + util::scale(6), false);
			if(!c.duration.empty())
				badge(nana::to_wstring(c.duration), tr.x + static_cast<int>(tr.width) - util::scale(6), tr.y + static_cast<int>(tr.height) - util::scale(6), true);

			// details (right)
			const int text_x {x + geo.thumb_w + pad}, text_w {geo.card_w - geo.thumb_w - 2 * pad};
			if(text_w < util::scale(60))
				continue;

			g.typeface(f_title);
			const int title_lh {static_cast<int>(g.text_extent_size(L"Ag").height)};
			auto [l1, l2] {two_lines(g, nana::to_wstring(c.title.empty() ? std::string {"---"} : c.title), text_w)};
			const color title_fg {c.checked ? dim_fg : text_fg}; // "checked" items (done/skipped) are dimmed
			int ty {y + util::scale(10)};
			g.string({text_x, ty}, l1, title_fg);
			ty += title_lh;
			if(!l2.empty())
			{
				g.string({text_x, ty}, l2, title_fg);
				ty += title_lh;
			}

			// site (with favicon) + status
			g.typeface(f_small);
			const int small_lh {static_cast<int>(g.text_extent_size(L"Ag").height)};
			ty += util::scale(4);
			int sx {text_x};
			if(c.icon && !c.icon->empty())
			{
				const int isz {util::scale(14)};
				c.icon->stretch(rectangle {c.icon->size()}, g, rectangle {sx, ty + (small_lh - isz) / 2, static_cast<unsigned>(isz), static_cast<unsigned>(isz)});
				sx += isz + util::scale(5);
			}
			std::string status {c.status};
			double pct {-1};
			if(!status.empty() && status.back() == '%')
			{
				try { pct = std::stod(status); } catch(...) {}
			}
			color sfg {text_fg};
			if(status == "done" || status == "complete" || status == "completed" || status == "already downloaded")
				sfg = ok_fg;
			else if(status.find("error") != std::string::npos || status.find("fail") != std::string::npos)
				sfg = err_fg;
			const std::wstring wstatus {nana::to_wstring(status.empty() ? std::string {"queued"} : status)};
			const int status_w {static_cast<int>(g.text_extent_size(wstatus).width)};
			const std::wstring wsite {fit(g, nana::to_wstring(c.site), std::max(0, text_w - (sx - text_x) - status_w - util::scale(14)))};
			g.string({sx, ty}, wsite, dim_fg);
			g.string({text_x + text_w - status_w, ty}, wstatus, sfg);
			ty += small_lh + util::scale(3);

			// format / note / ext / size, the same columns the list view has
			std::wstring details;
			for(const auto *part : {&c.format, &c.note, &c.ext, &c.filesize})
			{
				if(part->empty() || *part == "---")
					continue;
				if(!details.empty())
					details += L"  \u2022  ";
				details += nana::to_wstring(*part);
			}
			if(!c.info1.empty())
			{
				g.string({text_x, ty}, fit(g, nana::to_wstring(c.info1), text_w), text_fg);
				ty += small_lh + util::scale(1);
				if(!c.info2.empty())
					g.string({text_x, ty}, fit(g, nana::to_wstring(c.info2), text_w), dim_fg);
			}
			else if(!details.empty())
				g.string({text_x, ty}, fit(g, details, text_w), dim_fg);

			// per-item buttons (bottom of the details area): download folder (left) and quality (right)
			if(c.folder_state)
			{
				g.typeface(f_small);
				const int bh {util::scale(22)}, bpad {util::scale(8)}, bgap {util::scale(8)};
				const int by {y + geo.card_h - util::scale(18) - bh};
				auto draw_btn = [&](const rectangle &br, const std::wstring &label, bool custom, bool hovered)
				{
					const color bborder {custom ? accent : (dark ? bg.blend(colors::white, .22) : bg.blend(colors::black, .22))};
					const color bfill {hovered ? (dark ? bg.blend(colors::white, .16) : bg.blend(colors::black, .1)) : fill};
					g.round_rectangle(br, 5, 5, bborder, true, bfill);
					g.string({br.x + bpad, br.y + (bh - small_lh) / 2}, label, custom ? accent : text_fg);
				};

				// quality button: as wide as its text, at most half of the row
				const std::wstring qlabel {fit(g, L"Quality: " + (c.quality.empty() ? std::wstring {L"Default"} : c.quality), text_w / 2 - 2 * bpad)};
				const int qw {std::min(text_w / 2, static_cast<int>(g.text_extent_size(qlabel).width) + 2 * bpad)};
				const rectangle qr {text_x + text_w - qw, by, static_cast<unsigned>(qw), static_cast<unsigned>(bh)};

				// folder button: the rest of the row, shows the folder in use
				const int fmax {std::max(util::scale(40), text_w - qw - bgap)};
				const std::wstring flabel {fit(g, (c.folder_state == 2 ? std::wstring {} : std::wstring {L"Default: "}) + c.folder, fmax - 2 * bpad)};
				const int fw {std::min(fmax, static_cast<int>(g.text_extent_size(flabel).width) + 2 * bpad)};
				const rectangle fr {text_x, by, static_cast<unsigned>(fw), static_cast<unsigned>(bh)};

				draw_btn(fr, flabel, c.folder_state == 2, hover_btn == i * 2);
				draw_btn(qr, qlabel, c.quality_custom, hover_btn == i * 2 + 1);
				buttons[i * 2] = fr;
				buttons[i * 2 + 1] = qr;
			}

			if(pct >= 0)
			{
				const int bh {util::scale(4)};
				const rectangle track {text_x, y + geo.card_h - util::scale(12), static_cast<unsigned>(text_w), static_cast<unsigned>(bh)};
				g.rectangle(track, true, dark ? bg.blend(colors::white, .16) : bg.blend(colors::black, .14));
				g.rectangle({track.x, track.y, static_cast<unsigned>(track.width * std::clamp(pct, 0.0, 100.0) / 100.0), track.height}, true, accent);
			}

			if(!c.thumb && on_need_thumb)
				on_need_thumb(i);
		}

		// selection box
		if(box_active && box_moved)
		{
			auto br {box_rect_content()};
			br.y -= offset;
			g.blend(br, accent, .22);
			g.rectangle(br, false, accent);
		}

		// slim scroll indicator
		const int total {content_height()};
		if(total > view_h && view_h > 0)
		{
			const int track_h {view_h - 2 * util::scale(4)};
			const int bar_h {std::max(util::scale(24), track_h * view_h / total)};
			const int bar_y {util::scale(4) + (track_h - bar_h) * offset / std::max(1, total - view_h)};
			const int bw {util::scale(4)};
			const color bar_clr {dark ? bg.blend(colors::white, .35) : bg.blend(colors::black, .35)};
			g.round_rectangle(rectangle {static_cast<int>(size().width) - bw - util::scale(3), bar_y, static_cast<unsigned>(bw), static_cast<unsigned>(bar_h)},
				2, 2, bar_clr, true, bar_clr);
		}
	}
};
