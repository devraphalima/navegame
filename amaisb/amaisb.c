#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#define CANVAS_WIDTH 320
#define CANVAS_HEIGHT 180
#define SAMPLE_RATE 44100
#define SAMPLES_PER_BUFFER 4096
#define MAX_BULLETS 128
#define MAX_METEORS 64
#define ANIMATION_TIMER 1
#define SHIP_Y 151

typedef enum Screen
{
    SCREEN_MENU,
    SCREEN_GAME
} Screen;

typedef struct Bullet
{
    int x;
    int y;
} Bullet;

typedef struct Meteor
{
    int x;
    int y;
    int speed;
} Meteor;

typedef struct AudioBuffer
{
    WAVEHDR header;
    short samples[SAMPLES_PER_BUFFER];
} AudioBuffer;

static volatile LONG gAudioRunning = 0;
static volatile LONG gAudioEnabled = 1;
static volatile LONG gAudioAvailable = 0;
static volatile LONG gEffectFrequency = 880;
static volatile LONG gEffectSamplesRemaining = 0;
static HANDLE gAudioThread = NULL;

static Screen gCurrentScreen = SCREEN_MENU;
static int gSelectedItem = 0;
static int gAnimationFrame = 0;
static int gShipX = CANVAS_WIDTH / 2;
static int gScore = 0;
static int gShotCooldown = 0;
static int gMeteorSpawnTimer = 0;
static BOOL gGameOver = FALSE;
static uint32_t gRandomState = 0xA341316Cu;
static Bullet gBullets[MAX_BULLETS];
static int gBulletCount = 0;
static Meteor gMeteors[MAX_METEORS];
static int gMeteorCount = 0;

static const RECT gMenuItems[3] = {
    { 105, 78, 215, 97 },
    { 105, 102, 215, 121 },
    { 105, 126, 215, 145 }
};

static int min_int(int a, int b)
{
    return a < b ? a : b;
}

static int max_int(int a, int b)
{
    return a > b ? a : b;
}

static int random_in_range(int minimum, int maximum)
{
    gRandomState ^= gRandomState << 13;
    gRandomState ^= gRandomState >> 17;
    gRandomState ^= gRandomState << 5;
    return minimum + (int)(gRandomState %
        (uint32_t)(maximum - minimum + 1));
}

static BOOL rectangles_overlap(int leftA, int topA, int rightA, int bottomA,
    int leftB, int topB, int rightB, int bottomB)
{
    return leftA < rightB && rightA > leftB &&
        topA < bottomB && bottomA > topB;
}

static uint32_t phase_increment(int frequency)
{
    return (uint32_t)(((uint64_t)(unsigned int)frequency << 32) /
        SAMPLE_RATE);
}

static BOOL consume_effect_sample(void)
{
    LONG remaining;
    do
    {
        remaining = InterlockedCompareExchange(
            &gEffectSamplesRemaining, 0, 0);
        if (remaining <= 0)
        {
            return FALSE;
        }
    } while (InterlockedCompareExchange(&gEffectSamplesRemaining,
        remaining - 1, remaining) != remaining);

    return TRUE;
}

static void fill_audio_buffer(AudioBuffer* buffer, uint64_t* samplePosition)
{
    static const int melody[16] = {
        523, 659, 784, 659, 587, 698, 880, 698,
        523, 659, 784, 988, 880, 784, 659, 587
    };
    static const int bass[8] = {
        131, 165, 196, 165, 147, 175, 220, 175
    };
    const int samplesPerBeat = SAMPLE_RATE / 5;
    const BOOL enabled = InterlockedCompareExchange(&gAudioEnabled, 0, 0) != 0;
    int i;

    for (i = 0; i < SAMPLES_PER_BUFFER; ++i)
    {
        int mixed = 0;
        if (enabled)
        {
            const int beat = (int)((*samplePosition / samplesPerBeat) % 16);
            const int note = melody[beat];
            const int bassNote = bass[beat % 8];
            const uint32_t melodyPhase =
                (uint32_t)(*samplePosition * phase_increment(note));
            const uint32_t bassPhase =
                (uint32_t)(*samplePosition * phase_increment(bassNote));

            mixed += (melodyPhase & 0x80000000u) != 0 ? 3000 : -3000;
            mixed += (bassPhase & 0x80000000u) != 0 ? 1400 : -1400;

            if (consume_effect_sample())
            {
                const LONG frequency =
                    InterlockedCompareExchange(&gEffectFrequency, 0, 0);
                const uint32_t effectPhase = (uint32_t)(*samplePosition *
                    phase_increment((int)frequency));
                mixed += (effectPhase & 0x80000000u) != 0 ? 7000 : -7000;
            }
        }

        if (mixed > 32767)
        {
            mixed = 32767;
        }
        else if (mixed < -32768)
        {
            mixed = -32768;
        }
        buffer->samples[i] = (short)mixed;
        ++(*samplePosition);
    }
}

