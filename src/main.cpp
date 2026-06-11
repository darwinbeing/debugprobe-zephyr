#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <array>
#include <span>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

namespace {

/* C++20 smoke check: if concepts + std::span compile, the toolchain and
 * Kconfig selections are correct.
 */
template <typename T>
concept ByteBuffer = requires(T t) {
	{ t.size() } -> std::convertible_to<size_t>;
	{ t.data() };
};

static_assert(ByteBuffer<std::array<uint8_t, 4>>);
static_assert(ByteBuffer<std::span<const uint8_t>>);

} // namespace

int main()
{
	LOG_INF("debugprobe-zephyr starting");
	return 0;
}
