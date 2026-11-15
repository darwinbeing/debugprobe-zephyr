#pragma once
/* Micro test harness for the dap library host tests.
 * Zero dependencies; failures print a hex diff and the binary exits 1.
 */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace testing {

struct Test {
	const char *name;
	std::function<void()> fn;
};

inline std::vector<Test> &registry()
{
	static std::vector<Test> r;
	return r;
}

inline int failures = 0;
inline const char *current_test = "";

struct Register {
	Register(const char *name, std::function<void()> fn)
	{
		registry().push_back({name, std::move(fn)});
	}
};

inline std::string hex(std::span<const uint8_t> b)
{
	std::string s;
	char tmp[4];
	for (uint8_t v : b) {
		std::snprintf(tmp, sizeof(tmp), "%02X ", v);
		s += tmp;
	}
	return s;
}

inline void fail(const char *file, int line, const std::string &msg)
{
	std::printf("FAIL %s (%s:%d)\n  %s\n", current_test, file, line, msg.c_str());
	std::fflush(stdout);
	++failures;
}

inline void check_bytes(const char *file, int line, std::span<const uint8_t> got,
			std::span<const uint8_t> want)
{
	if (got.size() == want.size() &&
	    std::equal(got.begin(), got.end(), want.begin())) {
		return;
	}
	fail(file, line, "bytes differ\n  want: " + hex(want) + "\n  got:  " + hex(got));
}

inline int run_all()
{
	for (const auto &t : registry()) {
		current_test = t.name;
		t.fn();
	}
	if (failures) {
		std::printf("\n%d FAILURE(S)\n", failures);
		return 1;
	}
	std::printf("\nOK: %zu tests\n", registry().size());
	return 0;
}

} // namespace testing

#define TEST(name)                                                             \
	static void test_fn_##name();                                          \
	static ::testing::Register reg_##name(#name, test_fn_##name);          \
	static void test_fn_##name()

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			::testing::fail(__FILE__, __LINE__, "CHECK failed: " #cond); \
		}                                                              \
	} while (0)

/* Like CHECK, but ends the test: use it before code that would be
 * undefined if the condition were false (indexing, front(), back()). */
#define REQUIRE(cond)                                                          \
	do {                                                                   \
		if (!(cond)) {                                                 \
			::testing::fail(__FILE__, __LINE__, "REQUIRE failed: " #cond); \
			return;                                                \
		}                                                              \
	} while (0)

#define CHECK_EQ(a, b)                                                         \
	do {                                                                   \
		auto va = (a);                                                 \
		auto vb = (b);                                                 \
		if (!(va == vb)) {                                             \
			::testing::fail(__FILE__, __LINE__,                    \
					std::string("CHECK_EQ failed: " #a " == " #b \
						    " (") + std::to_string((long long)va) + \
						" != " + std::to_string((long long)vb) + ")"); \
		}                                                              \
	} while (0)

#define CHECK_BYTES(got, want)                                                 \
	::testing::check_bytes(__FILE__, __LINE__, got, want)

#define TEST_MAIN()                                                            \
	int main()                                                             \
	{                                                                      \
		return ::testing::run_all();                                   \
	}
