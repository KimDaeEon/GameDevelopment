#include "NetworkSession.h"
// WaitForSingleObject = time-out 시간이 지날때까지 기다리거나, 어떤 오브젝트가 signaled 될 때까지 기다린다. (blocking), timeout 짧게 하면 스레드 끝나기 전에 리턴한다.
// WAIT_OBJECT_0 해당 이벤트 오브젝트가 signaled 상태가 되었음을 알린다.
// WAIT_TIMEOUT 해당 이벤트 오브젝트 상태가 nonsignaled 인 상태로 time-out 되어버렸음을 알린다.
// WAIT_FAILED 해당 function 자체가 실패, 에러를 알기 위해서 GetLastError 호출이 필요하다.

// SetEvent = Event Object 의 상태를 Signaled 상태로 변환
// CreateEvent = EVent Object Handle 을 생성 
// CreateThread = Thread 생성, 최근에는 _begintreadex 가 더 권장된다고 한다.추후 시간되면 더 조사해본 후 적용.https://killsia.tistory.com/entry/CreateThread-beginthread-beginthreadex%EC%9D%98-%EC%B0%A8%EC%9D%B4

DWORD WINAPI ReliableUdpThreadCallback(LPVOID parameter) 
{
	CNetworkSession* Owner = (CNetworkSession*)parameter;
	Owner->ReliableUdpThreadCallback();

	return 0;
}

CNetworkSession::CNetworkSession(VOID) 
{
	memset(&mAcceptOverlapped, 0, sizeof(mAcceptOverlapped));
	memset(&mReadOverlapped, 0, sizeof(mReadBuffer));
	memset(&mWriteOverlapped, 0, sizeof(mWriteOverlapped));

	memset(&mReadBuffer, 0, sizeof(mReadBuffer));
	memset(&mUdpRemoteInfo, 0, sizeof(mUdpRemoteInfo));

	mSocket							= NULL;
	mReliableUdpThreadHandle		= NULL;
	mReliableUdpThreadStartupEvent	= NULL;
	mReliableUdpThreadDestroyEvent	= NULL;
	mReliableUdpThreadWakeUpEvent	= NULL;
	mReliableUdpWriteCompleteEvent	= NULL;

	mIsReliableUdpSending			= FALSE;

	mAcceptOverlapped.IoType		= IO_ACCEPT;
	mReadOverlapped.IoType			= IO_READ;
	mWriteOverlapped.IoType			= IO_WRITE;

	mAcceptOverlapped.Object		= this;
	mReadOverlapped.Object			= this;
	mWriteOverlapped.Object			= this;
}

CNetworkSession::~CNetworkSession(VOID) 
{

}

VOID CNetworkSession::ReliableUdpThreadCallback(VOID)
{
	DWORD	EventID						= 0;
	HANDLE	ThreadEvents[2]				= { mReliableUdpThreadDestroyEvent, mReliableUdpThreadWakeUpEvent };

	CHAR	RemoteAddress[32]			= { 0, };
	USHORT	RemotePort					= 0;
	BYTE	Data[MAX_BUFFER_LENGTH]		= { 0, };
	DWORD	DataLength					= 0;
	VOID*	Object						= NULL;
	
	while (TRUE)
	{
		SetEvent(mReliableUdpThreadStartupEvent);

		EventID = WaitForMultipleObjects(2, ThreadEvents, FALSE, INFINITE);

		switch (EventID)
		{
		case WAIT_OBJECT_0:
			return;
		case WAIT_OBJECT_0 + 1:
NEXT_DATA:
			if (mReliableWriteQueue.Pop(&Object, Data, DataLength, RemoteAddress, RemotePort))
			{
RETRY:
				if (!WriteTo2(RemoteAddress, RemotePort, Data, DataLength))
					return;

				DWORD result = WaitForSingleObject(mReliableUdpWriteCompleteEvent, 10);  // 10 밀리 세컨드(1000분의 1초)만큼만 기다리겠단 뜻. 즉 0.01초

				if (result == WAIT_OBJECT_0)  // 해당 이벤트가 singaled 상태, 즉 스레드에 의해서 사용될 수 있는 상태이면 넘어간다.
					goto NEXT_DATA;
				else  // 해당 시간안에 처리가 안되면 재시도
					goto RETRY;
			}
		default:
			break;
		}
	}
}

