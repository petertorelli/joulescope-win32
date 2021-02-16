/**
 * Copyright 2021 Peter Torelli
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
#define VERSION "1.5.0"

using namespace std;
using namespace std::filesystem;
using namespace boost;

const string EEMBC_EMON_SUFFIX("-energy.bin");
const string EEMBC_TIMESTAMP_SUFFIX("-timestamps.json");

float		 g_drop_thresh(0.1f);

// These are the primary "legos" that build the tracer.
Joulescope   g_joulescope;
RawProcessor g_raw_processor;
FileWriter   g_file_writer;
RawBuffer    g_raw_buffer;
// Per the EEMBC framework, this is a file convention
path         g_tmpdir(".");
path         g_fp_energy(g_tmpdir / string("js110" + EEMBC_EMON_SUFFIX));
path         g_fp_timestamps(g_tmpdir / string("js110" + EEMBC_TIMESTAMP_SUFFIX));
// There are three spinning loops in this code:
bool         g_device_spinning(false); // Device-driver process loop
bool         g_writer_spinning(false); // Async file write tail-pointer incr.
bool         g_userin_spinning(false); // Wait on user input.
HANDLE       g_device_thread(NULL);
HANDLE       g_writer_thread(NULL);
CommandTable g_commands = {
	make_pair("init",    Command{ cmd_init,    "[serial] Find the first JS110 (or by serial #) and initialize it." }),
	make_pair("deinit",  Command{ cmd_deinit,  "De-initialize the current JS110." }),
	make_pair("power",   Command{ cmd_power,   "[on|off] Get/set output power state." }),
	make_pair("timer",   Command{ cmd_timer,   "[on|off] Get/set timestamping state." }),
	make_pair("trace",   Command{ cmd_trace,   "[on path prefix|off] Get/set tracing and save files in 'path/prefix' (quote if 'path' uses spaces)." }),
	make_pair("rate",    Command{ cmd_rate,    "Set the sample rate to an integer multiple of 1e6." }),
	make_pair("voltage", Command{ cmd_voltage, "Report the internal 2s voltage average in mv." }),
	make_pair("exit",    Command{ cmd_exit,    "De-initialize (if necessary) and exit." }),
	make_pair("help",    Command{ cmd_help,    "Print this help." }),
};

/**
 * This thread handles USB endpoint processing in the driver (1 Hz). The
 * function `process()` waits on an array of events and determines if an
 * endpoint returned data or of an asynchronous control transfer completed.
 */
void
device_spin(void)
{
	try
	{
		while (g_device_spinning == true)
		{
			/**
			 * By default the device is configured for eight outstsanding
			 * endoint transfers, each with 256 bulk transfers of 
			 * 512 bytes, or 8*256*512=1MB. The pending overlapped transfers
			 * are stored in a deque<>. However, the RawBuffer used by the
			 * device can overflow as these transfers are deposited into it
			 * by the device `_expire` operation that puts them into the
			 * RawBuffer via `add_slice`. We need to service this routine
			 * at the rate of the RawBuffer, which is 16MB. Since the JS110
			 * can send back 2M samples, or 8MB/sec, we should service this
			 * at least 2x the theoretical max of the RawBuffer.
			 */
			g_joulescope.m_device.process(1000); // milliseconds
		}
	}
	catch (runtime_error re)
	{
		cout << "e-[Device thread runtime error: " << re.what() << "]" << endl;
	}
	catch (...)
	{
		cout << "e-[Unknown exception in device thread]" << endl;
	}
}

/**
 * This thread advances the tail pointer in the file writer ring-buffer. The
 * file writer downsamples raw data sent to it on the `process_signal`
 * endpoint callback (by way of a RawProcessor callback). Every time an entry
 * in the ring buffer fills, it is shipped of to an async WriteFile.
 */
void
writer_spin(void)
{
	try
	{
		while (g_writer_spinning == true)
		{
			/**
			 * This polling rate must exceed the bandwidth of the FileWriter.
			 * It has 8 pages of 64K floats, or 8*64*1024*4=2MBytes. Since
			 * the speed at which this drains depends on the downsampling rate
			 * and the speed of the storage media, we need to process quickly.
			 * Worst case would be the full 2Msamples per second which would
			 * be 8MB/sec, so the slowest polling rate is 250msec. Since this
			 * isn't an RTOS, go aggressive, 10msec.
			 */
			g_file_writer.wait(10); // milliseconds
		}
	}
	catch (runtime_error re)
	{
		cout << "e-[Writer thread runtime error: " << re.what() << "]" << endl;
	}
	catch (...)
	{
		cout << "e-[Unknown exception in writer thread]" << endl;
	}
}

