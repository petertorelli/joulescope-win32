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

#pragma once

#include "joulescope.hpp"
#include "raw_processor.hpp"
#include "raw_buffer.hpp"
#include "file_writer.hpp"
#include <fstream>
#include <filesystem>
#include <iomanip>

#include <csignal>

#include <boost\algorithm\string\trim_all.hpp>
#include <boost\tokenizer.hpp>

typedef boost::escaped_list_separator<char> delim_t;
typedef boost::tokenizer<delim_t> tokenizer_t;

struct TraceStats
{
	TraceStats()
	{
		m_sample_rate = 1000;
		reset();
	}
	void reset(void)
	{
		// don't reset sample rate, user might have set it!
		m_accumulator = 0.0f;
		m_last_gpi0 = true;
		m_last_pkt_index = 0;
		m_total_downsamples = MAX_SAMPLE_RATE / m_sample_rate;
		m_total_accumulated = 0;
		m_total_samples = 0;
		m_total_dropped_pkts = 0;
		m_total_nan = 0;
		m_total_inf = 0;
		m_timestamps.clear();
	}
	void set_samplerate(uint32_t sample_rate)
	{
		m_sample_rate = sample_rate;
		m_total_downsamples = MAX_SAMPLE_RATE / m_sample_rate;
	}
	uint32_t      m_sample_rate;
	double        m_accumulator;
	bool          m_last_gpi0;
	uint16_t      m_last_pkt_index;
	uint32_t      m_total_downsamples;
	uint32_t      m_total_accumulated;
	uint64_t      m_total_samples;
	uint64_t      m_total_dropped_pkts;
	uint64_t      m_total_nan;
	uint64_t      m_total_inf;

	std::vector<float> m_timestamps;
};

struct Command
{
	void(*func)(std::vector<std::string>);
	std::string desc;
};

typedef std::map<std::string, Command> CommandTable;

void cmd_debug(std::vector<std::string>);
void cmd_deinit(std::vector<std::string>);
void cmd_exit(std::vector<std::string>);
void cmd_help(std::vector<std::string>);
void cmd_init(std::vector<std::string>);
void cmd_power(std::vector<std::string>);
void cmd_timer(std::vector<std::string>);
void cmd_trace(std::vector<std::string>);
void cmd_rate(std::vector<std::string>);
void cmd_voltage(std::vector<std::string>);
void cmd_updates(std::vector<std::string>);