static DWORD WINAPI audio_thread_proc(LPVOID parameter)
{
    WAVEFORMATEX format;
    HANDLE eventHandle;
    HWAVEOUT output = NULL;
    MMRESULT result;
    AudioBuffer buffers[2];
    int preparedCount = 0;
    BOOL success = TRUE;
    uint64_t samplePosition = 0;
    int i;

    (void)parameter;
    ZeroMemory(&format, sizeof(format));
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = SAMPLE_RATE;
    format.wBitsPerSample = 16;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    eventHandle = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (eventHandle == NULL)
    {
        OutputDebugStringA("Nao foi possivel criar o evento de audio.\n");
        return 1;
    }

    result = waveOutOpen(&output, WAVE_MAPPER, &format,
        (DWORD_PTR)eventHandle, 0, CALLBACK_EVENT);
    if (result != MMSYSERR_NOERROR)
    {
        OutputDebugStringA("Nao foi possivel iniciar o audio.\n");
        CloseHandle(eventHandle);
        return 1;
    }
    InterlockedExchange(&gAudioAvailable, 1);

    ZeroMemory(buffers, sizeof(buffers));
    for (i = 0; i < 2; ++i)
    {
        buffers[i].header.lpData = (LPSTR)buffers[i].samples;
        buffers[i].header.dwBufferLength =
            (DWORD)sizeof(buffers[i].samples);
        result = waveOutPrepareHeader(output, &buffers[i].header,
            sizeof(buffers[i].header));
        if (result != MMSYSERR_NOERROR)
        {
            success = FALSE;
            break;
        }
        ++preparedCount;
    }

    if (success)
    {
        for (i = 0; i < 2; ++i)
        {
            fill_audio_buffer(&buffers[i], &samplePosition);
            result = waveOutWrite(output, &buffers[i].header,
                sizeof(buffers[i].header));
            if (result != MMSYSERR_NOERROR)
            {
                success = FALSE;
                break;
            }
        }
    }

    while (success &&
        InterlockedCompareExchange(&gAudioRunning, 0, 0) != 0)
    {
        WaitForSingleObject(eventHandle, 100);
        for (i = 0; i < 2; ++i)
        {
            if ((buffers[i].header.dwFlags & WHDR_DONE) != 0 &&
                InterlockedCompareExchange(&gAudioRunning, 0, 0) != 0)
            {
                fill_audio_buffer(&buffers[i], &samplePosition);
                result = waveOutWrite(output, &buffers[i].header,
                    sizeof(buffers[i].header));
                if (result != MMSYSERR_NOERROR)
                {
                    success = FALSE;
                    break;
                }
            }
        }
    }

    if (!success)
    {
        InterlockedExchange(&gAudioAvailable, 0);
        OutputDebugStringA("O dispositivo de audio parou de responder.\n");
    }

    waveOutReset(output);
    for (i = 0; i < preparedCount; ++i)
    {
        if ((buffers[i].header.dwFlags & WHDR_PREPARED) != 0)
        {
            waveOutUnprepareHeader(output, &buffers[i].header,
                sizeof(buffers[i].header));
        }
    }
    waveOutClose(output);
    CloseHandle(eventHandle);
    return success ? 0 : 1;
}

