#include "device.hpp"

using namespace std;

template<typename ... Args>
string string_format(const string& format, Args ... args)
{
	size_t size = snprintf(nullptr, 0, format.c_str(), args ...);
	size += 1; // Extra space for '\0'
	if (size <= 0)
	{
		throw runtime_error("Error during formatting.");
	}
	unique_ptr<char[]> buf(new char[size]);
	snprintf(buf.get(), size, format.c_str(), args ...);
	return string(buf.get(), buf.get() + size - 1); // We don't want the '\0' inside
}

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
	string message((LPSTR)lpMsgBuf);
	LocalFree(lpMsgBuf);
	return message;
}

EndpointIn::EndpointIn(
	HANDLE _winusb,
	UCHAR _pipe_id,
	UINT _transfers,
	UINT _block_size,
	EndpointIn_data_fn_t data_fn,
	EndpointIn_process_fn_t process_fn,
	EndpointIn_stop_fn_t stop_fn
)
{
	m_winusb = _winusb;
	m_pipe_id = _pipe_id;
	m_overlapped_free.clear();
	m_overlapped_pending.clear();
	m_transfers = _transfers;
	m_transfer_size = (UINT)floor(((double)((size_t)_block_size + BULK_IN_LENGTH - 1) / (double)BULK_IN_LENGTH)) * BULK_IN_LENGTH;
	m_data_fn = data_fn;
	m_process_fn = process_fn;
	m_stop_fn = stop_fn;
	m_process_transfers = 0;
	m_state = state_e::ST_IDLE;
	m_stop_code = DeviceEvent::NONE; // python uses None and enum & getlasterror!
	m_stop_message = "";
	m_byte_count_this = 0;
	m_byte_count_total = 0;
	m_transfer_count = 0;
	m_transfer_expire_max = 0;
	m_event = NULL;
}

void
EndpointIn::_open(void)
{
	DBG("EndpointIn::_open()");
	DBG("EndpointIN::_open() ... this=" << this << "");
	m_stop_code = DeviceEvent::NONE;
	m_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	DBG("EndpointIn::_open CreateEvent(event=" << m_event << ")");
	if (m_event == NULL)
	{
		throw runtime_error("count not create event");
	}
	for (size_t i(0); i < m_transfers; ++i)
	{
		DBG("EndpointIn::_open() ... adding TransferOverlapped #" << i << "");
		m_overlapped_free.push_back(new TransferOverlapped(m_event, m_transfer_size));
	}
}

void
EndpointIn::_close(void)
{
	DBG("EndpointIn::_close()");
	if (m_event != NULL)
	{
		DBG("EndpointIn::_close() ... CloseHandle(event=" << m_event << ")");
		CloseHandle(m_event);
	}
#ifdef ENDPOINT_PERFSTATS
	print_stats_to_console();
#endif
	m_event = NULL;
}

// true = error, false = ok
bool
EndpointIn::_issue(TransferOverlapped* ov)
{
	DBG("EndpointIn::_issue()");
	BOOL result;
	ov->reset();
	// QUESTION: why don't we use LengthTransferred here? (arg 5)
	DBG("EndpointIn::_issue() ... Calling WinUsb_ReadPipe(pipe_id=" << (int)m_pipe_id << ")");
	result = WinUsb_ReadPipe(m_winusb, m_pipe_id, ov->m_buffer.data(), (ULONG)ov->m_buffer.size(), NULL, ov->ov_ptr());
	DBG("EndpointIn::_issue() ... result       =" << result << ")");
	if (!result)
	{
		if (GetLastError() != ERROR_IO_PENDING)
		{
			DBG("EndpointIn::_issue() ... Read failed and error isn't ERROR_IO_PENDING!");
			string msg = string_format("EndpointIn %02x issue failed: %s", m_pipe_id, GetLastErrorText());
			m_overlapped_free.push_back(ov);
			_halt(DeviceEvent::COMMUNICATION_ERROR, msg);
			return true;
		}
	}
	DBG("EndpointIn::_issue() ... pushing onto overlapped_pending queue (size is currently " << m_overlapped_pending.size() << ")");
	m_overlapped_pending.push_back(ov);
	DBG("EndpointIn::_issue() ... PUSHED onto overlapped_pending queue (size is currently " << m_overlapped_pending.size() << ")");
	return false;
}

bool
EndpointIn::_pend(void)
{
	DBG("EndpointIn::_pend()");
	DBG("EndpointIn::_pend() ... overlapped_free queue has " << m_overlapped_free.size() << " entries");
	while (!m_overlapped_free.empty())
	{
		TransferOverlapped *ov = m_overlapped_free.front();
		m_overlapped_free.pop_front();
		if (_issue(ov))
		{
			DBG("EndpointIn::_pend() ... last issue failed, return 'true'");
			return true;
		}
	}
	DBG("EndpointIn::_pend() ... all good, return 'true'");
	return false;
}

