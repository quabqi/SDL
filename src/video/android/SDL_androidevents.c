/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2024 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_ANDROID

#include "SDL_androidevents.h"
#include "SDL_androidkeyboard.h"
#include "SDL_androidwindow.h"
#include "../SDL_sysvideo.h"
#include "../../events/SDL_events_c.h"

#include "../../audio/android/SDL_androidaudio.h"
#include "../../audio/aaudio/SDL_aaudio.h"
#include "../../audio/openslES/SDL_openslES.h"


#ifdef SDL_VIDEO_OPENGL_EGL
static void android_egl_context_restore(SDL_Window *window)
{
    if (window) {
        SDL_Event event;
        SDL_WindowData *data = window->internal;
        SDL_GL_MakeCurrent(window, NULL);
        if (SDL_GL_MakeCurrent(window, (SDL_GLContext)data->egl_context) < 0) {
            /* The context is no longer valid, create a new one */
            data->egl_context = (EGLContext)SDL_GL_CreateContext(window);
            SDL_GL_MakeCurrent(window, (SDL_GLContext)data->egl_context);
            event.type = SDL_EVENT_RENDER_DEVICE_RESET;
            event.common.timestamp = 0;
            SDL_PushEvent(&event);
        }
        data->backup_done = SDL_FALSE;

        if (data->has_swap_interval) {
            SDL_GL_SetSwapInterval(data->swap_interval);
        }

    }
}

static void android_egl_context_backup(SDL_Window *window)
{
    if (window) {
        int interval = 0;
        /* Keep a copy of the EGL Context so we can try to restore it when we resume */
        SDL_WindowData *data = window->internal;
        data->egl_context = SDL_GL_GetCurrentContext();

        /* Save/Restore the swap interval / vsync */
        if (SDL_GL_GetSwapInterval(&interval) == 0) {
            data->has_swap_interval = 1;
            data->swap_interval = interval;
        }

        /* We need to do this so the EGLSurface can be freed */
        SDL_GL_MakeCurrent(window, NULL);
        data->backup_done = SDL_TRUE;
    }
}
#endif

/*
 * Android_ResumeSem and Android_PauseSem are signaled from Java_org_libsdl_app_SDLActivity_nativePause and Java_org_libsdl_app_SDLActivity_nativeResume
 */
static SDL_bool Android_EventsInitialized;
static SDL_bool Android_BlockOnPause = SDL_TRUE;
static SDL_bool Android_Paused;
static SDL_bool Android_PausedAudio;
static SDL_bool Android_Destroyed;

void Android_InitEvents(void)
{
    if (!Android_EventsInitialized) {
        Android_BlockOnPause = SDL_GetHintBoolean(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, SDL_TRUE);
        Android_Paused = SDL_FALSE;
        Android_Destroyed = SDL_FALSE;
        Android_EventsInitialized = SDL_TRUE;
    }
}

static void Android_OnPause(void)
{
    SDL_OnApplicationWillEnterBackground();
    SDL_OnApplicationDidEnterBackground();

    /* The semantics are that as soon as the enter background event
     * has been queued, the app will block. The application should
     * do any life cycle handling in an event filter while the event
     * was being queued.
     */
#ifdef SDL_VIDEO_OPENGL_EGL
    if (Android_Window && !Android_Window->external_graphics_context) {
        Android_LockActivityMutex();
        android_egl_context_backup(Android_Window);
        Android_UnlockActivityMutex();
    }
#endif

    if (Android_BlockOnPause) {
        /* We're blocking, also pause audio */
        ANDROIDAUDIO_PauseDevices();
        OPENSLES_PauseDevices();
        AAUDIO_PauseDevices();
        Android_PausedAudio = SDL_TRUE;
    }

    Android_Paused = SDL_TRUE;
}

