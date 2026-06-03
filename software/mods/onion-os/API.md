# Onion DAO API

This document describes the API surface external apps can build on for Onion DAO attendee profiles and Onion request approvals.

Base URL:

```text
https://oniondao.dev
```

Local development:

```text
http://localhost:5173
```

All request and response bodies are JSON.

## Concepts

Onions exist in two modes:

- `points`: an attendee has not linked a badge. Their balance is stored in Postgres through the `point_transactions` ledger.
- `tokens`: an attendee has linked a badge. Their badge owns an SPL token wallet, and future Onion activity is minted or burned on Solana.

External apps should request actions by username or handle, never by Solana address. The server decides whether to use points or tokens.

## Authentication

Public profile lookup is unauthenticated.

External Onion request endpoints use bearer auth when `ONION_EXTERNAL_API_KEY` is configured:

```http
Authorization: Bearer <ONION_EXTERNAL_API_KEY>
```

If the env var is empty, the endpoint is open. Production should configure it.

## Public Profile

Fetch public profile and Onion balance information for an attendee.

```http
GET /api/public/profile/{username}
```

`username` may be a handle, name, or email. Handles may include or omit `@`.

Example:

```bash
curl https://oniondao.dev/api/public/profile/alice
```

Response:

```json
{
  "name": "Alice Example",
  "handle": "alice",
  "avatarUrl": "https://...",
  "onionId": 100001,
  "solanaWalletAddress": "8x...",
  "balanceType": "tokens",
  "currentOnionPoints": null,
  "currentOnionTokens": 420
}
```

For an attendee without a linked badge:

```json
{
  "name": "Bob Example",
  "handle": "bob",
  "avatarUrl": null,
  "onionId": null,
  "solanaWalletAddress": null,
  "balanceType": "points",
  "currentOnionPoints": 250,
  "currentOnionTokens": null
}
```

Errors:

```json
{ "error": "Not found" }
```

## Create Onion Request

Create a request for an attendee to approve a burn or transfer.

```http
POST /api/public/onions/requests
```

Body for burn:

```json
{
  "type": "burn",
  "username": "bob",
  "amount": 100,
  "callbackUrl": "https://example.app/api/onion-callback",
  "callbackSecret": "optional-shared-secret",
  "requester": "example-app",
  "externalId": "order_123",
  "note": "Redeem for workshop materials",
  "metadata": {
    "orderId": "order_123"
  }
}
```

Body for transfer:

```json
{
  "type": "transfer",
  "username": "bob",
  "recipientUsername": "alice",
  "amount": 100,
  "callbackUrl": "https://example.app/api/onion-callback",
  "requester": "example-app",
  "externalId": "transfer_123",
  "note": "Team prize split"
}
```

Fields:

- `type`: `burn` or `transfer`.
- `username`: sender attendee handle, name, or email.
- `recipientUsername`: required for `transfer`; recipient handle, name, or email.
- `amount`: positive whole number of Onions.
- `callbackUrl`: webhook URL called after approval, denial, or failure. Must be `https:` except localhost dev callbacks.
- `callbackSecret`: optional HMAC signing secret for callbacks.
- `requester`: optional external app identifier.
- `externalId`: optional idempotency key scoped by `requester`.
- `note`: optional text shown in the attendee approval queue.
- `metadata`: optional object stored with the request.

Success:

```json
{
  "id": "2fc2c4b6-7caa-4bf7-93e9-7a5d74ee8af1",
  "status": "pending"
}
```

Errors:

```json
{ "error": "invalid_type" }
{ "error": "invalid_amount" }
{ "error": "invalid_callback_url" }
{ "error": "sender_not_found" }
{ "error": "recipient_not_found" }
{ "error": "self_transfer" }
{ "error": "create_failed" }
```

## Lua Script Registry

Onion OS supports Lua scripts. The registry is designed so a badge can browse small pages of scripts and download a selected `.lua` file over WiFi.

### Publish Lua Script

Authenticated portal users can publish Lua scripts.

```http
POST /api/portal/lua-scripts
```

