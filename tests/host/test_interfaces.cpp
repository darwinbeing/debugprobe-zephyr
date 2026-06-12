#include "testing.hpp"
#include "mock_swd_driver.hpp"
#include "dap/constants.hpp"

TEST(mock_records_transfer_calls)
{
	MockSwdDriver drv;
	drv.script.push_back({dap::Ack::Ok, 0x12345678});
	uint32_t data = 0;
	dap::Ack ack = drv.transfer(dap::kReqRnW, &data); /* DP read */
	CHECK_EQ(static_cast<int>(ack), static_cast<int>(dap::Ack::Ok));
	CHECK_EQ(data, 0x12345678u);
	CHECK_EQ(drv.transfers.size(), 1u);
	CHECK(drv.transfers[0].data_null == false);
}

TEST(mock_default_script_is_ok)
{
	MockSwdDriver drv;
	uint32_t data = 0;
	CHECK_EQ(static_cast<int>(drv.transfer(dap::kReqRnW, &data)),
		 static_cast<int>(dap::Ack::Ok));
	CHECK_EQ(data, 0xDEADBEEFu);
}
