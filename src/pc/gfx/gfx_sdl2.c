#ifdef WAPI_SDL2

#ifdef __MINGW32__
#define FOR_WINDOWS 1
#else
#define FOR_WINDOWS 0
#endif

#if FOR_WINDOWS
#define GLEW_STATIC
#include <GL/glew.h>
#include <SDL2/SDL.h>
#define GL_GLEXT_PROTOTYPES 1
#include <SDL2/SDL_opengl.h>
#else
#include <SDL2/SDL.h>
#define GL_GLEXT_PROTOTYPES 1

#ifdef OSX_BUILD
#include <SDL2/SDL_opengl.h>
#else
#include <SDL2/SDL_opengles2.h>
#endif

#endif // End of OS-Specific GL defines

#include <stdio.h>
#include <unistd.h>

#include "gfx_window_manager_api.h"
#include "gfx_screen_config.h"
#include "../pc_main.h"
#include "../configfile.h"
#include "../cliopts.h"

#include "src/pc/controller/controller_keyboard.h"
#include "src/pc/controller/controller_sdl.h"
#include "src/pc/controller/controller_touchscreen.h"
#include "src/pc/controller/controller_bind_mapping.h"
#include "pc/utils/misc.h"

// TODO: figure out if this shit even works
#ifdef VERSION_EU
# define FRAMERATE 25
#else
# define FRAMERATE 30
#endif

#ifdef __ANDROID__
extern int render_multiplier;
#endif

// time between consequtive game frames
static const f64 sFrameTime = 1.0 / ((double)FRAMERATE);
static f64 sFrameTargetTime = 0;

static SDL_Window *wnd;
static SDL_GLContext ctx = NULL;

static kb_callback_t kb_key_down = NULL;
static kb_callback_t kb_key_up = NULL;
static void (*kb_all_keys_up)(void) = NULL;
static void (*touch_down_callback)(void* event);
static void (*touch_motion_callback)(void* event);
static void (*touch_up_callback)(void* event);
static void (*kb_text_input)(char*) = NULL;

#define IS_FULLSCREEN() ((SDL_GetWindowFlags(wnd) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)

static inline void sys_sleep(const uint64_t us) {
    // TODO: not everything has usleep()
    usleep(us);
}

static int test_vsync(void) {
    // Even if SDL_GL_SetSwapInterval succeeds, it doesn't mean that VSync actually works.
    // A 60 Hz monitor should have a swap interval of 16.67 milliseconds.
    // Try to detect the length of a vsync by swapping buffers some times.
    // Since the graphics card may enqueue a fixed number of frames,
    // first send in four dummy frames to hopefully fill the queue.
    // This method will fail if the refresh rate is changed, which, in
    // combination with that we can't control the queue size (i.e. lag)
    // is a reason this generic SDL2 backend should only be used as last resort.

    for (int i = 0; i < 8; ++i)
        SDL_GL_SwapWindow(wnd);

    Uint32 start = SDL_GetTicks();
    SDL_GL_SwapWindow(wnd);
    SDL_GL_SwapWindow(wnd);
    SDL_GL_SwapWindow(wnd);
    SDL_GL_SwapWindow(wnd);
    Uint32 end = SDL_GetTicks();

    const float average = 4.0 * 1000.0 / (end - start);

#ifndef __ANDROID__
    if (average > 27.0f && average < 33.0f) return 1;
    if (average > 57.0f && average < 63.0f) return 2;
    if (average > 86.0f && average < 94.0f) return 3;
    if (average > 115.0f && average < 125.0f) return 4;
    if (average > 234.0f && average < 246.0f) return 8;

    return 0;
#else
    /*Android's vsync seems finicky but timer based sync seems unusable too.
     * I think vsync does kind of work but not half-vsync and stuff like that.
     * Let's try to render multiple times if neccessary to lower the framerate.
     * I don't think this is a great solution but it works.
     * On SGI models, turning vsync off will help with framerate, but the best is 60fps patch.
     * The actual solution would be to render or copy the buffer to a texture
     * and then render that to the screen.*/
    render_multiplier = (average + 15) / 30;
    if (render_multiplier == 0)
        render_multiplier = 1;

    return 1;
#endif
}

static inline void gfx_sdl_set_vsync(const bool enabled) {
    if (enabled) {
        // try to detect refresh rate
        SDL_GL_SetSwapInterval(1);
        const int vblanks = gCLIOpts.SyncFrames ? (int)gCLIOpts.SyncFrames : test_vsync();
        if (vblanks) {
            printf("determined swap interval: %d\n", vblanks);
            SDL_GL_SetSwapInterval(vblanks);
            use_timer = false;
            return;
        } else {
            printf("could not determine swap interval, falling back to timer sync\n");
        }
    }

    use_timer = true;
    SDL_GL_SetSwapInterval(0);
}

