/******************************************************************************
 * OpenHD
 *
 * Licensed under the GNU General Public License (GPL) Version 3.
 *
 * This software is provided "as-is," without warranty of any kind, express or
 * implied, including but not limited to the warranties of merchantability,
 * fitness for a particular purpose, and non-infringement. For details, see the
 * full license in the LICENSE file provided with this source code.
 *
 * Non-Military Use Only:
 * This software and its associated components are explicitly intended for
 * civilian and non-military purposes. Use in any military or defense
 * applications is strictly prohibited unless explicitly and individually
 * licensed otherwise by the OpenHD Team.
 *
 * Contributors:
 * A full list of contributors can be found at the OpenHD GitHub repository:
 * https://github.com/OpenHD
 *
 * © OpenHD, All Rights Reserved.
 ******************************************************************************/

#include "dev/sdl_gles_window.hpp"

#include <utility>

#if OPENHD_GLIDE_HAS_SDL2
#include <SDL.h>
#endif

namespace glide::dev {

bool sdl_gles_available()
{
#if OPENHD_GLIDE_HAS_SDL2 && OPENHD_GLIDE_HAS_GLESV2
    return true;
#else
    return false;
#endif
}

SdlGlesWindow::~SdlGlesWindow()
{
#if OPENHD_GLIDE_HAS_SDL2
    if (context_ != nullptr) {
        SDL_GL_DeleteContext(context_);
    }
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
#endif
}

bool SdlGlesWindow::create(const char* title, std::uint32_t width, std::uint32_t height)
{
    return create(title, WindowPlacement {
        .width = width,
        .height = height,
    });
}

bool SdlGlesWindow::create(const char* title, WindowPlacement placement)
{
#if OPENHD_GLIDE_HAS_SDL2 && OPENHD_GLIDE_HAS_GLESV2
#if defined(_WIN32)
    // ANGLE supplies GLES on Windows; ask SDL to create its context through EGL.
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");
    SDL_SetHint("SDL_WINDOWS_DPI_SCALING", "0");
    SDL_SetHint("SDL_OPENGL_ES_DRIVER", "1");
    SDL_SetHint("SDL_VIDEO_FORCE_EGL", "1");
#endif
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        last_error_ = SDL_GetError();
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    auto flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN;
    if (placement.borderless) {
        flags |= SDL_WINDOW_BORDERLESS;
    }

    window_ = SDL_CreateWindow(
        title,
        placement.positioned ? placement.x : SDL_WINDOWPOS_CENTERED,
        placement.positioned ? placement.y : SDL_WINDOWPOS_CENTERED,
        static_cast<int>(placement.width),
        static_cast<int>(placement.height),
        flags);

    if (window_ == nullptr) {
        last_error_ = SDL_GetError();
        return false;
    }

    if (placement.opacity < 1.0F) {
        SDL_SetWindowOpacity(window_, placement.opacity);
    }
    if (placement.always_on_top) {
        SDL_SetWindowAlwaysOnTop(window_, SDL_TRUE);
    }

    context_ = SDL_GL_CreateContext(window_);
    if (context_ == nullptr) {
        last_error_ = SDL_GetError();
        return false;
    }

    if (SDL_GL_SetSwapInterval(-1) != 0) {
        SDL_GL_SetSwapInterval(1);
    }
    surface_ = flow::SurfaceSize {
        .width = placement.width,
        .height = placement.height,
    };
    return true;
#else
    (void)title;
    (void)placement;
    last_error_ = "SDL2 and OpenGL ES 2.0 are required for preview windows";
    return false;
#endif
}

bool SdlGlesWindow::poll()
{
#if OPENHD_GLIDE_HAS_SDL2
    SDL_Event event;
    while (SDL_PollEvent(&event) != 0) {
        if (event.type == SDL_QUIT) {
            return false;
        }
        if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            surface_.width = static_cast<std::uint32_t>(event.window.data1);
            surface_.height = static_cast<std::uint32_t>(event.window.data2);
        }
        if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
            click_x_ = event.button.x;
            click_y_ = event.button.y;
            has_click_ = true;
        }
        if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
            switch (event.key.keysym.sym) {
            case SDLK_m: key_ = "m"; break;
            case SDLK_n: key_ = "n"; break;
            case SDLK_SPACE: key_ = "space"; break;
            case SDLK_PLUS:
            case SDLK_KP_PLUS:
            case SDLK_EQUALS: key_ = "+"; break;
            case SDLK_MINUS:
            case SDLK_KP_MINUS: key_ = "-"; break;
            case SDLK_UP: key_ = "up"; break;
            case SDLK_DOWN: key_ = "down"; break;
            case SDLK_LEFT: key_ = "left"; break;
            case SDLK_RIGHT: key_ = "right"; break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER: key_ = "enter"; break;
            case SDLK_ESCAPE:
            case SDLK_BACKSPACE: key_ = "back"; break;
            default: break;
            }
        }
    }
#endif

    return true;
}

bool SdlGlesWindow::consume_key(std::string& key)
{
    if (key_.empty()) return false;
    key = std::move(key_);
    key_.clear();
    return true;
}

bool SdlGlesWindow::consume_click(int& x, int& y)
{
    if (!has_click_) {
        return false;
    }

    x = click_x_;
    y = click_y_;
    has_click_ = false;
    return true;
}

void SdlGlesWindow::swap()
{
#if OPENHD_GLIDE_HAS_SDL2
    if (window_ != nullptr) {
        SDL_GL_SwapWindow(window_);
    }
#endif
}

flow::SurfaceSize SdlGlesWindow::surface_size() const
{
    return surface_;
}

const std::string& SdlGlesWindow::last_error() const
{
    return last_error_;
}

} // namespace glide::dev
