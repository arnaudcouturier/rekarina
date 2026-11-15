# The project's Forgejo server: git.rekarina.dev

The source repository's home is a Forgejo instance of the project's own at https://git.rekarina.dev, run with Docker on
one small VPS. GitHub may stay as a push mirror of it. The site and the release files are elsewhere (Cloudflare:
`rekarina.dev`, `dl.rekarina.dev`; [docs/distribution.md](../../docs/distribution.md)) and do not depend on this server.

A public instance (Codeberg) was considered and not used: its terms of use, voted on 2026-07-23, make projects written
and maintained with heavy use of LLM assistants unwelcome, and this one is.

## What runs

| File | What it is |
| --- | --- |
| `docker-compose.yml` | Forgejo 17 (`codeberg.org/forgejo/forgejo:17`, SQLite, registration closed, Actions on) and Caddy 2 in front of it |
| `Caddyfile` | Caddy serves `git.rekarina.dev` over HTTPS and passes it to Forgejo's port 3000; the certificate is Caddy's own (ACME, renewed by itself) |
| `backup.sh` | `forgejo dump` of repositories, database and settings into `backups/`, the last 14 kept; run daily from cron |

Ports on the server: 22 (the server's own SSH), 80 and 443 (Caddy), 2222 (git over SSH into Forgejo's port 22). Clone
URLs show `ssh://git@git.rekarina.dev:2222/...` (`FORGEJO__server__SSH_PORT`).

Requirements: a VPS with Ubuntu 24.04, 2 GB of memory and 20 GB of disk or more, a public IPv4 address, and a DNS record
`git.rekarina.dev` of type A pointing at it. The record is **DNS only** in Cloudflare (not proxied): Cloudflare's proxy
passes neither SSH on 2222 nor Caddy's HTTP certificate challenge as set up here.

## Setting it up

1. The VPS with Ubuntu 24.04 and the maintainer's SSH public key (the provider's panel takes it at installation).
2. The DNS record `git` (type A, the server's IPv4, DNS only) in the `rekarina.dev` zone.
3. On the server, as root:

       apt update && apt install -y docker.io docker-compose-v2
       mkdir -p /root/forgejo/forgejo && chown 1000:1000 /root/forgejo/forgejo

   Copy `docker-compose.yml`, `Caddyfile` and `backup.sh` into `/root/forgejo/`, then:

       cd /root/forgejo && chmod +x backup.sh && docker compose up -d

4. https://git.rekarina.dev shows Forgejo's first-run page: SQLite is preset; set the administrator's name, e-mail and
   password under "Administrator account settings" and install. Registration stays closed
   (`FORGEJO__service__DISABLE_REGISTRATION`): accounts are made by the administrator.
5. An organization `rekarina` with a repository `rekarina` (empty), and the maintainer's SSH key under the account's
   SSH keys.

## Publishing the sources

The public history is one commit per release (the local branch `public`, [AGENTS.md](../../AGENTS.md)); only that
branch goes out:

    git remote add forgejo ssh://git@git.rekarina.dev:2222/rekarina/rekarina.git
    git push forgejo public:main

A push mirror to GitHub (repository settings → Mirror settings → push mirror, with a GitHub token that may write to
the repository) sends each update on.

## Running it

- Backups: `crontab -e`, the line `30 4 * * * /root/forgejo/backup.sh >> /root/forgejo/backup.log 2>&1`. A copy off
  the server (an R2 bucket beside the releases) is still to be added.
- Updates within 17.x: `cd /root/forgejo && docker compose pull && docker compose up -d`. A new major version (18) is
  a manual step after reading its release notes; the image tag pins the major.
- Logs: `docker compose logs -f forgejo` (or `caddy`).
- CI: Forgejo Actions are enabled; the runner (`forgejo-runner`) and a workflow for `tools/check_content.py` are still
  to be added. Until then the check runs in the pre-commit hook (`git config core.hooksPath .githooks`).
