#include "file_writer.hpp"

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
		throw runtime_error("Unable to create FileWriter file handle");
	}
	m_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	m_file_offset = 0;
	m_total_samples = 0;
	m_total_accumulated = 0;
	m_buffer_pos = 0;
	m_sample_rate = (2e6f / (float)m_samples_per_downsample);
	// We write 5-byte header which isn't a page, so use a quick hack.
	uint8_t bytes[5];
	bytes[0] = 0xf1; // Version byte TODO: sync with framework
	CopyMemory(&bytes[1], &m_sample_rate, sizeof(float));
	queue_bytes(&bytes, sizeof(bytes));
	wait(10000);
}

void
FileWriter::close(void)
{
	// Write partial buffer and set signal to exit on completion w/timeout.
	if (m_buffer_pos)
	{
		queue_page(m_head, m_buffer_pos);
		wait(10000);
	}
	CloseHandle(m_file_handle);
}

void
FileWriter::save_acc(void)
{
	unsigned saved_head;
	unsigned saved_len;
	m_pages[m_head][m_buffer_pos] = m_acc;
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
			throw runtime_error("Ring-buffer exhausted");
		}
		queue_page(saved_head, saved_len);
	}
}

void
FileWriter::queue_page(unsigned page, unsigned len)
{
	ZeroMemory(&m_ov[page], sizeof(OVERLAPPED));
	m_ov[page].hEvent = m_event;
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
			throw runtime_error("Failed to write queue_page");
		}
	}
	m_file_offset += len * sizeof(float);
}

void
FileWriter::queue_bytes(LPCVOID bytes, unsigned len)
{
	ZeroMemory(&m_overlapped, sizeof(OVERLAPPED));
	m_overlapped.hEvent = m_event;
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
			throw runtime_error("Failed to write queue_bytes");
		}
	}
	m_file_offset += len;
}

void
FileWriter::wait(DWORD msec)
{
	DWORD ret = WaitForSingleObject(m_event, msec);
	if (ret < MAXIMUM_WAIT_OBJECTS)
	{
		ResetEvent(m_event);
		m_tail = (m_tail + 1) & 0x7;
	}
	else if (ret != WAIT_TIMEOUT)
	{
		throw runtime_error("Wait failed");
	}
}

void
FileWriter::write_timestamps(string fn)
{
	fstream file;
	file.exceptions(std::ifstream::failbit | std::ifstream::badbit);
	file.open(fn, ios::out);
	file << "[" << endl;
	for (size_t i(0); i < m_timestamps.size(); ++i)
	{
		file << "\t" << m_timestamps[i];
		if (i < (m_timestamps.size() - 1))
		{
			file << ",";
		}
		file << endl;
	}
	file << "]" << endl;
	file.close();
	// Required by the framework
	cout
		<< "m-regfile-fn["
		<< fn
		<< "]-type[etime]-name[js110]"
		<< endl;
}