bool
EndpointIn::_expire(void)
{
	DBG("EndpointIn::_expire()");
	bool rv(false);
	DWORD length_transferred(0);
	UINT count(0);

	DBG("EndpointIn::_expire() ... entering big loop at line " << __LINE__ << "");
	DBG("EndpointIn::_expire() ... # of pending overlapped =" << m_overlapped_pending.size() << "");
	DBG("EndpointIn::_expire() ... # of -free-- overlapped =" << m_overlapped_free.size() << "");
	fflush(stdout);
	while (!rv && !m_overlapped_pending.empty())
	{
		TransferOverlapped *ov = m_overlapped_pending.front();
		if (WinUsb_GetOverlappedResult(m_winusb, ov->ov_ptr(), &length_transferred, FALSE))
		{
			DBG("EndpointIn::_expire() ... success!");
			DBG("EndpointIn::_expire() ... remove the pending event");
			m_overlapped_pending.pop_front(); // in the Python code, he reassigns ov here TODO?
			++m_transfer_count;
			ULONG length = length_transferred; // seems a little redundant
			m_byte_count_this += length;
			++count;
			if (m_data_fn != nullptr)
			{
				if (length > ov->m_buffer.size())
				{
					throw runtime_error("EndpointIn::_expire() ... transferred bytes exceed storage buffer size");
				}
				vector<UCHAR> slice(ov->m_buffer.begin(), ov->m_buffer.begin() + length);
				try
				{
#ifdef ENDPOINT_PERFSTATS
					std::chrono::high_resolution_clock::time_point a = std::chrono::high_resolution_clock::now();
					rv = m_data_fn(slice);
					std::chrono::high_resolution_clock::time_point b = std::chrono::high_resolution_clock::now();
					auto delta = b - a;
					float nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(delta).count() / 1e9;
					m_perf_stats.data_fn_time.push_back(nsec);
#else
					rv = m_data_fn(slice);
#endif
				}
				catch (...)
				{
					throw runtime_error("EndpointIn::_expire() ... exception in data function");
					rv = true;
				}
			}
			if (rv)
			{
				string msg = string_format("EndpointIn %02x terminated by data_fn", m_pipe_id);
				_halt(DeviceEvent::ENDPOINT_CALLBACK_STOP, msg);
				m_overlapped_free.push_back(ov);
			}
			else
			{
				DBG("EndpointIn::_expire() ... issuing the TransferOverlapped ... a copy or actual? does it matter?");
				rv = _issue(ov);
			}
		}
		else
		{
			DWORD ec = GetLastError();
			if (ec == ERROR_IO_INCOMPLETE || ec == ERROR_IO_PENDING)
			{
				DBG("EndpointIn::_expire() ... fail but event is PENDING or INCOMPLETE");
				break;
			}
			DBG("EndpointIn::_expire() ... BAD FAIL");
			m_overlapped_pending.pop_front(); // reassigns ov here ? TODO
			m_overlapped_free.push_back(ov);
			string msg = string_format("EndpointIn WinUsb_GetOverlappedResult fatal: %08x", ec);
			LOG(msg);
			rv = true;
			DBG("EndpointIn::_expire() ... calling halt....");
			_halt(DeviceEvent::COMMUNICATION_ERROR, msg);
		}
	} // end overlapped pending drain while()
	if (count > m_transfer_expire_max)
	{
		m_transfer_expire_max = count;
	}
	m_process_transfers += count;
	return rv;
}

void
EndpointIn::_cancel(void)
{
	DBG("EndpointIn::_cancel()");
	ULONG length_transferred = 0;
	if (!WinUsb_AbortPipe(m_winusb, m_pipe_id))
	{
		LOG("WinUsb_AbortPipe pipe_id " << (int)m_pipe_id << ": " << GetLastErrorText());
	}
	while (m_overlapped_pending.size() > 0)
	{
		TransferOverlapped *ov = m_overlapped_pending.front();
		m_overlapped_pending.pop_front();
		if (!WinUsb_GetOverlappedResult(m_winusb, ov->ov_ptr(), &length_transferred, TRUE))
		{
			if (GetLastError() != ERROR_OPERATION_ABORTED)
			{
				LOG("cancel overlapped: " << GetLastErrorText());
			}
			// skipped a few other error types here that were debug vs warning
		}
		m_overlapped_free.push_back(ov);
	}
}

