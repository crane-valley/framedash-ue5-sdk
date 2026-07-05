// Copyright Crane Valley. All Rights Reserved.
//
// Standalone unit tests for the raw HTTP/1.1 helpers the direct-socket
// fallback transport uses (FramedashRawHttp). Mirrors the Unity SDK
// RawHttpMessage tests (#1218): request-head shape, header-value and
// request-target sanitization (request-smuggling hygiene), and incremental
// status-line parsing. Runs without an UnrealEditor build.

#include "FramedashRawHttp.h"

#include <string>

#include <gtest/gtest.h>

using Framedash::BuildPostHead;
using Framedash::SanitizeHeaderValue;
using Framedash::SanitizeRequestTarget;
using Framedash::TryParseStatusCode;

namespace
{
	bool Parse(const std::string& Text, int& OutCode)
	{
		return TryParseStatusCode(Text.data(), Text.size(), OutCode);
	}
}

// -- BuildPostHead ------------------------------------------------------

TEST(BuildPostHead, GzippedHeadMatchesWireShape)
{
	const std::string Head = BuildPostHead(
		"/v1/events", "ingest.framedash.dev", "fd_key_123", "framedash-ue5/0.1.5", 1234, true);
	EXPECT_EQ(Head,
		"POST /v1/events HTTP/1.1\r\n"
		"Host: ingest.framedash.dev\r\n"
		"Content-Type: application/x-protobuf\r\n"
		"Content-Encoding: gzip\r\n"
		"X-API-Key: fd_key_123\r\n"
		"X-SDK-Version: framedash-ue5/0.1.5\r\n"
		"Content-Length: 1234\r\n"
		"Connection: close\r\n"
		"\r\n");
}

TEST(BuildPostHead, UncompressedHeadOmitsContentEncoding)
{
	const std::string Head = BuildPostHead(
		"/v1/events", "ingest.framedash.dev", "k", "v", 10, false);
	EXPECT_EQ(Head.find("Content-Encoding"), std::string::npos);
	EXPECT_NE(Head.find("Content-Length: 10\r\n"), std::string::npos);
}

TEST(BuildPostHead, CrLfInHeaderValuesCannotSplitRequest)
{
	// An injected CRLF in a developer-supplied value must not create an
	// extra header line (request-smuggling shape).
	const std::string Head = BuildPostHead(
		"/v1/events", "h.example", "evil\r\nX-Injected: 1", "v\r\n\r\nGET / HTTP/1.1", 5, true);
	// The injected text may survive INSIDE the value, but never as its own
	// header line (no CR/LF can remain in the value).
	EXPECT_EQ(Head.find("\r\nX-Injected"), std::string::npos);
	EXPECT_NE(Head.find("X-API-Key: evilX-Injected: 1\r\n"), std::string::npos);
	EXPECT_NE(Head.find("X-SDK-Version: vGET / HTTP/1.1\r\n"), std::string::npos);
}

TEST(BuildPostHead, SpaceInTargetCannotSplitRequestLine)
{
	const std::string Head = BuildPostHead(
		"/v1/events HTTP/1.1\r\nGET /steal", "h.example", "k", "v", 5, true);
	EXPECT_EQ(Head.substr(0, Head.find("\r\n")), "POST /v1/eventsHTTP/1.1GET/steal HTTP/1.1");
}

// -- SanitizeRequestTarget ----------------------------------------------

TEST(SanitizeRequestTarget, PassthroughForNormalTarget)
{
	EXPECT_EQ(SanitizeRequestTarget("/v1/events?a=b"), "/v1/events?a=b");
}

TEST(SanitizeRequestTarget, StripsControlsAndSpaces)
{
	EXPECT_EQ(SanitizeRequestTarget("/v1/ev ents\r\n\t"), "/v1/events");
	EXPECT_EQ(SanitizeRequestTarget("/a\x7f" "b"), "/ab");
}

TEST(SanitizeRequestTarget, ForcesOriginFormLeadingSlash)
{
	EXPECT_EQ(SanitizeRequestTarget("v1/events"), "/v1/events");
	EXPECT_EQ(SanitizeRequestTarget(""), "/");
	EXPECT_EQ(SanitizeRequestTarget(" \r\n"), "/");
}

// -- SanitizeHeaderValue --------------------------------------------------

TEST(SanitizeHeaderValue, KeepsSpacesStripsControls)
{
	EXPECT_EQ(SanitizeHeaderValue("plain value"), "plain value");
	EXPECT_EQ(SanitizeHeaderValue("a\r\nb\tc\x7f" "d"), "abcd");
	EXPECT_EQ(SanitizeHeaderValue(""), "");
}

// -- TryParseStatusCode ---------------------------------------------------

TEST(TryParseStatusCode, ParsesCompleteStatusLine)
{
	int Code = 0;
	EXPECT_TRUE(Parse("HTTP/1.1 202 Accepted\r\nServer: cf\r\n", Code));
	EXPECT_EQ(Code, 202);
}

TEST(TryParseStatusCode, ParsesLfOnlyAndNoReasonPhrase)
{
	int Code = 0;
	EXPECT_TRUE(Parse("HTTP/1.1 429\n", Code));
	EXPECT_EQ(Code, 429);
}

TEST(TryParseStatusCode, ParsesHttp10Intermediary)
{
	int Code = 0;
	EXPECT_TRUE(Parse("HTTP/1.0 502 Bad Gateway\r\n", Code));
	EXPECT_EQ(Code, 502);
}

TEST(TryParseStatusCode, IncompleteLineIsNotYet)
{
	int Code = 0;
	EXPECT_FALSE(Parse("HTTP/1.1 202 Accep", Code));
	EXPECT_FALSE(Parse("", Code));
	EXPECT_EQ(Code, 0);
}

TEST(TryParseStatusCode, NonHttpLineIsRejected)
{
	int Code = 0;
	EXPECT_FALSE(Parse("SSH-2.0-OpenSSH\r\n", Code));
	EXPECT_FALSE(Parse("HTTP2 202\r\n", Code));
}

TEST(TryParseStatusCode, MalformedCodesAreRejected)
{
	int Code = 0;
	EXPECT_FALSE(Parse("HTTP/1.1 20a Accepted\r\n", Code));
	EXPECT_FALSE(Parse("HTTP/1.1 20 Accepted\r\n", Code));
	EXPECT_FALSE(Parse("HTTP/1.1 2022 Accepted\r\n", Code));
	EXPECT_FALSE(Parse("HTTP/1.1 099 Accepted\r\n", Code));
	EXPECT_FALSE(Parse("HTTP/1.1\r\n", Code));
	EXPECT_FALSE(Parse("HTTP/1.1 \r\n", Code));
}

TEST(TryParseStatusCode, NullBufferIsRejected)
{
	int Code = 0;
	EXPECT_FALSE(TryParseStatusCode(nullptr, 10, Code));
}
