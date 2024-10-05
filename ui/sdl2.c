/*
 * QEMU SDL display driver
 *
 * Copyright (c) 2003 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
/* Ported SDL 1.2 code to 2.0 by Dave Airlie. */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/sdl2.h"
#include "sysemu/runstate.h"
#include "sysemu/runstate-action.h"
#include "sysemu/sysemu.h"
#include "ui/win32-kbd-hook.h"
#include "qemu/log.h"

static int sdl2_num_outputs;
static struct sdl2_console *sdl2_console;

static SDL_Surface *guest_sprite_surface;
static int gui_grab; /* if true, all keyboard/mouse events are grabbed */
static bool alt_grab;
static bool ctrl_grab;

static int gui_saved_grab;
static int gui_fullscreen;
static int gui_grab_code = KMOD_LALT | KMOD_LCTRL;
static SDL_Cursor *sdl_cursor_normal;
static SDL_Cursor *sdl_cursor_hidden;
static int absolute_enabled;
static int guest_cursor;
static int guest_x, guest_y;
static SDL_Cursor *guest_sprite;
static Notifier mouse_mode_notifier;

#define SDL2_REFRESH_INTERVAL_BUSY 10
#define SDL2_MAX_IDLE_COUNT (2 * GUI_REFRESH_INTERVAL_DEFAULT \
                             / SDL2_REFRESH_INTERVAL_BUSY + 1)

/* introduced in SDL 2.0.10 */
#ifndef SDL_HINT_RENDER_BATCHING
#define SDL_HINT_RENDER_BATCHING "SDL_RENDER_BATCHING"
#endif

static void sdl_update_caption(struct sdl2_console *scon);

static struct sdl2_console *get_scon_from_window(uint32_t window_id)
{
    int i;
    for (i = 0; i < sdl2_num_outputs; i++) {
        if (sdl2_console[i].real_window == SDL_GetWindowFromID(window_id)) {
            return &sdl2_console[i];
        }
    }
    return NULL;
}

void sdl2_window_create(struct sdl2_console *scon)
{
    int flags = 0;

    if (!scon->surface) {
        return;
    }
    assert(!scon->real_window);

    if (gui_fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN;
    } else {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (scon->hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }
#ifdef CONFIG_OPENGL
    if (scon->opengl) {
        flags |= SDL_WINDOW_OPENGL;
    }
#endif

    scon->real_window = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED,
                                         SDL_WINDOWPOS_UNDEFINED,
                                         surface_width(scon->surface),
                                         surface_height(scon->surface),
                                         flags);
    if (scon->opengl) {
        const char *driver = "opengl";

        if (scon->opts->gl == DISPLAYGL_MODE_ES) {
            driver = "opengles2";
        }

        SDL_SetHint(SDL_HINT_RENDER_DRIVER, driver);
        SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");

        scon->winctx = SDL_GL_CreateContext(scon->real_window);
    } else {
        /* The SDL renderer is only used by sdl2-2D, when OpenGL is disabled */
        scon->real_renderer = SDL_CreateRenderer(scon->real_window, -1, 0);
    }
    sdl_update_caption(scon);
}

void sdl2_window_destroy(struct sdl2_console *scon)
{
    if (!scon->real_window) {
        return;
    }

    if (scon->winctx) {
        SDL_GL_DeleteContext(scon->winctx);
        scon->winctx = NULL;
    }
    if (scon->real_renderer) {
        SDL_DestroyRenderer(scon->real_renderer);
        scon->real_renderer = NULL;
    }
    SDL_DestroyWindow(scon->real_window);
    scon->real_window = NULL;
}

void sdl2_window_resize(struct sdl2_console *scon)
{
    if (!scon->real_window) {
        return;
    }

    SDL_SetWindowSize(scon->real_window,
                      surface_width(scon->surface),
                      surface_height(scon->surface));
}

static void sdl2_redraw(struct sdl2_console *scon)
{
    if (scon->opengl) {
#ifdef CONFIG_OPENGL
        sdl2_gl_redraw(scon);
#endif
    } else {
        sdl2_2d_redraw(scon);
    }
}

static void sdl_update_caption(struct sdl2_console *scon)
{
    char win_title[1024];
    char icon_title[1024];
    const char *status = "";

    if (!runstate_is_running()) {
        status = " [Stopped]";
    } else if (gui_grab) {
        if (alt_grab) {
#ifdef CONFIG_DARWIN
            status = " - Press ⌃⌥⇧G to exit grab";
#else
            status = " - Press Ctrl-Alt-Shift-G to exit grab";
#endif
        } else if (ctrl_grab) {
            status = " - Press Right-Ctrl-G to exit grab";
        } else {
#ifdef CONFIG_DARWIN
            status = " - Press ⌃⌥G to exit grab";
#else
            status = " - Press Ctrl-Alt-G to exit grab";
#endif
        }
    }

    if (qemu_name) {
        snprintf(win_title, sizeof(win_title), "QEMU (%s-%d)%s", qemu_name,
                 scon->idx, status);
        snprintf(icon_title, sizeof(icon_title), "QEMU (%s)", qemu_name);
    } else {
        snprintf(win_title, sizeof(win_title), "QEMU%s", status);
        snprintf(icon_title, sizeof(icon_title), "QEMU");
    }

    if (scon->real_window) {
        SDL_SetWindowTitle(scon->real_window, win_title);
    }
}

static void sdl_hide_cursor(struct sdl2_console *scon)
{
    if (scon->opts->has_show_cursor && scon->opts->show_cursor) {
        return;
    }

    SDL_ShowCursor(SDL_DISABLE);
    SDL_SetCursor(sdl_cursor_hidden);

    if (!qemu_input_is_absolute(scon->dcl.con)) {
        SDL_SetRelativeMouseMode(SDL_TRUE);
    }
}

