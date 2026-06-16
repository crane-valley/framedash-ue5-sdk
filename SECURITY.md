# Security Policy

Framedash is a game-telemetry platform operated by Crane Valley LLC. We take the
security of our services, SDKs, and the player data entrusted to us seriously.
This document explains how to report a vulnerability and what to expect from us.

## Reporting a vulnerability

Please report suspected vulnerabilities by email to **security@framedash.dev**.

- Do not open public GitHub issues, pull requests, or discussions for security
  reports.
- Include a clear description, the affected component and version, reproduction
  steps, and an assessment of the impact.
- If the report contains sensitive details, tell us your preferred secure
  contact method and we will arrange one.

We accept reports in English and Japanese.

## Scope

In scope:

- The Framedash web application and REST API (`app.framedash.dev`)
- The telemetry ingestion endpoint
- The official SDKs (Unity, UE5)
- The published packages (`@framedash/api-client`, `@framedash/cli`,
  `@framedash/mcp-server`) and the protobuf schema

Out of scope:

- Denial-of-service, volumetric, or load testing
- Social engineering and physical attacks
- Findings that require a compromised device, rooted/jailbroken OS, or
  privileged local access
- Automated scanner output without a demonstrated, reproducible impact
- Vulnerabilities in third-party managed services we build on (report those to
  the respective vendor)

## Coordinated disclosure

- Please give us a reasonable opportunity to remediate before any public
  disclosure.
- We will acknowledge your report, keep you informed of remediation progress,
  and credit you (with your consent) once a fix has shipped.

## Response targets

- Acknowledge receipt within 3 business days.
- Provide an initial triage / assessment within 7 business days.
- Remediate confirmed vulnerabilities on a risk-prioritized timeline and notify
  you when the fix is live.

These are good-faith operational targets for the current pre-GA stage, not a
contractual service-level agreement.

## Safe harbor

We will not pursue or support legal action against researchers who:

- Act in good faith and avoid privacy violations, data destruction, and service
  degradation;
- Only interact with accounts and data they own or have explicit permission to
  test;
- Give us a reasonable time to remediate before disclosing.

## Supported versions

Framedash is delivered as a hosted service, so the current production deployment
is always the supported version. For the published SDKs and packages, the latest
released version on each public mirror and on npm is supported.
