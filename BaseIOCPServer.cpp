#include "IOCPCommon.h"
#include "IOCPBuffer.h"
#include "BaseIOCPServer.h"

#pragma comment(lib,"ws2_32.lib")


static GUID WSAID_AcceptEx = WSAID_ACCEPTEX;
static GUID WSAID_GetAcceptExSockaddrs = WSAID_GETACCEPTEXSOCKADDRS;

#define BASE_IOCP_RECEIVE_BUFFER_SIZE 0x1000

int myprintf(const char *lpFormat, ...) {

	int nLen = 0;
	int nRet = 0;
	char cBuffer[512];
	va_list arglist;
	HANDLE hOut = NULL;

	ZeroMemory(cBuffer, sizeof(cBuffer));

	va_start(arglist, lpFormat);

	nLen = lstrlenA(lpFormat);
	nRet = _vsnprintf(cBuffer, 512, lpFormat, arglist);

	if (nRet != -1)
	{
		if (nRet >= nLen || GetLastError() == 0) {
			hOut = GetStdHandle(STD_OUTPUT_HANDLE);
			if (hOut != INVALID_HANDLE_VALUE)
				WriteConsoleA(hOut, cBuffer, lstrlenA(cBuffer), (LPDWORD)&nLen, NULL);
		}
		return nLen;
	}
	
	char *buff=0;
	size_t buffSize=8096;
	buff = (char *)malloc(buffSize);
	while (1)
	{
		buff = (char*) realloc(buff, buffSize);
		nRet = _vsnprintf(buff, buffSize, lpFormat, arglist);

		if (nRet!=-1)
		{
			break;
		}
		buffSize*=2;
	}

	if (nRet >= nLen || GetLastError() == 0) {
		hOut = GetStdHandle(STD_OUTPUT_HANDLE);
		if (hOut != INVALID_HANDLE_VALUE)
			WriteConsoleA(hOut, buff, lstrlenA(buff), (LPDWORD)&nLen, NULL);
	}

	free(buff);


	return nLen;
}
_PER_IO_CONTEXT::_PER_IO_CONTEXT(){
	memset(&Overlapped, 0, sizeof(Overlapped));
	memset(&wsabuf, 0, sizeof(wsabuf));
	SocketAccept = INVALID_SOCKET;
	IOCPBuffer = NULL;
}
_PER_SOCKET_CONTEXT::_PER_SOCKET_CONTEXT(){
	QueryPerformanceCounter((LARGE_INTEGER*)&m_guid);
	m_NumberOfPendingIO = 0;
	m_Socket = INVALID_SOCKET;
}
_PER_SOCKET_CONTEXT::~_PER_SOCKET_CONTEXT()
{
	if (m_RecvContext.IOCPBuffer != NULL){
		OP_DELETE<CIOCPBuffer>(m_RecvContext.IOCPBuffer, _FILE_AND_LINE_);
		m_RecvContext.IOCPBuffer = NULL;
	}

	if (m_SendContext.IOCPBuffer != NULL){
		OP_DELETE<CIOCPBuffer>(m_SendContext.IOCPBuffer, _FILE_AND_LINE_);
		m_SendContext.IOCPBuffer = NULL;
	}

	while (!m_SendBufferList.Empty()){
		OP_DELETE<CIOCPBuffer>(m_SendBufferList.Pop(), _FILE_AND_LINE_);
	}
}
void _PER_SOCKET_CONTEXT_LIST::AddContext(PPER_SOCKET_CONTEXT lpPerSocketContext)
{
	m_ContextLock.Lock();

	m_vContextMap[lpPerSocketContext->m_guid] = lpPerSocketContext;

	lpPerSocketContext->pos = m_vContextList.insert(m_vContextList.end(), lpPerSocketContext);

	m_ContextLock.UnLock();
}

