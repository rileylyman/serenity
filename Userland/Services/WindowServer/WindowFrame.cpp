/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ClientConnection.h"
#include <AK/Badge.h>
#include <LibGfx/Font.h>
#include <LibGfx/Painter.h>
#include <LibGfx/StylePainter.h>
#include <LibGfx/WindowTheme.h>
#include <WindowServer/Button.h>
#include <WindowServer/Compositor.h>
#include <WindowServer/Event.h>
#include <WindowServer/MultiScaleBitmaps.h>
#include <WindowServer/Screen.h>
#include <WindowServer/Window.h>
#include <WindowServer/WindowFrame.h>
#include <WindowServer/WindowManager.h>

namespace WindowServer {

static Gfx::WindowTheme::WindowType to_theme_window_type(WindowType type)
{
    switch (type) {
    case WindowType::Normal:
        return Gfx::WindowTheme::WindowType::Normal;
    case WindowType::ToolWindow:
        return Gfx::WindowTheme::WindowType::ToolWindow;
    case WindowType::Notification:
        return Gfx::WindowTheme::WindowType::Notification;
    default:
        return Gfx::WindowTheme::WindowType::Other;
    }
}

static RefPtr<MultiScaleBitmaps> s_minimize_icon;
static RefPtr<MultiScaleBitmaps> s_maximize_icon;
static RefPtr<MultiScaleBitmaps> s_restore_icon;
static RefPtr<MultiScaleBitmaps> s_close_icon;
static RefPtr<MultiScaleBitmaps> s_close_modified_icon;

static RefPtr<MultiScaleBitmaps> s_active_window_shadow;
static RefPtr<MultiScaleBitmaps> s_inactive_window_shadow;
static RefPtr<MultiScaleBitmaps> s_menu_shadow;
static RefPtr<MultiScaleBitmaps> s_taskbar_shadow;
static RefPtr<MultiScaleBitmaps> s_tooltip_shadow;
static String s_last_active_window_shadow_path;
static String s_last_inactive_window_shadow_path;
static String s_last_menu_shadow_path;
static String s_last_taskbar_shadow_path;
static String s_last_tooltip_shadow_path;

static Gfx::IntRect frame_rect_for_window(Window& window, const Gfx::IntRect& rect)
{
    if (window.is_frameless())
        return rect;
    int menu_row_count = (window.menubar() && window.should_show_menubar()) ? 1 : 0;
    return Gfx::WindowTheme::current().frame_rect_for_window(to_theme_window_type(window.type()), rect, WindowManager::the().palette(), menu_row_count);
}

WindowFrame::WindowFrame(Window& window)
    : m_window(window)
{
    // Because Window constructs a WindowFrame during its construction, we need
    // to be careful and defer doing initialization that assumes a fully
    // constructed Window. It is fully constructed when Window notifies us with
    // a call to WindowFrame::window_was_constructed.
}

void WindowFrame::window_was_constructed(Badge<Window>)
{
    {
        auto button = make<Button>(*this, [this](auto&) {
            m_window.handle_window_menu_action(WindowMenuAction::Close);
        });
        m_close_button = button.ptr();
        m_buttons.append(move(button));
    }

    if (m_window.is_resizable()) {
        auto button = make<Button>(*this, [this](auto&) {
            m_window.handle_window_menu_action(WindowMenuAction::MaximizeOrRestore);
        });
        button->on_middle_click = [&](auto&) {
            m_window.set_vertically_maximized();
        };
        m_maximize_button = button.ptr();
        m_buttons.append(move(button));
    }

    if (m_window.is_minimizable()) {
        auto button = make<Button>(*this, [this](auto&) {
            m_window.handle_window_menu_action(WindowMenuAction::MinimizeOrUnminimize);
        });
        m_minimize_button = button.ptr();
        m_buttons.append(move(button));
    }

    set_button_icons();

    m_has_alpha_channel = Gfx::WindowTheme::current().frame_uses_alpha(window_state_for_theme(), WindowManager::the().palette());
}

WindowFrame::~WindowFrame()
{
}

void WindowFrame::set_button_icons()
{
    set_dirty();
    if (m_window.is_frameless())
        return;

    m_close_button->set_icon(m_window.is_modified() ? *s_close_modified_icon : *s_close_icon);
    if (m_window.is_minimizable())
        m_minimize_button->set_icon(s_minimize_icon);
    if (m_window.is_resizable())
        m_maximize_button->set_icon(m_window.is_maximized() ? *s_restore_icon : *s_maximize_icon);
}

void WindowFrame::reload_config()
{
    String icons_path = WindowManager::the().palette().title_button_icons_path();

    auto reload_icon = [&](RefPtr<MultiScaleBitmaps>& icon, StringView const& path, StringView const& default_path) {
        StringBuilder full_path;
        full_path.append(icons_path);
        full_path.append(path);
        if (icon)
            icon->load(full_path.to_string(), default_path);
        else
            icon = MultiScaleBitmaps::create(full_path.to_string(), default_path);
    };

    reload_icon(s_minimize_icon, "window-minimize.png", "/res/icons/16x16/downward-triangle.png");
    reload_icon(s_maximize_icon, "window-maximize.png", "/res/icons/16x16/upward-triangle.png");
    reload_icon(s_restore_icon, "window-restore.png", "/res/icons/16x16/window-restore.png");
    reload_icon(s_close_icon, "window-close.png", "/res/icons/16x16/window-close.png");
    reload_icon(s_close_modified_icon, "window-close-modified.png", "/res/icons/16x16/window-close-modified.png");

    auto load_shadow = [](const String& path, String& last_path, RefPtr<MultiScaleBitmaps>& shadow_bitmap) {
        if (path.is_empty()) {
            last_path = String::empty();
            shadow_bitmap = nullptr;
        } else if (!shadow_bitmap || last_path != path) {
            if (shadow_bitmap)
                shadow_bitmap->load(path);
            else
                shadow_bitmap = MultiScaleBitmaps::create(path);
            if (shadow_bitmap)
                last_path = path;
            else
                last_path = String::empty();
        }
    };
    load_shadow(WindowManager::the().palette().active_window_shadow_path(), s_last_active_window_shadow_path, s_active_window_shadow);
    load_shadow(WindowManager::the().palette().inactive_window_shadow_path(), s_last_inactive_window_shadow_path, s_inactive_window_shadow);
    load_shadow(WindowManager::the().palette().menu_shadow_path(), s_last_menu_shadow_path, s_menu_shadow);
    load_shadow(WindowManager::the().palette().taskbar_shadow_path(), s_last_taskbar_shadow_path, s_taskbar_shadow);
    load_shadow(WindowManager::the().palette().tooltip_shadow_path(), s_last_tooltip_shadow_path, s_tooltip_shadow);
}

MultiScaleBitmaps* WindowFrame::shadow_bitmap() const
{
    if (m_window.is_frameless())
        return nullptr;
    switch (m_window.type()) {
    case WindowType::Desktop:
        return nullptr;
    case WindowType::Menu:
        return s_menu_shadow;
    case WindowType::Tooltip:
        return s_tooltip_shadow;
    case WindowType::Taskbar:
        return s_taskbar_shadow;
    case WindowType::AppletArea:
        return nullptr;
    default:
        if (auto* highlight_window = WindowManager::the().highlight_window())
            return highlight_window == &m_window ? s_active_window_shadow : s_inactive_window_shadow;
        return m_window.is_active() ? s_active_window_shadow : s_inactive_window_shadow;
    }
}

bool WindowFrame::has_shadow() const
{
    if (auto* shadow_bitmap = this->shadow_bitmap(); shadow_bitmap && shadow_bitmap->format() == Gfx::BitmapFormat::BGRA8888)
        return true;
    return false;
}

void WindowFrame::did_set_maximized(Badge<Window>, bool maximized)
{
    VERIFY(m_maximize_button);
    m_maximize_button->set_icon(maximized ? *s_restore_icon : *s_maximize_icon);
}

Gfx::IntRect WindowFrame::menubar_rect() const
{
    if (!m_window.menubar() || !m_window.should_show_menubar())
        return {};
    return Gfx::WindowTheme::current().menubar_rect(to_theme_window_type(m_window.type()), m_window.rect(), WindowManager::the().palette(), menu_row_count());
}

Gfx::IntRect WindowFrame::titlebar_rect() const
{
    return Gfx::WindowTheme::current().titlebar_rect(to_theme_window_type(m_window.type()), m_window.rect(), WindowManager::the().palette());
}

Gfx::IntRect WindowFrame::titlebar_icon_rect() const
{
    return Gfx::WindowTheme::current().titlebar_icon_rect(to_theme_window_type(m_window.type()), m_window.rect(), WindowManager::the().palette());
}

Gfx::IntRect WindowFrame::titlebar_text_rect() const
{
    return Gfx::WindowTheme::current().titlebar_text_rect(to_theme_window_type(m_window.type()), m_window.rect(), WindowManager::the().palette());
}

Gfx::WindowTheme::WindowState WindowFrame::window_state_for_theme() const
{
    auto& wm = WindowManager::the();

    if (m_flash_timer && m_flash_timer->is_active())
        return m_flash_counter & 1 ? Gfx::WindowTheme::WindowState::Active : Gfx::WindowTheme::WindowState::Inactive;

    if (&m_window == wm.highlight_window())
        return Gfx::WindowTheme::WindowState::Highlighted;
    if (&m_window == wm.m_move_window)
        return Gfx::WindowTheme::WindowState::Moving;
    if (wm.is_active_window_or_accessory(m_window))
        return Gfx::WindowTheme::WindowState::Active;
    return Gfx::WindowTheme::WindowState::Inactive;
}

void WindowFrame::paint_notification_frame(Gfx::Painter& painter)
{
    auto palette = WindowManager::the().palette();
    Gfx::WindowTheme::current().paint_notification_frame(painter, m_window.rect(), palette, m_buttons.last().relative_rect());
}

void WindowFrame::paint_tool_window_frame(Gfx::Painter& painter)
{
    auto palette = WindowManager::the().palette();
    auto leftmost_button_rect = m_buttons.is_empty() ? Gfx::IntRect() : m_buttons.last().relative_rect();
    Gfx::WindowTheme::current().paint_tool_window_frame(painter, window_state_for_theme(), m_window.rect(), m_window.computed_title(), palette, leftmost_button_rect);
}

void WindowFrame::paint_menubar(Gfx::Painter& painter)
{
    auto& wm = WindowManager::the();
    auto& font = wm.font();
    auto palette = wm.palette();
    auto menubar_rect = this->menubar_rect();

    painter.fill_rect(menubar_rect, palette.window());

    Gfx::PainterStateSaver saver(painter);
    painter.add_clip_rect(menubar_rect);
    painter.translate(menubar_rect.location());

    m_window.menubar()->for_each_menu([&](Menu& menu) {
        auto text_rect = menu.rect_in_window_menubar();
        Color text_color = palette.window_text();
        auto is_open = menu.is_open();
        if (is_open)
            text_rect.translate_by(1, 1);
        bool paint_as_pressed = is_open;
        bool paint_as_hovered = !paint_as_pressed && &menu == MenuManager::the().hovered_menu();
        if (paint_as_pressed || paint_as_hovered) {
            Gfx::StylePainter::paint_button(painter, menu.rect_in_window_menubar(), palette, Gfx::ButtonStyle::Coolbar, paint_as_pressed, paint_as_hovered);
        }
        painter.draw_ui_text(text_rect, menu.name(), font, Gfx::TextAlignment::Center, text_color);
        return IterationDecision::Continue;
    });
}

void WindowFrame::paint_normal_frame(Gfx::Painter& painter)
{
    auto palette = WindowManager::the().palette();
    auto leftmost_button_rect = m_buttons.is_empty() ? Gfx::IntRect() : m_buttons.last().relative_rect();
    Gfx::WindowTheme::current().paint_normal_frame(painter, window_state_for_theme(), m_window.rect(), m_window.computed_title(), m_window.icon(), palette, leftmost_button_rect, menu_row_count(), m_window.is_modified());

    if (m_window.menubar() && m_window.should_show_menubar())
        paint_menubar(painter);
}

void WindowFrame::paint(Screen& screen, Gfx::Painter& painter, const Gfx::IntRect& rect)
{
    if (auto* cached = render_to_cache(screen))
        cached->paint(*this, painter, rect);
}

void WindowFrame::PerScaleRenderedCache::paint(WindowFrame& frame, Gfx::Painter& painter, const Gfx::IntRect& rect)
{
    auto frame_rect = frame.unconstrained_render_rect();
    auto window_rect = frame.window().rect();
    if (m_top_bottom) {
        auto top_bottom_height = frame_rect.height() - window_rect.height();
        if (m_bottom_y > 0) {
            // We have a top piece
            auto src_rect = rect.intersected({ frame_rect.location(), { frame_rect.width(), m_bottom_y } });
            if (!src_rect.is_empty())
                painter.blit(src_rect.location(), *m_top_bottom, src_rect.translated(-frame_rect.location()), frame.opacity());
        }
        if (m_bottom_y < top_bottom_height) {
            // We have a bottom piece
            Gfx::IntRect rect_in_frame { frame_rect.x(), window_rect.bottom() + 1, frame_rect.width(), top_bottom_height - m_bottom_y };
            auto src_rect = rect.intersected(rect_in_frame);
            if (!src_rect.is_empty())
                painter.blit(src_rect.location(), *m_top_bottom, src_rect.translated(-rect_in_frame.x(), -rect_in_frame.y() + m_bottom_y), frame.opacity());
        }
    }

    if (m_left_right) {
        auto left_right_width = frame_rect.width() - window_rect.width();
        if (m_right_x > 0) {
            // We have a left piece
            Gfx::IntRect rect_in_frame { frame_rect.x(), window_rect.y(), m_right_x, window_rect.height() };
            auto src_rect = rect.intersected(rect_in_frame);
            if (!src_rect.is_empty())
                painter.blit(src_rect.location(), *m_left_right, src_rect.translated(-rect_in_frame.location()), frame.opacity());
        }
        if (m_right_x < left_right_width) {
            // We have a right piece
            Gfx::IntRect rect_in_frame { window_rect.right() + 1, window_rect.y(), left_right_width - m_right_x, window_rect.height() };
            auto src_rect = rect.intersected(rect_in_frame);
            if (!src_rect.is_empty())
                painter.blit(src_rect.location(), *m_left_right, src_rect.translated(-rect_in_frame.x() + m_right_x, -rect_in_frame.y()), frame.opacity());
        }
    }
}

void WindowFrame::render(Screen& screen, Gfx::Painter& painter)
{
    if (m_window.is_frameless())
        return;

    if (m_window.type() == WindowType::Notification)
        paint_notification_frame(painter);
    else if (m_window.type() == WindowType::Normal)
        paint_normal_frame(painter);
    else if (m_window.type() == WindowType::ToolWindow)
        paint_tool_window_frame(painter);
    else
        return;

    for (auto& button : m_buttons)
        button.paint(screen, painter);
}

void WindowFrame::theme_changed()
{
    m_rendered_cache = {};

    layout_buttons();
    set_button_icons();

    m_has_alpha_channel = Gfx::WindowTheme::current().frame_uses_alpha(window_state_for_theme(), WindowManager::the().palette());
}

auto WindowFrame::render_to_cache(Screen& screen) -> PerScaleRenderedCache*
{
    auto scale = screen.scale_factor();
    PerScaleRenderedCache* rendered_cache;
    auto cached_it = m_rendered_cache.find(scale);
    if (cached_it == m_rendered_cache.end()) {
        auto new_rendered_cache = make<PerScaleRenderedCache>();
        rendered_cache = new_rendered_cache.ptr();
        m_rendered_cache.set(scale, move(new_rendered_cache));
    } else {
        rendered_cache = cached_it->value.ptr();
    }
    rendered_cache->render(*this, screen);
    return rendered_cache;
}

void WindowFrame::PerScaleRenderedCache::render(WindowFrame& frame, Screen& screen)
{
    if (!m_dirty)
        return;
    m_dirty = false;

    auto scale = screen.scale_factor();

    auto frame_rect = frame.rect();

    auto frame_rect_including_shadow = frame_rect;
    auto* shadow_bitmap = frame.shadow_bitmap();
    Gfx::IntPoint shadow_offset;

    if (shadow_bitmap) {
        auto total_shadow_size = shadow_bitmap->bitmap(screen.scale_factor()).height();
        frame_rect_including_shadow.inflate(total_shadow_size, total_shadow_size);
        auto offset = total_shadow_size / 2;
        shadow_offset = { offset, offset };
    }

    auto window_rect = frame.window().rect();

    // TODO: if we stop using a scaling factor we should clear cached bitmaps from this map
    static HashMap<int, RefPtr<Gfx::Bitmap>> s_tmp_bitmap_cache;
    Gfx::Bitmap* tmp_bitmap;
    {
        auto tmp_it = s_tmp_bitmap_cache.find(scale);
        if (tmp_it == s_tmp_bitmap_cache.end() || !tmp_it->value->size().contains(frame_rect_including_shadow.size())) {
            // Explicitly clear the old bitmap first so this works on machines with very little memory
            if (tmp_it != s_tmp_bitmap_cache.end())
                tmp_it->value = nullptr;

            auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, frame_rect_including_shadow.size(), scale);
            if (!bitmap) {
                s_tmp_bitmap_cache.remove(scale);
                dbgln("Could not create bitmap of size {}", frame_rect_including_shadow.size());
                return;
            }
            tmp_bitmap = bitmap.ptr();
            if (tmp_it != s_tmp_bitmap_cache.end())
                tmp_it->value = bitmap.release_nonnull();
            else
                s_tmp_bitmap_cache.set(scale, bitmap.release_nonnull());
        } else {
            tmp_bitmap = tmp_it->value.ptr();
        }
    }

