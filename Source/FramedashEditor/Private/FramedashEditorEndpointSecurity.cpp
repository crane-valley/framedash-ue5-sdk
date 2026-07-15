// Copyright Crane Valley. All Rights Reserved.

#include "FramedashEditorEndpointSecurity.h"

#include <algorithm>

namespace FramedashEditor
{
namespace
{
	struct FUrlOrigin
	{
		std::string Scheme;
		std::string Host;
		std::string Port;
	};

	char ToLowerAscii(char Character)
	{
		return (Character >= 'A' && Character <= 'Z')
			? static_cast<char>(Character - 'A' + 'a')
			: Character;
	}

	std::string ToLower(std::string_view Value)
	{
		std::string Result(Value);
		std::transform(Result.begin(), Result.end(), Result.begin(), ToLowerAscii);
		return Result;
	}

	bool StartsWithCaseInsensitive(std::string_view Value, std::string_view Prefix)
	{
		if (Value.size() < Prefix.size())
		{
			return false;
		}
		for (std::size_t Index = 0; Index < Prefix.size(); ++Index)
		{
			if (ToLowerAscii(Value[Index]) != ToLowerAscii(Prefix[Index]))
			{
				return false;
			}
		}
		return true;
	}

	bool HasUnsafeUrlCharacter(std::string_view Url)
	{
		for (const char Character : Url)
		{
			const unsigned char Byte = static_cast<unsigned char>(Character);
			if (Byte < 0x20 || Byte == 0x7f)
			{
				return true;
			}
		}
		return Url.find('@') != std::string_view::npos || Url.find('\\') != std::string_view::npos;
	}

	bool TryParseOrigin(std::string_view Url, FUrlOrigin& OutOrigin)
	{
		const std::size_t SchemeEnd = Url.find("://");
		if (SchemeEnd == std::string_view::npos || SchemeEnd == 0)
		{
			return false;
		}

		OutOrigin.Scheme = ToLower(Url.substr(0, SchemeEnd));
		if (OutOrigin.Scheme != "http" && OutOrigin.Scheme != "https")
		{
			return false;
		}

		std::string_view Remainder = Url.substr(SchemeEnd + 3);
		const std::size_t AuthorityEnd = Remainder.find_first_of("/?#");
		std::string_view Authority = AuthorityEnd == std::string_view::npos
			? Remainder
			: Remainder.substr(0, AuthorityEnd);
		if (Authority.empty())
		{
			return false;
		}

		std::string_view Host;
		std::string_view Port;
		if (Authority.front() == '[')
		{
			const std::size_t Close = Authority.find(']');
			if (Close == std::string_view::npos)
			{
				return false;
			}
			Host = Authority.substr(0, Close + 1);
			const std::string_view AfterClose = Authority.substr(Close + 1);
			if (!AfterClose.empty())
			{
				if (AfterClose.front() != ':')
				{
					return false;
				}
				Port = AfterClose.substr(1);
			}
		}
		else
		{
			const std::size_t Colon = Authority.find(':');
			if (Colon == std::string_view::npos)
			{
				Host = Authority;
			}
			else
			{
				// Everything after the first colon is kept as the raw port text (no
				// extra-colon rejection, no numeric validation) -- matching the
				// runtime module's ExtractUrlPort exactly, so a malformed port (e.g.
				// "443:evil") simply fails to match a legitimate origin string
				// instead of aborting the whole parse and failing open.
				Host = Authority.substr(0, Colon);
				Port = Authority.substr(Colon + 1);
			}
		}
		if (Host.empty())
		{
			return false;
		}

		OutOrigin.Host = ToLower(Host);
		if (Port.empty())
		{
			OutOrigin.Port = OutOrigin.Scheme == "https" ? "443" : "80";
			return true;
		}
		OutOrigin.Port = std::string(Port);
		return true;
	}
}

std::string ExtractUrlHost(std::string_view Url)
{
	const std::size_t SchemeEnd = Url.find("://");
	std::string_view Remainder = SchemeEnd == std::string_view::npos
		? Url
		: Url.substr(SchemeEnd + 3);

	const std::size_t AuthorityEnd = Remainder.find_first_of("/?#\\");
	std::string_view Authority = AuthorityEnd == std::string_view::npos
		? Remainder
		: Remainder.substr(0, AuthorityEnd);

	const std::size_t AtIndex = Authority.find_last_of('@');
	if (AtIndex != std::string_view::npos)
	{
		Authority = Authority.substr(AtIndex + 1);
	}

	if (!Authority.empty() && Authority.front() == '[')
	{
		const std::size_t Close = Authority.find(']');
		if (Close == std::string_view::npos)
		{
			return {};
		}
		const std::string_view AfterClose = Authority.substr(Close + 1);
		if (!AfterClose.empty() && AfterClose.front() != ':')
		{
			return {};
		}
		return ToLower(Authority.substr(0, Close + 1));
	}

	const std::size_t ColonIndex = Authority.find(':');
	if (ColonIndex != std::string_view::npos)
	{
		Authority = Authority.substr(0, ColonIndex);
	}
	return ToLower(Authority);
}

bool IsEndpointSecure(std::string_view Url)
{
	// Userinfo, controls, and backslashes create parser differentials across UE HTTP backends.
	if (HasUnsafeUrlCharacter(Url))
	{
		return false;
	}

	if (StartsWithCaseInsensitive(Url, "http://"))
	{
		const std::string Host = ExtractUrlHost(Url);
		return Host == "localhost" || Host == "127.0.0.1" || Host == "[::1]";
	}
	return StartsWithCaseInsensitive(Url, "https://");
}

bool IsCrossOriginRedirect(std::string_view ConfiguredUrl, std::string_view EffectiveUrl)
{
	if (EffectiveUrl.empty())
	{
		return false;
	}
	if (HasUnsafeUrlCharacter(ConfiguredUrl) || HasUnsafeUrlCharacter(EffectiveUrl))
	{
		return true;
	}

	// Deliberate divergence from the runtime module's IsCrossOriginRedirect: the
	// runtime fails OPEN here to avoid dropping already-collected telemetry on an
	// unparseable origin (settled decision, #1080). That rationale does not apply
	// to this editor-only read path -- an aborted fetch just means the user clicks
	// Fetch again, so there is no telemetry-loss cost to weigh against fail-closed.
	FUrlOrigin ConfiguredOrigin;
	FUrlOrigin EffectiveOrigin;
	if (!TryParseOrigin(ConfiguredUrl, ConfiguredOrigin) ||
		!TryParseOrigin(EffectiveUrl, EffectiveOrigin))
	{
		return true;
	}
	return ConfiguredOrigin.Scheme != EffectiveOrigin.Scheme ||
		ConfiguredOrigin.Host != EffectiveOrigin.Host ||
		ConfiguredOrigin.Port != EffectiveOrigin.Port;
}
}
