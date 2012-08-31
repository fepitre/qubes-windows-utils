#include <windows.h>
#include <tchar.h>
#include <Shlwapi.h>
#include <strsafe.h>
#include <stdlib.h>
#include <stdio.h>

#define CLIPBOARD_FORMAT CF_UNICODETEXT

#define MAX_CLIPBOARD_SIZE 65000

ULONG ConvertUTF8ToUTF16(PUCHAR pszUtf8, HANDLE *ppwszUtf16)
{
	HRESULT hResult;
	HANDLE	hGlbUTF16;
	ULONG   uResult;
	size_t  cchUTF8;
	int cchUTF16;
	PWCHAR  pwszUtf16;


	hResult = StringCchLengthA(pszUtf8, STRSAFE_MAX_CCH, &cchUTF8);
	if (FAILED(hResult)) {
		//lprintf_err(hResult, "UTF8ToUTF16(): StringCchLengthA()");
		return hResult;
	}

	cchUTF16 = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pszUtf8, cchUTF8 + 1, NULL, 0);
	if (!cchUTF16) {
		uResult = GetLastError();
		//lprintf_err(uResult, "UTF8ToUTF16(): MultiByteToWideChar()");
		return uResult;
	}

	hGlbUTF16 = GlobalAlloc(GMEM_MOVEABLE, cchUTF16 * sizeof(WCHAR));
	if (!hGlbUTF16) {
		return ERROR_NOT_ENOUGH_MEMORY;
	}

	pwszUtf16 = GlobalLock(hGlbUTF16);
	if (!pwszUtf16) {
		GlobalFree(hGlbUTF16);
		return ERROR_NOT_ENOUGH_MEMORY;
	}

	uResult = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pszUtf8, cchUTF8 + 1, pwszUtf16, cchUTF16);
	if (!uResult) {
		uResult = GetLastError();
		GlobalUnlock(hGlbUTF16);
		GlobalFree(hGlbUTF16);
		//lprintf_err(uResult, "UTF8ToUTF16(): MultiByteToWideChar()");
		return uResult;
	}

	pwszUtf16[cchUTF16 - 1] = L'\0';
	*ppwszUtf16 = hGlbUTF16;
	GlobalUnlock(hGlbUTF16);

	return ERROR_SUCCESS;
}

BOOL setClipboard(HWND hWin, HANDLE hInput)
{
	HANDLE hglb;
	UCHAR lpStr[MAX_CLIPBOARD_SIZE+1];
	size_t cchwStr;
	ULONG  uRead;

	if (!ReadFile(hInput, lpStr, sizeof(lpStr)-1, &uRead, NULL)) {
		fprintf(stderr, "failed to read stdin: %d\n", GetLastError());
		return FALSE;
	}

	lpStr[uRead] = '\0';

	if (FAILED(ConvertUTF8ToUTF16(lpStr, &hglb))) {
		fprintf(stderr, "failed to convert text from UTF-8\n");
		return FALSE;
	}

	if (!OpenClipboard(hWin)) {
		GlobalFree(hglb);
		return FALSE;
	}

	if (!EmptyClipboard()) {
		GlobalFree(hglb);
		CloseClipboard();
		return FALSE;
	}

	if (!SetClipboardData(CLIPBOARD_FORMAT, hglb)) {
		GlobalFree(hglb);
		CloseClipboard();
		return FALSE;
	}

	CloseClipboard();
	return TRUE;
}


HWND createMainWindow(HINSTANCE hInst)
{
	WNDCLASSEX wc;
	ATOM windowClass;

	wc.cbSize = sizeof(wc);
	wc.style = 0;
	wc.lpfnWndProc = (WNDPROC)DefWindowProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = hInst;
	wc.hIcon = NULL;
	wc.hCursor = NULL;
	wc.hbrBackground = (HBRUSH)COLOR_WINDOW;
	wc.lpszMenuName = NULL;
	wc.lpszClassName = TEXT("MainWindowClass");
	wc.hIconSm = NULL;

	windowClass = RegisterClassEx(&wc);
	if (!windowClass) {
		return NULL;
	}

	return CreateWindow(
			wc.lpszClassName, /* class */
			TEXT("Qubes clipboard service"), /* name */
			WS_OVERLAPPEDWINDOW, /* style */
			CW_USEDEFAULT, CW_USEDEFAULT, /* x,y */
			10, 10, /* w, h */
			HWND_MESSAGE, /* parent */
			NULL, /* menu */
			hInst, /* instance */
			NULL);
}

int APIENTRY _tWinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPTSTR lpCommandLine, int nCmdShow)
{
	HANDLE hStdIn;
	HWND hWin;

	hStdIn = GetStdHandle(STD_INPUT_HANDLE);
	if (hStdIn == INVALID_HANDLE_VALUE) {
		// some error handler?
		return 1;
	}

	hWin = createMainWindow(hInst);
	if (!hWin) {
		fprintf(stderr, "create window failed: %d\n", GetLastError());
		return 1;
	}

	if (!setClipboard(hWin, hStdIn)) {
		return 1;
	}

	DestroyWindow(hWin);
	UnregisterClass(TEXT("MainWindowClass"), hInst);

	return 0; 	
}
