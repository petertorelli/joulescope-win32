#pragma once

#include "joulescope.hpp"
#include "raw_processor.hpp"
#include <fstream>
#include <filesystem>
#include <iomanip>

#include <csignal>

#include <boost\algorithm\string.hpp>
#include <boost\algorithm\string\trim_all.hpp>

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
		if ((sample_rate < 1) || (MAX_SAMPLE_RATE % sample_rate) != 0)
		{
			throw std::runtime_error("Sample rate must be a factor of 1'000'000");
		}
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

void cmd_config(std::vector<std::string>);
void cmd_debug(std::vector<std::string>);
void cmd_deinit(std::vector<std::string>);
void cmd_exit(std::vector<std::string>);
void cmd_help(std::vector<std::string>);
void cmd_init(std::vector<std::string>);
void cmd_power(std::vector<std::string>);
void cmd_samplerate(std::vector<std::string>);
void cmd_timestamps(std::vector<std::string>);
void cmd_trace_start(std::vector<std::string>);
void cmd_trace_stop(std::vector<std::string>);
void cmd_updates(std::vector<std::string>);
