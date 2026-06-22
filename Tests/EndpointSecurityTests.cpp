// Copyright Crane Valley. All Rights Reserved.
//
// Standalone unit tests for Framedash::ExtractUrlHost, IsEndpointSecure, and
// IsCrossOriginRedirect. Verifies the loopback HTTPS exemption matches the parsed
// host exactly and rejects the look-alike / userinfo / fragment bypasses that an
// unanchored substring check (the previous implementation) would have accepted,
// and that the redirect cross-origin detection compares full origins (scheme +
// host + port) while failing open on empty/unparseable effective URLs. Runs
// without an UnrealEditor build.

#include "FramedashEndpointSecurity.h"

#include <gtest/gtest.h>

using Framedash::ExtractUrlHost;
using Framedash::IsCrossOriginRedirect;
using Framedash::IsEndpointSecure;

// -- ExtractUrlHost ----------------------------------------------------

TEST(ExtractUrlHost, PlainHttpsHost)
{
	EXPECT_EQ(ExtractUrlHost("https://ingest.framedash.dev/v1/events"), "ingest.framedash.dev");
}

TEST(ExtractUrlHost, StripsPort)
{
	EXPECT_EQ(ExtractUrlHost("http://localhost:8787/v1/events"), "localhost");
}

TEST(ExtractUrlHost, StripsUserinfo)
{
	EXPECT_EQ(ExtractUrlHost("http://user:pass@evil.example/path"), "evil.example");
}

TEST(ExtractUrlHost, UsesLastAtForUserinfo)
{
	// A userinfo segment containing '@' must not be mistaken for the host.
	EXPECT_EQ(ExtractUrlHost("http://user@name:p@evil.example/x"), "evil.example");
}

TEST(ExtractUrlHost, LowercasesHost)
{
	EXPECT_EQ(ExtractUrlHost("https://INGEST.Framedash.DEV/x"), "ingest.framedash.dev");
}

TEST(ExtractUrlHost, StripsQueryAndFragment)
{
	EXPECT_EQ(ExtractUrlHost("http://evil.example?x=localhost"), "evil.example");
	EXPECT_EQ(ExtractUrlHost("http://evil.example#localhost"), "evil.example");
}

TEST(ExtractUrlHost, Ipv6LiteralKeepsBrackets)
{
	EXPECT_EQ(ExtractUrlHost("http://[::1]:8787/v1/events"), "[::1]");
	EXPECT_EQ(ExtractUrlHost("http://[2001:db8::1]/x"), "[2001:db8::1]");
}

TEST(ExtractUrlHost, Ipv6TrailingTextIsMalformed)
{
	// Text between ']' and the port is invalid; fail closed (empty host) so it
	// cannot pose as the loopback literal "[::1]".
	EXPECT_EQ(ExtractUrlHost("http://[::1].evil.example/v1/events"), "");
	EXPECT_EQ(ExtractUrlHost("http://[::1]evil/v1/events"), "");
	EXPECT_EQ(ExtractUrlHost("http://[::1/v1/events"), "");
}

TEST(ExtractUrlHost, NoSchemeFallsBackToAuthority)
{
	EXPECT_EQ(ExtractUrlHost("localhost:8787/x"), "localhost");
}

TEST(ExtractUrlHost, BackslashTerminatesAuthority)
{
	// WHATWG treats '\' as '/' for special schemes, so the host is what precedes
	// the backslash -- not a loopback label smuggled in after it via "@".
	EXPECT_EQ(ExtractUrlHost("http://evil.com\\@localhost/v1/events"), "evil.com");
	EXPECT_EQ(ExtractUrlHost("http://evil.com\\localhost/v1/events"), "evil.com");
}

TEST(ExtractUrlHost, EmptyForMissingAuthority)
{
	EXPECT_EQ(ExtractUrlHost(""), "");
	EXPECT_EQ(ExtractUrlHost("https://"), "");
}