void _PER_SOCKET_CONTEXT_LIST::DeleteContext(PPER_SOCKET_CONTEXT lpPerSocketContext)
{
	std::map<LONGLONG, PPER_SOCKET_CONTEXT>::iterator map_iter;
	m_ContextLock.Lock();
	map_iter = m_vContextMap.find(lpPerSocketContext->m_guid);
	if (map_iter != m_vContextMap.end()){
		m_vContextMap.erase(map_iter);
		m_vContextList.erase(lpPerSocketContext->pos);
	}
	m_ContextLock.UnLock();
}
void _PER_SOCKET_CONTEXT_LIST::ClearAll()
{
	m_ContextLock.Lock();
	m_vContextMap.clear();
	m_vContextList.clear();
	m_ContextLock.UnLock();
}
PPER_SOCKET_CONTEXT _PER_SOCKET_CONTEXT_LIST::GetContext(LONGLONG guid)
{
	PPER_SOCKET_CONTEXT lpPerSocketContext = NULL;
	std::map<LONGLONG, PPER_SOCKET_CONTEXT>::iterator map_iter;

	map_iter = m_vContextMap.find(guid);
	if (map_iter != m_vContextMap.end()){
		lpPerSocketContext = map_iter->second;
	}

	return lpPerSocketContext;
}
CBaseIOCPServer::CBaseIOCPServer(void)
{
	WSAStartup(MAKEWORD(2, 2), &m_WSAData);
	m_AcceptEx = NULL;
	m_Listen = INVALID_SOCKET;
	m_IOCP = INVALID_HANDLE_VALUE;
	m_ThreadHandles = NULL;
	m_ThreadHandleCount = 0;
	m_CurrentConnectCount = 0;
	m_LimitConnectCount = 0;
	m_ListenContext = NULL;
}


CBaseIOCPServer::~CBaseIOCPServer(void){
	Shutdown();
}

ULONG CBaseIOCPServer::EnterIOLoop(PPER_SOCKET_CONTEXT lpPerSocketContext){
	return InterlockedIncrement(&lpPerSocketContext->m_NumberOfPendingIO);
}
ULONG CBaseIOCPServer::ExitIOLoop(PPER_SOCKET_CONTEXT lpPerSocketContext){
	return InterlockedDecrement(&lpPerSocketContext->m_NumberOfPendingIO);
}
VOID CBaseIOCPServer::UpdateCompletionPort(PPER_SOCKET_CONTEXT lpPerSocketContext){
	m_IOCP = CreateIoCompletionPort((HANDLE)lpPerSocketContext->m_Socket, m_IOCP, (DWORD_PTR)lpPerSocketContext, 0);
}
VOID CBaseIOCPServer::UpdateSocket(SOCKET sd){
	int nRet = setsockopt(
		sd,
		SOL_SOCKET,
		SO_UPDATE_ACCEPT_CONTEXT,
		(char *)&m_Listen,
		sizeof(m_Listen)
		);

	if (nRet == SOCKET_ERROR) {
		//
		//just warn user here.
		//
		myprintf("setsockopt(SO_UPDATE_ACCEPT_CONTEXT) failed to update accept socket failed: %d\n", WSAGetLastError());
	}
}
FARPROC CBaseIOCPServer::GetExtensionProcAddress(GUID& Guid)
{
	FARPROC lpfn = NULL;

	DWORD dwBytes = 0;

	WSAIoctl(
		m_Listen,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&Guid,
		sizeof(Guid),
		&lpfn,
		sizeof(lpfn),
		&dwBytes,
		NULL,
		NULL);


	return lpfn;
}
SOCKET CBaseIOCPServer::CreateSocket(void)
{
	int nRet = 0;
	int nZero = 0;
	SOCKET sdSocket = INVALID_SOCKET;

	sdSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (sdSocket == INVALID_SOCKET) {
		myprintf("WSASocket(sdSocket) failed: %d\n", WSAGetLastError());
		return sdSocket;
	}

	nZero = 0;
	nRet = setsockopt(sdSocket, SOL_SOCKET, SO_SNDBUF, (char *)&nZero, sizeof(nZero));
	if (nRet == SOCKET_ERROR) {
		myprintf("setsockopt(SNDBUF) failed: %d\n", WSAGetLastError());
		return sdSocket;
	}

	return sdSocket;
}

