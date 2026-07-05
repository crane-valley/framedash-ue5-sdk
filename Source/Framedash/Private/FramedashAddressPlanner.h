// Copyright Crane Valley. All Rights Reserved.
//
// Engine-independent planner for the prefer-IPv4-with-IPv6-fallback ingest
// connect (parity with the Unity/Godot EndpointAddressPlanner, #1218/#1216).
// It never resolves DNS itself -- the caller performs the actual resolution
// (ISocketSubsystem::GetAddressInfo on a background thread) and passes the
// first resolved address of each family in -- so the address-family ordering
// and endpoint-qualification logic is unit-testable under the GoogleTest
// harness in sdks/ue5/Tests/ without booting the engine.
//
// Divergence from the Unity planner (justified): Unity rewrites the endpoint
// URL into IP-literal URLs because its fallback still flows through a
// URL-taking API surface. The UE5 fallback connects an FSocket straight to
// (address, port), so the plan carries the resolved IP literals, the port,
// the Host header, the TLS common name, and the origin-form request target
// instead of rewritten URLs -- same semantics, no URL re-parsing.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace Framedash
{
	/** Address family for a delivery attempt. */
	enum class EIpFamily
	{
		IPv4,
		IPv6,
	};

	/** One direct-socket connect candidate: a resolved, unbracketed IP literal. */
	struct FAddressAttempt
	{
		EIpFamily Family = EIpFamily::IPv4;
		std::string IpLiteral;
	};

	/**
	 * Immutable delivery plan for the direct-socket fallback. Attempts are
	 * ordered IPv4 first (broken-IPv6 networks -- a global AAAA via Router
	 * Advertisement with no working IPv6 route -- are the dominant real-world
	 * failure and Cloudflare IPv4 anycast is near-universally reachable), IPv6
	 * second (so an IPv6-only network still delivers when IPv4 fails).
	 *
	 * A passthrough plan (empty Attempts) means the fallback must NOT engage:
	 * loopback / IP-literal / non-HTTPS endpoints, or a resolution that yielded
	 * nothing -- the engine HTTP stack keeps handling those unchanged.
	 */
	struct FAddressPlan
	{
		std::vector<FAddressAttempt> Attempts;
		/** Host header value: "host" or "host:port" for a non-default port. */
		std::string HostHeader;
		/** FQDN used for BOTH SNI and certificate hostname verification. */
		std::string CommonName;
		/** Origin-form request target (path + query, leading '/'). */
		std::string RequestTarget;
		/** TCP port to connect to (443 unless the URL carries an explicit port). */
		int Port = 443;

		bool IsPassthrough() const { return Attempts.empty(); }
	};

	/**
	 * Whether the direct-socket fallback applies to this endpoint at all. Only
	 * real remote HTTPS hostnames benefit; loopback / IP-literal / non-HTTPS
	 * endpoints (plain HTTP is already loopback-only per EndpointSecurity) pass
	 * through unchanged so local dev and self-hosted-by-IP deployments keep
	 * working. Also rejects URLs containing control characters, '@', or '\'
	 * (the same libcurl host-parsing differentials IsEndpointSecure rejects)
	 * and an explicit-but-invalid port.
	 */
	bool ShouldForceAddressFamily(std::string_view EndpointUrl);

	/**
	 * Build the delivery plan. ResolvedIPv4 / ResolvedIPv6 are the first
	 * resolved address of each family (empty when that family did not resolve;
	 * IPv6 accepted with or without brackets and stored unbracketed). Returns a
	 * passthrough plan when the endpoint does not qualify or neither family
	 * resolved, so the caller falls back to the engine HTTP stack rather than
	 * dropping the batch.
	 */
	FAddressPlan BuildAddressPlan(
		std::string_view EndpointUrl,
		std::string_view ResolvedIPv4,
		std::string_view ResolvedIPv6);

	/**
	 * Next attempt index after a transport-level failure (status 0) of the
	 * fallback itself: a modulo TOGGLE, not advance-and-clamp. Wrapping matters
	 * on a broken-IPv6 network -- after a single transient IPv4 glitch pushes
	 * selection to IPv6 (a blackhole), advance-and-clamp would wedge every
	 * remaining retry on IPv6 for the full timeout; wrapping returns to the
	 * preferred IPv4 on the next attempt. For a single-attempt plan the modulo
	 * keeps the index at 0 (no-op).
	 */
	int NextFamilyIndex(int CurrentIndex, int AttemptCount);

	/** "host" for the default HTTPS port, otherwise "host:port". Empty when unparseable. */
	std::string HostHeaderValue(std::string_view EndpointUrl);

	/**
	 * Origin-form request target (path + query, fragment dropped, leading '/'
	 * enforced). "/" when the URL has no path.
	 */
	std::string ExtractRequestTarget(std::string_view EndpointUrl);

	/**
	 * Explicit port when present and valid (1..65535), 443 when absent, -1 when
	 * an explicit port is present but malformed (non-numeric / out of range).
	 */
	int ExtractPortOrDefault(std::string_view EndpointUrl);
}
