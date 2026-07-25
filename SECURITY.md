# Security

## Supported configuration

This is a local embedded project, not an internet-facing service. The supported deployment is a trusted private LAN with firewall isolation from the public internet.

## Credential handling

- `secrets.h` is excluded by `.gitignore`.
- Only `secrets.example.h` belongs in version control.
- Wi-Fi credentials and WeatherAPI keys must not be placed in issues, screenshots, Serial logs, or commits.
- Changing the weather location through the web interface writes NVS only; it does not modify the credentials file.

If a credential is committed, deleting the file in a later commit is insufficient. Revoke/rotate the credential and remove it from Git history.

## Web interface risk

The HTTP server:

- is unencrypted;
- has no authentication;
- has no authorization model;
- uses GET requests for state changes;
- exposes operational data to any client that can reach port 80.

Do not expose the device through router port forwarding, public Wi-Fi, or an untrusted shared network.

## Reporting

Do not publish a security report containing active credentials, precise private location, or network details. Open a GitHub issue containing only sanitized reproduction information.