BOOL CBaseIOCPServer::CreateListenSocket(USHORT nPort) {
	int nRet = 0;
	sockaddr_in hints = { 0 };

	m_Listen = CreateSocket();
	if (m_Listen == INVALID_SOCKET) {
		return FALSE;
	}

	hints.sin_family = AF_INET;
	hints.sin_addr.S_un.S_addr = 0;
	hints.sin_port = htons(nPort);


	nRet = bind(m_Listen, (sockaddr *)&hints, sizeof(sockaddr_in));
	if (nRet == SOCKET_ERROR) {
		closesocket(m_Listen);
		m_Listen = INVALID_SOCKET;
		myprintf("bind() failed: %d\n", WSAGetLastError());
		return FALSE;
	}

	nRet = listen(m_Listen, SOMAXCONN);
	if (nRet == SOCKET_ERROR) {
		closesocket(m_Listen);
		m_Listen = INVALID_SOCKET;
		myprintf("listen() failed: %d\n", WSAGetLastError());
		return FALSE;
	}

	*(FARPROC*)&m_AcceptEx = GetExtensionProcAddress(WSAID_AcceptEx);
	return TRUE;
}


BOOL CBaseIOCPServer::CreateAcceptSocket(BOOL fUpdateIOCP, PPER_IO_CONTEXT lpPerIOContext) {
	int nRet = 0;
	DWORD dwRecvNumBytes = 0;

	if (fUpdateIOCP == TRUE) {
		m_ListenContext = AllocateSocketContext();
		m_ListenContext->m_Socket = m_Listen;
		UpdateCompletionPort(m_ListenContext);
	}

#define ACCEPTEX_BUFFER_SIZE ((sizeof(sockaddr_in) + 16) * 2)

	if (!lpPerIOContext){
		lpPerIOContext = OP_NEW<PER_IO_CONTEXT>(_FILE_AND_LINE_);
		lpPerIOContext->IOCPBuffer = OP_NEW_1<CIOCPBuffer, DWORD>(_FILE_AND_LINE_, ACCEPTEX_BUFFER_SIZE);
		m_AcceptIOContext.push_back(lpPerIOContext);
	}
	//AcceptEx����ͨ��accept������һ��
	//��Ҫ��׼����SOCKET
	lpPerIOContext->SocketAccept = CreateSocket();
	lpPerIOContext->IOOperation = ClientIoAccept;

	memset(lpPerIOContext->IOCPBuffer->m_pData->m_pData, 0, lpPerIOContext->IOCPBuffer->m_pData->m_dwDataLength);
	memset(&lpPerIOContext->Overlapped, 0, sizeof(lpPerIOContext->Overlapped));

	nRet = m_AcceptEx(m_Listen,
		lpPerIOContext->SocketAccept,
		lpPerIOContext->IOCPBuffer->m_pData->m_pData,
		0,
		sizeof(sockaddr_in) + 16,
		sizeof(sockaddr_in) + 16,
		&dwRecvNumBytes,
		&lpPerIOContext->Overlapped);

	if (nRet == FALSE && (ERROR_IO_PENDING != WSAGetLastError())) {
		myprintf("AcceptEx() failed: %d\n", WSAGetLastError());
		return FALSE;
	}

#undef ACCEPTEX_BUFFER_SIZE
	return TRUE;
}

