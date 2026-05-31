# ADB FUSE

## STATUS: NOT FINISHED

---

## REASONS

Complexity of implementing FUSE abstraction over Android Debug Bridge (ADB) shell commands.

---

## SOLVED PROBLEMS

### Instant mounting as storage device
Device mounts automatically to `/run/media/$USER/adb/` and appears immediately in file manager with left sidebar pinning.

### Reduced getattr latency
Metadata caching implemented. getattr does not query the device, reads data from cache instead.

---

## FUNDAMENTAL PROBLEMS

### Cannot read anything except `/`
Any nested directory (e.g. `/sdcard`) is read as a regular file, not as a directory. Navigation inside is impossible.

### 3 second delay
Each directory traversal requires 3 seconds of waiting. This is critically high for user experience.

---

## HOW TO BUILD

```bash
chmod +x build.sh
sudo ./build.sh
```
