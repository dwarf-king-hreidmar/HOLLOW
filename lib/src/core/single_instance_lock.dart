import 'dart:io';

/// Outcome of one non-blocking lock attempt.
enum _Probe { taken, held, unsupported }

/// One running Hollow per data root.
///
/// The kernel lock is the truth: it dies with the process, so a crash, a kill
/// or a self-restart never leaves a stale lock behind. The pid in the file is
/// for people, scripts, and the fallback where the filesystem has no locks.
class SingleInstanceLock {
  SingleInstanceLock._();

  /// Locked byte, far past the pid text: a Windows range lock also blocks
  /// READS of the locked bytes, and the pid is read by the profile switcher.
  static const int _lockByte = 1 << 30;

  /// "Another process holds it": EAGAIN/EACCES on Linux, EAGAIN on macOS,
  /// ERROR_LOCK_VIOLATION on Windows. Any other failure means this filesystem
  /// has no locks (a network share), never that a copy is running.
  static const Set<int> _heldCodes = {11, 13, 35, 33};

  /// Windows ERROR_SHARING_VIOLATION: open in a process that denies sharing.
  static const int _sharingViolation = 32;

  static RandomAccessFile? _handle;
  static String? _ownedPath;

  static String fileIn(String dir) =>
      '$dir${Platform.pathSeparator}hollow.lock';

  /// True when this process now owns [lockDir]; false when another live
  /// Hollow does. Never throws: an unwritable location launches rather than
  /// guards.
  static bool acquire(String lockDir) {
    final path = fileIn(lockDir);
    try {
      Directory(lockDir).createSync(recursive: true);
    } catch (_) {}
    switch (_tryLock(path, keep: true)) {
      case _Probe.held:
        return false;
      case _Probe.unsupported:
        if (_namesLiveHollow(path)) return false;
        try {
          File(path).writeAsStringSync('$pid');
        } catch (_) {}
      case _Probe.taken:
        try {
          _handle!
            ..truncateSync(0)
            ..setPositionSync(0)
            ..writeStringSync('$pid')
            ..flushSync();
        } catch (_) {}
    }
    _ownedPath = path;
    return true;
  }

  /// Drop the lock and remove the file. Safe to call more than once.
  static void release() {
    final file = _handle;
    final path = _ownedPath;
    _handle = null;
    _ownedPath = null;
    if (file != null) {
      try {
        file.unlockSync(_lockByte, _lockByte + 1);
      } catch (_) {}
      try {
        file.closeSync();
      } catch (_) {}
    }
    if (path != null) {
      try {
        File(path).deleteSync();
      } catch (_) {}
    }
  }

  /// True when a Hollow process other than this one holds the lock file at
  /// [lockFilePath]: the kernel lock first, then the pid it names for a copy
  /// old enough to have written only that.
  static bool heldByAnotherProcess(String lockFilePath) {
    // A POSIX lock belongs to the process, not the handle: opening and closing
    // our own lock file again would silently drop it.
    if (_isOwnPath(lockFilePath)) return false;
    if (!File(lockFilePath).existsSync()) return false;
    if (_tryLock(lockFilePath, keep: false) == _Probe.held) return true;
    return _namesLiveHollow(lockFilePath);
  }

  static _Probe _tryLock(String path, {required bool keep}) {
    final RandomAccessFile file;
    try {
      // Append, never truncate: the content belongs to whoever holds the lock
      // until that is known to be us.
      file = File(path).openSync(mode: FileMode.append);
    } on FileSystemException catch (e) {
      final denied =
          Platform.isWindows && e.osError?.errorCode == _sharingViolation;
      return denied ? _Probe.held : _Probe.unsupported;
    }
    try {
      file.lockSync(FileLock.exclusive, _lockByte, _lockByte + 1);
    } on FileSystemException catch (e) {
      file.closeSync();
      return _heldCodes.contains(e.osError?.errorCode)
          ? _Probe.held
          : _Probe.unsupported;
    }
    if (keep) {
      _handle = file;
    } else {
      try {
        file.unlockSync(_lockByte, _lockByte + 1);
      } catch (_) {}
      file.closeSync();
    }
    return _Probe.taken;
  }

  static bool _isOwnPath(String path) {
    final own = _ownedPath;
    if (own == null) return false;
    String norm(String p) {
      final abs = File(p).absolute.path.replaceAll(r'\', '/');
      return Platform.isWindows ? abs.toLowerCase() : abs;
    }

    return norm(path) == norm(own);
  }

  /// A flatpak sandbox has its own pid namespace and every launch gets the
  /// same small pid, so a lock naming OUR pid is never another instance (#69).
  static bool _namesLiveHollow(String lockFilePath) {
    try {
      final lockPid = int.tryParse(File(lockFilePath).readAsStringSync().trim());
      if (lockPid == null || lockPid == pid) return false;
      return _isHollowProcess(lockPid);
    } catch (_) {
      return false;
    }
  }

  /// The name check avoids a false positive from pid reuse after a crash.
  static bool _isHollowProcess(int targetPid) {
    try {
      if (Platform.isWindows) {
        final r =
            Process.runSync('tasklist', ['/FI', 'PID eq $targetPid', '/NH']);
        final out = r.stdout.toString().toLowerCase();
        return out.contains('$targetPid') && out.contains('hollow');
      }
      final r = Process.runSync('ps', ['-p', '$targetPid', '-o', 'comm=']);
      return r.exitCode == 0 &&
          r.stdout.toString().toLowerCase().contains('hollow');
    } catch (_) {
      return false;
    }
  }
}
