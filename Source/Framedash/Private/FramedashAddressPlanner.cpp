// Copyright 2026 Crane Valley. All Rights Reserved.

#include "FramedashAddressPlanner.h"

#include "FramedashEndpointSecurity.h"

namespace Framedash
{
namespace
{
	bool StartsWithCaseInsensitiveAscii(std::string_view Str, std::string_view Prefix)
	{
		if (Str.size() < Prefix.size())
		{
			return false;
		}
		for (std::size_t I = 0; I < Prefix.size(); ++I)
		{
			char A = Str[I];
			char B = Prefix[I];
			if (A >= 'A' && A <= 'Z')
			{
				A = static_cast<char>(A - 'A' + 'a');
			}
			if (B >= 'A' && B <= 'Z')
			{
				B = static_cast<char>(B - 'A' + 'a');
			}
			if (A != B)
			{
				return false;
			}
		}
		return true;
	}

	// The userinfo-stripped authority ("host", "host:port", "[v6]", or
	// "[v6]:port"). Same bounding rules as ExtractUrlHost (scheme up to "://",
	// authority up to the first of "/?#\\", userinfo up to the last '@') so the
	// host and port halves are parsed consistently with the security check.
	std::string_view PlannerExtractAuthority(std::string_view Url)
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

	// Explicit port digits, or empty when none is present. Empty is also
	// returned for a malformed IPv6 authority; the caller distinguishes
	// "absent" (default) from "present but invalid" via HasExplicitPort.
	std::string_view ExtractExplicitPort(std::string_view Url, bool& bOutHasExplicitPort)
	{
		bOutHasExplicitPort = false;
		std::string_view Authority = PlannerExtractAuthority(Url);
		if (Authority.empty())
		{
			return {};
		}

		if (Authority.front() == '[')
		{
			const std::size_t Close = Authority.find(']');
			if (Close == std::string_view::npos)
			{
				return {};
			}
			std::string_view AfterClose = Authority.substr(Close + 1);
			if (!AfterClose.empty() && AfterClose.front() == ':')
			{
				bOutHasExplicitPort = true;
				return AfterClose.substr(1);
			}
			return {};
		}

		const std::size_t ColonIndex = Authority.find(':');
		if (ColonIndex == std::string_view::npos)
		{
			return {};
		}
		bOutHasExplicitPort = true;
		return Authority.substr(ColonIndex + 1);
	}

	// Every character is an ASCII digit or '.', which is how an IPv4 literal
	// (or a bare-number host, which curl also treats as an address) looks.
	// DNS hostnames always contain at least one alphabetic character or '-'.
	bool LooksLikeIPv4Literal(std::string_view Host)
	{
		if (Host.empty())
		{
			return false;
		}
		for (const char Ch : Host)
		{
			if (!((Ch >= '0' && Ch <= '9') || Ch == '.'))
			{
				return false;
			}
		}
		return true;
	}

