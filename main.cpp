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

#define CURSOR_SIZE 64

// GLSL bindings
#define INPUT_BUFFER_SSBO_BINDING           0
#define INPUT_COUNTER_BUFFER_SSBO_BINDING   1
#define LATCHED_COUNTER_BUFFER_SSBO_BINDING 2
//
#define CURSOR_TEXTURE_TEXTURE_BINDING 0
//
#define PROJECTION_MATRIX_UNIFORM_LOCATION 0

void GLAPIENTRY DebugCallbackGL(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message, const void *userParam)
{
    fprintf(stderr, "DebugCallbackGL: %s\n", message);
}

volatile InputBufferItem* g_MappedInputBufferItems;
volatile uint32_t* g_MappedInputCounter;
uint32_t g_InputCounter;

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CLOSE:
        ExitProcess(0);
    case WM_SETCURSOR:
        if (g_CurrLatchMode == LATCHMODE_SETCURSOR)
        {
            static HCURSOR hCursor = LoadCursorFromFile(TEXT("Ragnarok.ani"));
            assert(hCursor != NULL);
            SetCursor(hCursor);
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
        if (g_MappedInputBufferItems && g_MappedInputCounter)
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

    printf("GL_VENDOR: %s\n", glGetString(GL_VENDOR));
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
    printf("GL_VERSION: %s\n", glGetString(GL_VERSION));
    printf("\n");

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
        "#define PROJECTION_MATRIX_UNIFORM_LOCATION " + std::to_string(PROJECTION_MATRIX_UNIFORM_LOCATION) + "\n";
        
    const char* const preamble_cstr = preamble.c_str();

    // Compute shader that just converts every 8-bit value into a float and stores the result in a buffer.
    const char* vs_srcs[] = {
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

    GLuint vs = CompileShader(GL_VERTEX_SHADER, _countof(vs_srcs), vs_srcs);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, _countof(fs_srcs), fs_srcs);
    GLuint sp = LinkProgram(vs, fs);

    const int kCursorSize = 64;
    unsigned char cursorPixelData[kCursorSize * kCursorSize * 4];
    for (int y = 0; y < kCursorSize; y++)
    {
        for (int x = 0; x < kCursorSize; x++)
        {
            unsigned char c = x <= y ? 127 : 0;
            cursorPixelData[4 * (y * kCursorSize + x) + 0] = c;
            cursorPixelData[4 * (y * kCursorSize + x) + 1] = c;
            cursorPixelData[4 * (y * kCursorSize + x) + 2] = c;
            cursorPixelData[4 * (y * kCursorSize + x) + 3] = c; // premultiplied alpha
        }
    }

    // Initialize the cursor texture
    GLuint cursorTexture;
    glGenTextures(1, &cursorTexture);
    glBindTexture(GL_TEXTURE_2D, cursorTexture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kCursorSize, kCursorSize);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kCursorSize, kCursorSize, GL_RGBA, GL_UNSIGNED_BYTE, cursorPixelData);
    glBindTexture(GL_TEXTURE_2D, 0);

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

    for (;;)
    {
        // Handle all events
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        ImGui_Impl_NewFrame(hWnd);

        if (ImGui::Begin("Options", 0, ImGuiWindowFlags_AlwaysAutoResize))
        {
            const char* latchModeNames[LATCHMODE_COUNT] = {};
            latchModeNames[LATCHMODE_LATE] = "Late-Latching";
            latchModeNames[LATCHMODE_SETCURSOR] = "Win32 SetCursor";

            for (const char* name : latchModeNames)
            {
                assert(name);
            }

            ImGui::ListBox("Mode", &g_CurrLatchMode, latchModeNames, LATCHMODE_COUNT);
        }
        ImGui::End();

        const GLuint kResetLatch = INPUT_BUFFER_SIZE;
        glBindBuffer(GL_ARRAY_BUFFER, latchedCounterBuffer);
        glClearBufferData(GL_ARRAY_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &kResetLatch);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        glClear(GL_COLOR_BUFFER_BIT);

        if (g_CurrLatchMode == LATCHMODE_LATE)
        {
            glUseProgram(sp);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // premultiplied alpha blend function

            glBindBuffersBase(GL_SHADER_STORAGE_BUFFER, INPUT_BUFFER_SSBO_BINDING, 1, &inputBuffer);
            glBindBuffersBase(GL_SHADER_STORAGE_BUFFER, INPUT_COUNTER_BUFFER_SSBO_BINDING, 1, &inputCounterBuffer);
            glBindBuffersBase(GL_SHADER_STORAGE_BUFFER, LATCHED_COUNTER_BUFFER_SSBO_BINDING, 1, &latchedCounterBuffer);

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

            glBindBuffersBase(GL_SHADER_STORAGE_BUFFER, INPUT_BUFFER_SSBO_BINDING, 1, NULL);
            glBindBuffersBase(GL_SHADER_STORAGE_BUFFER, INPUT_COUNTER_BUFFER_SSBO_BINDING, 1, NULL);
            glBindBuffersBase(GL_SHADER_STORAGE_BUFFER, LATCHED_COUNTER_BUFFER_SSBO_BINDING, 1, NULL);

            glBindTextures(CURSOR_TEXTURE_TEXTURE_BINDING, 1, NULL);

            glBlendFunc(GL_ONE, GL_ZERO);
            glDisable(GL_BLEND);
            glUseProgram(0);
        }

        ImGui::Render();

        ok = SwapBuffers(hDC) != FALSE;
        assert(ok);
    }
}
