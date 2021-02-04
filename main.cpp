/**
 * Copyright 2021 Peter Torelli
 * Copyright 2020 Jetperch LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "main.hpp"
// Note: Only the device and raw_processor are critical.
#define PYJOULESCOPE_GITHUB_HEAD "6b92e38"
#define VERSION "1.2.0"

/**
 * TODO
 * 1. wide strings
 * 2. pass a reference for m_data vectors rather than a copy
 * 3. fix the m_overlapped buffer grow/shrink issue
 */
using namespace std;
using namespace std::filesystem;
using namespace boost;

path          g_tmpdir(".");
path          g_pfx("js110");
const path    g_sfx_energy("-energy.bin");
const path    g_sfx_timestamps("-timestamps.json");
fstream       g_trace_file;
bool          g_spinning(false);
bool          g_waiting_on_user(false);
bool          g_observe_timestamps(false);
bool          g_updates(false);
HANDLE        g_hspin(NULL);
RawProcessor  g_raw_processor;
Joulescope    g_joulescope;
TraceStats    g_stats;
CommandTable  g_commands = {
	std::make_pair("init",    Command{ cmd_init,    "[serial] Find the first JS110 (or by serial #) and initialize it." }),
	std::make_pair("deinit",  Command{ cmd_deinit,  "De-initialize the current JS110." }),
	std::make_pair("power",   Command{ cmd_power,   "[on|off] Get/set output power state." }),
	std::make_pair("timer",   Command{ cmd_timer,   "[on|off] Get/set timestamping state." }),
	std::make_pair("trace",   Command{ cmd_trace,   "[on path prefix|off] Get/set tracing and save files in 'path/prefix' (quote if 'path' uses spaces)." }),
	std::make_pair("rate",    Command{ cmd_rate,    "Set the sample rate to an integer multiple of 1e6." }),
	std::make_pair("voltage", Command{ cmd_voltage, "Report the internal 2s voltage mean in mv." }),
	std::make_pair("updates", Command{ cmd_updates, "[on|off] Get/set one-second update state." }),
	std::make_pair("exit",    Command{ cmd_exit,    "De-initialize (if necessary) and exit." }),
	std::make_pair("help",    Command{ cmd_help,    "Print this help." }),
};

/*
 * going old school here so there can be no question
 */
 /**
  * The EndpointIn data_fn callback puts raw samples into the g_raw_*
  * buffers. The EndpointIn process_fn reads them out and calls the
  * RawProcessor, putting the resulting float values into another buffer.
  * The raw buffers are flushed by the process_fn function. If process_fn
  * can't respond fast enough, "Buffer overflow" is printed to the console.
  * The RawProcessor callback flushes the float buffer every time it fills.
  * It is up to the function that manages the file to flush the processed
  * sample buffer to disk before closing.
  *
  * Here's the flow:
  *
  * endpoint_data_fn_callback -> get a packet and call ...
  * queue_raw_sample -> queue the raw samples and complain if we overvlow.
  * endpoint_process_fn_callback -> call RawProcessor.process in a loop...
  * raw_processor_callback -> queue a calibrated sample from RawProcessor
  * flush_processed_samples_to_disk --> writes calibrated samples to disk
  */
  // TODO: Should the ratio of buffer sizes depend on the sample rate?
constexpr auto RAW_BUFFER_SIZE(1024 * 128);
constexpr auto CAL_BUFFER_SIZE(1024 * 64);
// Raw data
uint16_t g_raw_i[RAW_BUFFER_SIZE];
uint16_t g_raw_v[RAW_BUFFER_SIZE];
size_t   g_num_buffered_raw(0);
// Processed data
float    g_cal_e[CAL_BUFFER_SIZE];
size_t   g_num_buffered_cal(0);

void
queue_raw_sample(uint16_t _i, uint16_t _v)
{
	g_raw_i[g_num_buffered_raw] = _i;
	g_raw_v[g_num_buffered_raw] = _v;
	++g_num_buffered_raw;
	if (g_num_buffered_raw >= RAW_BUFFER_SIZE)
	{
		cout << "Buffer overflow" << endl;
		g_num_buffered_raw = 0;
	}
}

bool
endpoint_process_fn_callback(void)
{
	for (size_t j(0); j < g_num_buffered_raw; ++j)
	{
		g_raw_processor.process(g_raw_i[j], g_raw_v[j]);
	}
	//cout << "Processed " << g_num_buffered_raw << " samples." << endl;
	g_num_buffered_raw = 0;
	return false;
}

void
flush_processed_samples_to_disk(void)
{
	// sizeof_t(float) had better be 4!
	g_trace_file.write(reinterpret_cast<const char*>(g_cal_e), g_num_buffered_cal * 4);
	//cout << "Wrote " << g_num_buffered_cal << " processed samples to disk." << endl;
	g_num_buffered_cal = 0;
}