    VERIFY(tmp_bitmap);

    auto top_bottom_height = frame_rect_including_shadow.height() - window_rect.height();
    auto left_right_width = frame_rect_including_shadow.width() - window_rect.width();

    if (!m_top_bottom || m_top_bottom->width() != frame_rect_including_shadow.width() || m_top_bottom->height() != top_bottom_height || m_top_bottom->scale() != scale) {
        if (top_bottom_height > 0)
            m_top_bottom = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { frame_rect_including_shadow.width(), top_bottom_height }, scale);
        else
            m_top_bottom = nullptr;
        m_shadow_dirty = true;
    }
    if (!m_left_right || m_left_right->height() != frame_rect_including_shadow.height() || m_left_right->width() != left_right_width || m_left_right->scale() != scale) {
        if (left_right_width > 0)
            m_left_right = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { left_right_width, frame_rect_including_shadow.height() }, scale);
        else
            m_left_right = nullptr;
        m_shadow_dirty = true;
    }

    auto& frame_rect_to_update = m_shadow_dirty ? frame_rect_including_shadow : frame_rect;
    Gfx::IntPoint update_location(m_shadow_dirty ? Gfx::IntPoint { 0, 0 } : shadow_offset);

    Gfx::Painter painter(*tmp_bitmap);

    // Clear the frame area, not including the window content area, which we don't care about
    for (auto& rect : frame_rect_to_update.shatter(window_rect))
        painter.clear_rect({ rect.location() - frame_rect_to_update.location(), rect.size() }, { 255, 255, 255, 0 });

    if (m_shadow_dirty && shadow_bitmap)
        frame.paint_simple_rect_shadow(painter, { { 0, 0 }, frame_rect_including_shadow.size() }, shadow_bitmap->bitmap(screen.scale_factor()));

    {
        Gfx::PainterStateSaver save(painter);
        painter.translate(shadow_offset);
        frame.render(screen, painter);
    }

    if (m_top_bottom && top_bottom_height > 0) {
        m_bottom_y = window_rect.y() - frame_rect_including_shadow.y();
        VERIFY(m_bottom_y >= 0);

        Gfx::Painter top_bottom_painter(*m_top_bottom);
        top_bottom_painter.add_clip_rect({ update_location, { frame_rect_to_update.width(), top_bottom_height - update_location.y() - (frame_rect_including_shadow.bottom() - frame_rect_to_update.bottom()) } });
        if (m_bottom_y > 0)
            top_bottom_painter.blit({ 0, 0 }, *tmp_bitmap, { 0, 0, frame_rect_including_shadow.width(), m_bottom_y }, 1.0, false);
        if (m_bottom_y < top_bottom_height)
            top_bottom_painter.blit({ 0, m_bottom_y }, *tmp_bitmap, { 0, frame_rect_including_shadow.height() - (frame_rect_including_shadow.bottom() - window_rect.bottom()), frame_rect_including_shadow.width(), top_bottom_height - m_bottom_y }, 1.0, false);
    } else {
        m_bottom_y = 0;
    }

    if (left_right_width > 0) {
        m_right_x = window_rect.x() - frame_rect_including_shadow.x();
        VERIFY(m_right_x >= 0);

        Gfx::Painter left_right_painter(*m_left_right);
        left_right_painter.add_clip_rect({ update_location, { left_right_width - update_location.x() - (frame_rect_including_shadow.right() - frame_rect_to_update.right()), window_rect.height() } });
        if (m_right_x > 0)
            left_right_painter.blit({ 0, 0 }, *tmp_bitmap, { 0, m_bottom_y, m_right_x, window_rect.height() }, 1.0, false);
        if (m_right_x < left_right_width)
            left_right_painter.blit({ m_right_x, 0 }, *tmp_bitmap, { (window_rect.right() - frame_rect_including_shadow.x()) + 1, m_bottom_y, frame_rect_including_shadow.width() - (frame_rect_including_shadow.right() - window_rect.right()), window_rect.height() }, 1.0, false);
    } else {
        m_right_x = 0;
    }

    m_shadow_dirty = false;
}

