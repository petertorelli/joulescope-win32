#include "main.hpp"
#define VERSION "0.9.0"
#define PYJOULESCOPE_GITHUB_HEAD "97b9e90"

/**
 * TODO
 * 1. wide strings
 * 2. optimize m_data / buffer passing
 * 3. fix the m_overlapped buffer grow/shrink issue
 */
using namespace std;
using namespace std::filesystem;

path          g_tmpdir(".");
const path    g_power_fn = "downsampled.raw";
const path    g_etime_fn = "timestamps-emon.json";
fstream       g_trace_file;
bool          g_spinning(false);
HANDLE        g_hspin = NULL;
RawProcessor  g_raw_processor;
Joulescope    g_joulescope;
TraceStats    g_stats;
CommandTable  g_commands = {
	std::make_pair("exit", cmd_exit),
	std::make_pair("init", cmd_init),
	std::make_pair("deinit", cmd_deinit),
	std::make_pair("power-on", cmd_power_on),
	std::make_pair("power-off", cmd_power_off),
	std::make_pair("start-trace", cmd_start_trace),
	std::make_pair("stop-trace", cmd_stop_trace),
	std::make_pair("samplerate", cmd_samplerate),
	std::make_pair("debug", cmd_debug),
	//TODO std::make_pair("help", cmd_help)
};

inline void
gpi0_check(bool& last, bool current)
{
	// packed bits : 7 : 6 = 0, 5 = voltage_lsb, 4 = current_lsb, 3 : 0 = i_range
	if (last && !current)
	{
		float timestamp = (float)g_stats.m_total_samples / (float)g_stats.m_sample_rate;
		g_stats.m_timestamps.push_back(timestamp);
		cout << "m-lap-us-" << (unsigned int)(timestamp * 1e6) << endl;
	}
	last = current;
}

inline void
heartbeat(void)
{
	if ((g_stats.m_total_samples % g_stats.m_sample_rate) == 0)
	{
		cout << "Total samples " << g_stats.m_total_samples;
		cout << " # dropped packets " << g_stats.m_total_dropped_pkts;
		cout << " [ # NaN=" << g_stats.m_total_nan;
		cout << ", # inf=" << g_stats.m_total_inf;
		cout << " ]";
		cout << endl;
	}
}

inline void
transcendental_check(float f)
{
	if (isnan(f))
	{
		++g_stats.m_total_nan;
	}
	if (isinf(f))
	{
		++g_stats.m_total_inf;
	}
}

void
process_sample_callback_raw(void* lpuser, float I, float V, uint8_t bits)
{
	double E = ((double)I * (double)V) / 2.0; 
	float Ef = (float)E;
	transcendental_check(Ef);
	// TODO would a buffer be faster than byte writes?
	g_trace_file.write(reinterpret_cast<const char*>(&Ef), sizeof(float));
	++g_stats.m_total_samples;
	heartbeat();
	gpi0_check(g_stats.m_last_gpi0, ((bits >> 4) & 1) == 1);
}

void
process_sample_callback_downsampled(void* lpuser, float I, float V, uint8_t bits)
{
	double E = ((double)I * (double)V) / 2.0;
	float Ef = (float)E;
	transcendental_check(Ef);
	g_stats.m_accumulator += E;
	++g_stats.m_total_accumulated;
	if (g_stats.m_total_accumulated == g_stats.m_total_downsamples)
	{
		++g_stats.m_total_samples;
		// TODO would a buffer be faster than byte writes?
		float x = (float)g_stats.m_accumulator;
		g_trace_file.write(reinterpret_cast<const char*>(&x), sizeof(float));
		g_stats.m_total_accumulated = 0;
		g_stats.m_accumulator = 0;
		heartbeat();
	}
	gpi0_check(g_stats.m_last_gpi0, ((bits >> 4) & 1) == 1);
}

void
process_packet(JoulescopePacket* pkt)
{
	/*
	cout << "Joulescope Packet" << endl;
	cout << "  buffer_type = " << (int)pkt->buffer_type << endl;
	cout << "  status = " << (int)pkt->status << endl;
	cout << "  length = " << pkt->length << endl;
	cout << "  pkt_index = " << pkt->pkt_index << endl;
	cout << "  usb_frame_index = " << pkt->usb_frame_index << endl;
	*/
	uint16_t delta = pkt->pkt_index - g_stats.m_last_pkt_index;
	if (delta > 1)
	{
		g_stats.m_total_dropped_pkts += delta;
	}
	g_stats.m_last_pkt_index = pkt->pkt_index;
	for (size_t i(0); i < 126; ++i) // TODO Magic #
	{
		g_raw_processor.process(pkt->samples[i].current, pkt->samples[i].voltage);
	}
}