static void sdl_show_cursor(struct sdl2_console *scon)
{
    if (scon->opts->has_show_cursor && scon->opts->show_cursor) {
        return;
    }

    if (!qemu_input_is_absolute(scon->dcl.con)) {
        SDL_SetRelativeMouseMode(SDL_FALSE);
    }

    if (guest_cursor &&
        (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled)) {
        SDL_SetCursor(guest_sprite);
    } else {
        SDL_SetCursor(sdl_cursor_normal);
    }

    SDL_ShowCursor(SDL_ENABLE);
}

static void sdl_grab_start(struct sdl2_console *scon)
{
    QemuConsole *con = scon ? scon->dcl.con : NULL;

    if (!con || !qemu_console_is_graphic(con)) {
        return;
    }
    /*
     * If the application is not active, do not try to enter grab state. This
     * prevents 'SDL_WM_GrabInput(SDL_GRAB_ON)' from blocking all the
     * application (SDL bug).
     */
    if (!(SDL_GetWindowFlags(scon->real_window) & SDL_WINDOW_INPUT_FOCUS)) {
        return;
    }
    if (guest_cursor) {
        SDL_SetCursor(guest_sprite);
        if (!qemu_input_is_absolute(scon->dcl.con) && !absolute_enabled) {
            SDL_WarpMouseInWindow(scon->real_window, guest_x, guest_y);
        }
    } else {
        sdl_hide_cursor(scon);
    }
    SDL_SetWindowGrab(scon->real_window, SDL_TRUE);
    gui_grab = 1;
    win32_kbd_set_grab(true);
    sdl_update_caption(scon);
}

static void sdl_grab_end(struct sdl2_console *scon)
{
    SDL_SetWindowGrab(scon->real_window, SDL_FALSE);
    gui_grab = 0;
    win32_kbd_set_grab(false);
    sdl_show_cursor(scon);
    sdl_update_caption(scon);
}

static void absolute_mouse_grab(struct sdl2_console *scon)
{
    int mouse_x, mouse_y;
    int scr_w, scr_h;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    SDL_GetWindowSize(scon->real_window, &scr_w, &scr_h);
    if (mouse_x > 0 && mouse_x < scr_w - 1 &&
        mouse_y > 0 && mouse_y < scr_h - 1) {
        sdl_grab_start(scon);
    }
}

static void sdl_mouse_mode_change(Notifier *notify, void *data)
{
    if (qemu_input_is_absolute(sdl2_console[0].dcl.con)) {
        if (!absolute_enabled) {
            absolute_enabled = 1;
            SDL_SetRelativeMouseMode(SDL_FALSE);
            absolute_mouse_grab(&sdl2_console[0]);
        }
    } else if (absolute_enabled) {
        if (!gui_fullscreen) {
            sdl_grab_end(&sdl2_console[0]);
        }
        absolute_enabled = 0;
    }
}

static void sdl_send_mouse_event(struct sdl2_console *scon, int dx, int dy,
                                 int x, int y, int state)
{
    static uint32_t bmap[INPUT_BUTTON__MAX] = {
        [INPUT_BUTTON_LEFT]       = SDL_BUTTON(SDL_BUTTON_LEFT),
        [INPUT_BUTTON_MIDDLE]     = SDL_BUTTON(SDL_BUTTON_MIDDLE),
        [INPUT_BUTTON_RIGHT]      = SDL_BUTTON(SDL_BUTTON_RIGHT),
        [INPUT_BUTTON_SIDE]       = SDL_BUTTON(SDL_BUTTON_X1),
        [INPUT_BUTTON_EXTRA]      = SDL_BUTTON(SDL_BUTTON_X2)
    };
    static uint32_t prev_state;

    if (prev_state != state) {
        qemu_input_update_buttons(scon->dcl.con, bmap, prev_state, state);
        prev_state = state;
    }

    if (qemu_input_is_absolute(scon->dcl.con)) {
        qemu_input_queue_abs(scon->dcl.con, INPUT_AXIS_X,
                             x, 0, surface_width(scon->surface));
        qemu_input_queue_abs(scon->dcl.con, INPUT_AXIS_Y,
                             y, 0, surface_height(scon->surface));
    } else {
        if (guest_cursor) {
            x -= guest_x;
            y -= guest_y;
            guest_x += x;
            guest_y += y;
            dx = x;
            dy = y;
        }
        qemu_input_queue_rel(scon->dcl.con, INPUT_AXIS_X, dx);
        qemu_input_queue_rel(scon->dcl.con, INPUT_AXIS_Y, dy);
    }
    qemu_input_event_sync();
}

static void toggle_full_screen(struct sdl2_console *scon)
{
    gui_fullscreen = !gui_fullscreen;
    if (gui_fullscreen) {
        SDL_SetWindowFullscreen(scon->real_window,
                                SDL_WINDOW_FULLSCREEN);
        gui_saved_grab = gui_grab;
        sdl_grab_start(scon);
    } else {
        if (!gui_saved_grab) {
            sdl_grab_end(scon);
        }
        SDL_SetWindowFullscreen(scon->real_window, 0);
    }
    sdl2_redraw(scon);
}

static int get_mod_state(void)
{
    SDL_Keymod mod = SDL_GetModState();

    if (alt_grab) {
        return (mod & (gui_grab_code | KMOD_LSHIFT)) ==
            (gui_grab_code | KMOD_LSHIFT);
    } else if (ctrl_grab) {
        return (mod & KMOD_RCTRL) == KMOD_RCTRL;
    } else {
        return (mod & gui_grab_code) == gui_grab_code;
    }
}

