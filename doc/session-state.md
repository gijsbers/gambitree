# Session-state persistence

The existing version-2 key/value format, version-1 reader and XDG/HOME path
selection remain in `session_state.cpp`. `SessionStateSaver` adds scheduling
and field-level ownership; it does not add a second configuration format.

`observe()` compares actual persistent values with the last local observation.
A change moves the monotonic deadline to 1000 ms later. Identical values do
nothing, and reverting to the saved values cancels the write. The UI collects
window dimensions, analysis, board/piece selections, panel ratio, figurines
and both text sizes. Navigation, scroll, search, engine output and board
orientation are not persistent settings and do not affect the deadline.

ConfigureNotify batches retain size and position hints without file I/O or
extra X11 queries. Real and synthetic position coordinates are tracked
separately because they use different coordinate spaces. Initial WM placement
establishes a geometry baseline rather than an edit; it must not erase pending
explicit preferences. Outside initialization/final shutdown, only a due save
queries the settled outer-frame position.
Queued ConfigureNotify events are processed before file I/O begins.

The normal event loop services the saver every iteration, including indexing
and engine/idle work. Its existing select timeout is capped by the save deadline
as well as the piece-resize deadline. There is no new thread or busy-wait;
normal resize processing has no added sleeps. Normal exit and SIGTERM collect
final geometry and force a pending save.
The scoped SIGTERM handler only sets a volatile sig_atomic_t flag; it does no
file/X11 I/O. The existing select wait is at most 100 ms and is interruptible,
so even a signal just before select is noticed. Other termination signals keep
their existing behavior; SIGKILL or a crash cannot flush pending state.

On each save, a non-blocking fcntl lock on `state.lock` covers reading the latest
file, merging local changes and the existing atomic writer. Only fields changed
since this process's last successful local snapshot are merged; unrelated
newer preferences are retained. Positions are merged as a pair with validity
and coordinate-version flags. Version-1 client positions are converted using
the current reliable geometry or omitted, never relabelled as outer positions.
The UI does not live-sync another process's preferences. If both processes edit
the same field, the last successful save wins. A process with no local changes
does not write at shutdown.

The lock file is private and close-on-exec, and is deliberately not unlinked:
replacing its inode would defeat exclusion between concurrent writers. Lock
contention in the interactive loop defers retry by 1000 ms rather than blocking. Write/read errors
retain dirty state and defer retry by 5000 ms; new configuration activity also
retains the quiet-period requirement. The UI suppresses repeated warnings until
recovery. Final shutdown attempts an immediate write despite backoff and allows
a busy lock up to 25 waits of 10 ms for another exiting writer to finish. It
does not rapidly retry file-write errors; failure still exits with a warning.
Unreadable files are not blindly replaced; invalid fields retain the existing
reader's defaulting behavior. The existing writer
uses mkstemp in the state directory, mode 0600, checked writes/flush/close and
rename. Readers therefore see the old or new file, not a partial configuration.

When adding a persistent field, update the serializer/parser, `same_state`,
`merge_local_changes`, the UI snapshot and tests. Keep filesystem operations out
of `observe()` and the ConfigureNotify handler.

Tests use injected monotonic timestamps for exact debounce/backoff boundaries,
idle timeouts and 100000 resize events without waiting a second. Fork barriers
exercise lock contention, independent field updates and clean stale exits.
The X11 suite also starts real isolated viewers to check idle persistence,
SIGTERM flushing and stale-process shutdown. These GUI checks require an X
server; do not work around sandbox socket restrictions to run them.