inline void
gpi0_check(bool& last, bool current)
{
	float timestamp;
	// packed bits : 7 : 6 = 0, 5 = voltage_lsb, 4 = current_lsb, 3 : 0 = i_range
	if (last && !current)
	{
		if (g_observe_timestamps == true) {
			timestamp = (float)g_stats.m_total_samples / (float)g_stats.m_sample_rate;
			g_stats.m_timestamps.push_back(timestamp);
			cout << "m-lap-us-" << (unsigned int)(timestamp * 1e6) << endl;
		}
	}
	last = current;
}

inline void
heartbeat(void)
{
	static uint64_t prev_dropped = 0;

	// Has one second elapsed?
	if ((g_stats.m_total_samples % g_stats.m_sample_rate) == 0)
	{
		if (g_updates || (g_stats.m_total_dropped_pkts > prev_dropped))
		{
			cout << "Total samples " << g_stats.m_total_samples;
			cout << " # dropped packets " << g_stats.m_total_dropped_pkts;
			cout << " [ # NaN=" << g_stats.m_total_nan;
			cout << ", # inf=" << g_stats.m_total_inf;
			cout << " ]";
			cout << endl;
		}
		prev_dropped = g_stats.m_total_dropped_pkts;
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
raw_processor_callback(void *puser, float I, float V, uint8_t bits)
{
	double E = ((double)I * (double)V) / 2.0;
	float Ef = (float)E;
	transcendental_check(Ef);
	g_stats.m_accumulator += E;
	++g_stats.m_total_accumulated;
	if (g_stats.m_total_accumulated == g_stats.m_total_downsamples)
	{
		++g_stats.m_total_samples;
		g_cal_e[g_num_buffered_cal] = (float)g_stats.m_accumulator;
		++g_num_buffered_cal;
		if (g_num_buffered_cal == CAL_BUFFER_SIZE)
		{
			flush_processed_samples_to_disk(); // resets counter/index
		}
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
		// Packet loss due to USB processing latency
		g_stats.m_total_dropped_pkts += delta;
	}
	g_stats.m_last_pkt_index = pkt->pkt_index;
	for (size_t i(0); i < 126; ++i) // TODO Magic #
	{
		queue_raw_sample(pkt->samples[i].current, pkt->samples[i].voltage);
	}
}

// TODO this should be a reference otherwise we're copying lots of bytes
bool
endpoint_data_fn_callback(vector<UCHAR> data)
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
	// Always write this file, even if no timestamps
	path fn = g_pfx;
	fn += g_sfx_timestamps;
	path fp = g_tmpdir / fn;
	fstream file;
	file.exceptions(std::ifstream::failbit | std::ifstream::badbit);
	file.open(fp, ios::out);
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
	cout
		<< "m-regfile-fn["
		<< g_pfx.string()
		<< g_sfx_timestamps.string()
		<< "]-type[etime]-name[js110]"
		<< endl;
}

void
cmd_help(vector<string> tokens)
{
	for (CommandTable::iterator itr = g_commands.begin(); itr != g_commands.end(); ++itr)
	{
		cout << itr->first << " - " << itr->second.desc << endl;
	}
}

void
cmd_exit(vector<string> tokens)
{
	cmd_deinit(vector<string>());
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
cmd_power(vector<string> tokens)
{
	if (tokens.size() > 1)
	{
		if (g_joulescope.is_open() == false)
		{
			cout << "e-[No Joulescopes are open]" << endl;
			return;
		}
		if (tokens[1] == "on")
		{
			if (g_spinning)
			{
				cout << "e-[Cannot talk to Joulescope while tracing]" << endl;
				return;
			}
			g_joulescope.power_on(true);
		}
		else if (tokens[1] == "off")
		{
			if (g_spinning)
			{
				cout << "e-[Cannot talk to Joulescope while tracing]" << endl;
				return;
			}
			g_joulescope.power_on(false);
		}
		else
		{
			cout << "e-['power' options are 'on' or 'off']" << endl;
			return;
		}
	}
	cout << "m-power[" << (g_joulescope.is_powered() ? "on" : "off") << "]" << endl;
}

void
trace_start()
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
	g_raw_processor.callback_set(raw_processor_callback, nullptr);
	// Seems awkward to do this here.
	g_raw_processor.calibration_set(g_joulescope.m_calibration);
	path fn = g_pfx;
	fn += g_sfx_energy;
	path fp = g_tmpdir / fn;
	g_trace_file.open(fp, ios::binary | ios::out);
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
	g_joulescope.streaming_on(true, endpoint_data_fn_callback, endpoint_process_fn_callback);
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
		throw runtime_error("main::cmd_trace_start() ... Failed to CreateThread");
	}
}

void
trace_stop()
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
		throw runtime_error("main::cmd_trace_stop(): Trace thread failed to exit");
	}
	g_joulescope.streaming_on(false);
	flush_processed_samples_to_disk();
	g_trace_file.close();
	// Required by the framework
	cout
		<< "m-regfile-fn["
		<< g_pfx.string()
		<< g_sfx_energy.string()
		<< "]-type[emon]-name[js110]"
		<< endl;
	write_timestamps();
	double pct = (double)g_stats.m_total_dropped_pkts
		/ (double)g_stats.m_last_pkt_index * 100;
	cout
		<< "Dropped "
		<< g_stats.m_total_dropped_pkts
		<< " packets out of "
		<< g_stats.m_last_pkt_index
		<< ", "
		<< std::setprecision(3) << pct
		<< "%"
		<< endl;
	if (pct > MAX_DROPPED_PACKETS_PCT)
	{
		cout
			<< "e-[Dropped more than "
			<< MAX_DROPPED_PACKETS_PCT
			<< "% of packets]"
			<< endl;
	}
}