static void gfx_sdl_set_fullscreen(void) {
    if (configWindow.reset)
        configWindow.fullscreen = false;
    if (configWindow.fullscreen == IS_FULLSCREEN())
        return;
    if (configWindow.fullscreen) {
        SDL_SetWindowFullscreen(wnd, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        SDL_SetWindowFullscreen(wnd, 0);
        configWindow.exiting_fullscreen = true;
    }
}

static void gfx_sdl_reset_dimension_and_pos(void) {
    if (configWindow.exiting_fullscreen)
        configWindow.exiting_fullscreen = false;

    if (configWindow.reset) {
        configWindow.x = WAPI_WIN_CENTERPOS;
        configWindow.y = WAPI_WIN_CENTERPOS;
        configWindow.w = DESIRED_SCREEN_WIDTH;
        configWindow.h = DESIRED_SCREEN_HEIGHT;
        configWindow.reset = false;
    } else if (!configWindow.settings_changed) {
        return;
    }

    int xpos = (configWindow.x == WAPI_WIN_CENTERPOS) ? SDL_WINDOWPOS_CENTERED : configWindow.x;
    int ypos = (configWindow.y == WAPI_WIN_CENTERPOS) ? SDL_WINDOWPOS_CENTERED : configWindow.y;

    SDL_SetWindowSize(wnd, configWindow.w, configWindow.h);
    SDL_SetWindowPosition(wnd, xpos, ypos);
    // in case vsync changed
    gfx_sdl_set_vsync(configWindow.vsync);
}

static void gfx_sdl_init(const char *window_title) {
    SDL_Init(SDL_INIT_VIDEO);

	SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
 
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    #ifdef USE_GLES
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);  // These attributes allow for hardware acceleration on RPis.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    #endif

    //SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    //SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

    int xpos = (configWindow.x == WAPI_WIN_CENTERPOS) ? SDL_WINDOWPOS_CENTERED : configWindow.x;
    int ypos = (configWindow.y == WAPI_WIN_CENTERPOS) ? SDL_WINDOWPOS_CENTERED : configWindow.y;

    wnd = SDL_CreateWindow(
        window_title,
        xpos, ypos, configWindow.w, configWindow.h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    ctx = SDL_GL_CreateContext(wnd);

    gfx_sdl_set_vsync(configWindow.vsync);

    gfx_sdl_set_fullscreen();
    if (configWindow.fullscreen) {
        SDL_ShowCursor(SDL_DISABLE);
    }

    controller_bind_init();
}

static void gfx_sdl_main_loop(void (*run_one_game_iter)(void)) {
    run_one_game_iter();
}

static void gfx_sdl_get_dimensions(uint32_t *width, uint32_t *height) {
    int w, h;
    SDL_GetWindowSize(wnd, &w, &h);
    if (width) *width = w;
    if (height) *height = h;
}

static void gfx_sdl_onkeydown(int scancode) {
    const Uint8 *state = SDL_GetKeyboardState(NULL);

    if (state[SDL_SCANCODE_LALT] && state[SDL_SCANCODE_RETURN]) {
        configWindow.fullscreen = !configWindow.fullscreen;
        configWindow.settings_changed = true;
        return;
    }

    if (kb_key_down)
        kb_key_down(translate_sdl_scancode(scancode));
}

static void gfx_sdl_onkeyup(int scancode) {
    if (kb_key_up)
        kb_key_up(translate_sdl_scancode(scancode));
}

#ifdef TOUCH_CONTROLS
static void gfx_sdl_fingerdown(SDL_TouchFingerEvent sdl_event) {
    struct TouchEvent event;
    event.x = sdl_event.x;
    event.y = sdl_event.y;
    event.touchID = sdl_event.fingerId + 1;
    if (touch_down_callback != NULL) {
        touch_down_callback((void*)&event);
    }
}

static void gfx_sdl_fingermotion(SDL_TouchFingerEvent sdl_event) {
    struct TouchEvent event;
    event.x = sdl_event.x;
    event.y = sdl_event.y;
    event.touchID = sdl_event.fingerId + 1;
    if (touch_motion_callback != NULL) {
        touch_motion_callback((void*)&event);
    }
}

static void gfx_sdl_fingerup(SDL_TouchFingerEvent sdl_event) {
    struct TouchEvent event;
    event.x = sdl_event.x;
    event.y = sdl_event.y;
    event.touchID = sdl_event.fingerId + 1;
    if (touch_up_callback != NULL) {
        touch_up_callback((void*)&event);
    }
}
#endif

static void gfx_sdl_handle_events(void) {
    SDL_StartTextInput();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_TEXTINPUT:
                kb_text_input(event.text.text);
                break;
            case SDL_KEYDOWN:
                gfx_sdl_onkeydown(event.key.keysym.scancode);
                break;
            case SDL_KEYUP:
                gfx_sdl_onkeyup(event.key.keysym.scancode);
                break;
				
			#ifdef TOUCH_CONTROLS
			case SDL_FINGERDOWN:
					gfx_sdl_fingerdown(event.tfinger);
					break;
			case SDL_FINGERMOTION:
					gfx_sdl_fingermotion(event.tfinger);
					break;
			case SDL_FINGERUP:
					gfx_sdl_fingerup(event.tfinger);
					break;
			#endif
				
            case SDL_WINDOWEVENT:
                if (!IS_FULLSCREEN()) {
                    switch (event.window.event) {
                        case SDL_WINDOWEVENT_MOVED:
                            if (!configWindow.exiting_fullscreen) {
                                if (event.window.data1 >= 0) configWindow.x = event.window.data1;
                                if (event.window.data2 >= 0) configWindow.y = event.window.data2;
                            }
                            break;
                        case SDL_WINDOWEVENT_SIZE_CHANGED:
                            configWindow.w = event.window.data1;
                            configWindow.h = event.window.data2;
                            break;
                    }
                }
                break;
            case SDL_QUIT:
                game_exit();
                break;
        }
    }

    if (configWindow.settings_changed) {
        gfx_sdl_set_fullscreen();
        gfx_sdl_reset_dimension_and_pos();
        configWindow.settings_changed = false;
    }
}