BOOL CNetworkSession::Begin(VOID) {
	CThreadSync Sync;

	if (mSocket)
		return FALSE;

	memset(mReadBuffer, 0, sizeof(mReadBuffer));
	memset(&mUdpRemoteInfo, 0, sizeof(mUdpRemoteInfo));

	mSocket							= NULL;
	mReliableUdpThreadHandle		= NULL;
	mReliableUdpThreadStartupEvent	= NULL;
	mReliableUdpThreadDestroyEvent	= NULL;
	mReliableUdpThreadWakeUpEvent	= NULL;
	mReliableUdpWriteCompleteEvent	= NULL;

	mIsReliableUdpSending			= FALSE;

	return TRUE;
}

BOOL CNetworkSession::End(VOID)
{
	CThreadSync Sync;

	if (!mSocket)
		return FALSE;

	closesocket(mSocket);
	mSocket = NULL;

	if (mReliableUdpThreadHandle)
	{
		SetEvent(mReliableUdpThreadDestroyEvent);

		WaitForSingleObject(mReliableUdpThreadHandle, INFINITE);

		CloseHandle(mReliableUdpThreadHandle);
	}

	if (mReliableUdpThreadDestroyEvent)
		CloseHandle(mReliableUdpThreadDestroyEvent);

	if (mReliableUdpThreadStartupEvent)
		CloseHandle(mReliableUdpThreadStartupEvent);

	if (mReliableUdpThreadWakeUpEvent)
		CloseHandle(mReliableUdpThreadWakeUpEvent);

	if (mReliableUdpWriteCompleteEvent)
		CloseHandle(mReliableUdpWriteCompleteEvent);

	mReliableWriteQueue.End();

	return TRUE;
}

BOOL CNetworkSession::Listen(USHORT port, INT backLog)
{
	CThreadSync Sync;

	if (port <= 0 || backLog <= 0)
		return FALSE;

	if (!mSocket)
		return FALSE;

	SOCKADDR_IN ListenSocketInfo;

	ListenSocketInfo.sin_family				= AF_INET;
	ListenSocketInfo.sin_port				= htons(port);
	ListenSocketInfo.sin_addr.S_un.S_addr	= htonl(INADDR_ANY);

	if (bind(mSocket, (struct sockaddr*)&ListenSocketInfo, sizeof(SOCKADDR_IN)) == SOCKET_ERROR)
	{
		End();

		return FALSE;
	}

	if (listen(mSocket, backLog) == SOCKET_ERROR)
	{
		End();

		return FALSE;
	}

	LINGER Linger;
	Linger.l_onoff		= 1;
	Linger.l_linger		= 0;

	if (setsockopt(mSocket, SOL_SOCKET, SO_LINGER, (char*)&Linger, sizeof(LINGER)) == SOCKET_ERROR)
	{
		End();

		return FALSE;
	}

	return TRUE;
}

BOOL CNetworkSession::Accept(SOCKET listenSocket)
{
	CThreadSync Sync;

	if (!listenSocket)
		return FALSE;

	if (mSocket)
		return FALSE;

	mSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED); // WSA = Windows Sockets API

	if (mSocket == INVALID_SOCKET)
	{
		End();

		return FALSE;
	}

	//TODO: 아래 내용 여기 코드들 다 분석할 때에 다시 확인. 
	// BOOL NoDelay = TRUE; 
	//setsockopt(mSocket, IPPROTO_TCP, TCP_NODELAY, (const char FAR *)&NoDelay, sizeof(NoDelay));

	if (!AcceptEx(listenSocket,
		mSocket,
		mReadBuffer,
		0,
		sizeof(sockaddr_in) + 16,
		sizeof(sockaddr_in) + 16,
		NULL,
		&mAcceptOverlapped.Overlapped)) 
	{
		if (WSAGetLastError() != ERROR_IO_PENDING && WSAGetLastError() != WSAEWOULDBLOCK)
		{
			End();

			return FALSE;
		}
	}

	return TRUE;
}

BOOL CNetworkSession::InitializeReadForIocp(VOID)
{
	CThreadSync Sync;

	if (!mSocket)
		return FALSE;

	WSABUF	WsaBuf;
	DWORD	ReadBytes	= 0;
	DWORD	ReadFlag	= 0;
	
	WsaBuf.buf			= (CHAR*)mReadBuffer;
	WsaBuf.len			= MAX_BUFFER_LENGTH;

	INT	ReturnValue = WSARecv(mSocket,
		&WsaBuf,
		1,
		&ReadBytes,
		&ReadFlag,
		&mReadOverlapped.Overlapped,
		NULL);

	if (ReturnValue == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING && WSAGetLastError() != WSAEWOULDBLOCK)
	{
		End();

		return FALSE;
	}

	return TRUE;
}