void WindowFrame::set_opacity(float opacity)
{
    if (m_opacity == opacity)
        return;
    bool was_opaque = is_opaque();
    m_opacity = opacity;
    if (was_opaque != is_opaque())
        Compositor::the().invalidate_occlusions();
    Compositor::the().invalidate_screen(render_rect());
    WindowManager::the().notify_opacity_changed(m_window);
}

Gfx::IntRect WindowFrame::inflated_for_shadow(const Gfx::IntRect& frame_rect) const
{
    if (auto* shadow = shadow_bitmap()) {
        auto total_shadow_size = shadow->default_bitmap().height();
        return frame_rect.inflated(total_shadow_size, total_shadow_size);
    }
    return frame_rect;
}

Gfx::IntRect WindowFrame::rect() const
{
    return frame_rect_for_window(m_window, m_window.rect());
}

Gfx::IntRect WindowFrame::constrained_render_rect_to_screen(const Gfx::IntRect& render_rect) const
{
    if (m_window.is_maximized() || m_window.tiled() != WindowTileType::None)
        return render_rect.intersected(Screen::closest_to_rect(rect()).rect());
    return render_rect;
}

Gfx::IntRect WindowFrame::render_rect() const
{
    return constrained_render_rect_to_screen(inflated_for_shadow(rect()));
}

