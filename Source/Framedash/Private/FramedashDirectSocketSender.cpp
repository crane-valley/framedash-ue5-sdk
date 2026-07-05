// Copyright Crane Valley. All Rights Reserved.

#include "FramedashDirectSocketSender.h"

#if FRAMEDASH_WITH_DIRECT_SOCKET_TLS

#include "Framedash.h"
#include "FramedashEndpointSecurity.h"
#include "FramedashRawHttp.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Ssl.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#endif

// OpenSSL's ossl_typ.h declares "typedef struct ui_st UI", which collides
// with UE's "namespace UI" (UObject/ObjectMacros.h) in any translation unit
// that also sees CoreUObject. Rename the OpenSSL typedef for the includes --
// the same workaround engine consumers of both headers use. This SDK never
// touches the OpenSSL UI API.
#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
THIRD_PARTY_INCLUDES_END
#undef UI

#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{

std::string ToUtf8StdString(const FString& Value)
{
	if (Value.IsEmpty())
	{
		return {};
	}
	FTCHARToUTF8 Utf8(*Value);
	return std::string(Utf8.Get(), Utf8.Length());
}

double RemainingSeconds(double Deadline)
{
	return Deadline - FPlatformTime::Seconds();
}

// Drain everything OpenSSL buffered into the write BIO out to the socket.
// Returns false on a socket error or when the deadline passes. Called after
// every SSL_* operation so handshake records and alerts always hit the wire.
bool FlushOutgoing(BIO* WriteBio, FSocket& Socket, double Deadline)
{
	uint8 Buffer[4096];
	for (;;)
	{
		const int Pending = BIO_read(WriteBio, Buffer, static_cast<int>(sizeof(Buffer)));
		if (Pending <= 0)
		{
			return true; // memory BIO empty -- nothing (more) to send
		}
		int32 Offset = 0;
		while (Offset < Pending)
		{
			const double Remaining = RemainingSeconds(Deadline);
			if (Remaining <= 0.0)
			{
				return false;
			}
			if (!Socket.Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromSeconds(Remaining)))
			{
				return false;
			}
			int32 Sent = 0;
			if (!Socket.Send(Buffer + Offset, Pending - Offset, Sent) || Sent <= 0)
			{
				return false;
			}
			Offset += Sent;
		}
	}
}

// Read one chunk from the socket into the read BIO so a pending SSL_*
// operation can make progress. Returns false on error, close, or deadline.
bool PumpIncoming(BIO* ReadBio, FSocket& Socket, double Deadline)
{
	const double Remaining = RemainingSeconds(Deadline);
	if (Remaining <= 0.0)
	{
		return false;
	}
	if (!Socket.Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromSeconds(Remaining)))
	{
		return false;
	}
	uint8 Buffer[4096];
	int32 BytesRead = 0;
	if (!Socket.Recv(Buffer, static_cast<int32>(sizeof(Buffer)), BytesRead) || BytesRead <= 0)
	{
		return false; // error or orderly close
	}
	return BIO_write(ReadBio, Buffer, BytesRead) == BytesRead;
}

bool SslWriteAll(SSL* Ssl, BIO* ReadBio, BIO* WriteBio, FSocket& Socket,
	const uint8* Data, int32 Length, double Deadline)
{
	int32 Offset = 0;
	while (Offset < Length)
	{
		if (RemainingSeconds(Deadline) <= 0.0)
		{
			return false;
		}
		const int Written = SSL_write(Ssl, Data + Offset, Length - Offset);
		if (!FlushOutgoing(WriteBio, Socket, Deadline))
		{
			return false;
		}
		if (Written > 0)
		{
			Offset += Written;
			continue;
		}
		const int Error = SSL_get_error(Ssl, Written);
		if (Error == SSL_ERROR_WANT_READ)
		{
			if (!PumpIncoming(ReadBio, Socket, Deadline))
			{
				return false;
			}
		}
		else if (Error != SSL_ERROR_WANT_WRITE)
		{
			return false;
		}
	}
	return true;
}

