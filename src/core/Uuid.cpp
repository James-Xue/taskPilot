// Uuid.cpp — version-4 uuid generation and the shape check
//
// Entropy comes from /dev/urandom and from nowhere else. <random>'s
// std::random_device is permitted to be deterministic, and a deterministic
// uuid generator is the single failure this file exists to prevent: two
// machines would mint the same "random" uuids, and the collision would not
// announce itself at creation — it would surface much later, during a sync, as
// one task's record silently overwriting another's. So a read that fails is
// reported as an Error and never replaced with a weaker source, which is
// exactly what Uuid.hpp means by "a uuid that silently repeats is worse than a
// loud failure".
//
// The check is a pure function of the text it is handed: no database, no
// clock, no I/O. TaskSync's parser runs it against every line of a file that
// may have been hand-edited, truncated by a bad checkout, or written by an
// older version, and answering "is this even the right SHAPE of identifier"
// must not depend on anything the caller happens to have loaded.

#include "core/Uuid.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace taskpilot
{
namespace
{

/// A uuid is 128 bits, so the generator reads exactly this many bytes. Reading
/// fewer would leave a fixed tail in every uuid this process ever produces.
constexpr std::size_t kUuidBytes{ 16 };

/// 32 hex digits plus 4 hyphens. Also the length every valid text form has,
/// which makes it the check's first and cheapest rejection.
constexpr std::size_t kUuidTextLength{ 36 };

/// Where RFC 4122 carves the version and the variant out of the random bytes.
constexpr std::size_t kVersionByteIndex{ 6 };
constexpr std::size_t kVariantByteIndex{ 8 };

/// The same two fields in the rendered text. The groups are 8-4-4-4-12, so the
/// version nibble is the first character after the second hyphen and the
/// variant nibble the first after the third.
constexpr std::size_t kVersionCharacterIndex{ 14 };
constexpr std::size_t kVariantCharacterIndex{ 19 };

/// The only four offsets at which the canonical form has a hyphen. Listed
/// rather than derived from the group widths so the check and the generator
/// cannot drift apart while each stays internally consistent.
constexpr std::size_t kHyphenPositions[]{ std::size_t{ 8 }, std::size_t{ 13 },
                                          std::size_t{ 18 }, std::size_t{ 23 } };

/// The kernel's entropy pool. A character device rather than a library call
/// because it is the interface documented to block until the pool has been
/// seeded and to never hand out a value it has already given.
constexpr const char *kEntropyDevice = "/dev/urandom";

/// Lowercase hex alphabet, so the case the generator emits is stated in one
/// place. Uuid.hpp promises lowercase, and the promise carries weight: the
/// export is a set of lines for git to merge, and two machines spelling one
/// uid differently would break the "exactly one line per uid" property the
/// whole format rests on.
constexpr char kHexDigits[] = "0123456789abcdef";

/// One lowercase hex character for the low nibble of `value`.
char hexDigit(unsigned int value)
{
    return kHexDigits[value & 0x0FU];
}

/// True for a hex digit in either case.
///
/// Uppercase is accepted, and the reason is the variant nibble: the four
/// characters that satisfy RFC 4122's "variant is 10xx" marker are 8, 9, a and
/// b, and A/B are the very same nibbles. A reader that took one spelling and
/// refused the other would have to special-case a single position, which is a
/// far stranger rule than "hex is case-insensitive". Nothing taskPilot writes
/// is uppercase (see kHexDigits), so the tolerance only ever applies to a line
/// that arrived from outside.
[[nodiscard]] bool isHexDigit(char character)
{
    if ('0' <= character && '9' >= character)
    {
        return true;
    }
    if ('a' <= character && 'f' >= character)
    {
        return true;
    }
    return 'A' <= character && 'F' >= character;
}

/// True at the four text offsets where the canonical form carries a hyphen.
[[nodiscard]] bool isHyphenPosition(std::size_t index)
{
    for (const std::size_t position : kHyphenPositions)
    {
        if (position == index)
        {
            return true;
        }
    }
    return false;
}

/// The variant nibble: 8, 9, a or b, in either case. The other twelve values
/// describe uuids from schemes this project does not speak, so a line carrying
/// one is rejected rather than merged under a shape we would misread.
[[nodiscard]] bool isVariantNibble(char character)
{
    return '8' == character || '9' == character || 'a' == character || 'b' == character
        || 'A' == character || 'B' == character;
}

/// Read exactly kUuidBytes bytes from the kernel entropy pool.
///
/// The Result names the failing operation so generateUuidV4 can pass the reason
/// on; there is deliberately no fallback path (see this file's header).
[[nodiscard]] Result<std::array<unsigned char, kUuidBytes>> readEntropy()
{
    // 1. Open the pool. O_CLOEXEC because a caller may spawn a child (the MCP
    //    bridge starts no processes today, but an inherited descriptor is the
    //    kind of leak that only ever gets noticed by accident).
    const int descriptor = ::open(kEntropyDevice, O_RDONLY | O_CLOEXEC);
    if (-1 == descriptor)
    {
        return Error::storageFailure(std::string("cannot open the entropy device ")
                                     + kEntropyDevice + ": " + std::strerror(errno));
    }

    // 2. Fill the buffer completely. One read() call would not be enough even
    //    on a healthy system: the call may return fewer bytes than requested
    //    and may be interrupted by a signal. Every byte left unfilled would be
    //    an identical byte in every uuid the process generates, so the loop
    //    insists on all sixteen rather than trusting a single call.
    std::array<unsigned char, kUuidBytes> bytes{};
    std::size_t filled = 0;
    while (kUuidBytes > filled)
    {
        const ssize_t received = ::read(descriptor, bytes.data() + filled, kUuidBytes - filled);
        if (-1 == received)
        {
            if (EINTR == errno)
            {
                // Interrupted before any byte moved, so the buffer is still
                // consistent and the call can simply be repeated.
                continue;
            }
            // errno is only meaningful until the next call, so it is captured
            // before the close(): a message that has to be produced is worth
            // more than a tidy-looking statement order.
            const int failure = errno;
            ::close(descriptor);
            return Error::storageFailure(std::string("cannot read the entropy device ")
                                         + kEntropyDevice + ": " + std::strerror(failure));
        }
        if (0 == received)
        {
            // End of file from a device that has no end means the pool is not
            // usable. Looping again would spin forever without progress, so
            // this is reported as the failure it is.
            ::close(descriptor);
            return Error::storageFailure(std::string("unexpected end of file on the entropy device ")
                                         + kEntropyDevice);
        }
        filled += static_cast<std::size_t>(received);
    }

    // 3. The descriptor has done its job. A failed close() cannot change the
    //    bytes that are already in the buffer, so there is nothing to report.
    ::close(descriptor);

    return bytes;
}

} // namespace

Result<std::string> generateUuidV4()
{
    // 1. Collect the 128 random bits. Failure here is terminal by design: an
    //    entropy source that cannot be read is reported to the caller, never
    //    worked around, because a repeated uuid is a data-loss bug that only
    //    appears at sync time.
    const Result<std::array<unsigned char, kUuidBytes>> entropy = readEntropy();
    if (!entropy.ok())
    {
        return entropy.error();
    }

    // 2. Stamp the version and the variant. Both fields are written over bits
    //    of the random data rather than beside them, so the uuid stays 128 bits
    //    wide — a v4 uuid has 122 random bits and 6 fixed ones. The copy is
    //    taken before the first write so the buffer this function hands on is
    //    the only one that has been modified.
    std::array<unsigned char, kUuidBytes> stamped = entropy.value();
    stamped[kVersionByteIndex] =
        static_cast<unsigned char>((stamped[kVersionByteIndex] & 0x0FU) | 0x40U);
    stamped[kVariantByteIndex] =
        static_cast<unsigned char>((stamped[kVariantByteIndex] & 0x3FU) | 0x80U);

    // 3. Render as 8-4-4-4-12 lowercase hex. The hyphens are inserted by BYTE
    //    boundary (before bytes 4, 6, 8 and 10) rather than by counting output
    //    characters, so a change to the group widths cannot silently produce a
    //    shape that looksLikeUuidV4 would then reject.
    std::string text;
    text.reserve(kUuidTextLength);
    for (std::size_t index = 0; index < kUuidBytes; ++index)
    {
        if (4 == index || 6 == index || 8 == index || 10 == index)
        {
            text.push_back('-');
        }
        text.push_back(hexDigit(static_cast<unsigned int>(stamped[index] >> 4)));
        text.push_back(hexDigit(static_cast<unsigned int>(stamped[index] & 0x0FU)));
    }

    return text;
}

bool looksLikeUuidV4(const std::string &text)
{
    // 1. Length first: it is the one rejection that needs no per-character
    //    work, and every shape rule below assumes the indices it names exist.
    if (kUuidTextLength != text.size())
    {
        return false;
    }

    // 2. A hyphen at each group boundary and a hex digit everywhere else.
    //    Checked in a single pass so the two rules cannot disagree about which
    //    positions they own.
    for (std::size_t index = 0; index < kUuidTextLength; ++index)
    {
        const char character = text[index];
        if (isHyphenPosition(index))
        {
            if ('-' != character)
            {
                return false;
            }
            continue;
        }
        if (!isHexDigit(character))
        {
            return false;
        }
    }

    // 3. The version and variant nibbles are the only part of the shape that
    //    separates a v4 uuid from any other hyphenated hex string of this
    //    length, so they are checked on their own instead of being folded into
    //    the scan above. A v1 or v5 uuid is well-formed and still wrong here:
    //    the export promises v4, and a merge that accepted others would be
    //    reading an identifier whose uniqueness it has not established.
    if ('4' != text[kVersionCharacterIndex])
    {
        return false;
    }
    return isVariantNibble(text[kVariantCharacterIndex]);
}

} // namespace taskpilot
