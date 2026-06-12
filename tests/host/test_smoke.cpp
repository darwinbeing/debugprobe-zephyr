#include "testing.hpp"

TEST(harness_check_eq_works)
{
	CHECK_EQ(1 + 1, 2);
	CHECK(true);
}

TEST(harness_check_bytes_works)
{
	const uint8_t a[] = {0x00, 0x01};
	const uint8_t b[] = {0x00, 0x01};
	CHECK_BYTES(a, b);
}

TEST_MAIN()
