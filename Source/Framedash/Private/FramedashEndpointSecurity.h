// Copyright Crane Valley. All Rights Reserved.
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
}
