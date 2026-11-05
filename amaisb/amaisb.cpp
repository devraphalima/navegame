#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace
{
    constexpr int CanvasWidth = 320;
    constexpr int CanvasHeight = 180;
    constexpr UINT_PTR AnimationTimer = 1;
    constexpr int SampleRate = 44100;
    constexpr int SamplesPerBuffer = 4096;

    enum class Screen
    {
        Menu,
        Game
    };

    class AudioEngine
    {
    public:
        void start()
        {
            running = true;
            worker = std::thread(&AudioEngine::run, this);
        }

        void stop()
        {
            running = false;
            if (worker.joinable())
            {
                worker.join();
            }
        }

        void setEnabled(bool value)
        {
            enabled = value;
        }

        bool isEnabled() const
        {
            return enabled;
        }

        bool isAvailable() const
        {
            return available;
        }

        void playEffect(int frequency)
        {
            effectFrequency = frequency;
            effectSamplesRemaining = SampleRate / 12;
        }

    private:
        struct Buffer
        {
            WAVEHDR header{};
            std::vector<short> samples =
                std::vector<short>(SamplesPerBuffer);
        };

        std::atomic<bool> running{ false };
        std::atomic<bool> enabled{ true };
        std::atomic<bool> available{ false };
        std::atomic<int> effectFrequency{ 880 };
        std::atomic<int> effectSamplesRemaining{ 0 };
        std::thread worker;

        static uint32_t phaseIncrement(int frequency)
        {
            return static_cast<uint32_t>(
                (static_cast<uint64_t>(frequency) << 32) / SampleRate);
        }

        void fillBuffer(Buffer& buffer, uint64_t& samplePosition)
        {
            static constexpr std::array<int, 16> melody = {
                523, 659, 784, 659, 587, 698, 880, 698,
                523, 659, 784, 988, 880, 784, 659, 587
            };
            static constexpr std::array<int, 8> bass = {
                131, 165, 196, 165, 147, 175, 220, 175
            };
            constexpr int samplesPerBeat = SampleRate / 5;

            for (short& sample : buffer.samples)
            {
                int mixed = 0;
                if (enabled)
                {
                    const int beat = static_cast<int>(
                        (samplePosition / samplesPerBeat) % melody.size());
                    const int note = melody[beat];
                    const int bassNote = bass[beat % bass.size()];
                    const uint32_t melodyPhase =
                        static_cast<uint32_t>(samplePosition) * phaseIncrement(note);
                    const uint32_t bassPhase =
                        static_cast<uint32_t>(samplePosition) * phaseIncrement(bassNote);

                    mixed += (melodyPhase & 0x80000000u) ? 3000 : -3000;
                    mixed += (bassPhase & 0x80000000u) ? 1400 : -1400;

                    int remaining = effectSamplesRemaining.load();
                    if (remaining > 0)
                    {
                        const uint32_t effectPhase =
                            static_cast<uint32_t>(samplePosition) *
                            phaseIncrement(effectFrequency.load());
                        mixed += (effectPhase & 0x80000000u) ? 7000 : -7000;
                        effectSamplesRemaining.fetch_sub(1);
                    }
                }

                sample = static_cast<short>(
                    (std::max)(-32768, (std::min)(32767, mixed)));
                ++samplePosition;
            }
        }

        void run()
        {
            WAVEFORMATEX format{};
            format.wFormatTag = WAVE_FORMAT_PCM;
            format.nChannels = 1;
            format.nSamplesPerSec = SampleRate;
            format.wBitsPerSample = 16;
            format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
            format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

            HANDLE eventHandle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            if (eventHandle == nullptr)
            {
                OutputDebugStringA("Nao foi possivel criar o evento de audio.\n");
                return;
            }

            HWAVEOUT output = nullptr;
            MMRESULT result = waveOutOpen(
                &output, WAVE_MAPPER, &format,
                reinterpret_cast<DWORD_PTR>(eventHandle), 0, CALLBACK_EVENT);
            if (result != MMSYSERR_NOERROR)
            {
                OutputDebugStringA("Nao foi possivel iniciar o audio.\n");
                CloseHandle(eventHandle);
                return;
            }
            available = true;

            std::array<Buffer, 2> buffers;
            bool prepared = true;
            for (Buffer& buffer : buffers)
            {
                buffer.header.lpData =
                    reinterpret_cast<LPSTR>(buffer.samples.data());
                buffer.header.dwBufferLength =
                    static_cast<DWORD>(buffer.samples.size() * sizeof(short));
                result = waveOutPrepareHeader(output, &buffer.header,
                    sizeof(buffer.header));
                if (result != MMSYSERR_NOERROR)
                {
                    prepared = false;
                    break;
                }
            }

            uint64_t samplePosition = 0;
            if (prepared)
            {
                for (Buffer& buffer : buffers)
                {
                    fillBuffer(buffer, samplePosition);
                    result = waveOutWrite(output, &buffer.header,
                        sizeof(buffer.header));
                    if (result != MMSYSERR_NOERROR)
                    {
                        prepared = false;
                        break;
                    }
                }
            }

            while (prepared && running)
            {
                WaitForSingleObject(eventHandle, 100);
                for (Buffer& buffer : buffers)
                {
                    if ((buffer.header.dwFlags & WHDR_DONE) != 0 && running)
                    {
                        fillBuffer(buffer, samplePosition);
                        result = waveOutWrite(output, &buffer.header,
                            sizeof(buffer.header));
                        if (result != MMSYSERR_NOERROR)
                        {
                            prepared = false;
                            break;
                        }
                    }
                }
            }

            if (!prepared)
            {
                available = false;
                OutputDebugStringA("O dispositivo de audio parou de responder.\n");
            }
            waveOutReset(output);
            for (Buffer& buffer : buffers)
            {
                if ((buffer.header.dwFlags & WHDR_PREPARED) != 0)
                {
                    waveOutUnprepareHeader(output, &buffer.header,
                        sizeof(buffer.header));
                }
            }
            waveOutClose(output);
            CloseHandle(eventHandle);
        }
    };

    AudioEngine audio;
    Screen currentScreen = Screen::Menu;
    int selectedItem = 0;
    int animationFrame = 0;

    struct Bullet
    {
        int x;
        int y;
    };

    struct Meteor
    {
        int x;
        int y;
        int speed;
    };

    int shipX = CanvasWidth / 2;
    constexpr int ShipY = 151;
    int score = 0;
    int shotCooldown = 0;
    int meteorSpawnTimer = 0;
    bool gameOver = false;
    uint32_t randomState = 0xA341316Cu;
    std::vector<Bullet> bullets;
    std::vector<Meteor> meteors;

    constexpr RECT menuItems[] = {
        { 105, 78, 215, 97 },
        { 105, 102, 215, 121 },
        { 105, 126, 215, 145 }
    };

    int randomInRange(int minimum, int maximum)
    {
        randomState ^= randomState << 13;
        randomState ^= randomState >> 17;
        randomState ^= randomState << 5;
        return minimum + static_cast<int>(
            randomState % static_cast<uint32_t>(maximum - minimum + 1));
    }

    bool overlaps(int leftA, int topA, int rightA, int bottomA,
        int leftB, int topB, int rightB, int bottomB)
    {
        return leftA < rightB && rightA > leftB &&
            topA < bottomB && bottomA > topB;
    }

    void resetGame()
    {
        shipX = CanvasWidth / 2;
        score = 0;
        shotCooldown = 0;
        meteorSpawnTimer = 10;
        gameOver = false;
        bullets.clear();
        meteors.clear();
    }

    void updateGame(HWND window)
    {
        if (gameOver)
        {
            return;
        }

        if (GetForegroundWindow() == window)
        {
            if ((GetAsyncKeyState(VK_LEFT) & 0x8000) != 0 ||
                (GetAsyncKeyState('A') & 0x8000) != 0)
            {
                shipX -= 3;
            }
            if ((GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0 ||
                (GetAsyncKeyState('D') & 0x8000) != 0)
            {
                shipX += 3;
            }
            if ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0 &&
                shotCooldown == 0)
            {
                bullets.push_back({ shipX, ShipY - 10 });
                shotCooldown = 5;
                audio.playEffect(1320);
            }
        }

        shipX = (std::max)(12, (std::min)(CanvasWidth - 12, shipX));
        if (shotCooldown > 0)
        {
            --shotCooldown;
        }

        if (meteorSpawnTimer > 0)
        {
            --meteorSpawnTimer;
        }
        if (meteorSpawnTimer == 0)
        {
            meteors.push_back({ randomInRange(8, CanvasWidth - 16), -8,
                (std::min)(3, 1 + score / 100) });
            meteorSpawnTimer = (std::max)(7, 17 - score / 30);
        }

        for (int i = static_cast<int>(bullets.size()) - 1; i >= 0; --i)
        {
            bullets[i].y -= 5;
            if (bullets[i].y < 0)
            {
                bullets.erase(bullets.begin() + i);
            }
        }

        for (Meteor& meteor : meteors)
        {
            meteor.y += meteor.speed;
        }

        for (int bulletIndex = static_cast<int>(bullets.size()) - 1;
            bulletIndex >= 0; --bulletIndex)
        {
            for (int meteorIndex = static_cast<int>(meteors.size()) - 1;
                meteorIndex >= 0; --meteorIndex)
            {
                const Bullet& bullet = bullets[bulletIndex];
                const Meteor& meteor = meteors[meteorIndex];
                if (overlaps(bullet.x - 1, bullet.y - 5, bullet.x + 1,
                    bullet.y + 2, meteor.x, meteor.y, meteor.x + 8,
                    meteor.y + 8))
                {
                    meteors.erase(meteors.begin() + meteorIndex);
                    bullets.erase(bullets.begin() + bulletIndex);
                    score += 10;
                    audio.playEffect(460);
                    break;
                }
            }
        }

        for (int i = static_cast<int>(meteors.size()) - 1; i >= 0; --i)
        {
            const Meteor& meteor = meteors[i];
            if (overlaps(shipX - 6, ShipY - 9, shipX + 6, ShipY + 5,
                meteor.x, meteor.y, meteor.x + 8, meteor.y + 8))
            {
                gameOver = true;
                audio.playEffect(220);
                break;
            }
            if (meteor.y >= CanvasHeight)
            {
                meteors.erase(meteors.begin() + i);
            }
        }
    }

    void fillRect(HDC dc, int left, int top, int right, int bottom,
        COLORREF color)
    {
        RECT rect{ left, top, right, bottom };
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
    }

    void drawText(HDC dc, const char* text, int x, int y, int size,
        COLORREF color, bool bold = false)
    {
        HFONT font = CreateFontA(size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
            FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH,
            "Consolas");
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        TextOutA(dc, x, y, text, static_cast<int>(lstrlenA(text)));
        SelectObject(dc, oldFont);
        DeleteObject(font);
    }

    void drawPixelCharacter(HDC dc)
    {
        constexpr COLORREF skin = RGB(255, 202, 132);
        constexpr COLORREF blue = RGB(63, 180, 218);
        fillRect(dc, 257, 74, 273, 90, RGB(14, 17, 48));
        fillRect(dc, 260, 68, 270, 78, skin);
        fillRect(dc, 257, 64, 273, 70, RGB(255, 219, 88));
        fillRect(dc, 257, 70, 260, 77, RGB(255, 219, 88));
        fillRect(dc, 270, 70, 273, 77, RGB(255, 219, 88));
        fillRect(dc, 260, 78, 270, 82, skin);
        fillRect(dc, 257, 82, 273, 91, blue);
        fillRect(dc, 260, 91, 264, 96, RGB(255, 219, 88));
        fillRect(dc, 267, 91, 271, 96, RGB(255, 219, 88));
        fillRect(dc, 263, 71, 266, 74, RGB(24, 29, 68));
        fillRect(dc, 269, 71, 272, 74, RGB(24, 29, 68));
    }

    void drawBackground(HDC dc)
    {
        fillRect(dc, 0, 0, CanvasWidth, CanvasHeight, RGB(12, 14, 39));
        for (int i = 0; i < 24; ++i)
        {
            const int x = (i * 73 + 19) % CanvasWidth;
            const int y = (i * 47 + 11) % 158;
            const bool bright = ((i + animationFrame / 8) % 5) == 0;
            fillRect(dc, x, y, x + 1, y + 1,
                bright ? RGB(255, 219, 88) : RGB(79, 91, 155));
        }

        fillRect(dc, 0, 151, CanvasWidth, CanvasHeight, RGB(21, 25, 59));
        for (int x = 0; x < CanvasWidth; x += 16)
        {
            fillRect(dc, x, 151, x + 1, CanvasHeight, RGB(31, 37, 78));
        }
        for (int y = 151; y < CanvasHeight; y += 8)
        {
            fillRect(dc, 0, y, CanvasWidth, y + 1, RGB(31, 37, 78));
        }
    }

    void drawMenu(HDC dc)
    {
        drawText(dc, "BYTE QUEST", 72, 26, 22, RGB(255, 219, 88), true);
        drawText(dc, "UMA AVENTURA EM 8 BITS", 79, 53, 8,
            RGB(136, 211, 226));
        drawPixelCharacter(dc);

        for (int i = 0; i < 3; ++i)
        {
            const RECT& item = menuItems[i];
            const bool selected = i == selectedItem;
            const COLORREF background = selected
                ? RGB(49, 57, 107) : RGB(25, 30, 70);
            const COLORREF border = selected
                ? RGB(255, 219, 88) : RGB(75, 88, 146);
            fillRect(dc, item.left, item.top, item.right, item.bottom,
                background);
            RECT borderRect{ item.left, item.top, item.right, item.bottom };
            HBRUSH borderBrush = CreateSolidBrush(border);
            FrameRect(dc, &borderRect, borderBrush);
            DeleteObject(borderBrush);
        }

        drawText(dc, "JOGAR", 139, 84, 10, RGB(255, 255, 255), true);
        drawText(dc, audio.isEnabled() ? "SOM: LIGADO" : "SOM: DESLIGADO",
            115, 108, 8, RGB(255, 255, 255), true);
        drawText(dc, "SAIR", 143, 132, 10, RGB(255, 255, 255), true);
        if (animationFrame % 20 < 14)
        {
            drawText(dc, ">", 91, 107 + selectedItem * 24, 10,
                RGB(255, 219, 88), true);
        }
        drawText(dc, "SETAS / ENTER   -   CLIQUE PARA SELECIONAR",
            56, 160, 6, RGB(142, 152, 199));
        if (!audio.isAvailable())
        {
            drawText(dc, "DISPOSITIVO DE AUDIO INDISPONIVEL", 71, 174, 6,
                RGB(255, 120, 120));
        }
    }

    void drawGame(HDC dc)
    {
        fillRect(dc, 0, 0, CanvasWidth, CanvasHeight, RGB(9, 12, 32));
        for (int i = 0; i < 30; ++i)
        {
            const int x = (i * 73 + 19) % CanvasWidth;
            const int y = (i * 47 + 11 + animationFrame / 2) % CanvasHeight;
            fillRect(dc, x, y, x + 1, y + 1,
                i % 6 == 0 ? RGB(255, 219, 88) : RGB(79, 91, 155));
        }

        fillRect(dc, 0, 0, CanvasWidth, 14, RGB(17, 22, 53));
        drawText(dc, "PONTOS", 8, 4, 7, RGB(136, 211, 226), true);
        char scoreText[16]{};
        wsprintfA(scoreText, "%04d", score);
        drawText(dc, scoreText, 49, 4, 7, RGB(255, 219, 88), true);
        drawText(dc, "A/D OU SETAS: MOVER   ESPACO: ATIRAR",
            98, 4, 6, RGB(142, 152, 199));

        for (const Meteor& meteor : meteors)
        {
            const COLORREF rock = RGB(173, 105, 76);
            const COLORREF shade = RGB(111, 69, 74);
            fillRect(dc, meteor.x + 2, meteor.y, meteor.x + 7, meteor.y + 2,
                rock);
            fillRect(dc, meteor.x, meteor.y + 2, meteor.x + 8, meteor.y + 6,
                rock);
            fillRect(dc, meteor.x + 2, meteor.y + 6, meteor.x + 7,
                meteor.y + 8, rock);
            fillRect(dc, meteor.x + 1, meteor.y + 3, meteor.x + 4,
                meteor.y + 5, shade);
            fillRect(dc, meteor.x + 5, meteor.y + 2, meteor.x + 7,
                meteor.y + 4, RGB(220, 147, 87));
        }

        for (const Bullet& bullet : bullets)
        {
            fillRect(dc, bullet.x - 1, bullet.y - 5, bullet.x + 1,
                bullet.y + 2, RGB(255, 219, 88));
            fillRect(dc, bullet.x, bullet.y - 6, bullet.x + 1, bullet.y - 4,
                RGB(255, 255, 255));
        }

        fillRect(dc, shipX - 2, ShipY - 12, shipX + 2, ShipY + 2,
            RGB(228, 239, 255));
        fillRect(dc, shipX - 5, ShipY - 5, shipX + 5, ShipY,
            RGB(63, 180, 218));
        fillRect(dc, shipX - 8, ShipY, shipX + 8, ShipY + 3,
            RGB(46, 111, 190));
        fillRect(dc, shipX - 3, ShipY + 3, shipX + 3, ShipY + 6,
            animationFrame % 4 < 2 ? RGB(255, 144, 71) : RGB(255, 219, 88));
        fillRect(dc, shipX - 1, ShipY - 8, shipX + 1, ShipY - 4,
            RGB(255, 219, 88));

        if (gameOver)
        {
            fillRect(dc, 48, 61, 272, 122, RGB(17, 22, 53));
            RECT border{ 48, 61, 272, 122 };
            HBRUSH borderBrush = CreateSolidBrush(RGB(255, 219, 88));
            FrameRect(dc, &border, borderBrush);
            DeleteObject(borderBrush);
            drawText(dc, "FIM DE JOGO", 103, 72, 12,
                RGB(255, 219, 88), true);
            drawText(dc, "ENTER: JOGAR DE NOVO", 88, 93, 7,
                RGB(255, 255, 255), true);
            drawText(dc, "ESC: VOLTAR AO MENU", 91, 106, 7,
                RGB(136, 211, 226));
        }
    }

    void paint(HWND window)
    {
        PAINTSTRUCT paintInfo{};
        HDC windowDc = BeginPaint(window, &paintInfo);
        RECT client{};
        GetClientRect(window, &client);

        HDC canvasDc = CreateCompatibleDC(windowDc);
        HBITMAP canvasBitmap = CreateCompatibleBitmap(
            windowDc, CanvasWidth, CanvasHeight);
        HGDIOBJ oldBitmap = SelectObject(canvasDc, canvasBitmap);

        drawBackground(canvasDc);
        if (currentScreen == Screen::Menu)
        {
            drawBackground(canvasDc);
            drawMenu(canvasDc);
        }
        else
        {
            drawGame(canvasDc);
        }

        SetStretchBltMode(windowDc, COLORONCOLOR);
        StretchBlt(windowDc, 0, 0, client.right, client.bottom, canvasDc,
            0, 0, CanvasWidth, CanvasHeight, SRCCOPY);

        SelectObject(canvasDc, oldBitmap);
        DeleteObject(canvasBitmap);
        DeleteDC(canvasDc);
        EndPaint(window, &paintInfo);
    }

    void chooseItem(HWND window, int item)
    {
        if (item == 0)
        {
            resetGame();
            currentScreen = Screen::Game;
            audio.playEffect(880);
        }
        else if (item == 1)
        {
            const bool enabled = !audio.isEnabled();
            audio.setEnabled(enabled);
            audio.playEffect(660);
        }
        else
        {
            audio.playEffect(440);
            DestroyWindow(window);
            return;
        }
        InvalidateRect(window, nullptr, FALSE);
    }

    LRESULT CALLBACK windowProcedure(HWND window, UINT message,
        WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            audio.start();
            SetTimer(window, AnimationTimer, 50, nullptr);
            return 0;
        case WM_TIMER:
            ++animationFrame;
            if (currentScreen == Screen::Game)
            {
                updateGame(window);
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            paint(window);
            return 0;
        case WM_MOUSEMOVE:
            if (currentScreen == Screen::Menu)
            {
                RECT client{};
                GetClientRect(window, &client);
                const int x = LOWORD(lParam) * CanvasWidth /
                    (std::max)(1L, client.right);
                const int y = HIWORD(lParam) * CanvasHeight /
                    (std::max)(1L, client.bottom);
                for (int i = 0; i < 3; ++i)
                {
                    if (x >= menuItems[i].left && x < menuItems[i].right &&
                        y >= menuItems[i].top && y < menuItems[i].bottom &&
                        selectedItem != i)
                    {
                        selectedItem = i;
                        audio.playEffect(740);
                        InvalidateRect(window, nullptr, FALSE);
                    }
                }
            }
            return 0;
        case WM_LBUTTONUP:
            if (currentScreen == Screen::Menu)
            {
                RECT client{};
                GetClientRect(window, &client);
                const int x = LOWORD(lParam) * CanvasWidth /
                    (std::max)(1L, client.right);
                const int y = HIWORD(lParam) * CanvasHeight /
                    (std::max)(1L, client.bottom);
                for (int i = 0; i < 3; ++i)
                {
                    if (x >= menuItems[i].left && x < menuItems[i].right &&
                        y >= menuItems[i].top && y < menuItems[i].bottom)
                    {
                        selectedItem = i;
                        chooseItem(window, i);
                        break;
                    }
                }
            }
            return 0;
        case WM_KEYDOWN:
            if (currentScreen == Screen::Menu)
            {
                if (wParam == VK_UP || wParam == VK_DOWN)
                {
                    selectedItem = (selectedItem +
                        (wParam == VK_DOWN ? 1 : 2)) % 3;
                    audio.playEffect(740);
                    InvalidateRect(window, nullptr, FALSE);
                }
                else if (wParam == VK_RETURN || wParam == VK_SPACE)
                {
                    chooseItem(window, selectedItem);
                }
                else if (wParam == VK_ESCAPE)
                {
                    DestroyWindow(window);
                }
            }
            else if (wParam == VK_ESCAPE)
            {
                currentScreen = Screen::Menu;
                InvalidateRect(window, nullptr, FALSE);
            }
            else if (gameOver && wParam == VK_RETURN)
            {
                resetGame();
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_DESTROY:
            KillTimer(window, AnimationTimer);
            audio.stop();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcA(window, message, wParam, lParam);
        }
    }
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand)
{
    const char* className = "ByteQuestMenuWindow";
    WNDCLASSA windowClass{};
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground =
        reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = className;

    if (!RegisterClassA(&windowClass))
    {
        return 1;
    }

    RECT windowRect{ 0, 0, 960, 540 };
    AdjustWindowRect(&windowRect, WS_OVERLAPPED | WS_CAPTION |
        WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND window = CreateWindowExA(0, className, "BYTE QUEST",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        nullptr, nullptr, instance, nullptr);
    if (window == nullptr)
    {
        return 1;
    }

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageA(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }

    return static_cast<int>(message.wParam);
}
