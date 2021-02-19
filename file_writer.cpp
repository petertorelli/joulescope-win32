#include "file_writer.hpp"

#include <iostream>

#if 1
#	define DBG(x) { cout << x << endl; }
#else
#	define DBG(x) {}
#endif

/**
 * Add a calibrated i/v sample to the raw buffer as energy. Downsample as
 * needed by using the accumulator. When the downsample interval elapses,
 * call save_acc().
 * 
 * Also check to see if the GPIO IN0 changed (falling).
 */
void
FileWriter::add(float i, float v, uint8_t bits)
{
	float e = (float)((double)i * (double)v / 2.0f);
	m_acc += e;
	++m_total_accumulated;
	if (m_total_accumulated == m_samples_per_downsample)
	{
		++m_total_samples;
		m_total_accumulated = 0;
		save_acc();
		m_acc = 0;
	}
	gpi0_check(m_last_gpi0, ((bits >> 4) & 1) == 1);
}

/**
 * If the GPIO IN0 generated a falling edge, capture the approximate time.
 * The vector is written out on close.
 */
void
FileWriter::gpi0_check(bool& last, bool current)
{
	float timestamp;
	// packed bits : 7 : 6 = 0, 5 = voltage_lsb, 4 = current_lsb, 3 : 0 = i_range
	if (last && !current)
	{
		if (m_observe_timestamps == true) {
			timestamp = (float)m_total_samples / m_sample_rate;
			m_timestamps.push_back(timestamp);
			cout << "m-lap-us-" << (unsigned int)(timestamp * 1e6) << endl;
		}
	}
	last = current;
}

/**
 * Create a new file and write out the prologue. Also, act like a constructor
 * and reset some key variables.
 */
void
FileWriter::open(string fn)
{
	m_file_handle = CreateFileA(
		fn.c_str(),
		GENERIC_WRITE,
		0,
		NULL,
		CREATE_ALWAYS,
		FILE_FLAG_OVERLAPPED,
		NULL);
	if (m_file_handle == INVALID_HANDLE_VALUE)
	{
		DBG("Unable to create FileWriter file handle");
		throw runtime_error("Unable to create FileWriter file handle");
	}
	m_file_offset = 0;
	m_total_samples = 0;
	m_total_nan = 0;
	m_total_accumulated = 0;
	m_acc = 0;
	m_buffer_pos = 0;
	m_head = 0;
	m_tail = 0;
	//assert(2'000'000 % m_sample_rate == 0);
	m_samples_per_downsample = 2'000'000u / m_sample_rate;
	m_timestamps.clear();
	// Write the file header
	uint8_t bytes[5];
	union {
		float f;
		uint32_t dw;
	} pun;
	pun.f = (float)m_sample_rate;
	bytes[0] = 0xf1; // Version byte TODO: sync with framework
	CopyMemory(&bytes[1], &pun.dw, sizeof(uint32_t));
	queue_bytes(&bytes, sizeof(bytes));
	wait(5000);
}

/**
 * Write out partial data if any, and close the file.
 */
void
FileWriter::close(void)
{
	// Write partial buffer and set signal to exit on completion w/timeout.
	if (m_buffer_pos)
	{
		queue_page(m_head, m_buffer_pos);
		wait(5000);
	}
	CloseHandle(m_file_handle);
}

/**
 * Store the current accumulator in the correct page / offset. If we've filled
 * a page, queue it for a write and move to a new one.
 */
void
FileWriter::save_acc(void)
{
	unsigned saved_head;
	unsigned saved_len;
	m_pages[m_head][m_buffer_pos] = m_acc;
	if (isnan(m_acc))
	{
		++m_total_nan;
	}
	++m_buffer_pos;
	if (m_buffer_pos == MAX_PAGE_SIZE)
	{
		// Using 'saved' so we can update m_head ASAP before queue_page
		saved_head = m_head;
		m_head = (m_head + 1) & 0x7;
		saved_len = m_buffer_pos;
		m_buffer_pos = 0;
		if (m_head == m_tail)
		{
			DBG("Ring-buffer exhausted");
			throw runtime_error("Ring-buffer exhausted");
		}
		queue_page(saved_head, saved_len);
	}
}

void
FileWriter::queue_page(unsigned page, unsigned len)
{
	cout << "Writing page " << page << endl;
	ZeroMemory(&m_ov[page], sizeof(OVERLAPPED));
	m_ov[page].hEvent = m_events[QUEUE_PAGE_EVENT];
	m_ov[page].OffsetHigh = (m_file_offset >> 32) & 0xFFFF'FFFF;
	m_ov[page].Offset = m_file_offset & 0xFFFF'FFFF;
	if (!WriteFile(
		m_file_handle,
		(LPCVOID)m_pages[page],
		len * sizeof(float),
		NULL,
		(LPOVERLAPPED)&m_ov[page]))
	{
		DWORD err = GetLastError();
		if (err != ERROR_IO_PENDING)
		{
			DBG("Failed to write queue_page");
			throw runtime_error("Failed to write queue_page");
		}
	}
	m_file_offset += len * sizeof(float);
}

void
FileWriter::queue_bytes(LPCVOID bytes, unsigned len)
{
	ZeroMemory(&m_overlapped, sizeof(OVERLAPPED));
	m_overlapped.hEvent = m_events[QUEUE_BYTES_EVENT];
	m_overlapped.OffsetHigh = (m_file_offset >> 32) & 0xFFFF'FFFF;
	m_overlapped.Offset = m_file_offset & 0xFFFF'FFFF;
	if (!WriteFile(
		m_file_handle,
		bytes,
		len,
		NULL,
		(LPOVERLAPPED)&m_overlapped))
	{
		DWORD err = GetLastError();
		if (err != ERROR_IO_PENDING)
		{
			DBG("Failed to write queue_bytes");
			throw runtime_error("Failed to write queue_bytes");
		}
	}
	m_file_offset += len;
}

void
FileWriter::wait(DWORD msec)
{
	DWORD ret = WaitForMultipleObjects(2, m_events, FALSE, msec);
	if (ret < MAXIMUM_WAIT_OBJECTS)
	{
		DWORD event = ret - WAIT_OBJECT_0;
		switch (event) {
		case QUEUE_PAGE_EVENT:
			m_tail = (m_tail + 1) & 0x7;
			ResetEvent(m_events[event]);
			break;
		case QUEUE_BYTES_EVENT:
			ResetEvent(m_events[event]);
			break;
		default:
			DBG("Unexpected wait object");
			throw runtime_error("Unexpected wait object");
			break;
		}
	}
	else if (ret != WAIT_TIMEOUT)
	{
		DBG("Wait failed");
		throw runtime_error("Wait failed");
	}
}