void
cmd_trace(vector<string> tokens) {
	if (tokens.size() > 1)
	{
		if (tokens[1] == "on")
		{
			if (tokens.size() != 4)
			{
				cout << "e-['trace on' requires TMPDIR and PFX']" << endl;
				return;
			}
			g_tmpdir = tokens[2];
			g_pfx = tokens[3];
			trace_start();
		}
		else if (tokens[1] == "off")
		{
			trace_stop();
		}
		else
		{
			cout << "e-['trace' options are 'on' or 'off']" << endl;
		}
	}
	cout << "m-trace[" << (g_spinning ? "on" : "off") << "]" << endl;
}

void
cmd_timer(vector<string> tokens)
{
	if (tokens.size() > 1)
	{
		if (tokens[1] == "on")
		{
			g_observe_timestamps = true;
		}
		else if (tokens[1] == "off")
		{
			g_observe_timestamps = false;
		}
		else
		{
			cout << "e-['timer' options are 'on' or 'off']" << endl;
			return;
		}
	}
	cout << "m-timer[" << (g_observe_timestamps ? "on" : "off") << "]" << endl;
}

void
cmd_updates(vector<string> tokens)
{
	if (tokens.size() > 1)
	{
		if (tokens[1] == "on")
		{
			g_updates = true;
		}
		else if (tokens[1] == "off")
		{
			g_updates = false;
		}
		else
		{
			cout << "e-['updates' options are 'on' or 'off']" << endl;
			return;
		}
	}
	cout << "m-updates[" << (g_updates ? "on" : "off") << "]" << endl;
}

void
cmd_rate(vector<string> tokens)
{
	if (g_spinning)
	{
		cout << "e-[Cannot change sample rate while tracing]" << endl;
		return;
	}
	if (tokens.size() > 1)
	{
		g_stats.reset();
		int rate = stoi(tokens[1]);

		if ((rate < 1) || (MAX_SAMPLE_RATE % rate) != 0)
		{
			cout << "e-[Sample rate must be a factor of 1'000'000]" << endl;
		}
		else
		{
			g_stats.set_samplerate(stoi(tokens[1]));
		}
	}
	cout << "m-rate-hz[" << g_stats.m_sample_rate << "]" << endl;
}

void
cmd_voltage(vector<string> tokens)
{
	if (g_spinning)
	{
		cout << "e-[Cannot talk to Joulescope while tracing]" << endl;
		return;
	}
	unsigned int mv = g_joulescope.get_voltage();
	cout << "m-voltage-mv[" << mv << "]" << endl;
}

void
cmd_deinit(vector<string> tokens)
{
	if (g_spinning)
	{
		trace_stop();
	}
	if (g_joulescope.is_open())
	{
		g_joulescope.close();
	}
	if (g_trace_file.is_open())
	{
		g_trace_file.close();
	}
}

void
sigint_handler(int _signal)
{
	static bool cleanup_done(false);
	cout << "e-[Caught signal #" << _signal << "]" << endl;
	// In case of multiple signals (do I need a mutex?)
	if (!cleanup_done)
	{
		cleanup_done = true;
		g_waiting_on_user = false;
		cmd_exit(vector<string>());
	}
}

int
main(int argc, char* argv[])
{
	string line;

	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);
	signal(SIGBREAK, sigint_handler);

	g_trace_file.exceptions(std::ifstream::failbit | std::ifstream::badbit);

	cout << "Joulescope(R) JS110 Win32 Driver" << endl;
	cout << "Version : " << VERSION << endl;
	cout << "Head    : " << PYJOULESCOPE_GITHUB_HEAD << endl;
	g_waiting_on_user = true;
	try {
		vector<string> tokens;
		while (g_waiting_on_user)
		{
			tokens.clear();
			getline(cin, line);
			// CTRL-C causes a fail that needs clearing before calling again
			if (cin.fail() || cin.eof())
			{
				cin.clear();
				continue;
			}
			trim(line);
			tokenizer_t tok(line, delim_t("", " ", "\""));
			for (tokenizer_t::iterator itr = tok.begin(); itr != tok.end(); ++itr) {
				tokens.push_back(*itr);
			}
			if (!tokens.empty() && !tokens[0].empty())
			{
				CommandTable::iterator itr = g_commands.find(tokens[0]);
				if (itr == g_commands.end())
				{
					cout << "e-[Unknown command: " << line << "]" << endl;
				}
				else
				{
					itr->second.func(tokens);
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
	// We should never exit via this path!
	cout << "e-[Unexpected exit]" << endl;
	return -1;
}

void
cmd_debug(std::vector<string> tokens)
{
}
