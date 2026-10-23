#pragma once
#include <functional>
#include <string>
#include <algorithm>
#include <memory>
#include <nana/gui.hpp>
#include <nana/gui/drawing.hpp>
#include <nana/gui/widgets/panel.hpp>
#include "widgets.hpp"
#include "util.hpp"


// Card-style "thumbnail view" of the download queue. It owns no data: the GUI supplies the
// number of cards and the contents of each card through callbacks, and handles user actions
// (selection, double click, context menu) through the callbacks too, so the queue listbox
// remains the single source of truth.
class thumb_grid : public nana::panel<true>
{
public:
	struct card_t
	{
		std::string num, title, status, site;
		const nana::paint::image *thumb {nullptr};
		bool selected {false}, checked {false};
	};

	std::function<size_t()> count;
	std::function<card_t(size_t)> card;
	std::function<void(size_t, bool ctrl, bool shift)> on_click;
	std::function<void(size_t)> on_dblclick;
	std::function<void(size_t, int x, int y)> on_context;
	std::function<void(size_t)> on_need_thumb; // called (from paint) for visible cards that have no thumbnail yet

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
			const auto idx {hit(arg.pos.x, arg.pos.y)};
			if(idx != hover)
			{
				hover = idx;
				nana::api::refresh_window(*this);
			}
		});

		events().mouse_leave([this]
		{
			if(hover != npos)
			{
				hover = npos;
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
		});

		events().mouse_up([this](const nana::arg_mouse &arg)
		{
			if(arg.button == nana::mouse::right_button && on_context)
				on_context(hit(arg.pos.x, arg.pos.y), arg.pos.x, arg.pos.y);
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

	struct geom_t { int cols {1}, card_w {0}, card_h {0}, thumb_h {0}, gap {0}, margin {0}; };

	std::unique_ptr<nana::drawing> dw;
	int offset {0};
	size_t hover {npos};

	geom_t geometry() const
	{
		geom_t g;
		g.gap = util::scale(14);
		g.margin = util::scale(16);
		const int avail {std::max(1, static_cast<int>(size().width) - 2 * g.margin)};
		const int target {util::scale(236)};
		g.cols = std::max(1, (avail + g.gap) / (target + g.gap));
		g.card_w = (avail - (g.cols - 1) * g.gap) / g.cols;
		g.thumb_h = g.card_w * 9 / 16;
		g.card_h = g.thumb_h + util::scale(74);
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
			if(static_cast<int>(g.text_extent_size(str.substr(0, mid) + L"…").width) <= max_w)
				lo = mid;
			else hi = mid - 1;
		}
		return str.substr(0, lo) + L"…";
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
		clamp_scroll();
		const auto geo {geometry()};
		const int view_h {static_cast<int>(size().height)};

		const color card_bg {dark ? bg.blend(colors::white, .06) : bg.blend(colors::black, .035)},
			card_hover {dark ? bg.blend(colors::white, .11) : bg.blend(colors::black, .07)},
			text_fg {theme::lbfg},
			dim_fg {theme::lbfg.blend(bg, .45)},
			accent {theme::lbselfg},
			ok_fg {dark ? color {"#7fd48a"} : color {"#2e8b3d"}},
			err_fg {theme::Text_fg_error};

		const paint::font_info f_title {"Segoe UI", 9}, f_small {"Segoe UI", 8}, f_badge {"Segoe UI Semibold", 8};
		const int pad {util::scale(10)};

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

			g.round_rectangle(r, 8, 8, c.selected ? accent : color {bg}, true, c.selected ? card_hover : (i == hover ? card_hover : card_bg));
			if(c.selected)
				g.round_rectangle(r, 8, 8, accent, false, accent);

			// thumbnail area
			const rectangle tr {x + 1, y + 1, static_cast<unsigned>(geo.card_w - 2), static_cast<unsigned>(geo.thumb_h)};
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

			// number badge
			g.typeface(f_badge);
			const std::wstring badge {L"#" + nana::to_wstring(c.num)};
			const auto bext {g.text_extent_size(badge)};
			const rectangle br {tr.x + util::scale(6), tr.y + util::scale(6), bext.width + util::scale(12), bext.height + util::scale(4)};
			g.round_rectangle(br, 6, 6, color {"#000"}, true, color {"#000"});
			g.string({br.x + util::scale(6), br.y + util::scale(2)}, badge, colors::white);

			// title (up to two lines)
			g.typeface(f_title);
			const int text_x {x + pad}, text_w {geo.card_w - 2 * pad};
			const int line_h {static_cast<int>(g.text_extent_size(L"Ag").height)};
			auto [l1, l2] {two_lines(g, nana::to_wstring(c.title.empty() ? std::string {"---"} : c.title), text_w)};
			int ty {y + geo.thumb_h + util::scale(8)};
			const color title_fg {c.checked ? dim_fg : text_fg}; // "checked" items (done/skipped) are dimmed
			g.string({text_x, ty}, l1, title_fg);
			if(!l2.empty())
				g.string({text_x, ty + line_h}, l2, title_fg);

			// status line + progress bar
			g.typeface(f_small);
			const int sy {y + geo.card_h - util::scale(24)};
			std::string status {c.status};
			color sfg {dim_fg};
			double pct {-1};
			if(!status.empty() && status.back() == '%')
			{
				try { pct = std::stod(status); } catch(...) {}
			}
			if(status == "done" || status == "complete" || status == "completed")
				sfg = ok_fg;
			else if(status.find("error") != std::string::npos || status.find("fail") != std::string::npos)
				sfg = err_fg;
			else if(pct >= 0)
				sfg = text_fg;
			g.string({text_x, sy}, fit(g, nana::to_wstring(status.empty() ? std::string {"queued"} : status), text_w), sfg);

			if(pct >= 0)
			{
				const int bh {util::scale(3)};
				const rectangle track {x + pad, y + geo.card_h - util::scale(8), static_cast<unsigned>(text_w), static_cast<unsigned>(bh)};
				g.rectangle(track, true, dark ? bg.blend(colors::white, .16) : bg.blend(colors::black, .14));
				g.rectangle({track.x, track.y, static_cast<unsigned>(track.width * std::clamp(pct, 0.0, 100.0) / 100.0), track.height}, true, accent);
			}

			if(!c.thumb && on_need_thumb)
				on_need_thumb(i);
		}

		// slim scroll indicator
		const int total {content_height()};
		if(total > view_h && view_h > 0)
		{
			const int track_h {view_h - 2 * util::scale(4)};
			const int bar_h {std::max(util::scale(24), track_h * view_h / total)};
			const int bar_y {util::scale(4) + (track_h - bar_h) * offset / std::max(1, total - view_h)};
			const int bw {util::scale(4)};
			g.round_rectangle(rectangle {static_cast<int>(size().width) - bw - util::scale(3), bar_y, static_cast<unsigned>(bw), static_cast<unsigned>(bar_h)},
				2, 2, dark ? bg.blend(colors::white, .35) : bg.blend(colors::black, .35), true, dark ? bg.blend(colors::white, .35) : bg.blend(colors::black, .35));
		}
	}
};
