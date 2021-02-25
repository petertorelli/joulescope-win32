#include <Windows.h>
#include <string>

#include <iostream>
using namespace std;

string
GetLastErrorText(void)
{
	LPVOID lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		GetLastError(),
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf,
		0, NULL);
	cout << "GetLastError: " << (LPSTR)lpMsgBuf << endl;
	string message((LPSTR)lpMsgBuf);
	LocalFree(lpMsgBuf);
	return message;
}
