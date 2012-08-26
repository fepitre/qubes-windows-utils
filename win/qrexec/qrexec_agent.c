#include "qrexec_agent.h"
#include <Shlwapi.h>

HANDLE	g_hAddExistingClientEvent;

CLIENT_INFO	g_Clients[MAX_CLIENTS];
HANDLE	g_WatchedEvents[MAXIMUM_WAIT_OBJECTS];
HANDLE_INFO	g_HandlesInfo[MAXIMUM_WAIT_OBJECTS];

ULONG64	g_uPipeId = 0;

CRITICAL_SECTION	g_ClientsCriticalSection;
CRITICAL_SECTION	g_VchanCriticalSection;


extern HANDLE	g_hStopServiceEvent;
#ifndef BUILD_AS_SERVICE
HANDLE	g_hCleanupFinishedEvent;
#endif


ULONG CreateAsyncPipe(HANDLE *phReadPipe, HANDLE *phWritePipe, SECURITY_ATTRIBUTES *pSecurityAttributes)
{
	TCHAR	szPipeName[MAX_PATH + 1];
	HANDLE	hReadPipe;
	HANDLE	hWritePipe;
	ULONG	uResult;


	if (!phReadPipe || !phWritePipe)
		return ERROR_INVALID_PARAMETER;

	StringCchPrintf(szPipeName, MAX_PATH, TEXT("\\\\.\\pipe\\qrexec.%08x.%I64x"), GetCurrentProcessId(), g_uPipeId++);

	hReadPipe = CreateNamedPipe(
			szPipeName,
			PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
			PIPE_TYPE_BYTE,
			1,
			512,
			512,
			50,	// the default timeout is 50ms
			pSecurityAttributes);
	if (!hReadPipe) {
		uResult = GetLastError();
		lprintf_err(uResult, "CreateAsyncPipe(): CreateNamedPipe()");
		return uResult;
	}

	hWritePipe = CreateFile(
			szPipeName,
			GENERIC_WRITE,
			0,
			pSecurityAttributes,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
			NULL);
	if (INVALID_HANDLE_VALUE == hWritePipe) {
		uResult = GetLastError();
		CloseHandle(hReadPipe);
		lprintf_err(uResult, "CreateAsyncPipe(): CreateFile()");
		return uResult;
	}

	*phReadPipe = hReadPipe;
	*phWritePipe = hWritePipe;

	return ERROR_SUCCESS;
}


ULONG InitReadPipe(PIPE_DATA *pPipeData, HANDLE *phWritePipe, UCHAR bPipeType)
{
	SECURITY_ATTRIBUTES	sa;
	ULONG	uResult;


	memset(pPipeData, 0, sizeof(PIPE_DATA));
	memset(&sa, 0, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE; 
	sa.lpSecurityDescriptor = NULL; 

	if (!pPipeData || !phWritePipe)
		return ERROR_INVALID_PARAMETER;

	pPipeData->olRead.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (!pPipeData->olRead.hEvent)
		return GetLastError();

	uResult = CreateAsyncPipe(&pPipeData->hReadPipe, phWritePipe, &sa);
	if (ERROR_SUCCESS != uResult) {
		CloseHandle(pPipeData->olRead.hEvent);
		lprintf_err(uResult, "InitReadPipe(): CreateAsyncPipe()");
		return uResult;
	}

	// Ensure the read handle to the pipe is not inherited.
	SetHandleInformation(pPipeData->hReadPipe, HANDLE_FLAG_INHERIT, 0);

	pPipeData->bPipeType = bPipeType;

	return ERROR_SUCCESS;
}


ULONG ReturnData(int client_id, int type, PVOID pData, ULONG uDataSize)
{
	struct server_header s_hdr;


	EnterCriticalSection(&g_VchanCriticalSection);

	s_hdr.type = type;
	s_hdr.client_id = client_id;
	s_hdr.len = uDataSize;
	if (write_all_vchan_ext(&s_hdr, sizeof s_hdr) <= 0) {
		lprintf_err(ERROR_INVALID_FUNCTION, "ReturnData(): write_all_vchan_ext(s_hdr)");
		LeaveCriticalSection(&g_VchanCriticalSection);
		return ERROR_INVALID_FUNCTION;
	}

	if (!uDataSize) {
		LeaveCriticalSection(&g_VchanCriticalSection);
		return ERROR_SUCCESS;
	}

	if (write_all_vchan_ext(pData, uDataSize) <= 0) {
		lprintf_err(ERROR_INVALID_FUNCTION, "ReturnData(): write_all_vchan_ext(data, %d)", uDataSize);
		LeaveCriticalSection(&g_VchanCriticalSection);
		return ERROR_INVALID_FUNCTION;
	}

	LeaveCriticalSection(&g_VchanCriticalSection);
	return ERROR_SUCCESS;
}


ULONG send_exit_code(int client_id, int status)
{
	ULONG	uResult;


	uResult = ReturnData(client_id, MSG_AGENT_TO_SERVER_EXIT_CODE, &status, sizeof(status));
	if (ERROR_SUCCESS != uResult) {
		lprintf_err(uResult, "send_exit_code(): ReturnData()");
		return uResult;
	} else
		lprintf("send_exit_code(): Send exit code %d for client_id %d\n",
			status,
			client_id);

	return ERROR_SUCCESS;
}

PCLIENT_INFO FindClientById(int client_id)
{
	ULONG	uClientNumber;


	for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
		if (client_id == g_Clients[uClientNumber].client_id)
			return &g_Clients[uClientNumber];

	return NULL;
}


ULONG ReturnPipeData(int client_id, PIPE_DATA *pPipeData)
{
	DWORD	dwRead;
	int	message_type;
	PCLIENT_INFO	pClientInfo;
	ULONG	uResult;


	uResult = ERROR_SUCCESS;

	if (!pPipeData)
		return ERROR_INVALID_PARAMETER;

	pClientInfo = FindClientById(client_id);
	if (!pClientInfo)
		return ERROR_FILE_NOT_FOUND;

	if (pClientInfo->bReadingIsDisabled)
		// The client does not want to receive any data from this console.
		return ERROR_INVALID_FUNCTION;

	pPipeData->bReadInProgress = FALSE;
	pPipeData->bDataIsReady = FALSE;


	switch (pPipeData->bPipeType) {
	case PTYPE_STDOUT:
		message_type = MSG_AGENT_TO_SERVER_STDOUT;
		break;
	case PTYPE_STDERR:
		message_type = MSG_AGENT_TO_SERVER_STDERR;
		break;
	default:
		return ERROR_INVALID_FUNCTION;
	}


	dwRead = 0;
	GetOverlappedResult(pPipeData->hReadPipe, &pPipeData->olRead, &dwRead, FALSE);

	uResult = ERROR_SUCCESS;

	if (dwRead) {
		uResult = ReturnData(client_id, message_type, pPipeData->ReadBuffer, dwRead);
		if (ERROR_SUCCESS != uResult)
			lprintf_err(uResult, "ReturnPipeData(): ReturnData()");
	}

	return uResult;
}


ULONG CloseReadPipeHandles(int client_id, PIPE_DATA *pPipeData)
{
	ULONG	uResult;


	if (!pPipeData)
		return ERROR_INVALID_PARAMETER;


	uResult = ERROR_SUCCESS;

	if (pPipeData->olRead.hEvent) {

		if (pPipeData->bDataIsReady)
			ReturnPipeData(client_id, pPipeData);

		// ReturnPipeData() clears both bDataIsReady and bReadInProgress, but they cannot be ever set to a non-FALSE value at the same time.
		// So, if the above ReturnPipeData() has been executed (bDataIsReady was not FALSE), then bReadInProgress was FALSE
		// and this branch wouldn't be executed anyways.
		if (pPipeData->bReadInProgress) {

			// If bReadInProgress is not FALSE then hReadPipe must be a valid handle for which an
			// asynchornous read has been issued.
			if (CancelIo(pPipeData->hReadPipe)) {

				// Must wait for the canceled IO to complete, otherwise a race condition may occur on the
				// OVERLAPPED structure.
				WaitForSingleObject(pPipeData->olRead.hEvent, INFINITE);

				// See if there is something to return.
				ReturnPipeData(client_id, pPipeData);

			} else {
				uResult = GetLastError();
				lprintf_err(uResult, "CloseReadPipeHandles(): CancelIo()");
			}
		}

		CloseHandle(pPipeData->olRead.hEvent);
	}

	if (pPipeData->hReadPipe)
		// Can close the pipe only when there is no pending IO in progress.
		CloseHandle(pPipeData->hReadPipe);

	return uResult;
}


ULONG UTF8ToUTF16(PUCHAR pszUtf8, PWCHAR *ppwszUtf16)
{
	HRESULT	hResult;
	ULONG	uResult;
	size_t	cchUTF8;
	int	cchUTF16;
	PWCHAR	pwszUtf16;


	hResult = StringCchLengthA(pszUtf8, STRSAFE_MAX_CCH, &cchUTF8);
	if (FAILED(hResult)) {
		lprintf_err(hResult, "UTF8ToUTF16(): StringCchLengthA()");
		return hResult;
	}

	cchUTF16 = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pszUtf8, cchUTF8 + 1, NULL, 0);
	if (!cchUTF16) {
		uResult = GetLastError();
		lprintf_err(uResult, "UTF8ToUTF16(): MultiByteToWideChar()");
		return uResult;
	}

	pwszUtf16 = malloc(cchUTF16 * sizeof(WCHAR));
	if (!pwszUtf16)
		return ERROR_NOT_ENOUGH_MEMORY;

	uResult = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pszUtf8, cchUTF8 + 1, pwszUtf16, cchUTF16);
	if (!uResult) {
		uResult = GetLastError();
		lprintf_err(uResult, "UTF8ToUTF16(): MultiByteToWideChar()");
		return uResult;
	}

	pwszUtf16[cchUTF16 - 1] = L'\0';
	*ppwszUtf16 = pwszUtf16;

	return ERROR_SUCCESS;
}