static void *sdl2_win32_get_hwnd(struct sdl2_console *scon)
{
#ifdef CONFIG_WIN32
    SDL_SysWMinfo info;

    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(scon->real_window, &info)) {
        return info.info.win.window;
    }
#endif
    return NULL;
}

static void handle_keydown(SDL_Event *ev)
{
    int win;
    struct sdl2_console *scon = get_scon_from_window(ev->key.windowID);
    int gui_key_modifier_pressed = get_mod_state();
    int gui_keysym = 0;

    if (!scon) {
        return;
    }

    if (!scon->ignore_hotkeys && gui_key_modifier_pressed && !ev->key.repeat) {
        switch (ev->key.keysym.scancode) {
        case SDL_SCANCODE_2:
        case SDL_SCANCODE_3:
        case SDL_SCANCODE_4:
        case SDL_SCANCODE_5:
        case SDL_SCANCODE_6:
        case SDL_SCANCODE_7:
        case SDL_SCANCODE_8:
        case SDL_SCANCODE_9:
            if (gui_grab) {
                sdl_grab_end(scon);
            }

            win = ev->key.keysym.scancode - SDL_SCANCODE_1;
            if (win < sdl2_num_outputs) {
                sdl2_console[win].hidden = !sdl2_console[win].hidden;
                if (sdl2_console[win].real_window) {
                    if (sdl2_console[win].hidden) {
                        SDL_HideWindow(sdl2_console[win].real_window);
                    } else {
                        SDL_ShowWindow(sdl2_console[win].real_window);
                    }
                }
                gui_keysym = 1;
            }
            break;
        case SDL_SCANCODE_F:
            toggle_full_screen(scon);
            gui_keysym = 1;
            break;
        case SDL_SCANCODE_G:
            gui_keysym = 1;
            if (!gui_grab) {
                sdl_grab_start(scon);
            } else if (!gui_fullscreen) {
                sdl_grab_end(scon);
            }
            break;
        case SDL_SCANCODE_U:
            sdl2_window_resize(scon);
            if (!scon->opengl) {
                /* re-create scon->texture */
                sdl2_2d_switch(&scon->dcl, scon->surface);
            }
            gui_keysym = 1;
            break;
#if 0
        case SDL_SCANCODE_KP_PLUS:
        case SDL_SCANCODE_KP_MINUS:
            if (!gui_fullscreen) {
                int scr_w, scr_h;
                int width, height;
                SDL_GetWindowSize(scon->real_window, &scr_w, &scr_h);

                width = MAX(scr_w + (ev->key.keysym.scancode ==
                                     SDL_SCANCODE_KP_PLUS ? 50 : -50),
                            160);
                height = (surface_height(scon->surface) * width) /
                    surface_width(scon->surface);
                fprintf(stderr, "%s: scale to %dx%d\n",
                        __func__, width, height);
                sdl_scale(scon, width, height);
                sdl2_redraw(scon);
                gui_keysym = 1;
            }
#endif
        default:
            break;
        }
    }
    if (!gui_keysym) {
        sdl2_process_key(scon, &ev->key);
    }
}

static void handle_keyup(SDL_Event *ev)
{
    struct sdl2_console *scon = get_scon_from_window(ev->key.windowID);

    if (!scon) {
        return;
    }

    scon->ignore_hotkeys = false;
    sdl2_process_key(scon, &ev->key);
}

static void handle_textinput(SDL_Event *ev)
{
    struct sdl2_console *scon = get_scon_from_window(ev->text.windowID);
    QemuConsole *con = scon ? scon->dcl.con : NULL;

    if (!con) {
        return;
    }

    if (QEMU_IS_TEXT_CONSOLE(con)) {
        qemu_text_console_put_string(QEMU_TEXT_CONSOLE(con), ev->text.text, strlen(ev->text.text));
    }
}

static void handle_mousemotion(SDL_Event *ev)
{
    int max_x, max_y;
    struct sdl2_console *scon = get_scon_from_window(ev->motion.windowID);

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    if (qemu_input_is_absolute(scon->dcl.con) || absolute_enabled) {
        int scr_w, scr_h;
        SDL_GetWindowSize(scon->real_window, &scr_w, &scr_h);
        max_x = scr_w - 1;
        max_y = scr_h - 1;
        if (gui_grab && !gui_fullscreen
            && (ev->motion.x == 0 || ev->motion.y == 0 ||
                ev->motion.x == max_x || ev->motion.y == max_y)) {
            sdl_grab_end(scon);
        }
        if (!gui_grab &&
            (ev->motion.x > 0 && ev->motion.x < max_x &&
             ev->motion.y > 0 && ev->motion.y < max_y)) {
            sdl_grab_start(scon);
        }
    }
    if (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled) {
        sdl_send_mouse_event(scon, ev->motion.xrel, ev->motion.yrel,
                             ev->motion.x, ev->motion.y, ev->motion.state);
    }
}

static void handle_mousebutton(SDL_Event *ev)
{
    int buttonstate = SDL_GetMouseState(NULL, NULL);
    SDL_MouseButtonEvent *bev;
    struct sdl2_console *scon = get_scon_from_window(ev->button.windowID);

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    bev = &ev->button;
    if (!gui_grab && !qemu_input_is_absolute(scon->dcl.con)) {
        if (ev->type == SDL_MOUSEBUTTONUP && bev->button == SDL_BUTTON_LEFT) {
            /* start grabbing all events */
            sdl_grab_start(scon);
        }
    } else {
        if (ev->type == SDL_MOUSEBUTTONDOWN) {
            buttonstate |= SDL_BUTTON(bev->button);
        } else {
            buttonstate &= ~SDL_BUTTON(bev->button);
        }
        sdl_send_mouse_event(scon, 0, 0, bev->x, bev->y, buttonstate);
    }
}

