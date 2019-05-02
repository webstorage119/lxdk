/**
 * @file lxldr/driver.c
 *
 * @copyright 2019 Bill Zissimopoulos
 */

#include <ntifs.h>
#include <lxdk/lxdk.h>

#define LXDK_REGPATH                    "\\Registry\\Machine\\Software\\Lxdk"
#define LXDK_REGPATH_SERVICES           LXDK_REGPATH "\\Services"

#define LOG(Format, ...)                DbgPrint("%s" Format "\n", __FUNCTION__, __VA_ARGS__)

static NTSTATUS RegistryEnumerateKeys(
    HANDLE Root,
    PUNICODE_STRING Path,
    NTSTATUS (*Func)(
        HANDLE Root,
        PUNICODE_STRING Name,
        PVOID Context),
    PVOID Context)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Handle = 0;
    union
    {
        KEY_NAME_INFORMATION V;
        UINT8 B[256];
    } NameInfo;
    ULONG Length;
    UNICODE_STRING Name;
    NTSTATUS Status;

    InitializeObjectAttributes(&ObjectAttributes,
        Path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, Root, 0);
    Status = ZwOpenKey(&Handle, KEY_READ, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
        goto exit;

    for (ULONG I = 0;; I++)
    {
        Status = ZwEnumerateKey(Handle, I, KeyNameInformation, &NameInfo, Length, &Length);
        if (!NT_SUCCESS(Status))
        {
            if (STATUS_NO_MORE_ENTRIES == Status)
                break;
            else if (STATUS_BUFFER_OVERFLOW == Status)
                continue;
            goto exit;
        }

        Name.Length = Name.MaximumLength = (USHORT)NameInfo.V.NameLength;
        Name.Buffer = NameInfo.V.Name;
        Status = Func(Handle, &Name, Context);
        if (!NT_SUCCESS(Status))
            goto exit;
    }

    Status = STATUS_SUCCESS;

exit:
    if (0 != Handle)
        ZwClose(Handle);

    return Status;
}

static NTSTATUS RegistryEnumerateKeyValues(
    HANDLE Root,
    PUNICODE_STRING Path,
    NTSTATUS (*Func)(
        HANDLE Root,
        PUNICODE_STRING Name,
        ULONG Type,
        PVOID Buffer,
        ULONG Length,
        PVOID Context),
    PVOID Context)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Handle = 0;
    union
    {
        KEY_VALUE_FULL_INFORMATION V;
        UINT8 B[1024];
    } ValueInfo;
    ULONG Length;
    UNICODE_STRING Name;
    NTSTATUS Status;

    InitializeObjectAttributes(&ObjectAttributes,
        Path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, Root, 0);
    Status = ZwOpenKey(&Handle, KEY_READ, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
        goto exit;

    for (ULONG I = 0;; I++)
    {
        Status = ZwEnumerateValueKey(Handle, I, KeyValueFullInformation, &ValueInfo, Length, &Length);
        if (!NT_SUCCESS(Status))
        {
            if (STATUS_NO_MORE_ENTRIES == Status)
                break;
            else if (STATUS_BUFFER_OVERFLOW == Status)
                continue;
            goto exit;
        }

        Name.Length = Name.MaximumLength = (USHORT)ValueInfo.V.NameLength;
        Name.Buffer = ValueInfo.V.Name;
        Status = Func(Handle, &Name, ValueInfo.V.Type, (PUINT8)&ValueInfo, ValueInfo.V.DataLength,
            Context);
        if (!NT_SUCCESS(Status))
            goto exit;
    }

    Status = STATUS_SUCCESS;

exit:
    if (0 != Handle)
        ZwClose(Handle);

    return Status;
}

static unsigned wcstoint(const wchar_t *p, const wchar_t **endp, int base)
{
    unsigned v;
    int maxdig, maxalp;

    if (0 == base)
    {
        if ('0' == *p)
        {
            p++;
            if ('x' == *p || 'X' == *p)
            {
                p++;
                base = 16;
            }
            else
                base = 8;
        }
        else
        {
            base = 10;
        }
    }

    maxdig = 10 < base ? '9' : (base - 1) + '0';
    maxalp = 10 < base ? (base - 1 - 10) + 'a' : 0;

    for (v = 0; *p; p++)
    {
        int c = *p;

        if ('0' <= c && c <= maxdig)
            v = base * v + (c - '0');
        else
        {
            c |= 0x20;
            if ('a' <= c && c <= maxalp)
                v = base * v + (c - 'a') + 10;
            else
                break;
        }
    }

    if (0 != endp)
        *endp = (wchar_t *)p;

    return v;
}

