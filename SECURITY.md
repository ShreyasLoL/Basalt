# Security Policy

## Scope

BASALT is a local-first research and exchange-simulation application. No formal release support matrix is published; security review and fixes apply to the current default branch. When reporting an issue, include the affected commit, build platform, configuration and a minimal reproduction that uses simulated data only.

The intended deployment is a trusted single-user machine. The HTTP service binds to loopback only. The MCP interface uses a local standard-input/standard-output process. Remote deployment, live brokerage connectivity and live order placement are not supported security boundaries for this project.

## Reporting a vulnerability

Please use the repository host's private vulnerability-reporting feature when it is enabled. If that feature is unavailable, contact a project maintainer through an established private channel. Do not publish exploit details or sensitive data in a public issue before maintainers have had a reasonable opportunity to investigate.

Please include the affected version or commit, operating system, relevant configuration with secrets removed, impact, reproduction steps and any proposed mitigation. Do not include live credentials or real account data. This project does not publish a response-time guarantee or a dedicated security email address.

## In-scope issues

Reports are in scope when they demonstrate a security or integrity impact in the supported local simulator, including parser crashes from untrusted inputs, bypasses of HTTP or MCP validation, persistence corruption, resource exhaustion, financial-state invariant failures, secret exposure, or a vulnerable dependency that is used by the application.

Issues that require a fully compromised host, administrator-equivalent access, or arbitrary code execution as the same operating-system user are outside the protections BASALT can provide. Localhost does not authenticate clients. Do not test against real brokerage accounts or attempt remote access to a user's machine.
