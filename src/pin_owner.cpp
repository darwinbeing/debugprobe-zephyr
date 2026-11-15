#include "pin_owner.hpp"

namespace probe::pin_owner {

namespace {

enum class Owner { None, Swd, Sbw };

Owner g_owner = Owner::None;
void (*g_force_close)() = nullptr;
void (*g_idle)() = nullptr;

} // namespace

void swd_connect()
{
	if (g_owner == Owner::Sbw && g_force_close != nullptr) {
		g_force_close(); /* releases the pins through sbw_release() */
	}
	g_owner = Owner::Swd;
}

void swd_disconnect()
{
	if (g_owner == Owner::Swd) {
		g_owner = Owner::None;
		if (g_idle != nullptr) {
			g_idle(); /* the SWD driver leaves the pins driven */
		}
	}
}

bool sbw_claim()
{
	if (g_owner == Owner::Swd) {
		return false;
	}
	g_owner = Owner::Sbw;
	return true;
}

void sbw_release()
{
	if (g_owner == Owner::Sbw) {
		g_owner = Owner::None;
	}
}

bool sbw_owns()
{
	return g_owner == Owner::Sbw;
}

void set_sbw_force_close(void (*hook)())
{
	g_force_close = hook;
}

void set_idle_hook(void (*hook)())
{
	g_idle = hook;
}

} // namespace probe::pin_owner