static void handle_mousewheel(SDL_Event *ev)
{
    struct sdl2_console *scon = get_scon_from_window(ev->wheel.windowID);
    SDL_MouseWheelEvent *wev = &ev->wheel;
    InputButton btn;

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    if (wev->y > 0) {
        btn = INPUT_BUTTON_WHEEL_UP;
    } else if (wev->y < 0) {
        btn = INPUT_BUTTON_WHEEL_DOWN;
    } else if (wev->x < 0) {
        btn = INPUT_BUTTON_WHEEL_RIGHT;
    } else if (wev->x > 0) {
        btn = INPUT_BUTTON_WHEEL_LEFT;
    } else {
        return;
    }

    qemu_input_queue_btn(scon->dcl.con, btn, true);
    qemu_input_event_sync();
    qemu_input_queue_btn(scon->dcl.con, btn, false);
    qemu_input_event_sync();
}

static int fxui_grab_val(const int grab)
{
    static int fxui_grab;
    fxui_grab = (grab & 0x80U)? (grab & 0x01U):fxui_grab;
    return fxui_grab;
}
static int fxui_focus_lost(void)
{
    int ret = fxui_grab_val(0);
    fxui_grab_val(0x80);
    return ret;
}
static void fxui_focus_gained(struct sdl2_console *scon)
{
    if (fxui_grab_val(0)) {
        if (gui_grab) {
            sdl_grab_end(scon);
            fxui_grab_val(0x80);
        }
        sdl_grab_start(scon);
    }
}

static void handle_windowevent(SDL_Event *ev)
{
    struct sdl2_console *scon = get_scon_from_window(ev->window.windowID);
    bool allow_close = true;

    if (!scon) {
        return;
    }

    switch (ev->window.event) {
    case SDL_WINDOWEVENT_RESIZED:
        {
            QemuUIInfo info;
            memset(&info, 0, sizeof(info));
            info.width = ev->window.data1;
            info.height = ev->window.data2;
            dpy_set_ui_info(scon->dcl.con, &info, true);
        }
        sdl2_redraw(scon);
        break;
    case SDL_WINDOWEVENT_EXPOSED:
        sdl2_redraw(scon);
        break;
    case SDL_WINDOWEVENT_FOCUS_GAINED:
        fxui_focus_gained(scon);
        win32_kbd_set_grab(gui_grab);
        if (qemu_console_is_graphic(scon->dcl.con)) {
            win32_kbd_set_window(sdl2_win32_get_hwnd(scon));
        }
        /* fall through */
    case SDL_WINDOWEVENT_ENTER:
        if (!gui_grab && (qemu_input_is_absolute(scon->dcl.con) || absolute_enabled)) {
            absolute_mouse_grab(scon);
        }
        /* If a new console window opened using a hotkey receives the
         * focus, SDL sends another KEYDOWN event to the new window,
         * closing the console window immediately after.
         *
         * Work around this by ignoring further hotkey events until a
         * key is released.
         */
        scon->ignore_hotkeys = get_mod_state();
        break;
    case SDL_WINDOWEVENT_FOCUS_LOST:
        if (qemu_console_is_graphic(scon->dcl.con)) {
            win32_kbd_set_window(NULL);
        }
        if (!fxui_focus_lost() && gui_grab && !gui_fullscreen) {
            sdl_grab_end(scon);
        }
        break;
    case SDL_WINDOWEVENT_RESTORED:
        update_displaychangelistener(&scon->dcl, GUI_REFRESH_INTERVAL_DEFAULT);
        break;
    case SDL_WINDOWEVENT_MINIMIZED:
        update_displaychangelistener(&scon->dcl, 500);
        break;
    case SDL_WINDOWEVENT_CLOSE:
        if (qemu_console_is_graphic(scon->dcl.con)) {
            if (scon->opts->has_window_close && !scon->opts->window_close) {
                allow_close = false;
            }
            if (allow_close) {
                shutdown_action = SHUTDOWN_ACTION_POWEROFF;
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
            }
        } else {
            SDL_HideWindow(scon->real_window);
            scon->hidden = true;
        }
        break;
    case SDL_WINDOWEVENT_SHOWN:
        scon->hidden = false;
        break;
    case SDL_WINDOWEVENT_HIDDEN:
        scon->hidden = true;
        break;
    }
}

void sdl2_poll_events(struct sdl2_console *scon)
{
    SDL_Event ev1, *ev = &ev1;
    bool allow_close = true;
    int idle = 1;

    if (scon->last_vm_running != runstate_is_running()) {
        scon->last_vm_running = runstate_is_running();
        sdl_update_caption(scon);
    }

    while (SDL_PollEvent(ev)) {
        switch (ev->type) {
        case SDL_KEYDOWN:
            idle = 0;
            handle_keydown(ev);
            break;
        case SDL_KEYUP:
            idle = 0;
            handle_keyup(ev);
            break;
        case SDL_TEXTINPUT:
            idle = 0;
            handle_textinput(ev);
            break;
        case SDL_QUIT:
            if (scon->opts->has_window_close && !scon->opts->window_close) {
                allow_close = false;
            }
            if (allow_close) {
                shutdown_action = SHUTDOWN_ACTION_POWEROFF;
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
            }
            break;
        case SDL_MOUSEMOTION:
            idle = 0;
            handle_mousemotion(ev);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            idle = 0;
            handle_mousebutton(ev);
            break;
        case SDL_MOUSEWHEEL:
            idle = 0;
            handle_mousewheel(ev);
            break;
        case SDL_WINDOWEVENT:
            handle_windowevent(ev);
            break;
        default:
            break;
        }
    }

    if (idle) {
        if (scon->idle_counter < SDL2_MAX_IDLE_COUNT) {
            scon->idle_counter++;
            if (scon->idle_counter >= SDL2_MAX_IDLE_COUNT) {
                scon->dcl.update_interval = GUI_REFRESH_INTERVAL_DEFAULT;
            }
        }
    } else {
        scon->idle_counter = 0;
        scon->dcl.update_interval = SDL2_REFRESH_INTERVAL_BUSY;
    }
}