void
EndpointIn::_halt(DeviceEvent stop_code, string msg = "")
{
	DBG("EndpointIn::_halt()");
	if (m_state != state_e::ST_STOPPING)
	{
		m_state = state_e::ST_STOPPING;
		_cancel();
	}
	// python is "if stop_code" which is true for None, so i think he means UNDEFINED which is forced to 0 in enum
	if (stop_code != DeviceEvent::UNDEFINED)
	{
		// removed logging level stuff, all same level for now
		if (m_stop_code == DeviceEvent::NONE)
		{
			m_stop_code = stop_code; // which is weird because this can now be None in python, but we stopped
			m_stop_message = msg; // will be '' if default, which is our None
			LOG("endpoint halt [x] " << (int)stop_code << ": " << msg);
		}
		else
		{
			LOG("endpoint halt [y] " << (int)stop_code << " duplicate: " << msg);
		}
	}
}

bool
EndpointIn::process(void)
{
	DBG("EndpointIn::process()");
	bool rv;
	if (m_state != state_e::ST_RUNNING)
	{
		// Fake python None type; None > 0 == False
		return (m_stop_code == DeviceEvent::NONE) ? false : ((int)m_stop_code > 0); // warning enum to int!
	}
	rv = _expire();
	if (!rv)
	{
		rv = _pend();
	}
	return rv;
}

bool
EndpointIn::process_signal(void)
{
	DBG("EndpointIn::process_signal()");
	if (m_process_transfers > 0)
	{
		m_process_transfers = 0;
		try
		{
			if (m_process_fn != nullptr)
			{
#ifdef ENDPOINT_PERFSTATS
				std::chrono::high_resolution_clock::time_point a = std::chrono::high_resolution_clock::now();
				bool rv = m_process_fn();
				std::chrono::high_resolution_clock::time_point b = std::chrono::high_resolution_clock::now();
				auto delta = b - a;
				float nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(delta).count() / 1e9;
				m_perf_stats.process_fn_time.push_back(nsec);
				return rv;
#else
				return m_process_fn();
#endif
			}
		}
		catch (...)
		{
			LOG("m_process_fn excption: stop streaming");
			return true;
		}
	}
	return false;
}

void
EndpointIn::start(void)
{
	DBG("EndpointIn::start()");
	DBG("EndpointIn::start() pipe_id=" << (int)m_pipe_id << " transfer_size=" << m_transfer_size << " bytes");
	DBG("EndpointIn::start() ... call _open()");
	_open();
	m_state = state_e::ST_RUNNING;
	m_process_transfers = 0;
	DBG("EndpointIn::start() ... call _pend()");
	_pend();
	DBG("EndpointIn::start() ... done");
}

// TODO all "catch" should log to cout!

void
EndpointIn::stop(void)
{
	DBG("EndpointIn::stop()");
	if (m_state != state_e::ST_IDLE)
	{
		if (m_state != state_e::ST_STOPPING)
		{
			_cancel();
		}
		if (m_stop_code == DeviceEvent::NONE)
		{
			m_stop_code = DeviceEvent::UNDEFINED; // aka 0, org code mixes enums and ints
			process_signal();
		}
		_close();
		try
		{
			if (m_stop_fn != nullptr)
			{
				m_stop_fn((int)m_stop_code, m_stop_message);
			}
		}
		catch (...)
		{
			LOG("_stop_fn exception");
		}
		m_state = state_e::ST_IDLE;
	}
}

ControlTransferAsync::ControlTransferAsync(HANDLE _winusb)
{
	m_winusb = _winusb;
	m_overlapped = nullptr;
	m_event = NULL;
	m_commands.clear();
	m_time_start = 0;
	m_stop_code = DeviceEvent::NONE;
}

HANDLE
ControlTransferAsync::event(void)
{
	return m_event;
}

void
ControlTransferAsync::open(void)
{
	DBG("ControlTransferAsync::open()");
	m_stop_code = DeviceEvent::NONE;
	m_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	DBG("ControlTransferAsync::open CreateEvent(event=" << m_event << ")");
	if (m_event == NULL)
	{
		throw runtime_error("Could not create control event");
	}
	m_overlapped = new TransferOverlapped(m_event, 4096); // magic #, make #define
}

