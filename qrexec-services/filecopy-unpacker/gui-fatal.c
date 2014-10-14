#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdarg.h>
#include <Strsafe.h>

typedef LONG NTSTATUS;

extern HANDLE g_stderr;

static void produce_message(int icon, const WCHAR *fmt, va_list args)
{
    WCHAR buf[1024];
    WCHAR *pMessage = NULL;
    ULONG cchErrorTextSize;
    ULONG nWritten;

    if (FAILED(StringCbVPrintfW(buf, sizeof(buf), fmt, args)))
    {
        /* FIXME: some fallback method? */
        return;
    }

    cchErrorTextSize = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        GetLastError(),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        pMessage,
        0,
        NULL);

    if (cchErrorTextSize > 0)
    {
        if (FAILED(StringCbCat(buf, sizeof(buf), L": ")))
        {
            LocalFree(pMessage);
            return;
        }

        if (FAILED(StringCbCat(buf, sizeof(buf), pMessage)))
        {
            LocalFree(pMessage);
            return;
        }
    }

    if (FAILED(StringCbCat(buf, sizeof(buf), L"\n")))
    {
        LocalFree(pMessage);
        return;
    }

    if (g_stderr != INVALID_HANDLE_VALUE)
    {
        // message for qrexec log in dom0
        WriteFile(g_stderr, buf, wcslen(buf) * sizeof(WCHAR), &nWritten, NULL);
    }

    MessageBoxW(NULL, buf, L"Qubes file copy error", MB_OK | icon);
    LocalFree(pMessage);
}

void gui_fatal(const WCHAR *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    produce_message(MB_ICONERROR, fmt, args);
    va_end(args);
    exit(1);
}

void gui_nonfatal(const WCHAR *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    produce_message(MB_ICONWARNING, fmt, args);
    va_end(args);
}