// Copyright Crane Valley. All Rights Reserved.
//
// Standalone unit tests for the prefer-IPv4-with-IPv6-fallback address
// planner (FramedashAddressPlanner). Mirrors the Unity/Godot
// EndpointAddressPlanner test intent (#1218/#1216): endpoint qualification,
// IPv4-first attempt ordering, passthrough on resolution failure, the
// modulo family toggle, and the Host-header / request-target / port
// extraction the direct-socket transport feeds into the raw HTTP head.
// Runs without an UnrealEditor build.

#include "FramedashAddressPlanner.h"

#include <gtest/gtest.h>

using Framedash::BuildAddressPlan;
using Framedash::EIpFamily;
using Framedash::ExtractPortOrDefault;
using Framedash::ExtractRequestTarget;
using Framedash::FAddressPlan;
using Framedash::HostHeaderValue;
using Framedash::NextFamilyIndex;
using Framedash::ShouldForceAddressFamily;

namespace
{
	constexpr const char* IngestUrl = "https://ingest.framedash.dev/v1/events";
}

// -- ShouldForceAddressFamily ------------------------------------------

TEST(ShouldForceAddressFamily, RemoteHttpsHostnameQualifies)
{
	EXPECT_TRUE(ShouldForceAddressFamily(IngestUrl));
}

TEST(ShouldForceAddressFamily, HttpsWithExplicitPortQualifies)
{
	EXPECT_TRUE(ShouldForceAddressFamily("https://ingest.example.com:8443/v1/events"));
}

TEST(ShouldForceAddressFamily, UppercaseSchemeQualifies)
{
	EXPECT_TRUE(ShouldForceAddressFamily("HTTPS://ingest.framedash.dev/v1/events"));
}

TEST(ShouldForceAddressFamily, PlainHttpDoesNotQualify)
{
	// HTTP endpoints are loopback-only per EndpointSecurity: no exposure.
	EXPECT_FALSE(ShouldForceAddressFamily("http://localhost:8787/v1/events"));
	EXPECT_FALSE(ShouldForceAddressFamily("http://example.com/v1/events"));
}

TEST(ShouldForceAddressFamily, LocalhostDoesNotQualify)
{
	EXPECT_FALSE(ShouldForceAddressFamily("https://localhost/v1/events"));
	EXPECT_FALSE(ShouldForceAddressFamily("https://LOCALHOST/v1/events"));
}

TEST(ShouldForceAddressFamily, IpLiteralsDoNotQualify)
{
	// Already pinned to one address -- nothing to prefer.
	EXPECT_FALSE(ShouldForceAddressFamily("https://127.0.0.1/v1/events"));
	EXPECT_FALSE(ShouldForceAddressFamily("https://192.168.1.50:8443/v1/events"));
	EXPECT_FALSE(ShouldForceAddressFamily("https://[2606:4700::1]/v1/events"));
	EXPECT_FALSE(ShouldForceAddressFamily("https://[::1]:8443/v1/events"));
}

TEST(ShouldForceAddressFamily, EmptyAndMalformedDoNotQualify)
{
	EXPECT_FALSE(ShouldForceAddressFamily(""));
	EXPECT_FALSE(ShouldForceAddressFamily("https://"));
	EXPECT_FALSE(ShouldForceAddressFamily("not a url"));
	EXPECT_FALSE(ShouldForceAddressFamily("ftp://example.com/x"));
}

TEST(ShouldForceAddressFamily, HostParsingDifferentialsDoNotQualify)
{
	// '@' / '\' / control characters: same libcurl differential rejection as
	// IsEndpointSecure -- never engage on a host this parser could misread.
	EXPECT_FALSE(ShouldForceAddressFamily("https://user@evil.example/x"));
	EXPECT_FALSE(ShouldForceAddressFamily("https://evil.example\\@good.example/x"));
	EXPECT_FALSE(ShouldForceAddressFamily("https://evil.example/\r\nHost: x"));
}

TEST(ShouldForceAddressFamily, MalformedExplicitPortDoesNotQualify)
{
	EXPECT_FALSE(ShouldForceAddressFamily("https://example.com:notaport/x"));
	EXPECT_FALSE(ShouldForceAddressFamily("https://example.com:99999/x"));
	EXPECT_FALSE(ShouldForceAddressFamily("https://example.com:/x"));
}

// -- BuildAddressPlan ---------------------------------------------------

TEST(BuildAddressPlan, IPv4PreferredThenIPv6)
{
	const FAddressPlan Plan = BuildAddressPlan(IngestUrl, "104.16.1.2", "2606:4700::abcd");
	ASSERT_EQ(Plan.Attempts.size(), 2u);
	EXPECT_EQ(Plan.Attempts[0].Family, EIpFamily::IPv4);
	EXPECT_EQ(Plan.Attempts[0].IpLiteral, "104.16.1.2");
	EXPECT_EQ(Plan.Attempts[1].Family, EIpFamily::IPv6);
	EXPECT_EQ(Plan.Attempts[1].IpLiteral, "2606:4700::abcd");
	EXPECT_FALSE(Plan.IsPassthrough());
	EXPECT_EQ(Plan.HostHeader, "ingest.framedash.dev");
	EXPECT_EQ(Plan.CommonName, "ingest.framedash.dev");
	EXPECT_EQ(Plan.RequestTarget, "/v1/events");
	EXPECT_EQ(Plan.Port, 443);
}