void
ControlTransferAsync::close(void)
{
	DBG("ControlTransferAsync::close()");
	deque<ControlTransferAsync_Command> commands(m_commands);
	m_commands.clear();
	size_t commands_len = commands.size(); //sic
	if (!commands.empty())
	{
		/**
		 * Q: What is this first pop for?
		 * From Matt: "The first command is "special". It is actually pending in the
		 * WinUSB driver. The remainder are just in our local queue. We abort the
		 * remainder immediately by calling the callback (without issuing to WinUSB)."
		 */
		DBG("ControlTransferAsync::close() ... clearing FIRST command in queue");
		ControlTransferAsync_Command command(commands.front());
		commands.pop_front();
		_finish(command);
	}
	while (!commands.empty())
	{
		DBG("ControlTransferAsync::close() ... clearing NEXT command in queue");
		ControlTransferAsync_cbk_fn cbk_fn = commands.front().cbk_fn;
		WINUSB_SETUP_PACKET setup_packet = commands.front().setup_packet;
		commands.pop_front();
		cbk_fn(ControlTransferResponse(setup_packet, DeviceEvent::UNDEFINED, vector<UCHAR>()));
	}
	/**
	 * Q: Why aren't we finishing?
	 * From Matt: "If commands are outstanding, do not close the handle immediately.
	 * Otherwise close on the last command in _close_event."
	 */
	if (commands_len == 0)
	{
		if (m_event)
		{
			DBG("ControlTransferAsync::close() ... closing event (event=" << m_event << ")");
			CloseHandle(m_event);
			m_event = NULL;
		}
		if (m_overlapped != nullptr)
		{
			delete m_overlapped;
			m_overlapped = nullptr;
		}
	} 
	else
	{
		DBG("ControlTransferAsync::close() ... ALERT not closing the handle");
	}
}

void
ControlTransferAsync::_close_event(void)
{
	DBG("ControlTransferAsync::_close_event()");
	if (m_event)
	{
		DBG("ControlTransferAsync::_close_event() ... is this bad? (event=" << m_event << ")");
		CloseHandle(m_event);
		m_event = NULL;
		// should this be outside this "IF"?
		if (m_overlapped != nullptr)
		{
			delete m_overlapped;
			m_overlapped = nullptr; // if we zero this, we need to reopen!
		}
	}
}

bool
ControlTransferAsync::pend(
	ControlTransferAsync_cbk_fn cbk_fn,
	WINUSB_SETUP_PACKET         setup_packet = WINUSB_SETUP_PACKET(),
	vector<UCHAR>               buffer = vector<UCHAR>())
{
	DBG("ControlTransferAsync::pend()");
	if (m_stop_code != DeviceEvent::NONE)
	{
		DBG("ControlTransferAsync::pend() ... we have a stop code (m_stop_code=" << (int)m_stop_code << ")");
		ControlTransferResponse response(setup_packet, m_stop_code, vector<UCHAR>());
		DBG("ControlTransferAsync::pend() ... stop-code callback path");
		cbk_fn(response);
		DBG("ControlTransferAsync::pend() ... stop-code return 'false'");
		return false;
	}
	DBG("ControlTransferAsync::pend() ... no stop code, proceed...");
	ControlTransferAsync_Command command(cbk_fn, setup_packet, buffer);
	bool was_empty = m_commands.empty();
	DBG("ControlTransferAsync::pend() ... adding new command to queue...");
	m_commands.push_back(command);
	if (was_empty)
	{
		DBG("ControlTransferAsync::pend() ... que was empty so calling ::_issue()");
		return _issue();
	}
	DBG("ControlTransferAsync::pend() ... return 'true'");
	return true;
}

bool
ControlTransferAsync::_issue(void)
{
	DBG("ControlTransferAsync::_issue()");
	if (m_commands.empty())
	{
		DBG("ControlTransferAsync::_issue() ... no commands in the queue, return");
		return true;
	}
	DBG("ControlTransferAsync::_issue() ... there are " << m_commands.size() << " commands in the queue");
	ControlTransferAsync_cbk_fn cbk_fn = m_commands.front().cbk_fn;
	WINUSB_SETUP_PACKET         setup_packet = m_commands.front().setup_packet;
	vector<UCHAR>          buffer = m_commands.front().buffer;
	m_overlapped->reset();
	if (USB_ENDPOINT_DIRECTION_OUT(setup_packet.RequestType))
	{
		if (setup_packet.Length > 0)
		{
			DBG("ControlTransferAsync::_issue() ... OUT transaction setup_packet.Length=" << setup_packet.Length << "");
			m_overlapped->m_buffer = buffer;
		}
	}
	DBG("ControlTransferAsync::_issue() ... calling WinUsb_ControlTransfer(event=" << m_overlapped->m_event << ")");
	BOOL winres = WinUsb_ControlTransfer(
		m_winusb,
		setup_packet,
		m_overlapped->m_buffer.data(),
		setup_packet.Length,
		NULL,
		m_overlapped->ov_ptr()
	);
	DWORD dwResult;
	// sanitize_boolean_return_code() only used once here, and overrides enum for stop_code
	if (winres == TRUE)
	{
		DBG("ControlTransferAsync::_issue() ... success");
		dwResult = 0;
	}
	else
	{
		dwResult = GetLastError();
		if (dwResult == ERROR_IO_PENDING)
		{
			DBG("ControlTransferAsync::_issue() ... failed but IO is pending, so it's OK!");
		}
		else
		{
			DBG("ControlTransferAsync::_issue() ... failed: " << GetLastErrorText());
		}
	}
	m_time_start = time(nullptr);
	if (dwResult != ERROR_IO_PENDING)
	{
		DBG("ControlTransferAsync::_issue() ... no error pending, something else went wrong!");
		DBG("ControlTransferAsync::_issue() ... stop_code=" << (int)m_stop_code << "");
		if (m_stop_code == DeviceEvent::NONE)
		{
			m_stop_code = DeviceEvent::COMMUNICATION_ERROR;
			DBG("ControlTransferAsync::_issue() ... stop_code is none, setting to COMMUNICATION_ERROR (" << (int)m_stop_code << ")");
		}
		DBG("ControlTransferAsync::_issue() ... cbk_fn()");
		cbk_fn(ControlTransferResponse(setup_packet, DeviceEvent::FORCE_CAST_FROM_GETLASTERROR_BUG, vector<UCHAR>()));
		DBG("ControlTransferAsync::_issue() ... done, return 'false'");
		return false;
	}
	DBG("ControlTransferAsync::_issue() ... done, return 'true'");
	return true;
}

