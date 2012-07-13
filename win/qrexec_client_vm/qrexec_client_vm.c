#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <tchar.h>
#include <strsafe.h>
#include "qrexec.h"
#include "log.h"


ULONG CreatePipedProcessAsCurrentUser(
		PTCHAR pszCommand,
		BOOLEAN bRunInteractively,
		HANDLE hPipeStdin,
		HANDLE hPipeStdout,
		HANDLE hPipeStderr,
		HANDLE *phProcess)
{
	PROCESS_INFORMATION	pi;
	STARTUPINFO	si;
	ULONG	uResult;
	BOOLEAN	bInheritHandles;


	if (!pszCommand || !phProcess)
		return ERROR_INVALID_PARAMETER;

	*phProcess = INVALID_HANDLE_VALUE;

	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);

	bInheritHandles = FALSE;

	if (INVALID_HANDLE_VALUE != hPipeStdin &&
		INVALID_HANDLE_VALUE != hPipeStdout &&
		INVALID_HANDLE_VALUE != hPipeStderr) {

		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdInput = hPipeStdin;
		si.hStdOutput = hPipeStdout;
		si.hStdError = hPipeStderr;

		bInheritHandles = TRUE;
	}

	if (!CreateProcess(
			NULL,
			pszCommand,
			NULL,
			NULL,
			bInheritHandles, // inherit handles if IO is piped
			0,
			NULL,
			NULL,
			&si,
			&pi)) {

		uResult = GetLastError();
#ifdef UNICODE
		lprintf_err(uResult, "CreatePipedProcessAsCurrentUser(): CreateProcess(\"%S\")", pszCommand);
#else
		lprintf_err(uResult, "CreatePipedProcessAsCurrentUser(): CreateProcess(\"%s\")", pszCommand);
#endif
		return uResult;
	}

	lprintf("CreatePipedProcessAsCurrentUser(): pid %d\n", pi.dwProcessId);

	*phProcess = pi.hProcess;
	CloseHandle(pi.hThread);

	return ERROR_SUCCESS;
}


#ifdef UNICODE
ULONG UTF16ToUTF8(PWCHAR pwszUtf16, PUCHAR *ppszUtf8)
{
	HRESULT	hResult;
	ULONG	uResult;
	int	cbUTF8;
	size_t	cchUTF16;
	PUCHAR	pszUtf8;
	DWORD	dwConversionFlags;


	hResult = StringCchLengthW(pwszUtf16, STRSAFE_MAX_CCH, &cchUTF16);
	if (FAILED(hResult)) {
		lprintf_err(hResult, "UTF16ToUTF8(): StringCchLengthW()");
		return hResult;
	}

	// WC_ERR_INVALID_CHARS is defined for Vista and later only
#if (WINVER >= 0x0600)
	dwConversionFlags = WC_ERR_INVALID_CHARS;
#else
	dwConversionFlags = 0;
#endif

	cbUTF8 = WideCharToMultiByte(CP_UTF8, dwConversionFlags, pwszUtf16, cchUTF16 + 1, NULL, 0, NULL, NULL);
	if (!cbUTF8) {
		uResult = GetLastError();
		lprintf_err(uResult, "UTF16ToUTF8(): WideCharToMultiByte()");
		return uResult;
	}

	pszUtf8 = malloc(cbUTF8);
	if (!pszUtf8)
		return ERROR_NOT_ENOUGH_MEMORY;

	uResult = WideCharToMultiByte(CP_UTF8, dwConversionFlags, pwszUtf16, cchUTF16 + 1, pszUtf8, cbUTF8, NULL, NULL);
	if (!uResult) {
		uResult = GetLastError();
		lprintf_err(uResult, "UTF16ToUTF8(): WideCharToMultiByte()");
		return uResult;
	}

	*ppszUtf8 = pszUtf8;

	return ERROR_SUCCESS;
}
#endif