static void sdl_mouse_warp(DisplayChangeListener *dcl,
                           int x, int y, int on)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    if (!qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    if (on) {
        if (!guest_cursor) {
            sdl_show_cursor(scon);
        }
        if (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled) {
            SDL_SetCursor(guest_sprite);
            if (!qemu_input_is_absolute(scon->dcl.con) && !absolute_enabled) {
                SDL_WarpMouseInWindow(scon->real_window, x, y);
            }
        }
    } else if (gui_grab) {
        sdl_hide_cursor(scon);
    }
    guest_cursor = on;
    guest_x = x, guest_y = y;
}

static void sdl_mouse_define(DisplayChangeListener *dcl,
                             QEMUCursor *c)
{

    if (guest_sprite) {
        SDL_FreeCursor(guest_sprite);
    }

    if (guest_sprite_surface) {
        SDL_FreeSurface(guest_sprite_surface);
    }

    guest_sprite_surface =
        SDL_CreateRGBSurfaceFrom(c->data, c->width, c->height, 32, c->width * 4,
                                 0xff0000, 0x00ff00, 0xff, 0xff000000);

    if (!guest_sprite_surface) {
        fprintf(stderr, "Failed to make rgb surface from %p\n", c);
        return;
    }
    guest_sprite = SDL_CreateColorCursor(guest_sprite_surface,
                                         c->hot_x, c->hot_y);
    if (!guest_sprite) {
        fprintf(stderr, "Failed to make color cursor from %p\n", c);
        return;
    }
    if (guest_cursor &&
        (gui_grab || qemu_input_is_absolute(dcl->con) || absolute_enabled)) {
        SDL_SetCursor(guest_sprite);
    }
}

static void sdl_cleanup(void)
{
    if (guest_sprite) {
        SDL_FreeCursor(guest_sprite);
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

static void sdl_display_valid(const char *feat)
{
    if (!sdl2_console) {
        error_report("%s: invalid sdl display. Use -display sdl", feat);
        exit(1);
    }
}

static struct sdl_console_cb {
    QEMUTimer *ts;
    SDL_Surface *icon;
    struct sdl2_console *scon;
    int glide_on_mesa;
    int gui_saved_res;
    int render_pause;
    int res, msaa, alpha, dtimer, GLon12;
    void *opaque;
    void *hnwnd;
    void (*cwnd_fn)(void *, void *, void *);
} scon_cb;
static void sdl_gui_restart(struct sdl2_console *scon, SDL_Surface *icon)
{
    if (!gui_fullscreen)
        SDL_GetWindowPosition(scon->real_window, &scon->x, &scon->y);
    fxui_grab_val(0x80 | gui_grab);
    sdl_grab_end(scon);
    sdl2_window_destroy(scon);
    sdl2_window_create(scon);
    if (icon)
        SDL_SetWindowIcon(scon->real_window, icon);
    if (!gui_fullscreen)
        SDL_SetWindowPosition(scon->real_window, scon->x, scon->y);
}
static void sched_wndproc(void *opaque)
{
    struct sdl_console_cb *s = opaque;

    if (s->res == -1) {
        if (s->render_pause) {
            SDL_DestroyTexture(s->scon->texture);
            s->scon->texture = 0;
        }
        else {
            if (!s->scon->real_renderer)
                s->scon->real_renderer = SDL_CreateRenderer(s->scon->real_window, -1 ,0);
            if (!s->scon->opengl) {
                sdl2_2d_switch(&s->scon->dcl, s->scon->surface);
                if (!gui_fullscreen)
                    SDL_SetWindowPosition(s->scon->real_window, s->scon->x, s->scon->y);
            }
        }
    }
    else if (s->gui_saved_res) {
        SDL_GL_SetAttribute(SDL_GL_BUFFER_SIZE, 32);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,  24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
#ifdef CONFIG_DARWIN
        if (!s->dtimer)
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
        if (s->alpha)
            SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
        if (s->msaa) {
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, SDL_TRUE);
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, s->msaa);
        }
        const char hint[] = "opengl";
        if (!SDL_GetHint(SDL_HINT_RENDER_DRIVER) ||
            memcmp(SDL_GetHint(SDL_HINT_RENDER_DRIVER), hint, sizeof(hint) - 1)) {
            SDL_SetHint(SDL_HINT_RENDER_DRIVER, hint);
            sdl_gui_restart(s->scon, s->icon);
        }
        SDL_SysWMinfo wmi;
        SDL_VERSION(&wmi.version);
        if (SDL_GetWindowWMInfo(s->scon->real_window, &wmi)) {
            switch(wmi.subsystem) {
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
                case SDL_SYSWM_WINDOWS:
                    s->hnwnd = (void *)wmi.info.win.window;
                    break;
#endif
#if defined(SDL_VIDEO_DRIVER_X11)
                case SDL_SYSWM_X11:
                    s->hnwnd = (void *)wmi.info.x11.window;
                    break;
#endif
#if defined(SDL_VIDEO_DRIVER_COCOA)
                case SDL_SYSWM_COCOA:
                    s->hnwnd = (void *)wmi.info.cocoa.window;
                    break;
#endif
                default:
                    s->hnwnd = 0;
                    break;
            }
        }
        SDL_DestroyRenderer(s->scon->real_renderer);
        s->scon->real_renderer = 0;
        s->scon->winctx = SDL_GL_GetCurrentContext();
        s->scon->winctx = (s->scon->winctx)? s->scon->winctx:SDL_GL_CreateContext(s->scon->real_window);
        if (!s->scon->winctx) {
            error_report("%s", SDL_GetError());
            exit(1);
        }
        s->render_pause = 1;
        if (!s->opaque)
            s->cwnd_fn(s->scon->real_window, s->hnwnd, opaque);
    }
    else {
        SDL_GL_MakeCurrent(s->scon->real_window, NULL);
        SDL_GL_DeleteContext(s->scon->winctx);
        s->scon->winctx = 0;
        s->render_pause = 0;
        SDL_GL_ResetAttributes();
        if (!s->GLon12) {
            if (s->scon->texture) {
                SDL_DestroyTexture(s->scon->texture);
                s->scon->texture = 0;
            }
            SDL_SetHint(SDL_HINT_RENDER_DRIVER, "");
            sdl_gui_restart(s->scon, s->icon);
        }
        else {
            if (!s->scon->real_renderer)
                s->scon->real_renderer = SDL_CreateRenderer(s->scon->real_window, -1 ,0);
        }
        if (!s->scon->opengl)
            sdl2_2d_switch(&s->scon->dcl, s->scon->surface);
        timer_del(s->ts);
        timer_free(s->ts);
        s->ts = 0;
    }
    if (s->res > 0)
        SDL_SetWindowSize(s->scon->real_window, (s->res & 0xFFFFU), (s->res >> 0x10));
    if (s->opaque || !s->render_pause)
        graphic_hw_passthrough(s->scon->dcl.con, s->render_pause);
}