void
ControlTransferAsync::_finish(ControlTransferAsync_Command command)
{
	DBG("ControlTransferAsync::_finish()");
	ControlTransferAsync_cbk_fn cbk_fn = command.cbk_fn;
	WINUSB_SETUP_PACKET         setup_packet = command.setup_packet;
	vector<UCHAR>          buffer; // it is None in python, but we just use an empty one here
	ULONG                       length_transferred;

	DBG("ControlTransferAsync::_finish() ... calling WinUsb_GetOverlappedResults()");
	BOOL rc = WinUsb_GetOverlappedResult(
		m_winusb,
		m_overlapped->ov_ptr(),
		&length_transferred,
		TRUE
	);
	if (rc == FALSE)
	{
		DBG("ControlTransferAsync::_finish() ... failed, check GetLastError() ...");
		DWORD ec = GetLastError();
		if ((ec != ERROR_IO_INCOMPLETE) && (ec != ERROR_IO_PENDING))
		{
			DBG("ControlTransferAsync::_finish() ... not incomplete and not pending... reset our event: " << GetLastErrorText());
			ResetEvent(m_event);
		}
		DBG("ControlTransferAsync::_finish() ... error = " << ec << ", complete, pending or something else...");
	}
	else
	{
		DBG("ControlTransferAsync::_finish() ... succeeded ...");
		DBG("ControlTransferAsync::_finish() ... ResetEvent(" << m_event << ") ...");
		ResetEvent(m_event);
		rc = FALSE;
		time_t duration(time(nullptr) - m_time_start);
		DBG("ControlTransferAsync::_finish() ... duration=" << duration << " seconds");
		if (USB_ENDPOINT_DIRECTION_IN(setup_packet.RequestType) && setup_packet.Length > 0)
		{
			if (length_transferred > m_overlapped->m_buffer.size())
			{
				throw runtime_error("ControlTransferAsync::_finish() transferred size exceeds buffer!");
			}
			DBG("ControlTransferAsync::_finish() ... INCOMING setup_packet.Length=" << setup_packet.Length << ", length_transferred=" << length_transferred << " B");
			buffer = m_overlapped->m_buffer;
		}
	}
	ControlTransferResponse response(setup_packet, DeviceEvent::FORCE_CAST_FROM_GETLASTERROR_BUG, buffer);
	DBG("ControlTransferAsync::_finish() ... calling cbk_fn");
	cbk_fn(response);
}

void
ControlTransferAsync::process(void)
{
	DBG("ControlTransferAsync::process()");
	if (m_commands.empty())
	{
		DBG("ControlTransferAsync::process() ... no commands in the queue");
		return;
	}
	DBG("ControlTransferAsync::process() ... waiting for object (#cmds=" << m_commands.size() << ")");
	DWORD rc = WaitForSingleObject(m_event, 0);
	if (rc == WAIT_OBJECT_0) // transfer done
	{
		DBG("ControlTransferAsync::process() ... transfer finished (WAIT_OBJECT_0)");
		ControlTransferAsync_Command command = m_commands.front();
		m_commands.pop_front();
		DBG("ControlTransferAsync::process() ... finish command");
		_finish(command);
		if (m_stop_code == DeviceEvent::NONE)
		{
			DBG("ControlTransferAsync::process() ... no error, call _issue to see what's next");
			_issue();
		}
		else
		{
			DBG("ControlTransferAsync::process() ... stop_code was not none, close event (huh?)");
			_close_event();
		}
	}
	else if (rc == WAIT_TIMEOUT)
	{
		// not ready yet
		DBG("ControlTransferAsync::process() ... WAIT_TIMEOUT");
	}
	else
	{
		DBG("ControlTransferAsync::process() ... still waiting (rc=" << rc << ")");
	}
}

