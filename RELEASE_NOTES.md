# EduBox HUB VSCP – release notes

## VSCP library 2.3.0 / API 1.7

- Synchronized the library, emulators and protocol documentation with API 1.7
  and library version 2.3.0.
- Added transport-aware server handling, local session invalidation and optional
  sequence correlation for ordinary requests.
- Added the secured EduBox BLE bridge with bounded framing and connection
  lifecycle handling. Cancelled or stale operations cannot revive a lost
  control session or silently re-pair a device.
- Improved transaction, BYE and PING handling and updated regression inputs.
