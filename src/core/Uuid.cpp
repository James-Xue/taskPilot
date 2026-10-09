// Uuid.cpp — version-4 uuid generation, text-derived uuids, and the shape check
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
// deriveUuidFromText() is the mirror image of that rule rather than a
// contradiction of it. Backfilling a uid onto a row that predates the column is
// the one situation where randomness is the failure mode: two machines holding
// the same restored backup must derive the SAME uid for the same row, or the
// first sync sees two unknown uids and inserts every task twice. So that path
// is fixed arithmetic over the row's content — an identity, not a secret, and
// safe only where identity is the requirement.
//
// The check accepts exactly ONE spelling of the text form, lowercase, for the
// same reason the derivation is deterministic: every merge comparison keys on
// the uid string, so a second spelling is a second identity. It is otherwise a
// pure function of the text it is handed: no database, no clock, no I/O.
// TaskSync's parser runs it against every line of a file that may have been
// hand-edited, truncated by a bad checkout, or written by an older version, and
// answering "is this even the right SHAPE of identifier" must not depend on
// anything the caller happens to have loaded.

#include "core/Uuid.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
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

/// Half of a uuid, and therefore the number of bytes each of the two 64-bit
/// digest passes in deriveUuidFromText() contributes. Named so the layout
/// arithmetic below reads as "first half, second half" rather than as the
/// literal 8.
constexpr std::size_t kHalfBytes{ kUuidBytes / 2 };

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