Gfx::IntRect WindowFrame::unconstrained_render_rect() const
{
    return inflated_for_shadow(rect());
}

Gfx::DisjointRectSet WindowFrame::opaque_render_rects() const
{
    if (has_alpha_channel()) {
        if (m_window.is_opaque())
            return constrained_render_rect_to_screen(m_window.rect());
        return {};
    }
    if (m_window.is_opaque())
        return constrained_render_rect_to_screen(rect());
    Gfx::DisjointRectSet opaque_rects;
    opaque_rects.add_many(constrained_render_rect_to_screen(rect()).shatter(m_window.rect()));
    return opaque_rects;
}

Gfx::DisjointRectSet WindowFrame::transparent_render_rects() const
{
    if (has_alpha_channel()) {
        if (m_window.is_opaque()) {
            Gfx::DisjointRectSet transparent_rects;
            transparent_rects.add_many(render_rect().shatter(m_window.rect()));
            return transparent_rects;
        }
        return render_rect();
    }

    auto total_render_rect = render_rect();
    Gfx::DisjointRectSet transparent_rects;
    if (has_shadow())
        transparent_rects.add_many(total_render_rect.shatter(rect()));
    if (!m_window.is_opaque())
        transparent_rects.add(m_window.rect().intersected(total_render_rect));
    return transparent_rects;
}