TEST(BuildAddressPlan, IPv4OnlyWhenNoIPv6)
{
	const FAddressPlan Plan = BuildAddressPlan(IngestUrl, "104.16.1.2", "");
	ASSERT_EQ(Plan.Attempts.size(), 1u);
	EXPECT_EQ(Plan.Attempts[0].Family, EIpFamily::IPv4);
}

TEST(BuildAddressPlan, IPv6OnlyStillDelivers)
{
	// An IPv6-only network must not be regressed by the IPv4 preference.
	const FAddressPlan Plan = BuildAddressPlan(IngestUrl, "", "2606:4700::abcd");
	ASSERT_EQ(Plan.Attempts.size(), 1u);
	EXPECT_EQ(Plan.Attempts[0].Family, EIpFamily::IPv6);
	EXPECT_EQ(Plan.Attempts[0].IpLiteral, "2606:4700::abcd");
}

TEST(BuildAddressPlan, BracketedIPv6IsStoredUnbracketed)
{
	const FAddressPlan Plan = BuildAddressPlan(IngestUrl, "", "[2606:4700::abcd]");
	ASSERT_EQ(Plan.Attempts.size(), 1u);
	EXPECT_EQ(Plan.Attempts[0].IpLiteral, "2606:4700::abcd");
}

TEST(BuildAddressPlan, NothingResolvedIsPassthrough)
{
	const FAddressPlan Plan = BuildAddressPlan(IngestUrl, "", "");
	EXPECT_TRUE(Plan.IsPassthrough());
}

TEST(BuildAddressPlan, NonQualifyingEndpointIsPassthroughEvenWithAddresses)
{
	const FAddressPlan Plan =
		BuildAddressPlan("http://localhost:8787/v1/events", "104.16.1.2", "2606:4700::1");
	EXPECT_TRUE(Plan.IsPassthrough());
}

TEST(BuildAddressPlan, ExplicitPortFlowsIntoHostHeaderAndPort)
{
	const FAddressPlan Plan =
		BuildAddressPlan("https://ingest.example.com:8443/v1/events?fresh=1", "104.16.1.2", "");
	EXPECT_EQ(Plan.Port, 8443);
	EXPECT_EQ(Plan.HostHeader, "ingest.example.com:8443");
	EXPECT_EQ(Plan.RequestTarget, "/v1/events?fresh=1");
}

// -- NextFamilyIndex ----------------------------------------------------

TEST(NextFamilyIndex, TogglesBetweenTwoFamilies)
{
	EXPECT_EQ(NextFamilyIndex(0, 2), 1);
	EXPECT_EQ(NextFamilyIndex(1, 2), 0); // wraps back to preferred IPv4
}

TEST(NextFamilyIndex, SingleAttemptStaysAtZero)
{
	EXPECT_EQ(NextFamilyIndex(0, 1), 0);
}

TEST(NextFamilyIndex, DegenerateCountsStayAtZero)
{
	EXPECT_EQ(NextFamilyIndex(0, 0), 0);
	EXPECT_EQ(NextFamilyIndex(3, -1), 0);
	EXPECT_EQ(NextFamilyIndex(-5, 2), 0);
}

// -- HostHeaderValue / ExtractRequestTarget / ExtractPortOrDefault ------

TEST(HostHeaderValue, DefaultPortOmitted)
{
	EXPECT_EQ(HostHeaderValue(IngestUrl), "ingest.framedash.dev");
	EXPECT_EQ(HostHeaderValue("https://ingest.framedash.dev:443/v1/events"), "ingest.framedash.dev");
}

TEST(HostHeaderValue, NonDefaultPortIncluded)
{
	EXPECT_EQ(HostHeaderValue("https://ingest.example.com:8443/x"), "ingest.example.com:8443");
}

TEST(ExtractRequestTarget, PathAndQueryKept)
{
	EXPECT_EQ(ExtractRequestTarget("https://h.example/v1/events?a=b&c=d"), "/v1/events?a=b&c=d");
}

TEST(ExtractRequestTarget, NoPathIsRoot)
{
	EXPECT_EQ(ExtractRequestTarget("https://h.example"), "/");
	EXPECT_EQ(ExtractRequestTarget("https://h.example:8443"), "/");
}

TEST(ExtractRequestTarget, FragmentDropped)
{
	EXPECT_EQ(ExtractRequestTarget("https://h.example/v1/events#frag"), "/v1/events");
	EXPECT_EQ(ExtractRequestTarget("https://h.example#frag"), "/");
}

TEST(ExtractRequestTarget, QueryWithoutPathGetsLeadingSlash)
{
	EXPECT_EQ(ExtractRequestTarget("https://h.example?a=b"), "/?a=b");
}

TEST(ExtractPortOrDefault, DefaultAndExplicit)
{
	EXPECT_EQ(ExtractPortOrDefault(IngestUrl), 443);
	EXPECT_EQ(ExtractPortOrDefault("https://h.example:8443/x"), 8443);
	EXPECT_EQ(ExtractPortOrDefault("https://[2606:4700::1]:8443/x"), 8443);
}

TEST(ExtractPortOrDefault, MalformedPortIsNegative)
{
	EXPECT_EQ(ExtractPortOrDefault("https://h.example:0/x"), -1);
	EXPECT_EQ(ExtractPortOrDefault("https://h.example:65536/x"), -1);
	EXPECT_EQ(ExtractPortOrDefault("https://h.example:8a/x"), -1);
	EXPECT_EQ(ExtractPortOrDefault("https://h.example:/x"), -1);
}