	std::string StripBrackets(std::string_view Ip)
	{
		if (Ip.size() >= 2 && Ip.front() == '[' && Ip.back() == ']')
		{
			return std::string(Ip.substr(1, Ip.size() - 2));
		}
		return std::string(Ip);
	}
}

bool ShouldForceAddressFamily(std::string_view EndpointUrl)
{
	if (EndpointUrl.empty())
	{
		return false;
	}

	// Reject control characters plus the '@'/'\' host-parsing differentials
	// (same rationale as IsEndpointSecure): the fallback must never engage on
	// a URL whose host this parser and the HTTP client could disagree about.
	for (const char Ch : EndpointUrl)
	{
		const unsigned char Byte = static_cast<unsigned char>(Ch);
		if (Byte < 0x20 || Byte == 0x7f || Ch == '@' || Ch == '\\')
		{
			return false;
		}
	}

	// HTTP endpoints are loopback-only (EndpointSecurity) -- no IPv6-blackhole
	// exposure and forcing would only risk breaking local dev.
	if (!StartsWithCaseInsensitiveAscii(EndpointUrl, "https://"))
	{
		return false;
	}

	const std::string Host = ExtractUrlHost(EndpointUrl);
	if (Host.empty())
	{
		return false;
	}
	// Skip IP literals (already pinned to one address) and localhost.
	if (Host == "localhost")
	{
		return false;
	}
	if (Host.front() == '[')
	{
		return false;
	}
	if (LooksLikeIPv4Literal(Host))
	{
		return false;
	}

	// An explicit-but-malformed port can never be connected to; keep the
	// engine path (which will fail it consistently) instead of engaging.
	if (ExtractPortOrDefault(EndpointUrl) < 0)
	{
		return false;
	}

	return true;
}

FAddressPlan BuildAddressPlan(
	std::string_view EndpointUrl,
	std::string_view ResolvedIPv4,
	std::string_view ResolvedIPv6)
{
	FAddressPlan Plan;
	if (!ShouldForceAddressFamily(EndpointUrl))
	{
		return Plan; // passthrough
	}

	// IPv4 first, IPv6 second -- see FAddressPlan for the ordering rationale.
	if (!ResolvedIPv4.empty())
	{
		Plan.Attempts.push_back(FAddressAttempt{EIpFamily::IPv4, StripBrackets(ResolvedIPv4)});
	}
	if (!ResolvedIPv6.empty())
	{
		Plan.Attempts.push_back(FAddressAttempt{EIpFamily::IPv6, StripBrackets(ResolvedIPv6)});
	}
	if (Plan.Attempts.empty())
	{
		return Plan; // nothing resolved -> passthrough
	}

	Plan.HostHeader = HostHeaderValue(EndpointUrl);
	Plan.CommonName = ExtractUrlHost(EndpointUrl);
	Plan.RequestTarget = ExtractRequestTarget(EndpointUrl);
	Plan.Port = ExtractPortOrDefault(EndpointUrl);
	return Plan;
}

int NextFamilyIndex(int CurrentIndex, int AttemptCount)
{
	if (AttemptCount <= 0)
	{
		return 0;
	}
	const int Next = (CurrentIndex + 1) % AttemptCount;
	// Defensive: a (public-API) negative CurrentIndex would produce a negative
	// C++ remainder; clamp back to the preferred family.
	return Next < 0 ? 0 : Next;
}

std::string HostHeaderValue(std::string_view EndpointUrl)
{
	const std::string Host = ExtractUrlHost(EndpointUrl);
	if (Host.empty())
	{
		return {};
	}
	const int Port = ExtractPortOrDefault(EndpointUrl);
	if (Port == 443)
	{
		return Host;
	}
	if (Port < 0)
	{
		return {};
	}
	return Host + ":" + std::to_string(Port);
}

std::string ExtractRequestTarget(std::string_view EndpointUrl)
{
	const std::size_t SchemeEnd = EndpointUrl.find("://");
	std::string_view Remainder = (SchemeEnd == std::string_view::npos)
		? EndpointUrl
		: EndpointUrl.substr(SchemeEnd + 3);

	const std::size_t AuthorityEnd = Remainder.find_first_of("/?#");
	if (AuthorityEnd == std::string_view::npos)
	{
		return "/";
	}

	std::string_view Target = Remainder.substr(AuthorityEnd);
	// The fragment is client-side only and never sent in a request target.
	const std::size_t Fragment = Target.find('#');
	if (Fragment != std::string_view::npos)
	{
		Target = Target.substr(0, Fragment);
	}
	if (Target.empty())
	{
		return "/";
	}
	// A query with no path ("https://host?a=b") still needs the origin-form
	// leading slash (RFC 9112 3.2.1).
	if (Target.front() != '/')
	{
		return "/" + std::string(Target);
	}
	return std::string(Target);
}

int ExtractPortOrDefault(std::string_view EndpointUrl)
{
	bool bHasExplicitPort = false;
	const std::string_view PortDigits = ExtractExplicitPort(EndpointUrl, bHasExplicitPort);
	if (!bHasExplicitPort)
	{
		return 443;
	}
	if (PortDigits.empty() || PortDigits.size() > 5)
	{
		return -1;
	}
	int Port = 0;
	for (const char Ch : PortDigits)
	{
		if (Ch < '0' || Ch > '9')
		{
			return -1;
		}
		Port = Port * 10 + (Ch - '0');
	}
	if (Port < 1 || Port > 65535)
	{
		return -1;
	}
	return Port;
}
}