// -- IsEndpointSecure: allowed -----------------------------------------

TEST(IsEndpointSecure, HttpsProductionAllowed)
{
	EXPECT_TRUE(IsEndpointSecure("https://ingest.framedash.dev/v1/events"));
}

TEST(IsEndpointSecure, HttpsSchemeIsCaseInsensitive)
{
	EXPECT_TRUE(IsEndpointSecure("HTTPS://ingest.framedash.dev/v1/events"));
}

TEST(IsEndpointSecure, LoopbackHostsAllowedOverHttp)
{
	EXPECT_TRUE(IsEndpointSecure("http://localhost:8787/v1/events"));
	EXPECT_TRUE(IsEndpointSecure("http://127.0.0.1:8787/v1/events"));
	EXPECT_TRUE(IsEndpointSecure("http://[::1]:8787/v1/events"));
}

TEST(IsEndpointSecure, LoopbackAllowedWithoutPort)
{
	EXPECT_TRUE(IsEndpointSecure("http://localhost/v1/events"));
}

// -- IsEndpointSecure: rejected (the substring-match bypasses) ----------

TEST(IsEndpointSecure, RejectsCleartextProductionHost)
{
	EXPECT_FALSE(IsEndpointSecure("http://ingest.framedash.dev/v1/events"));
}

TEST(IsEndpointSecure, RejectsNonHttpSchemeToLoopback)
{
	// The cleartext exemption is for "http://" only; a loopback host under any
	// other scheme must not pass the gate.
	EXPECT_FALSE(IsEndpointSecure("ftp://localhost/x"));
	EXPECT_FALSE(IsEndpointSecure("file://localhost/etc/passwd"));
	EXPECT_FALSE(IsEndpointSecure("ws://localhost:8787/v1/events"));
}

TEST(IsEndpointSecure, RejectsLookAlikeLoopbackSubdomain)
{
	// "localhost.attacker.com" embeds "localhost" but the host is attacker-owned.
	EXPECT_FALSE(IsEndpointSecure("http://localhost.attacker.com/v1/events"));
	EXPECT_FALSE(IsEndpointSecure("http://127.0.0.1.attacker.com/v1/events"));
}

TEST(IsEndpointSecure, RejectsLoopbackInQueryOrFragment)
{
	EXPECT_FALSE(IsEndpointSecure("http://evil.example/v1/events?host=localhost"));
	EXPECT_FALSE(IsEndpointSecure("http://evil.example/v1/events#127.0.0.1"));
}

TEST(IsEndpointSecure, RejectsLoopbackInUserinfo)
{
	EXPECT_FALSE(IsEndpointSecure("http://localhost@evil.example/v1/events"));
	EXPECT_FALSE(IsEndpointSecure("http://127.0.0.1:pass@evil.example/v1/events"));
}

TEST(IsEndpointSecure, RejectsBackslashAuthoritySmuggling)
{
	// "http://evil.com\@localhost" -> host is evil.com (backslash ends the
	// authority), so the cleartext HTTP endpoint must be rejected.
	EXPECT_FALSE(IsEndpointSecure("http://evil.com\\@localhost/v1/events"));
	EXPECT_FALSE(IsEndpointSecure("http://evil.com\\localhost/v1/events"));
	// The reverse: our WHATWG parser would read "localhost" but libcurl would
	// connect to evil.com -- a parser differential, so reject any '@' or '\'.
	EXPECT_FALSE(IsEndpointSecure("http://localhost\\@evil.com/v1/events"));
	EXPECT_FALSE(IsEndpointSecure("https://localhost\\@evil.com/v1/events"));
	EXPECT_FALSE(IsEndpointSecure("https://localhost@evil.com/v1/events"));
}

TEST(IsEndpointSecure, RejectsNonCanonicalLoopbackIp)
{
	// 127.0.0.10 is a different address; only the canonical 127.0.0.1 is exempt.
	EXPECT_FALSE(IsEndpointSecure("http://127.0.0.10:8787/v1/events"));
}