void
trace_start(void)
{
	g_raw_buffer.reset();
	g_file_writer.open(g_fp_energy.string());
	g_writer_spinning = true;
	g_writer_thread = CreateThread(
		NULL,
		0,
		(LPTHREAD_START_ROUTINE)writer_spin,
		NULL,
		0,
		NULL);
	if (g_writer_thread == NULL)
	{
		throw runtime_error("Failed to create writer thread");
	}
	/**
	 * NOTE:
	 * We cannot call `streaming_on` after the device loop starts
	 * because it calls into `process()` which is not re-entrant.
	 * This can cause the deques to unexpectedly go to zero or
	 * samples to be lost or worse.
	 */
	g_joulescope.streaming_on(true);
	g_device_spinning = true;
	g_device_thread = CreateThread(
		NULL,
		0,
		(LPTHREAD_START_ROUTINE)device_spin,
		NULL,
		0,
		NULL);
	if (g_device_thread == NULL)
	{
		throw runtime_error("Failed to create device thread");
	}
}

void
interpolate_nans(void)
{
	// Need a C++ way to do this
	FILE *fi = NULL;
	FILE *fo = NULL;
	errno_t err;
	err = fopen_s(&fi, g_fp_energy.filename().string().c_str(), "rb");
	if (err) throw runtime_error("Failed to open input file in nan flow");
	err = fopen_s(&fo, g_fp_energy.filename().string().c_str(), "rb");
	if (err) throw runtime_error("Failed to open output file in nan flow");
	if (fi && fo)
	{
		float buf[1024];
		float last_good_val = NAN;
		float y0 = NAN, y1 = NAN, dy, inc;
		size_t t0 = 0, t1 = 0, dt;
		bool tracking = false;
		size_t sample_idx = 0;
		cout << "Removing tasty NaNs\n";
		fseek(fi, 5, 0);
		fseek(fo, 5, 0);
		while (!feof(fi)) {
			size_t numread = fread(buf, sizeof(float), 1024, fi);
			cout << "Read " << numread << endl;
			for (size_t i = 0; i < numread; ++i)
			{
				if (isnan(buf[i]))
				{
					if (!tracking)
					{
						y0 = last_good_val;
						t0 = sample_idx;
						tracking = true;
					}
				}
				else
				{
					if (tracking)
					{
						tracking = false;
						y1 = buf[i];
						t1 = sample_idx;
						if (isnan(y0))
						{
							throw runtime_error("Cannot interpolate if y0 is NAN");
						}
						dt = t1 - t0;
						dy = t1 - y0;
						inc = dt / dy;
						if (dt == 0)
						{
							throw runtime_error("Cannot interpolate if dt is zero");
						}
					}
					else
					{
						last_good_val = buf[i];
					}
				}
				++sample_idx;
			}
		}
		if (tracking)
		{
			throw runtime_error("Cannot interpolate when file ends with NAN");
		}
	}
	if (fi)
	{
		fclose(fi);
	}
	if (fo)
	{
		fclose(fo);
	}
}