ULONG TextBOMToUTF16(unsigned char *pszBuf, size_t cbBufLen, PWCHAR *ppwszUtf16)
{
	size_t cbSkipChars = 0;
	PWCHAR  pwszUtf16 = NULL;
	ULONG	uResult;
	HRESULT	hResult;


	if (!pszBuf || !cbBufLen || !ppwszUtf16)
		return ERROR_INVALID_PARAMETER;

	*ppwszUtf16 = NULL;

	// see http://en.wikipedia.org/wiki/Byte-order_mark for explaination of the BOM
	// encoding
	if(cbBufLen >= 3 && pszBuf[0] == 0xEF && pszBuf[1] == 0xBB && pszBuf[2] == 0xBF)
	{
		// UTF-8
		cbSkipChars = 3;
	}
	else if(cbBufLen >= 2 && pszBuf[0] == 0xFE && pszBuf[1] == 0xFF)
	{
		// UTF-16BE
		return ERROR_NOT_SUPPORTED;
	}
	else if(cbBufLen >= 2 && pszBuf[0] == 0xFF && pszBuf[1] == 0xFE)
	{
		// UTF-16LE
		cbSkipChars = 2;

		pwszUtf16 = malloc(cbBufLen - cbSkipChars + sizeof(WCHAR));
		if (!pwszUtf16)
			return ERROR_NOT_ENOUGH_MEMORY;

		hResult = StringCbCopyW(pwszUtf16, cbBufLen - cbSkipChars + sizeof(WCHAR), (STRSAFE_LPCWSTR)(pszBuf + cbSkipChars));
		if (FAILED(hResult)) {
			free(pwszUtf16);
			lprintf_err(hResult, "TextBOMToUTF16(): StringCbCopyW()");
			return hResult;
		}

		*ppwszUtf16 = pwszUtf16;
		return ERROR_SUCCESS;
	}
	else if(cbBufLen >= 4 && pszBuf[0] == 0 && pszBuf[1] == 0 && pszBuf[2] == 0xFE && pszBuf[3] == 0xFF)
	{
		// UTF-32BE
		return ERROR_NOT_SUPPORTED;
	}
	else if(cbBufLen >= 4 && pszBuf[0] == 0xFF && pszBuf[1] == 0xFE && pszBuf[2] == 0 && pszBuf[3] == 0)
	{
		// UTF-32LE
		return ERROR_NOT_SUPPORTED;
	}

	// Try UTF-8

	uResult = UTF8ToUTF16(pszBuf + cbSkipChars, ppwszUtf16);
	if (ERROR_SUCCESS != uResult) {
		lprintf_err(uResult, "TextBOMToUTF16(): UTF8ToUTF16()");
		return uResult;
	}

	return ERROR_SUCCESS;
}


ULONG ParseUtf8Command(PUCHAR pszUtf8Command, PWCHAR *ppwszCommand, PWCHAR *ppwszUserName, PWCHAR *ppwszCommandLine, PBOOLEAN pbRunInteractively)
{
	ULONG	uResult;
	PWCHAR	pwszCommand = NULL;
	PWCHAR	pwszCommandLine = NULL;
	PWCHAR	pwSeparator = NULL;
	PWCHAR	pwszUserName = NULL;


	if (!pszUtf8Command || !pbRunInteractively)
		return ERROR_INVALID_PARAMETER;

	*ppwszCommand = NULL;
	*ppwszUserName = NULL;
	*ppwszCommandLine = NULL;
	*pbRunInteractively = TRUE;

	pwszCommand = NULL;
	uResult = UTF8ToUTF16(pszUtf8Command, &pwszCommand);
	if (ERROR_SUCCESS != uResult) {
		lprintf_err(uResult, "ParseUtf8Command(): UTF8ToUTF16()");
		return uResult;
	}

	pwszUserName = pwszCommand;
	pwSeparator = wcschr(pwszCommand, L':');
	if (!pwSeparator) {
		free(pwszCommand);
		lprintf("ParseUtf8Command(): Command line is supposed to be in [nogui:]user:command form\n");
		return ERROR_INVALID_PARAMETER;
	}

	*pwSeparator = L'\0';
	pwSeparator++;

	if (!wcscmp(pwszUserName, L"nogui")) {
		pwszUserName = pwSeparator;
		pwSeparator = wcschr(pwSeparator, L':');
		if (!pwSeparator) {
			free(pwszCommand);
			lprintf("ParseUtf8Command(): Command line is supposed to be in [nogui:]user:command form\n");
			return ERROR_INVALID_PARAMETER;
		}

		*pwSeparator = L'\0';
		pwSeparator++;

		*pbRunInteractively = FALSE;
	}
	
	*ppwszCommand = pwszCommand;
	*ppwszUserName = pwszUserName;
	*ppwszCommandLine = pwSeparator;

	return ERROR_SUCCESS;
}