void WindowFrame::invalidate_titlebar()
{
    set_dirty();
    invalidate(titlebar_rect());
}

void WindowFrame::invalidate()
{
    auto frame_rect = render_rect();
    invalidate(Gfx::IntRect { frame_rect.location() - m_window.position(), frame_rect.size() });
    m_window.invalidate(true, true);
}

void WindowFrame::invalidate(Gfx::IntRect relative_rect)
{
    auto frame_rect = rect();
    auto window_rect = m_window.rect();
    relative_rect.translate_by(frame_rect.x() - window_rect.x(), frame_rect.y() - window_rect.y());
    set_dirty();
    m_window.invalidate(relative_rect, true);
}

void WindowFrame::window_rect_changed(const Gfx::IntRect& old_rect, const Gfx::IntRect& new_rect)
{
    layout_buttons();

    auto new_frame_rect = constrained_render_rect_to_screen(frame_rect_for_window(m_window, new_rect));
    set_dirty(true);
    auto& compositor = Compositor::the();

    {
        // Invalidate the areas outside of the new rect. Use the last computed occlusions for this purpose
        // as we can't reliably calculate the previous frame rect anymore. The window state (e.g. maximized
        // or tiled) may affect the calculations and it may have already been changed by the time we get
        // called here.
        auto invalidate_opaque = m_window.opaque_rects().shatter(new_frame_rect);
        for (auto& rect : invalidate_opaque.rects())
            compositor.invalidate_screen(rect);
        auto invalidate_transparent = m_window.transparency_rects().shatter(new_frame_rect);
        for (auto& rect : invalidate_transparent.rects())
            compositor.invalidate_screen(rect);
    }

    compositor.invalidate_occlusions();

    WindowManager::the().notify_rect_changed(m_window, old_rect, new_rect);
}

void WindowFrame::layout_buttons()
{
    auto button_rects = Gfx::WindowTheme::current().layout_buttons(to_theme_window_type(m_window.type()), m_window.rect(), WindowManager::the().palette(), m_buttons.size());
    for (size_t i = 0; i < m_buttons.size(); i++)
        m_buttons[i].set_relative_rect(button_rects[i]);
}