#define _event_append(event) \
{ \
	if (event != NULL) \
	{ \
		DBG("WinUsbDevice::_event_append(event=" << event << ")"); \
		m_event_list[m_event_list_count] = event; \
		m_event_list_count += 1; \
	} \
}

void
WinUsbDevice::_update_event_list(void)
{
	DBG("WinUsbDevice::_update_event_list()");
	m_event_list_count = 0;
	//_event_append(m_event);
	if (m_control_transfer != nullptr)
	{
		_event_append(m_control_transfer->event())
	}
	EndpointInMap::iterator it;
	for (it = m_endpoints.begin(); it != m_endpoints.end(); it++)
	{
		_event_append(it->second.event());
	}
}

#define CONTROL_TIMEOUT 1
void
WinUsbDevice::open(wstring _path, event_callback_fn_t* event_callback_fn)
{
	m_path = _path;
	DBG("WinUsbDevice::open() - pre close");
	close();
	DBG("WinUsbDevice::open() - main open");
	m_event_callback_fn = event_callback_fn;

	try
	{
		m_file = CreateFile(
			m_path.c_str(),
			GENERIC_WRITE | GENERIC_READ,
			FILE_SHARE_WRITE | FILE_SHARE_READ,
			NULL,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
			NULL);
		if (m_file == INVALID_HANDLE_VALUE)
		{
			throw runtime_error("Open failed on invalid handle");
		}
		if (!WinUsb_Initialize(m_file, &m_winusb))
		{
			throw runtime_error("Open failed"); // get last error
		}
		m_control_transfer = new ControlTransferAsync(m_winusb);
		m_control_transfer->open();
		DWORD timeout = CONTROL_TIMEOUT * 1000;
		BOOL result = WinUsb_SetPipePolicy(
			m_winusb,
			0,
			PIPE_TRANSFER_TIMEOUT,
			sizeof(timeout),
			&timeout
		);
		if (!result)
		{
			LOG("WinUsb_SetPipePolicy: " << GetLastErrorText());
		}
		_update_event_list();
	}
	catch (...)
	{
		close();
		throw runtime_error("rethrow from WinUsbDevice.open");
	}
}

void
WinUsbDevice::close(void)
{
	DBG("WinUsbDevice::close()");
	EndpointInMap::iterator it;
	for (it = m_endpoints.begin(); it != m_endpoints.end(); it++)
	{
		it->second.stop();
	}
	m_endpoints.clear();
	if (m_control_transfer != nullptr)
	{
		m_control_transfer->close();
		delete m_control_transfer;
		m_control_transfer = nullptr;
	}
	if (m_file != NULL)
	{
		WinUsb_Free(m_winusb);
		m_winusb = NULL;
		CloseHandle(m_file);
		m_file = NULL;
	}
	m_interface = 0;
	/*
	if (m_event)
	{
		CloseHandle(m_event);
		m_event = NULL;
	}
	*/
	m_event_callback_fn = nullptr;
}

class ControlTransferSynchronizer
{
public:
	ControlTransferSynchronizer(time_t _timeout)
	{
		m_timeout = _timeout;
		m_done = false;
		m_time_start = time(nullptr);
	}
	bool isDone(void)
	{
		if (m_done)
		{
			return true;
		}
		time_t time_delta(time(nullptr) - m_time_start);
		if (time_delta > m_timeout)
		{
			throw runtime_error("ControlTransferSynchronizer() ... timeout");
		}
		return false;
	}
	void callback(ControlTransferResponse ctr)
	{
		if (ctr.setup_packet.Length <= ctr.data.size())
		{
			m_data = vector<UCHAR>(ctr.data.begin(), ctr.data.begin() + ctr.setup_packet.Length);
		}
		m_done = true;
	}
	vector<UCHAR> response(void)
	{
		return m_data;
	}
private:
	bool          m_done;
	vector<UCHAR> m_data;
	time_t        m_time_start;
	time_t        m_timeout;
};

bool
WinUsbDevice::control_transfer_out_sync(
	UCHAR Recipient,
	UCHAR Type,
	UCHAR Request,
	UINT Value,
	UINT Index,
	vector<UCHAR> data)
{
	DBG("WinUsbDevice::control_transfer_out_sync()");
	ControlTransferSynchronizer sync(1);
	ControlTransferAsync_cbk_fn lambda = [&](ControlTransferResponse ctr)
	{
		sync.callback(ctr);
	};
	this->control_transfer_out(lambda, Recipient, Type, Request, Value, Index, data);
	while (!sync.isDone())
	{
		process(0.01f);
	}
	return false;
}