int __cdecl _tmain(ULONG argc, PTCHAR argv[])
{
	HANDLE	hPipe;
	BOOL	fSuccess = FALSE;
	DWORD	cbRead, cbWritten, dwMode;
	LPTSTR	lpszPipename = TEXT("\\\\.\\pipe\\qrexec_trigger");
	struct	trigger_connect_params params;
	ULONG	uResult;
	PUCHAR	pszParameter;
	HRESULT	hResult;
	IO_HANDLES_ARRAY	IoHandles;
	HANDLE	hProcess;


	if (argc < 4) {
		_tprintf(TEXT("usage: %s target_vmname program_ident local_program [local program arguments]\n"),
			argv[0]);
		exit(1);
	}

	// Prepare the parameter structure containing the first two arguments.
	memset(&params, 0, sizeof(params));


#ifdef UNICODE
	pszParameter = NULL;
	uResult = UTF16ToUTF8(argv[2], &pszParameter);
	if (ERROR_SUCCESS != uResult) {
		lprintf_err(uResult, "UTF16ToUTF8(): WideCharToMultiByte()");
		return uResult;
	}
#else
	pszParameter = argv[2];
#endif

	hResult = StringCchCopyA(params.exec_index, sizeof(params.exec_index), pszParameter);
	if (FAILED(hResult)) {
		lprintf_err(hResult, "StringCchCopyA()");
		return hResult;
	}

#ifdef UNICODE
	free(pszParameter);
	pszParameter = NULL;

	uResult = UTF16ToUTF8(argv[1], &pszParameter);
	if (ERROR_SUCCESS != uResult) {
		lprintf_err(uResult, "UTF16ToUTF8(): WideCharToMultiByte()");
		return uResult;
	}
#else
	pszParameter = argv[1];
#endif

	hResult = StringCchCopyA(params.target_vmname, sizeof(params.target_vmname), pszParameter);
	if (FAILED(hResult)) {
		lprintf_err(hResult, "StringCchCopyA()");
		return hResult;
	}

#ifdef UNICODE
	free(pszParameter);
#endif
	pszParameter = NULL;

	lprintf("Connecting to the pipe server\n");

	// Try to open a named pipe; wait for it, if necessary.

	while (TRUE) {
		hPipe = CreateFile(
				lpszPipename,
				GENERIC_READ | GENERIC_WRITE,
				0,
				NULL,
				OPEN_EXISTING,
				0,
				NULL);

		// Break if the pipe handle is valid.

		if (hPipe != INVALID_HANDLE_VALUE)
			break;

		// Exit if an error other than ERROR_PIPE_BUSY occurs.
		uResult = GetLastError();
		if (ERROR_PIPE_BUSY != uResult) {
			lprintf_err(uResult, "qrexec_agent pipe not found, CreateFile()");
			return uResult;
		}

		// All pipe instances are busy, so wait for 10 seconds.

		if (!WaitNamedPipe(lpszPipename, 10000)) {
			uResult = GetLastError();
			lprintf_err(uResult, "qrexec_agent pipe is busy, WaitNamedPipe()");
			return uResult;
		}
	}

	// The pipe connected; change to message-read mode.

	dwMode = PIPE_READMODE_MESSAGE;
	if (!SetNamedPipeHandleState(
			hPipe, // pipe handle
			&dwMode, // new pipe mode
			NULL, // don't set maximum bytes
			NULL)) { // don't set maximum time
		uResult = GetLastError();
		lprintf_err(uResult, "SetNamedPipeHandleState()");
		CloseHandle(hPipe);
		return uResult;
	}


	// Send the params to the pipe server.
	lprintf("Sending the parameters to the server\n");

	if (!WriteFile(hPipe, &params, sizeof(params), &cbWritten, NULL)) {
		uResult = GetLastError();
		lprintf_err(uResult, "WriteFile()");
		CloseHandle(hPipe);
		return uResult;
	}

	lprintf("Receiving the IO handles\n");

	// Read the handle array from the pipe.
	fSuccess = ReadFile(
			hPipe,
			&IoHandles,
			sizeof(IoHandles),
			&cbRead,
			NULL);

	if (!fSuccess || cbRead != sizeof(IoHandles)) {
		// If the message is too large to fit in a buffer, treat it as an error as well:
		// this shouldn't happen if the pipe server operates correctly.
		uResult = GetLastError();
		lprintf_err(uResult, "ReadFile()");
		CloseHandle(hPipe);
		return uResult;
	}

#ifdef UNICODE
	lprintf("Starting the local program \"%S\"\n", argv[3]);
#else
	lprintf("Starting the local program \"%s\"\n", argv[3]);
#endif

	uResult = CreatePipedProcessAsCurrentUser(
			argv[3],	// local program
			TRUE,
			IoHandles.hPipeStdin,
			IoHandles.hPipeStdout,
			IoHandles.hPipeStderr,
			&hProcess);

	CloseHandle(IoHandles.hPipeStdin);
	CloseHandle(IoHandles.hPipeStdout);
	CloseHandle(IoHandles.hPipeStderr);

	if (ERROR_SUCCESS != uResult) {
		lprintf_err(uResult, "CreatePipedProcessAsCurrentUser()");
		CloseHandle(hPipe);
		return uResult;
	}

	lprintf("Sending the process handle of the local program to the server\n");

	if (!WriteFile(
			hPipe,
			&hProcess,
			sizeof(HANDLE),
			&cbWritten,
			NULL)) {

		uResult = GetLastError();
		lprintf_err(uResult, "WriteFile()");
		CloseHandle(hProcess);
		CloseHandle(hPipe);
		return uResult;
	}

	lprintf("Waiting for the server to read the handle, duplicate it and close the pipe\n");

	// Issue a blocking dummy read that will finish when the server disconnects the pipe
	// after the process handle duplication is complete. We have to wait here because for
	// the handle to be duplicated successfully this process must be present.
	ReadFile(hPipe, &params, 1, &cbRead, NULL);

	lprintf("Closing the pipe\n");

	CloseHandle(hPipe);
	CloseHandle(hProcess);

	return 0;
}