static void skipws(const wchar_t *p, const wchar_t **endp)
{
    for (; ' ' == *p; p++)
        ;

    if (0 != endp)
        *endp = (wchar_t *)p;
}

typedef struct
{
    PLX_INSTANCE Instance;
} SERVICE_LOADER_CONTEXT;

static NTSTATUS AddVfsStartupEntries(
    HANDLE Root,
    PUNICODE_STRING Name,
    ULONG Type,
    PVOID Buffer,
    ULONG Length,
    PVOID Context0)
{
    UNREFERENCED_PARAMETER(Root);

    SERVICE_LOADER_CONTEXT *Context = Context0;
    PWSTR P = Buffer;
    LX_VFS_STARTUP_ENTRY Entry;
    INT Error;
    NTSTATUS Status;

    if (REG_SZ != Type || sizeof(WCHAR) > Length || L'\0' != P[Length / sizeof(WCHAR) - 1])
    {
        LOG(": \"%wZ\": error: invalid vfs entry", Name);

        Status = STATUS_INVALID_PARAMETER;
        goto exit;
    }

    RtlZeroMemory(&Entry, 0);
    Entry.Kind = VfsStartEntryNode;
    Entry.Path = *Name;
    Entry.Node.DeviceMajor = wcstoint(P, &P, 0); skipws(P, &P);
    Entry.Node.DeviceMinor = wcstoint(P, &P, 0); skipws(P, &P);
    Entry.Node.Mode = wcstoint(P, &P, 0); skipws(P, &P);
    Entry.Node.Uid = wcstoint(P, &P, 0); skipws(P, &P);
    Entry.Node.Gid = wcstoint(P, &P, 0); skipws(P, &P);
    Error = VfsInitializeStartupEntries(Context->Instance, &Entry, 1);
    if (0 > Error)
    {
        LOG(": \"%wZ\": error: VfsInitializeStartupEntries = %d", Name, Error);

        Status = STATUS_INVALID_PARAMETER;
        goto exit;
    }

    Status = STATUS_SUCCESS;

exit:
    return Status;
}

static NTSTATUS LoadService(
    HANDLE Root,
    PUNICODE_STRING Name,
    PVOID Context0)
{
    SERVICE_LOADER_CONTEXT *Context = Context0;
    WCHAR PathBuf[256 + sizeof L"Devices"];
    UNICODE_STRING Path;
    NTSTATUS Status;

    RtlInitEmptyUnicodeString(&Path, PathBuf, sizeof PathBuf);
    Status = RtlAppendUnicodeStringToString(&Path, Name);
    Status = NT_SUCCESS(Status) ? RtlAppendUnicodeToString(&Path, L"\\Devices") : Status;
    if (!NT_SUCCESS(Status))
    {
        LOG(": \"%wZ\": error: name too long", Name);

        Status = STATUS_INVALID_PARAMETER;
        goto exit;
    }

    Status = ZwLoadDriver(Name);
    if (!NT_SUCCESS(Status) && STATUS_IMAGE_ALREADY_LOADED != Status)
    {
        LOG(": \"%wZ\": error: ZwLoadDriver = %lx", Name, Status);

        goto exit;
    }

    Status = RegistryEnumerateKeyValues(Root, &Path, AddVfsStartupEntries, Context);
    if (!NT_SUCCESS(Status))
    {
        LOG(": \"%wZ\": error: RegistryEnumerateKeyValues = %lx", Name, Status);

        goto exit;
    }

exit:
    /*
     * A driver that is misconfigured should not prevent other drivers from loading.
     * Therefore always return SUCCESS.
     */
    return STATUS_SUCCESS;
}

static INT CreateInitialNamespace(
    PLX_INSTANCE Instance)
{
    SERVICE_LOADER_CONTEXT Context;
    UNICODE_STRING RegistryPath;
    NTSTATUS Status;

    RtlZeroMemory(&Context, sizeof Context);
    Context.Instance = Instance;

    RtlInitUnicodeString(&RegistryPath, L"" LXDK_REGPATH_SERVICES);
    Status = RegistryEnumerateKeys(0, &RegistryPath, LoadService, &Context);
    if (!NT_SUCCESS(Status))
        /* ignore */;

    return 0;
}

NTSTATUS DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    static LX_SUBSYSTEM Subsystem =
    {
        CreateInitialNamespace,
    };

    return LxInitialize(DriverObject, &Subsystem);
}