vector<UCHAR>
WinUsbDevice::control_transfer_in_sync(
	UCHAR Recipient,
	UCHAR Type,
	UCHAR Request,
	UINT Value,
	UINT Index,
	USHORT Length)
{
	DBG("WinUsbDevice::control_transfer_in_sync()");
	ControlTransferSynchronizer sync(1);
	ControlTransferAsync_cbk_fn lambda = [&](ControlTransferResponse ctr)
	{
		sync.callback(ctr);
	};
	this->control_transfer_in(lambda, Recipient, Type, Request, Value, Index, Length);
	while (!sync.isDone())
	{
		process(0.01f);
	}
	return sync.response();
}


bool
WinUsbDevice::control_transfer_out(
	ControlTransferAsync_cbk_fn cbk_fn,
	UCHAR Recipient,
	UCHAR Type,
	UCHAR Request,
	UINT Value,
	UINT Index,
	vector<UCHAR> data)
{
	BM_REQUEST_TYPE RequestType;
	RequestType.B = 0; // clear it first!
	RequestType.s.Dir = BMREQUEST_HOST_TO_DEVICE;
	RequestType.s.Type = Type;
	RequestType.s.Recipient = Recipient;
	WINUSB_SETUP_PACKET pkt;
	pkt.RequestType = RequestType.B;
	pkt.Request = Request;
	pkt.Value = Value;
	pkt.Index = Index;
	pkt.Length = (USHORT)data.size();
	DBG("WinUsbDevice::control_transfer_out() ... pend our packet (RequestType.B=" << (int)RequestType.B << ")");
	if (m_control_transfer == nullptr)
	{
		throw runtime_error("WinUsbDevice::control_transfer_out() ... WinUsbDevice is not open");
	}
	return m_control_transfer->pend(cbk_fn, pkt, data);
}

bool
WinUsbDevice::control_transfer_in(
	ControlTransferAsync_cbk_fn cbk_fn,
	UCHAR Recipient,
	UCHAR Type,
	UCHAR Request,
	UINT Value,
	UINT Index,
	USHORT Length)
{
	DBG("WinUsbDevice::control_transfer_in");
	BM_REQUEST_TYPE RequestType;
	RequestType.B = 0; // clear it first!
	RequestType.s.Dir = BMREQUEST_DEVICE_TO_HOST;
	RequestType.s.Type = Type;
	RequestType.s.Recipient = Recipient;
	WINUSB_SETUP_PACKET pkt;
	pkt.RequestType = RequestType.B;
	pkt.Request = Request;
	pkt.Value = Value;
	pkt.Index = Index;
	pkt.Length = Length;
	DBG("WinUsbDevice::control_transfer_in() ... pend our packet (RequestType.B=" << (int)RequestType.B << ")");
	if (m_control_transfer == nullptr)
	{
		throw runtime_error("WinUsbDevice::control_transfer_in() ... WinUsbDevice is not open");
	}
	return m_control_transfer->pend(cbk_fn, pkt);
}

void
WinUsbDevice::read_stream_start(
	UCHAR endpoint_id,
	UINT transfers,
	UINT block_size,
	EndpointIn_data_fn_t data_fn,
	EndpointIn_process_fn_t process_fn,
	EndpointIn_stop_fn_t stop_fn
)
{
	DBG("WinUsbDevice::read_stream_start(endpoint_id=" << (int)endpoint_id << ")");
	UCHAR pipe_id = (endpoint_id & 0x7f) | 0x80;
	EndpointInMap::iterator itr = m_endpoints.find(pipe_id);
	// don't pop yet, it invalidates the iterator
	if (itr != m_endpoints.end())
	{
		DBG("WinUsbDevice::read_stream_start() ... repeated start because endpoint found, stopping & removing it");
		itr->second.stop();
		m_endpoints.erase(itr);
	}
	DBG("WinUsbDevice::read_stream_start() ... creating & inserting endpoint");
	EndpointIn endpoint(m_winusb, pipe_id, transfers, block_size, data_fn, process_fn, stop_fn);
	m_endpoints.insert(make_pair(pipe_id, endpoint));
	//BUGBUG: the pair above is a COPY!
	//endpoint.start(); <- so we can't do this. heh.
	// and this complains m_endpoints[pipe_id].start();
	itr = m_endpoints.find(pipe_id);
	DBG("WinUsbDevice::read_stream_start() ... starting our new endpoint...");
	itr->second.start();
	DBG("WinUsbDevice::read_stream_start() ... returned from endpoint start, updating event list (event handle=" << itr->second.event() << ")");
	_update_event_list();
}

