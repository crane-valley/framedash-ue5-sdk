// Copyright Crane Valley. All Rights Reserved.

#include "FramedashEndpointSecurity.h"

#include <algorithm>

namespace Framedash
{
namespace
{
	// ASCII-only lowercase. Deliberately NOT std::tolower, which is locale-
	// dependent (e.g. a Turkish locale maps 'I' to a dotless 'i') and could make
	// a host-equality security check behave differently per machine locale.
	char ToLowerAscii(char C)
	{
		return (C >= 'A' && C <= 'Z') ? static_cast<char>(C - 'A' + 'a') : C;
	}

	std::string ToLower(std::string_view Str)
	{
		std::string Out(Str);
		std::transform(Out.begin(), Out.end(), Out.begin(), ToLowerAscii);
		return Out;
	}

	bool StartsWithCaseInsensitive(std::string_view Str, std::string_view Prefix)
	{
		if (Str.size() < Prefix.size())
		{
			return false;
		}
		for (std::size_t I = 0; I < Prefix.size(); ++I)
		{
			if (ToLowerAscii(Str[I]) != ToLowerAscii(Prefix[I]))
			{
				return false;
			}
		}
		return true;
	}

	// The userinfo-stripped authority ("host" or "host:port", or "[v6]" /
	// "[v6]:port"). Uses the same scheme/authority/userinfo bounding rules as
	// ExtractUrlHost so the host and port halves are parsed consistently.
	std::string_view ExtractAuthority(std::string_view Url)
	{
		const std::size_t SchemeEnd = Url.find("://");
		std::string_view Remainder =
			(SchemeEnd == std::string_view::npos) ? Url : Url.substr(SchemeEnd + 3);

		const std::size_t AuthorityEnd = Remainder.find_first_of("/?#\\");
		std::string_view Authority = (AuthorityEnd == std::string_view::npos)
			? Remainder
			: Remainder.substr(0, AuthorityEnd);

		const std::size_t AtIndex = Authority.find_last_of('@');
		if (AtIndex != std::string_view::npos)
		{
			Authority = Authority.substr(AtIndex + 1);
		}
		return Authority;
	}

	// Lowercased scheme, or empty when the URL has no "scheme://" prefix.
	std::string ExtractUrlScheme(std::string_view Url)
	{
		const std::size_t SchemeEnd = Url.find("://");
		if (SchemeEnd == std::string_view::npos)
		{
			return {};
		}
		return ToLower(Url.substr(0, SchemeEnd));
	}

	// Explicit port digits, or empty when none is present (caller defaults by
	// scheme). Returns empty on a malformed IPv6 authority.
	std::string ExtractUrlPort(std::string_view Url)
	{
		std::string_view Authority = ExtractAuthority(Url);
		if (Authority.empty())
		{
			return {};
		}

		if (Authority.front() == '[')
		{
			const std::size_t Close = Authority.find(']');
			if (Close == std::string_view::npos)
			{
				return {}; // unterminated bracket -> no usable port
			}
			std::string_view AfterClose = Authority.substr(Close + 1);
			if (AfterClose.size() > 1 && AfterClose.front() == ':')
			{
				return std::string(AfterClose.substr(1));
			}
			return {};
		}

		const std::size_t ColonIndex = Authority.find(':');
		if (ColonIndex == std::string_view::npos)
		{
			return {};
		}
		return std::string(Authority.substr(ColonIndex + 1));
	}