BOOL CNetworkSession::ReadForIocp(BYTE* data, DWORD& dataLength)
{
	CThreadSync Sync;

	if (!mSocket)
		return FALSE;

	if (!data || dataLength <= 0)
		return FALSE;

	memcpy(data, mReadBuffer, dataLength);

	return TRUE;
}

BOOL CNetworkSession::ReadForEventSelect(BYTE* data, DWORD& dataLength)
{
	CThreadSync Sync;

	if (!mSocket)
		return FALSE;

	if (!data)
		return FALSE;

	if (!mSocket)
		return FALSE;

	WSABUF	WsaBuf;
	DWORD	ReadBytes	= 0;
	DWORD	ReadFlag	= 0;
	
	WsaBuf.buf			= (CHAR*)mReadBuffer;
	WsaBuf.len			= MAX_BUFFER_LENGTH;
	
	INT		ReturnValue = WSARecv(mSocket,
		&WsaBuf,
		1,
		&ReadBytes,
		&ReadFlag,
		&mReadOverlapped.Overlapped,
		NULL);
	
	if (ReturnValue == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING && WSAGetLastError() != WSAEWOULDBLOCK)
	{
		End();

		return FALSE;
	}

	memcpy(data, mReadBuffer, ReadBytes);
	dataLength = ReadBytes;

	return TRUE;
}

BOOL CNetworkSession::Write(BYTE* data, DWORD dataLength)
{
	CThreadSync Sync;

	if (!mSocket)
		return FALSE;

	if (!data || dataLength <= 0)
		return FALSE;

	WSABUF	WsaBuf;
	DWORD	WriteBytes	= 0;
	DWORD	WriteFlag	= 0;

	WsaBuf.buf			= (CHAR*)data;
	WsaBuf.len			= dataLength;

	INT		ReturnValue = WSASend(mSocket,
		&WsaBuf,
		1,
		&WriteBytes,
		WriteFlag,
		&mWriteOverlapped.Overlapped,
		NULL);

	if (ReturnValue == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING && WSAGetLastError() != WSAEWOULDBLOCK)
	{
		End();

		return FALSE;
	}

	return TRUE;
}

BOOL CNetworkSession::Connect(LPSTR address, USHORT port)
{
	CThreadSync Sync;

	if (!address || port <= 0)
		return FALSE;

	if (!mSocket)
		return FALSE;

	SOCKADDR_IN RemoteAddressInfo;

	RemoteAddressInfo.sin_family	= AF_INET;
	RemoteAddressInfo.sin_port = htons(port);
	RemoteAddressInfo.sin_addr.S_un.S_addr = inet_addr(address);

	if (WSAConnect(mSocket, (LPSOCKADDR)&RemoteAddressInfo, sizeof(SOCKADDR_IN), NULL, NULL, NULL, NULL) == SOCKET_ERROR)
	{
		if (WSAGetLastError() != WSAEWOULDBLOCK)
		{
			End();

			return FALSE;
		}
	}

	return TRUE;
}

BOOL CNetworkSession::UdpBind(USHORT port)
{
	CThreadSync Sync;

	if (mSocket)
		return FALSE;

	SOCKADDR_IN RemoteAddressInfo;

	RemoteAddressInfo.sin_family			= AF_INET;
	RemoteAddressInfo.sin_port				= htons(port);
	RemoteAddressInfo.sin_addr.S_un.S_addr	= htonl(INADDR_ANY);

	mSocket = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);

	if (mSocket == INVALID_SOCKET)
		return FALSE;

	if (bind(mSocket, (struct sockaddr*)&RemoteAddressInfo, sizeof(SOCKADDR_IN)) == SOCKET_ERROR)
	{
		End();

		return FALSE;
	}

	mReliableUdpThreadDestroyEvent = CreateEvent(0, FALSE, FALSE, 0);
	if (mReliableUdpThreadDestroyEvent == NULL)
	{
		End();

		return FALSE;
	}

	mReliableUdpThreadWakeUpEvent = CreateEvent(0, FALSE, FALSE, 0);
	if (mReliableUdpThreadWakeUpEvent == NULL)
	{
		End();

		return FALSE;
	}

	mReliableUdpWriteCompleteEvent = CreateEvent(0, FALSE, FALSE, 0);
	if (mReliableUdpWriteCompleteEvent == NULL)
	{
		End();

		return FALSE;
	}

	DWORD ReliableUdpThreadID = 0;
	mReliableUdpThreadHandle = CreateThread(NULL, 0, ::ReliableUdpThreadCallback, this, 0, &ReliableUdpThreadID);

	WaitForSingleObject(mReliableUdpThreadStartupEvent, INFINITE);

	return TRUE;
}

