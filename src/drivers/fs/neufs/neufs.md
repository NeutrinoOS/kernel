# NEUFS

A volume here means the partition the NEUFS filesystem is on.

A "pointer" here is an offset from the beginning of the volume.

**Any `*` = pointer = `u64` offset.**

## Volume Layout

A metadata section is allocated at the creation of the volume.

Its size is configurable. The default is:

```text
clamp(
    volume_size * 2.25%,
    256 MiB,
    16 GiB
)
```

The metadata section contains all of the metadata for the root volume table (RVT), ndirs, files, dcptrs, etc.

The first entry in the metadata section **MUST** be the RVT so the driver can:

1. Read the first 8 bytes to verify the filesystem type using the magic.
2. Read the next 32 bits to get the NEUFS version.

NEUFS makes use of a bitmap for free space tracking on disk, and a secondary bitmap for metadata free space/allocation.

All metadata structures are padded to the nearest 32 bits.

NEUFS is designed for flash memory first and foremost (SSDs) and, while it may (slowly) work, is unsupported on HDDs.

## RVT

The Root Volume Table is the first metadata structure in the volume.

```text
RVT: {
    char[8]   magic    // 0x4E 0x45 0x55 0x46 0x53 0x00 0x77 0x42
                      // (NEUFS, nul, 2 specific bytes)

    int32_t   version  // Filesystem versioning information (default: 1)

    char[16]  name     // Custom partition name

    ndir*     root     // A directory representing the root of the disk
}
```

## Entries

```text
union entry {
    file* f;
    ndir* d;
};
```

The `type` field on ndirs and files exists to prevent ambiguity when reading an `entry`.

```text
enum tribool {
    DEFAULT = 2
    TRUE    = 1
    FALSE   = 0
}
```

## ACLs

ACLs are effectively ignored by the system if the user making the filesystem access is root.

```text
ACLEntry: {
    int64_t machine_id
    // The machine ID for the user or entity this ACL entry applies to

    int64_t local_id
    // The local ID for the user or entity this ACL entry applies to

    // For a file/dir creation operation, if it's permitted, the directory
    // or file should get a default ACLEntry giving the creator full
    // R+W+D+A access.

    // Write operations against existing files, unless they're meant to
    // update ACLs, should NEVER implicitly update an ACL entry!!!!

    tribool write
    // Should the entity this ACL entry applies to be allowed to write
    // into/over this file/directory? (W)

    tribool read
    // Should the entity this ACL entry applies to be allowed to read
    // the contents of this directory/file? (R)

    tribool delete
    // Should the entity this ACL entry applies to be allowed to delete
    // this directory/file altogether? (D)

    tribool acledit
    // Should the entity this ACL entry applies to be allowed to edit
    // ACLs for this directory/file? (A)

    // Any of these values, if DEFAULT, will be inherited from their parent
    // directory, or overridden otherwise.

    // ACLs should definitely be cached by the driver!!
    // Otherwise any arbitrary file access can repeatedly become O(n)!!!!

    // SECURITY(acledit):
    // Nobody with acledit should be able to grant a permission they don't
    // already have, nor should they be able to grant acledit.
}
```

## Directories

```text
ndir: {
    uint8_t type = 0;
    // NDIR type, to prevent entry union ambiguity

    char[256] name;
    // e.g. "mnt", "My Stuff". UTF-8.
    // Yes, this is expensive but it's v1

    int64_t ctime;
    // Creation time (unix epoch, optional via ctime flag)

    int64_t utime;
    // Update/modification time (unix epoch, optional via utime flag)

    ACLEntry*[32] acl;
    // Pointers to up to 32 ACL entries

    ndir* parent;
    // Parent directory

    entry*[64] contents;
    // Pointers to up to 64 entries (file or dir)

    ndir* next;
    // Links to a next ndir block if there's over 64 entries (else nullptr)

    ndir* last;
    // Were we linked to, and if so, by who? (if not, nullptr)
}
```

The driver is to handle this by treating any ndirs linked together with the same name as extensions of each other.

Yes, this is pretty much a doubly linked list.

Yes, this is O(n).

It's v1 of an FS for an OS with no stable ABI; it'll be handled later.

`next` and `last` only contain additional linking entries for this same directory.

If an ndir becomes empty, it's removed from the linked list, marked empty, and `next` and `last` are modified to stitch the list together over that empty ndir.

## Files

```text
file: {
    uint8_t type = 1;
    // FILE type, to prevent entry union ambiguity

    char[256] name;
    // e.g. "file.ext", "funny thing.elf". UTF-8.
    // Yes, this is expensive but it's v1

    int64_t ctime;
    // Creation time (unix epoch)

    int64_t utime;
    // Update/modification time (unix epoch)

    int64_t atime;
    // Access time (unix epoch, optional via atime flag)

    uint64_t size;
    // Updated on write

    ACLEntry*[32] acl;
    // Pointers to up to 32 ACL entries

    ndir* parent;
    // Parent directory

    dcblk*[512] content;
    // Array of up to 512 pointers to dcblks (v1 only supports up to 512,
    // if you need more than that, something awful has happened wrt
    // fragmentation already)

    // DCBLKs must be non-overlapping, sorted by file offset,
    // and ordered in the file
}
```

## Data Control Blocks

```text
dcblk: {
    dcptr*[512] dcptrs;
}
```

A `dcblk` contains up to 512 pointers to `dcptr`s.

## Data Control Pointers

```text
dcptr: {
    uint64_t start;
    // Pointer to where this dcptr's represented content starts on disk

    uint64_t len;
    // How many bytes of content this dcptr spans

    uint64_t crc64;
    // CRC64 of the dcptr's content.
    // This can be VERY heavy.
    //
    // Therefore, it's optional via the crc flag, and OFF BY DEFAULT.
    //
    // IN ANY COMPLIANT DRIVER IMPLEMENTATION!
    // THIS SHOULD NEVER BE ON BY DEFAULT!
}
```

`dcptr`s must represent non-overlapping ranges of file content.

They must be sorted by file offset and ordered in the file.

The `crc64` field is optional and controlled by the `crc` flag.

CRC64 is **OFF BY DEFAULT** and compliant drivers **MUST NOT** enable it by default.