/// True for a LOWERCASE hex digit: 0-9 or a-f, and nothing else.
///
/// The restriction is a correctness rule, not a preference, and its cost runs
/// in one direction only:
///   1. Every merge comparison keys on the uid STRING — the tasks.uid column,
///      its unique index, and the lookup in mergeJsonl.
///   2. So "9F3C…" and "9f3c…" are not two spellings of one identity, they are
///      two identities. Accepting both lets one task carry both, and it then
///      duplicates on every machine while the shared export file holds both
///      spellings forever.
///   3. Nothing inside taskPilot writes uppercase (see kHexDigits) and nothing
///      anywhere normalises case, so there is no legitimate producer the lenient
///      form could be serving — only a second way to name a row.
///
/// What that costs, stated plainly: a hand-edited line that capitalised a digit
/// is refused outright instead of merged. That is the intended trade — a loud
/// refusal is recoverable, a silent duplicate is not.
[[nodiscard]] bool isHexDigit(char character)
{
    if ('0' <= character && '9' >= character)
    {
        return true;
    }
    return 'a' <= character && 'f' >= character;
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

/// The variant nibble: 8, 9, a or b — lowercase, like every other hex digit the
/// text form may carry.
///
/// A and B were accepted until this rule was tightened, and they are precisely
/// the case the header's reasoning is about: they name the same nibbles as a
/// and b, so a checker that took them would give one uuid two spellings and
/// therefore two identities. They are refused for exactly the reason the body's
/// hex digits are.
///
/// The other twelve values are a different question entirely — they select uuid
/// schemes this project does not speak — so a line carrying one is rejected
/// rather than merged under a shape we would misread.
[[nodiscard]] bool isVariantNibble(char character)
{
    return '8' == character || '9' == character || 'a' == character || 'b' == character;
}

/// Return `bytes` with the version and the variant nibbles stamped onto them.
///
/// Both fields are written OVER bits of the input rather than beside it, so the
/// result stays 128 bits wide: a v4 uuid has 122 unconstrained bits and 6 fixed
/// ones. Taking the array by value is deliberate — the caller's buffer is left
/// untouched, so no caller can be surprised by a stamp it did not ask for, and
/// the compiler may forward the copy instead of materialising it.
///
/// Shared by generateUuidV4 and deriveUuidFromText so a uuid-shaped value is
/// stamped the same way whichever source produced it, and therefore answers to
/// the same reader in looksLikeUuidV4.
[[nodiscard]] std::array<unsigned char, kUuidBytes> stampedVersionAndVariant(
    std::array<unsigned char, kUuidBytes> bytes)
{
    bytes[kVersionByteIndex] =
        static_cast<unsigned char>((bytes[kVersionByteIndex] & 0x0FU) | 0x40U);
    bytes[kVariantByteIndex] =
        static_cast<unsigned char>((bytes[kVariantByteIndex] & 0x3FU) | 0x80U);
    return bytes;
}

/// Render sixteen bytes as the 8-4-4-4-12 lowercase hex text form.
///
/// The hyphens are inserted by BYTE boundary (before bytes 4, 6, 8 and 10)
/// rather than by counting output characters, so a change to the group widths
/// cannot silently produce a shape that looksLikeUuidV4 would then reject. The
/// layout lives here and nowhere else for the same reason: the generator and
/// the derivation cannot drift into two spellings of one identifier.
[[nodiscard]] std::string renderUuidText(const std::array<unsigned char, kUuidBytes> &bytes)
{
    std::string text;
    text.reserve(kUuidTextLength);
    for (std::size_t index = 0; index < kUuidBytes; ++index)
    {
        if (4 == index || 6 == index || 8 == index || 10 == index)
        {
            text.push_back('-');
        }
        text.push_back(hexDigit(static_cast<unsigned int>(bytes[index] >> 4)));
        text.push_back(hexDigit(static_cast<unsigned int>(bytes[index] & 0x0FU)));
    }
    return text;
}

/// The two constant pairs the derived uuid's 128 bits are computed with.
///
/// Two passes rather than one because a single 64-bit digest would make the
/// derived identity 64 bits wide, while the header's collision argument ("two
/// rows collide only if their canonical forms are identical") rests on 128. The
/// two pairs must share NO constant: two passes over the same text with the
/// same multiplier stay related by a factor that depends only on the text's
/// length, so the second 64 bits could be computed from the first and the value
/// would carry 64 bits of content however it is rendered.
///
/// The first pair is FNV-1a's own 64-bit offset basis and prime. The second is
/// two odd constants from unrelated 64-bit mixing designs (xxHash's primary
/// multiplier, and splitmix64's golden gamma). Oddness is the property that
/// matters: multiplying by an odd number is invertible modulo 2^64, which is
/// what keeps the multiply a mixing step rather than a value-destroying one.
constexpr std::uint64_t kFirstOffsetBasis{ 14695981039346656037ULL };
constexpr std::uint64_t kFirstPrime{ 1099511628211ULL };
constexpr std::uint64_t kSecondOffsetBasis{ 11400714785074694791ULL };
constexpr std::uint64_t kSecondPrime{ 11400714819323198485ULL };

/// One FNV-1a pass: start from `offsetBasis`, then for each byte XOR it into the
/// state and multiply by `prime`.
///
/// Basis and prime are parameters rather than constants because the derivation
/// runs this same arithmetic twice with two unrelated pairs; see the constants
/// above for why the pairs must not be shared.
[[nodiscard]] std::uint64_t fnv1a64(const std::string &text, std::uint64_t offsetBasis,
                                    std::uint64_t prime)
{
    std::uint64_t digest = offsetBasis;
    for (const char character : text)
    {
        // The byte enters the state as an UNSIGNED octet. A plain char may be
        // signed, and a byte above 0x7f would then arrive as a negative int and
        // sign-extend into the state — which would make the derived uid depend
        // on the platform's char signedness. Two machines disagreeing about the
        // uid of one row is the exact duplication this function exists to
        // prevent, so the widening is written out rather than left to the
        // compiler.
        digest ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
        digest *= prime;
    }
    return digest;
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

    // 2. Stamp the version and the variant, then render. Both steps are shared
    //    with deriveUuidFromText, so a uuid-shaped value produced by either
    //    function is laid out by the same code and read back by the same rules;
    //    the generator differs only in where its 128 bits come from.
    return renderUuidText(stampedVersionAndVariant(entropy.value()));
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

std::string deriveUuidFromText(const std::string &canonical)
{
    // A derived uuid is an IDENTITY, never a SECRET. It is arithmetic over the
    // row's own content, so anyone who holds that canonical text can reproduce
    // this value, and the derivation is deterministic by contract so that two
    // machines restoring one backup agree on it (Uuid.hpp). Where
    // unpredictability matters — a token, a capability, a nonce — this function
    // is the wrong tool and generateUuidV4() is the right one.
    //
    // 1. Two 64-bit passes over the canonical text, concatenated into the 128
    //    bits a uuid has. The two constant pairs share no constant, so the
    //    second half is not computable from the first (see the constants).
    const std::uint64_t first = fnv1a64(canonical, kFirstOffsetBasis, kFirstPrime);
    const std::uint64_t second = fnv1a64(canonical, kSecondOffsetBasis, kSecondPrime);

    // 2. Lay the passes out big-endian, byte by byte, rather than reinterpreting
    //    the integers' bytes. A memcpy would make the result depend on the
    //    host's byte order, and a big-endian machine would then derive a
    //    different uid for the same row than a little-endian one — the sync
    //    would insert the row on both machines and never converge.
    std::array<unsigned char, kUuidBytes> bytes{};
    for (std::size_t index = 0; index < kHalfBytes; ++index)
    {
        const std::size_t shift = 8 * (kHalfBytes - 1 - index);
        bytes[index] = static_cast<unsigned char>((first >> shift) & 0xFFU);
        bytes[kHalfBytes + index] = static_cast<unsigned char>((second >> shift) & 0xFFU);
    }

    // 3. Stamp the version and the variant so the result is a v4 uuid by SHAPE,
    //    which is what lets every reader in the system — the export parser, the
    //    merge lookup, the store's uid column — stay single-path instead of
    //    growing a second branch for derived identifiers. Six of the 128 derived
    //    bits are spent on those two nibbles; the other 122 carry the digest.
    //    Rendering then writes the one spelling this project spells, so a
    //    derived uuid is indistinguishable from a generated one downstream.
    return renderUuidText(stampedVersionAndVariant(bytes));
}

} // namespace taskpilot
