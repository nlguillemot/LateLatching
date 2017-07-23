#include <Windows.h>
#include <Windowsx.h>

#include "opengl.h"
#include "wglext.h"

#include "imgui_impl.h"
#include "imgui.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cassert>
#include <string>
#include <thread>
#include <future>

#pragma comment(lib, "opengl32.lib")

enum LatchMode
{
    LATCHMODE_LATE,
    LATCHMODE_UNIFORM,
    LATCHMODE_SETCURSOR,
    LATCHMODE_COUNT
};

int g_CurrLatchMode;

#define RENDER_WIDTH 640
#define RENDER_HEIGHT 480

struct InputBufferItem
{
    GLuint x, y;
};

#define INPUT_BUFFER_ITEM_SIZE_IN_DWORDS (sizeof(InputBufferItem) / 4)
#define INPUT_BUFFER_ITEM_X_OFFSET (offsetof(InputBufferItem, x) / 4)
#define INPUT_BUFFER_ITEM_Y_OFFSET (offsetof(InputBufferItem, y) / 4)

#define INPUT_BUFFER_SIZE 1024
static_assert((INPUT_BUFFER_SIZE & (INPUT_BUFFER_SIZE - 1)) == 0, "");

#define INPUT_BUFFER_SIZE_IN_BYTES (INPUT_BUFFER_SIZE * sizeof(InputBufferItem))

#define CURSOR_SIZE 32

// GLSL bindings
#define INPUT_BUFFER_SSBO_BINDING           0
#define INPUT_COUNTER_BUFFER_SSBO_BINDING   1
#define LATCHED_COUNTER_BUFFER_SSBO_BINDING 2
//
#define CURSOR_TEXTURE_TEXTURE_BINDING 0
//
#define PROJECTION_MATRIX_UNIFORM_LOCATION 0
#define INPUT_UNIFORM_LOCATION             1

void GLAPIENTRY DebugCallbackGL(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message, const void *userParam)
{
    fprintf(stderr, "DebugCallbackGL: %s\n", message);
}

volatile InputBufferItem* g_MappedInputBufferItems;
volatile uint32_t* g_MappedInputCounter;
uint32_t g_InputCounter;