//TODO this should be a reference otherwie we're copying lots of bytes
bool
data_callback(vector<UCHAR> data)
{
	JoulescopePacket* pkts = (JoulescopePacket*)data.data();
	size_t num_pkts = data.size() / 512;
	while (num_pkts--)
	{
		process_packet(pkts++);
	}
	return false;
}

void
spin(void)
{
	try
	{
		while (g_spinning == true)
		{
			g_joulescope.m_device.process(1);
			/**
			 * Note that in the Python, there is a command queue here that
			 * avoids races when issuing control transfers while streaming.
			 */
		}
	}
	catch (runtime_error re)
	{
		cout << "e-[Thread runtime error: " << re.what() << "]" << endl;
	}
	catch (...)
	{
		cout << "e-[Unknown exception in thread]" << endl;
	}
}

void
write_timestamps(void)
{
	// TODO: since we already use JsonCpp, why not use that?
	// Always write this file, even if no timestamps
	path fn = g_tmpdir / g_etime_fn;
	fstream file;
	file.open(fn, ios::out);
	cout << "# timestamps " << g_stats.m_timestamps.size() << endl;
	file << "[" << endl;
	for (size_t i(0); i < g_stats.m_timestamps.size(); ++i)
	{
		file << "\t" << g_stats.m_timestamps[i];
		if (i < (g_stats.m_timestamps.size() - 1))
		{
			file << ",";
		}
		file << endl;
	}
	file << "]" << endl;
	file.close();
	// Required by the framework
	wcout << "FileRegister:name(" << wstring(g_etime_fn.c_str()) << "), class(etime), type(js110)" << endl;
}

void
cmd_exit(vector<string> tokens)
{
	if (g_spinning)
	{
		cmd_stop_trace(vector<string>());
	}
	g_joulescope.close();
	g_trace_file.close();
	// Required to let the host know the exit was OK
	cout << "m-exit" << endl;
	exit(0);
}

void
cmd_init(vector<string> tokens)
{
	if (g_spinning)
	{
		cout << "e-[Cannot talk to Joulescipe while streaming]" << endl;
		return;
	}
	if (g_joulescope.is_open())
	{
		cout << "e-[A joulescope is already initialized, deinit first]" << endl;
		return;
	}
	string serial = tokens.size() < 2 ? "" : tokens[1];
	wstring path = g_joulescope.find_joulescope_by_serial_number(serial);
	if (path.empty())
	{
		if (tokens.size() < 2)
		{
			cout << "e-[No Joulescopes found]" << endl;
		}
		else
		{
			cout << "e-[Could not find a Joulescope with serial #" << serial << "]" << endl;
		}
	}
	else
	{
		g_joulescope.open(path.c_str());
		wcout << "m-[Opened Joulescope at path " << path << "]" << endl;
	}
}

void
cmd_deinit(vector<string> tokens)
{
	if (g_spinning)
	{
		cout << "e-[Cannot talk to Joulescipe while streaming]" << endl;
		return;
	}
	if (g_joulescope.is_open())
	{
		g_joulescope.close();
	}
	else
	{
		cout << "e-[No Joulescopes are open]" << endl;
	}
}

void
cmd_power_on(vector<string> tokens)
{
	if (g_spinning)
	{
		cout << "e-[Cannot talk to Joulescipe while streaming]" << endl;
		return;
	}
	if (g_joulescope.is_open())
	{
		g_joulescope.power_on(true);
		cout << "m-[Power on]" << endl;
	}
	else
	{
		cout << "e-[No Joulescopes are open]" << endl;
	}
}

void
cmd_power_off(vector<string> tokens)
{
	if (g_spinning)
	{
		cout << "e-[Cannot talk to Joulescipe while streaming]" << endl;
		return;
	}
	if (g_joulescope.is_open())
	{
		g_joulescope.power_on(false);
		cout << "m-[Power off]" << endl;
	}
	else
	{
		cout << "e-[No Joulescopes are open]" << endl;
	}
}