BOOL CNetworkSession::TcpBind(VOID)
{
	CThreadSync Sync;

	if (mSocket)
		return FALSE;

	mSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);

	if (mSocket == INVALID_SOCKET)
		return FALSE;

	return TRUE;
}

BOOL CNetworkSession::GetLocalIP(WCHAR* pIP)
{
	CThreadSync Sync;

	if (!mSocket)
		return FALSE;

	CHAR	Name[256] = { 0, };
	gethostname(Name, sizeof(Name));

	PHOSTENT host = gethostbyname(Name);
	if (host)
	{
		if (MultiByteToWideChar(CP_ACP, 0, inet_ntoa(*(struct in_addr*)*host->h_addr_list), -1, pIP, 32) > 0)
			return TRUE;
	}

	return FALSE;
}

USHORT CNetworkSession::GetLocalPort(VOID)
{
	CThreadSync Sync;

	if (!mSocket)
		return 0;

	SOCKADDR_IN Addr;
	ZeroMemory(&Addr, sizeof(Addr));
	
	INT AddrLength = sizeof(Addr);
	if (getsockname(mSocket, (sockaddr*)&Addr, &AddrLength) != SOCKET_ERROR)
		return ntohs(Addr.sin_port);

	return 0;
}

BOOL CNetworkSession::InitializeReadFromForIocp(VOID)
{
	CThreadSync Sync;

	if (!mSocket)
		return FALSE;

	WSABUF	WsaBuf;
	DWORD	ReadBytes				= 0;
	DWORD	ReadFlag				= 0;
	INT		RemoteAddressInfoSize	= sizeof(mUdpRemoteInfo);

	WsaBuf.buf	= (CHAR*)mReadBuffer;
	WsaBuf.len	= MAX_BUFFER_LENGTH;

	INT		ReturnValue = WSARecvFrom(mSocket,
		&WsaBuf,
		1,
		&ReadBytes,
		&ReadFlag,
		(SOCKADDR*)&mUdpRemoteInfo,
		&RemoteAddressInfoSize,
		&mReadOverlapped.Overlapped,
		NULL);

	if (ReturnValue == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING && WSAGetLastError() != WSAEWOULDBLOCK)
	{
		End();
	
		return FALSE;
	}

	return TRUE;
}

BOOL CNetworkSession::ReadFromForIocp(LPSTR remoteAddress, USHORT& remotePort, BYTE* data, DWORD& dataLength)
{
	CThreadSync Sync;

	if (!mSocket)
		return FALSE;

	if (!data || dataLength <= 0)
		return FALSE;

	memcpy(data, mReadBuffer, dataLength);

	strcpy(remoteAddress, inet_ntoa(mUdpRemoteInfo.sin_addr));
	remotePort = ntohs(mUdpRemoteInfo.sin_port);

	USHORT Ack = 0;
	memcpy(&Ack, mReadBuffer, sizeof(USHORT));

	if (Ack == 9999)
	{
		SetEvent(mReliableUdpWriteCompleteEvent);

		return FALSE;
	}
	else
	{
		Ack = 9999;
		WriteTo2(remoteAddress, remotePort, (BYTE*)&Ack, sizeof(USHORT));
	}

	return TRUE;
}

