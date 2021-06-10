#pragma once
#include<WinSock2.h>
#include<Windows.h> // <- ���⿡ winsock.h �� ���ԵǾ� �ִ�. �̷� ���� �ռ��� ��ġ�� �� �������� WinSock2.h �� ���� include �϶�� ���� �ִ�.
#include<tchar.h>
#include<fstream>
#include<pchannel.h>
#include<assert.h>
#include<minidumpapiset.h>
#include<mswsock.h>

enum IO_TYPE
{
	IO_ACCEPT,
	IO_READ,
	IO_WRITE
};

typedef struct _OVERLAPPED_EX
{
	OVERLAPPED	Overlapped;
	IO_TYPE		IoType;
	VOID* Object;
} OVERLAPPED_EX;

#define MAX_BUFFER_LENGTH	4096

#ifdef _SERVER_SIDE
#define MAX_QUEUE_LENGTH	50  // TODO: ������ �� �δ��� Ŀ�� �׷���? �� 50���� �ϼ�����..
#else
#define MAX_QUEUE_LENGTH	500
#endif