// TLS handshake + request + status-line read over an already-connected
// socket. Returns the HTTP status code or 0 on any failure.
int32 RunTlsPost(SSL* Ssl, BIO* ReadBio, BIO* WriteBio, FSocket& Socket,
	const std::string& Head, const TArray<uint8>& Payload, double Deadline,
	const TCHAR*& OutFailStage)
{
	// Handshake. SSL_VERIFY_PEER makes OpenSSL fail the handshake on ANY
	// chain/expiry/hostname validation error, so reaching the write below
	// proves the peer authenticated as the configured FQDN.
	for (;;)
	{
		const int Result = SSL_do_handshake(Ssl);
		if (!FlushOutgoing(WriteBio, Socket, Deadline))
		{
			OutFailStage = TEXT("handshake-send");
			return 0;
		}
		if (Result == 1)
		{
			break;
		}
		const int Error = SSL_get_error(Ssl, Result);
		if (Error == SSL_ERROR_WANT_READ)
		{
			if (!PumpIncoming(ReadBio, Socket, Deadline))
			{
				OutFailStage = TEXT("handshake-recv");
				return 0;
			}
		}
		else if (Error != SSL_ERROR_WANT_WRITE)
		{
			OutFailStage = TEXT("handshake-tls");
			return 0;
		}
		if (RemainingSeconds(Deadline) <= 0.0)
		{
			OutFailStage = TEXT("handshake-deadline");
			return 0;
		}
	}

	if (!SslWriteAll(Ssl, ReadBio, WriteBio, Socket,
			reinterpret_cast<const uint8*>(Head.data()), static_cast<int32>(Head.size()), Deadline))
	{
		OutFailStage = TEXT("request-write-head");
		return 0;
	}
	if (!SslWriteAll(Ssl, ReadBio, WriteBio, Socket,
			Payload.GetData(), Payload.Num(), Deadline))
	{
		OutFailStage = TEXT("request-write-body");
		return 0;
	}

	// Read until the status line is complete. The response head is tiny
	// (status line + a few headers); 1 KiB is ample to capture the status
	// line, which is all the retry classification needs. "Connection: close"
	// was sent with the request, so not draining the rest is fine.
	char Response[1024];
	int32 Total = 0;
	while (Total < static_cast<int32>(sizeof(Response)))
	{
		if (RemainingSeconds(Deadline) <= 0.0)
		{
			OutFailStage = TEXT("response-deadline");
			return 0;
		}
		const int Read = SSL_read(Ssl, Response + Total, static_cast<int>(sizeof(Response)) - Total);
		if (Read > 0)
		{
			Total += Read;
			int StatusCode = 0;
			if (Framedash::TryParseStatusCode(Response, static_cast<std::size_t>(Total), StatusCode))
			{
				return static_cast<int32>(StatusCode);
			}
			continue;
		}
		const int Error = SSL_get_error(Ssl, Read);
		if (Error == SSL_ERROR_WANT_READ)
		{
			if (!PumpIncoming(ReadBio, Socket, Deadline))
			{
				OutFailStage = TEXT("response-recv");
				return 0;
			}
		}
		else if (Error == SSL_ERROR_WANT_WRITE)
		{
			if (!FlushOutgoing(WriteBio, Socket, Deadline))
			{
				OutFailStage = TEXT("response-send");
				return 0;
			}
		}
		else
		{
			OutFailStage = TEXT("response-tls"); // close/error before a parseable status line
			return 0;
		}
	}
	OutFailStage = TEXT("response-overflow");
	return 0;
}

} // anonymous namespace

