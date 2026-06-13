#pragma once
/* Recording/scripting mock of dap::SwdDriver for unit tests. */
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "dap/interfaces.hpp"

class MockSwdDriver : public dap::SwdDriver {
public:
	/* --- scripting --- */
	struct XferResult {
		dap::Ack ack;
		uint32_t read_data; /* returned through *data on reads */
	};
	/* Results are consumed FIFO; when empty, Ok/0xDEADBEEF is returned. */
	std::deque<XferResult> script;

	/* --- block-read DMA fast-path mock --- */
	bool block_enabled = false;       /* when true, transfer_block_read handles it */
	std::vector<uint32_t> block_words; /* data words the DMA path "returns" */
	uint8_t block_ack = 0x01;          /* dap::kXferOk */
	int block_completed = -1;          /* -1 => block_words.size() */

	uint8_t pins_state = 0;     /* returned by read_pins() */
	uint32_t timestamp_value = 0;

	/* --- recordings --- */
	struct XferCall {
		uint8_t request;
		bool data_null;
		uint32_t write_data; /* valid when it was a write */
	};
	std::vector<XferCall> transfers;
	std::vector<std::string> events; /* coarse call log, in order */
	std::vector<uint8_t> swj_bits;   /* concatenated swj_sequence payloads */
	size_t swj_bit_count = 0;
	uint32_t last_clock = 0;
	uint8_t last_idle_cycles = 0;
	uint8_t last_turnaround = 0;
	bool last_data_phase = false;
	uint8_t last_pin_select = 0, last_pin_value = 0;
	uint32_t last_pin_wait = 0;
	uint32_t delays_us = 0;
	std::vector<uint8_t> swd_input_data; /* bytes fed back by swd_input */

	void set_clock(uint32_t hz) override
	{
		last_clock = hz;
		events.push_back("set_clock");
	}
	void connect() override { events.push_back("connect"); }
	void disconnect() override { events.push_back("disconnect"); }
	void configure(uint8_t turnaround, bool data_phase) override
	{
		last_turnaround = turnaround;
		last_data_phase = data_phase;
		events.push_back("configure");
	}
	void set_idle_cycles(uint8_t cycles) override
	{
		last_idle_cycles = cycles;
		events.push_back("set_idle_cycles");
	}
	void swj_sequence(std::span<const uint8_t> data, size_t bits) override
	{
		swj_bits.insert(swj_bits.end(), data.begin(), data.end());
		swj_bit_count += bits;
		events.push_back("swj_sequence");
	}
	void swd_output(std::span<const uint8_t> data, size_t bits) override
	{
		swj_bits.insert(swj_bits.end(), data.begin(), data.end());
		swj_bit_count += bits;
		events.push_back("swd_output");
	}
	void swd_input(std::span<uint8_t> data, size_t bits) override
	{
		for (size_t i = 0; i < data.size(); ++i) {
			data[i] = i < swd_input_data.size() ? swd_input_data[i] : 0xA5;
		}
		(void)bits;
		events.push_back("swd_input");
	}
	dap::Ack transfer(uint8_t request, uint32_t *data) override
	{
		XferResult r{dap::Ack::Ok, 0xDEADBEEF};
		if (!script.empty()) {
			r = script.front();
			script.pop_front();
		}
		XferCall call{request, data == nullptr, 0};
		const bool is_read = (request & 0x02) != 0;
		if (data != nullptr) {
			if (is_read) {
				*data = r.read_data;
			} else {
				call.write_data = *data;
			}
		}
		transfers.push_back(call);
		return r.ack;
	}
	bool transfer_block_read(uint8_t request, uint16_t count, uint8_t *out,
				 size_t out_cap, uint16_t *completed,
				 uint8_t *ack) override
	{
		(void)request;
		(void)count;
		if (!block_enabled) {
			return false;
		}
		events.push_back("transfer_block_read");
		size_t done = block_completed >= 0
				      ? static_cast<size_t>(block_completed)
				      : block_words.size();
		const size_t cap = out_cap / 4;
		if (done > cap) {
			done = cap;
		}
		for (size_t i = 0; i < done; ++i) {
			uint32_t w = i < block_words.size() ? block_words[i] : 0;
			out[i * 4 + 0] = static_cast<uint8_t>(w);
			out[i * 4 + 1] = static_cast<uint8_t>(w >> 8);
			out[i * 4 + 2] = static_cast<uint8_t>(w >> 16);
			out[i * 4 + 3] = static_cast<uint8_t>(w >> 24);
		}
		*completed = static_cast<uint16_t>(done);
		*ack = block_ack;
		return true;
	}
	void set_pins(uint8_t select, uint8_t value, uint32_t wait_us) override
	{
		last_pin_select = select;
		last_pin_value = value;
		last_pin_wait = wait_us;
		events.push_back("set_pins");
	}
	uint8_t read_pins() override { return pins_state; }
	void delay_us(uint32_t us) override { delays_us += us; }
	uint32_t timestamp() override { return timestamp_value; }
};

class MockHostStatus : public dap::HostStatus {
public:
	int connected_calls = 0, running_calls = 0;
	bool last_connected = false, last_running = false;
	void connected(bool on) override
	{
		++connected_calls;
		last_connected = on;
	}
	void running(bool on) override
	{
		++running_calls;
		last_running = on;
	}
};
