# Vendored dependencies

Pinned copies of third-party code the sketch depends on, so the host tests
build offline. The firmware itself uses the library installed through the
Arduino Library Manager (same major version).

| File | Library | Version | Source | SHA-256 |
| --- | --- | --- | --- | --- |
| `ArduinoJson.h` | ArduinoJson (MIT) | 7.2.0 | https://github.com/bblanchon/ArduinoJson/releases/download/v7.2.0/ArduinoJson-v7.2.0.h | `526223dc9e3eb1e7103be3028c1ec2a55718d7895a3b8eda96cd2f7b68224946` |

To update, download the single-header release for the new version, replace
the file, and update this table.
