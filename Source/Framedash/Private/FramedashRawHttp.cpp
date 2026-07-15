// Copyright 2026 Crane Valley. All Rights Reserved.

#include "FramedashRawHttp.h"

namespace Framedash
{
namespace
{
	// Shared control-stripping core. bRejectSpace additionally strips SP
	// (request-target rule); header values keep SP (0x20) legal.
	std::string StripControls(std::string_view Value, bool bRejectSpace)
	{
		std::string Out;
		Out.reserve(Value.size());
		for (const char Ch : Value)
		{
			const unsigned char Byte = static_cast<unsigned char>(Ch);
			const bool bOk = bRejectSpace
				? (Byte > 0x20 && Byte != 0x7f)
				: (Byte >= 0x20 && Byte != 0x7f);
			if (bOk)
			{
				Out.push_back(Ch);
			}
		}
		return Out;
	}
}

std::string BuildPostHead(
	std::string_view RequestTarget,
	std::string_view HostHeader,
	std::string_view ApiKey,
	std::string_view SdkVersion,
	long long ContentLength,
	bool bGzipped)
{
	std::string Head;
	Head.reserve(256);
	Head += "POST ";
	Head += SanitizeRequestTarget(RequestTarget);
	Head += " HTTP/1.1\r\n";
	Head += "Host: ";
	Head += SanitizeHeaderValue(HostHeader);
	Head += "\r\n";
	Head += "Content-Type: application/x-protobuf\r\n";
	if (bGzipped)
	{
		Head += "Content-Encoding: gzip\r\n";
	}
	Head += "X-API-Key: ";
	Head += SanitizeHeaderValue(ApiKey);
	Head += "\r\n";
	Head += "X-SDK-Version: ";
	Head += SanitizeHeaderValue(SdkVersion);
	Head += "\r\n";
	Head += "Content-Length: ";
	Head += std::to_string(ContentLength);
	Head += "\r\n";
	Head += "Connection: close\r\n";
	Head += "\r\n";
	return Head;
}

std::string SanitizeRequestTarget(std::string_view RequestTarget)
{
	std::string Clean = StripControls(RequestTarget, /*bRejectSpace*/ true);
	if (Clean.empty())
	{
		return "/";
	}
	if (Clean.front() != '/')
	{
		return "/" + Clean;
	}
	return Clean;
}

std::string SanitizeHeaderValue(std::string_view Value)
{
	return StripControls(Value, /*bRejectSpace*/ false);
}

bool TryParseStatusCode(const char* Buffer, std::size_t Count, int& OutStatusCode)
{
	OutStatusCode = 0;
	if (Buffer == nullptr)
	{
		return false;
	}

	// The status line ends at the first LF; without one the line may still be
	// arriving, so report "not yet" and let the caller read more bytes.
	std::size_t LineEnd = Count;
	for (std::size_t I = 0; I < Count; ++I)
	{
		if (Buffer[I] == '\n')
		{
			LineEnd = I;
			break;
		}
	}
	if (LineEnd == Count)
	{
		return false;
	}

	std::string_view Line(Buffer, LineEnd);
	if (!Line.empty() && Line.back() == '\r')
	{
		Line = Line.substr(0, Line.size() - 1);
	}

	// "HTTP/<version> <code> [reason]" -- accept any version token so an
	// HTTP/1.0 status line from an intermediary still parses.
	if (Line.size() < 5 || Line.substr(0, 5) != "HTTP/")
	{
		return false;
	}
	const std::size_t FirstSpace = Line.find(' ');
	if (FirstSpace == std::string_view::npos || FirstSpace + 1 >= Line.size())
	{
		return false;
	}

	const std::size_t CodeEnd = Line.find(' ', FirstSpace + 1);
	const std::string_view CodeToken = (CodeEnd == std::string_view::npos)
		? Line.substr(FirstSpace + 1)
		: Line.substr(FirstSpace + 1, CodeEnd - FirstSpace - 1);

	if (CodeToken.size() != 3)
	{
		return false;
	}
	int Code = 0;
	for (const char Ch : CodeToken)
	{
		if (Ch < '0' || Ch > '9')
		{
			return false;
		}
		Code = Code * 10 + (Ch - '0');
	}
	if (Code < 100 || Code > 999)
	{
		return false;
	}

	OutStatusCode = Code;
	return true;
}
}