static int sdl_gui_fullscreen(int *sizev, const char *feat)
{
    struct sdl_console_cb *s = &scon_cb;

    sdl_display_valid(feat);
    s->scon = &sdl2_console[0];
    if (sizev) {
        sizev[0] = surface_width(s->scon->surface);
        sizev[1] = surface_height(s->scon->surface);
        if (!memcmp(feat, "mesapt", sizeof("mesapt")))
            SDL_GL_GetDrawableSize(s->scon->real_window, &sizev[2], &sizev[3]);
    }
    return gui_fullscreen;
}

static void sdl_renderer_stat(const int activate, const char *feat)
{
    struct sdl_console_cb *s = &scon_cb;

    if (activate == s->render_pause)
        return;

    sdl_display_valid(feat);
    s->scon = &sdl2_console[0];
    s->res = -1;
    s->render_pause = activate;

    if (!s->ts)
        s->ts = timer_new_ms(QEMU_CLOCK_REALTIME, &sched_wndproc, s);
    timer_mod(s->ts, qemu_clock_get_ms(QEMU_CLOCK_REALTIME));
}

void glide_prepare_window(uint32_t res, int msaa, void *opaque, void *cwnd_fn)
{
    int scr_w, scr_h;
    struct sdl_console_cb *s = &scon_cb;

    sdl_display_valid("glidept");
    s->scon = &sdl2_console[0];
    s->opaque = opaque;
    s->cwnd_fn = (void (*)(void *, void *, void *))cwnd_fn;
    if (s->render_pause) {
        s->glide_on_mesa = 1;
        s->gui_saved_res = 0;
    }
    else {
        SDL_GetWindowSize(s->scon->real_window, &scr_w, &scr_h);
        s->gui_saved_res = ((scr_h & 0x7FFFU) << 0x10) | scr_w;
        s->res = res;
        s->msaa = msaa;
        s->alpha = 1;
#ifdef CONFIG_DARWIN
        s->dtimer = s->alpha;
#endif
        if (!s->ts)
            s->ts = timer_new_ms(QEMU_CLOCK_REALTIME, &sched_wndproc, s);
        timer_mod(s->ts, qemu_clock_get_ms(QEMU_CLOCK_REALTIME));
    }
}

void glide_release_window(void *opaque, void *cwnd_fn)
{
    struct sdl_console_cb *s = &scon_cb;

    sdl_display_valid("glidept");
    s->scon = &sdl2_console[0];
    s->opaque = opaque;
    s->cwnd_fn = (void (*)(void *, void *, void *))cwnd_fn;
    if (s->gui_saved_res) {
        s->res = s->gui_saved_res;
        s->gui_saved_res = 0;
        if (s->ts)
            timer_mod(s->ts, qemu_clock_get_ms(QEMU_CLOCK_REALTIME));
    }
}

int glide_window_stat(const int activate)
{
    int stat;
    struct sdl_console_cb *s = &scon_cb;

    if (activate) {
        if (s->scon->winctx) {
            int scr_w, scr_h;
            SDL_GetWindowSize(s->scon->real_window, &scr_w, &scr_h);
#ifdef CONFIG_DARWIN
            if (SDL_GL_MakeCurrent(s->scon->real_window, s->scon->winctx))
                fprintf(stderr, "%s\n", SDL_GetError());
#endif
            stat = ((scr_h & 0x7FFFU) << 0x10) | scr_w;
            s->cwnd_fn(s->scon->real_window, s->hnwnd, s->opaque);
        }
        else
            stat = 1;
    }
    else {
        s->cwnd_fn(s->scon->real_window, s->hnwnd, s->opaque);
        stat = s->glide_on_mesa;
        s->glide_on_mesa = 0;
        stat ^= (s->scon->winctx)? 1:0;
    }
    return stat;
}