void
trace_stop(void)
{
	DWORD rv;
	g_device_spinning = false;
	rv = WaitForSingleObject(g_device_thread, 10000);
	if (rv != WAIT_OBJECT_0)
	{
		throw runtime_error("Device thread failed to exit");
	}
	g_writer_spinning = false;
	rv = WaitForSingleObject(g_writer_thread, 10000);
	if (rv != WAIT_OBJECT_0)
	{
		throw runtime_error("Writer thread failed to exit");
	}
	/**
	 * NOTE:
	 * We cannot call `streaming_off` before the device loop stops
	 * because it calls into `process()` which is not re-entrant.
	 * This can cause the deques to unexpectedly go to zero or
	 * samples to be lost or worse.
	 */
	g_joulescope.streaming_on(false);
	g_file_writer.close();
	// Required by the framework
	cout
		<< "m-regfile-fn["
		<< g_fp_energy.filename().string()
		<< "]-type[emon]-name[js110]"
		<< endl;

	// Always write out a timestamp file, even if empty
	fstream file;
	file.exceptions(std::ifstream::failbit | std::ifstream::badbit);
	file.open(g_fp_timestamps, ios::out);
	file << "[" << endl;
	for (size_t i(0); i < g_file_writer.m_timestamps.size(); ++i)
	{
		file << "\t" << g_file_writer.m_timestamps[i];
		if (i < (g_file_writer.m_timestamps.size() - 1))
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
		<< g_fp_timestamps.filename().string()
		<< "]-type[etime]-name[js110]"
		<< endl;
	// Did we drop any packets? If so, run lengthy flow
	float pct = g_file_writer.nanpct();
	cout
		<< "m-[Found "
		<< setprecision(2) << pct << "% bad samples; limit is "
		<< g_drop_thresh << "%" << endl;
	if (pct > g_drop_thresh)
	{
		cout
			<< "e-[Bad sample percentage exceeded "
			<< setprecision(2) << g_drop_thresh << "%"
			<< endl;
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
	// Per EEMBC, required to let the host know the exit was OK
	cout << "m-exit" << endl;
	exit(0);
}

void
cmd_init(vector<string> tokens)
{
	if (g_joulescope.is_open())
	{
		cout << "e-[A Joulescope is already initialized, deinit first]" << endl;
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
		g_joulescope.set_raw_buffer(&g_raw_buffer);
		g_raw_processor.calibration_set(g_joulescope.m_calibration);
		g_raw_processor.set_writer(&g_file_writer);
		g_file_writer.samplerate(1000, MAX_SAMPLE_RATE);
		wcout << "m-[Opened Joulescope at path " << path << "]" << endl;
	}
	if (tokens.size() > 2) {
		g_drop_thresh = stof(tokens[2]);
	}
}

void
cmd_power(vector<string> tokens)
{
	if (tokens.size() > 1)
	{
		if (g_device_spinning)
		{
			// `power_on` is not thread safe, see notes in trace_start/stop.
			cout << "e-[Cannot change power state while tracing]" << endl;
		}
		else if (g_joulescope.is_open() == false)
		{
			cout << "e-[No Joulescopes are open]" << endl;
		}
		else if (tokens[1] == "on")
		{
			g_joulescope.power_on(true);
		}
		else if (tokens[1] == "off")
		{
			g_joulescope.power_on(false);
		}
		else
		{
			cout << "e-['power' takes 'on' or 'off']" << endl;
	}
		}
	cout << "m-power[" << (g_joulescope.is_powered() ? "on" : "off") << "]" << endl;
}

void
cmd_trace(vector<string> tokens) {
	if (tokens.size() > 1)
	{
		if (g_joulescope.is_open() == false)
		{
			cout << "e-[No Joulescopes are open]" << endl;
		}
		else if (tokens[1] == "on")
		{
			if (!g_device_spinning)
			{
				if (tokens.size() > 2)
				{
					g_tmpdir = tokens[2];
				}
				if (tokens.size() > 3)
				{
					g_fp_energy = g_tmpdir / (tokens[3] + EEMBC_EMON_SUFFIX);
					g_fp_timestamps = g_tmpdir / (tokens[3] + EEMBC_TIMESTAMP_SUFFIX);
				}
				// Always print this on trace start so we detect any cheating.
				cout << "m-dropthresh[" << std::setprecision(3) << g_drop_thresh << "]" << endl;
				trace_start();
			}
		}
		else if (tokens[1] == "off")
		{
			if (g_device_spinning)
			{
				trace_stop();
			}
		}
		else
		{
			cout << "e-['trace' takes 'on' or 'off' (and optional tmpdir and file prefix]" << endl;
		}
	}
	cout << "m-trace[" << (g_device_spinning ? "on" : "off") << "]" << endl;
}

void
cmd_timer(vector<string> tokens)
{
	if (tokens.size() > 1)
	{
		if (tokens[1] == "on")
		{
			g_file_writer.m_observe_timestamps = true;
		}
		else if (tokens[1] == "off")
		{
			g_file_writer.m_observe_timestamps = false;
		}
		else
		{
			cout << "e-['timer' options are 'on' or 'off']" << endl;
			return;
		}
	}
	cout << "m-timer[" << (g_file_writer.m_observe_timestamps ? "on" : "off") << "]" << endl;
}

void
cmd_rate(vector<string> tokens)
{
	if (g_device_spinning)
	{
		// This would screw up all of the memory pointers
		cout << "e-[Cannot change sample rate while tracing]" << endl;
	}
	else if (tokens.size() > 1)
	{
		int rate = stoi(tokens[1]);

		if ((rate < 1) || (rate > 2'000'000) || (MAX_SAMPLE_RATE % rate) != 0)
		{
			cout << "e-[Sample rate must be a factor of 2'000'000]" << endl;
		}
		else
		{
			g_file_writer.samplerate(stoi(tokens[1]), MAX_SAMPLE_RATE);
		}
	}
	cout << "m-rate-hz[" << g_file_writer.samplerate() << "]" << endl;
}

void
cmd_voltage(vector<string> tokens)
{
	if (g_device_spinning)
	{
		// `get_voltage` is not thread safe, see notes in trace_start/stop.
		cout << "e-[Cannot poll voltage while tracing]" << endl;
	}
	else
	{
		cout << "m-voltage-mv[" << g_joulescope.get_voltage() << "]" << endl;
	}
}

void
cmd_deinit(vector<string> tokens)
{
	if (g_device_spinning)
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
		g_userin_spinning = false;
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
	
	g_userin_spinning = true;
	try {
		vector<string> tokens;
		while (g_userin_spinning)
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