Optional<HitTestResult> WindowFrame::hit_test(Gfx::IntPoint const& position)
{
    if (m_window.is_frameless() || m_window.is_fullscreen())
        return {};
    if (!constrained_render_rect_to_screen(rect()).contains(position)) {
        // Checking just frame_rect is not enough. If we constrain rendering
        // a window to one screen (e.g. when it's maximized or tiled) so that
        // the frame doesn't bleed into the adjacent screen(s), then we need
        // to also check that we're within these bounds.
        return {};
    }
    auto window_rect = m_window.rect();
    if (window_rect.contains(position))
        return {};

    auto* screen = Screen::find_by_location(position);
    if (!screen)
        return {};
    auto* cached = render_to_cache(*screen);
    if (!cached)
        return {};

    auto window_relative_position = position.translated(-unconstrained_render_rect().location());
    return cached->hit_test(*this, position, window_relative_position);
}

Optional<HitTestResult> WindowFrame::PerScaleRenderedCache::hit_test(WindowFrame& frame, Gfx::IntPoint const& position, Gfx::IntPoint const& window_relative_position)
{
    HitTestResult result {
        .window = frame.window(),
        .screen_position = position,
        .window_relative_position = window_relative_position,
        .is_frame_hit = true,
    };

    u8 alpha_threshold = Gfx::WindowTheme::current().frame_alpha_hit_threshold(frame.window_state_for_theme()) * 255;
    if (alpha_threshold == 0)
        return result;
    u8 alpha = 0xff;

    auto window_rect = frame.window().rect();
    if (position.y() < window_rect.y()) {
        if (m_top_bottom) {
            auto scaled_relative_point = window_relative_position * m_top_bottom->scale();
            if (m_top_bottom->rect().contains(scaled_relative_point))
                alpha = m_top_bottom->get_pixel(scaled_relative_point).alpha();
        }
    } else if (position.y() > window_rect.bottom()) {
        if (m_top_bottom) {
            Gfx::IntPoint scaled_relative_point { window_relative_position.x() * m_top_bottom->scale(), m_bottom_y * m_top_bottom->scale() + position.y() - window_rect.bottom() - 1 };
            if (m_top_bottom->rect().contains(scaled_relative_point))
                alpha = m_top_bottom->get_pixel(scaled_relative_point).alpha();
        }
    } else if (position.x() < window_rect.x()) {
        if (m_left_right) {
            Gfx::IntPoint scaled_relative_point { window_relative_position.x() * m_left_right->scale(), (window_relative_position.y() - m_bottom_y) * m_left_right->scale() };
            if (m_left_right->rect().contains(scaled_relative_point))
                alpha = m_left_right->get_pixel(scaled_relative_point).alpha();
        }
    } else if (position.x() > window_rect.right()) {
        if (m_left_right) {
            Gfx::IntPoint scaled_relative_point { m_right_x * m_left_right->scale() + position.x() - window_rect.right() - 1, (window_relative_position.y() - m_bottom_y) * m_left_right->scale() };
            if (m_left_right->rect().contains(scaled_relative_point))
                alpha = m_left_right->get_pixel(scaled_relative_point).alpha();
        }
    } else {
        return {};
    }
    if (alpha >= alpha_threshold)
        return result;
    return {};
}

bool WindowFrame::handle_titlebar_icon_mouse_event(MouseEvent const& event)
{
    auto& wm = WindowManager::the();

    if (event.type() == Event::MouseDown && (event.button() == MouseButton::Left || event.button() == MouseButton::Right)) {
        // Manually start a potential double click. Since we're opening
        // a menu, we will only receive the MouseDown event, so we
        // need to record that fact. If the user subsequently clicks
        // on the same area, the menu will get closed, and we will
        // receive a MouseUp event, but because windows have changed
        // we don't get a MouseDoubleClick event. We can however record
        // this click, and when we receive the MouseUp event check if
        // it would have been considered a double click, if it weren't
        // for the fact that we opened and closed a window in the meanwhile
        wm.start_menu_doubleclick(m_window, event);

        m_window.popup_window_menu(titlebar_rect().bottom_left().translated(rect().location()), WindowMenuDefaultAction::Close);
        return true;
    } else if (event.type() == Event::MouseUp && event.button() == MouseButton::Left) {
        // Since the MouseDown event opened a menu, another MouseUp
        // from the second click outside the menu wouldn't be considered
        // a double click, so let's manually check if it would otherwise
        // have been be considered to be one
        if (wm.is_menu_doubleclick(m_window, event)) {
            // It is a double click, so perform activate the default item
            m_window.window_menu_activate_default();
        }
        return true;
    }
    return false;
}

void WindowFrame::handle_titlebar_mouse_event(MouseEvent const& event)
{
    auto& wm = WindowManager::the();

    if (titlebar_icon_rect().contains(event.position())) {
        if (handle_titlebar_icon_mouse_event(event))
            return;
    }

    for (auto& button : m_buttons) {
        if (button.relative_rect().contains(event.position()))
            return button.on_mouse_event(event.translated(-button.relative_rect().location()));
    }

    if (event.type() == Event::MouseDown) {
        if ((m_window.type() == WindowType::Normal || m_window.type() == WindowType::ToolWindow) && event.button() == MouseButton::Right) {
            auto default_action = m_window.is_maximized() ? WindowMenuDefaultAction::Restore : WindowMenuDefaultAction::Maximize;
            m_window.popup_window_menu(event.position().translated(rect().location()), default_action);
            return;
        }
        if (m_window.is_movable() && event.button() == MouseButton::Left)
            wm.start_window_move(m_window, event.translated(rect().location()));
    }
}

