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
#define VERSION "1.4.0"

/**
 * TODO
 * 1. wide strings
 * 2. pass a reference for m_data vectors rather than a copy
 * 3. fix the m_overlapped buffer grow/shrink issue
 */
using namespace std;
using namespace std::filesystem;
using namespace boost;

Joulescope    g_joulescope;
RawProcessor  g_raw_processor;
FileWriter    g_file_writer;
RawBuffer     g_raw_buffer;

path          g_tmpdir(".");
path          g_pfx("js110");
const path    g_sfx_energy("-energy.bin");
const path    g_sfx_timestamps("-timestamps.json");
bool          g_spinning(false);
bool          g_waiting_on_user(false);
bool          g_observe_timestamps(false);
bool          g_updates(false);
HANDLE        g_hspin(NULL);
float		  g_drop_thresh(0.1f);
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

#define NUM_FILE_BUFS 8
#define NUM_E_SAMPLES (1024u * 1024u)

float g_file_queue[NUM_FILE_BUFS][NUM_E_SAMPLES];
size_t g_head_file_buf = 0;
size_t g_tail_file_buf = 0;
size_t g_cur_e_pos = 0;
HANDLE h_file_mutex(NULL);


void
spin(void)
{
	cout << "ENTERING SPIN\n";
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
		// Connect up all our pieces!
		// The RawBuffer is connected to the Joulescope with Joulescope::trace_on();
		g_joulescope.open(path.c_str());
		g_raw_buffer.set_raw_processor(&g_raw_processor);
		g_raw_processor.calibration_set(g_joulescope.m_calibration);
		g_raw_processor.set_writer(&g_file_writer);

		wcout << "m-[Opened Joulescope at path " << path << "]" << endl;
	}
	// A bit of a hack b/c JS110 LibUSB/WinUSB can be erratic.
	if (tokens.size() == 3) {
		g_drop_thresh = stof(tokens[2]);
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
	// Seems awkward to do this here.
	path fn = g_pfx;
	fn += g_sfx_energy;
	path fp = g_tmpdir / fn;
	g_joulescope.power_on(true);
	g_joulescope.streaming_on(true, &g_raw_buffer);
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
	// Required by the framework
	cout
		<< "m-regfile-fn["
		<< g_pfx.string()
		<< g_sfx_energy.string()
		<< "]-type[emon]-name[js110]"
		<< endl;
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
			// Always print this on trace start so we detect any cheating.
			cout << "m-dropthresh[" << std::setprecision(3) << g_drop_thresh << "]" << endl;
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
		int rate = stoi(tokens[1]);

		if ((rate < 1) || (MAX_SAMPLE_RATE % rate) != 0)
		{
			cout << "e-[Sample rate must be a factor of 1'000'000]" << endl;
		}
		else
		{
			//g_stats.set_samplerate(stoi(tokens[1]));
		}
	}
//	cout << "m-rate-hz[" << g_stats.m_sample_rate << "]" << endl;
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