namespace FramedashDirectSocket
{

TSharedPtr<const Framedash::FAddressPlan, ESPMode::ThreadSafe> AcquirePlan(
	const FString& EndpointUrl,
	const TSharedRef<FFramedashPlanCache, ESPMode::ThreadSafe>& Cache)
{
	// How long one fallback attempt WAITS for DNS before giving up on the
	// fallback for this attempt (~3s, mirroring the Unity/Godot resolve
	// cap). The resolve itself keeps running (GetAddressInfoAsync task) and
	// writes the cache when it completes, so a slow resolver costs at most
	// this wait per attempt and exactly one resolver task total
	// (bResolveInFlight single-flight) -- it can never wedge the flush
	// pipeline for the resolver's full (OS-bounded, potentially long)
	// duration. The batch classification proceeds with status 0 and the
	// retry/offline queue handles delivery until a later attempt finds the
	// completed plan in the cache.
	constexpr double ResolveWaitCapSeconds = 3.0;
	constexpr float ResolvePollSeconds = 0.05f;

	bool bStartResolve = false;
	{
		FScopeLock ScopeLock(&Cache->Lock);
		if (Cache->bFinal)
		{
			return Cache->Plan;
		}
		if (!Cache->bResolveInFlight)
		{
			Cache->bResolveInFlight = true;
			bStartResolve = true;
		}
	}

	const std::string Url = ToUtf8StdString(EndpointUrl);
	if (!Framedash::ShouldForceAddressFamily(Url))
	{
		// Structural passthrough (loopback / IP-literal / non-HTTPS
		// endpoint): deterministic for a fixed endpoint, cache permanently.
		// (Defense in depth -- the transport already gates on eligibility.)
		FScopeLock ScopeLock(&Cache->Lock);
		if (bStartResolve)
		{
			Cache->bResolveInFlight = false;
		}
		Cache->Plan.Reset();
		Cache->bFinal = true;
		return nullptr;
	}

	if (bStartResolve)
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (Sockets == nullptr)
		{
			FScopeLock ScopeLock(&Cache->Lock);
			Cache->bResolveInFlight = false;
			return nullptr; // non-final: a later flush retries
		}

		const std::string HostUtf8 = Framedash::ExtractUrlHost(Url);
		const FString Host(UTF8_TO_TCHAR(HostUtf8.c_str()));
		// The callback runs on the resolver task's thread; it touches only
		// the mutex-guarded shared cache (kept alive by the captured
		// TSharedRef) and pure planner logic, and must never throw.
		TSharedRef<FFramedashPlanCache, ESPMode::ThreadSafe> CacheForCallback = Cache;
		const std::string UrlForCallback = Url;
		Sockets->GetAddressInfoAsync(
			[CacheForCallback, UrlForCallback](FAddressInfoResult Result)
			{
				// One query returns BOTH families (A + AAAA); the planner
				// orders IPv4 first.
				std::string ResolvedIPv4;
				std::string ResolvedIPv6;
				for (const FAddressInfoResultData& Data : Result.Results)
				{
					if (Data.AddressProtocolName == FNetworkProtocolTypes::IPv4 && ResolvedIPv4.empty())
					{
						ResolvedIPv4 = ToUtf8StdString(Data.Address->ToString(false));
					}
					else if (Data.AddressProtocolName == FNetworkProtocolTypes::IPv6 && ResolvedIPv6.empty())
					{
						ResolvedIPv6 = ToUtf8StdString(Data.Address->ToString(false));
					}
				}

#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
				UE_LOG(LogFramedash, Log, TEXT("Fallback DNS resolve: IPv4=%s IPv6=%s (%d raw results)"),
					ResolvedIPv4.empty() ? TEXT("(none)") : UTF8_TO_TCHAR(ResolvedIPv4.c_str()),
					ResolvedIPv6.empty() ? TEXT("(none)") : UTF8_TO_TCHAR(ResolvedIPv6.c_str()),
					Result.Results.Num());
#else
				UE_LOG(LogFramedash, Verbose, TEXT("Fallback DNS resolve: IPv4=%s IPv6=%s (%d raw results)"),
					ResolvedIPv4.empty() ? TEXT("(none)") : UTF8_TO_TCHAR(ResolvedIPv4.c_str()),
					ResolvedIPv6.empty() ? TEXT("(none)") : UTF8_TO_TCHAR(ResolvedIPv6.c_str()),
					Result.Results.Num());
#endif

				Framedash::FAddressPlan Plan =
					Framedash::BuildAddressPlan(UrlForCallback, ResolvedIPv4, ResolvedIPv6);
				TSharedPtr<const Framedash::FAddressPlan, ESPMode::ThreadSafe> NewPlan;
				bool bFinal = false;
				if (!Plan.IsPassthrough())
				{
					NewPlan = MakeShared<Framedash::FAddressPlan, ESPMode::ThreadSafe>(MoveTemp(Plan));
					bFinal = true;
				}
				// else: resolution failed -> non-final null plan so a later
				// flush retries DNS (a transient startup failure must not
				// permanently disable the fallback).

				FScopeLock ScopeLock(&CacheForCallback->Lock);
				CacheForCallback->bResolveInFlight = false;
				CacheForCallback->Plan = NewPlan;
				CacheForCallback->bFinal = bFinal;
			},
			*Host, nullptr, EAddressInfoFlags::Default, NAME_None, ESocketType::SOCKTYPE_Streaming);
	}

