# Security

## Supported configuration

This is a local embedded project, not an internet-facing service. The supported deployment is a trusted private LAN with firewall isolation from the public internet.

## Credential handling

- `secrets.h` is excluded by `.gitignore`.
- Only `secrets.example.h` belongs in version control.
- Do not put credentials in issues, screenshots, Serial logs, or commits.
- The setup page stores Wi-Fi credentials and coordinates in NVS.
- The location control stores coordinates in NVS.
- Neither web control changes `secrets.h`.

If a credential enters a commit, revoke the credential and remove it from Git history.

## Web interface risk

The HTTP server:

- uses unencrypted HTTP
- has no authentication
- has no authorization model
- uses GET requests for some state changes
- exposes operational data to any client that can reach port 80.

Do not expose the device through router port forwarding, public Wi-Fi, or an untrusted shared network.

## Setup access point risk

The device uses the fixed password `sen66-setup` for the temporary setup access point.

The access point closes after ten minutes.

Run setup in a controlled location. Do not enter credentials while untrusted people are nearby.

## Reporting

Do not publish a security report containing active credentials, precise private location, or network details. Open a GitHub issue containing only sanitized reproduction information.
