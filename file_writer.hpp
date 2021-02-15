#pragma once

#include <Windows.h>
#include <vector>
#include <iostream>

using namespace std;

// These are defines because they are shared
#define EVENT_DATA_READY      1
#define EVENT_STOP_WRITING    0
#define MAX_OVERLAPPED_WRITES 8 // don't change; using mask in save_acc()
#define MAX_PAGE_SIZE (64 * 1024) // in floats

class FileWriter
{
public:
	void add(float i, float v, uint8_t bits);
	void open(string fn);
	void close(void);
	void wait(DWORD msec);
private:
	unsigned m_idx = 0;
	float m_acc = 0;
	size_t m_total_accumulated = 0;
	size_t m_samples_per_downsample = 1;
	unsigned m_head;
	unsigned m_tail;
	float m_pages[MAX_OVERLAPPED_WRITES][MAX_PAGE_SIZE];
	void save_acc(void);
	void queue_page(unsigned page, unsigned len);
	HANDLE m_events[MAX_OVERLAPPED_WRITES];
	HANDLE m_file_handle;
	OVERLAPPED m_ov[MAX_OVERLAPPED_WRITES];
	uint64_t m_file_offset = 0;
};