PPER_SOCKET_CONTEXT CBaseIOCPServer::AllocateSocketContext()
{
	PPER_SOCKET_CONTEXT lpPerSocketContext = OP_NEW<PER_SOCKET_CONTEXT>(_FILE_AND_LINE_);

	EnterIOLoop(lpPerSocketContext);

	return lpPerSocketContext;
}
VOID CBaseIOCPServer::ReleaseSocketContext(PPER_SOCKET_CONTEXT lpPerSocketContext)
{
	if (ExitIOLoop(lpPerSocketContext) <= 0)
	{
		OP_DELETE<PER_SOCKET_CONTEXT>(lpPerSocketContext, _FILE_AND_LINE_);
	}
}
VOID CBaseIOCPServer::CloseClient(PPER_SOCKET_CONTEXT lpPerSocketContext)
{
	lpPerSocketContext->m_Lock.Lock();
	if (lpPerSocketContext->m_Socket != INVALID_SOCKET)
	{
		NotifyDisconnectedClient(lpPerSocketContext);
		InterlockedDecrement(&m_CurrentConnectCount);

		LINGER  lingerStruct;
		lingerStruct.l_onoff = 1;
		lingerStruct.l_linger = 0;
		//ǿ�ƹر��û�����
		setsockopt(lpPerSocketContext->m_Socket, SOL_SOCKET, SO_LINGER,
			(char *)&lingerStruct, sizeof(lingerStruct));

		m_ContextList.DeleteContext(lpPerSocketContext);

		closesocket(lpPerSocketContext->m_Socket);
		lpPerSocketContext->m_Socket = INVALID_SOCKET;
	}
	lpPerSocketContext->m_Lock.UnLock();
}