	// Normalized security origin "scheme://host:port" with default ports applied
	// (http -> 80, https -> 443). Empty when the scheme or host is missing, which
	// the caller treats as "unparseable -> do not flag".
	std::string ExtractUrlOrigin(std::string_view Url)
	{
		const std::string Scheme = ExtractUrlScheme(Url);
		const std::string Host = ExtractUrlHost(Url);
		if (Scheme.empty() || Host.empty())
		{
			return {};
		}

		std::string Port = ExtractUrlPort(Url);
		if (Port.empty())
		{
			if (Scheme == "http")
			{
				Port = "80";
			}
			else if (Scheme == "https")
			{
				Port = "443";
			}
		}

		return Scheme + "://" + Host + ":" + Port;
	}
}

std::string ExtractUrlHost(std::string_view Url)
{
	// Strip the scheme: everything up to and including "://".
	const std::size_t SchemeEnd = Url.find("://");
	std::string_view Remainder =
		(SchemeEnd == std::string_view::npos) ? Url : Url.substr(SchemeEnd + 3);

	// The authority ends at the first '/', '?', '#', or '\'. The backslash is
	// required: WHATWG treats '\' as equivalent to '/' for special schemes, so
	// "http://evil.com\@localhost/x" has host "evil.com" -- omitting it would
	// extract the trailing "localhost" label and wrongly grant the loopback
	// HTTP exemption while the client connects to evil.com in cleartext.
	const std::size_t AuthorityEnd = Remainder.find_first_of("/?#\\");
	std::string_view Authority = (AuthorityEnd == std::string_view::npos)
		? Remainder
		: Remainder.substr(0, AuthorityEnd);

	// Drop any userinfo: "user:pass@host" -> "host". Use the last '@' so a
	// userinfo segment that itself contains '@' cannot smuggle in a fake host.
	const std::size_t AtIndex = Authority.find_last_of('@');
	if (AtIndex != std::string_view::npos)
	{
		Authority = Authority.substr(AtIndex + 1);
	}

	// IPv6 literal: "[::1]" or "[::1]:port" -> "[::1]" (the bracketed form). Any
	// text between the closing ']' and the optional ":port" (e.g.
	// "[::1].evil.example") is malformed -- fail closed so a non-loopback host
	// cannot masquerade as the loopback literal.
	if (!Authority.empty() && Authority.front() == '[')
	{
		const std::size_t Close = Authority.find(']');
		if (Close == std::string_view::npos)
		{
			return {}; // unterminated bracket -> malformed
		}
		const std::string_view AfterClose = Authority.substr(Close + 1);
		if (!AfterClose.empty() && AfterClose.front() != ':')
		{
			return {}; // trailing text after ']' that is not a port -> malformed
		}
		return ToLower(Authority.substr(0, Close + 1));
	}

	// Strip the port: "host:port" -> "host".
	const std::size_t ColonIndex = Authority.find(':');
	if (ColonIndex != std::string_view::npos)
	{
		Authority = Authority.substr(0, ColonIndex);
	}

	return ToLower(Authority);
}

bool IsEndpointSecure(std::string_view Url)
{
	// Reject any control character (including an embedded NUL): a real URL never
	// contains raw control bytes, and a NUL could otherwise truncate a downstream
	// copy of the URL and open a parser differential with the HTTP client.
	for (const char Ch : Url)
	{
		const unsigned char Byte = static_cast<unsigned char>(Ch);
		if (Byte < 0x20 || Byte == 0x7f)
		{
			return false;
		}
	}

	// Reject '@' (userinfo) and '\' anywhere in the URL. Telemetry endpoints never
	// contain them, and HTTP clients disagree on how they bound the host: WHATWG
	// (and our parser) treat '\' as '/', while libcurl (RFC 3986, the UE5/Unity
	// HTTP backend on most platforms) treats '\' literally and splits the host at
	// the last '@'. So "http://localhost\@evil.com" parses here as host localhost
	// (loopback -> would grant cleartext HTTP) yet libcurl connects to evil.com.
	// Refusing both characters closes the differential for every scheme.
	if (Url.find('@') != std::string_view::npos || Url.find('\\') != std::string_view::npos)
	{
		return false;
	}

	// The exemption is for CLEARTEXT HTTP to a loopback dev host only, so require
	// the "http://" scheme explicitly -- otherwise "ftp://localhost" or
	// "file://localhost" would pass the gate. The host allowlist is a deliberately
	// tight set of the three canonical loopback forms, NOT a general IsLoopback():
	// non-canonical loopbacks (127.0.0.2, 127.1, "[::0001]", "localhost.") are
	// intentionally NOT exempted and must use HTTPS. Keeping it exact avoids
	// parsing every loopback spelling (attack surface) for no real dev benefit.
	if (StartsWithCaseInsensitive(Url, "http://"))
	{
		const std::string Host = ExtractUrlHost(Url);
		if (Host == "localhost" || Host == "127.0.0.1" || Host == "[::1]")
		{
			return true;
		}
	}
	return StartsWithCaseInsensitive(Url, "https://");
}

bool IsCrossOriginRedirect(std::string_view ConfiguredUrl, std::string_view EffectiveUrl)
{
	// Fast path: an identical URL (the common no-redirect case, since GetEffectiveURL
	// returns the request URL when nothing was redirected) is the same origin --
	// skip parsing and allocation entirely.
	if (ConfiguredUrl == EffectiveUrl)
	{
		return false;
	}

	// No effective URL reported (a backend that does not populate it): cannot
	// reason about origin, so treat as same origin and never drop legitimate
	// telemetry. This is defense in depth, not the primary gate.
	if (EffectiveUrl.empty())
	{
		return false;
	}

	// A control character, '@' (userinfo), or '\' in the effective URL is the same
	// libcurl host-parsing differential IsEndpointSecure rejects on the configured
	// URL: libcurl splits the host at the LAST '@' and treats '\' literally, while
	// this WHATWG-style parser stops the authority at '\'. So an effective URL like
	// "https://ingest.example\@evil/..." would parse here as the configured host yet
	// libcurl connected to evil. Fail CLOSED (treat as cross origin) rather than
	// risk a missed leak -- a legitimate telemetry URL never contains these.
	for (const char Ch : EffectiveUrl)
	{
		const unsigned char Byte = static_cast<unsigned char>(Ch);
		if (Byte < 0x20 || Byte == 0x7f || Ch == '@' || Ch == '\\')
		{
			return true;
		}
	}

	const std::string ConfiguredOrigin = ExtractUrlOrigin(ConfiguredUrl);
	const std::string EffectiveOrigin = ExtractUrlOrigin(EffectiveUrl);

	// Fail open only when an origin cannot be determined at all (missing scheme or
	// host) rather than dropping telemetry on a value we cannot parse. A parseable
	// but different origin -- including a malformed/non-numeric port, which never
	// appears in a legitimate landing URL -- fails closed below.
	if (ConfiguredOrigin.empty() || EffectiveOrigin.empty())
	{
		return false;
	}

	return ConfiguredOrigin != EffectiveOrigin;
}
}
