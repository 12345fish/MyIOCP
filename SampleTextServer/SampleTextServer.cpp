// SampleTextServer.cpp : �������̨Ӧ�ó������ڵ㡣
//

#include "stdafx.h"
#include "MyIOCP.h"


MyIOCP myiocp;

int _tmain(int argc, _TCHAR* argv[])
{
	myiocp.Startup(20000,4,10000);

	getchar();


	myiocp.Shutdown();

	return 0;
}

