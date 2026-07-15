// Copyright 2026 Crane Valley. All Rights Reserved.
//
// Engine-independent HTTP/1.1 message helpers for the direct-socket
// IPv4/IPv6 fallback transport (see FramedashDirectSocketSender). Builds the
// request head for the ingest POST and parses the response status line;
// nothing else of HTTP/1.1 is needed because the transport only feeds the
// status code into FRetryPolicy (the IHttpRequest path uses GetResponseCode
// the same way) and always sends "Connection: close". No Unreal types so the
// logic is testable under the GoogleTest harness in sdks/ue5/Tests/. Mirrors
// the Unity SDK RawHttpMessage (#1218) -- keep the two in sync semantically.

#pragma once

#include <string>
#include <string_view>

namespace Framedash
{
	/**
	 * Build the ASCII request head (request line + headers + blank line) for
	 * the telemetry POST. The caller writes this followed by the payload
	 * bytes. Headers mirror the IHttpRequest path exactly (Content-Type,
	 * Content-Encoding when gzipped, X-API-Key, X-SDK-Version) plus the
	 * framing headers the raw path must supply itself: Host (the FQDN, so
	 * Cloudflare routes by hostname despite the IP-literal connect),
	 * Content-Length, and "Connection: close" (one request per connection;
	 * the parser never needs keep-alive framing).
	 */
	std::string BuildPostHead(
		std::string_view RequestTarget,
		std::string_view HostHeader,
		std::string_view ApiKey,
		std::string_view SdkVersion,
		long long ContentLength,
		bool bGzipped);

	/**
	 * Sanitize an origin-form request target for the request line. Stricter
	 * than SanitizeHeaderValue: the request line is space-delimited
	 * ("POST target HTTP/1.1"), so a SPACE in the target would split the line
	 * (request-smuggling shape) -- strip all ASCII control characters AND
	 * spaces, then force the origin-form leading "/" (RFC 9112 3.2.1).
	 * Internal callers always pass ExtractRequestTarget output (which starts
	 * with "/"), but the function is public, so the invariant is enforced
	 * here rather than assumed.
	 */
	std::string SanitizeRequestTarget(std::string_view RequestTarget);

	/**
	 * Strip CR/LF and other ASCII control characters from a header value so a
	 * malformed developer-supplied value (API key) can never split the
	 * request into extra header lines (request-smuggling hygiene). The
	 * IHttpRequest path performs the equivalent validation internally.
	 * Non-ASCII bytes pass through unchanged (values are ASCII in practice).
	 */
	std::string SanitizeHeaderValue(std::string_view Value);

	/**
	 * Parse the HTTP status code out of a partially-read response buffer.
	 * Returns true once a COMPLETE status line ("HTTP/1.1 202 Accepted\r\n")
	 * is present in the first Count bytes and carries a 3-digit code in
	 * 100..999; false while the line is still incomplete OR the line is not
	 * HTTP (the caller treats never-true as a transport-level failure,
	 * status 0).
	 */
	bool TryParseStatusCode(const char* Buffer, std::size_t Count, int& OutStatusCode);
}