BOOL CNetworkSession::ReadFromForEventSelect(LPSTR remoteAddress, USHORT& remotePort, BYTE* data, DWORD& dataLength)
{
	CThreadSync	Sync;

	if (!mSocket)
		return FALSE;
	
	if (!data)
		return FALSE;

	if (!mSocket) // 이게 똑같은 mSocket 체크를 2번 씩 하시는데.. 의도된 것인가? 다른 것도 이러던데.. 실수 아닌지 확인해보자.
		return FALSE;

	WSABUF	WsaBuf;
	DWORD	ReadBytes				= 0;
	DWORD	ReadFlag				= 0;
	INT		RemoteAddressInfoSize	= sizeof(mUdpRemoteInfo);

	WsaBuf.buf = (CHAR*)mReadBuffer;
	WsaBuf.len = MAX_BUFFER_LENGTH;

	INT ReturnValue = WSARecvFrom(mSocket,
		&WsaBuf,
		1,
		&ReadBytes,
		&ReadFlag,
		(SOCKADDR*)&mUdpRemoteInfo,
		&RemoteAddressInfoSize,
		&mReadOverlapped.Overlapped,
		NULL);

	if (ReturnValue == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING && WSAGetLastError() != WSAEWOULDBLOCK)
	{
		End();

		return FALSE;
	}

	memcpy(data, mReadBuffer, ReadBytes);
	dataLength = ReadBytes;

	strcpy(remoteAddress, inet_ntoa(mUdpRemoteInfo.sin_addr));
	remotePort = ntohs(mUdpRemoteInfo.sin_port);

	USHORT Ack = 0;
	memcpy(&Ack, mReadBuffer, sizeof(USHORT));

	if (Ack == 9999)
	{
		SetEvent(mReliableUdpWriteCompleteEvent);

		return FALSE;
	}
	else
	{
		Ack = 9999;
		WriteTo2(remoteAddress, remotePort, (BYTE*)&Ack, sizeof(USHORT));
	}

	return TRUE;
}

BOOL CNetworkSession::WriteTo(LPCSTR remoteAddress, USHORT remotePort, BYTE* data, DWORD dataLength)
{
	CThreadSync Sync;

	if (!mSocket)
		return FALSE;

	if (!remoteAddress || remotePort <= 0 || !data || dataLength <= 0)
		return FALSE;

	if (!mReliableWriteQueue.Push(this, data, dataLength, remoteAddress, remotePort))
		return FALSE;

	if (!mIsReliableUdpSending)
	{
		mIsReliableUdpSending = TRUE;
		SetEvent(mReliableUdpThreadWakeUpEvent);
	}

	return TRUE;
}

BOOL CNetworkSession::WriteTo2(LPSTR remoteAddress, USHORT remotePort, BYTE* data, DWORD dataLength)
{
	CThreadSync Sync;

	if (!mSocket)
		return FALSE;

	if (!remoteAddress || remotePort <= 0 || !data || dataLength <= 0)
		return FALSE;

	WSABUF		WsaBuf;
	DWORD		WriteBytes				= 0;
	DWORD		WriteFlag				= 0;

	SOCKADDR_IN RemoteAddressInfo;
	INT			RemoteAddressInfoSize	= sizeof(RemoteAddressInfo);
	
	WsaBuf.buf							= (CHAR*)data;
	WsaBuf.len							= dataLength;

	RemoteAddressInfo.sin_family			= AF_INET;
	RemoteAddressInfo.sin_addr.S_un.S_addr	= inet_addr(remoteAddress);
	RemoteAddressInfo.sin_port				= htons(remotePort);

	INT			ReturnValue = WSASendTo(mSocket,
		&WsaBuf,
		1,
		&WriteBytes,
		WriteFlag,
		(SOCKADDR*)&RemoteAddressInfo,
		RemoteAddressInfoSize,
		&mWriteOverlapped.Overlapped,
		NULL);

	if (ReturnValue == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING && WSAGetLastError() != WSAEWOULDBLOCK)
	{
		End();

		return FALSE;
	}

	return TRUE;
}

BOOL CNetworkSession::GetRemoteAddressAfterAccept(LPTSTR remoteAddress, USHORT& remotePort)
{
	CThreadSync Sync;

	if (!remoteAddress)
		return FALSE;

	sockaddr_in*	Local			= NULL;
	INT				LocalLength		= 0;

	sockaddr_in*	Remote			= NULL;
	INT				RemoteLength	= 0;

	GetAcceptExSockaddrs(mReadBuffer,
		0,
		sizeof(sockaddr_in) + 16,
		sizeof(sockaddr_in) + 16,
		(sockaddr**)&Local,
		&LocalLength,
		(sockaddr**)&Remote,
		&RemoteLength);

	CHAR TempRemoteAddress[32] = { 0, };
	strcpy(TempRemoteAddress, inet_ntoa(Remote->sin_addr));

	MultiByteToWideChar(CP_ACP,
		0,
		TempRemoteAddress,
		-1,
		remoteAddress,
		32);

	remotePort = ntohs(Remote->sin_port);

	return TRUE;
}

SOCKET CNetworkSession::GetSocket(VOID)
{
	CThreadSync Sync;

	return mSocket;
}