VOID CBaseIOCPServer::PostClientIoRead(PPER_SOCKET_CONTEXT lpPerSocketContext, PPER_IO_CONTEXT lpPerIOContext, IO_POST_RESULT& PostResult)
{
	DWORD dwFlags = 0;
	DWORD dwNumberRecvd = 0;

	lpPerSocketContext->m_Lock.Lock();

	PostResult = PostIoSuccess;

	if (lpPerSocketContext->m_Socket == INVALID_SOCKET)
	{
		PostResult = PostIoInvalicSocket;
		goto _End;
	}

	lpPerIOContext->IOOperation = ClientIoRead;						//������Ϣ����
	lpPerIOContext->SocketAccept = lpPerSocketContext->m_Socket;

	//Ͷ�ݽ��ղ���
	lpPerIOContext->wsabuf.len = lpPerIOContext->IOCPBuffer->GetBufferLength();
	lpPerIOContext->wsabuf.buf = (CHAR*)lpPerIOContext->IOCPBuffer->GetBuffer();

	if (WSARecv(lpPerIOContext->SocketAccept, &lpPerIOContext->wsabuf, 1, &dwNumberRecvd, &dwFlags, &lpPerIOContext->Overlapped, NULL) == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
	{
		PostResult = PostIoFailed;
	}

_End:
	lpPerSocketContext->m_Lock.UnLock();
}

VOID CBaseIOCPServer::PostClientIoWrite(PPER_SOCKET_CONTEXT lpPerSocketContext, PPER_IO_CONTEXT lpPerIOContext, IO_POST_RESULT& PostResult)
{
	DWORD dwFlags = 0;
	DWORD dwNumberSent = 0;

	lpPerSocketContext->m_Lock.Lock();

	PostResult = PostIoSuccess;

	if (lpPerSocketContext->m_Socket == INVALID_SOCKET)
	{
		PostResult = PostIoInvalicSocket;
		goto _End;
	}

	//����IO����
	lpPerIOContext->IOOperation = ClientIoWrite;
	lpPerIOContext->SocketAccept = lpPerSocketContext->m_Socket;

	//�����Ӧ�÷��Ͷ����ֽ�
	DWORD nTotalBytes = lpPerIOContext->IOCPBuffer->GetBufferLength();
	DWORD nSentBytes = nTotalBytes - lpPerIOContext->IOCPBuffer->m_dwReaderPosition;

	//���㵽���ͻ�������λ�ò������ó���
	lpPerIOContext->wsabuf.len = nSentBytes;
	lpPerIOContext->wsabuf.buf = (CHAR*)&lpPerIOContext->IOCPBuffer->GetBuffer()[lpPerIOContext->IOCPBuffer->m_dwReaderPosition];
	//Ͷ�ݷ�����Ϣ
	if (WSASend(lpPerIOContext->SocketAccept, &lpPerIOContext->wsabuf, 1, &dwNumberSent, dwFlags, &lpPerIOContext->Overlapped, NULL) == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
	{
		PostResult = PostIoFailed;
	}

_End:
	lpPerSocketContext->m_Lock.UnLock();
}

VOID CBaseIOCPServer::OnClientIoAccept(PPER_SOCKET_CONTEXT lpPerSocketContext, PPER_IO_CONTEXT lpPerIOContext, DWORD dwIoSize)
{
	/*����WinSock2��һ����!!!*/
	UpdateSocket(lpPerIOContext->SocketAccept);		//һ��Ҫ��UpdateSocket,�����´�AcceptEx���м��ʳ���Ī����������⡭��

	if (InterlockedIncrement(&m_CurrentConnectCount) > m_LimitConnectCount)
	{
		InterlockedDecrement(&m_CurrentConnectCount);
		closesocket(lpPerIOContext->SocketAccept);
		CreateAcceptSocket(FALSE, lpPerIOContext);
		return;
	}

	IO_POST_RESULT PostResult;


	//����������,��������Ͷ����һ��AcceptEx����
	SOCKET SocketAccept = lpPerIOContext->SocketAccept;

	//����Ͷ����һ��AcceptEx
	CreateAcceptSocket(FALSE, lpPerIOContext);

	//�û����Ӵ���
	PPER_SOCKET_CONTEXT lpPerAcceptSocketContext = AllocateSocketContext();
	lpPerAcceptSocketContext->m_Socket = SocketAccept;
	m_ContextList.AddContext(lpPerAcceptSocketContext);
	UpdateCompletionPort(lpPerAcceptSocketContext);

	NotifyNewConnection(lpPerAcceptSocketContext);

	PPER_IO_CONTEXT lpPerRecvIOContext = &lpPerAcceptSocketContext->m_RecvContext;
	lpPerRecvIOContext->IOCPBuffer = OP_NEW_1<CIOCPBuffer, DWORD>(_FILE_AND_LINE_, BASE_IOCP_RECEIVE_BUFFER_SIZE);

	PostClientIoRead(lpPerAcceptSocketContext, lpPerRecvIOContext, PostResult);

	//���Ͷ�ݳɹ�,��ֱ�ӷ��ؼ���
	if (PostResult == PostIoSuccess){
		return;
	}

	//����Ͷ��ʧ�ܵĻ�,ֱ�ӹر�SOCKET���ͷ�PER_SOCKET_CONTEXT�ṹ
	if (PostResult == PostIoFailed){
		CloseClient(lpPerAcceptSocketContext);
		ReleaseSocketContext(lpPerAcceptSocketContext);
	}
}

VOID CBaseIOCPServer::OnClientIoRead(PPER_SOCKET_CONTEXT lpPerSocketContext, PPER_IO_CONTEXT lpPerIOContext, DWORD dwIoSize)
{
	IO_POST_RESULT PostResult;

	//�յ�����,���û�����,����֪ͨ�û��Ѿ��յ�������
	//lpPerIOContext->IOCPBuffer->m_pData->m_dwDataLength = dwIoSize;
	lpPerIOContext->IOCPBuffer->m_dwWriterPosition = dwIoSize;

	NotifyReceivedPackage(lpPerSocketContext, lpPerIOContext->IOCPBuffer);
	//����������
	OP_DELETE<CIOCPBuffer>(lpPerIOContext->IOCPBuffer, _FILE_AND_LINE_);
	lpPerIOContext->IOCPBuffer = OP_NEW_1<CIOCPBuffer, DWORD>(_FILE_AND_LINE_, BASE_IOCP_RECEIVE_BUFFER_SIZE);

	PostClientIoRead(lpPerSocketContext, lpPerIOContext, PostResult);

	//���Ͷ�ݳɹ�,��ֱ�ӷ��ؼ���
	if (PostResult == PostIoSuccess){
		return;
	}

	//����Ͷ��ʧ�ܵĻ�,ֱ�ӹر�SOCKET���ͷ�PER_SOCKET_CONTEXT�ṹ
	if (PostResult == PostIoFailed || PostResult == PostIoInvalicSocket){
		CloseClient(lpPerSocketContext);
		ReleaseSocketContext(lpPerSocketContext);
	}
}
VOID CBaseIOCPServer::OnClientIoWrite(PPER_SOCKET_CONTEXT lpPerSocketContext, PPER_IO_CONTEXT lpPerIOContext, DWORD dwIoSize)
{
	lpPerIOContext->IOCPBuffer->m_dwReaderPosition += dwIoSize;		//�����Ѿ�д��Ļ���������

	NotifyWritePackage(lpPerSocketContext, lpPerIOContext->IOCPBuffer);

	if (lpPerIOContext->IOCPBuffer->m_dwReaderPosition >= lpPerIOContext->IOCPBuffer->GetBufferLength())		//�������������,֪ͨ�û��ص�,�Ҹ�����һ�����ݼ�������
	{
		NotifyWriteCompleted(lpPerSocketContext, lpPerIOContext->IOCPBuffer);

		//��С����ʱ��
		lpPerSocketContext->m_Lock.Lock();
		OP_DELETE<CIOCPBuffer>(lpPerIOContext->IOCPBuffer, _FILE_AND_LINE_);
		lpPerIOContext->IOCPBuffer = lpPerSocketContext->m_SendBufferList.Pop();
		lpPerSocketContext->m_Lock.UnLock();
	}

	if (lpPerIOContext->IOCPBuffer)
	{
		IO_POST_RESULT PostResult;

		PostClientIoWrite(lpPerSocketContext, lpPerIOContext, PostResult);

		//���Ͷ�ݳɹ�,��ֱ�ӷ��ؼ���
		if (PostResult == PostIoSuccess){
			return;
		}

		//����Ͷ��ʧ�ܵĻ�,ֱ�ӹر�SOCKET���ͷ�PER_SOCKET_CONTEXT�ṹ
		if (PostResult == PostIoFailed || PostResult == PostIoInvalicSocket){
			CloseClient(lpPerSocketContext);
			ReleaseSocketContext(lpPerSocketContext);
		}
	}
	else
	{
		ReleaseSocketContext(lpPerSocketContext);
	}
}

DWORD WINAPI CBaseIOCPServer::WorkerThread(LPVOID Param)
{
	BOOL bSuccess;
	DWORD dwIoSize;
	PPER_SOCKET_CONTEXT lpPerSocketContext;
	PPER_IO_CONTEXT lpPerIOContext = NULL;

	CBaseIOCPServer* pThis = (CBaseIOCPServer*)Param;
	while (TRUE)
	{
		bSuccess = GetQueuedCompletionStatus(
			pThis->m_IOCP,
			&dwIoSize,
			(PULONG_PTR)&lpPerSocketContext,
			(LPOVERLAPPED *)&lpPerIOContext,
			INFINITE
			);

		if (lpPerSocketContext == NULL) {
			return 0;
		}
		//������Ҫע��һ��,AcceptEx��dwIoSize��0
		//����lpPerSocketContext�Ƿ�����Listen��Context
		//����һ��,�������AcceptEx�Ļ�,���ж��û��Ͽ�
		if (!bSuccess || (bSuccess && (dwIoSize == 0) && lpPerSocketContext != pThis->m_ListenContext)) {
			pThis->CloseClient(lpPerSocketContext);
			pThis->ReleaseSocketContext(lpPerSocketContext);
			continue;
		}

		//�ַ���ͬ��IOCP������
		switch (lpPerIOContext->IOOperation) {
		case ClientIoAccept:
			pThis->OnClientIoAccept(lpPerSocketContext, lpPerIOContext, dwIoSize);
			break;
		case ClientIoRead:
			pThis->OnClientIoRead(lpPerSocketContext, lpPerIOContext, dwIoSize);
			break;
		case ClientIoWrite:
			pThis->OnClientIoWrite(lpPerSocketContext, lpPerIOContext, dwIoSize);
			break;
		}
	}
	return 0;
}

BOOL CBaseIOCPServer::Startup(USHORT nPort, DWORD dwWorkerThreadCount, DWORD dwMaxConnection)
{
	if(m_Listen != INVALID_SOCKET){
		return FALSE;
	}
	//����һ�����ڼ�����SOCKET
	if (CreateListenSocket(nPort) == FALSE) {
		return FALSE;
	}
	
	m_IOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);

	//����һЩAcceptEx��Ϣ��Ͷ�ݵ�IOCP
	//AcceptEx��IOCPģ���µĸ�Ч���պ���
	//֧�ֶ��߳̽��տͻ�������
	if (CreateAcceptSocket(TRUE) == FALSE){
		return FALSE;
	}

	//���Ͷ��������AcceptEx��Ϣ
	for (DWORD dwThreadIndex = 0; dwThreadIndex < (dwWorkerThreadCount - 1); dwThreadIndex++){
		CreateAcceptSocket(FALSE);
	}
	
	//���������������
	m_LimitConnectCount = dwMaxConnection;

	//����IOCP�߳�
	m_ThreadHandles = OP_NEW_ARRAY<HANDLE>(dwWorkerThreadCount, _FILE_AND_LINE_);
	m_ThreadHandleCount = dwWorkerThreadCount;

	for (DWORD dwThreadIndex = 0; dwThreadIndex < dwWorkerThreadCount; dwThreadIndex++){
		m_ThreadHandles[dwThreadIndex] = CreateThread(NULL, NULL, WorkerThread, this, NULL, NULL);
	}

	return TRUE;
}
BOOL CBaseIOCPServer::Shutdown()
{
	if(m_Listen == INVALID_SOCKET){
		return FALSE;
	}
	//��ȫ�����߳��˳�
	for (DWORD i = 0; i < m_ThreadHandleCount; i++){
		PostQueuedCompletionStatus(m_IOCP, 0, NULL, NULL);
	}

	WaitForMultipleObjects(m_ThreadHandleCount, m_ThreadHandles, TRUE, INFINITE);

	for (DWORD i = 0; i < m_ThreadHandleCount; i++) {
		if (m_ThreadHandles[i] != INVALID_HANDLE_VALUE)
			CloseHandle(m_ThreadHandles[i]);
		m_ThreadHandles[i] = INVALID_HANDLE_VALUE;
	}

	OP_DELETE_ARRAY<HANDLE>(m_ThreadHandles, _FILE_AND_LINE_);

	m_ThreadHandles = NULL;

	//�رռ����׽���
	if (m_Listen != INVALID_SOCKET){
		closesocket(m_Listen);
		m_Listen = INVALID_SOCKET;
	}

	//�ͷ����й���AcceptEx������
	for (size_t i = 0; i < m_AcceptIOContext.size(); i++){
		PPER_IO_CONTEXT lpPerIOContext = m_AcceptIOContext[i];
		DWORD dwNumberBytes = 0;

		//�ȴ�ϵͳIO�������,ĳЩϵͳIO����û����ɻ�����
		//IO����û����ɵĻ��ᵼ�¶���
		while (!HasOverlappedIoCompleted(&lpPerIOContext->Overlapped))
			Sleep(1);

		closesocket(lpPerIOContext->SocketAccept);
		lpPerIOContext->SocketAccept = INVALID_SOCKET;

		OP_DELETE<CIOCPBuffer>(lpPerIOContext->IOCPBuffer, _FILE_AND_LINE_);
		OP_DELETE<PER_IO_CONTEXT>(lpPerIOContext, _FILE_AND_LINE_);
	}

	m_AcceptIOContext.clear();

	//��ȫ���ͷ����пͻ�����
	for (std::list<PPER_SOCKET_CONTEXT>::iterator iter = m_ContextList.m_vContextList.begin();
		iter != m_ContextList.m_vContextList.end(); iter++){
		DWORD dwNumberBytes = 0;
		PPER_SOCKET_CONTEXT lpPerSocketContext = (*iter);

		CloseClient(lpPerSocketContext);

		//�ȴ�ϵͳIO�������,ĳЩϵͳIO����û����ɻ�����
		//IO����û����ɵĻ��ᵼ�¶���
		//�������û��Ͷ��IOCP�Ļ�,HasOverlappedIoCompleted��һֱ����ʧ��,���Լ��IOCPBuffer�Ƿ���ֵ,��������Ѿ�Ͷ��
		while (lpPerSocketContext->m_RecvContext.IOCPBuffer && !HasOverlappedIoCompleted(&lpPerSocketContext->m_RecvContext.Overlapped))
			Sleep(1);

		while (lpPerSocketContext->m_SendContext.IOCPBuffer && !HasOverlappedIoCompleted(&lpPerSocketContext->m_SendContext.Overlapped))
			Sleep(1);
		
		OP_DELETE<PER_SOCKET_CONTEXT>(lpPerSocketContext, _FILE_AND_LINE_);
	}

	m_ContextList.ClearAll();
	
	delete m_ListenContext;

	CloseHandle(m_IOCP);
	m_IOCP = INVALID_HANDLE_VALUE;


	//����������Ϣ
	m_AcceptEx = NULL;
	m_Listen = INVALID_SOCKET;
	m_IOCP = INVALID_HANDLE_VALUE;
	m_ThreadHandles = NULL;
	m_ThreadHandleCount = 0;
	m_CurrentConnectCount = 0;
	m_LimitConnectCount = 0;
	m_ListenContext = NULL;

	return TRUE;
}

BOOL CBaseIOCPServer::Send(PPER_SOCKET_CONTEXT lpPerSocketContext, CIOCPBuffer* lpIOCPBuffer)
{
	IO_POST_RESULT PostResult;
	BOOL bRet = FALSE;
	lpPerSocketContext->m_Lock.Lock();

	if (lpPerSocketContext->m_Socket != INVALID_SOCKET){
		if (lpPerSocketContext->m_SendContext.IOCPBuffer){
			lpPerSocketContext->m_SendBufferList.Push(OP_NEW_1<CIOCPBuffer, CIOCPBuffer*>(_FILE_AND_LINE_, lpIOCPBuffer));
			bRet = TRUE;
		} else {
			lpPerSocketContext->m_SendContext.IOCPBuffer = OP_NEW_1<CIOCPBuffer, CIOCPBuffer*>(_FILE_AND_LINE_, lpIOCPBuffer);
			
			EnterIOLoop(lpPerSocketContext);

			PostClientIoWrite(lpPerSocketContext, &lpPerSocketContext->m_SendContext, PostResult);

			if (PostResult == PostIoSuccess){
				bRet = TRUE;
			}
			else {
				CloseClient(lpPerSocketContext);
				ReleaseSocketContext(lpPerSocketContext);
			}
		}
	}

	lpPerSocketContext->m_Lock.UnLock();

	return bRet;
}
BOOL CBaseIOCPServer::Send(LONGLONG guid,CIOCPBuffer* lpIOCPBuffer)
{
	BOOL bRet = FALSE;
	PPER_SOCKET_CONTEXT lpPerSocketContext;

	m_ContextList.m_ContextLock.Lock();
	lpPerSocketContext = m_ContextList.GetContext(guid);
	if(lpPerSocketContext){
		bRet = Send(lpPerSocketContext,lpIOCPBuffer);
	}

	m_ContextList.m_ContextLock.UnLock();

	return bRet;
}
VOID CBaseIOCPServer::NotifyNewConnection(PPER_SOCKET_CONTEXT lpPerSocketContext)
{

}
VOID CBaseIOCPServer::NotifyDisconnectedClient(PPER_SOCKET_CONTEXT lpPerSocketContext)
{

}
VOID CBaseIOCPServer::NotifyWriteCompleted(PPER_SOCKET_CONTEXT lpPerSocketContext, CIOCPBuffer* lpIOCPBuffer)
{

}
VOID CBaseIOCPServer::NotifyWritePackage(PPER_SOCKET_CONTEXT lpPerSocketContext, CIOCPBuffer* lpIOCPBuffer)
{

}
VOID CBaseIOCPServer::NotifyReceivedPackage(PPER_SOCKET_CONTEXT lpPerSocketContext, CIOCPBuffer* pBuffer)
{

}