static void audio_start(void)
{
    InterlockedExchange(&gAudioRunning, 1);
    gAudioThread = CreateThread(NULL, 0, audio_thread_proc, NULL, 0, NULL);
    if (gAudioThread == NULL)
    {
        InterlockedExchange(&gAudioRunning, 0);
        InterlockedExchange(&gAudioAvailable, 0);
        OutputDebugStringA("Nao foi possivel iniciar a thread de audio.\n");
    }
}

static void audio_stop(void)
{
    InterlockedExchange(&gAudioRunning, 0);
    if (gAudioThread != NULL)
    {
        WaitForSingleObject(gAudioThread, INFINITE);
        CloseHandle(gAudioThread);
        gAudioThread = NULL;
    }
}

static BOOL audio_is_enabled(void)
{
    return InterlockedCompareExchange(&gAudioEnabled, 0, 0) != 0;
}

static BOOL audio_is_available(void)
{
    return InterlockedCompareExchange(&gAudioAvailable, 0, 0) != 0;
}

static void audio_set_enabled(BOOL enabled)
{
    InterlockedExchange(&gAudioEnabled, enabled ? 1 : 0);
}

static void audio_play_effect(int frequency)
{
    InterlockedExchange(&gEffectFrequency, frequency);
    InterlockedExchange(&gEffectSamplesRemaining, SAMPLE_RATE / 12);
}

static void remove_bullet(int index)
{
    if (index < 0 || index >= gBulletCount)
    {
        return;
    }
    if (index + 1 < gBulletCount)
    {
        memmove(&gBullets[index], &gBullets[index + 1],
            (size_t)(gBulletCount - index - 1) * sizeof(gBullets[0]));
    }
    --gBulletCount;
}

static void remove_meteor(int index)
{
    if (index < 0 || index >= gMeteorCount)
    {
        return;
    }
    if (index + 1 < gMeteorCount)
    {
        memmove(&gMeteors[index], &gMeteors[index + 1],
            (size_t)(gMeteorCount - index - 1) * sizeof(gMeteors[0]));
    }
    --gMeteorCount;
}

static void reset_game(void)
{
    gShipX = CANVAS_WIDTH / 2;
    gScore = 0;
    gShotCooldown = 0;
    gMeteorSpawnTimer = 10;
    gGameOver = FALSE;
    gBulletCount = 0;
    gMeteorCount = 0;
}

static void update_game(HWND window)
{
    int i;
    if (gGameOver)
    {
        return;
    }

    if (GetForegroundWindow() == window)
    {
        if ((GetAsyncKeyState(VK_LEFT) & 0x8000) != 0 ||
            (GetAsyncKeyState('A') & 0x8000) != 0)
        {
            gShipX -= 3;
        }
        if ((GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0 ||
            (GetAsyncKeyState('D') & 0x8000) != 0)
        {
            gShipX += 3;
        }
        if ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0 &&
            gShotCooldown == 0 && gBulletCount < MAX_BULLETS)
        {
            gBullets[gBulletCount].x = gShipX;
            gBullets[gBulletCount].y = SHIP_Y - 10;
            ++gBulletCount;
            gShotCooldown = 5;
            audio_play_effect(1320);
        }
    }

    gShipX = max_int(12, min_int(CANVAS_WIDTH - 12, gShipX));
    if (gShotCooldown > 0)
    {
        --gShotCooldown;
    }

    if (gMeteorSpawnTimer > 0)
    {
        --gMeteorSpawnTimer;
    }
    if (gMeteorSpawnTimer == 0)
    {
        if (gMeteorCount < MAX_METEORS)
        {
            Meteor* meteor = &gMeteors[gMeteorCount++];
            meteor->x = random_in_range(8, CANVAS_WIDTH - 16);
            meteor->y = -8;
            meteor->speed = min_int(3, 1 + gScore / 100);
        }
        gMeteorSpawnTimer = max_int(7, 17 - gScore / 30);
    }

    for (i = gBulletCount - 1; i >= 0; --i)
    {
        gBullets[i].y -= 5;
        if (gBullets[i].y < 0)
        {
            remove_bullet(i);
        }
    }

    for (i = 0; i < gMeteorCount; ++i)
    {
        gMeteors[i].y += gMeteors[i].speed;
    }

    for (i = gBulletCount - 1; i >= 0; --i)
    {
        int meteorIndex;
        for (meteorIndex = gMeteorCount - 1; meteorIndex >= 0; --meteorIndex)
        {
            const Bullet* bullet = &gBullets[i];
            const Meteor* meteor = &gMeteors[meteorIndex];
            if (rectangles_overlap(bullet->x - 1, bullet->y - 5,
                bullet->x + 1, bullet->y + 2, meteor->x, meteor->y,
                meteor->x + 8, meteor->y + 8))
            {
                remove_meteor(meteorIndex);
                remove_bullet(i);
                gScore += 10;
                audio_play_effect(460);
                break;
            }
        }
    }

    for (i = gMeteorCount - 1; i >= 0; --i)
    {
        const Meteor* meteor = &gMeteors[i];
        if (rectangles_overlap(gShipX - 6, SHIP_Y - 9, gShipX + 6,
            SHIP_Y + 5, meteor->x, meteor->y,
            meteor->x + 8, meteor->y + 8))
        {
            gGameOver = TRUE;
            audio_play_effect(220);
            break;
        }
        if (meteor->y >= CANVAS_HEIGHT)
        {
            remove_meteor(i);
        }
    }
}