	// Bounded wait for the (this-attempt or earlier) resolve to land. On
	// cap, leave it running -- single-flight caps a wedged resolver at one
	// task -- and skip the fallback for this attempt.
	const double WaitDeadline = FPlatformTime::Seconds() + ResolveWaitCapSeconds;
	for (;;)
	{
		{
			FScopeLock ScopeLock(&Cache->Lock);
			if (!Cache->bResolveInFlight)
			{
				return Cache->Plan;
			}
		}
		if (FPlatformTime::Seconds() >= WaitDeadline)
		{
			return nullptr;
		}
		FPlatformProcess::Sleep(ResolvePollSeconds);
	}
}

int32 PostBlocking(
	const Framedash::FAddressPlan& Plan,
	int32 AttemptIndex,
	const std::string& Head,
	const TArray<uint8>& Payload,
	double TimeoutSeconds,
	ISslCertificateManager& CertificateManager)
{
	// Failure-stage note for the diagnostics log below: which step of the
	// attempt failed. Purely observational -- the return contract stays
	// "status 0 on any transport-level failure".
	const TCHAR* FailStage = TEXT("");

	if (AttemptIndex < 0 || AttemptIndex >= static_cast<int32>(Plan.Attempts.size()))
	{
		return 0;
	}
	const Framedash::FAddressAttempt& Attempt = Plan.Attempts[static_cast<std::size_t>(AttemptIndex)];

	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (Sockets == nullptr)
	{
		return 0;
	}

	TSharedPtr<FInternetAddr> Address =
		Sockets->GetAddressFromString(FString(UTF8_TO_TCHAR(Attempt.IpLiteral.c_str())));
	if (!Address.IsValid() || !Address->IsValid())
	{
#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
		UE_LOG(LogFramedash, Log, TEXT("Direct-socket fallback: address parse failed for %s"),
			UTF8_TO_TCHAR(Attempt.IpLiteral.c_str()));
#endif
		return 0;
	}
	Address->SetPort(Plan.Port);

	FSocket* Socket = Sockets->CreateSocket(
		NAME_Stream, TEXT("FramedashDirectSocketFallback"), Address->GetProtocolType());
	if (Socket == nullptr)
	{
#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
		UE_LOG(LogFramedash, Log, TEXT("Direct-socket fallback: socket creation failed (%s)"),
			*Address->GetProtocolType().ToString());
#endif
		return 0;
	}

	int32 StatusCode = 0;
	SSL_CTX* SslContext = nullptr;
	SSL* Ssl = nullptr;

	// Single-exit cleanup: everything below breaks out of this one-pass loop
	// on failure so the socket/SSL teardown always runs (no exceptions in UE).
	do
	{
		// Non-blocking is a hard requirement: on a blocking socket Connect
		// would stall until the OS TCP timeout, far past our deadline, and
		// hold the upstream single-flight flush guard with it. Fail the
		// attempt (status 0) rather than risk an unbounded connect.
		if (!Socket->SetNonBlocking(true))
		{
			FailStage = TEXT("set-non-blocking");
			break;
		}
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;

		// Non-blocking connect: initiate, then wait for writability within
		// the deadline. FSocket::Connect returns true for an in-progress
		// non-blocking connect (EWOULDBLOCK/EINPROGRESS are not errors in
		// FSocketBSD::Connect); false is a hard failure. A refused/
		// unreachable connect surfaces as the explicit error-state check
		// below, a Wait timeout, or an immediate TLS failure right after --
		// all classified as status 0.
		if (!Socket->Connect(*Address))
		{
			FailStage = TEXT("connect-initiate");
			break;
		}
		const double ConnectRemaining = RemainingSeconds(Deadline);
		if (ConnectRemaining <= 0.0 ||
			!Socket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromSeconds(ConnectRemaining)))
		{
			FailStage = TEXT("connect-wait");
			break;
		}
		// Fast-fail a connect that completed with an error (e.g. refused on
		// Windows, where the failure is signaled via the socket error state
		// rather than writability). Only a definite error aborts here; a
		// live connection proceeds and any residual failure surfaces in the
		// TLS handshake within the same deadline.
		if (Socket->GetConnectionState() == SCS_ConnectionError)
		{
			FailStage = TEXT("connect-error-state");
			break;
		}

		// Module-local OpenSSL context populated with the engine certificate
		// manager's trust roots -- the same pattern the engine's own libcurl
		// backend uses (CurlHttp.cpp sslctx_function). Deliberately NOT
		// ISslManager::CreateSslContext: that helper is compiled out
		// (#if IS_MONOLITHIC || UE_MERGED_MODULES) and always returns null in
		// modular (editor) builds, where each module links its own static
		// OpenSSL. Compression is disabled (CRIME-class hygiene; the payload
		// is already gzipped anyway); the TLS 1.2 floor is set per-SSL below.
		SslContext = SSL_CTX_new(TLS_client_method());
		if (SslContext == nullptr)
		{
			FailStage = TEXT("ssl-context");
			break;
		}
		SSL_CTX_set_options(SslContext, SSL_OP_NO_COMPRESSION);
		CertificateManager.AddCertificatesToSslContext(SslContext);

		Ssl = SSL_new(SslContext);
		if (Ssl == nullptr)
		{
			FailStage = TEXT("ssl-new");
			break;
		}

		BIO* ReadBio = BIO_new(BIO_s_mem());
		BIO* WriteBio = BIO_new(BIO_s_mem());
		if (ReadBio == nullptr || WriteBio == nullptr)
		{
			if (ReadBio != nullptr)
			{
				BIO_free(ReadBio);
			}
			if (WriteBio != nullptr)
			{
				BIO_free(WriteBio);
			}
			FailStage = TEXT("bio-new");
			break;
		}
		// An empty memory BIO must report "retry" (WANT_READ), not EOF.
		BIO_set_mem_eof_return(ReadBio, -1);
		BIO_set_mem_eof_return(WriteBio, -1);
		SSL_set_bio(Ssl, ReadBio, WriteBio); // ownership moves to Ssl
		SSL_set_connect_state(Ssl);

		// TLS 1.2 floor (the ingest edge negotiates 1.2/1.3), full peer
		// verification, and the ORIGINAL FQDN pinned as BOTH the SNI server
		// name and the certificate hostname-check reference. SSL_set1_host
		// makes the IP-literal connect validate exactly as a hostname
		// connect would; failure to set any of these aborts the attempt
		// rather than proceeding with weaker validation (fail closed).
		if (SSL_set_min_proto_version(Ssl, TLS1_2_VERSION) != 1)
		{
			FailStage = TEXT("ssl-min-version");
			break;
		}
		SSL_set_verify(Ssl, SSL_VERIFY_PEER, nullptr);
		if (SSL_set_tlsext_host_name(Ssl, Plan.CommonName.c_str()) != 1)
		{
			FailStage = TEXT("ssl-sni");
			break;
		}
		if (SSL_set1_host(Ssl, Plan.CommonName.c_str()) != 1)
		{
			FailStage = TEXT("ssl-set1-host");
			break;
		}

		StatusCode = RunTlsPost(Ssl, ReadBio, WriteBio, *Socket, Head, Payload, Deadline, FailStage);
	}
	while (false);

	if (Ssl != nullptr)
	{
		SSL_free(Ssl); // frees the BIOs it owns
	}
	if (SslContext != nullptr)
	{
		SSL_CTX_free(SslContext);
	}
	Socket->Close();
	Sockets->DestroySocket(Socket);

	if (StatusCode == 0)
	{
		// Diagnostics only (supportability): which stage the attempt died at.
		// Includes the OpenSSL error string when one is queued (TLS stages).
		const unsigned long SslError = ERR_peek_last_error();
		char SslErrorText[256] = {0};
		if (SslError != 0)
		{
			ERR_error_string_n(SslError, SslErrorText, sizeof(SslErrorText));
		}
#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
		UE_LOG(LogFramedash, Log, TEXT("Direct-socket fallback attempt failed at stage '%s' (%s:%d)%s%s"),
			FailStage, UTF8_TO_TCHAR(Attempt.IpLiteral.c_str()), Plan.Port,
			SslError != 0 ? TEXT(" openssl: ") : TEXT(""),
			SslError != 0 ? UTF8_TO_TCHAR(SslErrorText) : TEXT(""));
#else
		UE_LOG(LogFramedash, Verbose, TEXT("Direct-socket fallback attempt failed at stage '%s' (%s:%d)%s%s"),
			FailStage, UTF8_TO_TCHAR(Attempt.IpLiteral.c_str()), Plan.Port,
			SslError != 0 ? TEXT(" openssl: ") : TEXT(""),
			SslError != 0 ? UTF8_TO_TCHAR(SslErrorText) : TEXT(""));
#endif
	}

	// Clear any OpenSSL error-queue residue so a failure here can never
	// confuse another OpenSSL consumer on this (pooled) thread.
	ERR_clear_error();

	return StatusCode;
}

} // namespace FramedashDirectSocket

#endif // FRAMEDASH_WITH_DIRECT_SOCKET_TLS