Requires the normal attendee session cookie.

Request:

```json
{
  "title": "Badge Clock",
  "description": "Draws a simple clock on the badge display.",
  "fileName": "badge-clock.lua",
  "code": "print('hello onion')\n",
  "tags": ["clock", "display"],
  "isPublic": true
}
```

Rules:

- `title`: required, 1-80 characters.
- `description`: optional, trimmed to 500 characters.
- `fileName`: optional. Must end in `.lua` and use only letters, numbers, dots, dashes, or underscores. Defaults from title.
- `code`: required, max 64KB.
- `tags`: optional array or comma-separated string. Up to 8 normalized tags.
- `isPublic`: defaults to `true`. Only public scripts appear in badge-facing endpoints.

Success:

```json
{
  "success": true,
  "script": {
    "id": "4e26232a-0297-44d8-9af3-041dd8aa6460",
    "title": "Badge Clock",
    "slug": "badge-clock",
    "description": "Draws a simple clock on the badge display.",
    "fileName": "badge-clock.lua",
    "code": "print('hello onion')\n",
    "tags": ["clock", "display"],
    "downloadsCount": 0,
    "authorName": "Alice Example",
    "authorHandle": "alice",
    "authorUsername": "alice",
    "detailUrl": "/api/public/lua-scripts/4e26232a-0297-44d8-9af3-041dd8aa6460",
    "downloadUrl": "/api/public/lua-scripts/4e26232a-0297-44d8-9af3-041dd8aa6460/download"
  }
}
```

Errors:

```json
{ "error": "Unauthorized" }
{ "error": "invalid_title" }
{ "error": "invalid_file_name" }
{ "error": "invalid_code" }
{ "error": "duplicate_slug" }
```

### List Lua Scripts

Fetch a paginated script registry.

```http
GET /api/public/lua-scripts
```

Query params:

- `page`: page number, default `1`.
- `limit`: page size, default `20`, max `100`.
- `sort`: `recent` or `downloads`, default `recent`.
- `username`: partial author name or handle filter.
- `q`: partial title or description search.
- `tag`: exact normalized tag.

Example:

```bash
curl "https://oniondao.dev/api/public/lua-scripts?sort=downloads&username=ali&limit=10"
```

Response:

```json
{
  "page": 1,
  "limit": 10,
  "total": 2,
  "hasMore": false,
  "scripts": [
    {
      "id": "4e26232a-0297-44d8-9af3-041dd8aa6460",
      "title": "Badge Clock",
      "slug": "badge-clock",
      "description": "Draws a simple clock on the badge display.",
      "fileName": "badge-clock.lua",
      "tags": ["clock", "display"],
      "downloadsCount": 42,
      "authorName": "Alice Example",
      "authorHandle": "alice",
      "authorUsername": "alice",
      "detailUrl": "/api/public/lua-scripts/4e26232a-0297-44d8-9af3-041dd8aa6460",
      "downloadUrl": "/api/public/lua-scripts/4e26232a-0297-44d8-9af3-041dd8aa6460/download"
    }
  ]
}
```

The list response intentionally omits `code` so badges can browse cheaply.

### Get Lua Script Detail

Fetch metadata and source code without incrementing downloads.

```http
GET /api/public/lua-scripts/{id}
```

Response:

```json
{
  "script": {
    "id": "4e26232a-0297-44d8-9af3-041dd8aa6460",
    "title": "Badge Clock",
    "fileName": "badge-clock.lua",
    "code": "print('hello onion')\n",
    "downloadUrl": "/api/public/lua-scripts/4e26232a-0297-44d8-9af3-041dd8aa6460/download"
  }
}
```

### Download Lua Script

Download the raw `.lua` file and increment `downloadsCount`.

```http
GET /api/public/lua-scripts/{id}/download
```

Response headers:

```http
Content-Type: text/x-lua; charset=utf-8
Content-Disposition: attachment; filename="badge-clock.lua"
X-Onion-Script-Id: 4e26232a-0297-44d8-9af3-041dd8aa6460
X-Onion-Script-Title: Badge Clock
X-Onion-Script-Downloads: 43
```

