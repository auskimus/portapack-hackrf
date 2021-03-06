/*
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 *
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "ui_navigation.hpp"

#include "portapack.hpp"
#include "event_m0.hpp"

#include "ui_setup.hpp"
#include "ui_debug.hpp"

#include "analog_audio_app.hpp"
#include "ais_app.hpp"
#include "ert_app.hpp"
#include "tpms_app.hpp"
#include "capture_app.hpp"

#include "core_control.hpp"

#include "file.hpp"
#include "png_writer.hpp"

namespace ui {

/* SystemStatusView ******************************************************/

SystemStatusView::SystemStatusView() {
	add_children({
		&button_back,
		&title,
		&button_camera,
		&button_sleep,
		&sd_card_status_view,
	});

	button_back.on_select = [this](Button&){
		if( this->on_back ) {
			this->on_back();
		}
	};

	button_camera.on_select = [this](ImageButton&) {
		this->on_camera();
	};

	button_sleep.on_select = [this](ImageButton&) {
		DisplaySleepMessage message;
		EventDispatcher::send_message(message);
	};
}

void SystemStatusView::set_back_enabled(bool new_value) {
	button_back.set_text(new_value ? back_text_enabled : back_text_disabled);
	button_back.set_focusable(new_value);
}

void SystemStatusView::set_title(const std::string new_value) {
	if( new_value.empty() ) {
		title.set(default_title);
	} else {
		title.set(new_value);
	}
}

void SystemStatusView::on_camera() {
	auto path = next_filename_stem_matching_pattern(u"SCR_????");
	if( path.empty() ) {
		return;
	}

	PNGWriter png;
	auto create_error = png.create(path.replace_extension(u".PNG"));
	if( create_error.is_valid() ) {
		return;
	}

	for(int i=0; i<320; i++) {
		std::array<ColorRGB888, 240> row;
		portapack::display.read_pixels({ 0, i, 240, 1 }, row);
		png.write_scanline(row);
	}
}

/* Navigation ************************************************************/

bool NavigationView::is_top() const {
	return view_stack.size() == 1;
}

View* NavigationView::push_view(std::unique_ptr<View> new_view) {
	free_view();

	const auto p = new_view.get();
	view_stack.emplace_back(std::move(new_view));

	update_view();

	return p;
}

void NavigationView::pop() {
	if( view() == modal_view ) {
		modal_view = nullptr;
	}

	// Can't pop last item from stack.
	if( view_stack.size() > 1 ) {
		free_view();

		view_stack.pop_back();

		update_view();
	}
}

void NavigationView::display_modal(
	const std::string& title,
	const std::string& message
) {
	/* If a modal view is already visible, don't display another */
	if( !modal_view ) {
		modal_view = push<ModalMessageView>(title, message);
	}
}

void NavigationView::free_view() {
	remove_child(view());
}

void NavigationView::update_view() {
	const auto new_view = view_stack.back().get();
	
	add_child(new_view);
	new_view->set_parent_rect({ {0, 0}, size() });
	
	focus();
	set_dirty();

	if( on_view_changed ) {
		on_view_changed(*new_view);
	}
}

Widget* NavigationView::view() const {
	return children_.empty() ? nullptr : children_[0];
}

void NavigationView::focus() {
	if( view() ) {
		view()->focus();
	}
}

/* TransceiversMenuView **************************************************/

TranspondersMenuView::TranspondersMenuView(NavigationView& nav) {
	add_items({
		{ "AIS:  Boats",          [&nav](){ nav.push<AISAppView>(); } },
		{ "ERT:  Utility Meters", [&nav](){ nav.push<ERTAppView>(); } },
		{ "TPMS: Cars",           [&nav](){ nav.push<TPMSAppView>(); } },
	});
	on_left = [&nav](){ nav.pop(); };
}

/* ReceiverMenuView ******************************************************/

ReceiverMenuView::ReceiverMenuView(NavigationView& nav) {
	add_items({
		{ "Audio",        [&nav](){ nav.push<AnalogAudioView>(); } },
		{ "Transponders", [&nav](){ nav.push<TranspondersMenuView>(); } },
	});
	on_left = [&nav](){ nav.pop(); };
}

/* SystemMenuView ********************************************************/

SystemMenuView::SystemMenuView(NavigationView& nav) {
	add_items({
		{ "Receiver", [&nav](){ nav.push<ReceiverMenuView>(); } },
		{ "Capture",  [&nav](){ nav.push<CaptureAppView>(); } },
		{ "Analyze",  [&nav](){ nav.push<NotImplementedView>(); } },
		{ "Setup",    [&nav](){ nav.push<SetupMenuView>(); } },
		{ "About",    [&nav](){ nav.push<AboutView>(); } },
		{ "Debug",    [&nav](){ nav.push<DebugMenuView>(); } },
		{ "HackRF",   [&nav](){ nav.push<HackRFFirmwareView>(); } },
	});
}

/* SystemView ************************************************************/

static constexpr ui::Style style_default {
	.font = ui::font::fixed_8x16,
	.background = ui::Color::black(),
	.foreground = ui::Color::white(),
};

SystemView::SystemView(
	Context& context,
	const Rect parent_rect
) : View { parent_rect },
	context_(context)
{
	set_style(&style_default);

	constexpr ui::Dim status_view_height = 16;

	add_child(&status_view);
	status_view.set_parent_rect({
		{ 0, 0 },
		{ parent_rect.width(), status_view_height }
	});
	status_view.on_back = [this]() {
		this->navigation_view.pop();
	};

	add_child(&navigation_view);
	navigation_view.set_parent_rect({ 0, status_view_height, parent_rect.width(), parent_rect.height() - status_view_height });
	navigation_view.on_view_changed = [this](const View& new_view) {
		this->status_view.set_back_enabled(!this->navigation_view.is_top());
		this->status_view.set_title(new_view.title());
	};

	// Initial view.
	// TODO: Restore from non-volatile memory?
	navigation_view.push<SystemMenuView>();
}

Context& SystemView::context() const {
	return context_;
}

/* HackRFFirmwareView ****************************************************/

HackRFFirmwareView::HackRFFirmwareView(NavigationView& nav) {
	button_yes.on_select = [](Button&){
		EventDispatcher::request_stop();
	};

	button_no.on_select = [&nav](Button&){
		nav.pop();
	};

	add_children({
		&text_title,
		&text_description_1,
		&text_description_2,
		&text_description_3,
		&text_description_4,
		&button_yes,
		&button_no,
	});
}

void HackRFFirmwareView::focus() {
	button_no.focus();
}

/* NotImplementedView ****************************************************/

NotImplementedView::NotImplementedView(NavigationView& nav) {
	button_done.on_select = [&nav](Button&){
		nav.pop();
	};

	add_children({
		&text_title,
		&button_done,
	});
}

void NotImplementedView::focus() {
	button_done.focus();
}

/* ModalMessageView ******************************************************/

ModalMessageView::ModalMessageView(
	NavigationView& nav,
	const std::string& title,
	const std::string& message
) : title_ { title }
{
	button_done.on_select = [&nav](Button&){
		nav.pop();
	};

	add_children({
		&text_message,
		&button_done,
	});

	text_message.set(message);
	
	const int text_message_width = message.size() * 8;
 	text_message.set_parent_rect({
 		(240 - text_message_width) / 2, 7 * 16,
 		text_message_width, 16
 	});
}

void ModalMessageView::focus() {
	button_done.focus();
}

} /* namespace ui */