ULONG CreateClientPipes(CLIENT_INFO *pClientInfo, HANDLE *phPipeStdin, HANDLE *phPipeStdout, HANDLE *phPipeStderr)
{
	ULONG	uResult;
	SECURITY_ATTRIBUTES	sa;
	HANDLE	hPipeStdin = INVALID_HANDLE_VALUE;
	HANDLE	hPipeStdout = INVALID_HANDLE_VALUE;
	HANDLE	hPipeStderr = INVALID_HANDLE_VALUE;


	if (!pClientInfo || !phPipeStdin || !phPipeStdout || !phPipeStderr)
		return ERROR_INVALID_PARAMETER;


	memset(&sa, 0, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE; 
	sa.lpSecurityDescriptor = NULL; 



	uResult = InitReadPipe(&pClientInfo->Stdout, &hPipeStdout, PTYPE_STDOUT);
	if (ERROR_SUCCESS != uResult) {
		lprintf_err(uResult, "CreateClientPipes(): InitReadPipe(STDOUT)");
		return uResult;
	}
	uResult = InitReadPipe(&pClientInfo->Stderr, &hPipeStderr, PTYPE_STDERR);
	if (ERROR_SUCCESS != uResult) {

		CloseHandle(pClientInfo->Stdout.hReadPipe);
		CloseHandle(hPipeStdout);

		lprintf_err(uResult, "CreateClientPipes(): InitReadPipe(STDERR)");
		return uResult;
	}


	if (!CreatePipe(&hPipeStdin, &pClientInfo->hWriteStdinPipe, &sa, 0)) {
		uResult = GetLastError();

		CloseHandle(pClientInfo->Stdout.hReadPipe);
		CloseHandle(pClientInfo->Stderr.hReadPipe);
		CloseHandle(hPipeStdout);
		CloseHandle(hPipeStderr);

		lprintf_err(uResult, "CreateClientPipes(): CreatePipe(STDIN)");
		return uResult;
	}

	pClientInfo->bStdinPipeClosed = FALSE;

	// Ensure the write handle to the pipe for STDIN is not inherited.
	SetHandleInformation(pClientInfo->hWriteStdinPipe, HANDLE_FLAG_INHERIT, 0);

	*phPipeStdin = hPipeStdin;
	*phPipeStdout = hPipeStdout;
	*phPipeStderr = hPipeStderr;

	return ERROR_SUCCESS;
}


// This routine may be called by pipe server threads, hence the critical section around g_Clients array is required.
ULONG ReserveClientNumber(int client_id, PULONG puClientNumber)
{
	ULONG	uClientNumber;


	EnterCriticalSection(&g_ClientsCriticalSection);

	for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
		if (FREE_CLIENT_SPOT_ID == g_Clients[uClientNumber].client_id)
			break;

	if (MAX_CLIENTS == uClientNumber) {
		// There is no space for watching for another process
		LeaveCriticalSection(&g_ClientsCriticalSection);
		lprintf("ReserveClientNumber(): The maximum number of running processes (%d) has been reached\n", MAX_CLIENTS);
		return ERROR_TOO_MANY_CMDS;
	}

	if (FindClientById(client_id)) {
		LeaveCriticalSection(&g_ClientsCriticalSection);
		lprintf("ReserveClientNumber(): A client with the same id (#%d) already exists\n", client_id);
		return ERROR_ALREADY_EXISTS;
	}

	g_Clients[uClientNumber].bClientIsReady = FALSE;
	g_Clients[uClientNumber].client_id = client_id;
	*puClientNumber = uClientNumber;

	LeaveCriticalSection(&g_ClientsCriticalSection);

	return ERROR_SUCCESS;
}


ULONG ReleaseClientNumber(ULONG uClientNumber)
{
	if (uClientNumber >= MAX_CLIENTS)
		return ERROR_INVALID_PARAMETER;

	EnterCriticalSection(&g_ClientsCriticalSection);

	g_Clients[uClientNumber].bClientIsReady = FALSE;
	g_Clients[uClientNumber].client_id = FREE_CLIENT_SPOT_ID;

	LeaveCriticalSection(&g_ClientsCriticalSection);

	return ERROR_SUCCESS;
}


ULONG AddFilledClientInfo(ULONG uClientNumber, PCLIENT_INFO pClientInfo)
{
	if (!pClientInfo || uClientNumber >= MAX_CLIENTS)
		return ERROR_INVALID_PARAMETER;

	EnterCriticalSection(&g_ClientsCriticalSection);

	g_Clients[uClientNumber] = *pClientInfo;
	g_Clients[uClientNumber].bClientIsReady = TRUE;

	LeaveCriticalSection(&g_ClientsCriticalSection);

	return ERROR_SUCCESS;
}


ULONG AddClient(int client_id, PWCHAR pwszUserName, PWCHAR pwszCommandLine, BOOLEAN bRunInteractively)
{
	ULONG	uResult;
	CLIENT_INFO	ClientInfo;
	HANDLE	hPipeStdout = INVALID_HANDLE_VALUE;
	HANDLE	hPipeStderr = INVALID_HANDLE_VALUE;
	HANDLE	hPipeStdin = INVALID_HANDLE_VALUE;
	ULONG	uClientNumber;


	// if pwszUserName is NULL we run the process on behalf of the current user.
	if (!pwszCommandLine)
		return ERROR_INVALID_PARAMETER;


	uResult = ReserveClientNumber(client_id, &uClientNumber);
	if (ERROR_SUCCESS != uResult) {
		lprintf_err(uResult, "AddClient(): ReserveClientNumber()");
		return uResult;
	}


	if (pwszUserName)
		lprintf("AddClient(): Running \"%S\" as user \"%S\"\n", pwszCommandLine, pwszUserName);
	else {
#ifdef BUILD_AS_SERVICE
		lprintf("AddClient(): Running \"%S\" as SYSTEM\n", pwszCommandLine);
#else
		lprintf("AddClient(): Running \"%S\" as current user\n", pwszCommandLine);
#endif
	}

	memset(&ClientInfo, 0, sizeof(ClientInfo));
	ClientInfo.client_id = client_id;


	uResult = CreateClientPipes(&ClientInfo, &hPipeStdin, &hPipeStdout, &hPipeStderr);
	if (ERROR_SUCCESS != uResult) {
		ReleaseClientNumber(uClientNumber);
		lprintf_err(uResult, "AddClient(): CreateClientPipes()");
		return uResult;
	}

#ifdef BUILD_AS_SERVICE
	if (pwszUserName)
		uResult = CreatePipedProcessAsUserW(
				pwszUserName,
				DEFAULT_USER_PASSWORD_UNICODE,
				pwszCommandLine,
				bRunInteractively,
				hPipeStdin,
				hPipeStdout,
				hPipeStderr,
				&ClientInfo.hProcess);
	else
		uResult = CreatePipedProcessAsCurrentUserW(
				pwszCommandLine,
				bRunInteractively,
				hPipeStdin,
				hPipeStdout,
				hPipeStderr,
				&ClientInfo.hProcess);
#else
	uResult = CreatePipedProcessAsCurrentUserW(
			pwszCommandLine,
			bRunInteractively,
			hPipeStdin,
			hPipeStdout,
			hPipeStderr,
			&ClientInfo.hProcess);
#endif

	CloseHandle(hPipeStdout);
	CloseHandle(hPipeStderr);
	CloseHandle(hPipeStdin);

	if (ERROR_SUCCESS != uResult) {
		ReleaseClientNumber(uClientNumber);

		CloseHandle(ClientInfo.hWriteStdinPipe);
		CloseHandle(ClientInfo.Stdout.hReadPipe);
		CloseHandle(ClientInfo.Stderr.hReadPipe);

#ifdef BUILD_AS_SERVICE
		if (pwszUserName)
			lprintf_err(uResult, "AddClient(): CreatePipedProcessAsUserW()");
		else
			lprintf_err(uResult, "AddClient(): CreatePipedProcessAsCurrentUserW()");
#else
		lprintf_err(uResult, "AddClient(): CreatePipedProcessAsCurrentUserW()");
#endif
		return uResult;
	}

	uResult = AddFilledClientInfo(uClientNumber, &ClientInfo);
	if (ERROR_SUCCESS != uResult) {
		ReleaseClientNumber(uClientNumber);

		CloseHandle(ClientInfo.hWriteStdinPipe);
		CloseHandle(ClientInfo.Stdout.hReadPipe);
		CloseHandle(ClientInfo.Stderr.hReadPipe);
		CloseHandle(ClientInfo.hProcess);

		lprintf_err(uResult, "AddClient(): AddFilledClientInfo()");
		return uResult;
	}

	lprintf("AddClient(): New client %d (local id #%d)\n", client_id, uClientNumber);

	return ERROR_SUCCESS;
}


ULONG AddExistingClient(int client_id, PCLIENT_INFO pClientInfo)
{
	ULONG	uClientNumber;
	ULONG	uResult;


	if (!pClientInfo)
		return ERROR_INVALID_PARAMETER;


	uResult = ReserveClientNumber(client_id, &uClientNumber);
	if (ERROR_SUCCESS != uResult) {
		lprintf_err(uResult, "AddExistingClient(): ReserveClientNumber()");
		return uResult;
	}

	pClientInfo->client_id = client_id;

	uResult = AddFilledClientInfo(uClientNumber, pClientInfo);
	if (ERROR_SUCCESS != uResult) {
		ReleaseClientNumber(uClientNumber);
		lprintf_err(uResult, "AddExistingClient(): AddFilledClientInfo()");
		return uResult;
	}

	lprintf("AddExistingClient(): New client %d (local id #%d)\n", client_id, uClientNumber);

	SetEvent(g_hAddExistingClientEvent);

	return ERROR_SUCCESS;
}

VOID RemoveClientNoLocks(PCLIENT_INFO pClientInfo)
{
	if (!pClientInfo || (FREE_CLIENT_SPOT_ID == pClientInfo->client_id))
		return;

	CloseHandle(pClientInfo->hProcess);

	if (!pClientInfo->bStdinPipeClosed)
		CloseHandle(pClientInfo->hWriteStdinPipe);

	CloseReadPipeHandles(pClientInfo->client_id, &pClientInfo->Stdout);
	CloseReadPipeHandles(pClientInfo->client_id, &pClientInfo->Stderr);

	lprintf("RemoveClientNoLocks(): Client %d removed\n", pClientInfo->client_id);

	pClientInfo->client_id = FREE_CLIENT_SPOT_ID;
	pClientInfo->bClientIsReady = FALSE;
}

VOID RemoveClient(PCLIENT_INFO pClientInfo)
{
	EnterCriticalSection(&g_ClientsCriticalSection);

	RemoveClientNoLocks(pClientInfo);

	LeaveCriticalSection(&g_ClientsCriticalSection);
}

VOID RemoveAllClients()
{
	ULONG	uClientNumber;

	EnterCriticalSection(&g_ClientsCriticalSection);

	for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
		if (FREE_CLIENT_SPOT_ID != g_Clients[uClientNumber].client_id)
			RemoveClientNoLocks(&g_Clients[uClientNumber]);

	LeaveCriticalSection(&g_ClientsCriticalSection);
}

// Recognize magic RPC request command ("QUBESRPC") and replace it with real
// command to be executed, after reading RPC service configuration.
// pwszCommandLine will be modified (and possibly reallocated)
// ppwszSourceDomainName will contain source domain (if available) to be set in
// environment; must be freed by caller
ULONG InterceptRPCRequest(PWCHAR pwszCommandLine, PWCHAR *ppwszServiceCommandLine, PWCHAR *ppwszSourceDomainName)
{
	PWCHAR	pwszServiceName = NULL;
	PWCHAR	pwszSourceDomainName = NULL;
	PWCHAR	pwSeparator = NULL;
	UCHAR	szBuffer[sizeof(WCHAR) * (MAX_PATH + 1)];
	WCHAR	wszServiceFilePath[MAX_PATH + 1];
	PWCHAR	pwszRawServiceFilePath = NULL;
	PWCHAR  pwszServiceArgs = NULL;
	HANDLE	hServiceConfigFile;
	ULONG	uResult;
	ULONG	uBytesRead;
	ULONG	uPathLength;
	PWCHAR	pwszServiceCommandLine = NULL;


	if (!pwszCommandLine || !ppwszServiceCommandLine || !ppwszSourceDomainName)
		return ERROR_INVALID_PARAMETER;

	*ppwszServiceCommandLine = *ppwszSourceDomainName = NULL;

	if (wcsncmp(pwszCommandLine, RPC_REQUEST_COMMAND, wcslen(RPC_REQUEST_COMMAND))==0) {
		// RPC_REQUEST_COMMAND contains trailing space, so this must succeed
#pragma prefast(suppress:28193, "RPC_REQUEST_COMMAND contains trailing space, so this must succeed")
		pwSeparator = wcschr(pwszCommandLine, L' ');
		pwSeparator++;
		pwszServiceName = pwSeparator;
		pwSeparator = wcschr(pwszServiceName, L' ');
		if (pwSeparator) {
			*pwSeparator = L'\0';
			pwSeparator++;
			pwszSourceDomainName = _wcsdup(pwSeparator);
			if (!pwszSourceDomainName) {
				lprintf_err(ERROR_NOT_ENOUGH_MEMORY, "InterceptRPCRequest(): _wcsdup()");
				return ERROR_NOT_ENOUGH_MEMORY;
			}
		} else {
			lprintf("InterceptRPCRequest(): No source domain given\n");
			// Most qrexec services do not use source domain at all, so do not
			// abort if missing. This can be the case when RPC triggered
			// manualy using qvm-run (qvm-run -p vmname "QUBESRPC service_name").
		}

		// build RPC service config file path
		memset(wszServiceFilePath, 0, sizeof(wszServiceFilePath));
		if (!GetModuleFileNameW(NULL, wszServiceFilePath, MAX_PATH)) {
			uResult = GetLastError();
			if (pwszSourceDomainName)
				free(pwszSourceDomainName);
			lprintf_err(uResult, "InterceptRPCRequest(): GetModuleFileName()");
			return uResult;
		}
		// cut off file name (qrexec_agent.exe)
		pwSeparator = wcsrchr(wszServiceFilePath, L'\\');
		if (!pwSeparator) {
			if (pwszSourceDomainName)
				free(pwszSourceDomainName);
			lprintf("InterceptRPCRequest(): Cannot find dir containing qrexec_agent.exe\n");
			return ERROR_INVALID_PARAMETER;
		}
		*pwSeparator = L'\0';
		// cut off one dir (bin)
		pwSeparator = wcsrchr(wszServiceFilePath, L'\\');
		if (!pwSeparator) {
			if (pwszSourceDomainName)
				free(pwszSourceDomainName);
			lprintf("InterceptRPCRequest(): Cannot find dir containing bin\\qrexec_agent.exe\n");
			return ERROR_INVALID_PARAMETER;
		}
		// Leave trailing backslash
		pwSeparator++;
		*pwSeparator = L'\0';
		if (wcslen(wszServiceFilePath) + wcslen(L"qubes_rpc\\") + wcslen(pwszServiceName) > MAX_PATH) {
			if (pwszSourceDomainName)
				free(pwszSourceDomainName);
			lprintf("InterceptRPCRequest(): RPC service config file path too long\n");
			return ERROR_NOT_ENOUGH_MEMORY;
		}
		PathAppendW(wszServiceFilePath, L"qubes_rpc");
		PathAppendW(wszServiceFilePath, pwszServiceName);

		hServiceConfigFile = CreateFileW(wszServiceFilePath,               // file to open
				GENERIC_READ,          // open for reading
				FILE_SHARE_READ,       // share for reading
				NULL,                  // default security
				OPEN_EXISTING,         // existing file only
				FILE_ATTRIBUTE_NORMAL, // normal file
				NULL);                 // no attr. template

		if (hServiceConfigFile == INVALID_HANDLE_VALUE)
		{
			uResult = GetLastError();
			if (pwszSourceDomainName)
				free(pwszSourceDomainName);
			lprintf_err(uResult, "InterceptRPCRequest(): Failed to open RPC %S configuration file (%S)", pwszServiceName, wszServiceFilePath);
			return uResult;
		}
		uBytesRead = 0;
		memset(szBuffer, 0, sizeof(szBuffer));
		if (!ReadFile(hServiceConfigFile, szBuffer, sizeof(WCHAR) * MAX_PATH, &uBytesRead, NULL)) {
			uResult = GetLastError();
			if (pwszSourceDomainName)
				free(pwszSourceDomainName);
			lprintf_err(uResult, "InterceptRPCRequest(): Failed to read RPC %S configuration file (%S)", pwszServiceName, wszServiceFilePath);
			CloseHandle(hServiceConfigFile);
			return uResult;
		}
		CloseHandle(hServiceConfigFile);

		uResult = TextBOMToUTF16(szBuffer, uBytesRead, &pwszRawServiceFilePath);
		if (uResult != ERROR_SUCCESS) {
			if (pwszSourceDomainName)
				free(pwszSourceDomainName);
			lprintf_err(uResult, "InterceptRPCRequest(): Failed to parse the encoding in RPC %S configuration file (%S)", pwszServiceName, wszServiceFilePath);
			return uResult;
		}

		// strip white chars (especially end-of-line) from string
		uPathLength = wcslen(pwszRawServiceFilePath);
		while (iswspace(pwszRawServiceFilePath[uPathLength-1])) {
			uPathLength--;
			pwszRawServiceFilePath[uPathLength]=L'\0';
		}

		pwszServiceArgs = PathGetArgsW(pwszRawServiceFilePath);
		PathRemoveArgsW(pwszRawServiceFilePath);
		PathUnquoteSpacesW(pwszRawServiceFilePath);
		if (PathIsRelativeW(pwszRawServiceFilePath)) {
			// relative path are based in qubes_rpc_services
			// reuse separator found when preparing previous file path
			*pwSeparator = L'\0';
			PathAppendW(wszServiceFilePath, L"qubes_rpc_services");
			PathAppendW(wszServiceFilePath, pwszRawServiceFilePath);
		} else {
			StringCchCopyW(wszServiceFilePath, MAX_PATH + 1, pwszRawServiceFilePath);
		}
		PathQuoteSpacesW(wszServiceFilePath);
		if (pwszServiceArgs && pwszServiceArgs[0] != L'\0') {
			StringCchCatW(wszServiceFilePath, MAX_PATH + 1, L" ");
			StringCchCatW(wszServiceFilePath, MAX_PATH + 1, pwszServiceArgs);
		}
		free(pwszRawServiceFilePath);
		pwszServiceCommandLine = malloc((wcslen(wszServiceFilePath) + 1) * sizeof(WCHAR));
		if (pwszServiceCommandLine == NULL) {
			if (pwszSourceDomainName)
				free(pwszSourceDomainName);
			lprintf_err(ERROR_NOT_ENOUGH_MEMORY, "InterceptRPCRequest(): malloc()");
			return ERROR_NOT_ENOUGH_MEMORY;
		}
		lprintf("InterceptRPCRequest(): RPC %S: %S\n", pwszServiceName, wszServiceFilePath);
		StringCchCopyW(pwszServiceCommandLine, wcslen(wszServiceFilePath) + 1, wszServiceFilePath);

		*ppwszServiceCommandLine = pwszServiceCommandLine;
		*ppwszSourceDomainName = pwszSourceDomainName;
	}
	return ERROR_SUCCESS;
}

// This will return error only if vchan fails.
ULONG handle_connect_existing(int client_id, int len)
{
	ULONG	uResult;
	char *buf;
	PCLIENT_INFO	pClientInfo;
	DWORD	dwWritten;


	if (!len)
		return ERROR_SUCCESS;

	buf = malloc(len + 1);
	if (!buf)
		return ERROR_SUCCESS;
	buf[len] = 0;

	if (read_all_vchan_ext(buf, len) <= 0) {
		free(buf);
		lprintf_err(ERROR_INVALID_FUNCTION, "handle_connect_existing(): read_all_vchan_ext()");
		return ERROR_INVALID_FUNCTION;
	}


	lprintf("handle_connect_existing(): client %d, ident %s\n", client_id, buf);

	uResult = ProceedWithExecution(client_id, buf);
	free(buf);

	if (ERROR_SUCCESS != uResult)
		lprintf_err(uResult, "handle_connect_existing(): ProceedWithExecution()");

	return ERROR_SUCCESS;
}

// This will return error only if vchan fails.
ULONG handle_exec(int client_id, int len)
{
	char *buf;
	ULONG	uResult;
	PWCHAR	pwszCommand = NULL;
	PWCHAR	pwszUserName = NULL;
	PWCHAR	pwszCommandLine = NULL;
	PWCHAR	pwszServiceCommandLine = NULL;
	PWCHAR	pwszRemoteDomainName = NULL;
	BOOLEAN	bRunInteractively;


	buf = malloc(len + 1);
	if (!buf)
		return ERROR_SUCCESS;
	buf[len] = 0;


	if (read_all_vchan_ext(buf, len) <= 0) {
		free(buf);
		lprintf_err(ERROR_INVALID_FUNCTION, "handle_exec(): read_all_vchan_ext()");
		return ERROR_INVALID_FUNCTION;
	}

	bRunInteractively = TRUE;

	uResult = ParseUtf8Command(buf, &pwszCommand, &pwszUserName, &pwszCommandLine, &bRunInteractively);
	if (ERROR_SUCCESS != uResult) {
		free(buf);
		send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
		lprintf_err(uResult, "handle_just_exec(): ParseUtf8Command()");
		return ERROR_SUCCESS;
	}

	free(buf);
	buf = NULL;

	uResult = InterceptRPCRequest(pwszCommandLine, &pwszServiceCommandLine, &pwszRemoteDomainName);
	if (ERROR_SUCCESS != uResult) {
		free(pwszCommand);
		send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
		lprintf_err(uResult, "handle_exec(): InterceptRPCRequest()");
		return ERROR_SUCCESS;
	}

	if (pwszServiceCommandLine)
		pwszCommandLine = pwszServiceCommandLine;

	if (pwszRemoteDomainName)
		SetEnvironmentVariableW(L"QREXEC_REMOTE_DOMAIN", pwszRemoteDomainName);

	// Create a process and redirect its console IO to vchan.
	uResult = AddClient(client_id, pwszUserName, pwszCommandLine, bRunInteractively);
	if (ERROR_SUCCESS == uResult)
		lprintf("handle_exec(): Executed %S\n", pwszCommandLine);
	else {
		send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
		lprintf_err(uResult, "handle_exec(): AddClient(\"%S\")", pwszCommandLine);
	}

	if (pwszRemoteDomainName) {
		SetEnvironmentVariableW(L"QREXEC_REMOTE_DOMAIN", NULL);
		free(pwszRemoteDomainName);
	}
	if (pwszServiceCommandLine)
		free(pwszServiceCommandLine);

	free(pwszCommand);
	return ERROR_SUCCESS;
}

// This will return error only if vchan fails.
ULONG handle_just_exec(int client_id, int len)
{
	char *buf;
	ULONG	uResult;
	PWCHAR	pwszCommand = NULL;
	PWCHAR	pwszUserName = NULL;
	PWCHAR	pwszCommandLine = NULL;
	PWCHAR	pwszServiceCommandLine = NULL;
	PWCHAR	pwszRemoteDomainName = NULL;
	HANDLE	hProcess;
	BOOLEAN	bRunInteractively;


	buf = malloc(len + 1);
	if (!buf)
		return ERROR_SUCCESS;
	buf[len] = 0;


	if (read_all_vchan_ext(buf, len) <= 0) {
		free(buf);
		lprintf_err(ERROR_INVALID_FUNCTION, "handle_just_exec(): read_all_vchan_ext()");
		return ERROR_INVALID_FUNCTION;
	}

	bRunInteractively = TRUE;

	uResult = ParseUtf8Command(buf, &pwszCommand, &pwszUserName, &pwszCommandLine, &bRunInteractively);
	if (ERROR_SUCCESS != uResult) {
		free(buf);
		send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
		lprintf_err(uResult, "handle_just_exec(): ParseUtf8Command()");
		return ERROR_SUCCESS;
	}

	free(buf);
	buf = NULL;

	uResult = InterceptRPCRequest(pwszCommandLine, &pwszServiceCommandLine, &pwszRemoteDomainName);
	if (ERROR_SUCCESS != uResult) {
		free(pwszCommand);
		send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
		lprintf_err(uResult, "handle_just_exec(): InterceptRPCRequest()");
		return ERROR_SUCCESS;
	}

	if (pwszServiceCommandLine)
		pwszCommandLine = pwszServiceCommandLine;

	if (pwszRemoteDomainName)
		SetEnvironmentVariableW(L"QREXEC_REMOTE_DOMAIN", pwszRemoteDomainName);


#ifdef BUILD_AS_SERVICE
	// Create a process which IO is not redirected anywhere.
	uResult = CreateNormalProcessAsUserW(
			pwszUserName,
			DEFAULT_USER_PASSWORD_UNICODE,
			pwszCommandLine,
			bRunInteractively,
			&hProcess);
#else
	uResult = CreateNormalProcessAsCurrentUserW(
			pwszCommandLine,
			bRunInteractively,
			&hProcess);
#endif

	if (ERROR_SUCCESS == uResult) {
		CloseHandle(hProcess);
		lprintf("handle_just_exec(): Executed (nowait) %S\n", pwszCommandLine);
	} else {
		send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
#ifdef BUILD_AS_SERVICE
		lprintf_err(uResult, "handle_just_exec(): CreateNormalProcessAsUserW(\"%S\")", pwszCommandLine);
#else
		lprintf_err(uResult, "handle_just_exec(): CreateNormalProcessAsCurrentUserW(\"%S\")", pwszCommandLine);
#endif
	}

	if (pwszRemoteDomainName) {
		SetEnvironmentVariableW(L"QREXEC_REMOTE_DOMAIN", NULL);
		free(pwszRemoteDomainName);
	}
	if (pwszServiceCommandLine)
		free(pwszServiceCommandLine);

	free(pwszCommand);
	return ERROR_SUCCESS;
}

// This will return error only if vchan fails.
ULONG handle_input(int client_id, int len)
{
	char *buf;
	PCLIENT_INFO	pClientInfo;
	DWORD	dwWritten;


	// If pClientInfo is NULL after this it means we couldn't find a specified client.
	// Read and discard any data in the channel in this case.
	pClientInfo = FindClientById(client_id);

	if (!len) {
		if (pClientInfo) {
			CloseHandle(pClientInfo->hWriteStdinPipe);
			pClientInfo->bStdinPipeClosed = TRUE;
		}
		return ERROR_SUCCESS;
	}

	buf = malloc(len + 1);
	if (!buf)
		return ERROR_SUCCESS;
	buf[len] = 0;

	if (read_all_vchan_ext(buf, len) <= 0) {
		free(buf);
		lprintf_err(ERROR_INVALID_FUNCTION, "handle_input(): read_all_vchan_ext()");
		return ERROR_INVALID_FUNCTION;
	}

	if (pClientInfo && !pClientInfo->bStdinPipeClosed) {
		if (!WriteFile(pClientInfo->hWriteStdinPipe, buf, len, &dwWritten, NULL))
			lprintf_err(GetLastError(), "handle_input(): WriteFile()");
	}


	free(buf);
	return ERROR_SUCCESS;
}

void set_blocked_outerr(int client_id, BOOLEAN bBlockOutput)
{
	PCLIENT_INFO	pClientInfo;


	pClientInfo = FindClientById(client_id);
	if (!pClientInfo)
		return;

	pClientInfo->bReadingIsDisabled = bBlockOutput;
}

ULONG handle_server_data()
{
	struct server_header s_hdr;
	ULONG	uResult;


	if (read_all_vchan_ext(&s_hdr, sizeof s_hdr) <= 0) {
		lprintf_err(ERROR_INVALID_FUNCTION, "handle_server_data(): read_all_vchan_ext()");
		return ERROR_INVALID_FUNCTION;
	}

//	lprintf("got %x %x %x\n", s_hdr.type, s_hdr.client_id, s_hdr.len);

	switch (s_hdr.type) {
	case MSG_XON:
		lprintf("MSG_XON\n");
		set_blocked_outerr(s_hdr.client_id, FALSE);
		break;
	case MSG_XOFF:
		lprintf("MSG_XOFF\n");
		set_blocked_outerr(s_hdr.client_id, TRUE);
		break;
	case MSG_SERVER_TO_AGENT_CONNECT_EXISTING:
		lprintf("MSG_SERVER_TO_AGENT_CONNECT_EXISTING\n");
		handle_connect_existing(s_hdr.client_id, s_hdr.len);
		break;
	case MSG_SERVER_TO_AGENT_EXEC_CMDLINE:
		lprintf("MSG_SERVER_TO_AGENT_EXEC_CMDLINE\n");

		// This will return error only if vchan fails.
		uResult = handle_exec(s_hdr.client_id, s_hdr.len);
		if (ERROR_SUCCESS != uResult) {
			lprintf_err(uResult, "handle_server_data(): handle_exec()");
			return uResult;
		}		
		break;

	case MSG_SERVER_TO_AGENT_JUST_EXEC:
		lprintf("MSG_SERVER_TO_AGENT_JUST_EXEC\n");

		// This will return error only if vchan fails.
		uResult = handle_just_exec(s_hdr.client_id, s_hdr.len);
		if (ERROR_SUCCESS != uResult) {
			lprintf_err(uResult, "handle_server_data(): handle_just_exec()");
			return uResult;
		}
		break;

	case MSG_SERVER_TO_AGENT_INPUT:
		lprintf("MSG_SERVER_TO_AGENT_INPUT\n");

		// This will return error only if vchan fails.
		uResult = handle_input(s_hdr.client_id, s_hdr.len);
		if (ERROR_SUCCESS != uResult) {
			lprintf_err(uResult, "handle_server_data(): handle_input()");
			return uResult;
		}
		break;

	case MSG_SERVER_TO_AGENT_CLIENT_END:
		lprintf("MSG_SERVER_TO_AGENT_CLIENT_END\n");
		RemoveClient(FindClientById(s_hdr.client_id));
		break;
	default:
		lprintf("handle_server_data(): Msg type from daemon is %d ?\n",
			s_hdr.type);
		return ERROR_INVALID_FUNCTION;
	}

	return ERROR_SUCCESS;
}




ULONG FillAsyncIoData(ULONG uEventNumber, ULONG uClientNumber, UCHAR bHandleType, PIPE_DATA *pPipeData)
{
	ULONG	uResult;


	if (uEventNumber >= RTL_NUMBER_OF(g_WatchedEvents) || 
		uClientNumber >= RTL_NUMBER_OF(g_Clients) ||
		!pPipeData)
		return ERROR_INVALID_PARAMETER;


	uResult = ERROR_SUCCESS;

	if (!pPipeData->bReadInProgress && !pPipeData->bDataIsReady) {

		memset(&pPipeData->ReadBuffer, 0, READ_BUFFER_SIZE);

		if (!ReadFile(
			pPipeData->hReadPipe, 
			&pPipeData->ReadBuffer, 
			READ_BUFFER_SIZE, 
			NULL,
			&pPipeData->olRead)) {

			// Last error is usually ERROR_IO_PENDING here because of the asynchronous read.
			// But if the process has closed it would be ERROR_BROKEN_PIPE.
			uResult = GetLastError();
			if (ERROR_IO_PENDING == uResult)
				pPipeData->bReadInProgress = TRUE;

		} else {
			// The read has completed synchronously. 
			// The event in the OVERLAPPED structure should be signalled by now.
			pPipeData->bDataIsReady = TRUE;

			// Do not set bReadInProgress to TRUE in this case because if the pipes are to be closed
			// before the next read IO starts then there will be no IO to cancel.
			// bReadInProgress indicates to the CloseReadPipeHandles() that the IO should be canceled.

			// If after the WaitFormultipleObjects() this event is not chosen because of
			// some other event is also signaled, we will not rewrite the data in the buffer
			// on the next iteration of FillAsyncIoData() because bDataIsReady is set.
		}
	}

	if (pPipeData->bReadInProgress || pPipeData->bDataIsReady) {

		g_HandlesInfo[uEventNumber].uClientNumber = uClientNumber;
		g_HandlesInfo[uEventNumber].bType = bHandleType;
		g_WatchedEvents[uEventNumber] = pPipeData->olRead.hEvent;
	}


	return uResult;
}




ULONG WatchForEvents()
{
	EVTCHN	evtchn;
	OVERLAPPED	ol;
	unsigned int fired_port;
	ULONG	i, uEventNumber, uClientNumber;
	DWORD	dwSignaledEvent;
	PCLIENT_INFO	pClientInfo;
	DWORD	dwExitCode;
	BOOLEAN	bVchanIoInProgress;
	ULONG	uResult;
	BOOLEAN	bVchanReturnedError;
	BOOLEAN	bVchanClientConnected;
	int	client_id;


	// This will not block.
	uResult = peer_server_init(REXEC_PORT);
	if (uResult) {
		lprintf_err(ERROR_INVALID_FUNCTION, "WatchForEvents(): peer_server_init()");
		return ERROR_INVALID_FUNCTION;
	}

	lprintf("WatchForEvents(): Awaiting for a vchan client\n");

	evtchn = libvchan_fd_for_select(ctrl);

	memset(&ol, 0, sizeof(ol));
	ol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);


	bVchanClientConnected = FALSE;
	bVchanIoInProgress = FALSE;
	bVchanReturnedError = FALSE;

	for (;;) {

		uEventNumber = 0;

		// Order matters.
		g_WatchedEvents[uEventNumber++] = g_hStopServiceEvent;
		g_WatchedEvents[uEventNumber++] = g_hAddExistingClientEvent;

		g_HandlesInfo[0].bType = g_HandlesInfo[1].bType = HTYPE_INVALID;

		uResult = ERROR_SUCCESS;

		libvchan_prepare_to_select(ctrl);
		// read 1 byte instead of sizeof(fired_port) to not flush fired port
		// from evtchn buffer; evtchn driver will read only whole fired port
		// numbers (sizeof(fired_port)), so this will end in zero-length read
		if (!ReadFile(evtchn, &fired_port, 1, NULL, &ol)) {
			uResult = GetLastError();
			if (ERROR_IO_PENDING != uResult) {
				lprintf_err(uResult, "WatchForEvents(): Vchan async read");
				bVchanReturnedError = TRUE;
				break;
			}
		}

		bVchanIoInProgress = TRUE;

		if (ERROR_SUCCESS == uResult || ERROR_IO_PENDING == uResult) {
			g_HandlesInfo[uEventNumber].uClientNumber = FREE_CLIENT_SPOT_ID;
			g_HandlesInfo[uEventNumber].bType = HTYPE_VCHAN;
			g_WatchedEvents[uEventNumber++] = ol.hEvent;
		}


		EnterCriticalSection(&g_ClientsCriticalSection);

		for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++) {

			if (g_Clients[uClientNumber].bClientIsReady) {

				g_HandlesInfo[uEventNumber].uClientNumber = uClientNumber;
				g_HandlesInfo[uEventNumber].bType = HTYPE_PROCESS;
				g_WatchedEvents[uEventNumber++] = g_Clients[uClientNumber].hProcess;

				if (!g_Clients[uClientNumber].bReadingIsDisabled) {
					// Skip those clients which have received MSG_XOFF.
					FillAsyncIoData(uEventNumber++, uClientNumber, HTYPE_STDOUT, &g_Clients[uClientNumber].Stdout);
					FillAsyncIoData(uEventNumber++, uClientNumber, HTYPE_STDERR, &g_Clients[uClientNumber].Stderr);
				}
			}
		}
		LeaveCriticalSection(&g_ClientsCriticalSection);


		dwSignaledEvent = WaitForMultipleObjects(uEventNumber, g_WatchedEvents, FALSE, INFINITE);
		if (dwSignaledEvent >= MAXIMUM_WAIT_OBJECTS) {

			uResult = GetLastError();
			if (ERROR_INVALID_HANDLE != uResult) {
				lprintf_err(uResult, "WatchForEvents(): WaitForMultipleObjects()");
				break;
			}

			// WaitForMultipleObjects() may fail with ERROR_INVALID_HANDLE if the process which just has been added
			// to the client list terminated before WaitForMultipleObjects(). In this case IO pipe handles are closed
			// and invalidated, while a process handle is in the signaled state.
			// Check if any of the processes in the client list is terminated, remove it from the list and try again.

			EnterCriticalSection(&g_ClientsCriticalSection);

			for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++) {

				pClientInfo = &g_Clients[uClientNumber];

				if (!g_Clients[uClientNumber].bClientIsReady)
					continue;

				if (!GetExitCodeProcess(pClientInfo->hProcess, &dwExitCode)) {
					lprintf_err(GetLastError(), "WatchForEvents(): GetExitCodeProcess()");
					dwExitCode = ERROR_SUCCESS;
				}

				if (STILL_ACTIVE != dwExitCode) {
					int client_id;

					client_id = pClientInfo->client_id;
					// ensure that all data is sent before exit code
					RemoveClientNoLocks(pClientInfo);
					uResult = send_exit_code(client_id, dwExitCode);
					if (ERROR_SUCCESS != uResult) {
						bVchanReturnedError = TRUE;
						lprintf_err(uResult, "WatchForEvents(): send_exit_code()");
					}

				}
			}
			LeaveCriticalSection(&g_ClientsCriticalSection);

			continue;

		} else {

			if (0 == dwSignaledEvent)
				// g_hStopServiceEvent is signaled
				break;


			if (HTYPE_VCHAN != g_HandlesInfo[dwSignaledEvent].bType) {
				// If this is not a vchan event, cancel the event channel read so that libvchan_write() calls
				// could issue their own libvchan_wait on the same channel, and not interfere with the
				// ReadFile(evtchn, ...) above.
				if (CancelIo(evtchn))
					// Must wait for the canceled IO to complete, otherwise a race condition may occur on the
					// OVERLAPPED structure.
					WaitForSingleObject(ol.hEvent, INFINITE);
				bVchanIoInProgress = FALSE;
			}

			if (1 == dwSignaledEvent)
				// g_hAddExistingClientEvent is signaled. Since Vchan IO has been canceled,
				// safely re-iterate the loop and pick up the new handles to watch.
				continue;

			// Do not have to lock g_Clients here because other threads may only call 
			// ReserveClientNumber()/ReleaseClientNumber()/AddFilledClientInfo()
			// which operate on different uClientNumbers than those specified for WaitForMultipleObjects().

			// The other threads cannot call RemoveClient(), for example, they
			// operate only on newly allocated uClientNumbers.

			// So here in this thread we may call FindByClientId() with no locks safely.

			// When this thread (in this switch) calls RemoveClient() later the g_Clients
			// list will be locked as usual.

//			lprintf("client %d, type %d, signaled: %d, en %d\n", g_HandlesInfo[dwSignaledEvent].uClientNumber, g_HandlesInfo[dwSignaledEvent].bType, dwSignaledEvent, uEventNumber);
			switch (g_HandlesInfo[dwSignaledEvent].bType) {
				case HTYPE_VCHAN:

					// the following will never block; we need to do this to
					// clear libvchan_fd pending state
					//
					// using libvchan_wait here instead of reading fired
					// port at the beginning of the loop (ReadFile call) to be
					// sure that we clear pending state _only_
					// when handling vchan data in this loop iteration (not any
					// other process)
					libvchan_wait(ctrl);

					bVchanIoInProgress = FALSE;

					if (!bVchanClientConnected) {

						lprintf("WatchForEvents(): A vchan client has connected\n");

						// Remove the xenstore device/vchan/N entry.
						uResult = libvchan_server_handle_connected(ctrl);
						if (uResult) {
							lprintf_err(ERROR_INVALID_FUNCTION, "WatchForEvents(): libvchan_server_handle_connected()");
							bVchanReturnedError = TRUE;
							break;
						}

						bVchanClientConnected = TRUE;
						break;
					}

					if (!GetOverlappedResult(evtchn, &ol, &i, FALSE)) {
						if (GetLastError() != ERROR_OPERATION_ABORTED) {
							lprintf_err(GetLastError(), "WatchForEvents(): GetOverlappedResult(evtchn)");
							bVchanReturnedError = TRUE;
							break;
						}
					}

					EnterCriticalSection(&g_VchanCriticalSection);

					if (libvchan_is_eof(ctrl)) {
						bVchanReturnedError = TRUE;
						LeaveCriticalSection(&g_VchanCriticalSection);
						break;
					}

					while (read_ready_vchan_ext()) {
						uResult = handle_server_data();
						if (ERROR_SUCCESS != uResult) {
							bVchanReturnedError = TRUE;
							lprintf_err(uResult, "WatchForEvents(): handle_server_data()");
							LeaveCriticalSection(&g_VchanCriticalSection);
							break;
						}
					}

					LeaveCriticalSection(&g_VchanCriticalSection);
					break;

				case HTYPE_STDOUT:
#ifdef DISPLAY_CONSOLE_OUTPUT
					printf("%s", &g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].Stdout.ReadBuffer);
#endif

					uResult = ReturnPipeData(
							g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].client_id,
							&g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].Stdout);
					if (ERROR_SUCCESS != uResult) {
						bVchanReturnedError = TRUE;
						lprintf_err(uResult, "WatchForEvents(): ReturnPipeData(STDOUT)");
					}
					break;

				case HTYPE_STDERR:
#ifdef DISPLAY_CONSOLE_OUTPUT
					printf("%s", &g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].Stderr.ReadBuffer);
#endif

					uResult = ReturnPipeData(
							g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].client_id,
							&g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].Stderr);
					if (ERROR_SUCCESS != uResult) {
						bVchanReturnedError = TRUE;
						lprintf_err(uResult, "WatchForEvents(): ReturnPipeData(STDERR)");
					}
					break;

				case HTYPE_PROCESS:

					pClientInfo = &g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber];

					if (!GetExitCodeProcess(pClientInfo->hProcess, &dwExitCode)) {
						lprintf_err(GetLastError(), "WatchForEvents(): GetExitCodeProcess()");
						dwExitCode = ERROR_SUCCESS;
					}

					client_id = pClientInfo->client_id;
					RemoveClient(pClientInfo);

					uResult = send_exit_code(client_id, dwExitCode);
					if (ERROR_SUCCESS != uResult) {
						bVchanReturnedError = TRUE;
						lprintf_err(uResult, "WatchForEvents(): send_exit_code()");
					}

					break;
			}
		}


		if (bVchanReturnedError)
			break;

	}


	if (bVchanIoInProgress)
		if (CancelIo(evtchn))
			// Must wait for the canceled IO to complete, otherwise a race condition may occur on the
			// OVERLAPPED structure.
			WaitForSingleObject(ol.hEvent, INFINITE);

	if (!bVchanClientConnected)
		// Remove the xenstore device/vchan/N entry.
		libvchan_server_handle_connected(ctrl);

	// Cancel all the other pending IO.
	RemoveAllClients();

	if (bVchanClientConnected)
		libvchan_close(ctrl);

	// This is actually CloseHandle(evtchn)
	xc_evtchn_close(ctrl->evfd);

	CloseHandle(ol.hEvent);


	return bVchanReturnedError ? ERROR_INVALID_FUNCTION : ERROR_SUCCESS;
}