TEST(IsEndpointSecure, RejectsNonCanonicalLoopbackAliasesOverHttp)
{
	// Intentional allowlist: only localhost / 127.0.0.1 / [::1] get the HTTP
	// exemption. Other loopback spellings must use HTTPS.
	EXPECT_FALSE(IsEndpointSecure("http://127.0.0.2/v1/events"));
	EXPECT_FALSE(IsEndpointSecure("http://127.1/v1/events"));
	EXPECT_FALSE(IsEndpointSecure("http://[0:0:0:0:0:0:0:1]/v1/events"));
	EXPECT_FALSE(IsEndpointSecure("http://localhost./v1/events"));
}

TEST(IsEndpointSecure, RejectsIpv6TrailingTextSmuggling)
{
	EXPECT_FALSE(IsEndpointSecure("http://[::1].evil.example/v1/events"));
	// HTTPS is still allowed regardless of host (the key stays encrypted).
	EXPECT_TRUE(IsEndpointSecure("https://[::1].evil.example/v1/events"));
}

TEST(IsEndpointSecure, RejectsEmbeddedControlCharacters)
{
	// An embedded NUL must not let "http://localhost\0.evil" pass as loopback.
	std::string nulUrl = "http://localhost";
	nulUrl.push_back('\0');
	nulUrl += ".evil.example/v1/events";
	EXPECT_FALSE(IsEndpointSecure(nulUrl));
	// Any other raw control char is rejected too, even over HTTPS.
	EXPECT_FALSE(IsEndpointSecure(std::string("https://ingest.framedash.dev/\x01/x")));
}

TEST(IsEndpointSecure, HttpsLoopbackLookAlikeStillAllowedViaHttps)
{
	// Not a loopback host, but HTTPS keeps the key encrypted -> allowed.
	EXPECT_TRUE(IsEndpointSecure("https://localhost.attacker.com/v1/events"));
}

// -- IsCrossOriginRedirect: same origin (no flag) ----------------------

TEST(IsCrossOriginRedirect, IdenticalUrlIsSameOrigin)
{
	EXPECT_FALSE(IsCrossOriginRedirect(
		"https://ingest.framedash.dev/v1/events",
		"https://ingest.framedash.dev/v1/events"));
}

TEST(IsCrossOriginRedirect, SameOriginDifferentPathIsSameOrigin)
{
	// A redirect that stays on the configured host/scheme/port is not a leak.
	EXPECT_FALSE(IsCrossOriginRedirect(
		"https://ingest.framedash.dev/v1/events",
		"https://ingest.framedash.dev/v2/events?retry=1"));
}

TEST(IsCrossOriginRedirect, DefaultPortMatchesExplicitPort)
{
	// https default port 443 normalizes to an explicit :443 -> same origin.
	EXPECT_FALSE(IsCrossOriginRedirect(
		"https://ingest.framedash.dev/v1/events",
		"https://ingest.framedash.dev:443/v1/events"));
	EXPECT_FALSE(IsCrossOriginRedirect(
		"http://localhost:80/v1/events",
		"http://localhost/v1/events"));
}

TEST(IsCrossOriginRedirect, HostCaseIsNormalized)
{
	EXPECT_FALSE(IsCrossOriginRedirect(
		"https://ingest.framedash.dev/v1/events",
		"https://INGEST.Framedash.DEV/v1/events"));
}

TEST(IsCrossOriginRedirect, Ipv6SameOrigin)
{
	EXPECT_FALSE(IsCrossOriginRedirect(
		"https://[2001:db8::1]:8443/v1/events",
		"https://[2001:db8::1]:8443/other"));
}

// -- IsCrossOriginRedirect: fail open (no flag) ------------------------

