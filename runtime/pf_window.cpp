// 3dokit runtime -- pfboot's window (SDL3): what the display shows, the sound the DSP makes, the
// host's keyboard and a gamepad as the first Control Pad, and the guest's clock held to the host's.
//
// The program runs on a thread of its own (its tasks on theirs, pf_task.cpp) and the window on the
// host's main thread, as SDL wants. At each vertical blank the program's thread hands the window
// the field the VDLs describe (pf_display_field) and waits until the host's clock has caught up
// with the guest's -- the field's time counted from the run's first field; when the host has
// fallen behind by more than a few fields it counts again from there instead of running fast to
// catch up. The window shows the latest field, its 320 x lines scaled to the window with nearest
// neighbours. The pad is read from the keyboard and the first gamepad each time round the window's
// loop and handed to the event broker (pf_pad_live), which looks at it once a field; a record
// (--record FILE) writes each press as the --pad option that replays it on the field it landed
// on, so a run played in the window can be run again by pfboot, frame for frame.
//
// Keys: the arrows; Z, X, C for A, B, C; Enter for P (start); Backspace for X (stop); Q and W for
// L and R. A gamepad: the d-pad or the left stick; south, east, west for A, B, C; start for P;
// back for X; the shoulders for L and R. Closing the window ends the run (no key does: a player
// reaching for Esc to pause would end it -- the game's pause is P).
#include "pf.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>

namespace {

enum : uint32_t {
    PAD_DOWN = 0x80000000u, PAD_UP = 0x40000000u, PAD_RIGHT = 0x20000000u, PAD_LEFT = 0x10000000u,
    PAD_A = 0x08000000u, PAD_B = 0x04000000u, PAD_C = 0x02000000u, PAD_P = 0x01000000u,
    PAD_X = 0x00800000u, PAD_R = 0x00400000u, PAD_L = 0x00200000u,
};

std::mutex g_lock;                  // the field handed over
std::vector<uint8_t> g_rgb;
int g_lines;
uint64_t g_seq;
std::atomic<bool> g_done{false};
int g_result;
SDL_AudioStream* g_audio;

// The sound (pf_dsp.cpp), from the program's thread, as it is made: the guest's clock is held to
// the host's, so it comes at the rate it plays, a field's worth at a time -- the first time behind
// 60 ms of silence, which the stream keeps as its margin; past half a second queued (the host
// having held the guest back less than it plays) the queue starts again.
void window_audio(const int16_t* lr, size_t frames) {
    static bool primed;
    if (!primed) {
        primed = true;
        static const int16_t quiet[2 * 2646] = {};
        SDL_PutAudioStreamData(g_audio, quiet, sizeof quiet);
    }
    if (SDL_GetAudioStreamQueued(g_audio) > 44100 * 4 / 2) SDL_ClearAudioStream(g_audio);
    SDL_PutAudioStreamData(g_audio, lr, (int)(frames * 4));
}

using Clock = std::chrono::steady_clock;
Clock::time_point g_host0;
uint64_t g_guest0;
bool g_started;

// The program's thread, at each vertical blank.
void window_vbl(uint64_t when) {
    std::vector<uint8_t> rgb;
    int lines;
    if (pf_display_field(rgb, lines)) {
        std::lock_guard<std::mutex> l(g_lock);
        g_rgb.swap(rgb);
        g_lines = lines;
        ++g_seq;
    }
    Clock::time_point now = Clock::now();
    if (!g_started) {
        g_started = true;
        g_host0 = now;
        g_guest0 = when;
        return;
    }
    Clock::time_point due = g_host0 + std::chrono::nanoseconds(when - g_guest0);
    if (due > now) std::this_thread::sleep_until(due);
    else if (now - due > std::chrono::milliseconds(100)) {
        g_host0 = now;
        g_guest0 = when;
    }
}

uint32_t keyboard_pad() {
    static const struct { SDL_Scancode key; uint32_t bit; } kKeys[] = {
        {SDL_SCANCODE_UP, PAD_UP}, {SDL_SCANCODE_DOWN, PAD_DOWN}, {SDL_SCANCODE_LEFT, PAD_LEFT},
        {SDL_SCANCODE_RIGHT, PAD_RIGHT}, {SDL_SCANCODE_Z, PAD_A}, {SDL_SCANCODE_X, PAD_B},
        {SDL_SCANCODE_C, PAD_C}, {SDL_SCANCODE_RETURN, PAD_P}, {SDL_SCANCODE_BACKSPACE, PAD_X},
        {SDL_SCANCODE_Q, PAD_L}, {SDL_SCANCODE_W, PAD_R},
    };
    const bool* k = SDL_GetKeyboardState(nullptr);
    uint32_t bits = 0;
    for (const auto& m : kKeys)
        if (k[m.key]) bits |= m.bit;
    return bits;
}

uint32_t gamepad_pad(SDL_Gamepad* g) {
    if (!g) return 0;
    static const struct { SDL_GamepadButton button; uint32_t bit; } kButtons[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_RIGHT},
        {SDL_GAMEPAD_BUTTON_SOUTH, PAD_A}, {SDL_GAMEPAD_BUTTON_EAST, PAD_B},
        {SDL_GAMEPAD_BUTTON_WEST, PAD_C}, {SDL_GAMEPAD_BUTTON_START, PAD_P},
        {SDL_GAMEPAD_BUTTON_BACK, PAD_X}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_L},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_R},
    };
    uint32_t bits = 0;
    for (const auto& m : kButtons)
        if (SDL_GetGamepadButton(g, m.button)) bits |= m.bit;
    const int kDead = 16000;
    int x = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
    if (x < -kDead) bits |= PAD_LEFT;
    if (x > kDead) bits |= PAD_RIGHT;
    if (y < -kDead) bits |= PAD_UP;
    if (y > kDead) bits |= PAD_DOWN;
    return bits;
}

} // namespace