VOID Usage()
{
	_tprintf(TEXT("\nqrexec agent service\n\nUsage: qrexec_agent <-i|-u>\n"));
}


ULONG CheckForXenInterface()
{
	EVTCHN	xc;


	xc = xc_evtchn_open();
	if (INVALID_HANDLE_VALUE == xc)
		return ERROR_NOT_SUPPORTED;

	xc_evtchn_close(xc);
	return ERROR_SUCCESS;
}



ULONG WINAPI ServiceExecutionThread(PVOID pParam)
{
	ULONG	uResult;
	HANDLE	hTriggerEventsThread;


	lprintf("ServiceExecutionThread(): Service started\n");


	// Auto reset, initial state is not signaled
	g_hAddExistingClientEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (!g_hAddExistingClientEvent) {
		uResult = GetLastError();
		lprintf_err(uResult, "ServiceExecutionThread(): CreateEvent()");
		return uResult;
	}


	hTriggerEventsThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)WatchForTriggerEvents, NULL, 0, NULL);
	if (!hTriggerEventsThread) {
		uResult = GetLastError();
		CloseHandle(g_hAddExistingClientEvent);
		lprintf_err(uResult, "ServiceExecutionThread(): CreateThread()");
		return uResult;
	}


	for (;;) {

		uResult = WatchForEvents();
		if (ERROR_SUCCESS != uResult)
			lprintf_err(uResult, "ServiceExecutionThread(): WatchForEvents()");

		if (!WaitForSingleObject(g_hStopServiceEvent, 0))
			break;

		Sleep(1000);
	}

	lprintf("ServiceExecutionThread(): Waiting for the trigger thread to exit\n");
	WaitForSingleObject(hTriggerEventsThread, INFINITE);
	CloseHandle(hTriggerEventsThread);
	CloseHandle(g_hAddExistingClientEvent);

	DeleteCriticalSection(&g_ClientsCriticalSection);
	DeleteCriticalSection(&g_VchanCriticalSection);

	lprintf("ServiceExecutionThread(): Shutting down\n");

	return ERROR_SUCCESS;
}