TEST(IsCrossOriginRedirect, EmptyEffectiveUrlIsNotFlagged)
{
	// A backend that does not populate the effective URL must never drop traffic.
	EXPECT_FALSE(IsCrossOriginRedirect("https://ingest.framedash.dev/v1/events", ""));
}

TEST(IsCrossOriginRedirect, UnparseableEffectiveUrlIsNotFlagged)
{
	// No scheme/host -> cannot reason about origin -> fail open.
	EXPECT_FALSE(IsCrossOriginRedirect("https://ingest.framedash.dev/v1/events", "not a url"));
	EXPECT_FALSE(IsCrossOriginRedirect("https://ingest.framedash.dev/v1/events", "https://"));
}

// -- IsCrossOriginRedirect: cross origin (flagged) ---------------------

TEST(IsCrossOriginRedirect, DifferentHostIsCrossOrigin)
{
	EXPECT_TRUE(IsCrossOriginRedirect(
		"https://ingest.framedash.dev/v1/events",
		"https://evil.example/v1/events"));
}

TEST(IsCrossOriginRedirect, DifferentSchemeIsCrossOrigin)
{
	// An https -> http downgrade redirect would expose the key in cleartext.
	EXPECT_TRUE(IsCrossOriginRedirect(
		"https://ingest.framedash.dev/v1/events",
		"http://ingest.framedash.dev/v1/events"));
}

TEST(IsCrossOriginRedirect, DifferentPortIsCrossOrigin)
{
	EXPECT_TRUE(IsCrossOriginRedirect(
		"https://ingest.framedash.dev/v1/events",
		"https://ingest.framedash.dev:8443/v1/events"));
}

TEST(IsCrossOriginRedirect, SiblingSubdomainIsCrossOrigin)
{
	// Strict origin: a different host under the same registrable domain still
	// crosses a trust boundary for the pinned endpoint.
	EXPECT_TRUE(IsCrossOriginRedirect(
		"https://ingest.framedash.dev/v1/events",
		"https://collector.framedash.dev/v1/events"));
}

TEST(IsCrossOriginRedirect, Ipv6DifferentHostIsCrossOrigin)
{
	EXPECT_TRUE(IsCrossOriginRedirect(
		"https://[2001:db8::1]/v1/events",
		"https://[2001:db8::2]/v1/events"));
}

// -- IsCrossOriginRedirect: parser-differential effective URLs fail closed ---

TEST(IsCrossOriginRedirect, BackslashUserinfoEffectiveUrlFailsClosed)
{
	// libcurl would split the host at the last '@' and connect to evil.example,
	// while the WHATWG host parser stops at '\' and reads the configured host --
	// a missed leak. The '@' / '\' guard flags it as cross origin.
	EXPECT_TRUE(IsCrossOriginRedirect(
		"https://ingest.framedash.dev/v1/events",
		"https://ingest.framedash.dev\\@evil.example/v1/events"));
	EXPECT_TRUE(IsCrossOriginRedirect(
		"https://ingest.framedash.dev/v1/events",
		"https://ingest.framedash.dev@evil.example/v1/events"));
}

TEST(IsCrossOriginRedirect, ControlCharEffectiveUrlFailsClosed)
{
	std::string ctrlUrl = "https://ingest.framedash.dev";
	ctrlUrl.push_back('\0');
	ctrlUrl += ".evil.example/v1/events";
	EXPECT_TRUE(IsCrossOriginRedirect("https://ingest.framedash.dev/v1/events", ctrlUrl));
}

TEST(IsCrossOriginRedirect, MalformedPortEffectiveUrlFailsClosed)
{
	// A non-numeric port never appears in a legitimate landing URL; the origin
	// differs from the configured ":443", so it fails closed (dropped) rather than
	// being trusted. (Failing open is reserved for an undeterminable origin.)
	EXPECT_TRUE(IsCrossOriginRedirect(
		"https://ingest.framedash.dev/v1/events",
		"https://ingest.framedash.dev:abc/v1/events"));
}