static HCURSOR hCustomCursor = LoadCursorFromFile(TEXT("Ragnarok.ani"));

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CLOSE:
        ExitProcess(0);
    case WM_SETCURSOR:
        if (g_CurrLatchMode == LATCHMODE_SETCURSOR)
        {
            assert(hCustomCursor != NULL);
            SetCursor(hCustomCursor);
        }
        else
        {
            SetCursor(LoadCursor(NULL, IDC_ARROW));
        }
        break;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            ExitProcess(0);
        break;
    case WM_MOUSEMOVE:
        if (g_CurrLatchMode == LATCHMODE_LATE && g_MappedInputBufferItems && g_MappedInputCounter)
        {
            g_InputCounter = (g_InputCounter + 1) & (INPUT_BUFFER_SIZE - 1);
            g_MappedInputBufferItems[g_InputCounter].x = GET_X_LPARAM(lParam);
            g_MappedInputBufferItems[g_InputCounter].y = RENDER_HEIGHT - 1 - GET_Y_LPARAM(lParam);
            *g_MappedInputCounter = g_InputCounter;
        }
        break;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void WindowMain(std::promise<HWND> hWndPromise)
{
    // Register window class
    WNDCLASS wc = {};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = TEXT("WindowClass");
    bool ok = RegisterClass(&wc) != NULL;
    assert(ok);

    // Determine size of window based on window style
    DWORD dwStyle = WS_POPUP;
    RECT wr = { 0, 0, RENDER_WIDTH, RENDER_HEIGHT };
    ok = AdjustWindowRect(&wr, dwStyle, FALSE) != FALSE;
    assert(ok);

    TCHAR* title = TEXT("LateLatching");

    // Create window that will be used to create a GL context
    HWND hWnd = CreateWindow(
        TEXT("WindowClass"), title, dwStyle,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        0, 0, GetModuleHandle(NULL), 0);
    assert(hWnd != NULL);

    ShowWindow(hWnd, SW_SHOWDEFAULT);

    hWndPromise.set_value(hWnd);

    for (;;)
    {
        DWORD nCount = 0;
        const HANDLE* pHandles = NULL;
        DWORD result = MsgWaitForMultipleObjectsEx(nCount, pHandles, INFINITE, QS_ALLINPUT, 0);
        assert(result >= WAIT_OBJECT_0 && result <= WAIT_OBJECT_0 + nCount);

        // WAIT_OBJECT_0 + nCount means it was a window message
        if (result == WAIT_OBJECT_0 + nCount)
        {
            MSG msg;
            BOOL bMsgRet;
            while ((bMsgRet = PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) != 0)
            {
                assert(bMsgRet != -1);
                if (bMsgRet == -1)
                {
                    continue;
                }

                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
}

int main()
{
    std::promise<HWND> hWndPromise;
    std::future<HWND> hWndFuture = hWndPromise.get_future();
    std::thread windowThread = std::thread([&] {
        WindowMain(move(hWndPromise)); 
    });
    
    // wait for the window to be done initializing in the window thread
    HWND hWnd = hWndFuture.get();

    HDC hDC = GetDC(hWnd);
    assert(hDC != NULL);

    // set pixelformat for window that supports OpenGL
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int chosenPixelFormat = ChoosePixelFormat(hDC, &pfd);
    bool ok = SetPixelFormat(hDC, chosenPixelFormat, &pfd) != FALSE;
    assert(ok);

    // Create dummy GL context that will be used to create the real context
    HGLRC dummy_hGLRC = wglCreateContext(hDC);
    assert(dummy_hGLRC != NULL);

    // Use the dummy context to get function to create a better context
    ok = wglMakeCurrent(hDC, dummy_hGLRC) != FALSE;
    assert(ok);

    PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");

    int contextFlagsGL = 0;
#ifdef _DEBUG
    contextFlagsGL |= WGL_CONTEXT_DEBUG_BIT_ARB;
#endif

    int contextAttribsGL[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
        WGL_CONTEXT_MINOR_VERSION_ARB, 4,
        WGL_CONTEXT_FLAGS_ARB, contextFlagsGL,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };

    // Create better GL context
    HGLRC hGLRC = wglCreateContextAttribsARB(hDC, NULL, contextAttribsGL);
    assert(hGLRC != NULL);

    // Switch to the new context and ditch the old one
    ok = wglMakeCurrent(hDC, hGLRC) != FALSE;
    assert(ok);
    ok = wglDeleteContext(dummy_hGLRC) != FALSE;
    assert(ok);

    OpenGL_Init();

    // Enable OpenGL debugging
#ifdef _DEBUG
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glEnable(GL_DEBUG_OUTPUT);
    PFNGLDEBUGMESSAGECALLBACKPROC glDebugMessageCallback = (PFNGLDEBUGMESSAGECALLBACKPROC)wglGetProcAddress("glDebugMessageCallback");
    glDebugMessageCallback(DebugCallbackGL, 0);
#endif

    ImGui_Impl_Init(hWnd);

    const std::string preamble =
        "#define INPUT_BUFFER_ITEM_SIZE_IN_DWORDS " + std::to_string(sizeof(InputBufferItem) / 4) + "\n" +
        "#define INPUT_BUFFER_ITEM_X_OFFSET " + std::to_string(offsetof(InputBufferItem, x) / 4) + "\n" +
        "#define INPUT_BUFFER_ITEM_Y_OFFSET " + std::to_string(offsetof(InputBufferItem, y) / 4) + "\n" +
        "#define INPUT_BUFFER_SIZE " + std::to_string(INPUT_BUFFER_SIZE) + "\n" +
        "#define CURSOR_SIZE " + std::to_string(CURSOR_SIZE) + "\n" +
        "#define INPUT_BUFFER_SSBO_BINDING " + std::to_string(INPUT_BUFFER_SSBO_BINDING) + "\n" +
        "#define INPUT_COUNTER_BUFFER_SSBO_BINDING " + std::to_string(INPUT_COUNTER_BUFFER_SSBO_BINDING) + "\n" +
        "#define LATCHED_COUNTER_BUFFER_SSBO_BINDING " + std::to_string(LATCHED_COUNTER_BUFFER_SSBO_BINDING) + "\n" +
        "#define CURSOR_TEXTURE_TEXTURE_BINDING " + std::to_string(CURSOR_TEXTURE_TEXTURE_BINDING) + "\n" +
        "#define PROJECTION_MATRIX_UNIFORM_LOCATION " + std::to_string(PROJECTION_MATRIX_UNIFORM_LOCATION) + "\n" +
        "#define INPUT_UNIFORM_LOCATION " + std::to_string(INPUT_UNIFORM_LOCATION) + "\n";
        
    const char* const preamble_cstr = preamble.c_str();

    const char* latched_vs_srcs[] = {
        "#version 440 core\n",
        preamble_cstr,
R"GLSL(
layout(std430, binding = INPUT_BUFFER_SSBO_BINDING)
restrict volatile buffer InputBuffer { uint Input[]; };

layout(std430, binding = INPUT_COUNTER_BUFFER_SSBO_BINDING)
restrict volatile buffer InputCounterBuffer { uint InputCounter; };

layout(std430, binding = LATCHED_COUNTER_BUFFER_SSBO_BINDING)
restrict volatile buffer LatchedCounterBuffer { uint LatchedCounter; };

layout(location = PROJECTION_MATRIX_UNIFORM_LOCATION)
uniform mat4 ProjectionMatrix;

out vec2 TexCoord;

void main()
{
    uint counter = InputCounter;
    uint latched = atomicCompSwap(LatchedCounter, INPUT_BUFFER_SIZE, counter);
    if (latched == INPUT_BUFFER_SIZE) {
        latched = counter;
    }

    latched = latched & (INPUT_BUFFER_SIZE - 1);

    uvec2 latchedInput = uvec2(
        Input[latched * INPUT_BUFFER_ITEM_SIZE_IN_DWORDS + INPUT_BUFFER_ITEM_X_OFFSET],
        Input[latched * INPUT_BUFFER_ITEM_SIZE_IN_DWORDS + INPUT_BUFFER_ITEM_Y_OFFSET]);

    TexCoord = vec2(gl_VertexID % 2, gl_VertexID / 2);

    gl_Position = ProjectionMatrix * vec4(vec2(latchedInput) + TexCoord * CURSOR_SIZE - vec2(0, CURSOR_SIZE), 0, 1);
}
)GLSL"
    };

    const char* uniform_vs_srcs[] = {
        "#version 440 core\n",
        preamble_cstr,
R"GLSL(
layout(location = INPUT_UNIFORM_LOCATION)
uniform uvec2 Input;

layout(location = PROJECTION_MATRIX_UNIFORM_LOCATION)
uniform mat4 ProjectionMatrix;

out vec2 TexCoord;

void main()
{
    TexCoord = vec2(gl_VertexID % 2, gl_VertexID / 2);

    gl_Position = ProjectionMatrix * vec4(vec2(Input) + TexCoord * CURSOR_SIZE - vec2(0, CURSOR_SIZE), 0, 1);
}
)GLSL"
    };

    const char* fs_srcs[] = {
        "#version 440 core\n",
        preamble_cstr,
R"GLSL(
layout(binding = CURSOR_TEXTURE_TEXTURE_BINDING)
uniform sampler2D CursorTexture;

in vec2 TexCoord;

out vec4 FragColor;

void main()
{
    FragColor = texture(CursorTexture, TexCoord);
    if (FragColor.a == 0.0)
    {
        discard;
    }
}
)GLSL"
    };

    auto CompileShader = [&](GLenum type, GLsizei count, const GLchar *const* strings)
    {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, count, strings, NULL);
        glCompileShader(shader);

        GLint logLength;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        if (logLength > 0)
        {
            GLchar* infolog = (GLchar*)malloc(logLength);
            glGetShaderInfoLog(shader, logLength, NULL, infolog);
            fprintf(stderr, "%s", infolog);
            free(infolog);
        }
        return shader;
    };

    auto LinkProgram = [&](GLuint vs, GLuint fs)
    {
        GLuint program = glCreateProgram();
        if (vs) glAttachShader(program, vs);
        if (fs) glAttachShader(program, fs);
        glLinkProgram(program);

        GLint logLength;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
        if (logLength > 0)
        {
            GLchar* infolog = (GLchar*)malloc(logLength);
            glGetProgramInfoLog(program, logLength, NULL, infolog);
            fprintf(stderr, "%s", infolog);
            free(infolog);
        }
        return program;
    };

    GLuint latched_vs = CompileShader(GL_VERTEX_SHADER, _countof(latched_vs_srcs), latched_vs_srcs);
    GLuint uniform_vs = CompileShader(GL_VERTEX_SHADER, _countof(uniform_vs_srcs), uniform_vs_srcs);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, _countof(fs_srcs), fs_srcs);
    GLuint latched_sp = LinkProgram(latched_vs, fs);
    GLuint uniform_sp = LinkProgram(uniform_vs, fs);

    // Initialize the cursor texture
    GLuint cursorTexture;
    glGenTextures(1, &cursorTexture);
    glBindTexture(GL_TEXTURE_2D, cursorTexture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, CURSOR_SIZE, CURSOR_SIZE);
    glBindTexture(GL_TEXTURE_2D, 0);

    GLuint cursorUploadBuffer;
    glGenBuffers(1, &cursorUploadBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, cursorUploadBuffer);
    glBufferStorage(GL_ARRAY_BUFFER, CURSOR_SIZE * CURSOR_SIZE * 4, NULL, GL_CLIENT_STORAGE_BIT | GL_MAP_WRITE_BIT);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Create DC to decode the cursor
    HDC hdcMem = CreateCompatibleDC(hDC);
    // Create the bitmap to use as a canvas.
    HBITMAP hbmCanvas = CreateCompatibleBitmap(hDC, CURSOR_SIZE, CURSOR_SIZE);

    // Select the bitmap into the device context.
    HGDIOBJ hbmOld = SelectObject(hdcMem, hbmCanvas);

    uint64_t cursorTimeline = 0;

    GLuint nullVAO;
    glGenVertexArrays(1, &nullVAO);
    glBindVertexArray(nullVAO);
    glBindVertexArray(0);

    GLuint inputBuffer;
    glGenBuffers(1, &inputBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, inputBuffer);
    glBufferStorage(GL_ARRAY_BUFFER, INPUT_BUFFER_SIZE_IN_BYTES, NULL, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
    InputBufferItem* pInputBuffer = (InputBufferItem*)glMapBufferRange(GL_ARRAY_BUFFER, 0, INPUT_BUFFER_SIZE_IN_BYTES, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    GLuint inputCounterBuffer;
    glGenBuffers(1, &inputCounterBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, inputCounterBuffer);
    glBufferStorage(GL_ARRAY_BUFFER, sizeof(GLuint), NULL, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
    GLuint* pInputCounter = (GLuint*)glMapBufferRange(GL_ARRAY_BUFFER, 0, sizeof(GLuint), GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    GLuint latchedCounterBuffer;
    glGenBuffers(1, &latchedCounterBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, latchedCounterBuffer);
    glBufferStorage(GL_ARRAY_BUFFER, sizeof(GLuint), NULL, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    pInputBuffer[0] = InputBufferItem{ 200, 200 };
    *pInputCounter = 0;

    g_MappedInputBufferItems = pInputBuffer;
    g_MappedInputCounter = pInputCounter;

    bool finishAtEndOfFrame = true;

    PFNWGLGETEXTENSIONSSTRINGARBPROC wglGetExtensionsStringARB = (PFNWGLGETEXTENSIONSSTRINGARBPROC)wglGetProcAddress("wglGetExtensionsStringARB");

    PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
    bool adaptiveVSyncSupported = strstr(wglGetExtensionsStringARB(hDC), "WGL_EXT_swap_control_tear") != NULL;
    bool adaptiveVSync = false;
    int swapInterval = 0;
    bool swapIntervalOK = true;

    ULONGLONG then = GetTickCount64();

    for (;;)
    {
        ULONGLONG now = GetTickCount64();
        ULONGLONG dt = now - then;
        cursorTimeline += dt;

        ImGui_Impl_NewFrame(hWnd);

        if (ImGui::Begin("GUI", 0, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("GL_VENDOR: %s\n", glGetString(GL_VENDOR));
            ImGui::Text("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
            ImGui::Text("GL_VERSION: %s\n", glGetString(GL_VERSION));

            const char* latchModeNames[LATCHMODE_COUNT] = {};
            latchModeNames[LATCHMODE_LATE] = "Late-Latching";
            latchModeNames[LATCHMODE_UNIFORM] = "Pass-by-Uniform";
            latchModeNames[LATCHMODE_SETCURSOR] = "Win32 SetCursor";

            for (const char* name : latchModeNames)
            {
                assert(name);
            }

            ImGui::ListBox("Mode", &g_CurrLatchMode, latchModeNames, LATCHMODE_COUNT);

            if (adaptiveVSyncSupported)
            {
                if (ImGui::Checkbox("Adaptive VSync", &adaptiveVSync))
                {
                    if (adaptiveVSync)
                    {
                        swapIntervalOK = wglSwapIntervalEXT(-1);
                    }
                }
            }
            else
            {
                ImGui::Text("Adaptive VSync not supported (missing WGL_EXT_swap_control_tear)");
            }

            if (adaptiveVSync)
            {
                ImGui::Text("Disable Adaptive VSync to set the Swap Interval");
            }
            else
            {
                if (ImGui::InputInt("Swap Interval", &swapInterval))
                {
                    if (swapInterval < 0)
                        swapInterval = 0;

                    swapIntervalOK = wglSwapIntervalEXT(swapInterval);
                }
            }
            
            if (!swapIntervalOK)
            {
                if (adaptiveVSync)
                {
                    ImGui::Text("Setting adaptive VSync failed");
                }
                else
                {
                    ImGui::Text("Setting swap interval failed");

                }
            }

            ImGui::Checkbox("glFinish at end of frame", &finishAtEndOfFrame);
        }
        ImGui::End();

        glClear(GL_COLOR_BUFFER_BIT);

        ImGui::Render();

        if (g_CurrLatchMode == LATCHMODE_LATE || g_CurrLatchMode == LATCHMODE_UNIFORM)
        {
            unsigned char cursorMem[CURSOR_SIZE * CURSOR_SIZE * 4];

            UINT iconFrameRate = 8; // close enough
            UINT istepIfAniCur = UINT(cursorTimeline / (16.666 * iconFrameRate));

            // Draw the cursor's image into the canvas.
            ok = DrawIconEx(hdcMem, 0, 0, hCustomCursor, CURSOR_SIZE, CURSOR_SIZE, istepIfAniCur, NULL, DI_IMAGE) != FALSE;
            if (!ok) {
                // hack because there's no apparent windows API to know how many frames there are
                cursorTimeline -= UINT(istepIfAniCur * 16.666 * iconFrameRate);
                istepIfAniCur = UINT(cursorTimeline / (16.666 * iconFrameRate));
                ok = DrawIconEx(hdcMem, 0, 0, hCustomCursor, CURSOR_SIZE, CURSOR_SIZE, istepIfAniCur, NULL, DI_IMAGE) != FALSE;
                assert(ok);
            }

            // set the color based on the image
            for (int y = 0; y < CURSOR_SIZE; y++)
            {
                for (int x = 0; x < CURSOR_SIZE; x++)
                {
                    COLORREF clr = GetPixel(hdcMem, x, CURSOR_SIZE - 1 - y);
                    cursorMem[(y * CURSOR_SIZE + x) * 4 + 0] = GetRValue(clr);
                    cursorMem[(y * CURSOR_SIZE + x) * 4 + 1] = GetGValue(clr);
                    cursorMem[(y * CURSOR_SIZE + x) * 4 + 2] = GetBValue(clr);
                    cursorMem[(y * CURSOR_SIZE + x) * 4 + 3] = (LOBYTE((clr) >> 24));
                }
            }

            // Draw the cursor's mask into the canvas.
            ok = DrawIconEx(hdcMem, 0, 0, hCustomCursor, CURSOR_SIZE, CURSOR_SIZE, istepIfAniCur, NULL, DI_MASK) != FALSE;
            assert(ok);

            // set the alpha based on the mask
            for (int y = 0; y < CURSOR_SIZE; y++)
            {
                for (int x = 0; x < CURSOR_SIZE; x++)
                {
                    COLORREF clr = GetPixel(hdcMem, x, CURSOR_SIZE - 1 - y);
                    cursorMem[(y * CURSOR_SIZE + x) * 4 + 3] = clr ? 0 : 255;
                }
            }

            // Copy the cursor into the upload buffer
            glBindBuffer(GL_ARRAY_BUFFER, cursorUploadBuffer);
            unsigned char* pCursorPBO = (unsigned char*)glMapBufferRange(GL_ARRAY_BUFFER, 0, CURSOR_SIZE * CURSOR_SIZE * 4, GL_MAP_WRITE_BIT);
            memcpy(pCursorPBO, cursorMem, CURSOR_SIZE * CURSOR_SIZE * 4);
            glUnmapBuffer(GL_ARRAY_BUFFER);
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            // upload cursor for this frame
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, cursorUploadBuffer);
            glBindTexture(GL_TEXTURE_2D, cursorTexture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, CURSOR_SIZE, CURSOR_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

            if (g_CurrLatchMode == LATCHMODE_LATE)
            {
                const GLuint kResetLatch = INPUT_BUFFER_SIZE;
                glBindBuffer(GL_ARRAY_BUFFER, latchedCounterBuffer);
                glClearBufferData(GL_ARRAY_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &kResetLatch);
                glBindBuffer(GL_ARRAY_BUFFER, 0);

                glUseProgram(latched_sp);

                glBindBuffersBase(GL_SHADER_STORAGE_BUFFER, INPUT_BUFFER_SSBO_BINDING, 1, &inputBuffer);
                glBindBuffersBase(GL_SHADER_STORAGE_BUFFER, INPUT_COUNTER_BUFFER_SSBO_BINDING, 1, &inputCounterBuffer);
                glBindBuffersBase(GL_SHADER_STORAGE_BUFFER, LATCHED_COUNTER_BUFFER_SSBO_BINDING, 1, &latchedCounterBuffer);
            }
            else if (g_CurrLatchMode == LATCHMODE_UNIFORM)
            {
                glUseProgram(uniform_sp);

                POINT cursorPos;
                GetCursorPos(&cursorPos);
                ScreenToClient(hWnd, &cursorPos);

                glUniform2ui(INPUT_UNIFORM_LOCATION, cursorPos.x, RENDER_HEIGHT - 1 - cursorPos.y);
            }

            glBindTextures(CURSOR_TEXTURE_TEXTURE_BINDING, 1, &cursorTexture);

            GLfloat ortho[] = {
                2.0f / RENDER_WIDTH, 0.0f, 0.0f, 0.0f,
                0.0f, 2.0f / RENDER_HEIGHT, 0.0f, 0.0f,
                0.0f, 0.0f, -1.0f, 0.0f,
                -1.0f, -1.0f, 0.0f, 1.0f
            };

            glUniformMatrix4fv(PROJECTION_MATRIX_UNIFORM_LOCATION, 1, GL_FALSE, ortho);

            glBindVertexArray(nullVAO);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBindVertexArray(0);

            glBindBuffersBase(GL_SHADER_STORAGE_BUFFER, 0, 16, NULL);
            glBindTextures(0, 16, NULL);

            glUseProgram(0);
        }

        ok = SwapBuffers(hDC) != FALSE;
        assert(ok);

        if (finishAtEndOfFrame)
        {
            glFinish();
        }

        then = now;
    }
}