void
WinUsbDevice::read_stream_stop(UCHAR endpoint_id)
{
	DBG("WinUsbDevice::read_stream_stop(endpoint_id=" << (int)endpoint_id << ")");
	UCHAR pipe_id = (endpoint_id & 0x7f) | 0x80;
	EndpointInMap::iterator itr = m_endpoints.find(pipe_id);
	// don't pop yet, it invalidates the iterator
	if (itr != m_endpoints.end())
	{
		DBG("WinUsbDevice::read_stream_stop stopping endpoint");
		itr->second.stop();
		m_endpoints.erase(itr);
		_update_event_list();
	}
}

void
WinUsbDevice::_abort(int stop_code, string msg)
{
	DBG("WinUsbDevice::_abort(stop_code=" << stop_code << ", msg=" << msg << ")");
	EndpointInMap::iterator it;
	for (it = m_endpoints.begin(); it != m_endpoints.end(); it++)
	{
		it->second.stop();
	}
	m_endpoints.clear();
	if (m_control_transfer->m_stop_code == DeviceEvent::NONE)
	{
		m_control_transfer->m_stop_code = DeviceEvent::ENDPOINT_CALLBACK_STOP;
	}
	_update_event_list();
	event_callback_fn_t* event_callback_fn = m_event_callback_fn;
	m_event_callback_fn = nullptr;
	if (event_callback_fn != nullptr)
	{
		try
		{
			(*event_callback_fn)(stop_code, msg);
		}
		catch (...)
		{
			LOG("exception in _event_callback_fn");
		}
	}
}

void
WinUsbDevice::process(float timeout)
{
	DBG("WinUsbDevice::process(" << timeout << ")");
	DWORD timeout_ms = (DWORD)(timeout * 1000.0f);
	DWORD rv = WaitForMultipleObjects(m_event_list_count, m_event_list.data(), FALSE, timeout_ms);
	DBG("WinUsbDevice::process() rv = " << rv);
	if (rv < MAXIMUM_WAIT_OBJECTS)
	{
		//ResetEvent(m_event);
		vector<UCHAR> stop_endpoint_ids;
		DBG("WinUsbDevice::process() ... calling process() on " << m_endpoints.size() << " endpoints");
		for (EndpointInMap::iterator it = m_endpoints.begin(); it != m_endpoints.end(); it++)
		{
			DBG("WinUsbDevice::process() ... process endpoints (obj=" << (void*)&(it->second) << ")");
			if (it->second.process())
			{
				stop_endpoint_ids.push_back(it->second.m_pipe_id);
			}
		}
		DBG("WinUsbDevice::process() ... calling process_signal() on " << m_endpoints.size() << " endpoints");
		for (EndpointInMap::iterator it = m_endpoints.begin(); it != m_endpoints.end(); it++)
		{
			DBG("WinUsbDevice::process() ... process_signal endpoints");
			if (it->second.process_signal() || (it->second.m_stop_code != DeviceEvent::NONE))
			{
				stop_endpoint_ids.push_back(it->second.m_pipe_id);
			}
		}
		UCHAR pipe_id = 0; // needed for the last conditional below
		DBG("WinUsbDevice::process() ... calling end() on " << stop_endpoint_ids.size() << " stopped endpoint IDs");
		for (size_t i(0); i < stop_endpoint_ids.size(); i++)
		{
			DBG("WinUsbDevice::process() ... end stopped ids");
			pipe_id = stop_endpoint_ids[i];
			EndpointInMap::iterator itr = m_endpoints.find(pipe_id);
			// don't pop yet, it invalidates the iterator
			if (itr == m_endpoints.end())
			{
				continue;
			}
			EndpointIn e(itr->second);
			m_endpoints.erase(itr);
			e.stop();
			string msg = string_format("Endpoint pipe_id %02x stopped: %02x", pipe_id, (UCHAR)e.m_stop_code);
			LOG(msg);
			if ((int)e.m_stop_code > 0)
			{
				// recasting from enum to dword
				_abort((int)e.m_stop_code, msg);
			}
		}
		DBG("WinUsbDevice::process() ... process our control transfer");
		if (m_control_transfer != nullptr) // Jetperch:pyjoulescope HEAD f19c30e
		{
			m_control_transfer->process();
			if ((m_control_transfer->m_stop_code != DeviceEvent::NONE) && ((int)m_control_transfer->m_stop_code > 0))
			{
				// pipe_id is None (or 0 in C++ here)
				string msg = string_format("Control pipe %02x stopped: %02x", pipe_id, m_control_transfer->m_stop_code);
				LOG(msg);
				_abort((int)m_control_transfer->m_stop_code, msg);
			}
		}
	}
}

