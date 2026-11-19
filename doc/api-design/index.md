# API design guide

Status: index

The design guide is split into seven focused chapters and reference pages. It covers RZI 0.1.0 on
USP / LoRa Basics Modem and USP RAC; each service declaration remains the
authoritative contract. Counts include conditional and unavailable functions.

| Chapter | Topic |
|---|---|
| [1. Overview](overview.md) | Consumers, capability sources, evidence, ownership. |
| [2. Design principles](principles.md) | Contracts, comments, acceptance/completion, data lifetime, updates. |
| [3. Call paths](call-paths.md) | Initialization, join, traffic, domain dispatch. |
| [4. Backend coverage](backend-coverage.md) | USP ops, support limits, build selection, resource budgets. |
| [5. Errors](errors.md) | Sources, conversion, delivery, error catalog. |
| [Public index](public-index.md) | 168 declarations across 11 service/foundation headers. |
| [Troubleshooting](troubleshooting.md) | Rejection, failure, unknown outcomes, recovery evidence. |

The complete [183-entry RUI3 migration mapping](../compatibility/rui3-mapping.md)
is maintained under compatibility. This guide explains API design and links
to the mapping instead of maintaining another migration table.

See also: [service APIs](../api/index.md), [board design](../architecture/board-design.md),
[comment rules](../development/api-docs.md), [backend coverage reference](../compatibility/rzi-coverage.md).
