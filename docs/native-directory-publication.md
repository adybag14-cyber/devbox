# Native Windows installation publication

PR151's Windows qualification reached native initialization, then failed when
the final verified staging directory was renamed into place. Windows returned
`Access is denied`. This was outside the metadata-extraction change. The CI log
does not identify which process held the directory.

The C++23 initializer now publishes through a bounded directory operation.
Windows uses the non-replacing `MoveFileExW` operation and retries access,
sharing and lock errors for at most five seconds. A successful first attempt
has no retry delay. Each attempt rejects an existing destination; source and
destination must be sibling absolute paths, and the source must be a directory
without a symbolic link. It never falls back to copying over an installation or
scheduling a rename at reboot. Other platforms keep their rename path after
the same destination checks.

The Windows regression holds a directory data handle without delete sharing.
It proves that the original one-shot rename fails, then releases the handle
while the native publication operation is waiting and verifies complete
publication. A persistent lock reaches its bounded failure with staging
preserved. Empty and populated destination directories are both rejected with
their contents unchanged. Linux publication checks also pass.

This preserves the verified staging/configuration boundary of `manage init`
and `manage prepare`. It adds no administrator requirement or runtime
dependency. [Microsoft's MoveFileExW contract](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)
describes the same-volume directory move and move flags used here.
