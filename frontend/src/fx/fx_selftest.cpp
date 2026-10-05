// RunFxSelfTest: the exchange-rate store's pure rules (roadmap 9.6) -- reading the ECB's daily
// feed, refusing what is not one, the snapshot every consumer reads (EUR listed at 1, stale
// past a week), and where the streamer's currency comes from. Network-free and file-free:
// the live fetch and the cache are owed to a run with the network up.

#include <string>

#include "../log.hpp"
#include "../obs_bootstrap.hpp"
#include "fx_parse.hpp"
#include "fx_rates.hpp"

void ObsBootstrap::RunFxSelfTest()
{
	const std::string feed = "<gesmes:Envelope><Cube><Cube time='2026-10-02'>"
				 "<Cube currency='USD' rate='1.0812'/><Cube currency='JPY' rate='161.42'/>"
				 "<Cube currency='CZK' rate='25.105'/><Cube currency='DKK' rate='7.4603'/>"
				 "<Cube currency='GBP' rate='0.8411'/><Cube currency='HUF' rate='395.20'/>"
				 "<Cube currency='PLN' rate='4.2810'/><Cube currency='RON' rate='4.9747'/>"
				 "<Cube currency='SEK' rate='11.2380'/><Cube currency='CHF' rate='0.9378'/>"
				 "<Cube currency='INR' rate='90.415'/></Cube></Cube></gesmes:Envelope>";
	Fx::RateTable table;
	const bool parseOk = Fx::ParseEcbDaily(feed, table) && table.date == "2026-10-02" &&
			     table.perEur.size() == 11 && table.perEur["INR"] == 90.415;
	Fx::RateTable untouched;
	const bool refuseOk = !Fx::ParseEcbDaily("<html>Service unavailable</html>", untouched) &&
			      untouched.perEur.empty();

	const int64_t day = Fx::DaysFromIsoDate(table.date);
	const Fx::HomeChoice home = Fx::ResolveHome("", {"IN"}, "US");
	const Fx::json fresh = Fx::RateStore::SnapshotOf(table, 1, day + 3, home);
	const Fx::json old = Fx::RateStore::SnapshotOf(table, 1, day + Fx::kStaleAfterDays + 1, home);
	const Fx::json empty = Fx::RateStore::SnapshotOf(Fx::RateTable{}, 0, day, Fx::ResolveHome("", {}, "AR"));
	const bool snapshotOk = fresh["rates"]["EUR"] == 1.0 && fresh["rates"]["INR"] == 90.415 &&
				fresh["stale"] == false && old["stale"] == true && fresh["home"] == "INR" &&
				fresh["homeSource"] == "youtube" && empty["rates"].empty() && empty["stale"] == true &&
				empty["home"] == "" && empty["homeSource"] == "none";
	const bool homeOk = Fx::ResolveHome("JPY", {"IN"}, "US").source == "setting" &&
			    Fx::ResolveHome("", {}, "GB").code == "GBP" && Fx::CurrencyForCountry("de") == "EUR";

	const bool ok = parseOk && refuseOk && snapshotOk && homeOk;
	HostLog(std::string("[selftest] fx rates -> ") + (ok ? "OK" : "MISMATCH") +
		" (parse=" + (parseOk ? "ok" : "bad") + " refuse=" + (refuseOk ? "ok" : "bad") +
		" snapshot=" + (snapshotOk ? "ok" : "bad") + " home=" + (homeOk ? "ok" : "bad") + ")");
}
