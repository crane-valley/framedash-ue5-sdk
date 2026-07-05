// Copyright Crane Valley. All Rights Reserved.
//
// Direct-socket TLS fallback transport for the prefer-IPv4-with-IPv6-fallback
// ingest connect (UE5 parity with Unity #1218 / Godot #1216). Used ONLY when
// the primary IHttpRequest attempt fails at the transport level (status 0):
// connects an FSocket straight to a resolved IP literal (pinning the address
// family, which IHttpRequest / engine libcurl cannot do -- no portable
// CURLOPT_IPRESOLVE), then negotiates TLS over the socket with the engine SSL
// module's OpenSSL: a module-local SSL_CTX is populated with trust roots
// from ISslCertificateManager::AddCertificatesToSslContext (the exact
// pattern of the engine's own libcurl backend -- ISslManager's
// CreateSslContext helper is compiled out in modular builds and cannot be
// used), SSL_VERIFY_PEER enforces
// full chain/expiry validation during the handshake, and SSL_set1_host +
// SNI pin BOTH the certificate hostname check and the server name to the
// ORIGINAL FQDN, so connecting to an IP literal loses no validation
// whatsoever. Do NOT weaken the verify mode or add a verify callback that
// returns success -- the default validation IS the design.
//
// Everything here is blocking I/O intended for a background thread (the
// transport launches it on the same task infrastructure as serialization);
// nothing may run on the game thread. All failures collapse to status 0 --
// the same contract as a failed IHttpRequest -- and nothing throws (fail-safe
// SDK). This file is deliberately THIN: the testable logic (endpoint
// qualification, attempt ordering, family toggle, request head, status-line
// parsing) lives in FramedashAddressPlanner / FramedashRawHttp, which the
// GoogleTest host harness covers; the socket/TLS glue below is UE-coupled and
// engine-only by nature.

#pragma once

#include "CoreMinimal.h"

#if FRAMEDASH_WITH_DIRECT_SOCKET_TLS

#include "FramedashAddressPlanner.h"

#include <string>

class ISslCertificateManager;

/**
 * Thread-safe, shared cache of the resolved delivery plan. Owned via
 * TSharedPtr by the transport AND by each in-flight fallback task, so a task
 * that outlives the transport still has valid storage.
 *
 * Cache semantics (parity with Unity/Godot final/non-final plans):
 * - bFinal + a valid Plan: resolved plan, reused for the transport lifetime
 *   (fixed endpoint, stable anycast DNS).
 * - bFinal + null Plan: STRUCTURAL passthrough (loopback / IP-literal /
 *   non-HTTPS endpoint) -- deterministic, never rebuilt.
 * - !bFinal + null Plan: resolution failed; a later flush retries DNS so a
 *   transient startup failure does not permanently disable the fallback.
 * - bResolveInFlight: another fallback task is currently resolving; callers
 *   use the cached value (possibly null -> skip the fallback this attempt)
 *   instead of stacking a second blocking resolve.
 */
struct FFramedashPlanCache
{
	FCriticalSection Lock;
	TSharedPtr<const Framedash::FAddressPlan, ESPMode::ThreadSafe> Plan;
	bool bFinal = false;
	bool bResolveInFlight = false;
};

namespace FramedashDirectSocket
{
	/**
	 * Return the cached delivery plan, kicking off an async DNS resolve
	 * (GetAddressInfoAsync, single-flight) when the cache is not final and
	 * waiting a BOUNDED ~3s for it -- background thread only. A slow
	 * resolver keeps running detached and writes the cache on completion,
	 * so it can never wedge the flush pipeline (the caller proceeds as
	 * status 0 and a later attempt reads the completed cache). Returns null
	 * when the fallback must not engage (passthrough endpoint, resolution
	 * failure, or the resolve still running past the wait cap). Never
	 * throws.
	 */
	TSharedPtr<const Framedash::FAddressPlan, ESPMode::ThreadSafe> AcquirePlan(
		const FString& EndpointUrl,
		const TSharedRef<FFramedashPlanCache, ESPMode::ThreadSafe>& Cache);

	/**
	 * POST Head + Payload to Plan.Attempts[AttemptIndex] over a direct
	 * socket + TLS. Blocking (background thread only); one TimeoutSeconds
	 * budget covers connect + TLS handshake + request write + status read.
	 * Returns the HTTP status code, or 0 for ANY transport-level failure
	 * (connect/TLS/certificate/write/read error or timeout) -- the same
	 * contract as a failed IHttpRequest, so the caller classifies both paths
	 * identically. Never throws.
	 */
	int32 PostBlocking(
		const Framedash::FAddressPlan& Plan,
		int32 AttemptIndex,
		const std::string& Head,
		const TArray<uint8>& Payload,
		double TimeoutSeconds,
		ISslCertificateManager& CertificateManager);
}

#endif // FRAMEDASH_WITH_DIRECT_SOCKET_TLS
