#pragma once
struct IOLock; IOLock *IOLockAlloc(); void IOLockFree(IOLock*); void IOLockLock(IOLock*); void IOLockUnlock(IOLock*);