void WindowFrame::handle_mouse_event(MouseEvent const& event)
{
    VERIFY(!m_window.is_fullscreen());

    if (m_window.type() != WindowType::Normal && m_window.type() != WindowType::ToolWindow && m_window.type() != WindowType::Notification)
        return;

    auto& wm = WindowManager::the();
    if (m_window.type() == WindowType::Normal || m_window.type() == WindowType::ToolWindow) {
        if (event.type() == Event::MouseDown)
            wm.move_to_front_and_make_active(m_window);
    }

    if (m_window.blocking_modal_window())
        return;

    // This is slightly hackish, but expand the title bar rect by two pixels downwards,
    // so that mouse events between the title bar and window contents don't act like
    // mouse events on the border.
    auto adjusted_titlebar_rect = titlebar_rect();
    adjusted_titlebar_rect.set_height(adjusted_titlebar_rect.height() + 2);

    if (adjusted_titlebar_rect.contains(event.position())) {
        handle_titlebar_mouse_event(event);
        return;
    }

    if (menubar_rect().contains(event.position())) {
        handle_menubar_mouse_event(event);
        return;
    }

    handle_border_mouse_event(event);
}

void WindowFrame::handle_border_mouse_event(const MouseEvent& event)
{
    if (!m_window.is_resizable())
        return;

    auto& wm = WindowManager::the();

    if (event.type() == Event::MouseMove && event.buttons() == 0) {
        constexpr ResizeDirection direction_for_hot_area[3][3] = {
            { ResizeDirection::UpLeft, ResizeDirection::Up, ResizeDirection::UpRight },
            { ResizeDirection::Left, ResizeDirection::None, ResizeDirection::Right },
            { ResizeDirection::DownLeft, ResizeDirection::Down, ResizeDirection::DownRight },
        };
        Gfx::IntRect outer_rect = { {}, rect().size() };
        VERIFY(outer_rect.contains(event.position()));
        int window_relative_x = event.x() - outer_rect.x();
        int window_relative_y = event.y() - outer_rect.y();
        int hot_area_row = min(2, window_relative_y / (outer_rect.height() / 3));
        int hot_area_column = min(2, window_relative_x / (outer_rect.width() / 3));
        wm.set_resize_candidate(m_window, direction_for_hot_area[hot_area_row][hot_area_column]);
        Compositor::the().invalidate_cursor();
        return;
    }

    if (event.type() == Event::MouseDown && event.button() == MouseButton::Left)
        wm.start_window_resize(m_window, event.translated(rect().location()));
}