void
cmd_start_trace(vector<string> tokens)
{
	if (g_spinning)
	{
		cout << "e-[Trace is already running]" << endl;
		return;
	}
	if (!g_joulescope.is_open())
	{
		cout << "e-[No Joulescopes are open]" << endl;
		return;
	}
	// TODO: Wondering if the raw processor is faster than sample_rate = 1'000'000
	if (g_stats.m_sample_rate == MAX_SAMPLE_RATE)
	{
		g_raw_processor.callback_set(process_sample_callback_raw, nullptr);
	}
	else
	{
		g_raw_processor.callback_set(process_sample_callback_downsampled, nullptr);
	}
	// Seems awkward to do this here.
	g_raw_processor.calibration_set(g_joulescope.m_calibration);

	g_tmpdir = tokens.size() < 2 ? "." : tokens[1];
	path fn = g_tmpdir / g_power_fn;
	g_trace_file.open(fn, ios::binary | ios::out);
	// Required by the framework
	wcout << "FileRegister:name(" << wstring(g_power_fn.c_str()) << "), class(emon), type(js110)" << endl;
	union {
		float f;
		uint8_t b[4];
	} x;
	x.f = (float)g_stats.m_sample_rate;
	uint8_t version = 0xf1;
	g_trace_file.write((char*)&version, 1);
	g_trace_file.write((char*)x.b, 4);
	g_stats.reset();
	g_joulescope.power_on(true);
	g_joulescope.streaming_on(true, data_callback);
	g_spinning = true;
	g_hspin = CreateThread(
		NULL,
		0,
		(LPTHREAD_START_ROUTINE)spin,
		NULL,
		0,
		NULL);
	if (g_hspin == NULL)
	{
		throw runtime_error("main::cmd_start_trace() ... Failed to CreateThread");
	}
	cout << "m-[Trace started]" << endl;
}

void
cmd_stop_trace(vector<string> tokens)
{
	if (g_spinning == false)
	{
		cout << "e-[Trace isn't running]" << endl;
		return;
	}
	if (!g_joulescope.is_open())
	{
		cout << "e-[No Joulescopes are open]" << endl;
		return;
	}
	g_spinning = false;
	DWORD rv = WaitForSingleObject(g_hspin, 10000);
	if (rv != WAIT_OBJECT_0)
	{
		throw runtime_error("main::cmd_stop_trace() ... Trace thread failed to exit");
	}
	g_joulescope.streaming_on(false);
	g_trace_file.close();
	write_timestamps();
	cout << "m-[Trace stopped]" << endl;
}

void
cmd_samplerate(vector<string> tokens)
{
	if (g_spinning)
	{
		cout << "e-[Cannot change sample rate while tracing]" << endl;
		return;
	}
	if (tokens.size() > 1)
	{
		g_stats.reset();
		g_stats.set_samplerate(stoi(tokens[1]));
	}
	cout << "m-samplerate[" << g_stats.m_sample_rate << "]" << endl;
}

int
main(int argc, char* argv[])
{
	string line;
	
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	cout << "Joulescope(R) JS110 Win32 Driver" << endl;
	cout << "Version : " << VERSION << endl;
	cout << "Head    : " << PYJOULESCOPE_GITHUB_HEAD << endl;
	try {
		cout << "m-ready" << endl;
		while (getline(cin, line))
		{
			vector<string> tokens;
			boost::trim(line);
			boost::split(tokens, line, boost::is_any_of(" \t"));
			if (!tokens.empty() && !tokens[0].empty())
			{
				CommandTable::iterator itr = g_commands.find(tokens[0]);
				if (itr == g_commands.end())
				{
					cout << "e-[Unknown command: " << line << "]" << endl;
				}
				else
				{
					itr->second(tokens);
				}
			}
			cout << "m-ready" << endl;
		}
	}
	catch (runtime_error re)
	{
		cout << "e-[main() exception: " << re.what() << "]" << endl;
	}
	catch (...)
	{
		cout << "e-[Unknown exception in main()]" << endl;
	}
	// We should never exit via this path, only via cmd_exit()
	cout << "e-[Unexpected exit]" << endl;
	cout << "m-exit" << endl;
	return -1;
}

void
cmd_debug(std::vector<string> tokens)
{
}