static void gfx_sdl_set_keyboard_callbacks(kb_callback_t on_key_down, kb_callback_t on_key_up, void (*on_all_keys_up)(void), void (*on_text_input)(char*)) {
    kb_key_down = on_key_down;
    kb_key_up = on_key_up;
    kb_all_keys_up = on_all_keys_up;
    kb_text_input = on_text_input;
}

static void gfx_sdl_set_touchscreen_callbacks(void (*down)(void* event), void (*motion)(void* event), void (*up)(void* event)) {
    touch_down_callback = down;
    touch_motion_callback = motion;
    touch_up_callback = up;
}

static bool gfx_sdl_start_frame(void) {
    static Uint32 last_time = 0;
    bool ret = true;
    Uint32 ticks = SDL_GetTicks();
    if ((last_time == 0) || (last_time + 10000 < ticks))
        last_time = ticks;
    if (last_time + frame_time < ticks)
        ret = false;
    last_time += frame_time;
    return ret;
}

static inline void sync_framerate_with_timer(void) {
    static double last_time;
    static double last_sec;
    static int frames_since_last_sec;
    const double now = SDL_GetPerformanceCounter();
    frames_since_last_sec += 1;
    if (last_time) {
        const double elapsed = last_sec ? (now - last_sec) : (now - last_time);
        if ((elapsed < frame_time && !last_sec) || (elapsed < frames_since_last_sec * frame_time && last_sec)) {
            const double delay = last_sec ? frames_since_last_sec * frame_time - elapsed : frame_time - elapsed;
            sys_sleep(delay / perf_freq * 1000000.0);
            last_time = now + delay;
        } else {
            last_time = now;
        }
        if ((int64_t)(now / perf_freq) > (int64_t)(last_sec / perf_freq)) {
            last_sec = last_time;
            frames_since_last_sec = 0;
        }
    } else {
        last_time = now;
    }
}

static void gfx_sdl_swap_buffers_begin(void) {
    if (use_timer) sync_framerate_with_timer();
    SDL_GL_SwapWindow(wnd);
}

static void gfx_sdl_swap_buffers_end(void) {
}

static double gfx_sdl_get_time(void) {
    return 0.0;
}

static void gfx_sdl_delay(u32 ms) {
    SDL_Delay(ms);
}

static void gfx_sdl_shutdown(void) {
    if (SDL_WasInit(0)) {
        if (ctx) { SDL_GL_DeleteContext(ctx); ctx = NULL; }
        if (wnd) { SDL_DestroyWindow(wnd); wnd = NULL; }
        SDL_Quit();
    }
}

static void gfx_sdl_start_text_input(void) { SDL_StartTextInput(); }
static void gfx_sdl_stop_text_input(void) { SDL_StopTextInput(); }
static char* gfx_sdl_get_clipboard_text(void) { return SDL_GetClipboardText(); }
static void gfx_sdl_set_clipboard_text(char* text) { SDL_SetClipboardText(text); }
static void gfx_sdl_set_cursor_visible(bool visible) { SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE); }

struct GfxWindowManagerAPI gfx_sdl = {
    gfx_sdl_init,
    gfx_sdl_set_keyboard_callbacks,
	gfx_sdl_set_touchscreen_callbacks,
    gfx_sdl_main_loop,
    gfx_sdl_get_dimensions,
    gfx_sdl_handle_events,
    gfx_sdl_start_frame,
    gfx_sdl_swap_buffers_begin,
    gfx_sdl_swap_buffers_end,
    gfx_sdl_get_time,
    gfx_sdl_shutdown,
    gfx_sdl_start_text_input,
    gfx_sdl_stop_text_input,
    gfx_sdl_get_clipboard_text,
    gfx_sdl_set_clipboard_text,
    gfx_sdl_set_cursor_visible,
    gfx_sdl_delay,
};

#endif // BACKEND_WM