void WindowFrame::handle_menubar_mouse_event(const MouseEvent& event)
{
    Menu* hovered_menu = nullptr;
    auto menubar_rect = this->menubar_rect();
    auto adjusted_position = event.position().translated(-menubar_rect.location());
    m_window.menubar()->for_each_menu([&](Menu& menu) {
        if (menu.rect_in_window_menubar().contains(adjusted_position)) {
            hovered_menu = &menu;
            handle_menu_mouse_event(menu, event);
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    if (!hovered_menu && event.type() == Event::Type::MouseDown)
        MenuManager::the().close_everyone();
    if (hovered_menu != MenuManager::the().hovered_menu()) {
        MenuManager::the().set_hovered_menu(hovered_menu);
        invalidate(menubar_rect);
    }
}

void WindowFrame::open_menubar_menu(Menu& menu)
{
    auto menubar_rect = this->menubar_rect();
    MenuManager::the().close_everyone();
    menu.ensure_menu_window().move_to(menu.rect_in_window_menubar().bottom_left().translated(rect().location()).translated(menubar_rect.location()));
    MenuManager::the().open_menu(menu);
    WindowManager::the().set_window_with_active_menu(&m_window);
    invalidate(menubar_rect);
}

void WindowFrame::handle_menu_mouse_event(Menu& menu, const MouseEvent& event)
{
    auto menubar_rect = this->menubar_rect();
    bool is_hover_with_any_menu_open = event.type() == MouseEvent::MouseMove && &m_window == WindowManager::the().window_with_active_menu();
    bool is_mousedown_with_left_button = event.type() == MouseEvent::MouseDown && event.button() == MouseButton::Left;
    bool should_open_menu = &menu != MenuManager::the().current_menu() && (is_hover_with_any_menu_open || is_mousedown_with_left_button);
    bool should_close_menu = &menu == MenuManager::the().current_menu() && is_mousedown_with_left_button;

    if (should_open_menu) {
        open_menubar_menu(menu);
        return;
    }

    if (should_close_menu) {
        invalidate(menubar_rect);
        MenuManager::the().close_everyone();
    }
}

void WindowFrame::start_flash_animation()
{
    if (!m_flash_timer) {
        m_flash_timer = Core::Timer::construct(100, [this] {
            VERIFY(m_flash_counter);
            invalidate_titlebar();
            if (!--m_flash_counter)
                m_flash_timer->stop();
        });
    }
    m_flash_counter = 8;
    m_flash_timer->start();
}

void WindowFrame::paint_simple_rect_shadow(Gfx::Painter& painter, const Gfx::IntRect& containing_rect, const Gfx::Bitmap& shadow_bitmap, bool shadow_includes_frame, bool fill_content)
{
    // The layout of the shadow_bitmap is defined like this:
    // +---------+----+---------+----+----+----+
    // |   TL    | T  |   TR    | LT | L  | LB |
    // +---------+----+---------+----+----+----+
    // |   BL    | B  |   BR    | RT | R  | RB |
    // +---------+----+---------+----+----+----+
    // Located strictly on the top or bottom of the rectangle, above or below of the content:
    //   TL = top-left     T = top     TR = top-right
    //   BL = bottom-left  B = bottom  BR = bottom-right
    // Located on the left or right of the rectangle, but not above or below of the content:
    //   LT = left-top     L = left    LB = left-bottom
    //   RT = right-top    R = right   RB = right-bottom
    // So, the bitmap has two rows and 6 column, two of which are twice as wide.
    // The height divided by two defines a cell size, and width of each
    // column must be the same as the height of the cell, except for the
    // first an third column, which are twice as wide.
    // If fill_content is true, it will use the RGBA color of right-bottom pixel of TL to fill the rectangle enclosed
    if (shadow_bitmap.height() % 2 != 0) {
        dbgln("Can't paint simple rect shadow, shadow bitmap height {} is not even", shadow_bitmap.height());
        return;
    }
    auto base_size = shadow_bitmap.height() / 2;
    if (shadow_bitmap.width() != base_size * (6 + 2)) {
        if (shadow_bitmap.width() % base_size != 0)
            dbgln("Can't paint simple rect shadow, shadow bitmap width {} is not a multiple of {}", shadow_bitmap.width(), base_size);
        else
            dbgln("Can't paint simple rect shadow, shadow bitmap width {} but expected {}", shadow_bitmap.width(), base_size * (6 + 2));
        return;
    }

    // The containing_rect should have been inflated appropriately
    VERIFY(containing_rect.size().contains(Gfx::IntSize { base_size, base_size }));

    auto sides_height = containing_rect.height() - 2 * base_size;
    auto half_height = sides_height / 2;
    auto containing_horizontal_rect = containing_rect;

    int horizontal_shift = 0;
    if (half_height < base_size && !shadow_includes_frame) {
        // If the height is too small we need to shift the left/right accordingly, unless the shadow includes portions of the frame
        horizontal_shift = base_size - half_height;
        containing_horizontal_rect.set_left(containing_horizontal_rect.left() + horizontal_shift);
        containing_horizontal_rect.set_right(containing_horizontal_rect.right() - 2 * horizontal_shift);
    }
    auto half_width = containing_horizontal_rect.width() / 2;
    int corner_piece_width = min(containing_horizontal_rect.width() / 2, base_size * 2);
    int left_corners_right = containing_horizontal_rect.left() + corner_piece_width;
    int right_corners_left = max(containing_horizontal_rect.right() - corner_piece_width + 1, left_corners_right + 1);
    auto paint_horizontal = [&](int y, int src_row) {
        if (half_width <= 0)
            return;
        Gfx::PainterStateSaver save(painter);
        painter.add_clip_rect({ containing_horizontal_rect.left(), y, containing_horizontal_rect.width(), base_size });
        painter.blit({ containing_horizontal_rect.left(), y }, shadow_bitmap, { 0, src_row * base_size, corner_piece_width, base_size });
        painter.blit({ right_corners_left, y }, shadow_bitmap, { 5 * base_size - corner_piece_width, src_row * base_size, corner_piece_width, base_size });
        for (int x = left_corners_right; x < right_corners_left; x += base_size) {
            auto width = min(right_corners_left - x, base_size);
            painter.blit({ x, y }, shadow_bitmap, { corner_piece_width, src_row * base_size, width, base_size });
        }
    };

    paint_horizontal(containing_rect.top(), 0);
    paint_horizontal(containing_rect.bottom() - base_size + 1, 1);

    int corner_piece_height = min(half_height, base_size);
    int top_corners_bottom = base_size + corner_piece_height;
    int bottom_corners_top = base_size + max(half_height, sides_height - corner_piece_height);
    auto paint_vertical = [&](int x, int src_row, int hshift, int hsrcshift) {
        Gfx::PainterStateSaver save(painter);
        painter.add_clip_rect({ x, containing_rect.y() + base_size, base_size, containing_rect.height() - 2 * base_size });
        painter.blit({ x + hshift, containing_rect.top() + top_corners_bottom - corner_piece_height }, shadow_bitmap, { base_size * 5 + hsrcshift, src_row * base_size, base_size - hsrcshift, corner_piece_height });
        painter.blit({ x + hshift, containing_rect.top() + bottom_corners_top }, shadow_bitmap, { base_size * 7 + hsrcshift, src_row * base_size + base_size - corner_piece_height, base_size - hsrcshift, corner_piece_height });
        for (int y = top_corners_bottom; y < bottom_corners_top; y += base_size) {
            auto height = min(bottom_corners_top - y, base_size);
            painter.blit({ x, containing_rect.top() + y }, shadow_bitmap, { base_size * 6, src_row * base_size, base_size, height });
        }
    };

    paint_vertical(containing_rect.left(), 0, horizontal_shift, 0);
    if (shadow_includes_frame)
        horizontal_shift = 0; // TODO: fix off-by-one on rectangles barely wide enough
    paint_vertical(containing_rect.right() - base_size + 1, 1, 0, horizontal_shift);

    if (fill_content) {
        // Fill the enclosed rectangle with the RGBA color of the right-bottom pixel of the TL tile
        auto inner_rect = containing_rect.shrunken(2 * base_size, 2 * base_size);
        if (!inner_rect.is_empty())
            painter.fill_rect(inner_rect, shadow_bitmap.get_pixel(2 * base_size - 1, base_size - 1));
    }
}

int WindowFrame::menu_row_count() const
{
    if (!m_window.should_show_menubar())
        return 0;
    return m_window.menubar() ? 1 : 0;
}

}
