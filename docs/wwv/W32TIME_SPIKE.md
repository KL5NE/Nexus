# C4(b) spike: getting WWV time into the Windows clock through W32Time

From Sean (KL5NE), with Claude. 6 Nov 2026.

Status: **research only, from Microsoft's documentation.** Nothing was run or changed on HAMBEAST. Sean approved the read-only spike on 6 Nov.

## The question

Nexus's rule is "Nexus never calls a clock API; W32Time is the only clock writer". How can a VERIFIED WWV time reach the Windows clock under that rule?

## Findings

1. **The loopback NTP server idea is fragile, and probably blocked.**
   - Microsoft: "The built-in W32Time NTP client can only use UDP 123 as the source port." The client and server "share UDP port 123".
   - `manualpeerlist` takes host names or IP addresses with flags. No port syntax is documented.
   - So Nexus would have to answer NTP on 127.0.0.1:123 while W32Time's own client also uses UDP 123.
   - That's at best a socket-sharing gamble that depends on Windows' binding rules, and it can't be guaranteed across Windows builds.
   - **Recommendation: drop it as the primary design.**
2. **W32Time has a supported plug-in for exactly this: an input time provider.**
   - Microsoft: "Input time providers retrieve accurate time stamps from hardware or the network."
   - **What it is:** a DLL that exports `TimeProvOpen`, `TimeProvCommand` and `TimeProvClose`.
   - **How it's registered:** under `HKLM\System\CurrentControlSet\Services\W32Time\TimeProviders\<Name>`, with `DllName`, `Enabled` = 1 and `InputProvider` = 1.
   - **How samples flow:**
     1. The provider calls the manager's "samples available" callback.
     2. W32Time asks for samples with `TPC_GetSamples`.
     3. The provider returns its best sample, a `TimeSample` with offset, delay, dispersion, stratum and reference ID. It must return within half a second.
   - **How it fits Nexus's rule:**
     - **W32Time stays the only clock writer.** It decides whether and how to correct, and the provider only offers evidence. That's exactly the boundary Nexus's `clockdiag.rs` draws.
     - **No port conflict:** there's no network involved.
     - **Shows up in Windows' own tools:** `w32tm /query /source` would name it, so the operator can see what owns the clock.
   - **The proposed shape:**
     - **The provider DLL:** a tiny `nexus-wwv-timeprov.dll`, C or Rust `cdylib`, with **no decoder in it**. It reads Nexus's latest VERIFIED offset over a local named pipe, along with its uncertainty and age, and offers it as a sample: stratum 1, refid `WWV`/`WWVH`, dispersion ≥ our uncertainty. It offers **nothing** when Nexus isn't VERIFIED or the sample is stale.
     - **Install and remove:** an operator-confirmed, elevated "Use WWV as this PC's time source" action does both. It registers the provider, saves the previous W32Time settings, and restarts W32Time, using the existing `sc`/`w32tm` repair path. An equally explicit "Stop using WWV" unregisters it and restores the saved settings.
   - **To confirm in Phase C4:**
     - the exact `TimeSample` field semantics, which the spike didn't read;
     - code-signing and Smart App Control behaviour for a DLL loaded by an SCM service;
     - how W32Time weighs it against an NTP peer that is also configured.
3. **W32Time's correction limits matter for the "badly wrong clock" pitch.**
   - **Standalone (workgroup) PCs** default `MaxPos/MaxNegPhaseCorrection` to **54,000 s (15 hours)**. A larger error is **not corrected**; W32Time logs it instead.
   - **Domain members** default to "always correct".
   - **What this means:**
     - A laptop minutes or hours off is fixed through the provider.
     - One that's days or years off, after a dead CMOS battery, is refused by W32Time's own guard.
     - For that case, the honest UI shows the WWV date and time and asks the operator to set the date by hand once (or raise the limit, operator-confirmed). Nexus still never sets it.
   - This matches Nexus's own guard 3 (no steering beyond 60 s), so the skeptic test needs to cover it.
4. **How W32Time applies corrections.**
   - Standalone `MaxAllowedPhaseOffset` is 1 s. Below that W32Time **slews** (adjusts the rate); above it W32Time **steps** the clock.
   - Standalone polling is slow: MinPollInterval 2^10 ≈ 17 min, MaxPollInterval 2^15 ≈ 9 h. The "Use WWV" action should set a shorter poll while it's active, and restore the old value on undo.

## Recommendation

- Re-plan C4(b) around a **W32Time input time provider DLL** fed by Nexus over a local pipe, replacing the loopback NTP server.
- Build it after C3, as its own reviewed piece. It runs inside a Windows system service, so it gets the same no-panic, no-blocking scrutiny the codebase gives the radio loop.
- A one-shot "set the clock" button stays out: it would make Nexus a clock writer.

## Sources

- [Windows Time service tools and settings](https://learn.microsoft.com/en-us/windows-server/networking/windows-time-service/windows-time-service-tools-and-settings) (port 123, `manualpeerlist`, phase-correction defaults, slew/step, poll intervals)
- [Configure W32Time against a large time offset (KB 884776)](https://learn.microsoft.com/en-us/troubleshoot/windows-server/active-directory/configure-w32ime-against-huge-time-offset)
- [Time Provider](https://learn.microsoft.com/en-us/windows/win32/sysinfo/time-provider) · [Creating a Time Provider](https://learn.microsoft.com/en-us/windows/win32/sysinfo/creating-a-time-provider) · [Registering a Time Provider](https://learn.microsoft.com/en-us/windows/win32/sysinfo/registering-a-time-provider)
- [TimeProvCommand](https://learn.microsoft.com/en-us/windows/desktop/api/timeprov/nf-timeprov-timeprovcommand) · [TpcGetSamplesArgs](https://learn.microsoft.com/en-us/windows/desktop/api/timeprov/ns-timeprov-tpcgetsamplesargs)