Body:

```lua
print('hello onion')
```

Badge-friendly flow:

```text
1. GET /api/public/lua-scripts?sort=downloads&limit=10
2. Display title, authorUsername, downloadsCount
3. GET selected downloadUrl
4. Save response body using fileName
```

### Push Lua Script To Badge

Authenticated portal users can push a public registry script to their linked badge.

```http
POST /api/portal/lua-scripts/{id}/push
```

Requires the normal attendee session cookie.

The server requires the attendee to have a linked badge that has checked in within the last two minutes. It sends the script over MQTT to the badge, where Onion OS should show an accept/deny popup.

Success:

```json
{
  "success": true,
  "requestId": "a006f56b-673e-4b7a-8408-bfa33ad2740e",
  "delivered": true,
  "message": "Sent to badge. Accept the popup on your device."
}
```

Errors:

```json
{ "error": "Unauthorized" }
{ "error": "script_not_found" }
{ "error": "badge_not_linked" }
{ "error": "badge_offline" }
{ "error": "publish_failed" }
```

### Search Usernames

Fetch all public attendee usernames, optionally filtered by partial username.

```http
GET /api/public/usernames
```

Query params:

- `search` or `q`: partial name or handle.
- `page`: page number, default `1`.
- `limit`: page size, default `20`, max `100`.

Example:

```bash
curl "https://oniondao.dev/api/public/usernames?search=ali"
```

Response:

```json
{
  "page": 1,
  "limit": 20,
  "total": 1,
  "hasMore": false,
  "users": [
    {
      "username": "alice",
      "name": "Alice Example",
      "handle": "alice",
      "scriptCount": 3
    }
  ]
}
```

## Get Request Status

Fetch the current status of a previously created request.

```http
GET /api/public/onions/requests/{id}
```

Example:

```bash
curl \
  -H "Authorization: Bearer $ONION_EXTERNAL_API_KEY" \
  https://oniondao.dev/api/public/onions/requests/2fc2c4b6-7caa-4bf7-93e9-7a5d74ee8af1
```

Response:

```json
{
  "id": "2fc2c4b6-7caa-4bf7-93e9-7a5d74ee8af1",
  "requestType": "burn",
  "status": "completed",
  "amount": 100,
  "currencyMode": "points",
  "solanaSignature": null,
  "error": null,
  "createdAt": "2026-06-03T07:00:00.000Z",
  "updatedAt": "2026-06-03T07:03:00.000Z",
  "reviewedAt": "2026-06-03T07:03:00.000Z"
}
```

Statuses:

- `pending`: waiting for attendee approval in `/portal/onions`.
- `awaiting_badge_signature`: attendee approved, but the badge still needs to sign the Solana transaction.
- `completed`: request settled and callback sent.
- `denied`: attendee or badge denied the request.
- `failed`: server-side settlement failed.
- `processing`: reserved for future async workers.

## Approval Flow

External apps do not approve requests directly. The attendee opens `/portal/onions` and approves or denies.

If the sender has points:

- Burn deducts points from the database ledger.
- Transfer deducts sender points.
- If the recipient has points, the server credits recipient points.
- If the recipient has a linked badge, the server mints tokens to that badge wallet.

If the sender has tokens:

- The server creates a serialized Solana transaction.
- The server sends it to the badge.
- The badge signs or denies.
- On signature, the server broadcasts via Solana RPC as fee payer.
- For token-to-points transfer, the badge signs a burn transaction, then the server credits recipient points after confirmation.

## Callback Webhook

The server calls `callbackUrl` with `POST` after a request is denied, completed, or failed.

Headers:

```http
Content-Type: application/json
X-Onion-Request-Id: 2fc2c4b6-7caa-4bf7-93e9-7a5d74ee8af1
X-Onion-Signature: <hex hmac sha256>
```

`X-Onion-Signature` is included only when `callbackSecret` was provided. It is:

```text
hex(hmac_sha256(callbackSecret, raw_request_body))
```

Body:

```json
{
  "id": "2fc2c4b6-7caa-4bf7-93e9-7a5d74ee8af1",
  "type": "burn",
  "status": "completed",
  "success": true,
  "error": null,
  "amount": 100,
  "currencyMode": "tokens",
  "solanaSignature": "5x..."
}
```

Example verification in Node:

```js
import crypto from "node:crypto";

function verifyOnionSignature(rawBody, signature, secret) {
  const expected = crypto
    .createHmac("sha256", secret)
    .update(rawBody)
    .digest("hex");

  return crypto.timingSafeEqual(
    Buffer.from(signature, "hex"),
    Buffer.from(expected, "hex"),
  );
}
```

Current callback delivery is best effort. External apps should also poll `GET /api/public/onions/requests/{id}` if they need guaranteed reconciliation.

## Idempotency

Set both `requester` and `externalId` when creating a request. The pair is unique.

Example:

```json
{
  "requester": "arcade",
  "externalId": "game-round-2026-06-03-42"
}
```

If the same pair is submitted again, the server returns the existing request row instead of creating a duplicate.

## Error Handling

Treat `400` as an invalid request payload or unresolved user.

Treat `401` as missing or invalid bearer auth.

Treat `404` as unknown profile or request id.

External apps should display request state as asynchronous. A `201` from create means "queued for attendee approval", not "Onions were spent".

## Internal Badge Bridge

These endpoints are for Onion OS / trusted infrastructure, not external apps.

They are protected by `BADGE_API_KEY` when configured:

```http
Authorization: Bearer <BADGE_API_KEY>
```

### Badge Handshake

```http
POST /api/badge/handshake
```

Request:

```json
{
  "hardwareId": "large-random-hardware-id",
  "firmware": "onion-os-1.0.0"
}
```

Response:

```json
{
  "onionId": 100001,
  "status": "seen"
}
```

### Badge Link Response

```http
POST /api/badge/link-response
```

Request:

```json
{
  "onionId": 100001,
  "approved": true,
  "solanaPublicKey": "8x..."
}
```

If approved, the server records the badge wallet and migrates existing point balance to tokens.

### Badge Transaction Response

```http
POST /api/badge/transaction-response
```

Request:

```json
{
  "onionId": 100001,
  "operationId": "95ebc617-4b4e-4d84-9aca-04dc3aa0ee5e",
  "approved": true,
  "signedTransaction": "base64-serialized-signed-transaction"
}
```

If approved, the server broadcasts the signed Solana transaction and completes the external request after confirmation.

### Badge Lua Push Response

```http
POST /api/badge/lua-response
```

Request:

```json
{
  "onionId": 100001,
  "requestId": "a006f56b-673e-4b7a-8408-bfa33ad2740e",
  "approved": true
}
```

If denied:

```json
{
  "onionId": 100001,
  "requestId": "a006f56b-673e-4b7a-8408-bfa33ad2740e",
  "approved": false,
  "error": "User denied"
}
```

## MQTT Badge Bridge

When `ONION_MQTT_URL` is set, the server subscribes to these default topics:

```text
oniondao/badge/handshake
oniondao/badge/{onionId}/link/response
oniondao/badge/{onionId}/transaction/response
oniondao/badge/{onionId}/lua/response
```

It publishes:

```text
oniondao/badge/{onionId}/handshake/accepted
oniondao/badge/{onionId}/link/request
oniondao/badge/{onionId}/transaction/request
oniondao/badge/{onionId}/lua/request
```

The `oniondao` prefix can be changed with `ONION_MQTT_TOPIC_PREFIX`.

Lua request payload:

```json
{
  "requestId": "a006f56b-673e-4b7a-8408-bfa33ad2740e",
  "scriptId": "4e26232a-0297-44d8-9af3-041dd8aa6460",
  "title": "Badge Clock",
  "fileName": "badge-clock.lua",
  "description": "Draws a simple clock on the badge display.",
  "authorUsername": "alice",
  "tags": ["clock", "display"],
  "sizeBytes": 21,
  "code": "print('hello onion')\n"
}
```
