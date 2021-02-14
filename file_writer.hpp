#pragma once

#include <Windows.h>
#include <vector>
#include <iostream>
using namespace std;

// These are defines because they are shared
#define EVENT_DATA_READY      1
#define EVENT_STOP_WRITING    0
#define MAX_OVERLAPPED_WRITES 8
#define MAX_PAGE_SIZE (1024 * 1024)


class FileWriter
{
public:
	void add(float i, float v, uint8_t bits)
	{
		++m_idx;
		if (m_idx % 10000 == 0)
		{
			cout << "BLEEP " << m_idx << "\n";
		}
	}
private:
	size_t m_idx = 0;
	float m_pages[MAX_OVERLAPPED_WRITES][MAX_PAGE_SIZE];
	HANDLE m_events[MAX_OVERLAPPED_WRITES + 2];
	HANDLE m_stop_event;
	HANDLE m_file_handle;
	OVERLAPPED m_ov[MAX_OVERLAPPED_WRITES];
	size_t m_head;
	size_t m_tail;
	size_t m_ptr;
};