// pfboot --window: `run` (the program's boot and run) on a thread of its own, the window here.
// The program's result, or 0 when the window is closed first (the program's thread is left as it
// is: the process ends).
int pf_window_run(const char* title, const char* record, const std::function<int()>& run) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "--window: %s\n", SDL_GetError());
        return 2;
    }
    // The sound: 44,100 Hz, 16-bit stereo, as the DSP makes it. Without an audio device the run goes
    // on silent.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        const SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 44100};
        g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (g_audio) {
            SDL_ResumeAudioStreamDevice(g_audio);
            g_pf_audio_out = window_audio;
        }
    }
    if (!g_audio) std::fprintf(stderr, "--window: no sound (%s)\n", SDL_GetError());
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    if (!SDL_CreateWindowAndRenderer(title, 960, 720, SDL_WINDOW_RESIZABLE, &win, &ren)) {
        std::fprintf(stderr, "--window: %s\n", SDL_GetError());
        return 2;
    }
    if (record && !pf_pad_record_open(record)) {
        std::fprintf(stderr, "--record %s: cannot write it\n", record);
        return 2;
    }
    std::atexit(pf_pad_record_close);           // a run that stops still writes the buttons held
    g_pf_display_vbl = window_vbl;
    g_pf_wait_forever = true;
    std::thread program([&run] {
        g_result = run();
        g_done = true;
    });
    SDL_Texture* tex = nullptr;
    int tex_lines = 0;
    uint64_t shown = 0;
    SDL_Gamepad* pad = nullptr;
    std::vector<uint8_t> rgb;
    bool quit = false;
    while (!quit && !g_done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) quit = true;
            else if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad) pad = SDL_OpenGamepad(e.gdevice.which);
            else if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad && SDL_GetGamepadID(pad) == e.gdevice.which) {
                SDL_CloseGamepad(pad);
                pad = nullptr;
            }
        }
        pf_pad_live(keyboard_pad() | gamepad_pad(pad));
        int lines = 0;
        {
            std::lock_guard<std::mutex> l(g_lock);
            if (g_seq != shown) {
                shown = g_seq;
                rgb = g_rgb;
                lines = g_lines;
            }
        }
        if (lines) {
            if (lines != tex_lines) {
                if (tex) SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, 320, lines);
                SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
                SDL_SetRenderLogicalPresentation(ren, 320, lines, SDL_LOGICAL_PRESENTATION_LETTERBOX);
                tex_lines = lines;
            }
            SDL_UpdateTexture(tex, nullptr, rgb.data(), 320 * 3);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            SDL_RenderTexture(ren, tex, nullptr, nullptr);
            SDL_RenderPresent(ren);
        }
        SDL_WaitEventTimeout(nullptr, 4);
    }
    pf_pad_record_close();
    if (!g_done) {                              // closed by the player: the program cannot be stopped
        std::fflush(nullptr);
        std::_Exit(0);
    }
    program.join();
    if (pad) SDL_CloseGamepad(pad);
    if (tex) SDL_DestroyTexture(tex);
    if (g_audio) {
        g_pf_audio_out = nullptr;
        SDL_DestroyAudioStream(g_audio);
    }
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return g_result;
}