#ifdef BUILD_AS_SERVICE

ULONG Init(HANDLE *phServiceThread)
{
	ULONG	uResult;
	HANDLE	hThread;
	ULONG	uClientNumber;


	*phServiceThread = INVALID_HANDLE_VALUE;

	uResult = CheckForXenInterface();
	if (ERROR_SUCCESS != uResult) {
		lprintf_err(uResult, "Init(): CheckForXenInterface()");
		ReportErrorToEventLog(XEN_INTERFACE_NOT_FOUND);
		return ERROR_NOT_SUPPORTED;
	}

	// InitializeCriticalSection always succeeds in Vista and later OSes.
#if NTDDI_VERSION < NTDDI_VISTA
	__try {
#endif
		InitializeCriticalSection(&g_ClientsCriticalSection);
		InitializeCriticalSection(&g_VchanCriticalSection);
		InitializeCriticalSection(&g_PipesCriticalSection);
#if NTDDI_VERSION < NTDDI_VISTA
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		lprintf("main(): InitializeCriticalSection() raised an exception %d\n", GetExceptionCode());
		return ERROR_NOT_ENOUGH_MEMORY;
	}
#endif

	for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
		g_Clients[uClientNumber].client_id = FREE_CLIENT_SPOT_ID;


	hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)ServiceExecutionThread, NULL, 0, NULL);
	if (!hThread) {
		uResult = GetLastError();
		lprintf_err(uResult, "StartServiceThread(): CreateThread()");
		return uResult;
	}

	*phServiceThread = hThread;

	return ERROR_SUCCESS;
}