int glide_gui_fullscreen(int *width, int *height)
{
    int ret, v[2];
    ret = sdl_gui_fullscreen(v, "glidept");
    if (width)
        *width = v[0];
    if (height)
        *height = v[1];
    return ret;
}

void glide_renderer_stat(const int activate)
{
    sdl_renderer_stat(activate, "glidept");
}

void mesa_renderer_stat(const int activate)
{
    struct sdl_console_cb *s = &scon_cb;
    sdl_renderer_stat(activate, "mesapt");
    if (s->glide_on_mesa && !activate)
        glide_renderer_stat(1);
}

void mesa_prepare_window(int msaa, int alpha, int scale_x, void *cwnd_fn)
{
    int scr_w, scr_h;
    struct sdl_console_cb *s = &scon_cb;

    sdl_display_valid("mesapt");
    s->scon = &sdl2_console[0];
    s->msaa = msaa;
    s->alpha = alpha;
#ifdef CONFIG_WIN32
    s->GLon12 = s->alpha;
    s->alpha = 1;
#endif
#ifdef CONFIG_DARWIN
    s->dtimer = s->alpha;
    s->alpha = 1;
#endif
    s->opaque = 0;
    s->cwnd_fn = (void (*)(void *, void *, void *))cwnd_fn;

    SDL_GetWindowSize(s->scon->real_window, &scr_w, &scr_h);
    s->gui_saved_res = ((scr_h & 0x7FFFU) << 0x10) | scr_w;
    s->res = (((int)(scale_x * ((1.f * scr_h) / scr_w)) & 0x7FFFU) << 0x10) | scale_x;

    if (!s->ts)
        s->ts = timer_new_ms(QEMU_CLOCK_REALTIME, &sched_wndproc, s);
    timer_mod(s->ts, qemu_clock_get_ms(QEMU_CLOCK_REALTIME));
}

void mesa_release_window(void)
{
    struct sdl_console_cb *s = &scon_cb;

    sdl_display_valid("mesapt");
    s->scon = &sdl2_console[0];
    s->res = 0;
    s->opaque = 0;
    s->cwnd_fn = 0;
    s->gui_saved_res = 0;

    if (guest_sprite)
        SDL_FreeCursor(guest_sprite);
    guest_sprite = SDL_CreateSystemCursor(0);

    if (s->ts)
        timer_mod(s->ts, qemu_clock_get_ms(QEMU_CLOCK_REALTIME));
}

void mesa_cursor_define(int hot_x, int hot_y, int width, int height, const void *data)
{
    struct sdl_console_cb *s = &scon_cb;

    QemuConsole *con = s->scon ? s->scon->dcl.con : NULL;
    if (con) {
        QEMUCursor *c = cursor_alloc(width, (height & 1)? (height >> 1):height);
        c->hot_x = hot_x;
        c->hot_y = hot_y;
        if (height &  1) {
            uint8_t *and_mask = (uint8_t *)data,
                    *xor_mask = and_mask + cursor_get_mono_bpl(c) * c->height;
            cursor_set_mono(c, 0xffffff, 0x000000, xor_mask, 1, and_mask);
        }
        else
            memcpy(c->data, data, (width * height * sizeof(uint32_t)));
        dpy_cursor_define(con, c);
        cursor_unref(c);
    }
}

void mesa_mouse_warp(int x, int y, const int on)
{
    struct sdl_console_cb *s = &scon_cb;

    QemuConsole *con = s->scon ? s->scon->dcl.con : NULL;
    if (con /*&& !qemu_input_is_absolute(con)*/) {
        static int64_t last_update;
        int64_t curr_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        if (!on || (curr_time >= (last_update + GUI_REFRESH_INTERVAL_DEFAULT))) {
            last_update = curr_time;
            dpy_mouse_set(con, x, y, on);
        }
    }
}

int mesa_gui_fullscreen(int *sizev)
{
    return sdl_gui_fullscreen(sizev, "mesapt");
}

static const DisplayChangeListenerOps dcl_2d_ops = {
    .dpy_name             = "sdl2-2d",
    .dpy_gfx_update       = sdl2_2d_update,
    .dpy_gfx_switch       = sdl2_2d_switch,
    .dpy_gfx_check_format = sdl2_2d_check_format,
    .dpy_refresh          = sdl2_2d_refresh,
    .dpy_mouse_set        = sdl_mouse_warp,
    .dpy_cursor_define    = sdl_mouse_define,
};

#ifdef CONFIG_OPENGL
static const DisplayChangeListenerOps dcl_gl_ops = {
    .dpy_name                = "sdl2-gl",
    .dpy_gfx_update          = sdl2_gl_update,
    .dpy_gfx_switch          = sdl2_gl_switch,
    .dpy_gfx_check_format    = console_gl_check_format,
    .dpy_refresh             = sdl2_gl_refresh,
    .dpy_mouse_set           = sdl_mouse_warp,
    .dpy_cursor_define       = sdl_mouse_define,

    .dpy_gl_scanout_disable  = sdl2_gl_scanout_disable,
    .dpy_gl_scanout_texture  = sdl2_gl_scanout_texture,
    .dpy_gl_update           = sdl2_gl_scanout_flush,
};

static bool
sdl2_gl_is_compatible_dcl(DisplayGLCtx *dgc,
                          DisplayChangeListener *dcl)
{
    return dcl->ops == &dcl_gl_ops;
}