static void fill_rect(HDC dc, int left, int top, int right, int bottom,
    COLORREF color)
{
    RECT rect;
    HBRUSH brush;
    rect.left = left;
    rect.top = top;
    rect.right = right;
    rect.bottom = bottom;
    brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

static void draw_text(HDC dc, const char* text, int x, int y, int size,
    COLORREF color, BOOL bold)
{
    HFONT font = CreateFontA(size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
        FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH,
        "Consolas");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    TextOutA(dc, x, y, text, (int)lstrlenA(text));
    SelectObject(dc, oldFont);
    DeleteObject(font);
}

static void draw_pixel_character(HDC dc)
{
    const COLORREF skin = RGB(255, 202, 132);
    const COLORREF blue = RGB(63, 180, 218);
    fill_rect(dc, 257, 74, 273, 90, RGB(14, 17, 48));
    fill_rect(dc, 260, 68, 270, 78, skin);
    fill_rect(dc, 257, 64, 273, 70, RGB(255, 219, 88));
    fill_rect(dc, 257, 70, 260, 77, RGB(255, 219, 88));
    fill_rect(dc, 270, 70, 273, 77, RGB(255, 219, 88));
    fill_rect(dc, 260, 78, 270, 82, skin);
    fill_rect(dc, 257, 82, 273, 91, blue);
    fill_rect(dc, 260, 91, 264, 96, RGB(255, 219, 88));
    fill_rect(dc, 267, 91, 271, 96, RGB(255, 219, 88));
    fill_rect(dc, 263, 71, 266, 74, RGB(24, 29, 68));
    fill_rect(dc, 269, 71, 272, 74, RGB(24, 29, 68));
}

static void draw_background(HDC dc)
{
    int i;
    fill_rect(dc, 0, 0, CANVAS_WIDTH, CANVAS_HEIGHT, RGB(12, 14, 39));
    for (i = 0; i < 24; ++i)
    {
        const int x = (i * 73 + 19) % CANVAS_WIDTH;
        const int y = (i * 47 + 11) % 158;
        const BOOL bright = ((i + gAnimationFrame / 8) % 5) == 0;
        fill_rect(dc, x, y, x + 1, y + 1,
            bright ? RGB(255, 219, 88) : RGB(79, 91, 155));
    }

    fill_rect(dc, 0, 151, CANVAS_WIDTH, CANVAS_HEIGHT, RGB(21, 25, 59));
    for (i = 0; i < CANVAS_WIDTH; i += 16)
    {
        fill_rect(dc, i, 151, i + 1, CANVAS_HEIGHT, RGB(31, 37, 78));
    }
    for (i = 151; i < CANVAS_HEIGHT; i += 8)
    {
        fill_rect(dc, 0, i, CANVAS_WIDTH, i + 1, RGB(31, 37, 78));
    }
}

static void draw_menu(HDC dc)
{
    int i;
    draw_text(dc, "BYTE QUEST", 72, 26, 22, RGB(255, 219, 88), TRUE);
    draw_text(dc, "UMA AVENTURA EM 8 BITS", 79, 53, 8,
        RGB(136, 211, 226), FALSE);
    draw_pixel_character(dc);

    for (i = 0; i < 3; ++i)
    {
        const RECT* item = &gMenuItems[i];
        const BOOL selected = i == gSelectedItem;
        const COLORREF background = selected
            ? RGB(49, 57, 107) : RGB(25, 30, 70);
        const COLORREF border = selected
            ? RGB(255, 219, 88) : RGB(75, 88, 146);
        RECT borderRect = *item;
        HBRUSH borderBrush;
        fill_rect(dc, item->left, item->top, item->right, item->bottom,
            background);
        borderBrush = CreateSolidBrush(border);
        FrameRect(dc, &borderRect, borderBrush);
        DeleteObject(borderBrush);
    }

    draw_text(dc, "JOGAR", 139, 84, 10, RGB(255, 255, 255), TRUE);
    draw_text(dc, audio_is_enabled() ? "SOM: LIGADO" : "SOM: DESLIGADO",
        115, 108, 8, RGB(255, 255, 255), TRUE);
    draw_text(dc, "SAIR", 143, 132, 10, RGB(255, 255, 255), TRUE);
    if (gAnimationFrame % 20 < 14)
    {
        draw_text(dc, ">", 91, 107 + gSelectedItem * 24, 10,
            RGB(255, 219, 88), TRUE);
    }
    draw_text(dc, "SETAS / ENTER   -   CLIQUE PARA SELECIONAR",
        56, 160, 6, RGB(142, 152, 199), FALSE);
    if (!audio_is_available())
    {
        draw_text(dc, "DISPOSITIVO DE AUDIO INDISPONIVEL", 71, 174, 6,
            RGB(255, 120, 120), FALSE);
    }
}

static void draw_game(HDC dc)
{
    int i;
    fill_rect(dc, 0, 0, CANVAS_WIDTH, CANVAS_HEIGHT, RGB(9, 12, 32));
    for (i = 0; i < 30; ++i)
    {
        const int x = (i * 73 + 19) % CANVAS_WIDTH;
        const int y = (i * 47 + 11 + gAnimationFrame / 2) % CANVAS_HEIGHT;
        fill_rect(dc, x, y, x + 1, y + 1,
            i % 6 == 0 ? RGB(255, 219, 88) : RGB(79, 91, 155));
    }

    fill_rect(dc, 0, 0, CANVAS_WIDTH, 14, RGB(17, 22, 53));
    draw_text(dc, "PONTOS", 8, 4, 7, RGB(136, 211, 226), TRUE);
    {
        char scoreText[16];
        snprintf(scoreText, sizeof(scoreText), "%04d", gScore);
        draw_text(dc, scoreText, 49, 4, 7, RGB(255, 219, 88), TRUE);
    }
    draw_text(dc, "A/D OU SETAS: MOVER   ESPACO: ATIRAR",
        98, 4, 6, RGB(142, 152, 199), FALSE);

    for (i = 0; i < gMeteorCount; ++i)
    {
        const Meteor* meteor = &gMeteors[i];
        const COLORREF rock = RGB(173, 105, 76);
        const COLORREF shade = RGB(111, 69, 74);
        fill_rect(dc, meteor->x + 2, meteor->y, meteor->x + 7,
            meteor->y + 2, rock);
        fill_rect(dc, meteor->x, meteor->y + 2, meteor->x + 8,
            meteor->y + 6, rock);
        fill_rect(dc, meteor->x + 2, meteor->y + 6, meteor->x + 7,
            meteor->y + 8, rock);
        fill_rect(dc, meteor->x + 1, meteor->y + 3, meteor->x + 4,
            meteor->y + 5, shade);
        fill_rect(dc, meteor->x + 5, meteor->y + 2, meteor->x + 7,
            meteor->y + 4, RGB(220, 147, 87));
    }

    for (i = 0; i < gBulletCount; ++i)
    {
        const Bullet* bullet = &gBullets[i];
        fill_rect(dc, bullet->x - 1, bullet->y - 5, bullet->x + 1,
            bullet->y + 2, RGB(255, 219, 88));
        fill_rect(dc, bullet->x, bullet->y - 6, bullet->x + 1,
            bullet->y - 4, RGB(255, 255, 255));
    }

    fill_rect(dc, gShipX - 2, SHIP_Y - 12, gShipX + 2, SHIP_Y + 2,
        RGB(228, 239, 255));
    fill_rect(dc, gShipX - 5, SHIP_Y - 5, gShipX + 5, SHIP_Y,
        RGB(63, 180, 218));
    fill_rect(dc, gShipX - 8, SHIP_Y, gShipX + 8, SHIP_Y + 3,
        RGB(46, 111, 190));
    fill_rect(dc, gShipX - 3, SHIP_Y + 3, gShipX + 3, SHIP_Y + 6,
        gAnimationFrame % 4 < 2 ? RGB(255, 144, 71) : RGB(255, 219, 88));
    fill_rect(dc, gShipX - 1, SHIP_Y - 8, gShipX + 1, SHIP_Y - 4,
        RGB(255, 219, 88));

    if (gGameOver)
    {
        RECT border;
        HBRUSH borderBrush;
        fill_rect(dc, 48, 61, 272, 122, RGB(17, 22, 53));
        border.left = 48;
        border.top = 61;
        border.right = 272;
        border.bottom = 122;
        borderBrush = CreateSolidBrush(RGB(255, 219, 88));
        FrameRect(dc, &border, borderBrush);
        DeleteObject(borderBrush);
        draw_text(dc, "FIM DE JOGO", 103, 72, 12,
            RGB(255, 219, 88), TRUE);
        draw_text(dc, "ENTER: JOGAR DE NOVO", 88, 93, 7,
            RGB(255, 255, 255), TRUE);
        draw_text(dc, "ESC: VOLTAR AO MENU", 91, 106, 7,
            RGB(136, 211, 226), FALSE);
    }
}

static void paint(HWND window)
{
    PAINTSTRUCT paintInfo;
    HDC windowDc = BeginPaint(window, &paintInfo);
    RECT client;
    HDC canvasDc;
    HBITMAP canvasBitmap;
    HGDIOBJ oldBitmap;

    GetClientRect(window, &client);
    canvasDc = CreateCompatibleDC(windowDc);
    canvasBitmap = CreateCompatibleBitmap(windowDc, CANVAS_WIDTH,
        CANVAS_HEIGHT);
    oldBitmap = SelectObject(canvasDc, canvasBitmap);

    if (gCurrentScreen == SCREEN_MENU)
    {
        draw_background(canvasDc);
        draw_menu(canvasDc);
    }
    else
    {
        draw_game(canvasDc);
    }

    SetStretchBltMode(windowDc, COLORONCOLOR);
    StretchBlt(windowDc, 0, 0, client.right, client.bottom, canvasDc,
        0, 0, CANVAS_WIDTH, CANVAS_HEIGHT, SRCCOPY);

    SelectObject(canvasDc, oldBitmap);
    DeleteObject(canvasBitmap);
    DeleteDC(canvasDc);
    EndPaint(window, &paintInfo);
}

static void choose_item(HWND window, int item)
{
    if (item == 0)
    {
        reset_game();
        gCurrentScreen = SCREEN_GAME;
        audio_play_effect(880);
    }
    else if (item == 1)
    {
        audio_set_enabled(!audio_is_enabled());
        audio_play_effect(660);
    }
    else
    {
        audio_play_effect(440);
        DestroyWindow(window);
        return;
    }
    InvalidateRect(window, NULL, FALSE);
}

static LRESULT CALLBACK window_procedure(HWND window, UINT message,
    WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        audio_start();
        SetTimer(window, ANIMATION_TIMER, 50, NULL);
        return 0;
    case WM_TIMER:
        ++gAnimationFrame;
        if (gCurrentScreen == SCREEN_GAME)
        {
            update_game(window);
        }
        InvalidateRect(window, NULL, FALSE);
        return 0;
    case WM_PAINT:
        paint(window);
        return 0;
    case WM_MOUSEMOVE:
        if (gCurrentScreen == SCREEN_MENU)
        {
            RECT client;
            int x;
            int y;
            int i;
            GetClientRect(window, &client);
            x = (int)LOWORD(lParam) * CANVAS_WIDTH /
                max_int(1, (int)client.right);
            y = (int)HIWORD(lParam) * CANVAS_HEIGHT /
                max_int(1, (int)client.bottom);
            for (i = 0; i < 3; ++i)
            {
                const RECT* item = &gMenuItems[i];
                if (x >= item->left && x < item->right &&
                    y >= item->top && y < item->bottom &&
                    gSelectedItem != i)
                {
                    gSelectedItem = i;
                    audio_play_effect(740);
                    InvalidateRect(window, NULL, FALSE);
                }
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (gCurrentScreen == SCREEN_MENU)
        {
            RECT client;
            int x;
            int y;
            int i;
            GetClientRect(window, &client);
            x = (int)LOWORD(lParam) * CANVAS_WIDTH /
                max_int(1, (int)client.right);
            y = (int)HIWORD(lParam) * CANVAS_HEIGHT /
                max_int(1, (int)client.bottom);
            for (i = 0; i < 3; ++i)
            {
                const RECT* item = &gMenuItems[i];
                if (x >= item->left && x < item->right &&
                    y >= item->top && y < item->bottom)
                {
                    gSelectedItem = i;
                    choose_item(window, i);
                    break;
                }
            }
        }
        return 0;
    case WM_KEYDOWN:
        if (gCurrentScreen == SCREEN_MENU)
        {
            if (wParam == VK_UP || wParam == VK_DOWN)
            {
                gSelectedItem = (gSelectedItem +
                    (wParam == VK_DOWN ? 1 : 2)) % 3;
                audio_play_effect(740);
                InvalidateRect(window, NULL, FALSE);
            }
            else if (wParam == VK_RETURN || wParam == VK_SPACE)
            {
                choose_item(window, gSelectedItem);
            }
            else if (wParam == VK_ESCAPE)
            {
                DestroyWindow(window);
            }
        }
        else if (wParam == VK_ESCAPE)
        {
            gCurrentScreen = SCREEN_MENU;
            InvalidateRect(window, NULL, FALSE);
        }
        else if (gGameOver && wParam == VK_RETURN)
        {
            reset_game();
            InvalidateRect(window, NULL, FALSE);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, ANIMATION_TIMER);
        audio_stop();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(window, message, wParam, lParam);
    }
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previousInstance,
    LPSTR commandLine, int showCommand)
{
    const char* className = "ByteQuestMenuWindow";
    WNDCLASSA windowClass;
    RECT windowRect;
    HWND window;
    MSG message;

    (void)previousInstance;
    (void)commandLine;
    ZeroMemory(&windowClass, sizeof(windowClass));
    windowClass.lpfnWndProc = window_procedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    windowClass.lpszClassName = className;

    if (RegisterClassA(&windowClass) == 0)
    {
        return 1;
    }

    windowRect.left = 0;
    windowRect.top = 0;
    windowRect.right = 960;
    windowRect.bottom = 540;
    AdjustWindowRect(&windowRect, WS_OVERLAPPED | WS_CAPTION |
        WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    window = CreateWindowExA(0, className, "BYTE QUEST",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        NULL, NULL, instance, NULL);
    if (window == NULL)
    {
        return 1;
    }

    ShowWindow(window, showCommand);
    UpdateWindow(window);
    while (GetMessageA(&message, NULL, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }

    return (int)message.wParam;
}