// This is the entry point for a service module (BUILD_AS_SERVICE defined).
int __cdecl _tmain(ULONG argc, PTCHAR argv[])
{

	ULONG	uOption;
	PTCHAR	pszParam = NULL;
	TCHAR	szUserName[UNLEN + 1];
	TCHAR	szFullPath[MAX_PATH + 1];
	DWORD	nSize;
	ULONG	uResult;
	BOOL	bStop;
	TCHAR	bCommand;
	PTCHAR	pszAccountName = NULL;

	SERVICE_TABLE_ENTRY	ServiceTable[] = {
		{SERVICE_NAME, (LPSERVICE_MAIN_FUNCTION)ServiceMain},
		{NULL,NULL}
	};



	memset(szUserName, 0, sizeof(szUserName));
	nSize = RTL_NUMBER_OF(szUserName);
	if (!GetUserName(szUserName, &nSize)) {
		uResult = GetLastError();
		lprintf_err(uResult, "main(): GetUserName()");
		return uResult;
	}


	if ((1 == argc) && _tcscmp(szUserName, TEXT("SYSTEM"))) {
		Usage();
		return ERROR_INVALID_PARAMETER;
	}

	if (1 == argc) {

		lprintf("main(): Running as SYSTEM\n");

		uResult = ERROR_SUCCESS;
		if (!StartServiceCtrlDispatcher(ServiceTable)) {
			uResult = GetLastError();
			lprintf_err(uResult, "main(): StartServiceCtrlDispatcher()");
		}

		lprintf("main(): Exiting\n");
		return uResult;
	}

	memset(szFullPath, 0, sizeof(szFullPath));
	if (!GetModuleFileName(NULL, szFullPath, RTL_NUMBER_OF(szFullPath) - 1)) {
		uResult = GetLastError();
		lprintf_err(uResult, "main(): GetModuleFileName()");
		return uResult;
	}


	uResult = ERROR_SUCCESS;
	bStop = FALSE;
	bCommand = 0;

	while (!bStop) {

		uOption = GetOption(argc, argv, TEXT("iua:"), &pszParam);
		switch (uOption) {
		case 0:
			bStop = TRUE;
			break;

		case _T('i'):
		case _T('u'):
			if (bCommand) {
				bCommand = 0;
				bStop = TRUE;
			} else
				bCommand = (TCHAR)uOption;

			break;

		case _T('a'):
			if (pszParam)
				pszAccountName = pszParam;
			break;

		default:
			bCommand = 0;
			bStop = TRUE;
		}
	}

	if (pszAccountName) {

		lprintf("main(): GrantDesktopAccess(\"%S\")\n", pszAccountName);
		uResult = GrantDesktopAccess(pszAccountName, NULL);
		if (ERROR_SUCCESS != uResult)
			lprintf_err(uResult, "main(): GrantDesktopAccess(\"%S\")", pszAccountName);

		return uResult;
	}

	switch (bCommand) {
		case _T('i'):
			uResult = InstallService(szFullPath, SERVICE_NAME);
			break;

		case _T('u'):
			uResult = UninstallService(SERVICE_NAME);
			break;
		default:
			Usage();
	}

	return uResult;
}