static void Android_OnResume(void)
{
    Android_Paused = SDL_FALSE;

    SDL_OnApplicationWillEnterForeground();

    if (Android_PausedAudio) {
        ANDROIDAUDIO_ResumeDevices();
        OPENSLES_ResumeDevices();
        AAUDIO_ResumeDevices();
    }

#ifdef SDL_VIDEO_OPENGL_EGL
    /* Restore the GL Context from here, as this operation is thread dependent */
    if (Android_Window && !Android_Window->external_graphics_context && !SDL_HasEvent(SDL_EVENT_QUIT)) {
        Android_LockActivityMutex();
        android_egl_context_restore(Android_Window);
        Android_UnlockActivityMutex();
    }
#endif

    /* Make sure SW Keyboard is restored when an app becomes foreground */
    if (Android_Window) {
        Android_RestoreScreenKeyboardOnResume(SDL_GetVideoDevice(), Android_Window);
    }

    SDL_OnApplicationDidEnterForeground();
}

static void Android_OnLowMemory(void)
{
    SDL_SendAppEvent(SDL_EVENT_LOW_MEMORY);
}

static void Android_OnDestroy(void)
{
    /* Discard previous events. The user should have handled state storage
     * in SDL_EVENT_WILL_ENTER_BACKGROUND. After nativeSendQuit() is called, no
     * events other than SDL_EVENT_QUIT and SDL_EVENT_TERMINATING should fire */
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    SDL_SendQuit();
    SDL_SendAppEvent(SDL_EVENT_TERMINATING);

    Android_Destroyed = SDL_TRUE;
}

static void Android_HandleLifecycleEvent(SDL_AndroidLifecycleEvent event)
{
    switch (event) {
    case SDL_ANDROID_LIFECYCLE_WAKE:
        // Nothing to do, just return
        break;
    case SDL_ANDROID_LIFECYCLE_PAUSE:
        Android_OnPause();
        break;
    case SDL_ANDROID_LIFECYCLE_RESUME:
        Android_OnResume();
        break;
    case SDL_ANDROID_LIFECYCLE_LOWMEMORY:
        Android_OnLowMemory();
        break;
    case SDL_ANDROID_LIFECYCLE_DESTROY:
        Android_OnDestroy();
        break;
    default:
        break;
    }
}

static Sint64 GetLifecycleEventTimeout(SDL_bool paused, Sint64 timeoutNS)
{
    if (Android_Paused) {
        if (Android_BlockOnPause) {
            timeoutNS = -1;
        } else if (timeoutNS == 0) {
            timeoutNS = SDL_MS_TO_NS(100);
        }
    }
    return timeoutNS;
}

void Android_PumpEvents(Sint64 timeoutNS)
{
    SDL_AndroidLifecycleEvent event;
    SDL_bool paused = Android_Paused;

    while (Android_WaitLifecycleEvent(&event, GetLifecycleEventTimeout(paused, timeoutNS))) {
        Android_HandleLifecycleEvent(event);

        switch (event) {
        case SDL_ANDROID_LIFECYCLE_WAKE:
            // Finish handling events quickly if we're not paused
            timeoutNS = 0;
            break;
        case SDL_ANDROID_LIFECYCLE_PAUSE:
            // Finish handling events at the current timeout and return to process events one more time before blocking.
            break;
        case SDL_ANDROID_LIFECYCLE_RESUME:
            // Finish handling events at the resume state timeout
            paused = SDL_FALSE;
            break;
        default:
            break;
        }
    }
}

int Android_WaitActiveAndLockActivity(void)
{
    while (Android_Paused && !Android_Destroyed) {
        Android_PumpEvents(-1);
    }

    if (Android_Destroyed) {
        SDL_SetError("Android activity has been destroyed");
        return -1;
    }

    Android_LockActivityMutex();
    return 0;
}

void Android_QuitEvents(void)
{
    Android_EventsInitialized = SDL_FALSE;
}

#endif /* SDL_VIDEO_DRIVER_ANDROID */