static const DisplayGLCtxOps gl_ctx_ops = {
    .dpy_gl_ctx_is_compatible_dcl = sdl2_gl_is_compatible_dcl,
    .dpy_gl_ctx_create       = sdl2_gl_create_context,
    .dpy_gl_ctx_destroy      = sdl2_gl_destroy_context,
    .dpy_gl_ctx_make_current = sdl2_gl_make_context_current,
};
#endif

static void sdl2_display_early_init(DisplayOptions *o)
{
    assert(o->type == DISPLAY_TYPE_SDL);
    if (o->has_gl && o->gl) {
#ifdef CONFIG_OPENGL
        display_opengl = 1;
#endif
    }
}

static void sdl2_display_init(DisplayState *ds, DisplayOptions *o)
{
    uint8_t data = 0;
    int i;
    SDL_SysWMinfo info;
    SDL_Surface *icon = NULL;
    char *dir;

    assert(o->type == DISPLAY_TYPE_SDL);

    if (SDL_GetHintBoolean("QEMU_ENABLE_SDL_LOGGING", SDL_FALSE)) {
        SDL_LogSetAllPriority(SDL_LOG_PRIORITY_VERBOSE);
    }

    if (SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "Could not initialize SDL(%s) - exiting\n",
                SDL_GetError());
        exit(1);
    }
#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR /* only available since SDL 2.0.8 */
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
#ifndef CONFIG_WIN32
    /* QEMU uses its own low level keyboard hook procedure on Windows */
    SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");
#endif
#ifdef SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED
    SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0");
#endif
    SDL_SetHint(SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4, "1");
    memset(&info, 0, sizeof(info));
    SDL_VERSION(&info.version);

    gui_fullscreen = o->has_full_screen && o->full_screen;

    if (o->u.sdl.has_grab_mod) {
        if (o->u.sdl.grab_mod == HOT_KEY_MOD_LSHIFT_LCTRL_LALT) {
            alt_grab = true;
        } else if (o->u.sdl.grab_mod == HOT_KEY_MOD_RCTRL) {
            ctrl_grab = true;
        }
    }

    for (i = 0;; i++) {
        QemuConsole *con = qemu_console_lookup_by_index(i);
        if (!con) {
            break;
        }
    }
    sdl2_num_outputs = i;
    if (sdl2_num_outputs == 0) {
        return;
    }
    sdl2_console = g_new0(struct sdl2_console, sdl2_num_outputs);
    for (i = 0; i < sdl2_num_outputs; i++) {
        QemuConsole *con = qemu_console_lookup_by_index(i);
        assert(con != NULL);
        if (!qemu_console_is_graphic(con) &&
            qemu_console_get_index(con) != 0) {
            sdl2_console[i].hidden = true;
        }
        sdl2_console[i].idx = i;
        sdl2_console[i].opts = o;
#ifdef CONFIG_OPENGL
        sdl2_console[i].opengl = display_opengl;
        sdl2_console[i].dcl.ops = display_opengl ? &dcl_gl_ops : &dcl_2d_ops;
        sdl2_console[i].dgc.ops = display_opengl ? &gl_ctx_ops : NULL;
#else
        sdl2_console[i].opengl = 0;
        sdl2_console[i].dcl.ops = &dcl_2d_ops;
#endif
        sdl2_console[i].dcl.con = con;
        sdl2_console[i].kbd = qkbd_state_init(con);
        if (display_opengl) {
            qemu_console_set_display_gl_ctx(con, &sdl2_console[i].dgc);
        }
        register_displaychangelistener(&sdl2_console[i].dcl);

#if defined(SDL_VIDEO_DRIVER_WINDOWS) || defined(SDL_VIDEO_DRIVER_X11)
        if (SDL_GetWindowWMInfo(sdl2_console[i].real_window, &info)) {
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
            qemu_console_set_window_id(con, (uintptr_t)info.info.win.window);
#elif defined(SDL_VIDEO_DRIVER_X11)
            qemu_console_set_window_id(con, info.info.x11.window);
#endif
        }
#endif
    }

#ifdef CONFIG_SDL_IMAGE
    dir = get_relocated_path(CONFIG_QEMU_ICONDIR "/hicolor/128x128/apps/qemu.png");
    icon = IMG_Load(dir);
#else
    /* Load a 32x32x4 image. White pixels are transparent. */
    dir = get_relocated_path(CONFIG_QEMU_ICONDIR "/hicolor/32x32/apps/qemu.bmp");
    icon = SDL_LoadBMP(dir);
    if (icon) {
        uint32_t colorkey = SDL_MapRGB(icon->format, 255, 255, 255);
        SDL_SetColorKey(icon, SDL_TRUE, colorkey);
    }
#endif
    g_free(dir);
    if (icon) {
        SDL_SetWindowIcon(sdl2_console[0].real_window, icon);
        scon_cb.icon = icon;
    }

    mouse_mode_notifier.notify = sdl_mouse_mode_change;
    qemu_add_mouse_mode_change_notifier(&mouse_mode_notifier);

    sdl_cursor_hidden = SDL_CreateCursor(&data, &data, 8, 1, 0, 0);
    sdl_cursor_normal = SDL_GetCursor();

    if (gui_fullscreen) {
        sdl_grab_start(&sdl2_console[0]);
    }

    atexit(sdl_cleanup);
}

static QemuDisplay qemu_display_sdl2 = {
    .type       = DISPLAY_TYPE_SDL,
    .early_init = sdl2_display_early_init,
    .init       = sdl2_display_init,
};

static void register_sdl1(void)
{
    qemu_display_register(&qemu_display_sdl2);
}

type_init(register_sdl1);

#ifdef CONFIG_OPENGL
module_dep("ui-opengl");
#endif
