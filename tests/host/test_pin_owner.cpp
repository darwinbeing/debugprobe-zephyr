#include "testing.hpp"
#include "pin_owner.hpp"

using namespace probe;

namespace {

int g_force_closes = 0;
int g_idles = 0;

void fake_idle()
{
	++g_idles;
}

/* Mimics the real hook: closing the SBW session releases the pins. */
void fake_force_close()
{
	++g_force_closes;
	pin_owner::sbw_release();
}

} // namespace

TEST(pin_owner_sbw_claims_free_pins)
{
	CHECK(pin_owner::sbw_claim());
	CHECK(pin_owner::sbw_claim()); /* claiming again while owning is fine */
	pin_owner::sbw_release();
}

TEST(pin_owner_sbw_refused_while_swd_connected)
{
	pin_owner::swd_connect();
	CHECK(!pin_owner::sbw_claim());
	pin_owner::sbw_release(); /* must not drop SWD's ownership */
	CHECK(!pin_owner::sbw_claim());
	pin_owner::swd_disconnect();
	CHECK(pin_owner::sbw_claim());
	pin_owner::sbw_release();
}

TEST(pin_owner_swd_connect_force_closes_sbw)
{
	g_force_closes = 0;
	pin_owner::set_sbw_force_close(fake_force_close);
	CHECK(pin_owner::sbw_claim());
	pin_owner::swd_connect();
	CHECK_EQ(g_force_closes, 1);
	CHECK(!pin_owner::sbw_claim()); /* SWD owns the pins now */
	pin_owner::swd_connect();       /* reconnect: nothing to close */
	CHECK_EQ(g_force_closes, 1);
	pin_owner::swd_disconnect();
	pin_owner::set_sbw_force_close(nullptr);
}

TEST(pin_owner_swd_disconnect_without_connect_is_harmless)
{
	CHECK(pin_owner::sbw_claim());
	pin_owner::swd_disconnect(); /* must not drop SBW's ownership */
	CHECK(pin_owner::sbw_owns());
	pin_owner::swd_connect();    /* no hook installed: just takes over */
	CHECK(!pin_owner::sbw_claim());
	pin_owner::swd_disconnect();
}

TEST(pin_owner_sbw_owns_tracks_the_claim)
{
	CHECK(!pin_owner::sbw_owns());
	CHECK(pin_owner::sbw_claim());
	CHECK(pin_owner::sbw_owns());
	pin_owner::sbw_release();
	CHECK(!pin_owner::sbw_owns());
	pin_owner::swd_connect();
	CHECK(!pin_owner::sbw_owns());
	pin_owner::swd_disconnect();
}

TEST(pin_owner_idle_hook_runs_when_swd_lets_go)
{
	g_idles = 0;
	pin_owner::set_idle_hook(fake_idle);
	pin_owner::swd_disconnect(); /* SWD did not own the pins: nothing to hand back */
	CHECK_EQ(g_idles, 0);
	pin_owner::swd_connect();
	CHECK_EQ(g_idles, 0);
	pin_owner::swd_disconnect();
	CHECK_EQ(g_idles, 1);
	CHECK(pin_owner::sbw_claim()); /* the pins are free again */
	pin_owner::sbw_release();
	CHECK_EQ(g_idles, 1); /* releasing SBW does not run the hook */
	pin_owner::set_idle_hook(nullptr);
}