#else

// Is not called when built without BUILD_AS_SERVICE definition.
ULONG Init(HANDLE *phServiceThread)
{
	return ERROR_SUCCESS;
}

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType)
{
	lprintf("CtrlHandler(): Got shutdown signal\n");

	SetEvent(g_hStopServiceEvent);

	WaitForSingleObject(g_hCleanupFinishedEvent, 2000);

	CloseHandle(g_hStopServiceEvent);
	CloseHandle(g_hCleanupFinishedEvent);

	lprintf("CtrlHandler(): Shutdown complete\n");
	ExitProcess(0);
	return TRUE;
}

// This is the entry point for a console application (BUILD_AS_SERVICE not defined).
int __cdecl _tmain(ULONG argc, PTCHAR argv[])
{
	ULONG	uResult;
	ULONG	uClientNumber;


	_tprintf(TEXT("\nqrexec agent console application\n\n"));

	if (ERROR_SUCCESS != CheckForXenInterface()) {
		lprintf("main(): Could not find Xen interface\n");
		return ERROR_NOT_SUPPORTED;
	}

	// Manual reset, initial state is not signaled
	g_hStopServiceEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!g_hStopServiceEvent) {
		uResult = GetLastError();
		lprintf_err(uResult, "main(): CreateEvent()");
		return uResult;
	}

	// Manual reset, initial state is not signaled
	g_hCleanupFinishedEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!g_hCleanupFinishedEvent) {
		uResult = GetLastError();
		CloseHandle(g_hStopServiceEvent);
		lprintf_err(uResult, "main(): CreateEvent()");
		return uResult;
	}

	// InitializeCriticalSection always succeeds in Vista and later OSes.
#if NTDDI_VERSION < NTDDI_VISTA
	__try {
#endif
		InitializeCriticalSection(&g_ClientsCriticalSection);
		InitializeCriticalSection(&g_VchanCriticalSection);
		InitializeCriticalSection(&g_PipesCriticalSection);
#if NTDDI_VERSION < NTDDI_VISTA
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		lprintf("main(): InitializeCriticalSection() raised an exception %d\n", GetExceptionCode());
		return ERROR_NOT_ENOUGH_MEMORY;
	}
#endif
	SetConsoleCtrlHandler((PHANDLER_ROUTINE)CtrlHandler, TRUE);

	for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
		g_Clients[uClientNumber].client_id = FREE_CLIENT_SPOT_ID;

	ServiceExecutionThread(NULL);
	SetEvent(g_hCleanupFinishedEvent);

	return ERROR_SUCCESS;
}
#endif
