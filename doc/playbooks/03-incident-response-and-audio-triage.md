# Playbook 03: Incident Response & Audio Triage

<!-- docs-scope: current-guide -->

Emergency triage protocol for audio clipping, DSP segfaults, hanging headless studio sockets, and corrupted project manifests in HEAV3NS-MUSE.

---

## 1. Severity Classification Matrix

| Severity | Definition | Target Resolution | Immediate Action |
| :--- | :--- | :--- | :--- |
| **SEV-1** | Audio driver freeze, continuous high-gain feedback (> +6 dBFS), or headless host crash during live session | < 15 minutes | Immediate audio kill; terminate process; isolate hardware sink |
| **SEV-2** | Studio socket unresponsive, dropped MIDI notes, or failed render export | < 2 hours | Inspect thread backtrace; restart studio socket; verify memory RSS |
| **SEV-3** | Non-critical DSP artifact, cosmetic parameter bounds warning, or doc drift | < 24 hours | File tracking issue; assign gate in `doc/roadmap/OPEN_ITEMS.md` |

---

## 2. Emergency Audio Kill & Containment

If the engine enters an uncontrolled feedback loop or consumes excessive CPU:

```sh
# 1. Force terminate any running headless studio instance
pkill -9 -f "lmms.*--studio"

# 2. Clean up orphaned socket descriptors
rm -f /tmp/muse.sock /tmp/muse-debug.sock

# 3. Check for lingering child processes
ps aux | grep -i lmms
```

---

## 3. Post-Incident Forensic Recovery

1. **Capture Diagnostic Artifacts**:
   - Save the offending project file (`.mmpz` or `.json` manifest).
   - If a crash occurred, preserve the core dump or LLDB backtrace.
2. **Reproduce Headless in Debug Mode**:
   ```sh
   lldb -- ./build/lmms --studio --socket /tmp/triage.sock
   ```
3. **Log the Event**:
   - Append a dated incident record to [doc/roadmap/ROLLING_LOG.md](../roadmap/ROLLING_LOG.md).
   - If an existing roadmap gate was compromised, reopen it using:
     ```sh
     npm run roadmap:roll -- --gate <GATE_ID> --status in_progress --note "Reopened following SEV-1 incident"
     ```
