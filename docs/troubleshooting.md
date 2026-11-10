# Troubleshooting

| Symptom | Check and response |
|---|---|
| USB port disappears between commands | USB Serial/JTAG re-enumerates during reset. The CLI waits for it; reconnect or enter BOOT/RESET ROM mode if it does not return. |
| Board missing entirely | Use a data cable, another USB port and `./entrance doctor`. Close serial monitors before flashing. |
| No hotspot | Read USB logs. An absent/corrupt identity needs `entrance provision`; startup failures during a trial should roll back. |
| Browser certificate warning | Verify/enroll the device certificate using the quick-start instructions. Do not disable certificate checking globally. |
| CLI certificate pin mismatch | Stop, verify the board and restored profile over USB, then enroll the intended certificate. |
| Login rate limited | Wait one minute; check the administrator password in the private profile. Hotspot and administrator passwords differ. |
| Upload disabled just after boot | Allow 30 continuous healthy seconds. Check storage/maintenance state and current update phase. |
| Upload rejected | Use this project's complete RSA-signed application image, with a strictly newer version and the provisioned project key. Check size, board/layout/schema and diagnostics. |
| Update uploaded but deployment failed | Transfer success is not confirmation. Keep the initiating client connected; inspect active version and rollback outcome. |
| Transfer interrupted | Wait for timeout or cancel it, then restart from byte zero. The current application remains selected until full verification succeeds. |
| Remote check unavailable | Direct GitHub download needs a 2.4 GHz uplink, DNS, Internet and valid time. Captive portals are not supported by the board. Local uploads still work. |
| Station changes disconnect the browser briefly | AP and station share one radio/channel. Rejoin the hotspot; changes are blocked during OTA. |
| Rollback unavailable | The previous slot must contain a valid application. First installation and overwritten/invalid previous slots require USB recovery. |
| Settings storage unhealthy | Export diagnostics and back up flash. Do not erase NVS as a generic fix; recover the compatible snapshots or explicitly reprovision over USB. |
| Heap falls during TLS handshakes | Compare settled samples with equivalent connection counts. Lowest-ever heap alone is not a leak. Review the operating-soak record. |

Use `./entrance diagnostics` for a private JSON report and `./entrance monitor --port PORT` for serial output. Report version, slot, exact operation and observed error, while excluding credentials, private keys, tickets and raw crash dumps.
