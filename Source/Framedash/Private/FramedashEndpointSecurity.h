// Copyright 2026 Crane Valley. All Rights Reserved.
//
// Engine-independent endpoint URL security checks. Deliberately free of
// Unreal types (FString, etc.) so the host-parsing logic can be unit-tested
// under the GoogleTest harness in sdks/ue5/Tests/ without booting the engine.

#pragma once

#include <string>
#include <string_view>

namespace Framedash
{
	/**
	 * Extract the lowercased host from a URL, dropping the scheme, userinfo,
	 * port, and path/query/fragment. IPv6 literals keep their brackets
	 * (e.g. "[::1]"). Returns an empty string when no host component is present.
	 */
	std::string ExtractUrlHost(std::string_view Url);

	/**
	 * Whether it is safe to send the API key to this endpoint. HTTPS is allowed
	 * for any host; plain "http://" is allowed only for a canonical loopback host
	 * (localhost / 127.0.0.1 / [::1]) for local development. Any other scheme or
	 * host is rejected. The host is parsed and matched exactly -- an unanchored
	 * substring check would accept hostile URLs such as
	 * "http://localhost.attacker.com" or "http://evil.example/?x=localhost".
	 */
	bool IsEndpointSecure(std::string_view Url);

	/**
	 * Whether an HTTP response landed on a different security origin (scheme +
	 * host + port, with default ports normalized) than the configured endpoint --
	 * i.e. the request was redirected across a trust boundary.
	 *
	 * The UE HTTP backend (libcurl) follows 3xx redirects with
	 * CURLOPT_FOLLOWLOCATION and re-sends the X-API-Key header to the redirect
	 * target, and exposes no portable per-request toggle to disable it. The
	 * transport uses this as defense in depth: when the (final) effective URL
	 * crosses origin, the batch is dropped without persisting and a security error
	 * is logged. It does NOT prevent the one-time header transmission that the
	 * redirect already performed, and -- because only the final effective URL is
	 * available -- it does not detect a chain that leaves and returns to the
	 * configured origin.
	 *
	 * Returns true (cross origin -> fail closed) when the effective URL differs in
	 * origin, or contains a control character / '@' / '\' (the libcurl host-parsing
	 * differential IsEndpointSecure also rejects). Returns false (same origin ->
	 * fail open) when the URLs are identical, the effective URL is empty, or an
	 * origin cannot be determined (missing scheme/host) -- so a backend that does
	 * not populate the effective URL never drops legitimate telemetry.
	 */
	bool IsCrossOriginRedirect(std::string_view ConfiguredUrl, std::string_view EffectiveUrl);
}
